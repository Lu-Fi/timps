# Unabhängige Code-Prüfung – microstream / timps

**Datum:** 2026-07-18
**Prüfumfang:** Sicherheit, Stabilität, Speicher- und Lastoptimierung sowie A/V-Stream-Konformität
**Grundlage:** vollständiges `src/` (~13.400 LOC C), drei unabhängige statische Audits, Verifikation gegen `git log`, sowie ffprobe/ffmpeg-Analyse realer QA-Aufnahmen (`timps-qa-20260718-165812`, Kamera 192.168.241.190)

Diese Prüfung ist unabhängig vom bereits vorhandenen `CODE_REVIEW.md`. Sie verifiziert dessen Befunde gegen den **aktuellen** Codestand und ergänzt eine echte Stream-Konformitätsprüfung.

---

## Gesamturteil

Der Codestand ist **sehr gut**. Praktisch alle im früheren `CODE_REVIEW.md` dokumentierten HOCH- und MITTEL-Befunde sind zwischenzeitlich behoben (Härtungs-Commits `1418af5`, `5ea745d`, `63761c9` u. a.). Drei unabhängige Audits (Netzwerk, Kern/Speicher, Codec/HAL) bestätigen die Fixes an konkreten Zeilennummern. Zwei als „neu/kritisch" gemeldete Befunde haben sich in der Nachprüfung als **Fehlalarme** herausgestellt (siehe unten) — wichtig, weil einer davon eine Root-Command-Injection behauptet hatte.

Die produzierten Streams sind **konform**: alle Aufnahmen (RTSP main/sub, HTTP-fMP4) dekodieren mit **0 Video- und 0 Audio-Fehlern**, A/V ist synchron, Zeitstempel monoton, GOP regelmäßig.

Es verbleiben nur **kleinere** Restpunkte (LOW), keine kritischen offenen Sicherheits- oder Speicherkorruptions-Bugs.

---

## 1. Verifikation der früheren Befunde (Status im aktuellen Code)

| ID | Thema | Status |
| --- | --- | --- |
| H1 | Socket-Timeouts RTSP | **behoben** – `net_set_timeouts(cfd,30,15)` (rtsp.c:725), `net.c:30-43` |
| H2 | Socket-Timeouts HTTP | **behoben** – `net_set_timeouts` (httpd.c:996), Body-Loop mit 5s-Poll (httpd.c:934-950) |
| H3 | SRT-Passphrase-Fehler ignoriert | **behoben** – Rückgabe geprüft, startet nicht unverschlüsselt (srt.c:398-403) |
| H4 | OSD-Canvas Integer-Overflow | **behoben** – `pixel_h` 8..512, W/H ≤4096, Größe in double/SIZE_MAX-Guard (msttf.c:372-397); `font_size` `pint_cl(8,256)` (config.c:335) |
| H5 | OSD-Region nicht geklemmt | **behoben** – Bitmap verworfen wenn > Streamgröße (imp_osd.c:215-247) |
| H6 | IMP-SDK-Rückgaben ungeprüft | **behoben** – StartRecvPic/GetStream/Bind geprüft (hal_ingenic.c:685-747, 1512-1527) |
| M1 | TLS-Handshake ohne Timeout | **behoben** – `SO_RCVTIMEO` vor Handshake (tls.c:105-110) |
| M2 | Keine TLS-Mindestversion | **behoben** – min TLS 1.2 (tls.c:64-69) |
| M3 | UAF auf TLS-Kontext beim Shutdown | **behoben** – fd-Registry, `shutdown()` vor `ms_tls_ctx_free` (rtsp.c:822-832) |
| M4 | SRT streamid serverseitig ungeprüft | **behoben** – `listen_cb` verwirft Mismatch (srt.c:372-392) |
| M5 | Digest realm/uri ungeprüft | **teilweise** – Replay-Risiko via Server-Nonce-Bindung behoben (auth.c:74); literale realm/uri-Prüfung (RFC 2617) weiterhin offen. **Keine Auth-Bypass-Lücke.** |
| M6 | Vorhersagbares `rand()` | **behoben** – urandom für SSRC/seq/ts_base/Session-ID (rtp.c:15-27, rtsp.c:403) |
| M7 | Recorder ohne fsync | **behoben** – fflush/fsync/fclose geprüft, `sync_file_range` (record.c:318-341) |
| M8 | Leak in ing_start/ing_stop-Fehlerpfaden | **behoben** – geordneter `fail:`-Teardown (hal_ingenic.c:1573-1619) |
| M9 | OSD retired Double-Buffer UAF | **behoben** – Retire-Ring Tiefe 3 (imp_osd.c:31, 169-175) |
| M10 | Config lockfrei in refresh_text | **behoben** – ganzer Item-Snapshot unter Lock (imp_osd.c:185-240) |
| M11 | Config-Numerik ungeprüft | **behoben** – `pint_cl()` für width/height/fps/bitrate/gop/qp/Ports (config.c:300-317, 460-482) |
| M12 | Tag/Nacht Wanduhr statt monoton | **behoben** – `ms_now_us()/1000` (daynight.c:290, 308) |
| M13 | `config_get_kv` ohne record.* | **behoben** – record.*-Zweig ergänzt (config.c:839-866) |
| M16 | msttf-Schnittpunkte auf 128 | **behoben** – dynamisch `maxint` alloziert (msttf.c:447-463) |
| L1,L2,L4,L5,L6,L8,L9,L10,L11,L13,L14a | diverse | **behoben** (an aktuellen Zeilen verifiziert) |
| L14b | fmp4 mfhd-seq 32-bit-Wrap | **bewusst so** – dokumentierter, unschädlicher Zähler-Wrap (fmp4.c:387-391) |

