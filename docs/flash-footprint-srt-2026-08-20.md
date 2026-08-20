# Making SRT fit: where 765952 bytes came from

Date: 2026-08-20. Board: wuuk_y0510_t31x_sc4336p_ssv6158 at 192.168.15.190
("garage", the designated test camera). Partition `mtd4` = 5242880 bytes.

> ## ⚠ READ THIS FIRST: the 5 MB limit this document optimises against is not real
>
> **The rootfs partition is not a fixed size. It is cut to fit the image at
> U-Boot build time**, by `THINGINO_PATCH_DEV_ENV` in
> `package/thingino-uboot/thingino-uboot.mk`:
>
> ```
> ROOTFS_SIZE_ALIGNED = round_up(size(rootfs.squashfs), 65536)
> DATA_SIZE_KB        = FLASH_SIZE_KB - ROOTFS_OFFSET_KB - ROOTFS_SIZE_KB
> MTDPARTS            = ...,${ROOTFS_SIZE_KB}k(rootfs),${DATA_SIZE_KB}k(data),...
> ```
>
> The wuuk carries an **FM25Q128A, 16 MB** (`FLASH_SIZE_MB=16` in its
> defconfig; `mtd6 "all" = 0x1000000`). Its 5120k rootfs partition is not a
> hardware limit — it is the imprint of whatever image was on the camera the
> last time a **full** flash laid the partitions down. Beside it sits a 9280k
> data partition using 408 KB, 4% of itself.
>
> `make ota-rootfs` writes into the *existing* partition, so it fails once the
> new image outgrows the old one's imprint. `make ota` (mode `all`) writes
> boot, env, kernel, rootfs and data together and installs the recomputed
> layout. **That is the fix for "image does not fit", not shrinking the image.**
>
> Everything below was measured against a boundary that is itself a build
> output. The engineering findings stand on their own — the static C++ runtime,
> the libsrt section flags, `libaudioProcess-neo`, the block-granularity
> result, the `libgcc_s` dlopen trap. The *motivation* does not: several hours
> were spent giving up features (backchannel, speaker, rotation, all sound
> files) to stay under a number nobody had checked the origin of.
>
> The warning sign was visible early and misread: a fleet-wide survey that
> morning showed leftovers of 4096, 20480 and 45056 bytes on all twelve
> cameras. That is not "the fleet is tight everywhere" — it is *every partition
> being cut to its image*, which should have prompted the question of where the
> boundary comes from.
>
> **Before optimising an image for size, check what created the partition it
> has to fit.**

SRT had been switched off on this camera on 2026-08-19 because the image no
longer fitted. This documents how it was made to fit again with 11.7% to
spare, which of the obvious levers worked, and which did not.

## Result

| step | rootfs.squashfs | vs partition | saved |
|---|---:|---:|---:|
| SRT on, full feature set, stock libsrt | 5394432 | −151552 | — |
| − `TIMPS_ROTATE`/`SW_ROTATE`/`BACKCHANNEL`/`BC_AAC`/`PLAY` | 5386240 | −143360 | 8192 |
| + libsrt built with `-ffunction-sections -fdata-sections` | 5357568 | −114688 | 28672 |
| + 0-byte sound stubs in the device overlay | 5210112 | **+32768** | 147456 |
| + static C++ runtime, `libstdc++`/`libaudioProcess` stubbed | 4628480 | **+614400** | 581632 |

Total: **765952 bytes**, of which 76% came from the last step alone — and that
step was believed to give up no functionality; it does — see "…and why that
conclusion was wrong". It costs AEC, noise suppression, AGC and the high-pass
filter, all of which are off by default.

Once the C++ runtime finding was in, the two features that had been given up
were bought back:

| final configuration | rootfs.squashfs | free |
|---|---:|---:|
| SRT + rotation + backchannel + speaker + sounds + ONVIF + TLS | **4784128** | **458752 (8.7%)** |

That is the full feature set with SRT on and 8.7% headroom. The step that made
it possible cost nothing; the two steps that cost functionality
(8192 + 147456 bytes) were reverted.

## What each feature actually costs

Measured one build per variant against a single baseline, with the control
build reproducing the baseline byte for byte (`timpsd` 1294976, `.text`
1199295). `.text` is quoted because it is the only figure with enough
resolution — see the block-granularity note below.

