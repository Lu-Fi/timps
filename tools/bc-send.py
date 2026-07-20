#!/usr/bin/env python3
"""
bc-send.py - raw ONVIF backchannel tester for timps (no GStreamer).

Does the RTSP handshake itself (DESCRIBE+Require / SETUP trackID=2 / PLAY) with
Basic auth, then streams an RTP PCMU (G.711 mu-law) tone to the negotiated
server port. Prints the SDP + SETUP/PLAY responses so you can see exactly what
timps advertises.

Usage:
  python3 tools/bc-send.py                          # 440 Hz tone, 10 s
  python3 tools/bc-send.py --host 192.168.241.190 --secs 20 --freq 800
"""
import socket, base64, sys, time, math, struct, random, argparse, re

ap = argparse.ArgumentParser()
ap.add_argument("--host", default="192.168.241.190")
ap.add_argument("--port", type=int, default=554)
ap.add_argument("--path", default="ch0")
ap.add_argument("--user", default="thingino")
ap.add_argument("--pw", default="thingino")
ap.add_argument("--freq", type=float, default=440.0)
ap.add_argument("--secs", type=float, default=10.0)
args = ap.parse_args()

auth = "Basic " + base64.b64encode(f"{args.user}:{args.pw}".encode()).decode()
base_url = f"rtsp://{args.host}:{args.port}/{args.path}"

def ulaw(sample):                      # int16 -> uint8 mu-law (matches g711.c)
    BIAS, CLIP = 0x84, 32635
    sign = 0x80 if sample < 0 else 0
    if sign: sample = -sample
    if sample > CLIP: sample = CLIP
    sample += BIAS
    exp, m = 7, 0x4000
    while (sample & m) == 0 and exp > 0:
        exp -= 1; m >>= 1
    mant = (sample >> (exp + 3)) & 0x0F
    return (~(sign | (exp << 4) | mant)) & 0xFF

s = socket.create_connection((args.host, args.port), timeout=5)
cseq = [0]
def rtsp(method, url, extra=""):
    cseq[0] += 1
    s.sendall(f"{method} {url} RTSP/1.0\r\nCSeq: {cseq[0]}\r\n"
              f"Authorization: {auth}\r\n{extra}\r\n".encode())
    data = b""
    while b"\r\n\r\n" not in data:
        chunk = s.recv(4096)
        if not chunk: break
        data += chunk
    m = re.search(rb"Content-Length:\s*(\d+)", data, re.I)
    if m:
        start = data.find(b"\r\n\r\n") + 4
        while len(data) - start < int(m.group(1)):
            data += s.recv(4096)
    return data.decode(errors="replace")

# 1) DESCRIBE with the ONVIF backchannel Require header
d = rtsp("DESCRIBE", base_url,
         "Require: www.onvif.org/ver20/backchannel\r\nAccept: application/sdp\r\n")
print("=== DESCRIBE ===\n" + d + "\n")
if "trackID=2" not in d:
    sys.exit("!! no backchannel (trackID=2) in SDP - is audio.backchannel=1 and /bin/iac present?")

# 2) SETUP the backchannel track (bind our RTP socket first, use its real port)
u = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
u.bind(("", 0))
cli = u.getsockname()[1]
r = rtsp("SETUP", base_url + "/trackID=2",
         f"Transport: RTP/AVP;unicast;client_port={cli}-{cli+1}\r\n")
print("=== SETUP ===\n" + r + "\n")
sp = re.search(r"server_port=(\d+)-(\d+)", r)
se = re.search(r"Session:\s*([^\r\n;]+)", r)
if not sp or not se:
    sys.exit("!! SETUP failed (no server_port/Session)")
srv = int(sp.group(1)); session = se.group(1).strip()

# 3) PLAY
p = rtsp("PLAY", base_url, f"Session: {session}\r\n")
print("=== PLAY ===\n" + p + "\n")

# 4) stream RTP PCMU to host:srv (socket u was bound above)
seq = random.randint(0, 0xffff); ts = 0; ssrc = random.randint(0, 0xffffffff)
phase = 0.0
print(f">>> sending {args.freq:.0f} Hz PCMU tone to {args.host}:{srv} for {args.secs:.0f}s")
print("    watch on the camera:  logread -f | grep -iE '\\[bc\\]|iac'")
for _ in range(int(args.secs / 0.02)):
    pl = bytearray(160)                                   # 20 ms @ 8 kHz
    for j in range(160):
        pl[j] = ulaw(int(12000 * math.sin(phase)))
        phase += 2 * math.pi * args.freq / 8000
    u.sendto(struct.pack("!BBHII", 0x80, 0x00, seq & 0xffff, ts & 0xffffffff, ssrc) + bytes(pl),
             (args.host, srv))
    seq += 1; ts += 160
    time.sleep(0.02)

rtsp("TEARDOWN", base_url, f"Session: {session}\r\n")
print("done.")
