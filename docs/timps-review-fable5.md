# timps — Code- & Integrations-Review (Fable 5)

_Stand: 2026-07-17 · Quelle: `src` (~11k LoC C) + `thingino-firmware/package/timps`_

Fünf parallele Reviews (Speicher, Performance, A/V-Konformität, Bugs/Sicherheit, thingino-Integration). Befunde konsolidiert und dedupliziert; nach Schweregrad priorisiert.

Zwei Befunde wurden von mehreren Reviews unabhängig gemeldet (RTSP-SETUP-fd-Leak, Per-Frame-Heap-Churn) — die sind besonders belastbar.

> **Umsetzungsstand (2026-07-17):** Alle High/Medium sowie die meisten Low wurden im Working Tree umgesetzt. Ausgenommen (bewusst zurückgestellt, brauchen On-Device-Test): **L4** (Fixed-Point-Blend), **L5** (Glyph-Cache), **L14** (sendmmsg-Batching). **L13** (Timestamp-Overflow) nur als Kommentar dokumentiert. Ein Build (`make sim` bzw. Cross-Build) muss die Änderungen noch verifizieren. Die C-Fixes liegen im lokalen `Lu-Fi/timps`-Checkout — für einen Package-Build müssen sie committet/gepusht und `TIMPS_VERSION` in `timps.mk` gebumpt werden (Package baut aus Git-Tag).

---

## Kritisch / High

**H1 — RTSP: fd-Leak bei wiederholtem SETUP → unauthentifizierter DoS.** `rtsp/rtsp.c:369-393`
Jedes UDP-`SETUP` überschreibt `s->v_udp[]`/`s->a_udp[]` via `net_bind_udp_pair()`, ohne ein zuvor gebundenes Paar zu schließen; `client_thread` (`:623-626`) schließt nur das *letzte*. Ist `rtsp.user` leer, ist Auth aus (`rtsp.c:285-286`) → ein Client kann in der Control-Phase wiederholt SETUP schicken und je 2 fds leaken, bis der Prozess für **alle** Clients keine Sockets mehr hat. _Fix: vorhandenes udp-Paar vor dem Rebind schließen, oder ein zweites SETUP für einen bereits konfigurierten Track ablehnen._ (Von Bugs- **und** Speicher-Review gemeldet.) — **umgesetzt.**

**H2 — OSD-Rasterizer: `calloc` ohne NULL-Check → Crash.** `hal/msttf.c:414`
`uint8_t *cov=calloc(bw*bh,1);` wird nie geprüft und bei `:441` dereferenziert. Läuft bei jeder Textänderung (~1×/s); unter Speicherdruck reißt es den ganzen Daemon mit. _Fix: NULL-Check, Glyph überspringen._ — **umgesetzt.**

**H3 — `hub_publish` hält den Hub-Lock über die gesamte Veröffentlichung.** `hub.c:169-188`
Unter `s->lock` laufen `vparam_update()`, `pkt_new()` (malloc + Full-Frame-memcpy, bis ~1 MB pro IDR) **und** `fanqueue_push()` auf bis zu 16 Queues. Jeder Reader blockiert hinter dem Frame-Copy. _Fix: Paket vor dem Lock bauen; unter dem Lock nur `subs[]` snapshotten + vparam/fps aktualisieren, Push außerhalb._ — **umgesetzt** (inkl. In-Flight-Push-Guard gegen UAF beim `unsubscribe`+`fanqueue_free`).

**H4 — OSD-Scanline-Fill ist O(bw·ss²) Soft-Float pro Span.** `hal/msttf.c:434-446`
_Fix: Integer-Pixelbereich aus `xa/xb` direkt berechnen, nur den iterieren (bit-identische Coverage)._ — **umgesetzt.**

**H5 — Timelapse hält Hub-Subscription dauerhaft → Encoder/Framesource laufen 24/7.** `timelapse.c:186-205`
_Fix: pro Aufnahme just-in-time subscriben/unsubscriben._ — **umgesetzt** (via Interval-Gating in `hal_ingenic.c`, M8-Mechanik).

