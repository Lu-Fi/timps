# Adversarial Review – Opus Fixes to the Audits from 2026-08-07

**Date:** 2026-08-07
**Subject:** an uncommitted diff in worktree `agent-a879c6c77c61abd29` (base = `main` @ `074e8f5`, 16 files, +542/-162), produced by Opus as the implementation of `CODE_AUDIT_2026-08-07.md` (A1/A2/A3/A6), `config-completeness-audit-2026-08-07.md` (F-01...F-10), `PERFORMANCE_AUDIT_2026-08-07.md` (P-03/P-04/P-08), and the sdk-feature-gaps refresh.
**Method:** a full reading of the diff, targeted hand-verification of all five risk points Opus itself flagged, leftover greps for every snapshot refactor, an own build (`make sim`, `make test-auth`), **and** an own `-fsyntax-only` run of the three non-sim files against the T31 1.1.6 headers (claims were not taken on faith, but reproduced).

---

## Overall Verdict

**Mergeable.** No blocking finding. Every fix claimed in the Opus report is actually and correctly present in the diff; the five risk points hold up under deep inspection. The items explicitly skipped (P-01, P-02, SDK #5/#6) are cleanly skipped — **no** half-started code, no dead leftovers. Two new LOW/INFO findings (R-01, R-02), none of them merge-blocking.

Builds verified independently: `make sim` **warning-free** (`-Wall -Wextra`, C11), `make test-auth` **PASS=4 FAIL=0 SKIP=2** (identical to the audit baseline). `imp_osd.c`/`hal_ingenic.c`/`speaker.c` pass `-fsyntax-only` against the T31 headers warning-free (an independent run, not Opus's).

---

## Verdict per Item

### Tier 1

| Item | Verdict | Evidence |
| --- | --- | --- |
| A2 – OSD rotation moved to `g_cfg_boot` (5 sites) | ✅ **CONFIRMED** | grep: **no** remaining `g_hcfg->video[..].rotation` in imp_osd.c; all 5 sites (osd_rot_place :207, refresh_text :268, setup_logo :313, setup_cover :359, the setup warning :411) now use `g_cfg_boot`. `g_cfg_boot` is visible via `imp_osd.h → ../config.h`; `config_snapshot_boot()` (main.c:107) runs before HAL setup and before `record_start`/`timelapse_start` (main.c:160-161) — no read-before-snapshot gap. |
| A2 scope expansion (5 instead of 3 sites) | ✅ **correct, not overreach** | `setup_cover` (privacy-mask height clamp + even-align) and the setup warning both depend on the rotation the **running** encoder produces — exactly the same restart-only semantics and the same race class as the three sites named in the audit. The audit simply overlooked these two; the expansion is the more complete implementation of the same fix. |
| F-01 – `sensor.*` clamps | ✅ **CONFIRMED** | i2c_addr 0-0x7F (the full 7-bit address space), fps 0-120, width/height 0-8192; lo=0 preserves the "0 = auto" semantics. The clamp applies table-driven for both file load **and** POST. |
| A1/F-02/F-03 – completion of the C11 sweep | ✅ **CONFIRMED** | Leftover greps are clean: all remaining `g_rc->record.*`/`g_tc->timelapse.*` reads sit **inside** lock blocks (strings :180/:231-232/:123/:160-161; status/clip ints in the new lock blocks :604-607/:650-651/:349-351). The `rec_thread` snapshot is correctly threaded through `want_run`/`want_write`/`motion_recent`/`seg_open`/`prune_free` (signatures updated at every call site, `(void)rc` in the `!USE_CONTROL` stub). timelapse analogous. speaker.c/hal_ingenic.c/rtsp.c/status accessors: lock+copy present, the rest of each function consistently uses the local copy. `control_get_json`: **no** remaining `c->image.*`/`c->audio.*` (grep empty). |
| `apply_mu` in `control_apply_json` | ✅ **CONFIRMED — deadlock-free** | A complete enumeration of the function's return paths (control.c:357-659): exactly **three** exits — (1) the `!json` early return **before** the lock, (2) the OOM path with `unlock` before `return` (:425), (3) the end of the function with `unlock` (:658). No `goto`, no further `return`. Lock ordering checked: `apply_mu → config_str_lock` and `apply_mu → speaker-g_lock → config_str_lock`; `config_str_lock` is recursive and nowhere wraps a call back into speaker/control — no inversion. |
| A6 – two comments | ✅ **CONFIRMED** | The config.c doctrine comment has been replaced (no longer contradicts config.h), control.c's `on_motion` now correctly uses fork+execlp. |

### Tier 2

| Item | Verdict | Evidence |
| --- | --- | --- |
| F-04 – qp/max_gop marked RESERVED instead of wired up | ✅ **CONFIRMED, a well-founded decision** | No HAL consumer (the audit's counter-check for F-04 confirms: `rcAttr.maxGop = v->gop`), wiring it up would need the rate-control overhaul from SDK gap #1 — not verifiable without hardware; the `motion.roi_*` analogy holds. The warning mechanism works: the set_kv video branch, `key+7` is correct for `video0.`/`video1.` (7 characters each), one-shot via `static`. **But → R-01.** |
| F-05 – 12 daynight keys in the example | ✅ **CONFIRMED** | All 12 present; example values match `config_defaults()` exactly (5/120/20/3600/43200, mode=sensor). |
| F-06 – adaptive_drop | ✅ **CONFIRMED** (code default 1, config.c:224) |
| F-07 – speaker block/aec/opus | ✅ **CONFIRMED** |
| F-08 – QA extensions | ✅ **CONFIRMED, the custom-block rationale holds** | The generic `lv_section` would indeed be wrong for the TIME/SUN path: `mode` is POSTed as `mode` but echoed back as `dn_mode` (a read-back mismatch), `time_*_start` are <=5-character HH:MM buffers (the 8-character `qa_probe` would truncate), lat/long are floats. Format compatibility hand-verified: `%g` emits `52.5`/`13.5` → string comparisons match; `T_DNMODE` parses/emits `sensor|time|sun` symmetrically. Interruption safety: `LV_PENDING` is armed **before** the probe POST and only disarmed after the restore has landed; the established `EXIT`/`INT`/`TERM` traps (`lv_restore_pending`, qa:834-836) apply — convention followed. The spk gate is real: `caps.audio` lists `spk_volume` only under `USE_PLAY||USE_BACKCHANNEL` (control.c AUD_CAPS). |
| F-09 – 4 clamps | ✅ **CONFIRMED** (0-1, 0-2, 0-1, 8000-96000) |
| F-10 – 3 documentation fixes | ✅ **CONFIRMED** (wiki 1-300, wiki KB, example 1024; the QA spec range updated to 1-300 to match) |

### Tier 3

| Item | Verdict |
| --- | --- |
| A3 – cmfc/cmf2 removed | ✅ **CONFIRMED** — a clean revert with a correct rationale in the comment; `box_close` recomputes the ftyp size anyway, no size bookkeeping affected. |

### Tier 4

| Item | Verdict | Evidence |
| --- | --- | --- |
| P-08 – FQ_MAX_BYTES only for T10/T20/T21 | ✅ **CONFIRMED via `make -n`** | T20: `-DFQ_MAX_BYTES=1048576` present; **T31: 0 matches, T41: 0 matches**; T21: 1 match; the T10 branch carries the line (Makefile:92). `fanqueue.c:14` has the `#ifndef` guard → the `-D` does not collide. `make sim` unaffected (no `$(PLATFORM_CFLAGS)` in the sim target). No regression for T31/C100/T40/T41. |
| P-03/P-04 – now cache + control-poll gate | ✅ **CONFIRMED** | The `polled` flag correctly guards the `errno` check: when unpolled, `n=-1, polled=0` → neither the `n==0` break nor the errno evaluation fires — the stale-errno break is structurally impossible; a dead connection is detected at the next poll (<=~150 ms), a live one is never cut off spuriously. Worst-case TEARDOWN latency is ~150 ms (the 50 ms gate plus `now` being up to `pop_ms`=100 ms stale, since it is read before the pop) — far under any RTSP keepalive window. The backchannel polls via `ctl_poll_every` every iteration; `s->have_bc` is referenced only under `#ifdef USE_BACKCHANNEL`. The TEARDOWN BYE timestamps correctly fetch a fresh `ms_now_us()` (`bnow`). |

### Skipped Items (Negative Verification)

| Item | Verdict |
| --- | --- |
| P-01 (`hub_publish_take`) | ✅ **cleanly NOT touched** — no diff in hub.c/frame.c, no API leftovers. The rationale (11 instead of 3 call sites, UAF risk without a hardware soak) is plausible and matches the audit's "medium" rating. |
| P-02 (stop condvar) | ✅ cleanly not touched (ranked behind P-01 in the audit itself). |
| SDK #5/#6 | ✅ cleanly not touched — no `GetChnEvalInfo`/`SetAe_IT_MAX` code, no new `PLATFORM_` conditionals in the diff. |

---

## New Findings (introduced by, or made visible through, the fix)

| ID | Severity | Finding |
| --- | --- | --- |
| R-01 | 🟢 LOW | **The example contradicts the new F-04 warning.** `timps.conf.example:221/223` still ships `video0.max_gop = 60` and `video0.qp = 35` as active example lines. Since the new one-shot warning also fires on **file load** (set_kv is the shared path), any config derived from the example logs "reserved and IGNORED" on every boot. Only the wiki was marked RESERVED, not the example. *Fix (follow-up):* comment out both lines in the example, or annotate them with "reserved, no effect". Not a merge blocker (the warning is factually correct and fires exactly once per process). |
| R-02 | ⚪ INFO | **The QA `dir` probe uses a relative path.** `lv_section record ... "dir str"` POSTs `qa_probe` as the live `record.dir`/`timelapse.dir`. If an active recording is mid segment-rotation on the QA device at that moment, `/qa_probe/<host>/records/...` would briefly appear on the rootfs (daemon cwd `/`). The window is small (POST→GET→restore), rotation happens only at a keyframe — practically unlikely, but an absolute `/tmp/...` probe value would be the cleaner choice. |
| R-03 | ⚪ INFO | **New lock cadence in the recorder:** the per-pass snapshot takes `config_str_lock` once per loop iteration, roughly once per packet (25-75/s). Hold time is a struct copy (sub-us), contention only during /control writes — matches the perf audit's P-09 assessment exactly, no action needed. Documented only so a later profiler finding does not come as a surprise. |
| R-04 | ⚪ INFO | `hal_ingenic.c:2490-2491` (`speaker_set_volume(g_hcfg->audio.spk_volume)` in the apply path) still reads lock-free — this is now **correct**, because `apply_mu` serializes apply-vs-apply and the reader is thus the very same thread that just wrote the value (no more concurrent writer). A short comment at that spot ("safe: serialized by apply_mu") would future-proof this against later audits. |
| R-05 | ⚪ INFO | The QA TIME/SUN probe can trigger a one-time IR-cut toggle on a real camera (mode=sun with Berlin coordinates, depending on time of day), which swings back on restore. This is exactly the coverage the audit (F-08) explicitly requested; transient and accepted. |

Worth highlighting positively (correct beyond what the audits asked for): the pre-roll warning in `rec_thread` now reads `bitrate_kbps`/`fps` from `g_cfg_boot` instead of live — the right source for restart-only keys (the warning reflects what the encoder actually produces) and race-free as a side effect; not even mentioned in the Opus report.

---

## Recommendation

**Merge.** R-01 as a mini follow-up (comment out two example lines) right after, or in the next documentation pass; R-02/R-04 as capacity allows. Before the next release, a QA run against a real camera as usual (section 8b, including the new TIME/SUN and spk blocks), since `imp_osd.c`/`hal_ingenic.c`/`speaker.c` were, by their nature, only syntax-checked here.