Fazit: **alle** früheren H/M-Punkte sind umgesetzt; die einzigen „offenen" Positionen (M5-Literalprüfung, L14b) sind entweder harmlos oder beabsichtigt.

---

## 2. Zwei widerlegte Fehlalarme (wichtig)

Bei der Nachprüfung wurden zwei automatisch gemeldete „neue" Befunde **verworfen** — Verifikation lohnt sich:

- **Kein Root-Command-Injection über `/control`.** Gemeldet wurde, `daynight.switch_cmd` (fließt in `system()` als root) sei über `/control` setzbar. Tatsächlich nutzt `control.c` eine feste Whitelist `DN_KEYS` (nur vier numerische Schwellenwerte); `switch_cmd`/`isp_path` sind ausschließlich aus der (vertrauenswürdigen) Config-Datei setzbar, **nicht** über das Netz-Interface. Kein Injection-Vektor.
- **hvcC chromaFormat ist korrekt.** Gemeldet als `0xFC` (monochrome); der Code emittiert bereits `0xFD` (4:2:0) an der richtigen Stelle (vparam.c:169). Die `0xFC`-Zeile daneben ist `parallelismType=0` und korrekt.

---

## 3. Verbleibende neue Befunde (alle LOW)

- **N1 (Stabilität/Disk) — Aufnahme-Dauerwerte ungeklemmt.** `segment_s`/`pre_roll_s`/`post_roll_s`/`min_free_mb` werden mit blankem `pint()` geparst (config.c:571-574). `segment_s ≤ 0` deaktiviert die Rotation (record.c:444), eine einzelne Segmentdatei wächst dann unbegrenzt. *Fix:* `pint_cl` analog zu den übrigen Feldern (z. B. `segment_s` 1..3600).
- **N2 (Konformität, kosmetisch) — hvcC `numTemporalLayers=0`.** vparam.c:172 schreibt `0x03`; korrekt wäre `0x0B` (1 temporale Schicht). Browser und ffmpeg tolerieren `0` (=unbekannt); nur strenge Validatoren (Bento4, Apple HLS) monieren es. Betrifft nur H.265-Metadaten, nicht den Bitstream.
- **N3 (Rendering) — OSD nur Latin-1.** msttf.c füttert die Glyph-Suche byteweise; UTF-8-Mehrbyte-Zeichen in OSD-Text/`{hostname}` werden als Einzelbytes gerastert. Betrifft nur das Overlay-Bild, **nicht** die Stream-Konformität.
- **N4 (Robustheit) — RTSP-Transportwerte ungeprüft.** Interleaved-Channels/`client_port` aus `sscanf` ohne Bereichsprüfung (rtsp.c:412/427); durch Truncation auf `uint8_t`/`uint16_t` **speichersicher**, nur Protokoll-Robustheit.
- **N5 (Robustheit) — `/control`-Body bei Timeout teilweise angewandt.** Läuft der 5s-Body-Poll ab, wird der Torso-Puffer trotzdem geparst (httpd.c:952). Da der Scanner bereichsgebunden und `sanitize_val`-bereinigt ist und der Pfad authentifiziert, maximal Teilanwendung — kein Overflow.
- **N6 (latentes UB, benigne) — Zeiger hinter Pufferende im `APP()`/snprintf-Akkumulator** (control.c:541-565, httpd.c:560-563): Das Größenargument kollabiert korrekt auf 0, es wird nichts geschrieben; das Bilden des Out-of-Range-Zeigers ist formal UB, praktisch folgenlos.

