# timps — Code- & Integrations-Review (Fable 5)

_Stand: 2026-07-17 · Quelle: `microstream/src` (~11k LoC C) + `thingino-firmware/package/timps`_

Fünf parallele Reviews (Speicher, Performance, A/V-Konformität, Bugs/Sicherheit, thingino-Integration). Befunde konsolidiert und dedupliziert; nach Schweregrad priorisiert. Datei- und Zeilenangaben beziehen sich auf den aktuellen Stand im freigegebenen Ordner. Nichts wurde geändert — reines Review.

Zwei Befunde wurden von mehreren Reviews unabhängig gemeldet (RTSP-SETUP-fd-Leak, Per-Frame-Heap-Churn) — die sind besonders belastbar.

---

## Kritisch / High

**H1 — RTSP: fd-Leak bei wiederholtem SETUP → unauthentifizierter DoS.** `rtsp/rtsp.c:369-393`
Jedes UDP-`SETUP` überschreibt `s->v_udp[]`/`s->a_udp[]` via `net_bind_udp_pair()`, ohne ein zuvor gebundenes Paar zu schließen; `client_thread` (`:623-626`) schließt nur das *letzte*. Ist `rtsp.user` leer, ist Auth aus (`rtsp.c:285-286`) → ein Client kann in der Control-Phase wiederholt SETUP schicken und je 2 fds leaken, bis der Prozess für **alle** Clients keine Sockets mehr hat. _Fix: vorhandenes udp-Paar vor dem Rebind schließen, oder ein zweites SETUP für einen bereits konfigurierten Track ablehnen._ (Von Bugs- **und** Speicher-Review gemeldet.)

**H2 — OSD-Rasterizer: `calloc` ohne NULL-Check → Crash.** `hal/msttf.c:414`
`uint8_t *cov=calloc(bw*bh,1);` wird nie geprüft und bei `:441` dereferenziert. Läuft bei jeder Textänderung (~1×/s); unter Speicherdruck reißt es den ganzen Daemon mit. Die Nachbar-Allokationen sind geprüft — reines Versehen. _Fix: NULL-Check, Glyph überspringen._

**H3 — `hub_publish` hält den Hub-Lock über die gesamte Veröffentlichung.** `hub.c:169-188`
Unter `s->lock` laufen `vparam_update()` (SPS/PPS-Parse), `pkt_new()` (malloc + Full-Frame-memcpy, bis ~1 MB pro IDR) **und** `fanqueue_push()` auf bis zu 16 Subscriber-Queues. Jeder `hub_active/hub_get_vparam/hub_get_fps/hub_subscribe`-Aufrufer (inkl. der Producer-Threads selbst, OSD, SSE-Stats) blockiert hinter dem Frame-Copy; ein einzelner langsamer Sink-Mutex stallt den Publisher und alle anderen. _Fix: Paket vor dem Lock bauen; unter dem Lock nur `subs[]` snapshotten + vparam/fps aktualisieren, Push außerhalb._

**H4 — OSD-Scanline-Fill ist O(bw·ss²) Soft-Float pro Span.** `hal/msttf.c:434-446`
Für jeden Span wird über *jede* Pixelspalte der Glyph-Bbox mit Soft-Float-Vergleichen iteriert, statt den Integer-Pixelbereich aus `xa/xb` einmal zu berechnen. Läuft jede Sekunde je Textitem je Stream — auf FPU-losen SoCs teuer. _Fix: `pxa=(int)floorf(xa*ss)…` direkt, ~10–50× weniger Arbeit._

**H5 — Timelapse hält Hub-Subscription dauerhaft → Encoder/Framesource laufen 24/7.** `timelapse.c:186-205`
Solange aktiviert, bleibt der JPEG-Encoder (und bei piggybacked Source auch die *Video*-Framesource) rund um die Uhr am Laufen, um pro `interval_s` (Minuten/Stunden) genau ein Frame zu behalten — genau die Dauerlast, die das On-Demand-Design vermeiden soll. _Fix: pro Aufnahme just-in-time subscriben/unsubscriben wie `snapshot_jpg()`; die 2-s-Stop-Debounce fängt kurze Intervalle ab._

