/* speaker.h - the daemon's sole IMP_AO owner: it arbitrates the camera speaker
 * between two producers and resamples both to one AO output rate.
 *
 *   1. Backchannel (USE_BACKCHANNEL): live ONVIF RTP -> PCM, real-time, always
 *      preempts play.
 *   2. Play queue (USE_PLAY): a FIFO at /run/timps/audio_out fed
 *      "PLAY url=<path> [vol=N gain=N rate=N format=F loop=N delay=N]" / "STOP"
 *      lines (the same wrapper prudynt/raptor ship as /usr/sbin/play), decoding
 *      Opus (USE_PLAY_OPUS), WAV and raw PCM16.
 *
 * Replaces the old popen("/bin/iac") pipe: the speaker is now driven natively
 * via the hal_ao_* HAL calls, opened lazily on first use, closed when idle.
 */
#ifndef MS_SPEAKER_H
#define MS_SPEAKER_H
#if defined(USE_BACKCHANNEL) || defined(USE_PLAY)
#include <stdint.h>

/* Preferred AO output rate (Hz). Producers are resampled to whatever rate the
 * AO is actually programmed at (may differ if the board rejects this one). */
void speaker_configure(int out_rate);

/* Backchannel sink. The first caller becomes the exclusive speaker owner,
 * preempting any in-flight play; frames from other owners are dropped until the
 * owner releases. `pcm` is mono int16 at src_rate; `owner` is any stable
 * per-session pointer. */
void speaker_write_pcm(const void *owner, const int16_t *pcm, int nsamp, int src_rate);

/* Release the speaker if this owner holds it (call at backchannel teardown). */
void speaker_release(const void *owner);

#ifdef USE_PLAY
/* Start/stop the /run/timps/audio_out play-FIFO thread. */
void speaker_start(void);
void speaker_stop(void);
#endif

#endif /* USE_BACKCHANNEL || USE_PLAY */
#endif
