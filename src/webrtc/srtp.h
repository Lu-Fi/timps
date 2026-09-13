/* srtp.h - SRTP/SRTCP (RFC 3711) for the ONE profile WebRTC needs here:
 * SRTP_AES128_CM_HMAC_SHA1_80 (RFC 5764 4.1.2). No AES-GCM, no MKI, no
 * key derivation rate, no inbound SRTP (the browser is recvonly, so the only
 * thing it ever sends us is RTCP).
 *
 * Self-contained on purpose: AES-128 lives here rather than coming from the
 * mbedTLS this build already links, so `make test-srtp` can compile this exact
 * file for the host and check it against an independent implementation without
 * needing mbedTLS headers on the build machine. The crypto that actually
 * protects the media is therefore the crypto the test vectors verify.
 * Only compiled with USE_WEBRTC. */
#ifndef MS_SRTP_H
#define MS_SRTP_H
#ifdef USE_WEBRTC

#include <stdint.h>

/* RFC 5764 4.2: the DTLS exporter output for this profile is
 * 2*(16-byte key + 14-byte salt). */
#define SRTP_KEYING_LEN 60
#define SRTP_TAG_LEN    10
/* worst-case growth of one packet: SRTCP adds the E|index word too */
#define SRTP_MAX_OVERHEAD (4 + SRTP_TAG_LEN)

typedef struct {
    uint8_t rk[176];        /* AES-128 encryption round keys */
    uint8_t salt[14];
    uint8_t auth[20];
} srtp_keys;

typedef struct {
    srtp_keys rtp, rtcp;
} srtp_dir;

/* Outbound streams sharing this context: video + audio of one BUNDLE. */
#ifndef SRTP_MAX_STREAMS
#define SRTP_MAX_STREAMS 2
#endif

/* RFC 3711 3.2.1: the rollover counter is part of the cryptographic context,
 * and there is one context PER SSRC. Two bundled streams over one DTLS-SRTP
 * association therefore need one of these each - sharing a single roc/last_seq
 * across SSRCs makes every interleave look like a sequence wrap, inflates the
 * ROC without bound and produces an IV the receiver cannot reconstruct. */
typedef struct {
    uint32_t ssrc;
    uint32_t roc;
    uint16_t last_seq;
    int      have_seq;
    int      used;
} srtp_stream;

typedef struct {
    srtp_dir    out, in;
    srtp_stream out_rtp[SRTP_MAX_STREAMS];
    /* One outbound SRTCP index for the whole association rather than one per
     * SSRC: the IV already mixes the sender SSRC in, so a shared, strictly
     * increasing index still gives every (SSRC, index) pair - and therefore
     * every keystream - exactly once. Per-SSRC replay windows on the receiver
     * only require the index to increase, which it does. */
    uint32_t    rtcp_index;     /* outbound 31-bit SRTCP index */
    uint32_t    in_rtcp_index;  /* highest accepted inbound SRTCP index */
    int         ready;
} srtp_session;

/* km/km_len: the DTLS-SRTP keying material (SRTP_KEYING_LEN bytes).
 * we_are_server picks which half we protect with - the camera answers
 * a=setup:passive, so it is the DTLS server and writes with the server key.
 * Returns 0 on success. */
int srtp_init(srtp_session *s, const uint8_t *km, int km_len, int we_are_server);

/* All three work IN PLACE on a complete packet in `p` and need `cap` bytes of
 * room for the growth. Return the new length, or -1 (and leave the buffer
 * unusable) on a malformed packet, a short buffer or a failed tag check.
 * srtp_protect_rtp() also returns -1 when the packet's SSRC is one more than
 * SRTP_MAX_STREAMS distinct ones - it refuses rather than share another
 * stream's rollover counter. */
int srtp_protect_rtp  (srtp_session *s, uint8_t *p, int len, int cap);
int srtp_protect_rtcp (srtp_session *s, uint8_t *p, int len, int cap);
/* Verifies the tag FIRST, then decrypts; returns the plaintext RTCP length. */
int srtp_unprotect_rtcp(srtp_session *s, uint8_t *p, int len);

/* RFC 3711 4.3.1 key derivation (kdr = 0), exposed for scripts/test_srtp.c. */
void srtp_kdf(const uint8_t master_key[16], const uint8_t master_salt[14],
              uint8_t label, uint8_t *out, int outlen);

#endif /* USE_WEBRTC */
#endif
