/* imp_osd.c - on-screen display via IMP_OSD.
 * One OSD group per video stream, each with up to MS_MAX_OSD overlays (TEXT or
 * BGRA LOGO) from that stream's OWN item set (cfg->osd.items[stream][..]), so
 * main and sub stream carry independent overlays. Items are placed by x/y
 * (0=center, +from left/top, -from right/resolution) and re-rendered only when
 * their expanded value changes. Region handles never collide between streams:
 * every enabled item gets its own IMP_OSD_CreateRgn handle, registered only
 * into its stream's group. Compiled for target only (-DHAL_INGENIC). */
#include "imp_osd.h"
#include "osd_text.h"
#include "osd_vars.h"
#include "msttf.h"
#ifdef HAL_INGENIC
#include "../hub.h"
#include "../log.h"
#include <imp/imp_osd.h>
#include <imp/imp_system.h>
#include <pthread.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>

#define MOD "OSD"

typedef struct {
    int         rgn;         /* IMP region handle, -1 = unused */
    int         is_text;
    int         item;        /* index into cfg->osd.items[stream] */
    uint8_t    *buf;         /* current BGRA buffer */
    uint8_t    *retired;     /* previous buffer, freed on the NEXT update:
                              * OSD_REG_PIC keeps a pointer and the hardware
                              * may still read it for an in-flight frame */
    msttf_font *font;        /* per-item font, or shared */
    char        last[256];   /* last rendered text (change detection) */
} osd_region;

typedef struct {
    int         used;
    int         si;          /* video stream index (owns items[si][..]) */
    int         grp;         /* OSD group number */
    int         width, height;
    osd_region  r[MS_MAX_OSD];
    int         pr_rgn[MS_MAX_PRIVACY]; /* privacy cover region handles, -1 unused */
} osd_stream;

static osd_stream       g_os[MS_MAX_VSTREAM];
static const ms_config *g_hcfg;
static msttf_font       g_shared; static int g_shared_ok;
static volatile int     g_run;   static pthread_t g_thr;

/* live control (imp_osd_apply) touches the regions from the HTTP thread, so
 * region access is serialized against the updater thread. Zero-cost in a
 * minimal build (no USE_CONTROL -> no second toucher, macros empty). */
#ifdef USE_CONTROL
static pthread_mutex_t  g_osd_lock = PTHREAD_MUTEX_INITIALIZER;
#define OSD_LOCK()   pthread_mutex_lock(&g_osd_lock)
#define OSD_UNLOCK() pthread_mutex_unlock(&g_osd_lock)
#else
#define OSD_LOCK()   do{}while(0)
#define OSD_UNLOCK() do{}while(0)
#endif

/* position a w x h region in a W x H frame using the old-streamer convention:
 * x/y > 0 : from the left/top edge
 * x/y < 0 : from the right/bottom edge
 * x/y == 0: centered on that axis */
static void resolve_pos(int W, int H, int w, int h, int x, int y, int *ox, int *oy)
{
    *ox = (x>0) ? x : (x<0 ? W-w+x : (W-w)/2);
    *oy = (y>0) ? y : (y<0 ? H-h+y : (H-h)/2);
    if (*ox<0) *ox=0;
    if (*oy<0) *oy=0;
    if (*ox+w>W) *ox = (W-w>0) ? (W-w) : 0;
    if (*oy+h>H) *oy = (H-h>0) ? (H-h) : 0;
}

static uint8_t *load_bgra(const char *path, int w, int h)
{
    if (w<=0||h<=0) return NULL;
    FILE *f=fopen(path,"rb"); if(!f) return NULL;
    size_t need=(size_t)w*h*4;
    uint8_t *b=malloc(need);
    if (b && fread(b,1,need,f)!=need){ free(b); b=NULL; }
    fclose(f);
    return b;
}