**H6 — WebUI „Passwort setzen" aktiviert die Auth nie wirklich.** `package/timps/files/www/x/json-config-rtsp.cgi` (`update_password`)
Die Seite POSTet nur `{password}`; das CGI schreibt `rtsp.pass`, aber **nie** `rtsp.user`. Da `rtsp.user` leer bleibt, behandeln `rtsp_check_auth()` (`rtsp.c:286`) und `http_check_auth()` (`httpd.c:372-374`) das als *Auth deaktiviert*. Ergebnis: Die WebUI zeigt Username „thingino" + gespeichertes Passwort, `S96onvif_discovery` lässt ONVIF offen (schreibt Creds nur bei nicht-leerem User), und RTSP/HTTP/Snapshot bleiben unauthentifiziert. _Fix: CGI muss beim Setzen eines Passworts auch `rtsp.user` (Default `thingino`) schreiben und nach `onvif.json` spiegeln._ **(Direkt verknüpft mit dem ONVIF-Login-Thema.)**

**H7 — `S95timps` (Package) hat Config-Seeding & TLS-Cert-Gen verloren.** `package/timps/files/S95timps`
Die Package-Version ist ein nacktes start/stop-Script. Die im **gebauten Image** noch vorhandene Logik — `[ -f $CONF ] || cp timps.conf.example` (Seeding) und `mbedtls-certgen` bei `http.https` — fehlt. Folge: Bei einer **frischen** Installation wird `/etc/timps.conf` nie aus der Vorlage angelegt → timps startet auf eingebauten Defaults (`rtsp.user` leer), und die neu gesetzten `rtsp.user/pass = thingino`-Defaults der `timps.conf.example` **greifen nicht**. HTTPS-Cert-Auto-Gen ist ebenfalls weg. _Fix: Seeding + Cert-Gen in die Package-`S95timps` zurückholen (oder ins Package als eigenen Schritt)._

**H8 — Motion-Sensitivity 1–8 vs. 0–255.** `json-send2.cgi:145-158` ↔ `tool-send2.html/js` (Stock, nicht von timps überschrieben)
Die „Send to services"-Motion-Sektion nutzt einen 1–8-Slider (prudynt-Skala) und reicht den Wert unverändert an timps `/control` `motion.sensitivity` (0–255, `config.c:472-475`) weiter. UI-Wert 8 ⇒ nahezu Null Empfindlichkeit; zudem konkurriert die Seite mit `config-motion.html` (timps-Override, korrekt 0–255) um denselben Key. _Fix: in `json-send2.cgi` umrechnen oder `tool-send2.*` als timps-Override mit 0–255 ausliefern._

---

## Mittel

**M1 — Per-Frame-Heap-Churn im fMP4-Pfad.** `mp4/httpd.c:242`, `mp4/fmp4.c:406`, `record.c:232`
Pro Video-Frame pro Client: `ms_buf_init(len+256)`+`free` in `stream_mp4`/`seg_write`, **plus** in `fmp4_video_fragment` ein zweites `ms_buf_init(len+32)`+`free` mit Annex-B→AVCC-Copy, dann ein weiterer Copy in `fragment()`. Summe ~4 mallocs + 4 Full-AU-Copies je Frame je Client auf einem 24/7-musl-Heap → Fragmentierung + vermeidbare CPU. _Fix: ein persistenter `ms_buf` je Verbindung (`len=0` reset statt free/init); `annexb_to_sample` direkt in den Fragment-Puffer schreiben._ (Speicher- + Performance-Review.)

