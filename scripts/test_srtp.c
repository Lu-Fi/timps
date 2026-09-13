/* Host harness for src/webrtc/srtp.c, driven by scripts/test_srtp.py (see
 * `make test-srtp`). Hex in, hex out - every expected value is produced
 * independently in Python (its own AES-128 + hmac/hashlib), so the KDF, the
 * per-packet IV, the ROC handling and the auth tags are checked against code
 * that shares nothing with ours. Not part of the daemon. */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "srtp.h"

static int hex2bin(const char *h, uint8_t *o, int cap)
{
    int n = 0;
    while (h[0] && h[1] && n < cap) {
        unsigned v; sscanf(h, "%2x", &v); o[n++] = (uint8_t)v; h += 2;
    }
    return n;
}

static void puthex(const uint8_t *p, int n)
{
    for (int i = 0; i < n; i++) printf("%02x", p[i]);
    printf("\n");
}

/* argv[2] is always the 60-byte keying material; argv[3] the role
 * (1 = we are the DTLS server, i.e. we protect with the server key). */
static int load(srtp_session *s, char **argv)
{
    uint8_t km[SRTP_KEYING_LEN];
    if (hex2bin(argv[2], km, sizeof km) != SRTP_KEYING_LEN) return -1;
    return srtp_init(s, km, sizeof km, atoi(argv[3]));
}

int main(int argc, char **argv)
{
    if (argc < 3) return 1;

    if (!strcmp(argv[1], "kdf")) {          /* kdf <key32> <salt28> <label> <n> */
        uint8_t mk[16], ms[14], out[64];
        if (hex2bin(argv[2], mk, 16) != 16 || hex2bin(argv[3], ms, 14) != 14)
            return 1;
        int n = atoi(argv[5]);
        if (n < 1 || n > (int)sizeof out) return 1;
        srtp_kdf(mk, ms, (uint8_t)strtol(argv[4], NULL, 0), out, n);
        puthex(out, n);
        return 0;
    }

    srtp_session s;
    if (argc < 5 || load(&s, argv) != 0) return 1;

    /* rtp <km> <server> <pkt> [<pkt> ...] - protects each packet in order on
     * ONE session, so a wrapping sequence number exercises the ROC. */
    if (!strcmp(argv[1], "rtp")) {
        for (int i = 4; i < argc; i++) {
            uint8_t buf[2048];
            int n = hex2bin(argv[i], buf, (int)sizeof buf - SRTP_MAX_OVERHEAD);
            int r = srtp_protect_rtp(&s, buf, n, (int)sizeof buf);
            if (r < 0) { printf("ERR\n"); return 2; }
            puthex(buf, r);
        }
        return 0;
    }
    if (!strcmp(argv[1], "rtcp")) {
        for (int i = 4; i < argc; i++) {
            uint8_t buf[2048];
            int n = hex2bin(argv[i], buf, (int)sizeof buf - SRTP_MAX_OVERHEAD);
            int r = srtp_protect_rtcp(&s, buf, n, (int)sizeof buf);
            if (r < 0) { printf("ERR\n"); return 2; }
            puthex(buf, r);
        }
        return 0;
    }
    if (!strcmp(argv[1], "unrtcp")) {
        for (int i = 4; i < argc; i++) {
            uint8_t buf[2048];
            int n = hex2bin(argv[i], buf, (int)sizeof buf);
            int r = srtp_unprotect_rtcp(&s, buf, n);
            if (r < 0) { printf("REJECT\n"); continue; }
            puthex(buf, r);
        }
        return 0;
    }
    return 1;
}