static void refresh_text(osd_stream *s, osd_region *rg)
{
    const ms_osd_item *it=&g_hcfg->osd.items[s->si][rg->item];
    /* it->text is runtime-mutable via /control (copystr in config_apply_kv):
     * snapshot it under the config string lock before expanding */
    char tmpl[sizeof it->text];
    config_str_lock();
    snprintf(tmpl, sizeof tmpl, "%s", it->text);
    config_str_unlock();
    char txt[256];
    osd_expand(tmpl, g_hcfg->osd.vars_file, txt, sizeof txt);
    if (strcmp(txt, rg->last)==0) return;               /* unchanged: skip render */
    strncpy(rg->last, txt, sizeof rg->last - 1); rg->last[sizeof rg->last - 1]=0;

    /* scale the font with the stream height (font_size is for 1080p) so the
     * same layout stays readable and non-overlapping on smaller sub-streams */
    int fs = it->font_size * s->height / 1080;
    if (fs < 12) fs = 12;
    if (fs > it->font_size) fs = it->font_size;

    uint8_t *bgra; int w,h;
    if (rg->font){
        if (msttf_render(rg->font, txt, fs, it->color, 0x00000000,
                         it->outline, it->outline_color, &bgra,&w,&h)!=0) return;
    } else {
        int scale=fs/16; if(scale<1)scale=1;
        if (osd_text_render(txt, scale, it->color, 0x00000000,
                            it->outline, it->outline_color, &bgra,&w,&h)!=0) return;
    }
    int x,y; resolve_pos(s->width, s->height, w, h, it->x, it->y, &x,&y);
    IMPOSDRgnAttr a; memset(&a,0,sizeof a);
    a.type=OSD_REG_PIC;
    a.rect.p0.x=x; a.rect.p0.y=y; a.rect.p1.x=x+w-1; a.rect.p1.y=y+h-1;
    a.fmt=PIX_FMT_BGRA;
    a.data.picData.pData=bgra;
    IMP_OSD_SetRgnAttr(rg->rgn, &a);
    /* double buffering: the buffer replaced two updates ago can no longer be
     * referenced by in-flight frames -> free it now; the one just replaced is
     * kept as "retired" until the next update (prevents use-after-free) */
    free(rg->retired);
    rg->retired = rg->buf;
    rg->buf = bgra;
}

static void setup_logo(osd_stream *s, osd_region *rg)
{
    const ms_osd_item *it=&g_hcfg->osd.items[s->si][rg->item];
    uint8_t *b=load_bgra(it->logo_path, it->logo_w, it->logo_h);
    if (!b){ LOGW(MOD,"logo %s (%dx%d) not loaded", it->logo_path, it->logo_w, it->logo_h); return; }
    int x,y; resolve_pos(s->width, s->height, it->logo_w, it->logo_h, it->x, it->y, &x,&y);
    IMPOSDRgnAttr a; memset(&a,0,sizeof a);
    a.type=OSD_REG_PIC;
    a.rect.p0.x=x; a.rect.p0.y=y; a.rect.p1.x=x+it->logo_w-1; a.rect.p1.y=y+it->logo_h-1;
    a.fmt=PIX_FMT_BGRA;
    a.data.picData.pData=b;
    IMP_OSD_SetRgnAttr(rg->rgn, &a);
    /* same double buffering as refresh_text: keep the buffer that was just
     * replaced around for one more update (in-flight frames may reference it) */
    free(rg->retired);
    rg->retired = rg->buf;
    rg->buf = b;
}

/* (Re)apply one privacy cover region (a solid filled rectangle) on a stream.
 * The region handle must already be created+registered. x/y/w/h are clamped to
 * the frame; a zero/empty rect or a disabled region is simply hidden.
 * NOTE: the IMP cover API varies slightly between SDK versions - this uses the
 * common form (OSD_REG_COVER + IMPOSDRgnAttr.data.coverData.color, 0xAARRGGBB).
 * If a build fails here, check <imp/imp_osd.h> for the exact coverData layout. */
