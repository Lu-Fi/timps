# timps — Developer and test notes

Applies to timps v1.12.1 (source: `main`, 2026-11-16).

**Audience: an AI assistant or a developer changing timps itself.** For
answering user questions read `reference.md` first; for build basics
(`make sim`, cross-compiling, every `USE_*` flag) see
[`docs/wiki/Building.md`](../wiki/Building.md). This file covers what those
leave out: how to test a change against both libimp stacks, on a camera,
without flashing, and the vendor-library behaviour that has caught us.

## 1. Two libimp stacks, one source

A `timpsd` binary is built for one stack:

| Build | Links against | `--check-libs` says |
|---|---|---|
| `USE_OPENIMP=0` (default) | Ingenic's `libimp.so` + `libalog` + `libsysutils` | `vendor build` |
| `USE_OPENIMP=1` | OpenIMP's `libimp.so` | `OpenIMP build` |

`timpsd --check-libs` exits 78 when the binary and the installed libimp are
from different stacks and 79 when a required function is missing
(`reference.md` §11). Some features are gated at run time on OpenIMP marker
symbols (`OpenIMP_Cap_*`, weak imports, list in
`scripts/check-weak-imports.sh`), so a vendor build and an OpenIMP build of the
same commit can report different `caps`.

A change in `src/hal/` that touches IMP calls needs a test on both stacks
whenever the SoC has both (state per SoC: `docs/wiki/Platform-SDK-Support.md`).

## 2. Host checks before any camera

- `make sim` and the host tests: `make test-config`, `test-auth`,
  `test-http-sim`, `test-fmp4`, `test-hub-*`, `test-srtp`, `test-stun`,
  `test-roi`, `test-fs-rot`, `test-libimp-check`, `test-daynight-*`,
  `test-dn-replay` (day/night replay corpus; timing-sensitive on a busy host,
  rerun a failing family alone before believing it).
- `scripts/check-config-keys.sh` after adding or renaming a key,
  `scripts/check-weak-imports.sh` after adding an OpenIMP marker.
- Size: report the `timpsd` size before/after for every feature
  (`size timpsd` on the stripped target build).

## 3. A test binary without a firmware image

The simplest toolchain is an existing thingino build output for the camera:
use its `per-package/timps/host/bin/mipsel-linux-` as `CROSS_COMPILE`, its
sysroot `.../host/mipsel-buildroot-linux-uclibc/sysroot/usr/lib` as
`IMP_LIB`, and copy the remaining flags from the `timps-custom target` line in
that build's log (`-DPLATFORM_<SOC>`, `-DKERNEL_VERSION_4` on T40/T41,
`-DLIBC_UCLIBC`, `LIBS`). A tree from `git archive` has an empty `include/`
submodule: pass `INC_ROOT=<checkout>/include`. Give the build a distinct
`VERSION=` so the binary can be identified (`timpsd -V`, the
`[MAIN] timps ... starting` log line).

The output dir decides the stack: an output built with the proprietary ISP
(`BR2_PACKAGE_THINGINO_ISP_PROPRIETARY=y`) has the vendor libimp in its
sysroot, an open-stack output has OpenIMP.

## 4. Running it on a camera without flashing

1. Stop the installed streamer: `/etc/init.d/S95timps stop`.
2. Copy the binary to `/tmp` and start it with a config copy:
   `nohup /tmp/timpsd -c /tmp/t.conf > /tmp/t.log 2>&1 < /dev/null &`.
   Append test overrides to a copy of `/etc/timps.conf` instead of editing it.
3. Restore: kill the test binary, `/etc/init.d/S95timps start`, check
   `/snapshot.jpg` returns 200.

Never run `timpsd -v` to get a version: `-v` is verbose and starts a full
second streamer. The version flag is `-V`.

