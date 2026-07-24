# timps v1.4.4

## Fixed

- **Crash (use-after-free in libimp) on a live audio-module toggle.**
  Changing `audio.agc`, `audio.ns` or `audio.high_pass` via `/control` while a
  stream was running could segfault `timpsd` — SIGSEGV with the fault inside
  `libimp.so` (`epc=0`, return address in libimp). The `/control` request
  thread called `IMP_AI_Enable/DisableAgc/Ns/Hpf` on the live AI channel while
  the audio thread was inside `IMP_AI_GetFrame` — where libimp processes those
  modules — freeing state the frame path was still using.

  The toggle is now **deferred**: `/control` only flags the change; the audio
  thread re-applies it from config at a frame-free point (loop top, no frame
  held), serialized by a dedicated lock. Volume/gain/alc_gain stay direct
  (plain parameter writes, not module create/destroy). Rapid toggles coalesce
  and converge to the final configured value; no deadlock, no lost updates.

## Tooling (`tools/timps-qa.sh`)

- Added `--only <sections>` to run just selected QA sections (names or
  numbers, comma-separated), e.g. `--only onvif` or `--only 3,10`. Preflight
  always runs.
- Fixed a false positive in the on-device config-integrity check: it counted
  `=` inside comments (e.g. `# 0=err 1=warn`, `# 1 = 50Hz`) as "glued" config
  lines. Comments are now stripped before the check; a real `a=b c=d` line is
  still detected.

**Full changelog:** https://github.com/Lu-Fi/timps/compare/v1.4.3...v1.4.4
