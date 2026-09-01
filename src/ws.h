#ifndef MOTORS_WS_H
#define MOTORS_WS_H

#include <stdbool.h>
#include <stddef.h>

/* ------------------------------------------------------------------------
 * VENDORED from thingino-motors, branch feature/websocket-daemon (src/ws.h).
 * See the provenance block at the top of ws.c for what was changed and why.
 * Kept close to the original so the two copies stay diffable.
 * ------------------------------------------------------------------------ */

/* Generic RFC 6455 WebSocket server mechanics: HTTP-Upgrade handshake,
 * frame read/write, masking, control frames.
 *
 * Deliberately knows NOTHING about motors, tokens or JSON. Everything that
 * is policy (which Origins are acceptable, which token unlocks the socket,
 * what the messages mean) lives in motor-ws.c; everything that is protocol
 * lives here. The split exists so a second daemon in this project family can
 * take ws.c/ws.h verbatim - there is no library to share, so the seam has to
 * carry that weight instead.
 *
 * TLS (wss://) rides on the ws_io handle below - the one seam every byte read
 * or written by this file passes through. ws.c itself still knows nothing
 * about TLS beyond "there may be a ws_tls_conn to call instead of
 * read()/send()"; the mbedTLS specifics live in ws_tls.c, compiled only when
 * MOTORS_WS_TLS is defined. The policy - which certificate, and how a plain
 * ws:// client is still accepted on the same port - is motor-ws.c's. */

/* Frame payload cap.
 *
 * Every command this protocol accepts is a short fixed-shape JSON object
 * (see motor-ws.c); 2 KiB is already an order of magnitude of headroom. The
 * cap is what stops a client from making the daemon allocate - the receive
 * buffer is a fixed member of ws_conn, never a malloc driven by the peer's
 * declared length, so an absurd 64-bit length header costs a close frame
 * rather than an OOM on a 64 MB camera.
 *
 * timps lowers the motors default of 2048 to 1024. The only payload this
 * daemon accepts is one 20 ms block of G.711 mu-law (talk_ws.c): 160 bytes at
 * 8 kHz, and 960 bytes at 48 kHz - the highest rate a browser AudioContext
 * may force on us when it declines the requested one (notably iOS Safari).
 * 1024 covers every rate in TALK_RATES with margin and halves the fixed
 * ws_conn.frag member, which lives on the per-connection stack. Guarded so a
 * board build can raise it without editing the vendored file. */
#ifndef WS_MAX_PAYLOAD
#define WS_MAX_PAYLOAD 1024
#endif

/* HTTP request head cap for the handshake. Long enough for a real browser's
 * upgrade request (Origin + Cookie + User-Agent + Sec-*), short enough that a
 * client that never sends the blank line cannot grow the daemon's memory. */
#define WS_MAX_HANDSHAKE 3072

/* opcodes (RFC 6455 section 5.2) */
#define WS_OP_CONT 0x0
#define WS_OP_TEXT 0x1
#define WS_OP_BIN 0x2
#define WS_OP_CLOSE 0x8
#define WS_OP_PING 0x9
#define WS_OP_PONG 0xA

/* close codes (RFC 6455 section 7.4.1) used by this implementation */
#define WS_CLOSE_NORMAL 1000
#define WS_CLOSE_GOING_AWAY 1001
#define WS_CLOSE_PROTOCOL 1002
#define WS_CLOSE_UNSUPPORTED 1003
#define WS_CLOSE_POLICY 1008
#define WS_CLOSE_TOO_BIG 1009
#define WS_CLOSE_INTERNAL 1011

/* return codes; negative values are all terminal for the connection */
#define WS_OK 0
#define WS_AGAIN 1     /* timeout expired with no complete frame */
#define WS_CLOSED (-1) /* peer sent CLOSE, or EOF */
#define WS_EPROTO (-2) /* framing violation -> close 1002 */
#define WS_ETOOBIG (-3)/* payload over WS_MAX_PAYLOAD -> close 1009 */
#define WS_EIO (-4)    /* socket error/timeout on a partial frame */

