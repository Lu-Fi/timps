/* speaker.c - see speaker.h. Compiled only when USE_BACKCHANNEL or USE_PLAY. */
#if defined(USE_BACKCHANNEL) || defined(USE_PLAY)
#include "speaker.h"
#include "../hal/hal.h"
#include "../codec/resample.h"
#include "../codec/g711.h"
#include "../config.h"
#include "../log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <pthread.h>

#ifdef USE_PLAY
#include <fcntl.h>
#include <errno.h>
#include <poll.h>
#include <sys/stat.h>
#endif
#ifdef USE_PLAY_OPUS
#include <opus/opusfile.h>
#endif

#define MOD "spk"
#define SPK_RS_CAP 16384

/* -------- ownership + AO device state (the single arbiter) ---------------- */
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static const void *g_owner = NULL;   /* who holds the speaker (session ptr or &g_play_tok) */
static int   g_out_rate   = 16000;   /* preferred AO rate */
static int   g_ao_rate    = 0;       /* rate the AO was actually opened at */
static int   g_ao_open    = 0;       /* HAL AO is up */
static int   g_bc_active  = 0;       /* backchannel currently owns/preempts */
static int16_t g_rs[SPK_RS_CAP];     /* resample scratch, only touched under g_lock */

#ifdef USE_PLAY
static const char g_play_tok = 0;    /* sentinel owner value for the play queue */
static volatile int g_stop_play = 0; /* abort current playback (STOP / new PLAY / preempt) */
#endif

/* caller holds g_lock */
static int ao_ensure(void)
{
    if (g_ao_open) return 0;
    int r = hal_ao_open(g_out_rate);
    if (r < 0) return -1;
    g_ao_rate = r; g_ao_open = 1;
    /* global default; a per-PLAY vol=/gain= overrides it after ownership is
     * taken (play_write), backchannel keeps it. */
    hal_ao_set_vol(g_cfg.audio.spk_volume);
    hal_ao_set_gain(g_cfg.audio.spk_gain);
    return 0;
}
/* caller holds g_lock */
static void ao_drop(void)
{
    if (g_ao_open){ hal_ao_close(); g_ao_open = 0; g_ao_rate = 0; }
}

void speaker_configure(int out_rate)
{
    if (out_rate < 8000 || out_rate > 48000) out_rate = 16000;
    g_out_rate = out_rate;
}

/* -------- backchannel producer ------------------------------------------- */
void speaker_write_pcm(const void *owner, const int16_t *pcm, int nsamp, int src_rate)
{
    if (nsamp <= 0) return;
    pthread_mutex_lock(&g_lock);
    if (g_owner != owner){
        /* backchannel is the real-time path: preempt whoever holds the speaker
         * (a running play sees g_stop_play/g_bc_active and yields on its next
         * block) and take it. */
#ifdef USE_PLAY
        if (g_owner == &g_play_tok) g_stop_play = 1;
#endif
        if (ao_ensure() != 0){ pthread_mutex_unlock(&g_lock); return; }
        g_owner = owner; g_bc_active = 1;
        LOGI(MOD, "speaker owner acquired (backchannel, %d Hz)", g_ao_rate);
    }
    int rn = ms_resample(pcm, nsamp, src_rate, g_ao_rate, g_rs, SPK_RS_CAP);
    hal_ao_write(g_rs, rn);
    pthread_mutex_unlock(&g_lock);
}

void speaker_release(const void *owner)
{
    pthread_mutex_lock(&g_lock);
    if (g_owner == owner){
        g_owner = NULL; g_bc_active = 0;
        ao_drop();
        LOGI(MOD, "speaker owner released (backchannel)");
    }
    pthread_mutex_unlock(&g_lock);
}

void speaker_set_volume(int vol)
{
    pthread_mutex_lock(&g_lock);
    if (g_ao_open) hal_ao_set_vol(vol);
    pthread_mutex_unlock(&g_lock);
}

void speaker_set_gain(int gain)
{
    pthread_mutex_lock(&g_lock);
    if (g_ao_open) hal_ao_set_gain(gain);
    pthread_mutex_unlock(&g_lock);
}

/* ======================================================================== */
/*  Play queue (USE_PLAY)                                                    */
/* ======================================================================== */
#ifdef USE_PLAY

