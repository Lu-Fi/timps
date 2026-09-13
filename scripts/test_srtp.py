#!/usr/bin/env python3
"""Independent (Python) check of src/webrtc/srtp.c.

Nothing here is shared with the C code: AES-128 is reimplemented below from
FIPS-197, the HMAC comes from hashlib, and the SRTP/SRTCP framing is written
straight from RFC 3711. The key-derivation results are additionally pinned to
the published RFC 3711 B.3 test vector, so a self-consistent-but-wrong pair of
implementations still fails.
"""
import binascii, hashlib, hmac, os, struct, subprocess, sys

BIN = sys.argv[1]

# --- AES-128, FIPS-197, table-free ------------------------------------------
_SB = [0] * 256
_p = _q = 1
while True:                       # generate the S-box instead of copying it
    _p = _p ^ ((_p << 1) & 0xFF) ^ (0x1B if _p & 0x80 else 0)
    _q ^= _q << 1; _q ^= _q << 2; _q ^= _q << 4; _q &= 0xFF
    if _q & 0x80: _q ^= 0x09
    x = _q ^ ((_q << 1) | (_q >> 7)) ^ ((_q << 2) | (_q >> 6)) \
          ^ ((_q << 3) | (_q >> 5)) ^ ((_q << 4) | (_q >> 4))
    _SB[_p] = (x & 0xFF) ^ 0x63
    if _p == 1: break
_SB[0] = 0x63


def _xt(a):
    return ((a << 1) ^ 0x1B) & 0xFF if a & 0x80 else (a << 1) & 0xFF


def _expand(key):
    w = [list(key[i:i + 4]) for i in range(0, 16, 4)]
    rcon = 1
    for i in range(4, 44):
        t = list(w[i - 1])
        if i % 4 == 0:
            t = t[1:] + t[:1]
            t = [_SB[b] for b in t]
            t[0] ^= rcon
            rcon = _xt(rcon)
        w.append([w[i - 4][j] ^ t[j] for j in range(4)])
    return [bytes(b for word in w[r * 4:r * 4 + 4] for b in word) for r in range(11)]


def aes128(key, block):
    rk = _expand(key)
    s = bytearray(a ^ b for a, b in zip(block, rk[0]))
    for r in range(1, 11):
        s = bytearray(_SB[b] for b in s)
        # ShiftRows on the column-major state
        s = bytearray([s[0], s[5], s[10], s[15], s[4], s[9], s[14], s[3],
                       s[8], s[13], s[2], s[7], s[12], s[1], s[6], s[11]])
        if r != 10:
            n = bytearray(16)
            for c in range(0, 16, 4):
                a = s[c:c + 4]
                for j in range(4):
                    n[c + j] = (_xt(a[j]) ^ _xt(a[(j + 1) % 4]) ^ a[(j + 1) % 4]
                                ^ a[(j + 2) % 4] ^ a[(j + 3) % 4])
            s = n
        s = bytearray(a ^ b for a, b in zip(s, rk[r]))
    return bytes(s)


def aes_ctr(key, iv, data):
    out, ctr = bytearray(), int.from_bytes(iv, "big")
    for off in range(0, len(data), 16):
        ks = aes128(key, ctr.to_bytes(16, "big"))
        chunk = data[off:off + 16]
        out += bytes(a ^ b for a, b in zip(chunk, ks))
        ctr += 1
    return bytes(out)


try:                                        # cross-check the AES above
    from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes
    k, b = os.urandom(16), os.urandom(16)
    ref = Cipher(algorithms.AES(k), modes.ECB()).encryptor().update(b)
    assert aes128(k, b) == ref, "this script's AES-128 disagrees with pyca"
    print("AES-128 (this script) matches pyca/cryptography")
except ImportError:
    print("pyca/cryptography not installed - AES-128 self-check skipped")


