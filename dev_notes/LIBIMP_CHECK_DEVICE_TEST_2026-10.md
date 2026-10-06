# libimp check: device test procedure (2026-10, pending)

Status: the start-up libimp check (`src/hal/libimp_check.c`, exit 78/79) and
`timpsd --check-libs` (0/78/79/80) are **host-verified only**:
`make test-libimp-check` (decision table + dlopen glue on glibc),
`scripts/test_s95timps_preflight.sh` (init script, sh + busybox sh),
`scripts/check-libimp-required.sh` against eight vendor libimps and the IMP
headers, and the OpenIMP marker in OpenIMP's `libimp.map`. Not yet run on a
camera. What the device run has to show:

* no false 78/79 on a matching pair (that would leave the camera without a
  streamer after the update: S95timps refuses to start it);
* a real 78 on a mismatched pair;
* the daemon path with `RTLD_NOLOAD` on uClibc-ng does not log
  `libimp check skipped` (if it does, the check is ineffective there, but the
  camera still starts).

## Binaries

`/mnt/NVMe/git/scratch-builds/prerelease-fixed/devtest/` (built with
`EXTRA="USE_FAAC=0" build.sh devtest <flavour>` from the pre-release HEAD,
stripped; version string, size and md5 in `md5sums.txt` there). Without libfaac their `NEEDED` set is only `libimp.so`
(+ `libalog.so libsysutils.so` for vendor), mbedtls, `libuclibcshim.so`,
`libc.so.0`, so they load on both stacks' images regardless of the libfaac
soname:

| File | Build |
| --- | --- |
| `timpsd-t23o` | T23, OpenIMP |
| `timpsd-t23v` | T23, vendor libimp 1.3.0 |
| `timpsd-t31o` | T31, OpenIMP |
| `timpsd-t31v` | T31, vendor libimp 1.1.6 |

`--check-libs` reads no config, takes no lock and makes no IMP call, so it is
safe beside the running daemon. The copies go to `/tmp` (tmpfs, ~410 KB
each) and are deleted afterwards.

## Cameras

* **A**: a T23 on the open stack (OpenIMP).
* **B**: a T31 on the vendor libimp (image with `BR2_PACKAGE_INGENIC_LIB_LIBIMP`).

Confirm the stack first (busybox grep stops at NUL bytes, hence `tr`):

```sh
tr -d '\000' </usr/lib/libimp.so | grep -c OpenIMP_P0_GetState   # >0 OpenIMP, 0 vendor
ls /usr/lib/libalog.so /usr/lib/libsysutils.so                    # needed by the vendor copies
```

Copy a binary (works without an sftp server on the camera):

```sh
cat timpsd-t31o | ssh root@<cam> 'cat >/tmp/timpsd-x && chmod +x /tmp/timpsd-x'
```

## 1. Installed binary

```sh
timpsd -V
tr -d '\000' </usr/bin/timpsd | grep -c -- --check-libs     # >=1: the preflight asks it
timpsd --check-libs; echo "rc=$?"
```

Expected, A: `timpsd: libimp OK (OpenIMP build, /usr/lib/libimp.so)`, `rc=0`.
B: `timpsd: libimp OK (vendor build, /usr/lib/libimp.so)`, `rc=0`. The path is
what `dladdr` reports for the loaded copy. A firmware older than the check has
no `--check-libs` string: use step 2 instead.

## 2. Matching copy

A: `timpsd-t23o`, B: `timpsd-t31v`.

```sh
/tmp/timpsd-x --check-libs; echo "rc=$?"
```

Expected: the same `libimp OK` line, `rc=0`.

* `rc=79` (`libimp lacks <name> - … does not fit this timpsd`) or `rc=78` on
  a matching pair: **false positive, stop the release** and record the libimp
  version (`ls -l /usr/lib/libimp.so*`, md5) here.
* `rc=80` with `… - libimp check skipped`: `libimp.so` could not be opened by
  name or nothing resolved. The daemon would still start, but the check does
  nothing on this image; record the warning.

## 3. Mismatched copy

A: `timpsd-t23v` (vendor build on OpenIMP). Expected on stderr, `rc=78`:

```
timpsd: libimp mismatch: this timpsd is built for the Ingenic vendor libimp (USE_OPENIMP=0) but /usr/lib/libimp.so is OpenIMP - install the OpenIMP-flavour timpsd (USE_OPENIMP=1) or the vendor libimp
```

If the image lacks `libalog.so`/`libsysutils.so`, the loader fails before
`main()` instead (`can't load library 'libalog.so'`, non-zero, neither 78 nor
79): that is the documented case the check cannot see, not a result.

B: `timpsd-t31o` (OpenIMP build on vendor). Expected on stderr, `rc=78`:

```
timpsd: libimp mismatch: this timpsd is built for OpenIMP (USE_OPENIMP=1) but /usr/lib/libimp.so is the Ingenic vendor libimp - install the vendor-flavour timpsd (USE_OPENIMP=0) or the OpenIMP libimp
```

## 4. Daemon start (RTLD_NOLOAD path)

Only with the installed, matching binary:

```sh
/etc/init.d/S95timps restart
logread | grep -E 'LIBIMP|timps: timpsd|check-libs' | tail
pidof timpsd
```

Expected: no `LIBIMP` line and no `timpsd --check-libs exited` line;
`pidof` prints one PID. A `[LIBIMP] dlopen(libimp.so): … - libimp check
skipped` warning means uClibc-ng refuses `RTLD_NOLOAD` for the NEEDED copy:
harmless (the daemon starts), but the daemon-side check is then inert on that
image; record it.

## 5. Init-script refusal (optional, B)

With the mismatched copy from step 3 still in `/tmp/timpsd-x`:

```sh
sed 's|^DAEMON=.*|DAEMON=/tmp/timpsd-x|' /etc/init.d/S95timps >/tmp/S95t
sh /tmp/S95t start; echo "rc=$?"
logread | grep 'timps:' | tail -3
rm -f /tmp/S95t /tmp/timpsd-x
```

Expected: the mismatch line, then `timpsd not started (exit 78): libimp does
not fit this timpsd build`, `rc=1`; both lines in `logread` at `daemon.err`
under tag `timps`. The running daemon is untouched (had the preflight passed,
`start-stop-daemon` would have found it running via the pidfile).

## Results

| Camera | Image / libimp | Step | Result |
| --- | --- | --- | --- |
| | | | |