#define SPK_FIFO     "/run/timps/audio_out"
#define SPK_DIR      "/run/timps"
#define SPK_DEC_FR   4096                 /* mono output frames per decoder read */

struct pjob {
    char path[512];
    char fmt[16];
    int  vol, gain;      /* -1 = leave AO default */
    int  rate;           /* raw-PCM sample rate hint, 0 = use g_out_rate */
    int  loops;          /* total plays, 1..32 */
    int  delay_ms;       /* pause between loops */
};

/* one queued request (single slot, latest wins) + STOP, under g_lock */
static struct pjob g_pending;
static int         g_pending_valid = 0;
static pthread_t   g_play_thr;
static int         g_play_started = 0;
static volatile int g_play_run = 0;
static int         g_fifo_fd = -1;

/* interleaved decode scratch (up to 2 source channels), play thread only */
static int16_t g_ilv[SPK_DEC_FR * 2];
static int16_t g_dec[SPK_DEC_FR];     /* downmixed mono block */

/* ---- tiny streaming decoder (WAV / raw PCM16 / µ-law·A-law / Opus) ------ */
struct dec {
    int   kind;          /* 0 = PCM/law from FILE, 1 = Opus */
    FILE *fp;
    int   fmt;           /* WAV audioFormat: 1=PCM, 6=A-law, 7=µ-law */
    int   channels;
    int   rate;
    long  data_left;     /* bytes remaining in the WAV data chunk, -1 = to EOF */
#ifdef USE_PLAY_OPUS
    OggOpusFile *of;
#endif
};

static uint32_t rd_le32(const uint8_t *p){ return p[0]|(p[1]<<8)|(p[2]<<16)|((uint32_t)p[3]<<24); }
static uint16_t rd_le16(const uint8_t *p){ return p[0]|(p[1]<<8); }

/* parse a canonical RIFF/WAVE header; leaves fp positioned at PCM data. */
static int wav_open(struct dec *d)
{
    uint8_t h[12];
    if (fread(h,1,12,d->fp)!=12) return -1;
    if (memcmp(h,"RIFF",4) || memcmp(h+8,"WAVE",4)) return -1;
    int have_fmt = 0;
    for (;;){
        uint8_t ch[8];
        if (fread(ch,1,8,d->fp)!=8) return -1;
        uint32_t sz = rd_le32(ch+4);
        if (!memcmp(ch,"fmt ",4)){
            uint8_t f[16];
            uint32_t take = sz < 16 ? sz : 16;
            if (fread(f,1,take,d->fp)!=take) return -1;
            d->fmt      = rd_le16(f);
            d->channels = rd_le16(f+2);
            d->rate     = (int)rd_le32(f+4);
            have_fmt = 1;
            if (sz > take) fseek(d->fp,(long)(sz-take),SEEK_CUR);
        } else if (!memcmp(ch,"data",4)){
            d->data_left = (long)sz;
            break;
        } else {
            fseek(d->fp,(long)sz,SEEK_CUR);           /* skip LIST/fact/etc. */
        }
        if (sz & 1) fseek(d->fp,1,SEEK_CUR);          /* chunks are word-aligned */
    }
    if (!have_fmt || d->channels < 1 || d->channels > 2 || d->rate <= 0) return -1;
    if (d->fmt != 1 && d->fmt != 6 && d->fmt != 7) return -1;
    return 0;
}

static struct dec *dec_open(const struct pjob *j)
{
    struct dec *d = calloc(1,sizeof *d);
    if (!d) return NULL;
    d->data_left = -1; d->channels = 1; d->fmt = 1;

    /* sniff the first bytes unless format= forced it */
    FILE *fp = fopen(j->path,"rb");
    if (!fp){ LOGW(MOD,"play: cannot open %s: %s", j->path, strerror(errno)); free(d); return NULL; }
    uint8_t sig[4] = {0};
    size_t got = fread(sig,1,4,fp);
    rewind(fp);
    d->fp = fp;

