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
    f->num_hmetrics=hhea?u16(d+hhea+34):1;
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
typedef struct { pt *p; int n, cap; } poly;   /* flattened polyline (one contour) */

static void poly_add(poly *pl, float x, float y){
    if (pl->n>=pl->cap){
        int ncap=pl->cap?pl->cap*2:64;
        pt *np=realloc(pl->p,(size_t)ncap*sizeof(pt));
        /* on OOM, drop the point rather than overwrite pl->p with NULL
         * while pl->cap already reflects the larger (unrealized) capacity -
         * that combination used to cause a NULL-pointer write on the very
         * next poly_add() call */
        if (!np) return;
        pl->p=np; pl->cap=ncap;
    }
    pl->p[pl->n].x=x; pl->p[pl->n].y=y; pl->n++;
}
static void quad(poly *pl, pt a, pt c, pt b){
    int steps=8;
    for (int i=1;i<=steps;i++){
        float t=(float)i/steps, mt=1-t;
        float x=mt*mt*a.x+2*mt*t*c.x+t*t*b.x;
        float y=mt*mt*a.y+2*mt*t*c.y+t*t*b.y;
        poly_add(pl,x,y);
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
            cur=startpt; poly_add(pl,cur.x,cur.y); si=start;
        } else {
            startpt.x=PX(si); startpt.y=PY(si); cur=startpt;
            poly_add(pl,cur.x,cur.y);
        }
        for (int k=1;k<=cnt;k++){
            int idx=start+((si-start)+k)%cnt;
            if (ONC(idx)){ pt e={PX(idx),PY(idx)}; poly_add(pl,e.x,e.y); cur=e; }
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
                            for (int j=0;j<pl->n;j++){
                                pt A=pl->p[j], B=pl->p[(j+1)%pl->n];
                                if ((A.y<=yc&&B.y>yc)||(B.y<=yc&&A.y>yc)){
                                    float t=(yc-A.y)/(B.y-A.y);
                                    if (nx<maxint) xint[nx++]=A.x+t*(B.x-A.x);
                                }
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
        for (int i=0;i<npoly;i++) free(polys[i].p);
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