**M2 — `gen_sdp`: ungeschützte `snprintf`-Akkumulation.** `rtsp/rtsp.c:217-255`
`n += snprintf(body+n, sizeof(body)-n, …)` ohne Schritt-Guard; `sizeof(body)-n` ist `size_t`, wird `n>2048`, wrappt die Länge → Schreib-Overflow. Derzeit nur durch die zufällige `fmtp`-Obergrenze (~1980 B) gebändigt. _Fix: jeden Schritt gegen `n < sizeof(body)` prüfen (wie es der RTP-Info-Code tut)._

**M3 — Data-Race auf live-mutierten `g_cfg`-Strings.** `config.c` ↔ `control.c:160-164`
`control.c` schreibt g_cfg-Strings (OSD-Text, `rtsp_path`, `record/timelapse.dir/name`, `sensor.model`) unter `config_str_lock`. Mehrere Reader snapshotten korrekt, aber jeder Reader ohne den Lock kann einen mid-`strncpy`-Torn-String (ohne Terminator) sehen → OOB-Read in `strlen`. _Fix: jeder Reader runtime-mutabler Strings nimmt `config_str_lock`._

**M4 — AAC über RTP: fehlende Per-Paket-AU-Header bei Fragmentierung.** `rtsp/rtp.c:171-193`
Bei AAC-Frames > MTU fehlt die korrekte RFC-3640-AU-Header-Sektion pro Fragment → nicht konform, Clients können den fragmentierten Zugriff verwerfen. (Bei üblichen Bitraten selten, aber möglich.) _Fix: AU-headers-length + AU-header je Fragment gemäß RFC 3640._

**M5 — AAC-Sample-Dauern aus PTS-Jitter statt fix 1024.** `mp4/fmp4.c:84-94, 428`
Die Audio-Sample-Dauer wird aus der PTS-Differenz abgeleitet statt auf die nominalen 1024 Samples fixiert → im MSE-Player A/V-Drift. _Fix: feste 1024-Sample-Dauer für AAC-Track._

**M6 — NAL-Iterator bricht bei Zero-Length-NAL ab.** `codec/nal.c:34`
Ein leeres NAL beendet die Iteration → Rest der Access Unit wird verworfen. _Fix: Zero-Length überspringen statt abbrechen._

**M7 — Piggybacked JPEG: encodet mit Video-fps, gelesen mit jpeg-fps.** `hal/hal_ingenic.c:680-698`
Der HW-JPEG-Encoder produziert bei `videoN.jpeg` mit der Video-Framerate (z. B. 25 fps), `jpeg_thread` zieht aber nur ~5 fps → ~80 % der HW-JPEGs werden verworfen/veralten im FIFO (Snapshot liefert dadurch ein Frame FIFO-Tiefe zu alt). _Fix: `StartRecvPic/StopRecvPic` pro Shot oder Kanal-Framerate = `jc->fps`._

**M8 — `jpeg.snapshot_path` erzwingt `jwant=true` → JPEG-Pipeline 24/7.** `hal/hal_ingenic.c:715-719`
Ein gesetzter Snapshot-Pfad hält die dedizierte JPEG-Framesource + HW-Encoder dauerhaft aktiv und schreibt die Datei mit voller `jc->fps`, unabhängig von Consumern. _Fix: `snapshot_interval_s` einführen und Pipeline daran gaten._

**M9 — SRT ohne Client-Limit.** `srt.c:357-369`
Anders als RTSP/HTTP kein `MAX_CLIENTS`-Cap; jeder Caller bekommt Thread + 256-Slot-Fanqueue, die Frame-Refs pinnt → Thread-/Speicher-Erschöpfung auf Klein-RAM-SoCs. _Fix: über `g_srt_clients` cappen (analog `rtsp.c:642`)._