static void setup_cover(osd_stream *s, int n)
{
    int rgn = s->pr_rgn[n];
    if (rgn < 0) return;
    const ms_privacy_region *p=&g_hcfg->privacy[s->si][n];
    int W=s->width, H=s->height;
    int x=p->x, y=p->y, w=p->w, h=p->h;
    if (x<0) x=0; if (y<0) y=0;
    if (w>0 && x+w>W) w=W-x;
    if (h>0 && y+h>H) h=H-y;
    if (!p->enabled || w<=0 || h<=0){
        IMP_OSD_ShowRgn(rgn, s->grp, 0);
        return;
    }
    IMPOSDRgnAttr a; memset(&a,0,sizeof a);
    a.type=OSD_REG_COVER;
    a.rect.p0.x=x; a.rect.p0.y=y; a.rect.p1.x=x+w-1; a.rect.p1.y=y+h-1;
    a.fmt=PIX_FMT_BGRA;
    a.data.coverData.color = p->color;      /* 0xAARRGGBB fill */
    IMP_OSD_SetRgnAttr(rgn, &a);
    IMPOSDGrpRgnAttr g; memset(&g,0,sizeof g);
    g.show=1; g.gAlphaEn=1; g.fgAlhpa=(uint8_t)((p->color>>24)&0xFF);
    IMP_OSD_SetGrpRgnAttr(rgn, s->grp, &g);
    IMP_OSD_ShowRgn(rgn, s->grp, 1);
}

int imp_osd_setup(const ms_config *cfg, int stream_idx, int width, int height)
{
    g_hcfg = cfg;
    if (stream_idx<0 || stream_idx>=MS_MAX_VSTREAM) return -1;

    /* the OSD group is also the carrier for privacy cover masks, so build it
     * when EITHER OSD text/logo overlays OR any privacy region is wanted */
    int want_priv=0;
    for (int i=0;i<MS_MAX_PRIVACY;i++)
        if (cfg->privacy[stream_idx][i].enabled) want_priv=1;
    if (!cfg->osd.enabled && !want_priv) return -1;

    if (cfg->osd.enabled && !g_shared_ok){
        if (cfg->osd.font_path[0] && msttf_load(&g_shared,cfg->osd.font_path)==0){
            g_shared_ok=1; LOGI(MOD,"OSD TrueType font %s",cfg->osd.font_path);
        } else LOGI(MOD,"OSD embedded bitmap font");
    }

    osd_stream *s=&g_os[stream_idx];
    s->used=1; s->si=stream_idx; s->grp=stream_idx; s->width=width; s->height=height;
    for (int i=0;i<MS_MAX_OSD;i++) s->r[i].rgn=-1;
    for (int i=0;i<MS_MAX_PRIVACY;i++) s->pr_rgn[i]=-1;

    if (IMP_OSD_CreateGroup(s->grp)<0){ LOGE(MOD,"CreateGroup %d failed",s->grp); s->used=0; return -1; }

    int active=0;
    if (cfg->osd.enabled)
    for (int i=0;i<MS_MAX_OSD;i++){
        const ms_osd_item *it=&cfg->osd.items[stream_idx][i];
        if (!it->enabled) continue;
        osd_region *rg=&s->r[i];
        rg->item=i; rg->last[0]=0;
        rg->rgn=IMP_OSD_CreateRgn(NULL);
        IMP_OSD_RegisterRgn(rg->rgn, s->grp, NULL);
        IMPOSDGrpRgnAttr g; memset(&g,0,sizeof g);
        g.show=1; g.gAlphaEn=1; g.fgAlhpa=(uint8_t)it->transparency;
        IMP_OSD_SetGrpRgnAttr(rg->rgn, s->grp, &g);
        rg->is_text=(it->type==MS_OSD_TEXT);
        if (rg->is_text){
            if (it->font_path[0]){
                msttf_font *pf=malloc(sizeof(msttf_font));
                if (pf && msttf_load(pf,it->font_path)==0) rg->font=pf;
                else { free(pf); rg->font=g_shared_ok?&g_shared:NULL; }
            } else rg->font=g_shared_ok?&g_shared:NULL;
            refresh_text(s,rg);
        } else {
            setup_logo(s,rg);
        }
        active++;
    }

    /* privacy cover masks: pre-create ALL region handles (hidden if disabled)
     * so masks can be toggled/moved LIVE without a restart, as long as the
     * group exists (OSD or privacy was on at startup) */
    int priv=0;
    for (int n=0;n<MS_MAX_PRIVACY;n++){
        s->pr_rgn[n]=IMP_OSD_CreateRgn(NULL);
        IMP_OSD_RegisterRgn(s->pr_rgn[n], s->grp, NULL);
        setup_cover(s, n);
        if (cfg->privacy[stream_idx][n].enabled) priv++;
    }

    IMP_OSD_Start(s->grp);
    LOGI(MOD,"OSD stream %d: %d overlay(s), %d privacy mask(s), group %d (%dx%d)",
         stream_idx, active, priv, s->grp, width, height);
    return s->grp;
}