---

## 4. Speicher & Last (aus QA-Soak-Lauf, real gemessen)

- **Speicher:** RSS von `timpsd` bleibt über den Client-Ramp stabil bei ~5 MB (5108→5140→4904→5712→5904 kB); **kein Leck** erkennbar. Frame-Refcounting (`frame.c`, `__sync`-Atomics) und der hub/fanqueue-Handshake sind sauber; alle `fanqueue_pop`-Pfade `pkt_unref`en korrekt.
- **Last:** voll fps-stabil bis **4 gleichzeitige Clients** (min 23,5 fps, Aggregat 98 fps/s). Bei 8 Clients degradiert es kontrolliert (6 ok / 2 abgewiesen, min 16 fps) — kein Absturz, kein Leck. On-Demand-Encoding hält Idle-CPU niedrig.
- **Keine Busy-Loops** in Recorder-, Timelapse- oder Daynight-Threads (alle blockieren auf Pop-Timeouts bzw. gescheiteltem Sleep).

---

## 5. Stream-Konformität (unabhängig mit ffprobe/ffmpeg verifiziert)

Geprüft an realen Aufnahmen des aktuellen Builds (RTSP main+sub, HTTP-fMP4):

| Stream | Video | Audio | Decode-Fehler |
| --- | --- | --- | --- |
| RTSP ch0 (main) | H.264 **High@L5.1**, 1920×1080, yuv420p, progressive, 25 fps | AAC-LC 16 kHz mono | **0 / 0** |
| RTSP ch1 (sub) | H.264 High@L5.1, 640×360, yuv420p, 25 fps | AAC-LC 16 kHz mono | **0 / 0** |
| HTTP fMP4 | H.264 High@L5.1, 1920×1080, 25 fps | AAC-LC 16 kHz mono | **0 / 0** |

- **GOP** regelmäßig (Keyframes bei Frame 1,2,102,202,302,402 → Intervall 100), SPS/PPS in-band, IDR sauber.
- **A/V-Sync**: Drift −0,02 s (RTSP main), −0,03 s (sub), −0,08 s (fMP4) — synchron. Zeitstempel monoton (nonmono=0), keine >1,5 s Stille.
- **fps** 24,8 — innerhalb 10 % des Nominalwerts; Realtime-Rate 0,98×.
- Statisch bestätigt korrekt: AAC-ASC (AOT2/2-Byte), `esds`, ADTS-Strip, `avcC` (Profil/Level aus SPS), `trun`/`tfdt`(v1 64-bit)/`trex`/`tfhd`, NAL-Iteration (3-/4-Byte-Startcodes), G.711 µ-/A-law.
- Die von ffmpeg beim Re-Muxen gemeldeten „non-monotonic DTS"-Meldungen stammen vom Null-Muxer/MKV-Millisekunden-Raster der QA-Aufnahme, **nicht** aus dem gesendeten Bitstream (der QA-Timestamp-Parser meldet nonmono=0).

**Ergebnis: alle produzierten Audio-/Video-Streams sind konform.** Einziger inhaltlicher Konformitäts-Nit ist der kosmetische HEVC-`numTemporalLayers`-Wert (N2).

---

## Empfohlene Reihenfolge (Rest)

1. **N1** – `segment_s` & Aufnahme-Dauerwerte klemmen (einzige mit realem Betriebsrisiko).
2. **N2** – hvcC `numTemporalLayers` auf `0x0B` (saubere HEVC-Metadaten für strenge Validatoren).
3. **M5-Rest** – optional `strcmp(realm,AUTH_REALM)` + URI-Prüfung (RFC-2617-Formkonformität).
4. **N3** – optional UTF-8 im OSD-Renderer.
5. N4–N6 nach Kapazität (reines Hardening).

Keine dieser Positionen blockiert einen Produktiveinsatz.
