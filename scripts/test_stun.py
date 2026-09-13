#!/usr/bin/env python3
"""Independent (Python) check of src/webrtc/stun.c: verify what the C code
emits, and hand it a request the C code must accept."""
import binascii, hashlib, hmac, os, struct, subprocess, sys, zlib

BIN = sys.argv[1]
PWD = b"VOkJxbRl1RmTxUk/WvJxBt"
MAGIC = 0x2112A442


def integrity(msg):
    """HMAC-SHA1 over msg with the length field patched to include MI."""
    plen = len(msg) - 20 + 24
    m = msg[:2] + struct.pack(">H", plen) + msg[4:]
    return hmac.new(PWD, m, hashlib.sha1).digest()


def fingerprint(msg):
    plen = len(msg) - 20 + 8
    m = msg[:2] + struct.pack(">H", plen) + msg[4:]
    return (zlib.crc32(m) & 0xFFFFFFFF) ^ 0x5354554E


def attrs(msg):
    off, out = 20, []
    while off + 4 <= len(msg):
        t, l = struct.unpack(">HH", msg[off:off + 4])
        out.append((t, off, msg[off + 4:off + 4 + l]))
        off = (off + 4 + l + 3) & ~3
    return out


# --- 1. check the response the C code builds ---------------------------------
txid = os.urandom(12)
ip, port = "192.168.10.77", 51234
hexout = subprocess.check_output(
    [BIN, "resp", binascii.hexlify(txid).decode(), ip, str(port)]).strip()
msg = binascii.unhexlify(hexout)

assert struct.unpack(">H", msg[0:2])[0] == 0x0101, "not a success response"
assert struct.unpack(">H", msg[2:4])[0] == len(msg) - 20, "bad length field"
assert msg[4:8] == struct.pack(">I", MAGIC)
assert msg[8:20] == txid, "txid not echoed"

got = dict((t, (off, v)) for t, off, v in attrs(msg))
assert 0x0020 in got and 0x0008 in got and 0x8028 in got, "missing attributes"

off, v = got[0x0020]
assert v[1] == 1, "not IPv4"
xport = struct.unpack(">H", v[2:4])[0] ^ (MAGIC >> 16)
xaddr = struct.unpack(">I", v[4:8])[0] ^ MAGIC
assert xport == port, xport
assert struct.pack(">I", xaddr) == bytes(int(x) for x in ip.split(".")), xaddr

off, v = got[0x0008]
assert v == integrity(msg[:off]), "MESSAGE-INTEGRITY mismatch"
off, v = got[0x8028]
assert struct.unpack(">I", v)[0] == fingerprint(msg[:off]), "FINGERPRINT mismatch"
print("response OK: XOR-MAPPED-ADDRESS, MESSAGE-INTEGRITY, FINGERPRINT all verify")


# --- 2. hand the C code a request it must accept -----------------------------
def build_request(username, use_candidate, break_mi=False, break_fp=False):
    txid = os.urandom(12)
    body = b""

    def attr(t, v):
        return struct.pack(">HH", t, len(v)) + v + b"\x00" * ((4 - len(v) % 4) % 4)

    body += attr(0x0006, username)                     # USERNAME
    body += attr(0x0024, struct.pack(">I", 0x6E7F1EFF))  # PRIORITY
    body += attr(0x802A, os.urandom(8))                # ICE-CONTROLLING
    if use_candidate:
        body += attr(0x0025, b"")
    hdr = struct.pack(">HHI", 0x0001, len(body), MAGIC) + txid
    mi = integrity(hdr + body)
    if break_mi:
        mi = bytes(b ^ 1 for b in mi)
    body += struct.pack(">HH", 0x0008, 20) + mi
    hdr = struct.pack(">HHI", 0x0001, len(body), MAGIC) + txid
    fp = fingerprint(hdr + body)
    if break_fp:
        fp ^= 1
    body += struct.pack(">HH", 0x8028, 4) + struct.pack(">I", fp)
    hdr = struct.pack(">HHI", 0x0001, len(body), MAGIC) + txid
    return hdr + body


def run_parse(req):
    p = subprocess.run([BIN, "parse", binascii.hexlify(req).decode()],
                       capture_output=True, text=True)
    return p.stdout.strip()


r = run_parse(build_request(b"LFRG:RFRG", False))
assert r == "1 LFRG:RFRG 0", r
print("request accepted:", r)
r = run_parse(build_request(b"LFRG:RFRG", True))
assert r == "1 LFRG:RFRG 1", r
print("nominated request accepted:", r)
r = run_parse(build_request(b"LFRG:RFRG", True, break_mi=True))
assert r.startswith("0"), r
print("tampered MESSAGE-INTEGRITY rejected")
r = run_parse(build_request(b"LFRG:RFRG", True, break_fp=True))
assert r.startswith("0"), r
print("tampered FINGERPRINT rejected")
print("ALL OK")
