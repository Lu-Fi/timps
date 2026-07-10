#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <imp/imp_audio.h>
#include "audio.h"
#include "config.h"
#include "log.h"

#define AI_DEV   0
#define AI_CHN   0
#define AE_CHN   0
#define MAX_AUDIO_SINKS  8

static struct {
    volatile int   running;
    pthread_t      thread;

    pthread_mutex_t sink_lock;
    AudioSink       sinks[MAX_AUDIO_SINKS];
    int             nsinks;
} g_audio;

int audio_init(void)
{
    IMPAudioIOAttr   attr;
    IMPAudioEncChnAttr enc_attr;
    int ret;

    if (g_cfg.audio_codec == AUDIO_NONE) return 0;

    memset(&attr, 0, sizeof(attr));
    attr.samplerate = g_cfg.audio_sample_rate;
    attr.bitwidth   = AUDIO_BIT_WIDTH_16;
    attr.soundmode  = AUDIO_SOUND_MODE_MONO;
    attr.frmNum     = 20;
    attr.numPerFrm  = 640;
    attr.chnCnt     = 1;

    ret = IMP_AI_SetPubAttr(AI_DEV, &attr);
    if (ret) { log_error("IMP_AI_SetPubAttr: %d", ret); return ret; }

    ret = IMP_AI_Enable(AI_DEV);
    if (ret) { log_error("IMP_AI_Enable: %d", ret); return ret; }

    ret = IMP_AI_EnableChn(AI_DEV, AI_CHN);
    if (ret) { log_error("IMP_AI_EnableChn: %d", ret); goto err_dev; }

    /* Volume */
    IMP_AI_SetVolume(AI_DEV, AI_CHN, g_cfg.audio_volume);

    /* Encoder channel */
    memset(&enc_attr, 0, sizeof(enc_attr));
    switch (g_cfg.audio_codec) {
    case AUDIO_G711A:
        enc_attr.type = PT_G711A;
        break;
    case AUDIO_G711U:
        enc_attr.type = PT_G711U;
        break;
    case AUDIO_AAC:
        enc_attr.type = PT_AAC;
        break;
    default:
        enc_attr.type = PT_G711A;
        break;
    }
    enc_attr.bufSize = 20;

    ret = IMP_AENC_CreateChn(AE_CHN, &enc_attr);
    if (ret) { log_error("IMP_AENC_CreateChn: %d", ret); goto err_chn; }

    pthread_mutex_init(&g_audio.sink_lock, NULL);
    log_info("audio init ok: codec=%d  rate=%dHz",
             g_cfg.audio_codec, g_cfg.audio_sample_rate);
    return 0;

err_chn:
    IMP_AI_DisableChn(AI_DEV, AI_CHN);
err_dev:
    IMP_AI_Disable(AI_DEV);
    return ret;
}

void audio_exit(void)
{
    if (g_cfg.audio_codec == AUDIO_NONE) return;
    audio_stop();
    IMP_AENC_DestroyChn(AE_CHN);
    IMP_AI_DisableChn(AI_DEV, AI_CHN);
    IMP_AI_Disable(AI_DEV);
    pthread_mutex_destroy(&g_audio.sink_lock);
}

static void *audio_thread(void *arg)
{
    IMPAudioFrame  pcm;
    IMPAudioStream enc;
    AudioFrame     af;
    (void)arg;

    log_debug("audio thread started");
    while (g_audio.running) {
        if (IMP_AI_PollingFrame(AI_DEV, AI_CHN, 1000) != 0) continue;

        memset(&pcm, 0, sizeof(pcm));
        if (IMP_AI_GetFrame(AI_DEV, AI_CHN, &pcm, BLOCK) != 0) continue;

        IMP_AENC_SendFrame(AE_CHN, &pcm);
        IMP_AI_ReleaseFrame(AI_DEV, AI_CHN, &pcm);

        if (IMP_AENC_PollingStream(AE_CHN, 1000) != 0) continue;

        memset(&enc, 0, sizeof(enc));
        if (IMP_AENC_GetStream(AE_CHN, &enc, BLOCK) != 0) continue;

        af.data = (uint8_t *)enc.stream;
        af.len  = enc.len;
        af.pts  = enc.timeStamp;

        pthread_mutex_lock(&g_audio.sink_lock);
        for (int i = 0; i < g_audio.nsinks; i++) {
            if (g_audio.sinks[i].cb)
                g_audio.sinks[i].cb(&af, g_audio.sinks[i].userdata);
        }
        pthread_mutex_unlock(&g_audio.sink_lock);

        IMP_AENC_ReleaseStream(AE_CHN, &enc);
    }
    log_debug("audio thread exiting");
    return NULL;
}

int audio_start(void)
{
    if (g_cfg.audio_codec == AUDIO_NONE) return 0;
    if (g_audio.running) return 0;

    g_audio.running = 1;
    if (pthread_create(&g_audio.thread, NULL, audio_thread, NULL)) {
        g_audio.running = 0;
        return -1;
    }
    return 0;
}

void audio_stop(void)
{
    if (!g_audio.running) return;
    g_audio.running = 0;
    pthread_join(g_audio.thread, NULL);
}

int audio_add_sink(AudioSink *sink)
{
    pthread_mutex_lock(&g_audio.sink_lock);
    if (g_audio.nsinks >= MAX_AUDIO_SINKS) {
        pthread_mutex_unlock(&g_audio.sink_lock);
        return -1;
    }
    g_audio.sinks[g_audio.nsinks++] = *sink;
    pthread_mutex_unlock(&g_audio.sink_lock);
    return 0;
}

void audio_remove_sink(AudioSink *sink)
{
    pthread_mutex_lock(&g_audio.sink_lock);
    for (int i = 0; i < g_audio.nsinks; i++) {
        if (g_audio.sinks[i].cb       == sink->cb &&
            g_audio.sinks[i].userdata == sink->userdata) {
            g_audio.sinks[i] = g_audio.sinks[--g_audio.nsinks];
            break;
        }
    }
    pthread_mutex_unlock(&g_audio.sink_lock);
}

int audio_rtp_pt(void)
{
    switch (g_cfg.audio_codec) {
    case AUDIO_G711U: return 0;   /* PCMU */
    case AUDIO_G711A: return 8;   /* PCMA */
    case AUDIO_AAC:   return 97;  /* dynamic */
    default:          return -1;
    }
}

int audio_sample_rate(void)
{
    return g_cfg.audio_sample_rate;
}
