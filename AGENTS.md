# timps - guide for AI assistants

timps is a small RTSP / fragmented-MP4 / MJPEG / WebRTC / SRT streamer for
Ingenic-SoC IP cameras, one of the streamer choices in thingino firmware.
Config lives in `/etc/timps.conf`; a live JSON API is served at `/control`.

If you are answering a user's question about timps, read in this order:

1. [`docs/ai/reference.md`](docs/ai/reference.md) - overview: what timps is,
   enabling/building it, endpoints, auth, `/control`, rate control, WebRTC, TLS,
   day/night, a first troubleshooting playbook, and accuracy notes.
2. [`docs/ai/config-keys.md`](docs/ai/config-keys.md) - every configuration key:
   type, default, range, whether it applies live, needs a restart or is
   file-only, per-SoC limits, pitfalls. Includes the firmware Kconfig symbols.
3. [`docs/wiki/`](docs/wiki/Home.md) - the human-oriented wiki. Where it
   disagrees with `docs/ai/config-keys.md` on a default or an apply mode,
   trust `config-keys.md` (checked against `src/config.c`).
4. [`docs/ai/feature-list-de.md`](docs/ai/feature-list-de.md) - German feature
   overview.

Rules that avoid wrong answers:

- Quote exact key names, paths and endpoints; do not paraphrase them.
- Say whether a change needs a restart (`/etc/init.d/S95timps restart`) or can be
  POSTed to `/control`.
- Features can be compiled out per build (Kconfig) or unsupported on a SoC; check
  the capability notes before saying a key "should work".
- `videoN.fps` is the stream rate only. The sensor rate is `sensor.fps`.
- Documented defaults refer to the version named at the top of each file; the
  source (`src/config.c`) is the final authority.
