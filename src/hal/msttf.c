#include "msttf.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stddef.h>   /* ptrdiff_t, used for bounds checks below */

/* ---------- big-endian readers ---------- */
static uint16_t u16(const uint8_t *p){ return (uint16_t)((p[0]<<8)|p[1]); }
static int16_t  s16(const uint8_t *p){ return (int16_t)u16(p); }
static uint32_t u32(const uint8_t *p){ return ((uint32_t)p[0]<<24)|(p[1]<<16)|(p[2]<<8)|p[3]; }

int msttf_load(msttf_font *f, const char *path)
{
    memset(f,0,sizeof *f);
    FILE *fp=fopen(path,"rb"); if(!fp) return -1;
    fseek(fp,0,SEEK_END); long n=ftell(fp); fseek(fp,0,SEEK_SET);
    /* smaller than any usable sfnt header (version+numTables+3 shorts) -
     * also rejects ftell() returning -1/0 on a weird fopen */
    if (n < 12){ fclose(fp); return -1; }
    f->data=(uint8_t*)malloc((size_t)n); if(!f->data){fclose(fp);return -1;}
    if(fread(f->data,1,(size_t)n,fp)!=(size_t)n){fclose(fp);free(f->data);f->data=NULL;return -1;}
    fclose(fp); f->size=n;

    const uint8_t *d=f->data;
    uint32_t ver=u32(d);
    if (ver==0x4F54544F/*OTTO*/){ free(f->data); f->data=NULL; return -2; } /* CFF: not supported by this glyf rasterizer */
    if (ver!=0x00010000 && ver!=0x74727565 /*true*/){ free(f->data); f->data=NULL; return -2; }
    int numTables=u16(d+4);
    /* the table directory itself (12 + 16 bytes/entry) must fit before any
     * entry is read - a truncated/corrupt font (bad flash, wrong file
     * configured) used to read past the buffer here and crash the daemon
     * instead of failing cleanly. */
    if (numTables<0 || 12+16*(long)numTables > n){ free(f->data); f->data=NULL; return -3; }
    uint32_t cmap=0,glyf=0,loca=0,head=0,hhea=0,hmtx=0,maxp=0;
    for (int i=0;i<numTables;i++){
        const uint8_t *r=d+12+16*i;
        uint32_t off=u32(r+8);
        if(!memcmp(r,"cmap",4))cmap=off; else if(!memcmp(r,"glyf",4))glyf=off;
        else if(!memcmp(r,"loca",4))loca=off; else if(!memcmp(r,"head",4))head=off;
        else if(!memcmp(r,"hhea",4))hhea=off; else if(!memcmp(r,"hmtx",4))hmtx=off;
        else if(!memcmp(r,"maxp",4))maxp=off;
    }
    /* every table offset the fixed field reads below dereference must
     * itself be in-bounds, with room for the specific field: head needs 54
     * bytes (loca_fmt at +50), maxp needs 6 (num_glyphs at +4), hhea needs
     * 36 (num_hmetrics at +34); cmap/glyf/loca just need to exist inside
     * the file at all (their own parsers bounds-check further reads). */
    if (!cmap||!glyf||!loca||!head||!maxp ||
        (uint64_t)cmap>=(uint64_t)f->size || (uint64_t)glyf>=(uint64_t)f->size ||
        (uint64_t)loca>=(uint64_t)f->size ||
        (uint64_t)head+54 > (uint64_t)f->size ||
        (uint64_t)maxp+6  > (uint64_t)f->size ||
        (hhea && (uint64_t)hhea+36 > (uint64_t)f->size)) {
        free(f->data); f->data=NULL; return -3;
    }
    f->off_cmap=cmap; f->off_glyf=glyf; f->off_loca=loca; f->off_head=head;
    f->off_hhea=hhea; f->off_hmtx=hmtx; f->off_maxp=maxp;
    f->units_per_em=u16(d+head+18);
    f->loca_fmt=s16(d+head+50);
    f->num_glyphs=u16(d+maxp+4);
    /* 0 is malformed (the spec requires at least one metric) and would make
     * both the hmtx room-check below vacuous and advance()'s last-metric
     * fallback index -1, reading 4 bytes before the table. Floor it to the
     * same 1 the no-hhea case falls back to. */
    f->num_hmetrics=hhea?u16(d+hhea+34):1;
    if (f->num_hmetrics<1) f->num_hmetrics=1;
    if (f->units_per_em==0){ free(f->data); f->data=NULL; return -3; } /* msttf_render divides by this */
    /* glyf_offset()/advance() trust loca/hmtx to have room for every
     * gid/hmetric they're asked about without their own bounds check -
     * verify that room actually exists for the declared num_glyphs /
     * num_hmetrics now, once, instead of at every render call. */
    uint64_t loca_need = (f->loca_fmt==0)
        ? 2ULL*((uint64_t)f->num_glyphs+1) : 4ULL*((uint64_t)f->num_glyphs+1);
    if ((uint64_t)loca + loca_need > (uint64_t)f->size){ free(f->data); f->data=NULL; return -3; }
    if (hmtx && (uint64_t)hmtx + 4ULL*(uint64_t)f->num_hmetrics > (uint64_t)f->size){
        free(f->data); f->data=NULL; return -3;
    }
    return 0;
}

void msttf_free(msttf_font *f){ free(f->data); f->data=NULL; }

