/* Generic RFC 6455 WebSocket server mechanics. See ws.h for the split
 * between this file (protocol) and the policy layer (talk_ws.c here).
 *
 * ------------------------------------------------------------------------
 * VENDORED from thingino-motors, branch feature/websocket-daemon (src/ws.c).
 *
 * Deliberately kept as close to the original as possible so the two copies
 * stay diffable; there is no shared library between the daemons, so the file
 * itself is the unit of reuse. Exactly two things were changed:
 *
 *   1. The transport seam (io_pending/io_read/io_write below). motors runs a
 *      standalone listener on its own port and therefore owns a raw fd and
 *      needs ws_tls.c to terminate TLS itself. timps does not: this file is
 *      driven from httpd.c's existing per-connection thread, on a connection
 *      ms_tls_accept() has ALREADY terminated (httpd.c conn_thread), so the
 *      seam calls timps' own ms_tls_* primitives from tls.h and ws_tls.c is
 *      not vendored at all. The guard is #ifdef USE_TLS, not MOTORS_WS_TLS.
 *
 *      That is not a rename: ms_tls_read()'s contract INVERTS motors' (0 means
 *      "no data yet" and -1 means "peer closed", the opposite of POSIX), so
 *      io_read() translates. See the table there.
 *
 *   2. WS_MAX_PAYLOAD lowered 2048 -> 1024 in ws.h (audio frames are smaller
 *      than motors' JSON commands), behind an #ifndef so it stays overridable.
 *
 * INTENTIONALLY UNUSED HERE - do not "clean up":
 *
 *   ws_handshake_read()       timps' conn_thread has ALREADY read the request
 *   ws_header()               head into its own buffer before dispatch, and
 *   ws_handshake_is_upgrade() talk_ws.c parses it from there with its own
 *   header_value / header_is  scanner; re-reading the socket here would block
 *   hexval                    forever on a head that is already consumed.
 *   ws_query_param()          talk_ws.c has its own '?'/'&'-anchored parse.
 *
 * They are left in place rather than deleted so this file stays a near-copy
 * of motors'. The build uses -ffunction-sections + -Wl,--gc-sections, so the
 * linker drops all of them (measured: ~2.5 KB of MIPS -Os text) and they cost
 * nothing in the shipped binary.
 * ------------------------------------------------------------------------ */

/* strcasestr() is a GNU extension; the target toolchain is uClibc-ng, which
 * provides it under the same guard glibc does. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <errno.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "sha1.h"
#include "ws.h"

/* timps: tls.h is a pure declaration header (no mbedTLS includes), so this
 * file still pulls in nothing from mbedTLS - the ws_io.tls handle stays an
 * opaque void * exactly as it is in motors. */
#ifdef USE_TLS
#include "tls.h"
#endif

/* RFC 6455 section 1.3. Fixed by the spec, not a secret, not a salt. */
#define WS_GUID "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"

/* ------------------------------------------------------------------ *
 * clock
 * ------------------------------------------------------------------ */

long long ws_now_ms(void) {
#ifdef CLOCK_MONOTONIC
  struct timespec ts;
  /* uClibc-ng implements clock_gettime() in libc (no -lrt), and CLOCK_MONOTONIC
   * has been in the 3.10 Ingenic kernels since forever. The gettimeofday()
   * fallback below is only there so this file stays portable to a host that
   * lacks it - it is not expected to be taken on the target. */
  if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0)
    return (long long)ts.tv_sec * 1000LL + ts.tv_nsec / 1000000L;
#endif
  {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (long long)tv.tv_sec * 1000LL + tv.tv_usec / 1000;
  }
}

/* ------------------------------------------------------------------ *
 * base64
 * ------------------------------------------------------------------ */

