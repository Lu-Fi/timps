#!/usr/bin/env python3
"""
talk-ws.py - synthetic /talk WebSocket client for timps (no browser needed).

Exercises the browser-microphone backchannel (USE_BC_WS, src/rtsp/talk_ws.c)
the way a page would, but from a shell: does the RFC 6455 upgrade itself over
TLS, verifies Sec-WebSocket-Accept, optionally streams G.711 mu-law silence as
20 ms binary frames, then closes cleanly with a real close frame so the camera
runs bc_release() immediately instead of waiting out its stale-owner timeout.

Every interesting fact is printed as a machine-greppable line so
scripts/timps-qa.sh can assert on it without parsing prose:

  HTTP <code>            status of the upgrade response
  ACCEPT ok|bad          Sec-WebSocket-Accept matched the RFC 6455 derivation
  HELLO <json>           the server's hello text frame
  SENT <n>               binary audio frames sent
  CLOSE <code>|none      close frame the server sent back
  RESULT ok|fail: <why>  overall verdict (also the exit status: 0 = ok)

Usage:
  python3 scripts/talk-ws.py --host 192.168.1.100 --token abc            # handshake only
  python3 scripts/talk-ws.py --host 192.168.1.100 --token abc --secs 1   # + stream silence
  python3 scripts/talk-ws.py --host 192.168.1.100                        # no token (expect 401/403)
  python3 scripts/talk-ws.py --host 192.168.1.100 --token abc --rate 12345
  python3 scripts/talk-ws.py --host 192.168.1.100 --token abc --method POST
  python3 scripts/talk-ws.py --host 192.168.1.100 --token abc --unmasked
"""
import argparse
import base64
import hashlib
import os
import socket
import ssl
import struct
import sys
import time

GUID = b"258EAFA5-E914-47DA-95CA-C5AB0DC85B11"

# RFC 6455 section 1.3's own example key, so ACCEPT is checked against a value
# published in the spec rather than one this script also computed for itself.
SAMPLE_KEY = "dGhlIHNhbXBsZSBub25jZQ=="
SAMPLE_ACCEPT = "s3pPLMBiTxaQ9kYGzzhZRbK+xOo="

# G.711 mu-law encoding of PCM 0 (see g711_ulaw_encode in src/codec/g711.c:
# 0 -> +BIAS -> exp 0, mant 0 -> ~0). Digital silence, so a QA run does not
# make the camera audibly beep in someone's house at 3am.
ULAW_SILENCE = 0xFF

OPC = {0x1: "text", 0x2: "bin", 0x8: "close", 0x9: "ping", 0xA: "pong"}


def out(line):
    print(line, flush=True)


class Reader:
    """Buffered exact-length reader over a socket (recv returns short reads)."""

    def __init__(self, sock):
        self.sock = sock
        self.buf = b""

    def read_exact(self, n):
        while len(self.buf) < n:
            d = self.sock.recv(65536)
            if not d:
                raise EOFError("peer closed")
            self.buf += d
        b, self.buf = self.buf[:n], self.buf[n:]
        return b

    def read_head(self, cap=8192):
        """Read up to and including the CRLFCRLF that ends an HTTP head."""
        while b"\r\n\r\n" not in self.buf and len(self.buf) < cap:
            d = self.sock.recv(4096)
            if not d:
                break
            self.buf += d
        i = self.buf.find(b"\r\n\r\n")
        if i < 0:
            head, self.buf = self.buf, b""
            return head
        head, self.buf = self.buf[: i + 4], self.buf[i + 4 :]
        return head


def frame(op, payload=b"", masked=True):
    """Build one client frame. RFC 6455 5.1: a client MUST mask; timps'
    ws_read_message() rejects an unmasked frame with a protocol error, which
    --unmasked exists to prove."""
    n = len(payload)
    h = bytearray()
    h.append(0x80 | op)  # FIN + opcode
    mbit = 0x80 if masked else 0x00
    if n < 126:
        h.append(mbit | n)
    elif n < 65536:
        h.append(mbit | 126)
        h += struct.pack(">H", n)
    else:
        h.append(mbit | 127)
        h += struct.pack(">Q", n)
    if not masked:
        return bytes(h) + payload
    key = os.urandom(4)
    masked_payload = bytes(payload[i] ^ key[i & 3] for i in range(n))
    return bytes(h) + key + masked_payload