    int is_opus = !strcmp(j->fmt,"opus");
    int is_wav  = !strcmp(j->fmt,"wav");
    int is_pcm  = !strcmp(j->fmt,"pcm") || !strcmp(j->fmt,"raw");
    if (!j->fmt[0]){                                   /* auto-detect */
        if (got>=4 && !memcmp(sig,"OggS",4)) is_opus = 1;
        else if (got>=4 && !memcmp(sig,"RIFF",4)) is_wav = 1;
        else is_pcm = 1;
    }

    if (is_opus){
#ifdef USE_PLAY_OPUS
        fclose(d->fp); d->fp = NULL;
        int oe = 0;
        d->of = op_open_file(j->path,&oe);
        if (!d->of){ LOGW(MOD,"play: op_open_file(%s) failed (%d)", j->path, oe); free(d); return NULL; }
        d->kind = 1;
        d->rate = 48000;                               /* opusfile always decodes to 48 kHz */
        int cc = op_channel_count(d->of,-1);
        d->channels = (cc==1||cc==2) ? cc : 2;
        return d;
#else
        LOGW(MOD,"play: %s is Opus but USE_PLAY_OPUS not built", j->path);
        fclose(d->fp); free(d); return NULL;
#endif
    }
    if (is_wav){
        if (wav_open(d)!=0){ LOGW(MOD,"play: bad WAV header in %s", j->path); fclose(d->fp); free(d); return NULL; }
        d->kind = 0;
        return d;
    }
    /* raw PCM16 mono */
    (void)is_pcm;
    d->kind = 0; d->fmt = 1; d->channels = 1;
    d->rate = j->rate > 0 ? j->rate : g_out_rate;
    d->data_left = -1;
    return d;
}

static void dec_close(struct dec *d)
{
    if (!d) return;
#ifdef USE_PLAY_OPUS
    if (d->of) op_free(d->of);
#endif
    if (d->fp) fclose(d->fp);
    free(d);
}

/* downmix `frames` interleaved int16 (ch channels) in g_ilv -> g_dec mono */
static void downmix(int frames, int ch)
{
    if (ch == 1){ memcpy(g_dec, g_ilv, (size_t)frames*sizeof(int16_t)); return; }
    for (int i=0;i<frames;i++)
        g_dec[i] = (int16_t)(((int)g_ilv[2*i] + (int)g_ilv[2*i+1]) / 2);
}

/* read one mono block into g_dec; returns frames (>0), 0 = EOF, <0 = error.
 * *src_rate is the block's sample rate. */
static int dec_read(struct dec *d, int *src_rate)
{
    *src_rate = d->rate;
#ifdef USE_PLAY_OPUS
    if (d->kind == 1){
        /* buf_size is the TOTAL samples across channels op_read may write, and
         * it returns samples-per-channel. Cap it at SPK_DEC_FR so a mono file
         * never returns more per channel than g_dec holds (no dropped audio),
         * and g_ilv (SPK_DEC_FR*2) always fits the interleaved result. */
        int n = op_read(d->of, g_ilv, SPK_DEC_FR, NULL);        /* n = samples/channel */
        if (n <= 0) return n;                                   /* 0=EOF, <0=hole/err */
        downmix(n, d->channels);
        return n;
    }
#endif
    int ch = d->channels;
    if (d->fmt == 6 || d->fmt == 7){                            /* 8-bit companded */
        int want = SPK_DEC_FR * ch;
        if (d->data_left >= 0 && want > d->data_left) want = (int)d->data_left;
        if (want <= 0) return 0;
        uint8_t law[SPK_DEC_FR * 2];
        int got = (int)fread(law,1,(size_t)want,d->fp);
        if (got <= 0) return 0;
        if (d->data_left >= 0) d->data_left -= got;
        if (d->fmt == 7) g711_ulaw_decode(law,(size_t)got,g_ilv);
        else             g711_alaw_decode(law,(size_t)got,g_ilv);
        downmix(got/ch, ch);
        return got/ch;
    }
    /* PCM16 */
    int want_bytes = SPK_DEC_FR * ch * (int)sizeof(int16_t);
    if (d->data_left >= 0 && want_bytes > d->data_left) want_bytes = (int)d->data_left;
    if (want_bytes < (int)sizeof(int16_t)) return 0;
    int got = (int)fread(g_ilv,1,(size_t)want_bytes,d->fp);
    if (got < (int)sizeof(int16_t)) return 0;
    if (d->data_left >= 0) d->data_left -= got;
    int frames = (got/2)/ch;
    downmix(frames, ch);
    return frames;
}

