#!/usr/bin/env python3
"""
rtsp-stall.py - deliberately hostile RTSP client for timps QA.

Does a normal RTSP handshake (OPTIONS / DESCRIBE / SETUP with TCP interleaving
/ PLAY) with Basic auth and then STOPS READING the socket entirely, while
holding the connection open. Because the media is interleaved on the same TCP
connection, a client that never reads makes the server's send window fill up -
which is exactly the condition a real-world stalled/wedged viewer creates.

It exists to answer questions no well-behaved client can:
  * does one stuck client degrade the healthy ones?  (fanqueue is per-client,
    so it must not - see src/fanqueue.h)
  * does it spike everyone's bitrate via forced IDR requests?  IDR requests are
    GLOBAL to the shared encoder, so the drop-triggered request is rate-limited
    on purpose (src/fanqueue.h:38-47, src/rtsp/rtsp.c) - a chronically slow
    client must not turn into a keyframe storm for every other subscriber.
  * does the memory it pins stay bounded?  (~1.5-2.5 MB per stalled client at
    4-6 Mbit/s per fanqueue.c's own estimate, on a 32 MB-class SoC.)

Used by scripts/timps-qa.sh section 13b (--test-hostile); also useful by hand.

Usage:
  python3 scripts/rtsp-stall.py --host 192.168.1.100 --secs 60
  python3 scripts/rtsp-stall.py --host CAM --path ch0 --user U --pw P --secs 45

Prints "STALLED" on stdout once PLAY has been accepted and it has gone deaf,
so a caller can wait for the hostile condition to be established rather than
guessing with a sleep. Exits non-zero if the handshake never got that far.

The server reaps a wedged TCP peer after SO_SNDTIMEO (15 s) of zero write
progress, so holding one socket for --secs would only be hostile for the first
~15-20 s of it. This re-establishes the stall on a timer just under that reap
(overlapping, so the hostile condition is continuous) and prints
"RESTALLED <n>" at the end if it had to.
"""
import argparse
import base64
import socket
import sys
import time

ap = argparse.ArgumentParser()
ap.add_argument("--host", required=True)
ap.add_argument("--port", type=int, default=554)
ap.add_argument("--path", default="ch0")
ap.add_argument("--user", default="thingino")
ap.add_argument("--pw", default="thingino")
ap.add_argument("--secs", type=float, default=60.0, help="how long to stay stalled")
args = ap.parse_args()

auth = "Basic " + base64.b64encode(f"{args.user}:{args.pw}".encode()).decode()
url = f"rtsp://{args.host}:{args.port}/{args.path}"
cseq = 0


def request(sock, verb, extra=""):
    """Send one RTSP request and return (status_code, headers_blob)."""
    global cseq
    cseq += 1
    req = (
        f"{verb} {url} RTSP/1.0\r\n"
        f"CSeq: {cseq}\r\n"
        f"User-Agent: timps-qa-stalled/1.0\r\n"
        f"Authorization: {auth}\r\n"
        f"{extra}\r\n"
    )
    sock.sendall(req.encode())
    # Read only up to the end of the response head. Deliberately bounded: we
    # must not drain any interleaved media that may already be queued behind
    # it, or we would not be a stalled client any more.
    buf = b""
    while b"\r\n\r\n" not in buf:
        chunk = sock.recv(1)
        if not chunk:
            return 0, buf.decode(errors="replace")
        buf += chunk
    head = buf.decode(errors="replace")
    try:
        code = int(head.split(" ", 2)[1])
    except (IndexError, ValueError):
        code = 0
    # A DESCRIBE body (the SDP) follows the head; consume exactly
    # Content-Length bytes so the next response starts at a clean boundary.
    clen = 0
    for line in head.split("\r\n"):
        if line.lower().startswith("content-length:"):
            try:
                clen = int(line.split(":", 1)[1].strip())
            except ValueError:
                clen = 0
    while clen > 0:
        chunk = sock.recv(min(clen, 4096))
        if not chunk:
            break
        clen -= len(chunk)
    return code, head


def header(blob, name):
    for line in blob.split("\r\n"):
        if line.lower().startswith(name.lower() + ":"):
            return line.split(":", 1)[1].strip()
    return ""


def stall_once():
    """One full handshake, left in the deaf state. Returns the live socket."""
    sk = socket.create_connection((args.host, args.port), timeout=10)
    sk.settimeout(10)
    try:
        code, _ = request(sk, "OPTIONS")
        if code != 200:
            raise RuntimeError(f"OPTIONS failed: {code}")
        code, blob = request(sk, "DESCRIBE", "Accept: application/sdp\r\n")
        if code != 200:
            raise RuntimeError(f"DESCRIBE failed: {code}")
        # Interleaved (RTP-over-the-same-TCP-connection) transport is the point:
        # it puts the media in the very socket we are refusing to read.
        code, blob = request(
            sk,
            "SETUP",
            "Transport: RTP/AVP/TCP;unicast;interleaved=0-1\r\n",
        )
        if code != 200:
            raise RuntimeError(f"SETUP failed: {code}")
        sess = header(blob, "Session").split(";")[0]
        if not sess:
            raise RuntimeError("SETUP returned no Session header")
        code, _ = request(sk, "PLAY", f"Session: {sess}\r\nRange: npt=0.000-\r\n")
        if code != 200:
            raise RuntimeError(f"PLAY failed: {code}")
    except Exception:
        sk.close()
        raise
    return sk


try:
    sock = stall_once()
except (OSError, RuntimeError) as e:
    print(e, file=sys.stderr)
    sys.exit(1)

# From here on: never read, never TEARDOWN, just hold the socket.
print("STALLED", flush=True)

# ...but the server does NOT hold it forever: timps gives a TCP-interleaved
# media write SO_SNDTIMEO seconds (15) of zero progress before it declares the
# peer dead and tears the session down (src/rtsp/rtsp.c, "H-1"). That reap is a
# feature - it is one of the two mechanisms (with the per-client fanqueue) that
# keeps a wedged viewer from mattering to anyone else - but it means a naive
# sleep(30) here is only actually hostile for its first ~15-20 s, and a caller
# measuring over a longer window would unknowingly average in a clean second
# half.
#
# The reap cannot be OBSERVED from this side, which is worth knowing before
# anyone "improves" this into a poll() loop: the server's close() has to push
# its queued media out before the FIN, our receive window is nailed shut
# because we never read, so the FIN never arrives and the socket just sits
# there looking established. Refresh on a timer instead - and establish the
# replacement BEFORE dropping the old one, so there is no instant in the
# window without a stalled client attached.
REFRESH_S = 12.0        # < the server's 15 s SO_SNDTIMEO reap
deadline = time.monotonic() + args.secs
restalls = 0
try:
    while True:
        left = deadline - time.monotonic()
        if left <= 0:
            break
        time.sleep(min(REFRESH_S, left))
        if time.monotonic() >= deadline:
            break
        try:
            fresh = stall_once()
        except (OSError, RuntimeError) as e:
            print(f"re-stall failed after {restalls} refresh(es): {e}",
                  file=sys.stderr)
            sys.exit(1)
        sock.close()
        sock = fresh
        restalls += 1
finally:
    try:
        sock.close()
    except OSError:
        pass
if restalls:
    print(f"RESTALLED {restalls}", flush=True)