static const char b64tab[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

size_t ws_base64_encode(const unsigned char *in, size_t len, char *out) {
  size_t i = 0, o = 0;

  while (i + 3 <= len) {
    uint32_t v = ((uint32_t)in[i] << 16) | ((uint32_t)in[i + 1] << 8) | in[i + 2];
    out[o++] = b64tab[(v >> 18) & 0x3F];
    out[o++] = b64tab[(v >> 12) & 0x3F];
    out[o++] = b64tab[(v >> 6) & 0x3F];
    out[o++] = b64tab[v & 0x3F];
    i += 3;
  }

  if (i < len) {
    /* 1 or 2 trailing bytes: pad the missing ones with zero bits and emit
     * '=' for each absent input byte */
    uint32_t v = (uint32_t)in[i] << 16;
    bool two = (len - i) == 2;
    if (two)
      v |= (uint32_t)in[i + 1] << 8;
    out[o++] = b64tab[(v >> 18) & 0x3F];
    out[o++] = b64tab[(v >> 12) & 0x3F];
    out[o++] = two ? b64tab[(v >> 6) & 0x3F] : '=';
    out[o++] = '=';
  }

  out[o] = '\0';
  return o;
}

/* ------------------------------------------------------------------ *
 * timed socket I/O
 *
 * Everything here is poll()-then-read() against a monotonic-ish deadline
 * rather than SO_RCVTIMEO, because a single frame can straddle several
 * read() calls and only a deadline (not a per-read timeout) bounds the
 * TOTAL time a peer can keep a worker thread parked mid-frame. That is the
 * slow-loris defence for this listener.
 * ------------------------------------------------------------------ */

/* --- the transport seam ---
 *
 * Three functions, and nothing else in this file touches the socket. A plain
 * connection is exactly what it always was; a TLS one substitutes timps'
 * ms_tls_* primitives. The contract the rest of this file is written against
 * is the POSIX one:
 *
 *   >0  bytes moved
 *    0  on read, the peer closed
 *   -1  errno set; EAGAIN/EWOULDBLOCK means "not yet, poll and retry".
 *
 * timps DIVERGES from motors here (see the provenance block at the top).
 * motors' ws_tls.c was written to honour the POSIX contract directly, so its
 * branches were two lines. timps reuses tls.c, which httpd.c and rtsp.c
 * already share, and tls.c's ms_tls_read() has the OPPOSITE convention:
 *
 *   ms_tls_read() returns        meaning                       -> io_read()
 *   ---------------------------------------------------------------------
 *   r  >  0                      bytes decrypted                  r
 *   r == 0                       WANT_READ/WANT_WRITE, no data    -1, EAGAIN
 *   r == -1                      PEER_CLOSE_NOTIFY (tls.c's own   0  (EOF)
 *                                explicit -1; every other mbedTLS
 *                                error is a large negative)
 *   r  < -1                      transport/protocol error         -1, ECONNRESET
 *
 * Translating rather than "fixing" tls.c is deliberate: ms_tls_read()'s
 * convention is depended on by httpd.c's and rtsp.c's streaming loops, and
 * this feature is not the place to change it under them.
 *
 * Unlike motors, timps' sockets stay BLOCKING. ms_tls_accept() sets
 * SO_RCVTIMEO on a blocking fd and documents that assumption, and
 * ms_tls_write() retries WANT_READ/WANT_WRITE with a bare continue - safe
 * only because the underlying send() blocks. Making the fd non-blocking here
 * would turn that continue into an unbounded busy-spin on a single-core SoC.
 * Consequence: the EAGAIN paths below are near-dead on the TLS path (poll()
 * plus SO_RCVTIMEO/SO_SNDTIMEO, set by httpd.c's accept_thread at 30/15 s,
 * do the bounding), but they are kept because they are still correct. */

static int io_pending(const ws_io *io) {
#ifdef USE_TLS
  if (io->tls)
    return ms_tls_pending((ms_tls_conn *)io->tls);
#else
  (void)io;
#endif
  return 0;
}

static ssize_t io_read(ws_io *io, void *buf, size_t n) {
#ifdef USE_TLS
  if (io->tls) {
    int r = ms_tls_read((ms_tls_conn *)io->tls, buf, (int)n);
    if (r > 0)
      return r;
    if (r == 0) { /* no data yet -> the POSIX "retry" spelling */
      errno = EAGAIN;
      return -1;
    }
    if (r == -1) /* orderly close_notify -> EOF, becomes WS_CLOSED */
      return 0;
    errno = ECONNRESET; /* any other mbedTLS error -> WS_EIO */
    return -1;
  }
#endif
  return read(io->fd, buf, n);
}

static ssize_t io_write(ws_io *io, const void *buf, size_t n) {
#ifdef USE_TLS
  if (io->tls) {
    /* ms_tls_write() writes all len bytes and returns len, or -1 - already
     * bytes-moved compatible. It does NOT set errno on failure, though, and
     * write_all() below branches on errno == EAGAIN to decide whether to
     * poll and retry: a stale EAGAIN left by an earlier syscall would make
     * it retry forever against a dead session. Set errno explicitly. */
    int r = ms_tls_write((ms_tls_conn *)io->tls, buf, (int)n);
    if (r < 0) {
      errno = EIO;
      return -1;
    }
    return r;
  }
#endif
  /* MSG_NOSIGNAL: a client that closes its tab mid-push must not take the
   * whole daemon down with SIGPIPE. (timps' main.c also ignores SIGPIPE
   * globally, exactly as net.c's net_sendall() notes, but this helper does
   * not depend on that staying true.) */
  return send(io->fd, buf, n, MSG_NOSIGNAL);
}

static int poll_fd(int fd, short events, int timeout_ms) {
  struct pollfd p = {.fd = fd, .events = events, .revents = 0};
  int r;

  do {
    r = poll(&p, 1, timeout_ms);
  } while (r < 0 && errno == EINTR);

  if (r == 0)
    return WS_AGAIN;
  if (r < 0)
    return WS_EIO;
  if (p.revents & (POLLERR | POLLNVAL))
    return WS_EIO;
  /* POLLHUP with POLLIN still has buffered data worth reading; a bare
   * POLLHUP is EOF, which read() below reports as WS_CLOSED anyway */
  return WS_OK;
}

static int poll_readable(ws_io *io, int timeout_ms) {
  /* Bytes mbedTLS has already decrypted into its own buffer are invisible to
   * poll() on the raw fd - a whole message can sit there while poll() blocks
   * the full timeout and then reports WS_AGAIN. Checking first is what stops a
   * TLS connection from stalling for push_ms on data it already holds. */
  if (io_pending(io) > 0)
    return WS_OK;
  return poll_fd(io->fd, POLLIN, timeout_ms);
}

/* How long a blocked write may wait before the connection is given up on.
 * Only ever reached on the TLS path (the plain socket is blocking, so send()
 * does this waiting inside the kernel); it exists so a peer that stops reading
 * cannot pin a connection thread forever once mbedTLS starts returning
 * WANT_WRITE. Generous, because a genuinely slow WiFi client must not be
 * dropped mid-frame - and a torn write is not resynchronisable, so the only
 * thing to do when it does expire is close. */
#define WS_WRITE_TIMEOUT_MS 10000

/* Read exactly n bytes or fail. deadline_ms is an ABSOLUTE ws_now_ms() instant,
 * so a caller assembling a multi-part frame shares one wall-clock deadline
 * across all its reads. It has to be absolute: a per-call budget that is only
 * zeroed on a timeout never charges a SUCCESSFUL read for the time it took, so
 * a peer trickling one byte per poll slice renews the full budget on every
 * iteration and holds the connection open forever (same slow-loris shape the
 * mp4/httpd.c head-read loop was fixed for). */
static int read_exact(ws_io *io, void *buf, size_t n, long long deadline_ms) {
  unsigned char *p = (unsigned char *)buf;
  size_t got = 0;

  while (got < n) {
    long long left = deadline_ms - ws_now_ms();
    int pr = poll_readable(io, left > 0 ? (int)left : 0);
    if (pr == WS_AGAIN) {
      /* Timing out with a partial frame in hand is not recoverable: the
       * stream is byte-oriented and we have already consumed a prefix, so
       * there is no way to resynchronise. Only a timeout on a frame that
       * has not started yet is benign, and that case is handled by the
       * caller peeking the first byte under the same deadline. */
      return (got == 0) ? WS_AGAIN : WS_EIO;
    }
    if (pr != WS_OK)
      return pr;

    ssize_t r = io_read(io, p + got, n - got);
    if (r == 0)
      return WS_CLOSED;
    if (r < 0) {
      if (errno == EINTR)
        continue;
      /* TLS only: poll() saw a readable socket but the record it carried was
       * incomplete, so nothing decrypted out of it yet. Go back and wait for
       * the rest under the same deadline. */
      if (errno == EAGAIN || errno == EWOULDBLOCK)
        continue;
      return WS_EIO;
    }
    got += (size_t)r;
  }
  return WS_OK;
}

static int write_all(ws_io *io, const void *buf, size_t n) {
  const unsigned char *p = (const unsigned char *)buf;
  size_t sent = 0;

  while (sent < n) {
    ssize_t w = io_write(io, p + sent, n - sent);
    if (w < 0) {
      if (errno == EINTR)
        continue;
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        /* POLLIN as well as POLLOUT: mbedTLS can need to READ to make
         * progress on a write (a post-handshake message it has to consume
         * first), and ws_tls_write() reports both as EAGAIN. Waiting only for
         * writability would then spin until the timeout on a socket that is
         * perfectly writable. */
        if (poll_fd(io->fd, POLLOUT | POLLIN, WS_WRITE_TIMEOUT_MS) != WS_OK)
          return WS_EIO;
        continue;
      }
      return WS_EIO;
    }
    if (w == 0)
      return WS_EIO;
    sent += (size_t)w;
  }
  return WS_OK;
}

