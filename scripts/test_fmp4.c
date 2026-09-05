/* test_fmp4.c - host-only unit test for the fMP4 gather-write mux path.
 *
 * fmp4_video_fragment_iov() must produce EXACTLY the byte stream
 * fmp4_video_fragment() produces - it only changes how those bytes are handed
 * to the socket (moof + mdat header in a small buffer, the AU itself sent
 * straight out of the packet). This is an ISO BMFF wire format, so the check
 * that matters is byte-for-byte equality against the mux that is already in
 * the field, not "does a player accept it".
 *
 * Both muxers are also stateful (mfhd sequence number, per-track tfdt
 * accumulators, the shared A/V zero point), so every case runs a SEQUENCE of
 * access units through two independently initialised muxers and compares the
 * concatenated output plus the resulting mux state - a divergence that only
 * shows up on the second fragment would otherwise pass.
 *
 * Build/run: make test-fmp4
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mp4/fmp4.h"
#include "config.h"

static int failures = 0;
static int checks   = 0;

/* ---------- synthetic Annex-B access units ---------------------------------
 * Real NAL payloads are irrelevant here: the mux only looks at the first byte
 * (NAL type, to decide what to skip) and the length. Bodies are filled with a
 * per-NAL pattern so a misplaced or truncated copy shows as a mismatch rather
 * than as coincidentally-equal zeros. */

typedef struct { uint8_t *buf; size_t len; } au_t;

static void au_add_nal(au_t *a, uint8_t hdr0, size_t body, int sc4)
{
    size_t sc = sc4 ? 4 : 3;
    a->buf = realloc(a->buf, a->len + sc + 1 + body);
    uint8_t *p = a->buf + a->len;
    if (sc4) { *p++ = 0; }
    *p++ = 0; *p++ = 0; *p++ = 1;
    *p++ = hdr0;
    for (size_t i = 0; i < body; i++) *p++ = (uint8_t)(0xA0 ^ (hdr0 + i));
    a->len += sc + 1 + body;
}

static void au_free(au_t *a){ free(a->buf); a->buf = NULL; a->len = 0; }

/* ---------- the two paths -------------------------------------------------- */

/* reference: the contiguous mux, exactly as record.c and the TLS path use it */
static int mux_contiguous(fmp4_mux *m, const au_t *a, int key, int64_t pts,
                          ms_buf *acc)
{
    ms_buf f;
    if (ms_buf_init(&f, 4096)) return -1;
    int rc = fmp4_video_fragment(m, a->buf, a->len, key, pts, &f);
    if (rc == 0 && f.len) ms_buf_put(acc, f.data, f.len);
    ms_buf_free(&f);
    return rc;
}

/* under test: the gather-write mux, flattened the way the kernel would
 * flatten the iovec array into the TCP stream */
static int mux_iov(fmp4_mux *m, const au_t *a, int key, int64_t pts, ms_buf *acc)
{
    ms_buf head;
    if (ms_buf_init(&head, 4096)) return -1;
    fmp4_frag_iov fi;
    int rc = fmp4_video_fragment_iov(m, a->buf, a->len, key, pts, &head, &fi);
    if (rc == 0) {
        if (fi.niov > FMP4_IOV_MAX) { printf("  niov %d > FMP4_IOV_MAX\n", fi.niov); rc = -1; }
        for (int i = 0; i < fi.niov; i++)
            ms_buf_put(acc, fi.iov[i].iov_base, fi.iov[i].iov_len);
    }
    ms_buf_free(&head);
    return rc;
}

/* ---------- box walker (independent of the mux) ---------------------------
 * Byte-identity proves the new path matches the old one; it cannot catch a
 * bug the OLD path already had. Walking the top-level boxes checks the
 * fragment is self-describing: sizes must chain exactly to the end of the
 * buffer, and mdat must be 8 + the trun sample size. */
static uint32_t rd32(const uint8_t *p)
{ return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|p[3]; }

static int walk_boxes(const uint8_t *b, size_t n, const char *what)
{
    size_t off = 0; int nmoof = 0, nmdat = 0;
    uint32_t last_trun_size = 0;
    while (off < n) {
        if (n - off < 8) { printf("  [%s] trailing %zu bytes, not a box\n", what, n-off); return -1; }
        uint32_t sz = rd32(b + off);
        char ty[5] = {0}; memcpy(ty, b + off + 4, 4);
        if (sz < 8 || (size_t)sz > n - off) {
            printf("  [%s] box '%s' size %u overruns (%zu left)\n", what, ty, sz, n-off);
            return -1;
        }
        if (!strcmp(ty, "moof")) {
            nmoof++;
            /* trun's sample_size is the last of the three 32-bit fields the
             * mux writes (data-offset, [first-flags,] duration, size) and
             * sits 4 bytes before the end of the trun box */
            const uint8_t *t = NULL;
            for (size_t i = off + 8; i + 8 <= off + sz; i++)
                if (!memcmp(b + i + 4, "trun", 4)) { t = b + i; break; }
            if (!t) { printf("  [%s] moof with no trun\n", what); return -1; }
            last_trun_size = rd32(t + rd32(t) - 4);
        } else if (!strcmp(ty, "mdat")) {
            nmdat++;
            if (sz != 8 + last_trun_size) {
                printf("  [%s] mdat size %u != 8 + trun sample size %u\n",
                       what, sz, last_trun_size);
                return -1;
            }
        }
        off += sz;
    }
    if (nmoof != nmdat) { printf("  [%s] %d moof vs %d mdat\n", what, nmoof, nmdat); return -1; }
    return 0;
}