| build flag | `.text` | `timpsd` | packed image |
|---|---:|---:|---:|
| `TIMPS_SRT` | **921593** | 1008392 | **278528** |
| `TIMPS_PLAY` | 7240 | 4136 | 0 |
| `TIMPS_BACKCHANNEL` (without BC_AAC) | 4262 | — | 0 |
| `TIMPS_BC_AAC` | 903 | 20 | 0 |
| `TIMPS_ROTATE` | 1075 | 4 | 0 |
| `TIMPS_SW_ROTATE` | 0 | 0 | 0 |
| the four above, together | 13480 | — | 0 |

Non-flag levers, for comparison:

| lever | packed image |
|---|---:|
| static C++ runtime + `libstdc++`/`libaudioProcess` stubs | **581632** |
| libsrt `-ffunction-sections -fdata-sections` | 28672 |
| sound files replaced by 0-byte stubs | 147456 |

### Turning these features off is not a size lever

squashfs rounds to 4096-byte blocks. Every one of the four small features
costs less than a block, so switching them off changes the packed image by
**zero bytes** — measured, not inferred: four variants with visibly different
`USE_*` build flags produced byte-identical 4784128-byte images.

This retroactively corrects the "8192 bytes for the five `TIMPS_*` switches"
figure quoted earlier in this document's step table. That 8192 was two blocks
of rounding difference between two builds, not the sum of what the features
cost. The one feature large enough to matter is SRT itself, and switching SRT
off defeats the purpose.

The practical consequence: the hours spent disabling rotation, backchannel and
the speaker bought nothing. The 765952 bytes came from the two changes that
were thought to be free (610304 of them — 28672 of that genuinely is; the
581632 costs the audio filters) and from the sound files
(147456), and even the sound files were put back once the C++ runtime finding
landed.

## Method rules that this exercise re-confirmed

1. **Measure by packing, never by arithmetic.** A `gzip` estimate put
   `/var/www/onvif` at 122 KB; squashfs actually packed it far smaller, and
   the whole ONVIF line of attack was worth nothing. `du -sk` is worse still:
   it reports block usage, which made 194 small XML files look like 3644 KB.
2. **Compare two builds of the same source state.** The first figure quoted
   for the three timps features was 24576 B, derived against a baseline from
   an earlier session with different code. Re-measured against its own
   baseline it is 8192 B — a third of the claim.
3. **`rm` in `$(TARGET_DIR)` is undone by `target-finalize`**, which re-applies
   per-package trees. This cuts both ways: it silently restored the real
   `libgcc_s.so.1` after its overlay stub was withdrawn, which is why the
   corrected image was sound.
4. **Below 4096 bytes, the packed image cannot see the change.** squashfs
   rounds to blocks, so a feature worth a few KB of `.text` is invisible in
   `rootfs.squashfs`. Measure such things on the binary (`size`), and do not
   read a difference of one or two blocks as a feature's cost.
5. **A control run only validates a series that actually varied.** The first
   attempt at the per-feature series wrote its variant lines with
   `printf '%s%s\n' "$BASE" "$LINE"`, where `BASE=$(python3 ...)` had its
   trailing newline stripped by command substitution - so every variant line
   was glued onto the end of the previous line, inside a comment. Seven builds
   of the identical config, and the control run dutifully confirmed a return
   to a baseline it had never left. The fix that makes this self-checking is
   to record the resulting `USE_*` build flags on every row.
6. **`BR2_ROOTFS_OVERLAY` is the per-device lever.** For this board it lists
   the device directory last, so 0-byte stubs under
   `user/<camera>/<ip>/overlay/` affect exactly one camera. A
   `TARGET_FINALIZE_HOOK` in a package `.mk` would hit the whole fleet.

## What was changed, and where

Exactly **one Buildroot package** was touched. Everything else is either the
timps source tree or per-device configuration, which are not packages.

| what | file | repo / tree | scope |
|---|---|---|---|
| libsrt section flags | `package/libsrt/libsrt.mk` | firmware, **both** `piuma` and `.ciao-wt` (`ciao`) | every camera that builds SRT |
| static C++ runtime | `Makefile` | **timps** source repo | every SRT build |
| five `TIMPS_*` switches | `user/wuuk_y0510_t31x_sc4336p_ssv6158/192.168.15.190/local.fragment` | firmware (shared, see below) | this camera |
| sound + library stubs | `user/wuuk_y0510_t31x_sc4336p_ssv6158/192.168.15.190/overlay/` | firmware (shared) | this camera |

