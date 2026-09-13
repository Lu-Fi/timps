/* Host harness for src/webrtc/stun.c, driven by scripts/test_stun.py (see
 * `make test-stun`). It only marshals hex on stdin/stdout - the actual
 * checking is done in Python against hmac/hashlib/zlib, so the STUN
 * MESSAGE-INTEGRITY and FINGERPRINT are verified against an implementation
 * that shares no code with ours. Not part of the daemon. */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <arpa/inet.h>
#include "stun.h"

static int hex2bin(const char *h, uint8_t *o, int cap)
{
    int n = 0;
    while (h[0] && h[1] && n < cap) {
        unsigned v; sscanf(h, "%2x", &v); o[n++] = (uint8_t)v; h += 2;
    }
    return n;
}

int main(int argc, char **argv)
{
    const char *pwd = "VOkJxbRl1RmTxUk/WvJxBt";
    if (!strcmp(argv[1], "resp")) {
        uint8_t txid[12];
        hex2bin(argv[2], txid, 12);
        struct sockaddr_in pa; memset(&pa, 0, sizeof pa);
        pa.sin_family = AF_INET;
        pa.sin_port = htons((uint16_t)atoi(argv[4]));
        inet_pton(AF_INET, argv[3], &pa.sin_addr);
        uint8_t out[512];
        int n = stun_build_response(out, sizeof out, txid, &pa, pwd);
        if (n < 0) { fprintf(stderr, "build failed\n"); return 1; }
        for (int i = 0; i < n; i++) printf("%02x", out[i]);
        printf("\n");
        return 0;
    }
    if (!strcmp(argv[1], "parse")) {
        uint8_t msg[1500];
        int n = hex2bin(argv[2], msg, sizeof msg);
        stun_req rq;
        int ok = stun_parse_request(msg, n, pwd, &rq);
        printf("%d %s %d\n", ok, ok ? rq.username : "-", ok ? rq.use_candidate : 0);
        return ok ? 0 : 2;
    }
    return 1;
}