**The other stack on the same camera.** The libimp must match the ISP kernel
driver: the vendor libimp needs the vendor `tx-isp-<soc>.ko`, OpenIMP needs
the driver it was built for. To run a vendor build on an open-stack camera:
copy the vendor `libimp.so`, `libalog.so`, `libsysutils.so` to `/tmp/vlib`,
unload the sensor module and the ISP module, load the vendor ISP and sensor
modules (same module parameters as the image uses), then start the test
binary with `LD_LIBRARY_PATH=/tmp/vlib`. Check the mapping with
`LD_TRACE_LOADED_OBJECTS=1 LD_LIBRARY_PATH=/tmp/vlib /tmp/timpsd | grep libimp`.

- Unloading a vendor ISP module can oops on some SoCs (seen on T40): restore
  with a reboot instead of `rmmod`.
- This swaps video only. Swapping the audio module too did not work on T31
  (`IMP_AI_Enable` failed on both devices); for vendor audio tests use an
  image built with the proprietary stack.
- Such a run has no day/night board control (IR-cut, IR LEDs): at night the
  picture is IR-tinted (purple). The vendor ISP also prints a stack dump
  `tisp_netlink_init: create netlink socket error` at every start; it is
  harmless.

## 5. Measuring audio over RTSP

```bash
timeout -k 5 45 ffmpeg -nostdin -timeout 10000000 -rtsp_transport tcp \
  -i rtsp://<user>:<pass>@<ip>/ch1 -vn -t 20 -ac 1 -ar 8000 -f s16le out.pcm
```

- `-timeout` is the RTSP socket timeout in microseconds; `-rw_timeout` is
  not accepted for RTSP. ffmpeg keeps waiting on a stream whose audio never
  starts and ignores SIGTERM there, so use `timeout -k`.
- With `audio.codec = pcmu` the floor below about -60 dBFS is lost in G.711
  quantisation. Compare relative changes, and look at the DC offset (a working
  high-pass removes it).
- The QA script (`scripts/timps-qa.sh`) plays sound in section 2c (a short
  backchannel tone, on by default) and 2d (`--test-backchannel`), and opens
  the speaker in 2e. Use `--only` without these when sound must be avoided.

## 6. Vendor libimp behaviour that matters for timps

- **AI device binding (Lu-Fi/openimp#591, #605, #612).** In the vendor
  libimp of T31, T40 1.3.1 and T41 1.2.5, `IMP_AI_EnableNs`, `EnableAgc`,
  `EnableHpf` and `EnableHs` take no device argument and act only on AI
  device 1; on device 0 they log "Ai device is no enabled" and still return 0.
  timps records on device 0, so with the vendor libimp `audio.ns`,
  `audio.agc` and `audio.high_pass` have no effect. Measured with timps
  recording on device 1 (2026-11-15/16, G.711 captures):
  - T40 (vendor 1.3.1): device 0 unchanged by every combination; device 1
    changed only with HPF and NS both on (DC offset removed, about 21 dB less
    noise). HPF alone or NS alone: no visible change.
  - T31X (vendor image): device 0 unchanged by every combination; device 1:
    HPF alone no change, HPF + NS removed the DC offset, and **NS without HPF
    delivered no audio frames at all** (reproduced twice; RTSP clients wait
    for audio forever).
  OpenIMP applies the algorithms to the recording device, so device 0 works
  there. Since the next release timps records on device 1 for vendor builds
  of T31/T40/T41 (`AI_DEV` in `src/hal/hal_ingenic.c`) and switches HPF on
  whenever NS is on. Other vendor SoCs (T10-T30, T23, T32/T33) are not
  verified and stay on device 0.
- `IMP_ISP_Tuning_GetAeExpList` hangs on the vendor T40 driver: do not call it.

## 7. Day/night tests

- `image.running_mode` switches the ISP only; IR-cut and IR LEDs belong to
  the board scripts. Forcing day in real darkness gives an IR-tinted picture.
  Compare `ircut status` / `color status` with `running_mode` first.
- Judge engine changes by behaviour (minutes in the wrong mode, switches,
  no flapping) over the replay corpus and real traces
  (`daynight.trace_path`), not by log text.