No other package `.mk`, `Config.in` or hook was modified for this work. In
particular `package/timps/timps.mk` was **not** touched: its
`TARGET_FINALIZE_HOOKS` purge is shared by the whole fleet, which is why the
sound removal was moved out of it and into the device overlay.

### The two-worktree trap

The firmware checkout carries a second git worktree, `.ciao-wt`, on branch
`ciao`, and **that is the tree the build runs from**. The two differ in a way
that matters:

```
.ciao-wt/package/   own directory, per branch   -> must be edited separately
.ciao-wt/user  ->  ../user   (symlink)          -> shared, edit once
```

So fragments and overlays written in the main tree take effect immediately,
while a package `.mk` edited only in the main tree does nothing. The first
attempt at the libsrt change was made in `piuma` alone and produced a
byte-identical `libsrt.a` (1060438 both times) — the flags never reached the
compiler. Both copies now carry the same two-line change.

### The libsrt change

```make
# package/libsrt/libsrt.mk
-DCMAKE_C_FLAGS="$(TARGET_CFLAGS) -ffunction-sections -fdata-sections"
-DCMAKE_CXX_FLAGS="$(TARGET_CXXFLAGS) -ffunction-sections -fdata-sections"
```

`ENABLE_LOGGING=OFF`, `ENABLE_APPS=OFF`, `ENABLE_TESTING=OFF` and
`ENABLE_SHARED=OFF` were already set and needed no change.

### The timps change

```make
ifeq ($(USE_SRT),1)
LINK_DRV := $(CXX)
CXXRT_LDFLAGS := -static-libstdc++ -static-libgcc
else
LINK_DRV := $(CC)
CXXRT_LDFLAGS :=
endif
```

plus `$(CXXRT_LDFLAGS)` on the link line, between `$(LDFLAGS)` and
`$(IMP_LIB)`.

## The levers, in detail

### 1. libsrt with section granularity (28672 B, fleet-wide, no functional cost)

`timps` already compiled its own sources with `-ffunction-sections
-fdata-sections` and linked with `-Wl,--gc-sections` (`Makefile`). But
`libsrt.a` was built by Buildroot without those flags, so the linker could
only discard whole object files, never individual functions — and timps uses
a small slice of libsrt's C API.

```make
# package/libsrt/libsrt.mk
-DCMAKE_C_FLAGS="$(TARGET_CFLAGS) -ffunction-sections -fdata-sections"
-DCMAKE_CXX_FLAGS="$(TARGET_CXXFLAGS) -ffunction-sections -fdata-sections"
```

`libsrt.a` grows (1060438 → 1253430 bytes; per-function sections cost archive
overhead) while the linked binary shrinks. `ENABLE_LOGGING=OFF` was already
set and needed no change.

Note: the firmware tree used for building is the `.ciao-wt` git worktree,
which has its own `package/` directory. Editing the main tree's copy has no
effect on the build — the first attempt at this change was byte-identical
for exactly that reason.

### 2. Static C++ runtime (581632 B — costs the four audio filters)

`libstdc++.so.6.0.35` was the single largest file in the image — 2130 KB in
the target, more than `libimp.so` (1175 KB), the ISP driver (1102 KB) or
busybox (845 KB), and over three times `timpsd` itself.

It had exactly two consumers:

* `libaudioProcess.so` (553 KB in the target), which is **not linked by
  anything** and was not mapped in any process on the running camera — it
  belongs to the backchannel echo-cancellation path;
* `timpsd`, because libsrt is C++.

The `Makefile` carried this comment:

> LINK the final binary with g++ so libstdc++ is resolved and static-linked
> (-static-libstdc++, passed via IMPLIBS) reliably.

`-static-libstdc++` appeared **only in that comment**. `IMPLIBS` was
`-l:libimp.a -l:libalog.a -l:libsysutils.a`, and the firmware build overrides
it anyway. The intent was documented and never implemented. Fix:

```make
ifeq ($(USE_SRT),1)
LINK_DRV := $(CXX)
CXXRT_LDFLAGS := -static-libstdc++ -static-libgcc
else
LINK_DRV := $(CC)
CXXRT_LDFLAGS :=
endif
```

