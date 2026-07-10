#include "nal.h"

void nal_iter_init(nal_iter *it, const uint8_t *au, size_t len)
{
    it->au = au; it->au_len = len; it->pos = 0;
}

/* find next start code (00 00 01 or 00 00 00 01) at or after p, return index
 * of the byte following the start code, and set *sc_len; -1 if none */
static long find_start(const uint8_t *b, size_t n, size_t from, int *sc_len)
{
    for (size_t i=from; i+3<=n; i++){
        if (b[i]==0 && b[i+1]==0){
            if (b[i+2]==1){ *sc_len=3; return (long)i; }
            if (i+4<=n && b[i+2]==0 && b[i+3]==1){ *sc_len=4; return (long)i; }
        }
    }
    return -1;
}

int nal_iter_next(nal_iter *it, nal_unit *out)
{
    if (it->pos >= it->au_len) return 0;
    int sc;
    long s = find_start(it->au, it->au_len, it->pos, &sc);
    if (s < 0) return 0;
    size_t nal_start = (size_t)s + sc;
    int sc2;
    long e = find_start(it->au, it->au_len, nal_start, &sc2);
    size_t nal_end = (e < 0) ? it->au_len : (size_t)e;
    out->data = it->au + nal_start;
    out->len  = nal_end - nal_start;
    it->pos   = nal_end;
    return out->len > 0;
}
