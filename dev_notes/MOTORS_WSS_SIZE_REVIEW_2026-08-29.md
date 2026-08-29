# motors-daemon wss:// size review (2026-08-29)

Review of the binary-size cost of the wss:// (TLS WebSocket) support added to
thingino-motors today, plus the root cause of the Kconfig `default y` that
never fired. All numbers measured on the real production build: cross gcc from
`output/ciao/wuuk_y0510_...-192.168.10.21/per-package/thingino-motors/host/bin/mipsel-linux-gcc`,
production flags (`-Os -s -ffunction-sections -fdata-sections
-Wl,-z,max-page-size=0x1000 -Wl,--gc-sections`). Local rebuilds reproduce the
fleet numbers exactly: 59,576 bytes WS-only, 67,936 bytes WS+TLS, matching
`build/thingino-motors-custom/motors-daemon` byte-for-size.

## Verdict up front

The +8.3 KB is essentially the honest minimum for what was added, and the one
real optimization was already taken before this review started: **mbedTLS is
NOT statically linked**. No worthwhile size reduction is left on the motors
side; the size cost is justified by the mixed-content fix it enables. One real
bug WAS found and fixed, but it's a Kconfig correctness bug, not a size one
(see the last section).

## 1. mbedTLS linkage: already shared, zero library duplication

`package/thingino-mbedtls/mbedtls-override.mk` forces
`-DUSE_SHARED_MBEDTLS_LIBRARY=ON -DUSE_STATIC_MBEDTLS_LIBRARY=OFF`, so the
image only has shared objects (`libmbedtls.so.3.6.6` 300,636 B,
`libmbedcrypto.so.3.6.6` 555,704 B, `libmbedx509.so.3.6.6` 70,892 B), and
`readelf -d motors-daemon` confirms `NEEDED libmbedtls.so.21 / libmbedx509.so.7
/ libmbedcrypto.so.16` — the same .so's that uhttpd, libcurl, mosquitto and
timps (`timps.mk` line 160, identical `-lmbedtls -lmbedx509 -lmbedcrypto`)
already load. motors-daemon adds **zero** mbedTLS library bytes to the image.
The highest-value question of this review was answered "already done" before it
was asked.

## 2. Where the +8,360 file bytes actually are

`size -A` WS-only vs WS+TLS (production flags):

| section      | WS-only | WS+TLS | delta  |
|--------------|--------:|-------:|-------:|
| .text        | 39,968  | 42,912 | +2,944 |
| .rodata      |  8,204  |  9,076 |   +872 |
| .dynsym      |  2,368  |  3,024 |   +656 |
| .dynstr      |  1,767  |  2,562 |   +795 |
| .MIPS.stubs  |  1,328  |  1,808 |   +480 |
| .hash/.got/.dynamic/.rel |     | | +356 |
| .bss         |  1,120  |  1,156 |    +36 |
| **sections total** | 57,421 | 63,560 | **+6,139** |

So: ~2.9 KB real new code (ws_tls.c: `ws_tls_accept` 760 B, `ws_tls_ctx_new`
540 B, read/write/close/pending ~530 B; `motor_ws_start` +496 for cert
resolution, `conn_thread` +308 for the first-byte sniff), ~0.9 KB strings
(syslog messages, cert candidate paths), and ~2.3 KB pure dynamic-link
plumbing for the 30 imported `mbedtls_*` symbols (.dynsym/.dynstr/.hash
entries, MIPS PLT stubs, GOT slots) — that part is the fixed toll of talking to
a shared library at all and shrinks only by importing fewer symbols, which the
code doesn't overspend on. The remaining ~2.2 KB of file delta is 4 KB
page-alignment padding (`-z max-page-size=0x1000` rounds each LOAD segment).
Nothing here is fat; there is no debug scaffolding, no oversized table, and
`nm --size-sort` shows the biggest functions are the pre-existing core
(`conn_thread` 4.7 KB, `main` 3.5 KB, `load_config_file` 3.5 KB).

RAM: ~21 KB heap per **connected** TLS client (16 KB mbedTLS IN buffer + the
already-trimmed 4 KB OUT buffer + session state), bounded by
`ws_max_clients=4`. Session tickets deliberately not wired up — correct call
for a hold-one-connection client shape.

## 3. The ws_io seam: free where it matters, one caveat