/* ---------- cmap format 4 lookup ---------- */
static int glyph_index(msttf_font *f, int cp)
{
    const uint8_t *d=f->data;
    uint64_t fsize=(uint64_t)f->size;
    if ((uint64_t)f->off_cmap+4 > fsize) return 0;
    const uint8_t *c=d+f->off_cmap;
    int nt=u16(c+2); uint32_t sub=0;
    if (nt<0 || (uint64_t)f->off_cmap+4+8ULL*(uint32_t)nt > fsize) return 0;
    for (int i=0;i<nt;i++){
        const uint8_t *r=c+4+8*i;
        int plat=u16(r), enc=u16(r+2);
        uint32_t off=u32(r+4);
        if ((plat==3&&(enc==1||enc==0))||plat==0){ sub=f->off_cmap+off; if(plat==3&&enc==1)break; }
    }
    /* sub is itself a font-data-controlled offset (table directory entry),
     * not validated by msttf_load(); every subsequent read must be bounds-
     * checked against it - a corrupt/adversarial cmap used to read
     * arbitrarily far past the buffer (crash) with no checks at all here. */
    if(!sub || (uint64_t)sub+14 > fsize) return 0;
    const uint8_t *s=d+sub;
    if (u16(s)!=4) return 0;
    int segX2=u16(s+6);
    if (segX2<=0 || segX2%2) return 0;
    int seg=segX2/2;
    /* endCode[seg] + pad(2) + startCode[seg] + idDelta[seg] + idRangeOffset[seg] */
    uint64_t need = 14ULL + 4ULL*(uint32_t)segX2 + 2ULL;
    if ((uint64_t)sub + need > fsize) return 0;
    const uint8_t *endC=s+14, *startC=endC+segX2+2, *idD=startC+segX2, *idR=idD+segX2;
    for (int i=0;i<seg;i++){
        int end=u16(endC+2*i);
        if (cp<=end){
            int st=u16(startC+2*i);
            if (cp<st) return 0;
            int delta=s16(idD+2*i), ro=u16(idR+2*i);
            if (ro==0) return (cp+delta)&0xFFFF;
            /* compute the target offset numerically first (rather than
             * forming a pointer that may already be far out of bounds)
             * before bounds-checking and dereferencing it */
            uint64_t idr_off = (uint64_t)(idR-d) + 2ULL*i;
            uint64_t goff = idr_off + (uint32_t)ro + 2ULL*(uint32_t)(cp-st);
            if (goff+2 > fsize) return 0;
            int g=u16(d+goff); return g?((g+delta)&0xFFFF):0;
        }
    }
    return 0;
}

static uint32_t glyf_offset(msttf_font *f, int gid, uint32_t *len)
{
    const uint8_t *l=f->data+f->off_loca;
    uint32_t a,b;
    if (f->loca_fmt==0){ a=2*u16(l+2*gid); b=2*u16(l+2*gid+2); }
    else { a=u32(l+4*gid); b=u32(l+4*gid+4); }
    /* loca entries come straight from font data (msttf_load only checked
     * the loca *table* has room for num_glyphs entries, not that the
     * offsets it contains are sane) - a corrupt/malicious b<a or an
     * a/b pointing past the glyf table used to be handed straight to
     * callers as a valid (offset,len), which then read glyf data OOB.
     * Treat any such entry as "no outline" - parse_glyph()/callers
     * already handle len==0 as empty. */
    if (b<a || (uint64_t)f->off_glyf+b > (uint64_t)f->size){ *len=0; return f->off_glyf; }
    *len=b-a; return f->off_glyf+a;
}

/* ---------- outline extraction ---------- */
typedef struct { float x,y; } pt;
/* flattened polyline (one contour). curve[i]!=0 marks a point emitted by
 * quad(), i.e. the edge ENDING at i is a flattened-bezier chord and not a
 * straight edge of the source outline - the autohinter has to tell those
 * apart (see autohint_glyph()) and length alone no longer can. */
typedef struct { pt *p; uint8_t *curve; int n, cap; } poly;

static void poly_add(poly *pl, float x, float y, int curve){
    if (pl->n>=pl->cap){
        int ncap=pl->cap?pl->cap*2:64;
        pt *np=realloc(pl->p,(size_t)ncap*sizeof(pt));
        /* on OOM, drop the point rather than overwrite pl->p with NULL
         * while pl->cap already reflects the larger (unrealized) capacity -
         * that combination used to cause a NULL-pointer write on the very
         * next poly_add() call */
        if (!np) return;
        pl->p=np;
        uint8_t *nf=realloc(pl->curve,(size_t)ncap);
        /* same rule for the parallel flag array, and cap is raised only once
         * BOTH grew: a half-grown pair would let the writes below run past
         * curve[]. Leaving p[] larger than cap costs memory, nothing else. */
        if (!nf) return;
        pl->curve=nf; pl->cap=ncap;
    }
    pl->p[pl->n].x=x; pl->p[pl->n].y=y; pl->curve[pl->n]=(uint8_t)(curve?1:0);
    pl->n++;
}
/* Chord deviation (device px) accepted when flattening a curve. Well under
 * one supersample cell (1/ss px, i.e. >=0.25px), so the coverage the scanline
 * fill computes off these chords is visually the same as a finer flattening. */
#define QUAD_TOL 0.02f

static void quad(poly *pl, pt a, pt c, pt b){
    /* a/c/b are already in device pixels (parse_glyph applied ox/oy/sx/sy),
     * so size the flattening to the curve itself instead of emitting a fixed
     * count at every font size: for n uniform steps the polyline deviates
     * from the curve by at most |a-2c+b|/(8n^2), which solves for n directly.
     * A 12px sub-stream glyph costs 2 segments per curve where it used to
     * cost 8; a 128px one gets up to 10 where it used to be under-tessellated
     * at 8. */
    float ddx=a.x-2*c.x+b.x, ddy=a.y-2*c.y+b.y;
    int steps=(int)ceilf(sqrtf(sqrtf(ddx*ddx+ddy*ddy)/(8.0f*QUAD_TOL)));
    if (steps<2) steps=2;
    if (steps>16) steps=16;   /* bound the point count; pixel_h is clamped to 512 */
    for (int i=1;i<=steps;i++){
        float t=(float)i/steps, mt=1-t;
        float x=mt*mt*a.x+2*mt*t*c.x+t*t*b.x;
        float y=mt*mt*a.y+2*mt*t*c.y+t*t*b.y;
        poly_add(pl,x,y,1);
    }
}