def read_frame(rd, timeout=None):
    """Read one server frame -> (opcode, payload). Servers never mask."""
    if timeout is not None:
        rd.sock.settimeout(timeout)
    h = rd.read_exact(2)
    op = h[0] & 0x0F
    masked = (h[1] & 0x80) != 0
    n = h[1] & 0x7F
    if n == 126:
        n = struct.unpack(">H", rd.read_exact(2))[0]
    elif n == 127:
        n = struct.unpack(">Q", rd.read_exact(8))[0]
    key = rd.read_exact(4) if masked else None
    payload = rd.read_exact(n) if n else b""
    if key:
        payload = bytes(payload[i] ^ key[i & 3] for i in range(len(payload)))
    return op, payload


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="192.168.1.100")
    ap.add_argument("--port", type=int, default=8880)
    ap.add_argument("--token", default="")
    ap.add_argument("--rate", type=int, default=8000)
    ap.add_argument("--secs", type=float, default=0.0,
                    help="stream this many seconds of mu-law silence (0 = handshake only)")
    ap.add_argument("--method", default="GET", help="override the request method (RFC 6455 wants GET)")
    ap.add_argument("--unmasked", action="store_true",
                    help="send one UNMASKED frame; the server must reject it")
    ap.add_argument("--plain", action="store_true", help="plain TCP instead of TLS")
    ap.add_argument("--origin", default=None, help="send this Origin header")
    ap.add_argument("--timeout", type=float, default=10.0)
    a = ap.parse_args()

    # --- connect -----------------------------------------------------------
    try:
        sock = socket.create_connection((a.host, a.port), timeout=a.timeout)
    except Exception as e:
        out("RESULT fail: connect: %s" % e)
        return 1
    if not a.plain:
        # The camera's certificate is self-signed by design (S95timps /
        # ensure_tls_certs), so verification is off: this tests the /talk
        # protocol, not the PKI.
        cctx = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
        cctx.check_hostname = False
        cctx.verify_mode = ssl.CERT_NONE
        try:
            sock = cctx.wrap_socket(sock, server_hostname=a.host)
        except Exception as e:
            out("RESULT fail: tls: %s" % e)
            return 1
    sock.settimeout(a.timeout)
    rd = Reader(sock)

    # --- upgrade -----------------------------------------------------------
    path = "/talk?rate=%d" % a.rate
    if a.token:
        path += "&token=" + a.token
    req = [
        "%s %s HTTP/1.1" % (a.method, path),
        "Host: %s:%d" % (a.host, a.port),
        "Upgrade: websocket",
        "Connection: keep-alive, Upgrade",
        "Sec-WebSocket-Key: " + SAMPLE_KEY,
        "Sec-WebSocket-Version: 13",
    ]
    if a.origin:
        req.append("Origin: " + a.origin)
    try:
        sock.sendall(("\r\n".join(req) + "\r\n\r\n").encode())
        head = rd.read_head()
    except Exception as e:
        out("RESULT fail: upgrade write/read: %s" % e)
        return 1
    if not head:
        out("RESULT fail: no response to the upgrade")
        return 1

    first = head.split(b"\r\n", 1)[0].decode("latin-1")
    parts = first.split(" ")
    code = parts[1] if len(parts) > 1 else "?"
    out("HTTP " + code)
    if code != "101":
        # A refusal is a perfectly good outcome for the negative tests, so this
        # is not a failure of the tool - the caller decides what the code means.
        out("RESULT ok: refused with HTTP %s" % code)
        return 0

    want = base64.b64encode(hashlib.sha1(SAMPLE_KEY.encode() + GUID).digest()).decode()
    got = ""
    for line in head.decode("latin-1").split("\r\n"):
        if line.lower().startswith("sec-websocket-accept:"):
            got = line.split(":", 1)[1].strip()
    ok_accept = (got == want == SAMPLE_ACCEPT)
    out("ACCEPT " + ("ok" if ok_accept else "bad"))
    if not ok_accept:
        out("RESULT fail: Sec-WebSocket-Accept %r, expected %r" % (got, SAMPLE_ACCEPT))
        return 1

    # --- the hello frame talk_ws.c sends on connect ------------------------
    try:
        op, payload = read_frame(rd, timeout=3.0)
        if op == 0x1:
            out("HELLO " + payload.decode("utf-8", "replace"))
    except Exception:
        out("HELLO none")

    # --- negative: an unmasked frame must be rejected ----------------------
    if a.unmasked:
        try:
            sock.sendall(frame(0x2, bytes([ULAW_SILENCE]) * 160, masked=False))
            op, payload = read_frame(rd, timeout=5.0)
            if op == 0x8:
                c = struct.unpack(">H", payload[:2])[0] if len(payload) >= 2 else 0
                out("CLOSE %d" % c)
                out("RESULT ok: unmasked frame rejected (close %d)" % c)
                return 0
            out("RESULT fail: unmasked frame accepted (got %s)" % OPC.get(op, op))
            return 1
        except EOFError:
            # dropping the connection outright is also a valid rejection
            out("CLOSE none")
            out("RESULT ok: unmasked frame rejected (connection dropped)")
            return 0
        except Exception as e:
            out("RESULT fail: unmasked probe: %s" % e)
            return 1

    # --- stream mu-law silence in real time --------------------------------
    sent = 0
    if a.secs > 0:
        nsamp = max(1, int(round(a.rate * 0.020)))   # 20 ms, same as the page
        block = bytes([ULAW_SILENCE]) * nsamp
        nframes = int(a.secs / 0.020)
        t0 = time.monotonic()
        try:
            for i in range(nframes):
                sock.sendall(frame(0x2, block))
                sent += 1
                # pace to real time: bursting would exercise talk_ws.c's
                # backlog guard rather than the normal path
                due = t0 + (i + 1) * 0.020
                slack = due - time.monotonic()
                if slack > 0:
                    time.sleep(slack)
        except Exception as e:
            out("SENT %d" % sent)
            out("RESULT fail: send: %s" % e)
            return 1
    out("SENT %d" % sent)

    # --- clean client-initiated close --------------------------------------
    try:
        sock.sendall(frame(0x8, struct.pack(">H", 1000) + b"bye"))
    except Exception as e:
        out("RESULT fail: close write: %s" % e)
        return 1
    closed = "none"
    try:
        deadline = time.monotonic() + 5.0
        while time.monotonic() < deadline:
            op, payload = read_frame(rd, timeout=5.0)
            if op == 0x8:
                closed = str(struct.unpack(">H", payload[:2])[0]) if len(payload) >= 2 else "0"
                break
            # ignore a ping/pong/hello that raced our close
    except Exception:
        pass
    out("CLOSE " + closed)
    try:
        sock.close()
    except Exception:
        pass
    out("RESULT ok: session completed (%d frames)" % sent)
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        sys.exit(130)
