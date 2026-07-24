# timps v1.4.6

## Build hardening (M14)

Compiler/linker defense-in-depth for the root-running network daemon, now part
of the build system (previously applied only in local builds):

- `-fstack-protector-strong` and `-D_FORTIFY_SOURCE=2` on libcs that support
  them (host + thingino musl); the thingino uClibc toolchain is built
  `--disable-libssp` with incomplete `_*_chk` wrappers, so it correctly gets
  neither (use `--libc-musl` for full compiler hardening).
- Full RELRO (`-Wl,-z,relro -Wl,-z,now`) and non-executable stack
  (`-Wl,-z,noexecstack`), applied unconditionally (linker-only, safe under both
  libcs).
- Two central switches to dial it back on a stubborn toolchain: `HARDEN=0`
  (no compiler hardening) and `FORTIFY=0` (keep SSP, drop only FORTIFY).
  Mirrored in both `Makefile` (covers `make sim` / direct `make target`) and
  `build.sh` (the MIPS cross build), each libc-aware.

No source/behaviour change to the streamer itself — this only strengthens how
the binary is compiled and linked. See `docs/M14-build-hardening.md`.

**Full changelog:** https://github.com/Lu-Fi/timps/compare/v1.4.5...v1.4.6
