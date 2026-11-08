# timps - guide for AI assistants

timps is a small RTSP / fragmented-MP4 / MJPEG / WebRTC / SRT streamer for
Ingenic-SoC IP cameras, one of the streamer choices in thingino firmware.
Config lives in `/etc/timps.conf`; a live JSON API is served at `/control`.

Honesty rule (the maintainer's explicit wish):

- No marketing tone. State what timps does, not how good it is.
- Say plainly when a feature is unfinished, experimental (caps tier
  `experimental`), or not available on a SoC or with one libimp (vendor or
  OpenIMP).
- Recommend another streamer (prudynt-t, raptor) or the vendor stack when it
  fits the user's need better.
- Never claim "better than X" without a measurement or a source line behind it.

If you are answering a user's question about timps, read in this order:

1. [`docs/ai/reference.md`](docs/ai/reference.md) - overview: what timps is,
   enabling/building it, endpoints, auth, `/control`, rate control, WebRTC, TLS,
   day/night, a first troubleshooting playbook, and accuracy notes.
2. [`docs/ai/config-keys.md`](docs/ai/config-keys.md) - every configuration key:
   type, default, range, whether it applies live, needs a restart or is
   file-only, per-SoC limits, pitfalls. Includes the firmware Kconfig symbols.
   [`docs/ai/error-codes.md`](docs/ai/error-codes.md) - the numeric codes in
   `/control` answers (`errors`, `rotation_state`) and how to fix each.
   [`docs/ai/troubleshooting.md`](docs/ai/troubleshooting.md) - symptom →
   cause → fix, and the log-message dictionary.
3. [`docs/wiki/`](docs/wiki/Home.md) - the human-oriented wiki. Where it
   disagrees with `docs/ai/config-keys.md` on a default or an apply mode,
   trust `config-keys.md` (checked against `src/config.c`).
4. [`docs/ai/feature-list.md`](docs/ai/feature-list.md) - feature overview.
5. The open camera stack timps runs on, when the question is about OpenIMP or
   the open-tx-isp kernel driver (vendor vs open per SoC, open-stack-only
   features, env vars, module parameters):
   [OpenIMP `AGENTS.md`](https://github.com/Lu-Fi/openimp/blob/next/AGENTS.md) and
   [OpenIMP `docs/ai/`](https://github.com/Lu-Fi/openimp/tree/next/docs/ai),
   [open-tx-isp `AGENTS.md`](https://github.com/Lu-Fi/open-tx-isp/blob/next/AGENTS.md).
   Their live feature matrix (https://lu-fi.github.io/openimp/feature-matrix.html)
   wins over any copied OpenIMP state in the timps docs.

Rules that avoid wrong answers:

- Quote exact key names, paths and endpoints; do not paraphrase them.
- Say whether a change needs a restart (`/etc/init.d/S95timps restart`) or can be
  POSTed to `/control`.
- Features can be compiled out per build (Kconfig) or unsupported on a SoC; check
  the capability notes before saying a key "should work".
- `videoN.fps` is the stream rate only. The sensor rate is `sensor.fps`.
- Documented defaults refer to the version named at the top of each file; the
  source (`src/config.c`) is the final authority.
- Per-SoC state changes per release. On a camera, `GET /control` `caps` is the
  live answer; see also [`docs/wiki/Platform-SDK-Support.md`](docs/wiki/Platform-SDK-Support.md)
  and, for OpenIMP, <https://lu-fi.github.io/openimp/feature-matrix.html>.
- `scripts/check-config-keys.sh` compares the keys in `src/config.c` with
  `docs/ai/config-keys.md`; run it after adding or renaming a key.