/* ------------------------------------------------------------------ *
 * handshake
 * ------------------------------------------------------------------ */

void ws_conn_init(ws_conn *c, ws_io *io) {
  memset(c, 0, sizeof(*c));
  c->io = io;
  /* Seed from "now", not from zero: a connection that has just been accepted
   * has not been silent since the epoch, and a zero here would make the very
   * first liveness check fire immediately. */
  c->last_rx_ms = ws_now_ms();
}

long long ws_conn_idle_ms(const ws_conn *c) {
  long long d = ws_now_ms() - c->last_rx_ms;
  return (d < 0) ? 0 : d;
}

/* Copy a header value into out, stopping at CR/LF. Values are cut at the line
 * end, so nothing read here can carry an embedded newline into a response. */
static void header_value(const char *p, char *out, size_t cap) {
  size_t i = 0;
  while (*p == ' ' || *p == '\t')
    p++;
  while (*p && *p != '\r' && *p != '\n' && i + 1 < cap)
    out[i++] = *p++;
  /* trim trailing spaces so ' Origin: http://x ' matches an allow-list */
  while (i > 0 && (out[i - 1] == ' ' || out[i - 1] == '\t'))
    i--;
  out[i] = '\0';
}

static bool header_is(const char *line, const char *name) {
  size_t n = strlen(name);
  return strncasecmp(line, name, n) == 0 && line[n] == ':';
}