/* write one decoded block. Returns 0 ok, -1 = must abort (preempted/failed). */
static int play_write(int frames, int src_rate, int vol, int gain)
{
    pthread_mutex_lock(&g_lock);
    if (g_bc_active){ pthread_mutex_unlock(&g_lock); return -1; }
    if (g_owner != &g_play_tok){
        if (g_owner != NULL){ pthread_mutex_unlock(&g_lock); return -1; }  /* busy */
        if (ao_ensure() != 0){ pthread_mutex_unlock(&g_lock); return -1; }
        g_owner = &g_play_tok;
        if (vol  >= 0) hal_ao_set_vol(vol);
        if (gain >= 0) hal_ao_set_gain(gain);
        LOGI(MOD, "speaker owner acquired (play, %d Hz)", g_ao_rate);
    }
    int rn = ms_resample(g_dec, frames, src_rate, g_ao_rate, g_rs, SPK_RS_CAP);
    int rc = hal_ao_write(g_rs, rn);
    pthread_mutex_unlock(&g_lock);
    return rc;
}

static void play_release(void)
{
    pthread_mutex_lock(&g_lock);
    if (g_owner == &g_play_tok){ g_owner = NULL; ao_drop(); LOGI(MOD,"speaker owner released (play)"); }
    pthread_mutex_unlock(&g_lock);
}

static void parse_play(char *line, struct pjob *j)
{
    memset(j,0,sizeof *j);
    j->vol = -1; j->gain = -1; j->loops = 1;
    char *save = NULL;
    for (char *t = strtok_r(line," \t",&save); t; t = strtok_r(NULL," \t",&save)){
        if (!strncmp(t,"url=",4))         snprintf(j->path,sizeof j->path,"%s",t+4);
        else if (!strncmp(t,"format=",7)) snprintf(j->fmt,sizeof j->fmt,"%s",t+7);
        else if (!strncmp(t,"vol=",4))    j->vol   = atoi(t+4);
        else if (!strncmp(t,"gain=",5))   j->gain  = atoi(t+5);
        else if (!strncmp(t,"rate=",5))   j->rate  = atoi(t+5);
        else if (!strncmp(t,"loop=",5))   j->loops = atoi(t+5);
        else if (!strncmp(t,"delay=",6))  j->delay_ms = atoi(t+6);
        /* append= is accepted for wrapper compatibility but ignored: the queue
         * is a single latest-wins slot, not an accumulating playlist. */
    }
    if (j->loops < 1)  j->loops = 1;
    if (j->loops > 32) j->loops = 32;
    if (j->delay_ms < 0)    j->delay_ms = 0;
    if (j->delay_ms > 5000) j->delay_ms = 5000;
}

/* drain any complete command lines from the FIFO. block_ms<0 = block until a
 * line or g_play_run clears; 0 = non-blocking poll. Updates g_pending/g_stop_play. */
static void fifo_drain(int block_ms)
{
    static char buf[1024];
    static int  used = 0;
    struct pollfd pfd = { .fd = g_fifo_fd, .events = POLLIN };
    int budget = block_ms;
    do {
        int to = (block_ms < 0) ? 200 : block_ms;
        int pr = poll(&pfd,1,to);
        if (!g_play_run) return;
        if (pr <= 0){
            if (block_ms < 0) continue;               /* keep blocking */
            return;
        }
        char rb[512];
        int n = (int)read(g_fifo_fd, rb, sizeof rb);
        if (n <= 0) return;
        for (int i=0;i<n;i++){
            char c = rb[i];
            if (c=='\n' || c=='\r'){
                if (used == 0) continue;
                buf[used] = 0; used = 0;
                if (!strncmp(buf,"STOP",4)){
                    pthread_mutex_lock(&g_lock);
                    g_pending_valid = 0; g_stop_play = 1;
                    pthread_mutex_unlock(&g_lock);
                } else if (!strncmp(buf,"PLAY",4)){
                    struct pjob j; parse_play(buf+4,&j);
                    if (j.path[0]){
                        pthread_mutex_lock(&g_lock);
                        g_pending = j; g_pending_valid = 1; g_stop_play = 1;
                        pthread_mutex_unlock(&g_lock);
                    }
                }
            } else if (used < (int)sizeof(buf)-1){
                buf[used++] = c;
            }
        }
        return;                                       /* one read per call is plenty */
    } while (budget != 0);
}

