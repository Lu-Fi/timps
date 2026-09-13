/* stun.h - the sliver of RFC 5389/8445 an ICE-lite responder needs.
 *
 * timps never gathers candidates, never sends a Binding Request and never
 * runs connectivity checks: the browser does all of that. All that is needed
 * here is to recognise the browser's checks, prove we hold the ice-pwd we put
 * in the SDP answer, and answer them. Only compiled with USE_WEBRTC.
 */
#ifndef MS_STUN_H
#define MS_STUN_H
#ifdef USE_WEBRTC

#include <stdint.h>
#include <netinet/in.h>

#define STUN_MAX_MSG 1024

typedef struct {
    uint8_t txid[12];
    char    username[192];   /* USERNAME value: "<our-ufrag>:<their-ufrag>" */
    int     use_candidate;   /* nomination flag (RFC 8445 7.3.1.5) */
} stun_req;

/* Cheap classifier for the RFC 7983 demux: first two bits zero and the magic
 * cookie present. Says nothing about authenticity. */
int stun_is_stun(const uint8_t *p, int len);

/* Parse a Binding Request and verify BOTH its FINGERPRINT and its
 * MESSAGE-INTEGRITY against `pwd` (our ice-pwd, the short-term credential).
 * Returns 1 only for a request that carried a valid integrity attribute -
 * everything else is 0, which is what lets the DTLS layer below treat a
 * latched peer address as already authenticated. */
int stun_parse_request(const uint8_t *p, int len, const char *pwd,
                       stun_req *out);

/* Binding Success Response with XOR-MAPPED-ADDRESS, MESSAGE-INTEGRITY and
 * FINGERPRINT. Returns the length written, or -1. */
int stun_build_response(uint8_t *out, int cap, const uint8_t txid[12],
                        const struct sockaddr_in *peer, const char *pwd);

#endif /* USE_WEBRTC */
#endif