bool ws_header(const ws_handshake *hs, const char *name, char *out,
               size_t cap) {
  const char *p = hs->head;
  size_t nlen = strlen(name);

  if (cap == 0)
    return false;
  out[0] = '\0';

  /* skip the request line - a header search must never match it */
  p = strchr(p, '\n');
  while (p) {
    p++;
    if (*p == '\r' || *p == '\n' || *p == '\0')
      break;
    if (strncasecmp(p, name, nlen) == 0 && p[nlen] == ':') {
      header_value(p + nlen + 1, out, cap);
      return true;
    }
    p = strchr(p, '\n');
  }
  return false;
}

int ws_handshake_read(ws_io *io, ws_handshake *hs, int timeout_ms) {
  char *buf;
  size_t len = 0;
  long long deadline = ws_now_ms() + timeout_ms;
  const char *p;

  memset(hs, 0, sizeof(*hs));
  hs->version = -1;
  buf = hs->head;

  /* Read until the CRLFCRLF (or LFLF - be liberal, some tools omit the CRs)
   * that ends the request head. Bounded by both the byte cap and the
   * deadline; a client that opens a socket and says nothing is dropped by
   * the deadline, which is the cheap half of the DoS story. */
  for (;;) {
    long long left = deadline - ws_now_ms();
    int pr = poll_readable(io, left > 0 ? (int)left : 0);
    if (pr == WS_AGAIN)
      return WS_EIO; /* handshake never completed */
    if (pr != WS_OK)
      return pr;

    ssize_t r = io_read(io, buf + len, WS_MAX_HANDSHAKE - len);
    if (r == 0)
      return WS_CLOSED;
    if (r < 0) {
      if (errno == EINTR)
        continue;
      if (errno == EAGAIN || errno == EWOULDBLOCK)
        continue; /* partial TLS record; see read_exact() */
      return WS_EIO;
    }
    len += (size_t)r;
    buf[len] = '\0';

    if (strstr(buf, "\r\n\r\n") || strstr(buf, "\n\n"))
      break;

    if (len >= WS_MAX_HANDSHAKE)
      return WS_ETOOBIG;
  }

  hs->head_len = len;

  /* --- request line: METHOD SP TARGET SP HTTP/x.y --- */
  {
    const char *sp1 = strchr(buf, ' ');
    if (!sp1)
      return WS_EPROTO;
    size_t mlen = (size_t)(sp1 - buf);
    if (mlen >= sizeof(hs->method))
      return WS_EPROTO;
    memcpy(hs->method, buf, mlen);
    hs->method[mlen] = '\0';

    const char *target = sp1 + 1;
    const char *sp2 = strchr(target, ' ');
    const char *eol = strpbrk(target, "\r\n");
    if (!sp2 || (eol && sp2 > eol))
      return WS_EPROTO;

    const char *qm = memchr(target, '?', (size_t)(sp2 - target));
    size_t plen = qm ? (size_t)(qm - target) : (size_t)(sp2 - target);
    if (plen >= sizeof(hs->path))
      plen = sizeof(hs->path) - 1;
    memcpy(hs->path, target, plen);
    hs->path[plen] = '\0';

    if (qm) {
      size_t qlen = (size_t)(sp2 - qm - 1);
      if (qlen >= sizeof(hs->query))
        qlen = sizeof(hs->query) - 1;
      memcpy(hs->query, qm + 1, qlen);
      hs->query[qlen] = '\0';
    }
  }

  /* --- headers --- */
  p = strchr(buf, '\n');
  while (p) {
    p++;
    if (*p == '\r' || *p == '\n' || *p == '\0')
      break; /* end of head */

    if (header_is(p, "Host")) {
      header_value(p + 5, hs->host, sizeof(hs->host));
    } else if (header_is(p, "Origin")) {
      hs->has_origin = true;
      header_value(p + 7, hs->origin, sizeof(hs->origin));
    } else if (header_is(p, "Sec-WebSocket-Key")) {
      header_value(p + 18, hs->key, sizeof(hs->key));
    } else if (header_is(p, "Sec-WebSocket-Version")) {
      char v[16];
      header_value(p + 22, v, sizeof(v));
      hs->version = (int)strtol(v, NULL, 10);
    } else if (header_is(p, "Upgrade")) {
      char v[64];
      header_value(p + 8, v, sizeof(v));
      hs->has_upgrade_websocket = (strcasecmp(v, "websocket") == 0);
    } else if (header_is(p, "Connection")) {
      char v[128];
      header_value(p + 11, v, sizeof(v));
      /* "Connection: keep-alive, Upgrade" is legal and common, so this is a
       * token search rather than an equality test (RFC 6455 section 4.1) */
      hs->has_connection_upgrade = (strcasestr(v, "upgrade") != NULL);
    }

    p = strchr(p, '\n');
  }

  return WS_OK;
}