/* OSD text only needs re-rendering while frames are actually encoded: any
 * video stream (or a JPEG encoder piggybacked on one) with subscribers. The
 * dedicated jpeg.* channel is not bound through OSD. With zero clients the
 * updater keeps sleeping - re-rasterizing the TTF timestamp every second on
 * every stream was pure idle CPU on the SoC. */
static int osd_needed(void)
{
    if (hub_video_subs() > 0) return 1;
    for (int i=0; i<MS_MAX_VSTREAM; i++)
        if (hub_active(HUB_JPEG_SRC_N(i))) return 1;
    return 0;
}

static void *osd_thread(void *arg)
{
    (void)arg;
    while (g_run){
        int need = osd_needed();
        if (need){
            osd_vars_set_fps(hub_get_fps(g_hcfg->osd.monitor_stream));
            OSD_LOCK();
            for (int si=0; si<MS_MAX_VSTREAM; si++){
                if (!g_os[si].used) continue;
                for (int i=0;i<MS_MAX_OSD;i++)
                    if (g_os[si].r[i].rgn>=0 && g_os[si].r[i].is_text &&
                        g_hcfg->osd.items[si][i].enabled)
                        refresh_text(&g_os[si], &g_os[si].r[i]);
            }
            OSD_UNLOCK();
        }
        for (int k=0;k<10 && g_run;k++){
            usleep(100000);
            /* a client just connected while we were idle: refresh the (stale)
             * timestamp right away instead of up to 1 s later */
            if (!need && osd_needed()) break;
        }
    }
    return NULL;
}

void imp_osd_start_updater(void)
{
    if (!g_hcfg || !g_hcfg->osd.enabled || g_run) return;
    g_run=1;
    if (pthread_create(&g_thr,NULL,osd_thread,NULL)!=0)
        g_run=0;                     /* imp_osd_stop must not join a non-thread */
}

#ifdef USE_CONTROL
/* Live re-apply of one osd item (g_cfg->osd.items[stream][item] was already
 * updated by /control) on ONE stream (or all streams for stream<0, the legacy
 * shared osdN.* keys): visibility via ShowRgn/group attr, position/text/color/
 * size/outline by invalidating the render cache and re-rendering, logos by
 * reloading. Limitation: an item that was disabled at startup has no region
 * (regions are only created in imp_osd_setup) - enabling it live is refused
 * and takes effect after a restart. */
void imp_osd_apply(int stream, int item)
{
    if (!g_hcfg || item<0 || item>=MS_MAX_OSD) return;
    if (stream>=MS_MAX_VSTREAM) return;
    OSD_LOCK();
    for (int si=0; si<MS_MAX_VSTREAM; si++){
        if (stream>=0 && si!=stream) continue;
        osd_stream *s=&g_os[si];
        if (!s->used) continue;
        const ms_osd_item *it=&g_hcfg->osd.items[si][item];
        osd_region *rg=&s->r[item];
        if (rg->rgn<0){
            if (it->enabled)
                LOGW(MOD,"osd item %d: no region on stream %d (disabled at "
                         "startup) - enable persisted, applies on restart",
                     item, si);
            continue;
        }
        IMPOSDGrpRgnAttr g; memset(&g,0,sizeof g);
        g.show=it->enabled?1:0; g.gAlphaEn=1; g.fgAlhpa=(uint8_t)it->transparency;
        IMP_OSD_SetGrpRgnAttr(rg->rgn, s->grp, &g);
        IMP_OSD_ShowRgn(rg->rgn, s->grp, it->enabled?1:0);
        LOGI(MOD,"osd stream %d item %d re-applied (enabled=%d x=%d y=%d "
                 "outline=%d)", si, item, it->enabled, it->x, it->y,
             it->outline);
        if (!it->enabled) continue;
        if (rg->is_text){
            rg->last[0]=0;              /* invalidate -> full re-render */
            refresh_text(s, rg);
        } else {
            setup_logo(s, rg);          /* reload + reposition (retires old buf) */
        }
    }
    OSD_UNLOCK();
}