/* parse a simple/composite glyph, appending flattened contours (in font units,
 * y-up) to polys[]; returns number of contours added. transform tx,ty offset. */
static int parse_glyph(msttf_font *f, int gid, poly **polys, int *npoly, int *cappoly,
                       float ox, float oy, float sx, float sy, int depth);

static int parse_simple(msttf_font *f, const uint8_t *g, uint32_t len,
                        poly **polys, int *npoly, int *cappoly,
                        float ox, float oy, float sx, float sy)
{
    (void)f;
    /* g/len come from glyf_offset(), which is now bounds-checked against
     * f->size, but every field inside the glyph record is still attacker-
     * controlled - previously this function trusted the declared contour
     * count/point count/instruction length completely and read straight
     * off the end of the buffer for any corrupt glyph. pend is the hard
     * upper bound for every read of p from here on. */
    const uint8_t *pend = g + len;
    int *ends=NULL; uint8_t *flags=NULL; int16_t *xs=NULL, *ys=NULL;
    if (len < 10) return 0;
    int nc=s16(g);
    if (nc<0) return 0;
    const uint8_t *p=g+10;
    if (pend - p < (ptrdiff_t)(2*(uint32_t)nc)) goto fail;
    ends = nc ? malloc((size_t)nc*sizeof(int)) : NULL;
    if (nc>0 && !ends) goto fail;
    for (int i=0;i<nc;i++){ ends[i]=u16(p); p+=2; }
    int npts=nc?ends[nc-1]+1:0;
    if (npts<0 || npts>20000) goto fail;
    if (pend-p < 2) goto fail;
    int insLen=u16(p); p+=2;
    if (pend-p < insLen) goto fail;
    p+=insLen;
    flags = npts ? malloc((size_t)npts) : NULL;
    if (npts>0 && !flags) goto fail;
    for (int i=0;i<npts;){
        if (pend-p < 1) goto fail;
        uint8_t fl=*p++; flags[i++]=fl;
        if (fl&8){
            if (pend-p < 1) goto fail;
            int r=*p++; while(r-- && i<npts) flags[i++]=fl;
        }
    }
    xs = npts ? malloc((size_t)npts*sizeof(int16_t)) : NULL;
    ys = npts ? malloc((size_t)npts*sizeof(int16_t)) : NULL;
    if (npts>0 && (!xs || !ys)) goto fail;
    int xacc=0;
    for (int i=0;i<npts;i++){ uint8_t fl=flags[i];
        if (fl&2){ if (pend-p<1) goto fail; int dx=*p++; xacc+=(fl&16)?dx:-dx; }
        else if(!(fl&16)){ if (pend-p<2) goto fail; xacc+=s16(p); p+=2; }
        xs[i]=xacc;
    }
    int yacc=0;
    for (int i=0;i<npts;i++){ uint8_t fl=flags[i];
        if (fl&4){ if (pend-p<1) goto fail; int dy=*p++; yacc+=(fl&32)?dy:-dy; }
        else if(!(fl&32)){ if (pend-p<2) goto fail; yacc+=s16(p); p+=2; }
        ys[i]=yacc;
    }
    /* ends[] is expected non-decreasing with ends[nc-1]==npts-1 (that's
     * how npts was derived above); a corrupt/adversarial font can violate
     * that, which would otherwise let idx run past npts-1 below and read
     * flags[]/xs[]/ys[] out of bounds. Reject rather than guess. */
    for (int i=0;i<nc;i++){
        if (ends[i]<0 || ends[i]>=npts || (i>0 && ends[i]<ends[i-1])) goto fail;
    }
    int start=0;
    for (int ci=0;ci<nc;ci++){
        int end=ends[ci], cnt=end-start+1;
        if (cnt<=0){ start=end+1; continue; }
        if (*npoly>=*cappoly){
            int ncap=*cappoly?*cappoly*2:8;
            poly *np=realloc(*polys,(size_t)ncap*sizeof(poly));
            if (!np) goto fail;
            *polys=np; *cappoly=ncap;
        }
        poly *pl=&(*polys)[*npoly]; memset(pl,0,sizeof *pl);
        /* build on-curve point list, inserting implied midpoints */
        #define PX(i) (ox + sx*xs[i])
        #define PY(i) (oy + sy*ys[i])
        #define ONC(i) (flags[i]&1)
        int first=start;
        /* find a starting on-curve point */
        pt cur; int si=-1;
        for (int k=0;k<cnt;k++){ int idx=start+k; if(ONC(idx)){ si=idx; break; } }
        pt startpt;
        if (si<0){ /* all off-curve: synth midpoint of first two */
            int i0=start, i1=start+1<=end?start+1:start;
            startpt.x=(PX(i0)+PX(i1))/2; startpt.y=(PY(i0)+PY(i1))/2;
            /* synthesized from two off-curve points: a curve point too */
            cur=startpt; poly_add(pl,cur.x,cur.y,1); si=start;
        } else {
            startpt.x=PX(si); startpt.y=PY(si); cur=startpt;
            poly_add(pl,cur.x,cur.y,0);
        }
        for (int k=1;k<=cnt;k++){
            int idx=start+((si-start)+k)%cnt;
            if (ONC(idx)){ pt e={PX(idx),PY(idx)}; poly_add(pl,e.x,e.y,0); cur=e; }
            else {
                pt c={PX(idx),PY(idx)};
                int nidx=start+((si-start)+k+1)%cnt;
                pt e;
                if (ONC(nidx)){ e.x=PX(nidx); e.y=PY(nidx); k++; }
                else { e.x=(c.x+PX(nidx))/2; e.y=(c.y+PY(nidx))/2; }
                quad(pl,cur,c,e); cur=e;
            }
        }
        (void)first;
        (*npoly)++;
        start=end+1;
        #undef PX
        #undef PY
        #undef ONC
    }
    free(ends);free(flags);free(xs);free(ys);
    return nc;
fail:
    free(ends);free(flags);free(xs);free(ys);
    return 0;
}