static void play_job(const struct pjob *j)
{
    LOGI(MOD, "play: %s (vol=%d gain=%d loop=%d)", j->path, j->vol, j->gain, j->loops);
    for (int rep=0; rep < j->loops && g_play_run && !g_stop_play; rep++){
        struct dec *d = dec_open(j);
        if (!d) break;
        int frames, src;
        while ((frames = dec_read(d,&src)) > 0){
            if (g_stop_play || !g_play_run) break;
            if (play_write(frames, src, j->vol, j->gain) != 0){ g_stop_play = 1; break; }
            fifo_drain(0);                            /* stay responsive to STOP / new PLAY */
        }
        dec_close(d);
        if (rep+1 < j->loops && j->delay_ms > 0 && !g_stop_play){
            for (int slept=0; slept < j->delay_ms && g_play_run && !g_stop_play; slept += 50){
                usleep(50000);
                fifo_drain(0);
            }
        }
    }
    play_release();
}

static void *play_thread(void *arg)
{
    (void)arg;
    LOGI(MOD, "play-FIFO thread started (%s)", SPK_FIFO);
    while (g_play_run){
        struct pjob j; int got = 0;
        pthread_mutex_lock(&g_lock);
        if (g_pending_valid){ j = g_pending; g_pending_valid = 0; g_stop_play = 0; got = 1; }
        pthread_mutex_unlock(&g_lock);
        if (!got){ fifo_drain(-1); continue; }
        /* wait out any active backchannel before starting, staying cancellable */
        while (g_play_run && g_bc_active && !g_stop_play) fifo_drain(200);
        if (!g_play_run || g_stop_play) continue;     /* cancelled while waiting */
        play_job(&j);
    }
    LOGI(MOD, "play-FIFO thread stopped");
    return NULL;
}

void speaker_start(void)
{
    if (g_play_started) return;
    mkdir(SPK_DIR, 0755);                              /* usually a tmpfs mount already */
    unlink(SPK_FIFO);
    if (mkfifo(SPK_FIFO, 0660) != 0 && errno != EEXIST){
        LOGW(MOD, "mkfifo %s failed: %s - play queue disabled", SPK_FIFO, strerror(errno));
        return;
    }
    /* O_RDWR so the reader always has a writer: open() never blocks and reads
     * return EAGAIN (via poll) on an empty FIFO instead of a spurious EOF. */
    g_fifo_fd = open(SPK_FIFO, O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (g_fifo_fd < 0){
        LOGW(MOD, "open %s failed: %s - play queue disabled", SPK_FIFO, strerror(errno));
        return;
    }
    g_play_run = 1;
    if (pthread_create(&g_play_thr, NULL, play_thread, NULL) != 0){
        LOGW(MOD, "cannot start play-FIFO thread");
        g_play_run = 0; close(g_fifo_fd); g_fifo_fd = -1;
        return;
    }
    g_play_started = 1;
}

void speaker_stop(void)
{
    if (!g_play_started) return;
    g_play_run = 0; g_stop_play = 1;
    pthread_join(g_play_thr, NULL);
    if (g_fifo_fd >= 0){ close(g_fifo_fd); g_fifo_fd = -1; }
    unlink(SPK_FIFO);
    g_play_started = 0;
}

int speaker_play_line(const char *line)
{
    if (!line || !line[0]) return -1;
    /* the reader holds the FIFO O_RDWR, so this write side never blocks on a
     * missing reader; one line stays under PIPE_BUF, so the write is atomic. */
    int fd = open(SPK_FIFO, O_WRONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return -1;
    char b[600];
    int n = snprintf(b, sizeof b, "%s\n", line);
    if (n < 0){ close(fd); return -1; }
    if (n > (int)sizeof b) n = (int)sizeof b;
    ssize_t w = write(fd, b, (size_t)n);
    close(fd);
    return (w == n) ? 0 : -1;
}

#endif /* USE_PLAY */
#endif /* USE_BACKCHANNEL || USE_PLAY */