**H6 — WebUI „Passwort setzen" aktiviert die Auth nie wirklich.** `package/timps/files/www/x/json-config-rtsp.cgi`
Das CGI schrieb `rtsp.pass`, aber nie `rtsp.user` → leerer User = Auth aus, ONVIF offen. _Fix: beim Passwort-Setzen auch `rtsp.user` (Default `thingino`) schreiben, beim Löschen beide leeren._ — **umgesetzt.** (Direkt am ONVIF-Login-Thema.)

**H7 — `S95timps` (Package) hatte Config-Seeding & TLS-Cert-Gen verloren.** `package/timps/files/S95timps`
_Fix: Seed-if-absent + Merge-Seed + `gen_tls_cert` zurück, alle idempotent._ — **umgesetzt.** Zusätzlich: Default-Credentials `thingino/thingino` in die aktive Vorlage `files/timps.conf` gesetzt.

**H8 — Motion-Sensitivity 1–8 vs. 0–255.** `json-send2.cgi`
_Fix: im CGI 1–8 ↔ 0–255 rescalen (Read und Write), GET-Default 128._ — **umgesetzt.**

---

## Mittel

**M1 — Per-Frame-Heap-Churn im fMP4-Pfad.** `mp4/httpd.c`, `mp4/fmp4.c`, `record.c` — **umgesetzt** (persistenter/TLS-`ms_buf`, Reset statt init/free pro Frame).

**M2 — `gen_sdp`: ungeschützte `snprintf`-Akkumulation → size_t-Underflow.** `rtsp/rtsp.c:217-255` — **umgesetzt** (Schritt-Guards).

**M3 — Data-Race auf live-mutierten `g_cfg`-Strings.** `config.c` ↔ `control.c` — **umgesetzt** (`config_get_kv` sperrt intern via rekursivem `config_str_lock`; hal_ingenic-Reads gesperrt).

**M4 — AAC über RTP: fehlende Per-Paket-AU-Header bei Fragmentierung.** `rtsp/rtp.c` — **umgesetzt** (RFC 3640 pro Fragment).

**M5 — AAC-Sample-Dauern aus PTS-Jitter statt fix 1024.** `mp4/fmp4.c` — **umgesetzt** (fixe 1024 für Audio, Video unverändert).

**M6 — NAL-Iterator bricht bei Zero-Length-NAL ab.** `codec/nal.c` — **umgesetzt** (überspringt leere NALs).

**M7 — Piggybacked JPEG: encodet mit Video-fps, gelesen mit jpeg-fps.** `hal/hal_ingenic.c` — **umgesetzt** (Encoder-Framerate = `jpeg_fps`).

**M8 — `jpeg.snapshot_path` erzwingt `jwant=true` → JPEG-Pipeline 24/7.** `hal/hal_ingenic.c` — **umgesetzt** (Interval-Gating, Default 60 s; Config-Key `jpeg.snapshot_interval_s` als TODO markiert).

**M9 — SRT ohne Client-Limit.** `srt.c` — **umgesetzt** (`SRT_MAX_CLIENTS`, Default 8).

**M10 — SRT/Record-Fanqueue-Tiefe 256 → mehrere MB pinned.** `srt.c`, `record.c` — **umgesetzt** (auf 128 reduziert, `-D`-overridable).

**M11 — httpd.conf-Proxies hardcoden 8880.** `timps.mk` — **im neueren `timps.mk` bereits gelöst** (P:-Proxies lesen `http.port` aus der Vorlage zur Finalize-Zeit).

**M12 — Kein Runtime-Seeding neuer Default-Keys auf Upgrades.** `S95timps` — **umgesetzt** (`merge_seed_config`).

---

## Niedrig