static int parse_glyph(msttf_font *f, int gid, poly **polys, int *npoly, int *cappoly,
                       float ox, float oy, float sx, float sy, int depth)
{
    if (depth>4 || gid<0 || gid>=f->num_glyphs) return 0;
    uint32_t len, off=glyf_offset(f,gid,&len);
    if (len==0) return 0;
    const uint8_t *g=f->data+off;
    int nc=s16(g);
    if (nc>=0) return parse_simple(f,g,len,polys,npoly,cappoly,ox,oy,sx,sy);
    /* composite - previously read flags/cgid/dx/dy/scale fields with no
     * check against the glyph's declared length at all, so a truncated or
     * corrupt composite glyph (or one whose MORE_COMPONENTS chain never
     * terminates) would walk p arbitrarily far past the glyf buffer. */
    if (len < 10) return 0;
    const uint8_t *pend = g + len;
    const uint8_t *p=g+10;
    while (1){
        if (pend-p < 4) break;
        int flags=u16(p); int cgid=u16(p+2); p+=4;
        float dx,dy;
        if (flags&1){ if (pend-p<4) break; dx=s16(p); dy=s16(p+2); p+=4; }
        else { if (pend-p<2) break; dx=(int8_t)p[0]; dy=(int8_t)p[1]; p+=2; }
        float a=1,b2=0,c2=0,dd=1;
        if (flags&8){ if (pend-p<2) break; a=dd=s16(p)/16384.0f; p+=2; }
        else if (flags&0x40){ if (pend-p<4) break; a=s16(p)/16384.0f; dd=s16(p+2)/16384.0f; p+=4; }
        else if (flags&0x80){ if (pend-p<8) break; a=s16(p)/16384.0f; b2=s16(p+2)/16384.0f; c2=s16(p+4)/16384.0f; dd=s16(p+6)/16384.0f; p+=8; }
        /* only ARGS_ARE_XY_VALUES supported for placement */
        float nox=ox+sx*dx, noy=oy+sy*dy;
        parse_glyph(f,cgid,polys,npoly,cappoly,nox,noy,sx*a,sy*dd,depth+1);
        (void)b2;(void)c2;
        if (!(flags&0x20)) break; /* no MORE_COMPONENTS */
    }
    return 1;
}

static int advance(msttf_font *f, int gid)
{
    if (!f->off_hmtx) return f->units_per_em/2;
    int idx = gid<f->num_hmetrics ? gid : f->num_hmetrics-1;
    return u16(f->data+f->off_hmtx+4*idx);
}

/* ---------- scanline fill with supersampling ---------- */
/* Runtime-configurable AA quality (samples/axis/pixel), default 2. Used to
 * be a hardcoded #define SS 4; benchmarked (host x86, real OSD strings from
 * camera.conf) at ~2x the raster CPU cost of 2 for no visible difference at
 * typical OSD text sizes (12-32px) - see msttf_set_ss()/osd.supersample. */
static int g_ss = 2;

void msttf_set_ss(int ss)
{
    if (ss < 1) ss = 1;
    if (ss > 4) ss = 4;   /* coverage counters are uint8_t (0..ss*ss); ss=4 -> 16, plenty of margin under 255 */
    g_ss = ss;
}

/* ---------- optional autohinting (opt-in, geometric, no bytecode) ----------
 * Compile-time gated on USE_OSD_HINTING (BR2_PACKAGE_TIMPS_OSD_HINTING in the
 * buildroot package): when undefined, none of autohint_glyph()/resolve_snaps()/
 * hint_inside() nor their call site below are compiled in at all (measured
 * ~2.1KB smaller .text on T31/GCC 16.1.0/-Os). msttf_set_hinting() itself is
 * still always defined (see the #else stub past the #endif below) so
 * callers - and the osd.hinting config key's own parsing in config.c, which
 * is unconditional - never need to know
 * whether the feature was compiled in: setting osd.hinting=1 in timps.conf on
 * a build without USE_OSD_HINTING is accepted but is a no-op, the same
 * graceful-degradation pattern ROT_HAS_* uses for an unsupported rotation
 * value.
 * Real TrueType hinting executes the font's embedded per-glyph instruction
 * bytecode (a stack-based VM: SVTCA/MDAP/MIAP/IUP/... operating on point
 * coordinates). That is genuinely correct (it uses the font designer's own
 * intent) but is a real interpreter with a real correctness/security surface
 * - a malformed or merely-unexpected instruction stream must not run
 * unbounded or read/write out of bounds, since this runs on-device with no
 * sandboxing. Deliberately NOT implemented here.
 *
 * Instead: after a glyph's contours are flattened to device-pixel-space
 * polylines (parse_glyph() has already applied ox/oy/sx/sy, so pl->p[].x/y
 * are plain canvas pixel coordinates), snap the endpoints of any long,
 * nearly axis-aligned edge to a common integer pixel column/row. Typical
 * letter stems (the vertical strokes of 'l'/'H'/'i', the horizontal bars of
 * 'e'/'t') are single straight line segments in the source outline, so they
 * are long; the chords quad() emits per flattened bezier are skipped via
 * their poly.curve[] flag (see autohint_glyph()) so round glyphs ('o', 'O')
 * are not chunked up. This does not reproduce the font's authored hints and
 * can't preserve exact stem width the way real hinting would, but it directly
 * targets the symptom this rasterizer actually has: unhinted glyphs landing
 * at inconsistent sub-pixel positions and rendering with uneven stroke
 * widths at small sizes. */
#ifdef USE_OSD_HINTING
static int g_hinting = 0;

void msttf_set_hinting(int enable) { g_hinting = enable ? 1 : 0; }

