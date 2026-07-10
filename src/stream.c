#include <pthread.h>
#include "stream.h"
#include "encoder.h"
#include "audio.h"
#include "log.h"

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

static int g_video_refs[2]  = {0, 0};
static int g_audio_refs     = 0;

int stream_acquire(int idx)
{
    int ret = 0;
    pthread_mutex_lock(&g_lock);
    if (g_video_refs[idx] == 0) {
        ret = encoder_create(idx);
        if (ret == 0)
            ret = encoder_start(idx);
        if (ret != 0) {
            log_error("stream_acquire(%d): encoder start failed", idx);
            goto out;
        }
    }
    g_video_refs[idx]++;
out:
    pthread_mutex_unlock(&g_lock);
    return ret;
}

void stream_release(int idx)
{
    pthread_mutex_lock(&g_lock);
    if (g_video_refs[idx] <= 0) {
        pthread_mutex_unlock(&g_lock);
        return;
    }
    if (--g_video_refs[idx] == 0) {
        encoder_stop(idx);
        encoder_destroy(idx);
    }
    pthread_mutex_unlock(&g_lock);
}

int stream_audio_acquire(void)
{
    int ret = 0;
    pthread_mutex_lock(&g_lock);
    if (g_audio_refs == 0) {
        ret = audio_start();
        if (ret != 0) {
            log_error("stream_audio_acquire: audio start failed");
            goto out;
        }
    }
    g_audio_refs++;
out:
    pthread_mutex_unlock(&g_lock);
    return ret;
}

void stream_audio_release(void)
{
    pthread_mutex_lock(&g_lock);
    if (g_audio_refs <= 0) {
        pthread_mutex_unlock(&g_lock);
        return;
    }
    if (--g_audio_refs == 0)
        audio_stop();
    pthread_mutex_unlock(&g_lock);
}

int stream_add_video_sink(int idx, EncoderSink *sink)
{
    return encoder_add_sink(idx, sink);
}

void stream_remove_video_sink(int idx, EncoderSink *sink)
{
    encoder_remove_sink(idx, sink);
}

int stream_add_audio_sink(AudioSink *sink)
{
    return audio_add_sink(sink);
}

void stream_remove_audio_sink(AudioSink *sink)
{
    audio_remove_sink(sink);
}

int stream_viewer_count(int idx)
{
    int c;
    pthread_mutex_lock(&g_lock);
    c = g_video_refs[idx];
    pthread_mutex_unlock(&g_lock);
    return c;
}

int stream_audio_listeners(void)
{
    int c;
    pthread_mutex_lock(&g_lock);
    c = g_audio_refs;
    pthread_mutex_unlock(&g_lock);
    return c;
}