/* --- transport --- *
 *
 * Every read and write in ws.c goes through one of these instead of a bare
 * fd. A plain ws:// connection leaves `tls` NULL and the calls collapse to
 * read()/send(); a wss:// connection carries the ws_tls_conn that ws_tls.c
 * handed back and the same calls go through mbedTLS.
 *
 * `fd` stays visible rather than being hidden behind the handle because the
 * things that are NOT byte I/O still need the raw descriptor: poll(),
 * setsockopt(TCP_NODELAY/SO_KEEPALIVE), and close(). Only the byte movement
 * is abstracted, which keeps the abstraction to the two functions that
 * actually differ.
 *
 * Ownership: ws.c never allocates or frees either member. The caller accepts
 * the socket, optionally wraps it (ws_tls_accept), and is responsible for
 * ws_tls_close() + close() afterwards - see conn_thread() in motor-ws.c. */
typedef struct {
  int fd;
  /* ws_tls_conn *, or NULL for a plain socket. Deliberately void * so this
   * header stays buildable - and this struct stays one shape - whether or not
   * the TLS support was compiled in. */
  void *tls;
} ws_io;

/* Parsed handshake request. All strings are NUL-terminated and truncated to
 * the buffer size rather than overflowing; a truncated value simply fails to
 * match an allow-list entry, which is the safe direction. */
typedef struct {
  char method[8];
  char path[64];   /* request target with the query string stripped */
  char query[256]; /* everything after '?', or "" */
  char host[128];  /* Host: header value, port included if the client sent one */
  char origin[192];/* Origin: header value, or "" when the client sent none */
  char key[64];    /* Sec-WebSocket-Key: header value */
  int version;     /* Sec-WebSocket-Version:, -1 if absent/unparsable */
  bool has_upgrade_websocket;
  bool has_connection_upgrade;
  bool has_origin; /* distinguishes "Origin: " (empty) from no Origin at all */
  /* The raw request head, retained so callers can read headers this struct
   * does not model (ws_header() below). Keeping the bytes rather than adding
   * named fields for every application-specific header is what lets this
   * layer stay free of motors-specific knowledge. */
  char head[WS_MAX_HANDSHAKE + 1];
  size_t head_len;
} ws_handshake;

/* Read an arbitrary header value out of a parsed handshake. name is given
 * WITHOUT the colon; matching is case-insensitive and anchored at a line
 * start, so a value can never impersonate a header name. Returns true when
 * found; out is always NUL-terminated. */
bool ws_header(const ws_handshake *hs, const char *name, char *out, size_t cap);

typedef struct {
  /* Borrowed, not owned: it must outlive the ws_conn. In motor-ws.c both live
   * in the same per-connection ws_client, so that is automatic. */
  ws_io *io;
  /* Reassembly buffer for fragmented messages (RFC 6455 section 5.4). Fixed
   * size, so a fragment sequence that would exceed WS_MAX_PAYLOAD in total is
   * rejected with WS_ETOOBIG instead of growing without bound - the classic
   * way a naive WebSocket server is turned into a memory bomb. */
  unsigned char frag[WS_MAX_PAYLOAD];
  size_t frag_len;
  int frag_op; /* opcode of the message being reassembled, 0 when idle */
  /* Liveness. Stamped by ws_read_message() every time a COMPLETE, well-formed
   * frame of any kind arrives - PONG, PING, CLOSE, a data frame, or one
   * fragment of one. "Any frame" rather than "a PONG" on purpose: a PONG is
   * only the cheapest proof a peer is alive, not the only one, and a client
   * that never answers PINGs but does send commands is plainly not stale.
   * Read it through ws_conn_idle_ms(). */
  long long last_rx_ms;
} ws_conn;

void ws_conn_init(ws_conn *c, ws_io *io);