The `ws_io {fd, tls}` abstraction is compile-time `#ifdef` branching — no
function pointers, no extra buffering. In a TLS build the dispatch is one
predictable `if (io->tls)` per I/O call; that's the structural minimum for
dual-transport support.

Caveat worth knowing (flagging per the review brief): the non-TLS build is no
longer **byte-identical** to pre-refactor HEAD — it is **file-size-identical**
(59,576 = 59,576) but carries ~548 bytes of real section growth (+256 .text,
+32 .rodata, +260 .bss) hidden inside the page padding. Cause: the three
`motor_ws_cfg` TLS fields (`tls_enabled`, `tls_cert[128]`, `tls_key[128]`) and
the `ws_tls`/`ws_tls_cert`/`ws_tls_key` JSON parsing in
`load_ws_config_file()` are not `#ifdef MOTORS_WS_TLS`-guarded. Defensible
(uniform config surface across builds, and the fleet ships TLS builds anyway);
guarding them would recover 0 file bytes on this target. Not changed. If the
padding headroom ever gets consumed, this is the first place to look.

## 4. mbedTLS feature trim: an image question, not a motors question

The shared `mbedtls_config.h` is deliberately fat for uhttpd's HTTP/2 (ALPN,
SNI, session tickets, TLS 1.3, three NIST curves) and is shared by six-plus
consumers; trimming it to motors' minimal server handshake would break the
others, and motors contributes no per-consumer library bytes to trim. The one
per-connection knob that mattered (OUT buffer 16 KB -> 4 KB) was already taken
2026-07-31. No action.

## 5. Root-caused and fixed: the Kconfig `default y` that never fired

`BR2_PACKAGE_THINGINO_MOTORS_WS_TLS`'s `default y if
BR2_PACKAGE_THINGINO_UHTTPD_TLS_MBEDTLS` failing to auto-apply on cam-garage's
regen was NOT merge-ordering or visibility — the new option itself created a
**Kconfig recursive dependency**, verbatim from kconfig:

```
package/mbedtls/Config.in:1:error: recursive dependency detected!
  symbol BR2_PACKAGE_MBEDTLS is selected by BR2_PACKAGE_THINGINO_MOTORS_WS_TLS
  symbol BR2_PACKAGE_THINGINO_MOTORS_WS_TLS depends on BR2_PACKAGE_THINGINO_UHTTPD_TLS_MBEDTLS
  symbol BR2_PACKAGE_THINGINO_UHTTPD_TLS_MBEDTLS is part of choice <choice>
  choice <choice> depends on BR2_PACKAGE_MBEDTLS   [via the choice's own default line]
```

The cycle: WS_TLS's `default y if` reads the uhttpd choice member; the uhttpd
choice's `default BR2_PACKAGE_THINGINO_UHTTPD_TLS_MBEDTLS if
BR2_PACKAGE_MBEDTLS` reads MBEDTLS; WS_TLS's `select BR2_PACKAGE_MBEDTLS`
closes the loop. kconfig breaks such cycles by ignoring the involved defaults —
so WS_TLS never defaulted on, and worse, with WS_TLS absent from .config,
`olddefconfig` even flipped `BR2_PACKAGE_THINGINO_UHTTPD_TLS_MBEDTLS` itself to
"not set" (reproduced empirically on a copy of the garage .config).

**Fix applied** (uncommitted, `package/thingino-motors/Config.in`): replace
`select BR2_PACKAGE_MBEDTLS` with `depends on BR2_PACKAGE_MBEDTLS`. Verified
against the real regen input (`.config_original` minus the local.fragment
workaround line): recursion error gone, `BR2_PACKAGE_THINGINO_MOTORS_WS_TLS=y`
and `BR2_PACKAGE_THINGINO_UHTTPD_TLS_MBEDTLS=y` both land from defaults alone.
The explicit `BR2_PACKAGE_THINGINO_MOTORS_WS_TLS=y` workaround in
`user/wuuk_y0510_t31x_sc4336p_ssv6158/192.168.10.21/local.fragment` is now
redundant (harmless to keep; its comment block should be updated or the line
dropped when convenient). Semantics change: on an image with no mbedTLS at all
the option is now hidden instead of self-enabling mbedTLS — every camera in
this fleet has mbedTLS via uhttpd TLS/thingino-ssl/timps, so nothing observable
changes here.
