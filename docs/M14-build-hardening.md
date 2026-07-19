# M14 – Build hardening flags (umgesetzt)

Befund M14: Der Dienst läuft als root und öffnet Netzwerk-Sockets, das Build
nutzte aber keine Compiler-/Linker-Härtung (`Makefile:93–95`, `build.sh:273–283`).

## Was geändert wurde

**Makefile** – zentraler Härtungsblock vor den `CFLAGS`/`LDFLAGS`-Defaults:

- `Makefile:114–115` – zwei zentrale Schalter: `HARDEN ?= 1`, `FORTIFY ?= 1`.
- `Makefile:117–123` – `HARDEN_CFLAGS`/`HARDEN_LDFLAGS` abgeleitet aus den Schaltern.
- `Makefile:127` – `CFLAGS` erhält `$(HARDEN_CFLAGS)` (SSP + FORTIFY).
- `Makefile:128` – `LDFLAGS` erhält `$(HARDEN_LDFLAGS)` (RELRO/NOW/noexecstack).
  Ersetzt das alte `-Wl,-z,relro,-z,now` und ergänzt `-Wl,-z,noexecstack`.

**build.sh** – libc-bewusste, äquivalente Logik (build.sh übergibt eigene
CFLAGS/LDFLAGS und überschreibt damit die Makefile-Defaults):

- `build.sh:68–69` – `HARDEN`/`FORTIFY`-Env-Schalter (Default 1).
- `build.sh:70–85` – `apply_libc_env()`: musl → `-fstack-protector-strong`
  (+ `-D_FORTIFY_SOURCE=2`, wenn `FORTIFY=1`); uClibc → `-fno-stack-protector`.
- `build.sh:317` – ldflags um `-Wl,-z,noexecstack` ergänzt.
- `build.sh:371–372` – `HARDEN`/`FORTIFY` in der Usage dokumentiert.

## Aktive Flags

| Flag | Zweck | Wo aktiv |
|---|---|---|
| `-fstack-protector-strong` | Stack-Canary (SSP) | host/sim, musl-Cross |
| `-D_FORTIFY_SOURCE=2` | abgesicherte libc-Wrapper (`*_chk`), braucht `-Os`/`-O2` | host/sim, musl-Cross (FORTIFY=1) |
| `-Wl,-z,relro` + `-Wl,-z,now` | Full RELRO (read-only GOT nach Reloc) | überall |
| `-Wl,-z,noexecstack` | nicht ausführbarer Stack (NX) | überall (auch bei HARDEN=0) |

`-no-pie` bleibt erzwungen (non-PIC Vendor-Archive), daher kein PIE/ASLR fürs
Hauptbinary — nachvollziehbar und unverändert.

## Zentrale Konfigurierbarkeit

- `make sim` / `make target` → `HARDEN=0` (alles aus) bzw. `FORTIFY=0` (nur FORTIFY aus).
- `./build.sh timps <SOC>` → `HARDEN=0` / `FORTIFY=0` als Env-Variablen.
- RELRO/NOW/noexecstack sind linker-only und bleiben unabhängig davon an.

## Build-Verifikation (`make sim`, Host x86-64)

Build läuft sauber durch, keine neuen Warnungen, Binär läuft (`timpsd-sim -v`
→ Version, Usage). Härtungsmarker im ELF bestätigt:

- FORTIFY aktiv: `__memcpy_chk`, `__printf_chk`, `__snprintf_chk`, `__syslog_chk` …
- SSP aktiv: `__stack_chk_fail`
- Full RELRO: `FLAGS = BIND_NOW`, `GNU_RELRO`-Segment vorhanden
- NX-Stack: `GNU_STACK … RW` (nicht ausführbar)

Toggle-Test bestätigt: `FORTIFY=0` entfernt nur `-D_FORTIFY_SOURCE=2` (SSP bleibt);
`HARDEN=0` entfernt SSP+FORTIFY, `noexecstack` bleibt.

**Binärgröße:** sim unverändert bei 156.248 Bytes (Baseline = geändert), also
kein Größenregress; der Cross-Compile-Ablauf (compile-then-link, `-no-pie`,
Vendor-Statik) ist unangetastet.

## Einschränkungen bei der Ziel-Toolchain

- **uClibc (Default-Cameras):** Die thingino-uClibc-Toolchain ist mit
  `--disable-libssp` gebaut (keine `__stack_chk_*`-Symbole) und ihre Header
  haben keinen vollständigen `_FORTIFY_SOURCE`-`*_chk`-Satz. Daher bleiben SSP
  und FORTIFY dort bewusst **aus** (`-fno-stack-protector`) — sonst schlägt das
  Linken fehl bzw. FORTIFY no-opt. RELRO/NOW/noexecstack sind aktiv.
  Für volle Compiler-Härtung `--libc-musl` (oder eine libssp-fähige Toolchain)
  verwenden.
- **musl (Mainline-thingino):** volle Härtung (SSP + FORTIFY + RELRO + NX).
- **Ziel-Cross-Build konnte nicht end-to-end getestet werden:** in dieser
  Umgebung ist keine MIPS-Toolchain vorhanden und der Download der
  thingino-Toolchain war nicht möglich. Verifikation erfolgte über den
  Host-Sim-Build (`make sim`, volle Härtung inkl. FORTIFY/SSP) und einen
  `make -n target`-Trockenlauf, der die fünf Flags korrekt in der
  Compile-/Link-Zeile zeigt.