/* Milliseconds since an arbitrary fixed point, from CLOCK_MONOTONIC where the
 * platform has it. Exposed because every timer in this frontend has to agree
 * on one clock, and because a monotonic one is the only correct source for an
 * interval that can DISCONNECT someone: these cameras have no RTC, so the
 * wall clock takes a large forward step the moment NTP first syncs after boot
 * - which under gettimeofday() would look exactly like every open connection
 * simultaneously falling silent for hours. */
long long ws_now_ms(void);

/* How long since the peer last sent anything at all. The caller decides what
 * to do about it; see WS_LIVENESS_TIMEOUT_MS in motor-ws.c for this project's
 * policy. */
long long ws_conn_idle_ms(const ws_conn *c);

/* --- handshake --- */

/* Read and parse the HTTP request head (up to the blank line). Returns WS_OK,
 * WS_ETOOBIG (head over WS_MAX_HANDSHAKE), WS_EPROTO (malformed request line)
 * or WS_EIO/WS_CLOSED. Does not validate Origin or any credential: that is
 * policy, and policy lives in the caller. */
int ws_handshake_read(ws_io *io, ws_handshake *hs, int timeout_ms);

/* True if hs is a structurally valid RFC 6455 version-13 upgrade request
 * (GET, Upgrade: websocket, Connection: ...upgrade..., a Sec-WebSocket-Key,
 * version 13). Says nothing about whether it should be ALLOWED. */
bool ws_handshake_is_upgrade(const ws_handshake *hs);

/* Compute the Sec-WebSocket-Accept value for a Sec-WebSocket-Key:
 *   base64(SHA1(key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"))
 * out must hold at least 29 bytes (28 base64 chars + NUL). Exposed mainly so
 * the self-test can check the RFC 6455 section 1.3 vector. */
void ws_accept_key(const char *key, char out[29]);

/* Send the 101 response. subproto may be NULL. */
int ws_handshake_accept(ws_io *io, const ws_handshake *hs,
                        const char *subproto);

/* Send a plain HTTP error response and nothing else - used for every refusal
 * before the upgrade completes (bad origin, missing token, too many clients).
 * The body is a fixed string; nothing from the request is ever reflected, so
 * this cannot become a reflected-XSS or header-injection surface. */
int ws_handshake_reject(ws_io *io, int status, const char *status_text,
                        const char *body);

/* --- frames --- */

/* Read one complete (possibly reassembled) data message.
 *
 * Control frames are handled internally: PING is answered with PONG, PONG is
 * swallowed, CLOSE returns WS_CLOSED. The function therefore only ever hands
 * back TEXT or BIN, and the caller never has to think about the control
 * plane. timeout_ms bounds the total wait; WS_AGAIN means "nothing arrived",
 * which is the caller's cue to do its periodic work (status push, keepalive).
 */
int ws_read_message(ws_conn *c, int *opcode, unsigned char *out, size_t cap,
                    size_t *out_len, int timeout_ms);

/* Send an unmasked server frame (RFC 6455 section 5.1: the server MUST NOT
 * mask). Returns WS_OK or WS_EIO. */
int ws_send_frame(ws_io *io, int opcode, const void *payload, size_t len);
int ws_send_text(ws_io *io, const char *text);
int ws_send_ping(ws_io *io);
int ws_send_close(ws_io *io, int code, const char *reason);

/* --- base64 --- */

/* Standard base64 with padding. Returns the number of characters written
 * (excluding the NUL). out must hold 4*((len+2)/3)+1 bytes. Hand-rolled
 * because the handshake needs exactly one 20-byte encode and pulling in a
 * dependency for that would be absurd. */
size_t ws_base64_encode(const unsigned char *in, size_t len, char *out);

/* Extract a query-string parameter value ("name=value&..."). Returns true and
 * fills out (NUL-terminated, truncated to cap) when found. URL-decodes %XX
 * and '+'; a malformed escape is copied through literally rather than
 * consuming the terminator. */
bool ws_query_param(const char *query, const char *name, char *out, size_t cap);

#endif /* MOTORS_WS_H */