bool ws_handshake_is_upgrade(const ws_handshake *hs) {
  return strcmp(hs->method, "GET") == 0 && hs->has_upgrade_websocket &&
         hs->has_connection_upgrade && hs->key[0] != '\0' && hs->version == 13;
}

void ws_accept_key(const char *key, char out[29]) {
  unsigned char digest[20];
  sha1_ctx ctx;

  sha1_init(&ctx);
  sha1_update(&ctx, (const unsigned char *)key, strlen(key));
  sha1_update(&ctx, (const unsigned char *)WS_GUID, sizeof(WS_GUID) - 1);
  sha1_final(&ctx, digest);

  ws_base64_encode(digest, sizeof(digest), out);
}

int ws_handshake_accept(ws_io *io, const ws_handshake *hs,
                        const char *subproto) {
  char accept[29];
  char resp[320];
  int n;

  ws_accept_key(hs->key, accept);

  /* No CORS headers: WebSocket is not subject to CORS at all (that is
   * exactly why motor-ws.c has to police Origin itself). Nothing from the
   * request is echoed except the computed accept value, which is a base64
   * digest and therefore cannot carry CR/LF. */
  n = snprintf(resp, sizeof(resp),
               "HTTP/1.1 101 Switching Protocols\r\n"
               "Upgrade: websocket\r\n"
               "Connection: Upgrade\r\n"
               "Sec-WebSocket-Accept: %s\r\n",
               accept);
  if (subproto && *subproto)
    n += snprintf(resp + n, sizeof(resp) - (size_t)n,
                  "Sec-WebSocket-Protocol: %s\r\n", subproto);
  n += snprintf(resp + n, sizeof(resp) - (size_t)n, "\r\n");

  if (n < 0 || (size_t)n >= sizeof(resp))
    return WS_EIO;

  return write_all(io, resp, (size_t)n);
}