/* ---------- the comparison ------------------------------------------------- */

static void hexdiff(const ms_buf *a, const ms_buf *b)
{
    size_t n = a->len < b->len ? a->len : b->len, i;
    for (i = 0; i < n && a->data[i] == b->data[i]; i++) {}
    printf("  lengths %zu vs %zu, first difference at byte %zu\n", a->len, b->len, i);
    size_t s = i > 8 ? i - 8 : 0;
    printf("  ref :"); for (size_t k = s; k < s+24 && k < a->len; k++) printf(" %02x", a->data[k]);
    printf("\n  iov :"); for (size_t k = s; k < s+24 && k < b->len; k++) printf(" %02x", b->data[k]);
    printf("\n");
}

/* Run the same AU sequence through both muxers and compare bytes + state.
 * `aus` is a list of {AU, keyframe, pts}; NULL-terminated by n. */
typedef struct { const au_t *au; int key; int64_t pts; } step_t;

static void run_case(const char *name, int vcodec, int fps,
                     const step_t *steps, int n)
{
    checks++;
    fmp4_mux ref, iov;
    fmp4_init(&ref); fmp4_init(&iov);
    ref.vcodec = iov.vcodec = vcodec;
    ref.has_video = iov.has_video = 1;
    ref.fps = iov.fps = fps;
    ref.width = iov.width = 1920; ref.height = iov.height = 1080;

    ms_buf ba, bb;
    ms_buf_init(&ba, 4096); ms_buf_init(&bb, 4096);
    int bad = 0;
    for (int i = 0; i < n; i++) {
        int ra = mux_contiguous(&ref, steps[i].au, steps[i].key, steps[i].pts, &ba);
        int rb = mux_iov(&iov, steps[i].au, steps[i].key, steps[i].pts, &bb);
        if (ra != rb) { printf("FAIL %s: step %d return %d vs %d\n", name, i, ra, rb); bad = 1; break; }
    }
    if (!bad && (ba.len != bb.len || (ba.len && memcmp(ba.data, bb.data, ba.len)))) {
        printf("FAIL %s: byte streams differ\n", name);
        hexdiff(&ba, &bb);
        bad = 1;
    }
    /* the mux is stateful: identical bytes from divergent state would still
     * break the NEXT fragment */
    if (!bad && (ref.seq != iov.seq || ref.v_dts != iov.v_dts ||
                 ref.v_last_pts_us != iov.v_last_pts_us ||
                 ref.base_pts_us != iov.base_pts_us)) {
        printf("FAIL %s: mux state diverged (seq %u/%u dts %llu/%llu)\n", name,
               ref.seq, iov.seq, (unsigned long long)ref.v_dts,
               (unsigned long long)iov.v_dts);
        bad = 1;
    }
    if (!bad && ba.len && walk_boxes(ba.data, ba.len, name) != 0) {
        printf("FAIL %s: box structure invalid\n", name);
        bad = 1;
    }
    if (!bad) printf("ok   %s (%zu bytes, %u fragments)\n", name, ba.len, ref.seq);
    else failures++;
    ms_buf_free(&ba); ms_buf_free(&bb);
}

