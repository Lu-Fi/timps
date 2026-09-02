#include <string.h>

#include "nal.h"

void nal_iter_init(nal_iter *it, const uint8_t *au, size_t len)
{
    it->au = au; it->au_len = len; it->pos = 0;
}

/* find next start code (00 00 01 or 00 00 00 01) at or after p, return index
 * of the byte following the start code, and set *sc_len; -1 if none.
 *
 * Every start code begins with a 0x00, so memchr() does the skipping over the
 * (long, entropy-coded, essentially never zero) NAL payload runs instead of a
 * byte-at-a-time loop here. This runs once per NAL per consumer - the RTP
 * packetizer, the fMP4 muxer and the recorder each iterate every AU - so the
 * inner loop is worth handing to libc. Candidates still have to be confirmed
 * byte by byte: a lone 0x00 is not a start code, and a 00 00 00 00 01 run must
 * report the start code at the LAST of the leading zeros, not the first. */
static long find_start(const uint8_t *b, size_t n, size_t from, int *sc_len)
{
    size_t i = from;
    while (i+3<=n){                    /* i<=n-3, so b[i+1] and b[i+2] are in range */
        /* n-2-i = the span [i, n-3]: a 0x00 past n-3 cannot begin a start code */
        const uint8_t *z = memchr(b+i,0,n-2-i);
        if (!z) return -1;
        i = (size_t)(z-b);
        if (b[i+1]==0){
            if (b[i+2]==1){ *sc_len=3; return (long)i; }
            if (b[i+2]==0 && i+4<=n && b[i+3]==1){ *sc_len=4; return (long)i; }
            i++;
        } else i+=2;                   /* b[i+1] nonzero: nothing can start at i or i+1 */
    }
    return -1;
}

int nal_iter_next(nal_iter *it, nal_unit *out)
{
    /* a zero-length NAL (two start codes back-to-back) used to make this
     * return 0, which the caller (annexb_to_sample's while loop) reads as
     * "end of access unit" - silently dropping every NAL after the empty
     * one instead of just skipping it. Keep scanning past empty NALs so the
     * rest of the AU still gets emitted. */
    while (it->pos < it->au_len) {
        int sc;
        long s = find_start(it->au, it->au_len, it->pos, &sc);
        if (s < 0) { it->pos = it->au_len; return 0; }
        size_t nal_start = (size_t)s + sc;
        int sc2;
        long e = find_start(it->au, it->au_len, nal_start, &sc2);
        size_t nal_end = (e < 0) ? it->au_len : (size_t)e;
        it->pos = nal_end;
        if (nal_end == nal_start) continue;   /* empty NAL: skip, keep scanning */
        out->data = it->au + nal_start;
        out->len  = nal_end - nal_start;
        return 1;
    }
    return 0;
}