int ws_handshake_reject(ws_io *io, int status, const char *status_text,
                        const char *body) {
  char resp[512];
  int n = snprintf(resp, sizeof(resp),
                   "HTTP/1.1 %d %s\r\n"
                   "Content-Type: text/plain\r\n"
                   "Content-Length: %u\r\n"
                   "Connection: close\r\n"
                   "\r\n"
                   "%s",
                   status, status_text, (unsigned)strlen(body), body);
  if (n < 0 || (size_t)n >= sizeof(resp))
    return WS_EIO;
  return write_all(io, resp, (size_t)n);
}

/* ------------------------------------------------------------------ *
 * frames
 * ------------------------------------------------------------------ */

int ws_send_frame(ws_io *io, int opcode, const void *payload, size_t len) {
  unsigned char hdr[10];
  size_t hlen = 0;

  hdr[0] = (unsigned char)(0x80 | (opcode & 0x0F)); /* FIN, no RSV */

  /* Server frames are never masked (RFC 6455 section 5.1), so the mask bit
   * stays clear and no mask key follows. */
  if (len < 126) {
    hdr[1] = (unsigned char)len;
    hlen = 2;
  } else if (len <= 0xFFFF) {
    hdr[1] = 126;
    hdr[2] = (unsigned char)(len >> 8);
    hdr[3] = (unsigned char)(len & 0xFF);
    hlen = 4;
  } else {
    hdr[1] = 127;
    for (int i = 0; i < 8; i++)
      hdr[2 + i] = (unsigned char)((uint64_t)len >> (56 - 8 * i));
    hlen = 10;
  }

  if (write_all(io, hdr, hlen) != WS_OK)
    return WS_EIO;
  if (len > 0 && write_all(io, payload, len) != WS_OK)
    return WS_EIO;
  return WS_OK;
}