/* A candidate stem edge, collected pre-snap across the WHOLE glyph:
 * ci is the contour index, (j,k) the endpoint indices in that contour,
 * orig the pre-snap mid coordinate along the snap axis (x for a
 * near-vertical edge, y for a near-horizontal one), lo/hi its extent along
 * the perpendicular axis, dir the traversal direction along the stem axis
 * (+1/-1: the two sides of one stroke are always traversed in opposite
 * directions by a consistently-wound outline), snapped its independently
 * rounded pixel column/row, and fin the final resolved value applied. */
typedef struct { int ci, j, k, dir; float orig, lo, hi, snapped, fin; } stem_edge;

/* Two pre-snap coordinates closer than this (device px) are the same edge
 * line: collinear split segments of one stem side, or the aligned caps of
 * twin stems. This is a float-identity tolerance, not a tuned font metric. */
#define HINT_EPS_MERGED  0.05f
/* Minimum shared perpendicular extent (device px) for two opposing edges to
 * count as bounding the same stroke segment. */
#define HINT_MIN_OVERLAP 0.25f

/* Even-odd point-in-outline test on the pre-snap outline, same crossing
 * rule as the scanline fill in msttf_render() below. ray_x=1 casts the ray
 * in +x (for testing between two near-VERTICAL edges, so the ray crosses
 * them transversally), ray_x=0 casts in +y (for near-horizontal pairs). */
static int hint_inside(const poly *polys, int npoly, float x, float y, int ray_x)
{
    int cnt=0;
    for (int i=0;i<npoly;i++){
        const poly *pl=&polys[i];
        for (int j=0;j<pl->n;j++){
            pt A=pl->p[j], B=pl->p[(j+1)%pl->n];
            if (ray_x){
                if ((A.y<=y&&B.y>y)||(B.y<=y&&A.y>y)){
                    float t=(y-A.y)/(B.y-A.y);
                    if (A.x+t*(B.x-A.x) > x) cnt++;
                }
            } else {
                if ((A.x<=x&&B.x>x)||(B.x<=x&&A.x>x)){
                    float t=(x-A.x)/(B.x-A.x);
                    if (A.y+t*(B.y-A.y) > y) cnt++;
                }
            }
        }
    }
    return cnt&1;
}

/* Snapping each stem-side edge independently - the first cut of this hinter
 * - can collapse a sub-pixel stem to zero width: two edges a hair under 1px
 * apart (UbuntuMono Regular's ~0.98px vertical stroke at the 12px OSD
 * default) can both round to the SAME column, and a zero-width path renders
 * as nothing. The first repair attempt (parity-pairing sorted candidates
 * and pushing "colliding" neighbours apart) was geometrically wrong: sorted
 * adjacency says nothing about whether two edges actually bound the same
 * stroke, so it forced apart edges that were legitimately collinear (twin
 * stem caps, split stem sides) and mis-paired stems whose sides interleave
 * with bar/bowl edges in sort order, leaving real collapses unfixed.
 *
 * This version pairs edges by actual stroke geometry instead. A pair is a
 * genuine stroke iff:
 *   - opposite traversal direction (a consistently-wound contour walks the
 *     two sides of one stroke in opposite directions - available for free
 *     from the point order at collection time),
 *   - overlapping perpendicular extents (they face each other along the
 *     stroke, not merely share a column somewhere else in the glyph),
 *   - ink between them (even-odd midpoint test on the pre-snap outline,
 *     same fill rule as the rasterizer - rejects sub-pixel COUNTERS, whose
 *     bounding edges also satisfy the two conditions above; forcing those
 *     apart is what tore '+' crossbars in the previous attempt's testing).
 * Only such pairs whose independent snaps landed on the same column/row are
 * touched: the lower edge keeps its snap, the upper is pushed out by the
 * pair's own pre-snap width rounded (floored at 1px), so the stroke
 * survives. For width>=1px, floor(x+0.5) != floor(x+w+0.5) always (a
 * half-open interval of length>=1 contains an integer), so thick stems
 * never even enter the collision branch.
 *
 * Edges at near-equal pre-snap positions are the opposite case - already
 * merged - and are never pushed apart: the collision pass skips them
 * (w<=eps is not a collision), and the collinearity pass moves them
 * together if any one of them was pushed by its own opposing partner
 * (keeps split stem sides collinear). Edges the collision pass does not
 * touch keep their plain independent snap bit-for-bit - including a
 * near-equal pair straddling a .5 rounding boundary, which snaps exactly
 * as it did before this repair - so everything that rendered correctly
 * before still renders identically.
 *
 * No font- or size-specific constants: widths/separations are measured per
 * pair at render time; the two epsilons above are float-identity and
 * degenerate-overlap tolerances in device pixel space. */
static void resolve_snaps(stem_edge *e, int n, const poly *polys, int npoly,
                          int vert)
{
    /* pass 1: resolve genuine opposing-pair snap collisions */
    for (int i=0;i<n;i++) for (int l=i+1;l<n;l++){
        if (e[i].dir == e[l].dir) continue;          /* same stroke side */
        float w = fabsf(e[i].orig - e[l].orig);
        if (w <= HINT_EPS_MERGED) continue;          /* already merged */
        if (e[i].snapped != e[l].snapped) continue;  /* no collision */
        float olo = fmaxf(e[i].lo, e[l].lo);
        float ohi = fminf(e[i].hi, e[l].hi);
        if (ohi - olo < HINT_MIN_OVERLAP) continue;  /* not the same stroke */
        float mid = (e[i].orig + e[l].orig)*0.5f, pm = (olo + ohi)*0.5f;
        int ink = vert ? hint_inside(polys, npoly, mid, pm, 1)
                       : hint_inside(polys, npoly, pm, mid, 0);
        if (!ink) continue;                          /* sub-pixel gap/counter */
        int a = (e[i].orig < e[l].orig) ? i : l;
        int b = (a == i) ? l : i;
        float sep = (w < 1.0f) ? 1.0f : floorf(w + 0.5f);
        e[b].fin = e[a].snapped + sep;
        /* note: an edge can't be pushed inconsistently by two different
         * partners - it would need ink on both of its sides, which an
         * outline edge by definition doesn't have */
    }

    /* pass 2: collinearity guard - if a collision push moved one segment of
     * a split edge line, its near-equal siblings follow (otherwise e.g. a
     * '+' crossbar edge split by the vertical stroke tears onto two rows
     * when only one arm's opposing pair collided). Only PUSHED edges
     * propagate; groups nobody pushed keep their independent snaps
     * untouched. i<l order carries a push through the whole group. */
    for (int i=0;i<n;i++) for (int l=i+1;l<n;l++){
        if (fabsf(e[i].orig - e[l].orig) > HINT_EPS_MERGED) continue;
        if (e[i].fin == e[l].fin) continue;
        if      (e[i].fin != e[i].snapped) e[l].fin = e[i].fin;
        else if (e[l].fin != e[l].snapped) e[i].fin = e[l].fin;
    }
}