/* Live re-apply of one privacy cover region (g_cfg->privacy[stream][item] was
 * already updated by /control): enable/disable, move, resize, recolor. Regions
 * are pre-created in imp_osd_setup, so this works without a restart AS LONG AS
 * the group exists (OSD or at least one privacy region was on at startup). If
 * OSD and all privacy were off at startup there is no group/region - the change
 * persists and applies on the next restart. */
void imp_osd_privacy_apply(int stream, int item)
{
    if (!g_hcfg || item<0 || item>=MS_MAX_PRIVACY) return;
    if (stream<0 || stream>=MS_MAX_VSTREAM) return;
    OSD_LOCK();
    osd_stream *s=&g_os[stream];
    if (s->used){
        if (s->pr_rgn[item] < 0){
            if (g_hcfg->privacy[stream][item].enabled)
                LOGW(MOD,"privacy %d: no region on stream %d (OSD+privacy off at "
                         "startup) - persisted, applies on restart", item, stream);
        } else {
            setup_cover(s, item);
            LOGI(MOD,"privacy stream %d region %d re-applied (enabled=%d)",
                 stream, item, g_hcfg->privacy[stream][item].enabled);
        }
    }
    OSD_UNLOCK();
}
#endif /* USE_CONTROL */

void imp_osd_stop(void)
{
    if (g_run){ g_run=0; pthread_join(g_thr,NULL); }
    for (int si=0; si<MS_MAX_VSTREAM; si++){
        osd_stream *s=&g_os[si];
        if (!s->used) continue;
        for (int i=0;i<MS_MAX_OSD;i++){
            osd_region *rg=&s->r[i];
            if (rg->rgn<0) continue;
            IMP_OSD_ShowRgn(rg->rgn, s->grp, 0);
            IMP_OSD_UnRegisterRgn(rg->rgn, s->grp);
            IMP_OSD_DestroyRgn(rg->rgn);
            free(rg->buf); rg->buf=NULL;
            free(rg->retired); rg->retired=NULL;
            if (rg->font && rg->font!=&g_shared){ msttf_free(rg->font); free(rg->font); }
            rg->rgn=-1;
        }
        for (int n=0;n<MS_MAX_PRIVACY;n++){
            if (s->pr_rgn[n]<0) continue;
            IMP_OSD_ShowRgn(s->pr_rgn[n], s->grp, 0);
            IMP_OSD_UnRegisterRgn(s->pr_rgn[n], s->grp);
            IMP_OSD_DestroyRgn(s->pr_rgn[n]);
            s->pr_rgn[n]=-1;
        }
        IMP_OSD_DestroyGroup(s->grp);
        s->used=0;
    }
    if (g_shared_ok){ msttf_free(&g_shared); g_shared_ok=0; }
}
#else
int  imp_osd_setup(const ms_config *cfg, int s, int w, int h){ (void)cfg;(void)s;(void)w;(void)h; return -1; }
void imp_osd_start_updater(void){}
void imp_osd_stop(void){}
#ifdef USE_CONTROL
void imp_osd_apply(int stream, int item){ (void)stream; (void)item; }
void imp_osd_privacy_apply(int stream, int item){ (void)stream; (void)item; }
#endif
#endif