# --- RFC 3711 -----------------------------------------------------------------
def kdf(master_key, master_salt, label, n):
    x = bytearray(master_salt) + b"\x00\x00"
    x[7] ^= label
    out = b""
    for i in range((n + 15) // 16):
        out += aes128(master_key, bytes(x[:14]) + struct.pack(">H", i))
    return out[:n]


def session_keys(master_key, master_salt, rtcp=False):
    base = 3 if rtcp else 0
    return (kdf(master_key, master_salt, base + 0, 16),
            kdf(master_key, master_salt, base + 1, 20),
            kdf(master_key, master_salt, base + 2, 14))


def iv(salt, ssrc, index48):
    v = int.from_bytes(salt + b"\x00\x00", "big")
    v ^= ssrc << 64
    v ^= index48 << 16
    return v.to_bytes(16, "big")


def protect_rtp(key, auth, salt, pkt, roc):
    hlen = 12 + 4 * (pkt[0] & 0x0F)
    seq = struct.unpack(">H", pkt[2:4])[0]
    ssrc = struct.unpack(">I", pkt[8:12])[0]
    body = aes_ctr(key, iv(salt, ssrc, (roc << 16) | seq), pkt[hlen:])
    out = pkt[:hlen] + body
    return out + hmac.new(auth, out + struct.pack(">I", roc), hashlib.sha1).digest()[:10]


def protect_rtcp(key, auth, salt, pkt, index):
    ssrc = struct.unpack(">I", pkt[4:8])[0]
    body = aes_ctr(key, iv(salt, ssrc, index), pkt[8:])
    out = pkt[:8] + body + struct.pack(">I", 0x80000000 | index)
    return out + hmac.new(auth, out, hashlib.sha1).digest()[:10]


def run(*args):
    p = subprocess.run([BIN] + list(args), capture_output=True, text=True)
    return p.stdout.strip().split("\n")


H = lambda b: binascii.hexlify(b).decode()
U = binascii.unhexlify

# --- 1. key derivation against RFC 3711 B.3 ----------------------------------
MK = U("E1F97A0D3E018BE0D64FA32C06DE4139")
MS = U("0EC675AD498AFEEBB6960B3AABE6")
VEC = {0x00: "c61e7a93744f39ee10734afe3ff7a087",
       0x01: "cebe321f6ff7716b6fd4ab49af256a156d38baa4",
       0x02: "30cbbc08863d8c85d49db34a9ae1"}
for label, want in VEC.items():
    n = len(want) // 2
    got = run("kdf", H(MK), H(MS), str(label), str(n))[0]
    assert got == want, "C KDF label %d: %s != %s" % (label, got, want)
    assert H(kdf(MK, MS, label, n)) == want, "python KDF label %d" % label
print("KDF matches RFC 3711 B.3 (cipher key, auth key, salt) in C and Python")

for label in (0x03, 0x04, 0x05):            # SRTCP keys: C vs Python only
    n = {3: 16, 4: 20, 5: 14}[label]
    assert run("kdf", H(MK), H(MS), str(label), str(n))[0] == H(kdf(MK, MS, label, n))
print("SRTCP key derivation (labels 3/4/5) matches the Python KDF")

# --- 2. SRTP protect, including a sequence-number wrap ------------------------
KM = os.urandom(60)
ck, sk, cs, ss = KM[:16], KM[16:32], KM[32:46], KM[46:60]
SSRC = 0x1BADCAFE


def rtp_pkt(seq, payload):
    return struct.pack(">BBHII", 0x80, 96, seq, 0x11223344, SSRC) + payload


pkts = [rtp_pkt(s, os.urandom(160)) for s in (0xFFFD, 0xFFFE, 0xFFFF, 0x0000, 0x0001)]
for server in (1, 0):
    mk, ms = (sk, ss) if server else (ck, cs)
    key, auth, salt = session_keys(mk, ms)
    got = run("rtp", H(KM), str(server), *[H(p) for p in pkts])
    roc = 0
    for i, p in enumerate(pkts):
        if i and struct.unpack(">H", p[2:4])[0] < struct.unpack(">H", pkts[i - 1][2:4])[0]:
            roc += 1
        want = H(protect_rtp(key, auth, salt, p, roc))
        assert got[i] == want, "SRTP pkt %d (roc=%d):\n  C: %s\n  py:%s" % (i, roc, got[i], want)
    assert roc == 1, roc
print("SRTP protect matches byte-for-byte in both roles, ROC rolls over on wrap")

# --- 2b. two bundled SSRCs share one context but NOT one ROC -----------------
# The audio track of a WebRTC BUNDLE runs its own SSRC and its own sequence
# space over the same DTLS-SRTP association. With a single shared rollover
# counter, every interleave looks like a wrap: SSRC B's low seq after SSRC A's
# high one bumps the ROC, and the receiver - which keeps a context per SSRC -
# can no longer rebuild the IV. Interleave two streams deliberately out of
# phase and require each to be protected against ITS OWN roc (0 throughout).
SSRC_B = 0x0BADF00D


def rtp_pkt_ssrc(ssrc, seq, payload):
    return struct.pack(">BBHII", 0x80, 96, seq, 0x11223344, ssrc) + payload


mix = []
for a_seq, b_seq in ((0xFFF0, 0x0001), (0xFFF1, 0x0002), (0xFFF2, 0x0003)):
    mix.append(rtp_pkt_ssrc(SSRC, a_seq, os.urandom(80)))
    mix.append(rtp_pkt_ssrc(SSRC_B, b_seq, os.urandom(80)))
key, auth, salt = session_keys(sk, ss)
got = run("rtp", H(KM), "1", *[H(p) for p in mix])
for i, p in enumerate(mix):
    want = H(protect_rtp(key, auth, salt, p, 0))
    assert got[i] == want, "bundled pkt %d:\n  C: %s\n  py:%s" % (i, got[i], want)
print("two interleaved SSRCs each keep their own ROC (neither is bumped)")

# A third SSRC has no context left. Refusing is the only safe answer: reusing
# another stream's ROC would emit a packet the peer cannot decrypt, silently.
out = run("rtp", H(KM), "1", H(mix[0]), H(mix[1]),
          H(rtp_pkt_ssrc(0xDEADBEEF, 1, os.urandom(16))))
assert out[2] == "ERR", out
print("a third outbound SSRC is refused, not given a borrowed ROC")

# --- 3. SRTCP protect --------------------------------------------------------
sr = struct.pack(">BBH", 0x80, 200, 6) + struct.pack(">I", SSRC) + os.urandom(20)
key, auth, salt = session_keys(sk, ss, rtcp=True)
got = run("rtcp", H(KM), "1", H(sr), H(sr))
for i in (0, 1):
    want = H(protect_rtcp(key, auth, salt, sr, i + 1))
    assert got[i] == want, "SRTCP pkt %d:\n  C: %s\n  py:%s" % (i, got[i], want)
print("SRTCP protect matches, E-flag set and the index increments")

# --- 4. SRTCP unprotect: accept, reject a tampered tag, reject a replay -------
# The C session below is the peer of the one above: it is the CLIENT, so what
# it verifies as inbound is exactly what a server protected.
ckey, cauth, csalt = session_keys(sk, ss, rtcp=True)
good = [protect_rtcp(ckey, cauth, csalt, sr, i) for i in (1, 2, 3)]
bad_tag = good[0][:-1] + bytes([good[0][-1] ^ 1])
bad_body = good[1][:12] + bytes([good[1][12] ^ 1]) + good[1][13:]

out = run("unrtcp", H(KM), "0", H(good[0]), H(good[1]), H(good[2]))
assert out[0] == H(sr), "decrypted SRTCP mismatch:\n  %s\n  %s" % (out[0], H(sr))
assert out[1] == H(sr) and out[2] == H(sr)
print("SRTCP unprotect recovers the plaintext compound packet")

out = run("unrtcp", H(KM), "0", H(bad_tag))
assert out[0] == "REJECT", out
out = run("unrtcp", H(KM), "0", H(bad_body))
assert out[0] == "REJECT", out
print("tampered auth tag and tampered ciphertext both rejected")

out = run("unrtcp", H(KM), "0", H(good[1]), H(good[0]), H(good[2]))
assert out[0] == H(sr) and out[1] == "REJECT" and out[2] == H(sr), out
print("replayed/old SRTCP index rejected")

# --- 5. wrong role must not verify ------------------------------------------
out = run("unrtcp", H(KM), "1", H(good[0]))
assert out[0] == "REJECT", out
print("packet protected with the server key is rejected by a server-role session")
print("ALL OK")