**M10 — SRT/Record-Fanqueue mit 256 Paketen ≈ 5–10 MB pinned.** `srt.c:30`, `record.c:37`
Queue-Tiefe ist in Paketen begrenzt, nicht in Bytes. Ein stehender Consumer bei 256 Slots auf 2K@4–6 Mbit/s pinnt mehrere MB `ms_pkt`-Payload je Queue — relevant auf 64-MB-Targets. _Fix: Byte-Budget in `fanqueue_push`, oder auf 64–128 senken (RTSP/MP4 nutzen 64)._

**M11 — httpd.conf-Proxies hardcoden 8880, CGIs lesen `http.port` dynamisch.** `timps.mk:188-197`
`/mjpeg`-Alias, `/onvif/image.cgi`, `/x/dl0.jpg`/`dl1.jpg` werden mit literalem `8880` geschrieben; ändert der User `http.port`, brechen MJPEG-Alias, ONVIF-Snapshot-Proxy und Download-Buttons, während die CGIs weiter funktionieren. _Fix: Port zur Finalize-Zeit aus timps.conf ableiten oder über ein CGI routen._

**M12 — Kein Runtime-Seeding neuer Default-Keys auf Upgrades.** `timps.mk` / `S95timps`
`/etc/timps.conf` lebt auf dem beschreibbaren Overlay; neue Default-Keys in der gepackten Vorlage erreichen eine bestehende Overlay-Kopie nicht. Die CGIs hängen fehlende Keys beim Schreiben an (grep-guarded), aber ein reiner Config-Key ohne CGI-Writer materialisiert auf Upgrade-Units nie. (Verschärft durch H7.) _Fix: Merge-Seeding beim Start (fehlende Keys aus `.example` ergänzen)._

---

## Niedrig

- **L1** `fanqueue.c:53` — `cond_signal` unter gehaltenem Lock; nach Unlock signalisieren.
- **L2** `fanqueue.c:38-48` — Overflow droppt nur ein ältestes Paket; Consumer sendet danach undekodierbare P-Frames bis zum IDR. Bis zum nächsten Keyframe durchdroppen.
- **L3** (Baumweit) — keine Thread-Prioritäten; TLS-Handshake (Soft-Float-RSA), OSD-Render, Verzeichnis-Walks konkurrieren mit den Echtzeit-Producern. `SCHED_RR`/nice erwägen.
- **L4** `msttf.c:345` — `px_blend` Soft-Float pro Pixel; auf 8.8-Fixed-Point umstellen.
- **L5** `msttf.c:376` — Glyph wird pro Render neu geparst/geflattet, kein Cache; Polylines je (gid, pixel_h) cachen.
- **L6** `osd_vars.c:121-136` — `resolve()` fopent `/proc`-Dateien je Placeholder je Sekunde; alle mit ~1-s-TTL cachen (wie `get_net_tx`).
- **L7** `imp_osd.c:243`, `msttf.c:21` — TTF wird je Item voll in RAM kopiert, Dubletten bei gleichem Font; per Pfad cachen oder `mmap(PROT_READ)`.
- **L8** `record.c:114-130` — `find_oldest` unbegrenzt rekursiv + folgt Symlinks (`stat` statt `lstat`); Depth-Bound + `lstat` wie in `timelapse.c:66`.
- **L9** `httpd.c:307, 356` — `snprintf`-Rückgabe ohne Truncation-Clamp als Sendelänge (OOB, wenn Header je zu groß). Clamp wie `http_send_ex`.
- **L10** `auth.c:22, 68` — nicht-konstantzeitige Credential-Vergleiche (Timing-Seitenkanal); konstantzeitig wie `auth_token_eq`.
- **L11** `httpd.c:846-861` — negatives `Content-Length` umgeht den 413-Guard; `>= 0` validieren.
- **L12** `rtsp.c:213` — `getsockname`-Rückgabe ungeprüft → ggf. Müll-IP im SDP `o=`/`c=`.
- **L13** `rtp.c:24` — theoretischer int64-Timestamp-Overflow nach ~3 Jahren Dauerbetrieb.
- **L14** `rtp.c`/`net.c` — ein `sendto()` pro RTP-Paket; UDP mit `sendmmsg()` batchen.
- **L15** `rtsp.c:623` — UDP-fd-Close nur `>0` statt `>=0`; fds mit -1 initialisieren.
- **L16** `imp_osd.c:147` — `retired`-BGRA-Puffer wird erst beim nächsten Update frei; im Idle nach ~2 s freigeben.
- **L17** `record.c:46` — `RING_MAX_BYTES` 8 MB als Compile-Konstante; als `-D`-Override + kleinerer Default.
- **L18** A/V-Kleinkram: nicht-compound RTCP-SR; relative RTP-Info-URLs / kein `Content-Base`; schwacher ADTS-Sync-Check; Trailing-EPB-De-Emulation-Edge; 16.16-Samplerate-Overflow-Rand; keine CTS-Offsets; MSE-Sequence-Mode-Wahl.
- **L19** `json-send2.cgi:67` — GET-Default `m_sensitivity=4` (prudynt-Einheit) falsch für 0–255; 128 setzen.
- **L20** `restart-prudynt.cgi:2-3` — falscher Kommentar (kein `restart-timps.cgi` installiert); kosmetisch.