static void autohint_glyph(poly *polys, int npoly)
{
    /* generous fixed capacity: real glyphs never come close to this many
     * qualifying stem-like edges. If a pathological glyph ever did, the
     * overflow edges below still get their own independent snap applied
     * in-place immediately (old per-edge behavior) rather than being
     * silently dropped or overflowing this array. */
    enum { MAXE = 96 };
    stem_edge vedge[MAXE]; int nv=0;
    stem_edge hedge[MAXE]; int nh=0;

    for (int ci=0; ci<npoly; ci++){
        poly *pl = &polys[ci];
        if (pl->n < 2) continue;
        for (int j=0;j<pl->n;j++){
            int k=(j+1)%pl->n;
            /* never snap a flattened-bezier chord: doing so chunks up round
             * glyphs ('o', 'O'). This used to be inferred from the 2px
             * length gate below, which only held while quad() emitted a
             * fixed 8 segments AND the text stayed small - a chord is
             * ~0.04*pixel_h long at 8 segments, so it already leaked past
             * 2px above ~50px, and quad()'s size-adaptive stepping makes
             * chords longer still. The flag says so exactly, at any size. */
            if (pl->curve[k]) continue;
            float dx=pl->p[k].x-pl->p[j].x, dy=pl->p[k].y-pl->p[j].y;
            float adx=fabsf(dx), ady=fabsf(dy);
            /* length gate: tiny serif nubs, keeping this to genuine
             * stem-height edges */
            if (dx*dx+dy*dy < 4.0f) continue;   /* len < 2px */
            if (adx < 0.2f*ady && ady > 1.5f){
                /* near-vertical stem edge */
                float orig = (pl->p[j].x+pl->p[k].x)*0.5f;
                float snapped = floorf(orig + 0.5f);
                if (nv < MAXE) vedge[nv++] = (stem_edge){ci,j,k, dy>0?1:-1,
                    orig, fminf(pl->p[j].y,pl->p[k].y),
                    fmaxf(pl->p[j].y,pl->p[k].y), snapped, snapped};
                else { pl->p[j].x = snapped; pl->p[k].x = snapped; }
            } else if (ady < 0.2f*adx && adx > 1.5f){
                /* near-horizontal edge (serif/crossbar) */
                float orig = (pl->p[j].y+pl->p[k].y)*0.5f;
                float snapped = floorf(orig + 0.5f);
                if (nh < MAXE) hedge[nh++] = (stem_edge){ci,j,k, dx>0?1:-1,
                    orig, fminf(pl->p[j].x,pl->p[k].x),
                    fmaxf(pl->p[j].x,pl->p[k].x), snapped, snapped};
                else { pl->p[j].y = snapped; pl->p[k].y = snapped; }
            }
        }
    }

    /* resolve BOTH orientations before applying anything, so every
     * hint_inside() ink test above sees the untouched pre-snap outline */
    resolve_snaps(vedge, nv, polys, npoly, 1);
    resolve_snaps(hedge, nh, polys, npoly, 0);

    for (int i=0;i<nv;i++){
        polys[vedge[i].ci].p[vedge[i].j].x = vedge[i].fin;
        polys[vedge[i].ci].p[vedge[i].k].x = vedge[i].fin;
    }
    for (int i=0;i<nh;i++){
        polys[hedge[i].ci].p[hedge[i].j].y = hedge[i].fin;
        polys[hedge[i].ci].p[hedge[i].k].y = hedge[i].fin;
    }
}
#else /* !USE_OSD_HINTING */
/* Feature compiled out: msttf_set_hinting() stays a real (empty) function so
 * imp_osd.c's unconditional msttf_set_hinting(cfg->osd.hinting) call site
 * doesn't need its own #ifdef, and osd.hinting's config.c parsing keeps
 * working unmodified - the value is just discarded, matching how an
 * unsupported ROT_HAS_* rotation value coerces to a no-op rather than a
 * build/config error. */
void msttf_set_hinting(int enable) { (void)enable; }
#endif /* USE_OSD_HINTING */

/* blend 'color' onto img[idx] with additional alpha factor 'a' (0..1) */
static void px_blend(uint32_t *img, size_t idx, uint32_t color, float a)
{
    a *= ((color>>24)&0xFF)/255.0f;
    if (a<=0.0f) return;
    int fr=(color>>16)&0xFF, fgn=(color>>8)&0xFF, fb=color&0xFF;
    uint32_t bgp=img[idx];
    int br=(bgp>>16)&0xFF, bgc=(bgp>>8)&0xFF, bb=bgp&0xFF, ba=(bgp>>24)&0xFF;
    uint32_t rr=(uint32_t)(fr*a+br*(1-a));
    uint32_t gg=(uint32_t)(fgn*a+bgc*(1-a));
    uint32_t bbb=(uint32_t)(fb*a+bb*(1-a));
    uint32_t aa=(uint32_t)(255*a+ba*(1-a));
    img[idx]=(aa<<24)|(rr<<16)|(gg<<8)|bbb;
}