int ws_send_text(ws_io *io, const char *text) {
  return ws_send_frame(io, WS_OP_TEXT, text, strlen(text));
}

int ws_send_ping(ws_io *io) { return ws_send_frame(io, WS_OP_PING, NULL, 0); }

int ws_send_close(ws_io *io, int code, const char *reason) {
  unsigned char buf[125];
  size_t rl = reason ? strlen(reason) : 0;

  /* control frame payloads are capped at 125 bytes (RFC 6455 section 5.5) */
  if (rl > sizeof(buf) - 2)
    rl = sizeof(buf) - 2;
  buf[0] = (unsigned char)(code >> 8);
  buf[1] = (unsigned char)(code & 0xFF);
  if (rl)
    memcpy(buf + 2, reason, rl);
  return ws_send_frame(io, WS_OP_CLOSE, buf, rl + 2);
}

int ws_read_message(ws_conn *c, int *opcode, unsigned char *out, size_t cap,
                    size_t *out_len, int timeout_ms) {
  /* one absolute deadline for the whole message, fragments included: every
   * read below charges against it whether it succeeded or timed out. */
  long long deadline = ws_now_ms() + timeout_ms;

  for (;;) {
    unsigned char h[2];
    int rc = read_exact(c->io, h, 2, deadline);
    if (rc != WS_OK)
      return rc; /* WS_AGAIN here is benign: no frame had started */

    bool fin = (h[0] & 0x80) != 0;
    int rsv = h[0] & 0x70;
    int op = h[0] & 0x0F;
    bool masked = (h[1] & 0x80) != 0;
    uint64_t plen = h[1] & 0x7F;

    /* No extension was negotiated, so any RSV bit set is a protocol error
     * (RFC 6455 section 5.2). Being strict here is free and stops a peer
     * from smuggling a compressed frame past the length accounting. */
    if (rsv)
      return WS_EPROTO;

    /* "The server MUST close the connection upon receiving a frame that is
     * not masked" (RFC 6455 section 5.1). Not optional: an unmasked client
     * frame is the signature of a cache-poisoning proxy attack, which is
     * the entire reason masking exists. */
    if (!masked)
      return WS_EPROTO;

    if (plen == 126) {
      unsigned char e[2];
      /* Once a header has started, a timeout means a torn frame - read_exact
       * turns that into WS_EIO for us. */
      if ((rc = read_exact(c->io, e, 2, deadline)) != WS_OK)
        return (rc == WS_AGAIN) ? WS_EIO : rc;
      plen = ((uint64_t)e[0] << 8) | e[1];
    } else if (plen == 127) {
      unsigned char e[8];
      if ((rc = read_exact(c->io, e, 8, deadline)) != WS_OK)
        return (rc == WS_AGAIN) ? WS_EIO : rc;
      plen = 0;
      for (int i = 0; i < 8; i++)
        plen = (plen << 8) | e[i];
      /* the high bit MUST be 0 (RFC 6455 section 5.2); a negative length is
       * how a signed-arithmetic parser gets turned into a heap overflow */
      if (plen & 0x8000000000000000ULL)
        return WS_EPROTO;
    }

    /* Control frames: never fragmented, payload <= 125 (section 5.5). */
    bool is_control = (op & 0x08) != 0;
    if (is_control && (!fin || plen > 125))
      return WS_EPROTO;

    if (plen > WS_MAX_PAYLOAD)
      return WS_ETOOBIG;

    unsigned char mask[4];
    if ((rc = read_exact(c->io, mask, 4, deadline)) != WS_OK)
      return (rc == WS_AGAIN) ? WS_EIO : rc;

    unsigned char payload[WS_MAX_PAYLOAD];
    if (plen > 0) {
      if ((rc = read_exact(c->io, payload, (size_t)plen, deadline)) != WS_OK)
        return (rc == WS_AGAIN) ? WS_EIO : rc;
      for (uint64_t i = 0; i < plen; i++)
        payload[i] ^= mask[i & 3];
    }

    /* Liveness stamp. Deliberately here and not at the top of the loop: only a
     * frame that arrived COMPLETE and well-formed counts as proof of life. A
     * peer that dribbles two header bytes and then stalls forever must not be
     * able to hold the connection open by feeding the liveness timer, which is
     * exactly the slow-loris shape read_exact()'s deadline already defends
     * against on a single frame. */
    c->last_rx_ms = ws_now_ms();

    if (is_control) {
      /* Handled here so the caller's command loop never sees the control
       * plane. A PING may arrive in the middle of a fragmented message, and
       * answering it must not disturb c->frag - it does not, because the
       * reassembly state is only touched on the data path below. */
      if (op == WS_OP_CLOSE)
        return WS_CLOSED;
      if (op == WS_OP_PING) {
        if (ws_send_frame(c->io, WS_OP_PONG, payload, (size_t)plen) != WS_OK)
          return WS_EIO;
      }
      /* WS_OP_PONG: nothing further to do - the stamp above is the whole
       * point of receiving one. The keepalive in motor-ws.c treats "any
       * traffic" as liveness, so an unsolicited pong is harmless (it can at
       * worst keep a connection that is talking to us alive, which is the
       * answer we wanted anyway). */
      continue;
    }

    /* --- data frame --- */
    if (op == WS_OP_CONT) {
      if (c->frag_op == 0)
        return WS_EPROTO; /* continuation with nothing to continue */
    } else {
      if (c->frag_op != 0)
        return WS_EPROTO; /* new message started mid-fragment */
      c->frag_op = op;
      c->frag_len = 0;
    }

    if (c->frag_len + (size_t)plen > sizeof(c->frag))
      return WS_ETOOBIG;
    if (plen > 0) {
      memcpy(c->frag + c->frag_len, payload, (size_t)plen);
      c->frag_len += (size_t)plen;
    }

    if (!fin)
      continue; /* wait for the rest, under the same deadline */

    if (c->frag_len >= cap)
      return WS_ETOOBIG;
    memcpy(out, c->frag, c->frag_len);
    out[c->frag_len] = '\0'; /* callers parse TEXT frames as C strings */
    *out_len = c->frag_len;
    *opcode = c->frag_op;

    c->frag_op = 0;
    c->frag_len = 0;
    return WS_OK;
  }
}

