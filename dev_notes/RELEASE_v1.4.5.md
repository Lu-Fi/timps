# timps v1.4.5

## Fixed

- **Crash (use-after-free in libaudioProcess.so) on live AGC/NS/HPF toggle — definitive fix.**
  Toggling `audio.agc`, `audio.ns` or `audio.high_pass` via `/control` on a
  running stream could still segfault `timpsd` (SIGSEGV, `epc=0`, fault in
  `libaudioProcess.so`, return address in `libimp.so`) even after the v1.4.4
  mitigation. Root cause, confirmed by disassembling the vendor libraries:
  libimp runs the AGC/NS/HPF DSP modules on its **own internal record thread**,
  and `IMP_AI_Disable{Agc,Ns,Hpf}` frees that module state and NULLs the
  processing function pointers **with no lock** — so any live enable/disable
  races the vendor thread and dereferences freed/NULL state. No caller-side
  lock or "frame-free" scheduling can close a race against a thread inside the
  closed-source library.

  `agc`, `ns`, `high_pass`, `agc_target_dbfs`, `agc_compression_db` are now
  **restart-required** (persist-only): they are written to config and applied
  only once at boot (single-threaded, before the capture loop starts — a
  one-shot Enable with no Disable, so no free/recreate churn to race). This
  eliminates the crash class by construction. Volume, gain and mic-mute remain
  live. The WebUI shows the five DSP fields as "applies on restart", like the
  codec/sample-rate settings.

## Hardening (from the 2026-07-18 code audit)

- **N1** — record duration/size keys (`segment_s`, `pre_roll_s`, `post_roll_s`,
  `min_free_mb`) are now range-clamped, so a garbage value can't disable
  rotation or set absurd rolls (`segment_s=0` still means "single file", as
  documented). Prevents an unbounded segment file filling the disk.
- **N2** — hvcC `numTemporalLayers` corrected 0 → 1 (H.265 metadata; strict
  validators like Bento4/Apple HLS).
- **N4** — RTSP `interleaved`/`client_port` values are range-checked and fall
  back to defaults on out-of-range input (protocol robustness).
- **N6** — the `/control` and status JSON accumulators no longer form an
  out-of-range pointer when the output buffer fills (latent UB removed;
  ASan-verified across all three sites plus the `control_get_json` cap==0 path).

**Full changelog:** https://github.com/Lu-Fi/timps/compare/v1.4.4...v1.4.5
