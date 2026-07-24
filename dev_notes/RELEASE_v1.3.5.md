# v1.3.5

One fix: audio input on the Ingenic HAL could silently fail to come up,
leaving RTSP/HTTP clients an advertised-but-permanently-silent audio track
with nothing in the log to explain why.

## Fixed

- **hal_ingenic: check IMP_AI enable/config return codes, add a capture
  watchdog.** `IMP_AI_Enable`, `IMP_AI_SetChnParam` and `IMP_AI_EnableChn`
  were fire-and-forget - the audio thread fell through to marking audio
  "up" and advertising it to RTSP/HTTP regardless of what they returned. On
  some T-series + sensor combinations one of these calls fails (the
  identical failure was reported against prudynt-t on a T23N + sc2336
  board), so the stream kept a live-looking audio track that never carried
  a single frame, with no error anywhere in the log.
  - All three calls are now return-checked; on failure the thread logs,
    tears down what it already brought up, and bails before advertising
    audio at all.
  - A new consecutive-failure watchdog in the capture loop catches the case
    where `IMP_AI_EnableChn` reports success but the channel still never
    yields a frame: after ~5 s of straight timeouts it logs once, clears
    the advertised audio state, and exits cleanly instead of spinning
    forever with a dead track.

**Full diff**: https://github.com/Lu-Fi/timps/compare/v1.3.4...v1.3.5