plus `$(CXXRT_LDFLAGS)` on the link line. `timpsd` grows 683744 → 1278396
(+594652) and drops `libstdc++.so.6` from its `NEEDED` list. With
`libaudioProcess.so` stubbed as well, both libraries leave the image.

### 3. Sound stubs (147456 B, device-only, costs the sound files)

`TIMPS_PLAY` is off on this camera, so no sound can be played. The 16 `.ulaw`
files (252854 bytes) are dead weight. They were replaced with 0-byte stubs in
`user/<camera>/<ip>/overlay/usr/share/sounds/`, both `.ulaw` and `.opus`
names — the format is a Kconfig `choice` that flips to Opus once `TIMPS_PLAY`
is off, and stale files from the previous format remain in the target.

This is the same mechanism already used fleet-wide in
`user/common/overlay/usr/share/sounds/` for the portal announcements.

### 4. Five timps features (0 B — this lever does not exist)

`TIMPS_ROTATE`, `SW_ROTATE`, `BACKCHANNEL`, `BC_AAC`, `PLAY` disabled in the
device fragment. Quoted at 8192 bytes in the step table above, because that
was the difference between the two builds that happened to bracket the change.
Measured one flag at a time it is **zero** bytes of packed image — the whole
group is 13480 bytes of `.text`, well under one squashfs block. See "What each
feature actually costs".

This was the worst move of the exercise: two-way audio and the speaker were
given up, with the user's explicit sign-off, for nothing. Reverted; the
shipping image has all five back.

### 5. libaudioProcess-neo — the answer to the AEC trade (user's find)

`package/libaudioprocess-neo` was already in the firmware tree, fully
packaged, and selected by nobody. It is a clean-room pure-C11 drop-in for
Ingenic's proprietary `libaudioProcess.so` (MIT, gtxaspec), and
`ingenic-lib/Config.in` already carries
`depends on !BR2_PACKAGE_LIBAUDIOPROCESS_NEO`, so the two exclude each other
cleanly. Enabling it needs one line in the device fragment.

| | proprietary | 0-byte stub | **neo** |
|---|---:|---:|---:|
| `libaudioProcess.so` | 671472 | 0 (broken) | **75056** |
| needs `libstdc++` | yes | — | **no, libc only** |
| AEC | works | silently off | works |
| exported functions | 1574 | — | 70 |
| loopback tone band | −21.3 dB | −5.0 dB | **−22.8 dB** |
| packed image | does not fit | 4784128 | **4816896** |

With neo in place **nothing in the image links `libstdc++` at all** — removing
it stops being a trick and becomes the removal of a library with zero
consumers. All four audio filters cost **32768 bytes** instead of 581632, and
`IMP_AI_EnableAec` reports `AEC enabled (AI 0/0 <- AO 0/0)` with the echo
canceller measurably attenuating the acoustic loopback, within 1.5 dB of the
vendor library.

Caveat: 70 exported functions against the original's 1574, and the upstream
repo has 19 commits, one star, one fork. Verified working on this camera; not
something to push to eleven others without a stretch of real service behind
it. The QA acoustic loopback (`--test-backchannel`) is the right acceptance
criterion — it measures whether the filters actually do anything.

## Levers that did not work

* **`# BR2_PACKAGE_THINGINO_SOUNDS is not set`** in the device fragment is
  accepted by the merge (no override warning) and then pulled back to `y` by
  kconfig's own resolution. No `select` from an enabled symbol was found to
  explain it; the cause is unresolved. Use the overlay stubs instead.
* **`# BR2_PACKAGE_THINGINO_ONVIF is not set`** likewise does not take, while
  the identical construction works for every `BR2_PACKAGE_TIMPS_*` symbol.
  ONVIF is wanted on this board anyway (it has motors; `wsd_simple_server`
  and `onvif_notify_server` run and an NVR pans through them).
* **`BR2_PACKAGE_THINGINO_KOPT_IPV6`** is a kernel option and the kernel lives
  in its own mtd partition. For the rootfs it removes only `sit.ko` (22 KB
  raw), and it needs a full kernel rebuild. Not worth it. The camera has no
  global IPv6 address and no IPv6 default route.