int msttf_render(msttf_font *f, const char *s, int pixel_h,
                 uint32_t fg, uint32_t bg, int outline, uint32_t oc,
                 uint8_t **out, int *w, int *h)
{
    /* snapshot once: keeps one render call internally consistent even if
     * msttf_set_ss() were ever called concurrently (it isn't today - set
     * once at startup from osd.supersample - but this is free either way) */
    const int ss = g_ss;
    /* snapshot, same rationale as ss above: osd.hinting is a File-only config
     * key (see config.c), applied once via msttf_set_hinting() at startup, so
     * this never actually races a concurrent writer today either. Compiled
     * out entirely (along with the call site below) when USE_OSD_HINTING is
     * not defined - see the #ifdef block above. */
#ifdef USE_OSD_HINTING
    const int hinting = g_hinting;
#endif
    /* H4: pixel_h derives from config font_size (live-settable via /control)
     * scaled by the stream height - hard-clamp it HERE too, independent of
     * any caller-side clamp, so the canvas math below can never be pushed
     * toward overflow by a bad config value. */
    if (pixel_h < 8)   pixel_h = 8;
    if (pixel_h > 512) pixel_h = 512;
    float scale = (float)pixel_h / f->units_per_em;
    int ascent = (int)(f->units_per_em*1.0f);   /* use em box */
    (void)ascent;
    if (outline<0) outline=0;
    if (outline>pixel_h/4+1) outline=pixel_h/4+1;   /* keep the stroke sane */
    if (((oc>>24)&0xFF)==0) outline=0;              /* fully transparent = off */
    int pad = pixel_h/4 + 1 + outline;   /* outline enlarges the canvas */
    /* first pass: total advance width */
    int totalAdv=0; const char *q=s;
    for (; *q; q++){ int gid=glyph_index(f,(unsigned char)*q); totalAdv+=advance(f,gid); }
    /* H4: bound the canvas. totalAdv is summed per character with no limit,
     * so a long string at a big font size used to size W past any sane frame
     * - and (size_t)W*H*4 on 32-bit could wrap and under-allocate, after
     * which the (previously int-indexed) fill loops corrupted the heap.
     * Compute in double/uint64_t and clamp both axes; 4096 comfortably
     * covers every frame size this daemon can produce. */
    double Wf = (double)totalAdv*scale + 2.0*pad;
    int W = (Wf < 1.0) ? 1 : (Wf > 4096.0 ? 4096 : (int)Wf);
    int H = pixel_h + 2*pad;
    if (H<1) H=1; if (H>4096) H=4096;
    W = (W + 1) & ~1;   /* IMP_OSD needs an even picture width (avoids row shear) */
    uint64_t npx = (uint64_t)W * (uint64_t)H;
    if (npx == 0 || npx > (uint64_t)4096*4096 ||
        npx*4 > (uint64_t)SIZE_MAX) return -1;    /* keep the guard explicit */
    uint32_t *img=malloc((size_t)(npx*4));
    if(!img) return -1;
    for (size_t i=0;i<(size_t)npx;i++) img[i]=bg;
    /* whole-string coverage plane (0..ss*ss): glyphs rasterize into this and
     * the composite runs ONCE afterwards, so an outline can be drawn under
     * the complete fill (no later glyph stroking over its neighbour's fill) */
    uint8_t *gcov=calloc((size_t)npx,1);
    if(!gcov){ free(img); return -1; }

    /* baseline near bottom (leave descent room) */
    float penx = pad;
    float baseline = H - pad - pixel_h*0.2f;

    for (const char *cptr=s; *cptr; cptr++){
        int cp=(unsigned char)*cptr;
        int gid=glyph_index(f,cp);
        poly *polys=NULL; int npoly=0, cap=0;
        /* y-up font units -> device: x = penx + sx*X ; y = baseline - scale*Y */
        parse_glyph(f,gid,&polys,&npoly,&cap, penx, baseline, scale, -scale, 0);
        /* opt-in geometric autohinting: snap stem-like edges to the pixel
         * grid in device space, before the bbox is measured off these same
         * points (so the dilated outline pass and coverage rasterization
         * below both see the snapped geometry too). Glyph-level, not
         * per-contour: a bowl wall is bounded by one outer-contour edge and
         * one counter-contour edge, and those must be paired together. */
#ifdef USE_OSD_HINTING
        if (hinting) autohint_glyph(polys, npoly);
#endif
        /* rasterize into supersampled coverage over glyph bbox */
        if (npoly){
            /* bbox */
            float minx=1e9f,miny=1e9f,maxx=-1e9f,maxy=-1e9f;
            for (int i=0;i<npoly;i++) for(int j=0;j<polys[i].n;j++){
                pt P=polys[i].p[j];
                if(P.x<minx)minx=P.x; if(P.x>maxx)maxx=P.x;
                if(P.y<miny)miny=P.y; if(P.y>maxy)maxy=P.y;
            }
            int x0=(int)floorf(minx), x1=(int)ceilf(maxx);
            int y0=(int)floorf(miny), y1=(int)ceilf(maxy);
            if(x0<0)x0=0; if(y0<0)y0=0; if(x1>W)x1=W; if(y1>H)y1=H;
            int bw=x1-x0, bh=y1-y0;
            if (bw>0&&bh>0){
                uint8_t *cov=calloc((size_t)bw*bh,1);
                /* calloc can legitimately fail here (runs ~1x/s per text
                 * item, under whatever memory pressure the daemon is under
                 * at that moment) - the sibling allocations in this function
                 * are all NULL-checked, this one wasn't; skip just this
                 * glyph's coverage cleanly instead of dereferencing NULL
                 * below (polys are still freed and rendering continues with
                 * the next character, same as if the glyph rasterized to
                 * empty coverage). */
                /* M16: the scanline crossing buffer used to be a fixed
                 * float[128]; a glyph outline with more than 128 edge
                 * crossings on one scanline silently dropped the excess,
                 * and an odd RETAINED count then mis-paired every following
                 * even-odd span (visual corruption, no memory error). Size
                 * it to the worst case instead: one crossing per polyline
                 * segment, i.e. the total point count of all contours. */
                int maxint=0;
                for (int i=0;i<npoly;i++) maxint+=polys[i].n;
                float *xint = (cov && maxint>0)
                            ? malloc((size_t)maxint*sizeof(float)) : NULL;
                if (cov && xint){
                /* supersample scanlines */
                for (int py=y0;py<y1;py++){
                    for (int sub=0;sub<ss;sub++){
                        float yc=py+(sub+0.5f)/ss;
                        int nx=0;
                        for (int i=0;i<npoly;i++){
                            poly *pl=&polys[i];
                            /* Walk the closing edge first and carry the
                             * previous point, instead of indexing the far end
                             * as p[(j+1)%n]: this is the hottest loop in the
                             * file (contour segments x sub-scanlines x glyph
                             * rows) and it drops both the per-segment integer
                             * division and one of the two point loads. Same
                             * edges in the same orientation, only rotated by
                             * one, and the crossings get sorted below, so the
                             * coverage is bit-identical (verified). Replacing
                             * the % with an if(jn==n) branch instead measured
                             * ~5% SLOWER than the % on the T31 this runs on,
                             * however it reads on a desktop - the divider
                             * overlaps the float work, the branch does not. */
                            if (pl->n<1) continue;   /* p[] is NULL if poly_add() hit OOM */
                            pt A=pl->p[pl->n-1];
                            for (int j=0;j<pl->n;j++){
                                pt B=pl->p[j];
                                if ((A.y<=yc&&B.y>yc)||(B.y<=yc&&A.y>yc)){
                                    float t=(yc-A.y)/(B.y-A.y);
                                    if (nx<maxint) xint[nx++]=A.x+t*(B.x-A.x);
                                }
                                A=B;
                            }
                        }
                        /* sort */
                        for (int a=0;a<nx-1;a++) for(int b=a+1;b<nx;b++)
                            if (xint[b]<xint[a]){ float tmp=xint[a];xint[a]=xint[b];xint[b]=tmp; }
                        for (int a=0;a+1<nx;a+=2){
                            float xa=xint[a], xb=xint[a+1];
                            /* bound the pixel columns directly from xa/xb
                             * instead of scanning every column of the whole
                             * glyph bbox for every span: xc=px+(sx2+0.5)/ss
                             * has a fractional part in [0,1), so no column
                             * below floor(xa) can ever satisfy xc>=xa, and
                             * none at/above ceil(xb) can ever satisfy xc<xb.
                             * The inner per-sample compare is unchanged, so
                             * coverage output is identical - this only
                             * shrinks the (previously x0..x1, i.e. the full
                             * bbox width) range down to the span's own
                             * width. */
                            int pxa=(int)floorf(xa); if (pxa<x0) pxa=x0;
                            int pxb=(int)ceilf(xb);  if (pxb>x1) pxb=x1;
                            for (int px=pxa;px<pxb;px++){
                                for (int sx2=0;sx2<ss;sx2++){
                                    /* sub-pixel columns */
                                    float xc=px+(sx2+0.5f)/ss;
                                    if (xc>=xa&&xc<xb){
                                        uint8_t *cc=&cov[(py-y0)*bw+(px-x0)];
                                        if (*cc<ss*ss) (*cc)++;
                                    }
                                }
                            }
                        }
                    }
                }
                /* accumulate coverage into the whole-string plane */
                for (int py=y0;py<y1;py++) for(int px=x0;px<x1;px++){
                    int c=gcov[py*W+px] + cov[(py-y0)*bw+(px-x0)];
                    gcov[py*W+px]=(uint8_t)(c>ss*ss ? ss*ss : c);
                }
                }
                free(xint);
                free(cov);
            }
        }
        for (int i=0;i<npoly;i++){ free(polys[i].p); free(polys[i].curve); }
        free(polys);
        penx += advance(f,gid)*scale;
    }

    /* outline: dilate the coverage by 'outline' px (separable max filter,
     * O(W*H*outline)) and blend it in the outline color UNDER the fill */
    if (outline>0){
        uint8_t *d1=malloc((size_t)npx), *d2=malloc((size_t)npx);
        if (d1 && d2){
            for (int y=0;y<H;y++){          /* horizontal max */
                for (int x=0;x<W;x++){
                    int m=0;
                    int a=x-outline; if(a<0)a=0;
                    int b=x+outline; if(b>=W)b=W-1;
                    for (int k=a;k<=b;k++){ int c=gcov[y*W+k]; if(c>m)m=c; }
                    d1[y*W+x]=(uint8_t)m;
                }
            }
            for (int x=0;x<W;x++){          /* vertical max */
                for (int y=0;y<H;y++){
                    int m=0;
                    int a=y-outline; if(a<0)a=0;
                    int b=y+outline; if(b>=H)b=H-1;
                    for (int k=a;k<=b;k++){ int c=d1[k*W+x]; if(c>m)m=c; }
                    d2[y*W+x]=(uint8_t)m;
                }
            }
            for (size_t i=0;i<(size_t)npx;i++)
                if (d2[i]) px_blend(img, i, oc, (float)d2[i]/(ss*ss));
        }
        free(d1); free(d2);
    }
    /* fill on top of (a possible) outline */
    for (size_t i=0;i<(size_t)npx;i++)
        if (gcov[i]) px_blend(img, i, fg, (float)gcov[i]/(ss*ss));
    free(gcov);

    *out=(uint8_t*)img; *w=W; *h=H;
    return 0;
}