---

## Verifiziert in Ordnung (Stärken)

- **Refcounting** in `frame.c`/`fanqueue.c` korrekt: `__sync`-Atomics, Ownership-Transfer beim Push, Drain in `fanqueue_free`, Unref auf jedem Consumer-Branch — kein Double-Free/UAF. Zero-Copy-Fan-out (ein Copy je publiziertem Frame) ist das richtige Design.
- **On-Demand-Pipeline** (`fs_use/fs_unuse` + condvar-blockierte Idle-Producer, Stop-only-2-s-Debounce, level-basierte Aktivität) eliminiert Idle-CPU sauber und race-frei.
- **Kern-Konformität**: RTP-Packetization, SDP, `avcC`/`hvcC`/`esds`-Konstruktion und MSE-Codec-Strings (`avc1.XXYYZZ`/`hvc1`/`mp4a.40.2`) verifiziert korrekt.
- **SRT-TS-Muxer**: PAT/PMT/PES/Adaptation-Field/CRC32 stimmen; Client-Refcount-Drain vor Cleanup korrekt.
- **tls.c**: Partial-Read/Write-Loops und WANT_*/close-notify-Mapping konsistent mit den Streaming-Schleifen.
- **`config_write_keys`**: atomar (tmp+rename + `fsync` Datei & Verzeichnis), In-Place-Ersetzung + Dubletten-Drop korrekt.
- **events.c** Motion-Ring/Config-Cursor sound; **md5**/**base64**-Bounds ok.
- **ONVIF-`S96`-Finalize-Hook** korrekt ungegatet + Target-Probe (überschreibt stale raptor-Kopie).
- **/control-Schema ↔ CGIs/JS** konsistent (image/audio/video/osd/motion/privacy/daynight/record/timelapse); Token-/Loopback-Auth-Flow korrekt.

---

## Empfohlene Reihenfolge

1. **H1** (unauth DoS) und **H2** (Crash) — Stabilität/Sicherheit, klein zu fixen.
2. **H6 + H7** — Credential-/Seeding-Kette; ohne die greift die ganze Auth-/ONVIF-Konfiguration auf frischen Units nicht. Hängt direkt am ONVIF-Login-Thema.
3. **H3, M1** — Lock-Contention + Heap-Churn im Streaming-Hotpath (CPU/Latenz auf schwachen SoCs).
4. **H4, H5, M7, M8** — Idle-/OSD-Last (Strom, CPU, Flash-Wear bei Snapshot-Dauerschreiben).
5. **H8, M11, M12** — WebUI-/Package-Feinschliff.
6. A/V-Mediums (M4–M6) je nach Ziel-Client (Frigate/VLC/Browser).