* **Unloaded kernel modules** (`sit`, `pwm_hal`, `pwm_core`, `jz-aes`) total
  51 KB raw and can only be removed through a shared package hook.

## The trap: libgcc_s.so.1 is dlopen'd, not linked

`libgcc_s.so.1` was stubbed too, on the strength of a `readelf -d` sweep that
found no consumer once `-static-libgcc` was in place. The camera booted and
`timpsd` died silently after audio init:

```
libgcc_s.so.1 must be installed for pthread_cancel to work
*** timpsd FATAL: SIGABRT si_addr=0x000006f2 pc=0x777daeb0
```

uClibc **dlopens** `libgcc_s.so.1` for `pthread_cancel` stack unwinding. There
is no `DT_NEEDED` entry, so a link-time dependency scan cannot see it, and
`-static-libgcc` does not help because the loader is libc, not timpsd.

The lesson generalises: a `readelf -d` sweep proves nothing about runtime
loading. The check that would have caught it is the one already used for
`libaudioProcess` — `grep -l <lib> /proc/*/maps` on a running camera — and it
was simply not repeated for this library.

`libgcc_s.so.1` (200320 B in the target) stays. Its stub was withdrawn and
`target-finalize` restored the real file.

## Verification performed on the camera

* `timpsd` running, RTSP 554 and HTTP 8880 listening;
* `/snapshot.jpg` → HTTP 200, 192723 bytes, valid JPEG header;
* daynight setting night references and emitting its learned line;
* `/control` reports `last_errors`, `queue_drops`, `write_errors`;
* SRT: `srt.enabled = 1` in `/etc/timps.conf`, UDP 9000 bound, listener mode;
* the SRT stream pulled from the host with ffprobe carries **H.264 1920x1080
  plus AAC 16000 Hz mono** — the first on-device verification of the TS mux,
  which `src/srt.c` had flagged as untested;
* 12 s of that audio decoded to 192009 samples, mean −48.1 dB / peak
  −35.2 dB, i.e. real room noise rather than digital silence. Not listened to;
  correctness of pitch, rate and lip-sync is unverified.

### Two-way audio with both libraries stubbed

**This section reached the wrong conclusion at first. Read to the end.**

The open question — whether echo cancellation dlopens `libaudioProcess.so`,
which would drag `libstdc++` back into the image and cost roughly 600 KB of
the saving — was first tested with `scripts/timps-qa.sh --only backchannel
--test-backchannel`, run against the camera while
`grep -l 'audioProcess\|libstdc' /proc/*/maps` sampled twice a second:

| check | result |
|---|---|
| 2c ONVIF backchannel handshake (SDP trackID=2, SETUP+PLAY, 2 s PCMU tone) | PASS |
| 2c camera received the audio (speaker owner acquired, native IMP_AO) | PASS |
| 2d acoustic loopback: tone band −5.0 dB vs control band −35.6 dB, delta 30.6 dB (threshold 12) | PASS |
| `libaudioProcess.so` / `libstdc++.so` mapped at any point | **never** |

The 2d test is the strong one: a 1500 Hz tone was pushed into the camera over
the backchannel, played through the speaker, and picked up acoustically by the
camera's own microphone in timps' outgoing stream. The full duplex path ran
with both libraries present only as 0-byte stubs. timps drives the speaker
through `IMP_AO` directly, as its own startup line says
("audio backchannel enabled (codec=0 rate=16000, native IMP_AO)").

### …and why that conclusion was wrong

The run above proves only that **disabled** filters load no library.
`audio.aec`, `audio.ns`, `audio.agc` and `audio.high_pass` all default to 0
(`config.c`: "AEC opt-in, default off"), and `libaudioProcess.so` is exactly
the vendor library behind them — `hal_ingenic.c` says so itself, warning that
`IMP_AI_EnableAgc/Ns/Aec` race the vendor thread "-> UAF/SIGSEGV in
libaudioProcess.so".

Repeated with all four enabled (`aec=1 ns=2 agc=1 high_pass=1`, confirmed
parsed via `/control`), and then once more as a control with the real
libraries copied back onto the camera:

| libraries | AEC log line | `libaudioProcess` mapped | loopback tone band |
|---|---|---|---|
| 0-byte stubs | `IMP_AI_EnableAec failed - continuing without echo cancellation` | no | −5.0 dB |
| real | `AEC enabled (AI 0/0 <- AO 0/0)` | **yes** | −21.3 dB |