/* ------------------------------------------------------------------ *
 * query string
 * ------------------------------------------------------------------ */

static int hexval(int ch) {
  if (ch >= '0' && ch <= '9')
    return ch - '0';
  if (ch >= 'a' && ch <= 'f')
    return ch - 'a' + 10;
  if (ch >= 'A' && ch <= 'F')
    return ch - 'A' + 10;
  return -1;
}

bool ws_query_param(const char *query, const char *name, char *out,
                    size_t cap) {
  size_t nlen = strlen(name);
  const char *p = query;

  if (!query || !*query || cap == 0)
    return false;

  while (*p) {
    const char *amp = strchr(p, '&');
    size_t seglen = amp ? (size_t)(amp - p) : strlen(p);

    if (seglen > nlen && strncmp(p, name, nlen) == 0 && p[nlen] == '=') {
      const char *v = p + nlen + 1;
      size_t vlen = seglen - nlen - 1;
      size_t i = 0, o = 0;

      while (i < vlen && o + 1 < cap) {
        if (v[i] == '%' && i + 2 < vlen) {
          int hi = hexval((unsigned char)v[i + 1]);
          int lo = hexval((unsigned char)v[i + 2]);
          if (hi >= 0 && lo >= 0) {
            out[o++] = (char)((hi << 4) | lo);
            i += 3;
            continue;
          }
        }
        out[o++] = (v[i] == '+') ? ' ' : v[i];
        i++;
      }
      out[o] = '\0';
      return true;
    }

    if (!amp)
      break;
    p = amp + 1;
  }

  return false;
}