- **L1** `fanqueue.c` — `cond_signal` nach Unlock. **umgesetzt.**
- **L2** `fanqueue.c` — bei Overflow bis zum nächsten Keyframe durchdroppen. **umgesetzt.**
- **L3** (Baumweit) — keine Thread-Prioritäten; `SCHED_RR`/nice erwägen. _offen (Design-Entscheidung)._
- **L4** `msttf.c` — `px_blend` auf 8.8-Fixed-Point. _zurückgestellt (Pixel-Identität, Test nötig)._
- **L5** `msttf.c` — Glyph-/Polyline-Cache. _zurückgestellt (invasiver, Test nötig)._
- **L6** `osd_vars.c` — `/proc`-Reads mit ~1-s-TTL cachen. **umgesetzt.**
- **L7** `imp_osd.c` — TTF per Pfad cachen. **umgesetzt** (Refcount-Cache).
- **L8** `record.c` — `find_oldest` Depth-Bound + `lstat`. **umgesetzt.**
- **L9** `httpd.c` — `snprintf`-Truncation-Clamp. **umgesetzt.**
- **L10** `auth.c` — konstantzeitige Vergleiche. **umgesetzt.**
- **L11** `httpd.c` — negatives `Content-Length` abweisen. **umgesetzt.**
- **L12** `rtsp.c` — `getsockname`-Rückgabe prüfen. **umgesetzt.**
- **L13** `rtp.c` — int64-Timestamp-Overflow nach ~3 J. _nur dokumentiert (Kommentar); echter Fix = periodisches Rebasing._
- **L14** `rtp.c`/`net.c` — `sendmmsg()`-Batching. _zurückgestellt (On-Device-Test)._
- **L15** `rtsp.c` — UDP-fd-Close `>=0`, Init `-1`. **umgesetzt.**
- **L16** `imp_osd.c` — `retired`-Puffer im Idle freigeben. **umgesetzt.**
- **L17** `record.c` — `RING_MAX_BYTES` `-D`-overridable. **umgesetzt.**
- **L18** A/V-Kleinkram (nicht-compound RTCP-SR, relative RTP-Info-URLs, ADTS-Sync-Check, EPB-Edge, 16.16-Samplerate, CTS-Offsets, MSE-Sequence-Mode). _teils offen; niedrige Priorität, clientabhängig._
- **L19** `json-send2.cgi` — GET-Default 128. **umgesetzt** (Teil von H8).
- **L20** `restart-prudynt.cgi` — falscher Kommentar. _offen (kosmetisch)._

---

## Verifiziert in Ordnung (Stärken)

- **Refcounting** in `frame.c`/`fanqueue.c` korrekt: `__sync`-Atomics, Ownership-Transfer, Drain in `fanqueue_free` — kein Double-Free/UAF. Zero-Copy-Fan-out.
- **On-Demand-Pipeline** (`fs_use/fs_unuse` + condvar-Idle-Producer, Stop-only-Debounce) eliminiert Idle-CPU race-frei.
- **Kern-Konformität**: RTP-Packetization, SDP, `avcC`/`hvcC`/`esds`, MSE-Codec-Strings korrekt.
- **SRT-TS-Muxer**: PAT/PMT/PES/CRC32 korrekt; Client-Refcount-Drain korrekt.
- **tls.c**: Partial-Read/Write + WANT_*/close-notify konsistent.
- **`config_write_keys`**: atomar (tmp+rename + fsync).
- **events.c**, **md5**/**base64**-Bounds ok.
- **ONVIF-`S96`-Finalize-Hook** korrekt ungegatet + Target-Probe.
- **/control-Schema ↔ CGIs/JS** konsistent; Token-/Loopback-Auth-Flow korrekt.

---

## Nächste Schritte

1. **Build-Verifikation:** `make sim` (Host) und Cross-Build; Compile-Fehler beheben.
2. **C-Fixes ins Git bringen:** committen/pushen nach `Lu-Fi/timps`, neuen Tag setzen, `TIMPS_VERSION` in `timps.mk` bumpen (das Package baut aus dem Git-Tag, nicht aus dem lokalen Tree).
3. **On-Device-Test:** RTSP/MSE/ONVIF/Frigate, OSD-Rendering, Timelapse/Snapshot-Idle-Last.
4. Optional: die zurückgestellten Perf-Items (L4/L5/L14) nach Messung.