The stub silently disables echo cancellation, noise suppression, AGC and the
high-pass filter. Nothing fails loudly: `IMP_AI_EnableAec` returns an error,
timps logs one WARN and carries on. The 16.3 dB drop in the loopback tone band
with the real library is the echo canceller doing its job — measurable proof
that the feature was actually lost, not merely unreported.

**So the 581632 bytes are a trade, not a free win.** They cost the four audio
filters. And on this board the two cannot coexist:

```
image with the stubs   4784128   (458752 free)
image without them     5365760   (122880 OVER the 5242880 partition)
```

SRT and the audio filters are mutually exclusive on a 5 MB rootfs. All four
filters are off by default, so the shipping configuration loses nothing it was
using — but that is a default, not an absence, and enabling any of them on a
stubbed image produces one WARN and silently no filtering.

The `-static-libstdc++` change itself remains correct and free: timpsd no
longer needs the shared library. But *removing* `libstdc++.so` from the image
requires removing `libaudioProcess.so` too, and that is where the cost sits.

Note that `audio.backchannel` in `/etc/timps.conf` is a separate runtime
switch from the `BACKCHANNEL` build flag; it was still off from the previous
image, and the first QA run reported "no backchannel advertised" because of
it, not because of the stubs.

The same applies to `srt.enabled`, and with a sharper edge: **flashing the
rootfs replaces `/etc/timps.conf`**, so any runtime switch set by hand on the
camera is gone after the next `ota-rootfs`. SRT was enabled by hand and
verified working, then silently reset to `0` by the following flash. Compiled
in and available is not the same as running - check `srt.enabled` and whether
UDP 9000 is bound, not just `"available":1` in `/control`.

Note that the version string is `v1.9.0-16-g98ca82e`, identical to the fleet:
none of this work is committed, so **the version cannot distinguish this
build**. Verify by binary size (1278396) or by the SRT block in `/control`.

## What is actually in place now

`U` below is `user/wuuk_y0510_t31x_sc4336p_ssv6158/192.168.15.190`.

Kept, fleet-wide (the libsrt flags are free; the static link only pays off if
the two libraries are also removed, which costs the audio filters):
* `package/libsrt/libsrt.mk` section flags — **in both worktrees**;
* `-static-libstdc++ -static-libgcc` in timps' `Makefile` for `USE_SRT=1`.

Kept, this camera only:
* `BR2_PACKAGE_LIBAUDIOPROCESS_NEO=y` in `$U/local.fragment`
* `$U/overlay/usr/lib/libstdc++.so.6.0.35` — 0 bytes, now with **zero**
  consumers in the image rather than one
* `$U/STUBS.txt` — why, and what to restore first

Withdrawn: the `libaudioProcess.so` stub. neo ships a real 75056-byte
library, so there is nothing to stub.

Applied and then **withdrawn again**, once the C++ runtime finding made them
unnecessary — they are no longer in the tree:
* the sound stubs under `$U/overlay/usr/share/sounds/` (147456 bytes, but the
  camera loses every sound file);
* the five `# BR2_PACKAGE_TIMPS_* is not set` lines in `$U/local.fragment`
  (measured at 0 bytes of packed image — see "What each feature actually
  costs").

The stub run also left 19 zero-byte orphans in `$(TARGET_DIR)/usr/share/sounds`
— files for sounds this configuration never installs (`doorbell_*`,
`wireguardvpnis*`, every `.opus`). They cost nothing packed but turn
`play chime_1.opus` into a silent no-op instead of an error, and
`thingino-button` calls exactly that. They were deleted; the five legitimate
stubs from `user/common/overlay` remain.

Do **not** stub `libgcc_s.so.1`.

## Open

* Whether to keep the `libaudioProcess`/`libstdc++` stubs on this camera at
  all. They buy SRT its headroom and cost AEC/NS/AGC/high-pass, which are off
  by default. Anyone enabling an audio filter on a stubbed image gets one WARN
  and no filtering.
* Why `BR2_PACKAGE_THINGINO_*` symbols resist `is not set` in a device
  fragment while `BR2_PACKAGE_TIMPS_*` symbols do not.