int main(void)
{
    /* --- H.264 AU shapes ---------------------------------------------- */
    au_t idr = {0}, p1 = {0}, ps_only = {0}, sc3 = {0}, big = {0};
    au_t exact = {0}, over = {0}, tiny = {0};

    /* keyframe: AUD + SPS + PPS + SEI + IDR slice - the first three are
     * dropped by the mux (they live in the moov's avcC) */
    au_add_nal(&idr, 0x09, 2, 1);      /* AUD  - skipped */
    au_add_nal(&idr, 0x67, 20, 1);     /* SPS  - skipped */
    au_add_nal(&idr, 0x68, 6, 1);      /* PPS  - skipped */
    au_add_nal(&idr, 0x06, 30, 1);     /* SEI  - kept */
    au_add_nal(&idr, 0x65, 4000, 1);   /* IDR  - kept */

    au_add_nal(&p1, 0x41, 900, 1);     /* plain P frame, one NAL */

    au_add_nal(&ps_only, 0x67, 20, 1); /* parameter sets only -> emit nothing */
    au_add_nal(&ps_only, 0x68, 6, 1);

    au_add_nal(&sc3, 0x06, 5, 0);      /* 3-byte start codes */
    au_add_nal(&sc3, 0x41, 700, 0);

    au_add_nal(&big, 0x65, 300000, 1); /* IDR-sized single slice */

    au_add_nal(&tiny, 0x41, 1, 1);     /* 1-byte body, smallest kept NAL */

    for (int i = 0; i < FMP4_NAL_IDX; i++)          /* exactly at the index cap */
        au_add_nal(&exact, 0x41, 40 + i, 1);
    for (int i = 0; i < FMP4_NAL_IDX + 8; i++)      /* past it -> fallback path */
        au_add_nal(&over, 0x41, 40 + i, 1);

    /* a realistic GOP: IDR then P frames at 25 fps, with capture jitter */
    step_t gop[13];
    gop[0] = (step_t){ &idr, 1, 1000000 };
    for (int i = 1; i < 13; i++)
        gop[i] = (step_t){ &p1, 0, 1000000 + (int64_t)i*40000 + (i%3)*900 };

    run_case("h264 GOP (IDR + 12 P, jittered pts)", MS_VC_H264, 25, gop, 13);

    step_t s_one[1]  = { { &idr, 1, 0 } };
    run_case("single IDR, unknown pts", MS_VC_H264, 25, s_one, 1);

    step_t s_ps[3]   = { { &idr, 1, 1000000 }, { &ps_only, 0, 1040000 }, { &p1, 0, 1080000 } };
    run_case("parameter-set-only AU mid-stream", MS_VC_H264, 25, s_ps, 3);

    step_t s_sc3[3]  = { { &sc3, 1, 1000000 }, { &sc3, 0, 1040000 }, { &p1, 0, 1080000 } };
    run_case("3-byte start codes", MS_VC_H264, 25, s_sc3, 3);

    step_t s_big[2]  = { { &big, 1, 1000000 }, { &p1, 0, 1040000 } };
    run_case("300 KB IDR", MS_VC_H264, 25, s_big, 2);

    step_t s_tiny[2] = { { &tiny, 1, 1000000 }, { &tiny, 0, 1040000 } };
    run_case("1-byte NAL bodies", MS_VC_H264, 25, s_tiny, 2);

    step_t s_ex[2]   = { { &exact, 1, 1000000 }, { &p1, 0, 1040000 } };
    run_case("exactly FMP4_NAL_IDX NALs", MS_VC_H264, 25, s_ex, 2);

    /* the >FMP4_NAL_IDX fallback, and - importantly - a normal AU AFTER it,
     * to prove the fallback leaves the timeline exactly where the reference
     * mux leaves it */
    step_t s_ov[3]   = { { &over, 1, 1000000 }, { &p1, 0, 1040000 }, { &over, 0, 1080000 } };
    run_case("NAL-index overflow + resume", MS_VC_H264, 25, s_ov, 3);

    /* a long transport stall: pts jumps far enough to trip the M6 re-anchor */
    step_t s_stall[3] = { { &idr, 1, 1000000 }, { &p1, 0, 1040000 }, { &idr, 1, 41040000 } };
    run_case("24s pts gap (M6 re-anchor)", MS_VC_H264, 25, s_stall, 3);

    /* fps=0 -> the nominal 3000 duration branch */
    run_case("fps unset (nominal duration)", MS_VC_H264, 0, gop, 13);

    /* --- H.265: a different skip set (VPS/SPS/PPS/AUD = 32/33/34/35) ---- */
    au_t h265_idr = {0}, h265_p = {0};
    au_add_nal(&h265_idr, 32<<1, 12, 1);   /* VPS - skipped */
    au_add_nal(&h265_idr, 33<<1, 20, 1);   /* SPS - skipped */
    au_add_nal(&h265_idr, 34<<1, 8,  1);   /* PPS - skipped */
    au_add_nal(&h265_idr, 35<<1, 2,  1);   /* AUD - skipped */
    au_add_nal(&h265_idr, 39<<1, 25, 1);   /* SEI - kept */
    au_add_nal(&h265_idr, 19<<1, 5000, 1); /* IDR_W_RADL - kept */
    au_add_nal(&h265_p,    1<<1, 800, 1);  /* TRAIL_R */

    step_t s_h265[5] = { { &h265_idr, 1, 1000000 }, { &h265_p, 0, 1040000 },
                         { &h265_p, 0, 1080000 },   { &h265_p, 0, 1120000 },
                         { &h265_idr, 1, 1160000 } };
    run_case("h265 GOP", MS_VC_H265, 25, s_h265, 5);

    au_free(&idr); au_free(&p1); au_free(&ps_only); au_free(&sc3);
    au_free(&big); au_free(&exact); au_free(&over); au_free(&tiny);
    au_free(&h265_idr); au_free(&h265_p);

    printf("\n%d/%d cases passed\n", checks - failures, checks);
    return failures ? 1 : 0;
}
