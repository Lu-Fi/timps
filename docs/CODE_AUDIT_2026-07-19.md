# Unabhängige Code-Prüfung – microstream / timps

**Datum:** 2026-07-19
**Stand:** Branch `main`, HEAD `fe92235` („audit fixes: simulated stereo, honest caps, warnings, clamps, roi-deprecation"), Arbeitsbaum sauber (keine uncommitteten Änderungen).
**Prüfumfang:** vollständiges `src/` (~13.600 LOC C) – Sicherheit, Stabilität/Crash-Sicherheit, Speicher (Leaks/UAF/Overflow), Nebenläufigkeit, A/V-Konformität. Schwerpunkt: die in `fe92235` frisch applizierten Audit-Fixes.
**Grundlage:** Zeilengenaue Lektüre aller Kernmodule (hal_ingenic.c, config.c, control.c, imp_osd.c, hub.c, fanqueue.c, frame.c, rtsp.c, rtp.c, httpd.c, fmp4.c, srt.c, record.c, aac.c, vparam.c, nal.c, auth.c, tls.c, net.c, events.c, util.c, main.c), Verifikation gegen die vendored IMP-Header (`include/T31/1.1.6/en/imp/imp_isp.h`, `imp_audio.h`) und gegen den tatsächlich gebauten faac-Header (`thingino dl/faac/git/include/faac.h`, knik0/faac b92b7f8). Rotation (Branch `feature/rotation`) ist ausdrücklich NICHT Gegenstand dieser Prüfung.

---

## Gesamturteil

**Der Stand ist sauber. Alle fünf frisch applizierten Fix-Bereiche bestehen die Prüfung (PASS), es wurde kein neuer HIGH- oder MEDIUM-Defekt gefunden.** Die kritischste Einzelprüfung – die Simulated-Stereo-Reblocking-Arithmetik – ist nachweislich überlauffrei (Beweis unten), die Kanalzahl propagiert korrekt in ASC/SDP/esds/stsd/ADTS, und der Mono-Pfad ist byte-identisch zum Vorzustand. Die Stichproben der als „behoben" geführten Altbefunde (H1–H6, M1–M13, N1, N2, N4, N6) halten auf dem aktuellen Baum stand.

Verbleibend sind ausschließlich LOW-Punkte: zwei bekannte Alt-Themen (N3 UTF-8-OSD, M5 Digest-realm/uri) und drei neue Härtungs-Notizen (einseitiges Locking in `imp_osd_stop`, fehlender Non-TLS-Thread-Drain in `httpd_stop`, ungeklemmtes `audio.bitrate`). Keiner davon blockiert einen Release.

---

## 1. Verifikation der frisch applizierten Fixes (Priorität 1)

| # | Fix | Urteil | Kernevidenz |
| --- | --- | --- | --- |
| 1 | Simulated Stereo (dual-mono AAC) | **PASS** | hal_ingenic.c:1151–1189, 1225, 1238, 1281–1309, 1578–1585; faac.h:155/184/260 |
| 2 | Caps-Ehrlichkeit (`imp_osd_group_active`, privacy.available, osd/restart-Listen) | **PASS** (1 Härtungs-Notiz L-2) | imp_osd.c:504–512, control.c:591–620, imp_osd.h:33–35 |
| 3 | Werte-Clamps (image.\*, audio.\*, transparency) | **PASS** | config.c:397–460, imp_isp.h:561/835/1257/1286/1315/1329/2028/2056/986, imp_audio.h:249–250/577 |
| 4 | record/SRT Audio-Inaktiv-Warnungen | **PASS** | record.c:426–441, srt.c:277–291 |
| 5 | motion.roi_\*-Deprecation | **PASS** | config.c:568–587 |

### 1.1 Simulated Stereo – Detailnachweis

**Setup.** `ing_start` setzt `g_ach=2` nur bei `channels==2 || force_stereo` UND `g_acodec==MS_AC_AAC` (hal_ingenic.c:1578–1585); jeder Fallback auf G.711 setzt `g_ach=1` zurück (1182, 1188). `g_ach` wird vor dem `pthread_create` des Audio-Threads geschrieben, danach nur noch vom Audio-Thread selbst – kein Race.

**faac-Parameter (gegen den realen Header verifiziert).** `dl/faac/git/include/faac.h` des tatsächlich gebauten knik0/faac:
- Zeile 155: `bit_rate … target bits/sec PER CHANNEL` → die Division `bitrate_kbps*1000/g_ach` (hal_ingenic.c:1164) ist korrekt; `audio.bitrate` bleibt die **Gesamtrate** (32 kbps gesamt = 16 kbps/Kanal dual-mono).
- Zeile 184: `frame_samples … samples/channel per full frame (1024 LC)` → `faac_in *= g_ach` (1175) ergibt die interleaved Gesamtzahl (Mono 1024, Stereo 2048).
- Zeile 260–265: `faac_encoder_encode` nimmt `in_samples` als „interleaved PCM samples (total across all channels) … 0 bis frame_samples*num_channels" → der Aufruf mit exakt `faac_in` (1300) ist spezifikationskonform; `FAAC_ERR_INPUT_OVERFLOW` ist unerreichbar.

**Überlaufbeweis für `acc[4096]` (int16).** Invarianten der Append-/Drain-Schleife (1281–1309):
1. Nach jedem Drain gilt `acc_n < faac_in` (while-Bedingung 1294). Für AAC-LC: `faac_in = 1024·g_ach ≤ 2048`, also `acc_n ≤ 2047`.
2. Beim Append gilt `room = 4096 − acc_n` und `take·g_ach ≤ room` durch den Clamp `take = room/g_ach` (1284). Der höchste beschriebene Index ist `acc_n + 2·take − 1 ≤ acc_n + room − 1 = 4095`. **Kein Overrun, für keinen Input.**
3. Worst Case nominal: Leftover 2047 + verdoppelter 40-ms-Frame @16 kHz (2·640 = 1280) = 3327 < 4096 – der Clamp greift im Normalbetrieb nie; er greift nur bei hypothetisch größeren AI-Frames und chunked dann korrekt (die äußere `off`-Schleife iteriert; `ai_rate_enum` lässt ohnehin nur 8/16 kHz zu, hal_ingenic.c:988–994).
4. Terminierung: `faac_in ≥ 1024` ist garantiert (Initialwert 1024 in Zeile 1151, auch wenn `faac_encoder_get_info` fehlschlüge), `take = 0` ist nur bei `room < g_ach` möglich, was `acc_n ≥ 4095` erfordert – dann drained die innere Schleife (`acc_n ≥ faac_in`). Keine Endlosschleife.
5. `acc_n` bleibt in Stereo stets gerade (Inkremente `take·2`, Dekremente `faac_in=2048`) – das Interleaving L,R,L,R… (1287–1288, L=R) kann nie um ein Sample verrutschen. Gültiges 16-bit-interleaved-Stereo für `FAAC_INPUT_16BIT`.
6. `memmove`-Rest nach Encode (1308) verschiebt `acc_n·2 ≤ 4094` Bytes innerhalb des Puffers – korrekt.

**Mono-Regression:** bei `g_ach==1` sind Clamp (`take·1>room`), `memcpy`-Pfad (1291), `faac_in` (1024·1), `bit_rate` (÷1) und `in_samples` identisch zum Vorzustand – **byte-identisch**.

**Propagation der Kanalzahl (alle `hub_get_audio`-Konsumenten geprüft):**
- Hub: einmalige Publikation `hub_set_audio_params(g_acodec,g_asr,g_ach)` erst NACH validiertem AI + faac (hal_ingenic.c:1225) – die bekannte Advertise-Race-Absicherung bleibt intakt.
- ASC: `aac_asc()` schreibt `channelConfiguration = channels<<3` korrekt (aac.c:20); channels ist auf 1..2 geklemmt (config.c:441), kein Feldüberlauf.
- RTSP/SDP: `a=rtpmap:… mpeg4-generic/<asr>/<ach>` + `config=` aus `aac_asc(asr,ach)` (rtsp.c:276–285). ✓
- fMP4: stsd `a_channels` (fmp4.c:272), esds-ASC (fmp4.c:258); httpd.c:200–204, record.c:246–248, record_clip 592–593. ✓
- SRT/ADTS: `channel_configuration` aus `a_ch` (srt.c:81–85), Sampling-Index-Guard bleibt (srt.c:294–305). ✓
- **Timestamps:** RTP-AAC advanciert `audio_samples += 1024` pro AU (rtp.c:243–245) – das ist die Per-Kanal-Samplezahl, die bei 2 Kanälen unverändert 1024 bleibt; RTP-Clock = asr (rtsp.c:513–515). fMP4 nutzt fix `dur=1024` in `a_timescale=asr` (fmp4.c:501–506). Beide korrekt für 2ch; kein Faktor-2-Fehler.
- Sim-Backend spiegelt die Regel (hal_sim.c:196–203). Fußnote: der Sim spielt eine fertige ADTS-Datei ab – deren realer Kanalinhalt kann von `ach=2` abweichen (reines Host-Testwerkzeug, kein Gerätepfad).
- G.711-Guard: Stereo-Wunsch mit G.711 (inkl. AAC→PCMU-Degradation) bleibt mono mit Warnung (hal_ingenic.c:1580–1583). RTP-PT 0/8 bleibt spec-konform mono/8000.

**Kein Off-by-one, kein Overrun, keine Timing-Regression gefunden.**

### 1.2 Caps-Ehrlichkeit

- `imp_osd_group_active()` (imp_osd.c:504–512): Bounds-Check, Lesen von `g_os[stream].used` unter `OSD_LOCK`, Sim-/Stub-Variante liefert 0 (imp_osd.c:554). Deklaration in imp_osd.h:33–35 unter `USE_CONTROL`, Aufruf in control.c:612–620 zusätzlich unter `#ifdef HAL_INGENIC` – alle vier Build-Permutationen (target/sim × control an/aus) linken.
- JSON-Form geprüft: Klammerbilanz der `caps`-Konstruktion (control.c:575–622) stimmt; `caps.osd` ist weiter ein String-Array (nur ohne `"enabled"`), `caps.restart` weiter ein String-Array (neu: `"osd.enabled"` als drittes Element), `caps.privacy` behält Form `{"available":N,"max_regions":N}`. Keine strukturelle Änderung, die einen JSON-Parser bricht.
- Semantik ehrlich: `imp_osd_setup` baut die Gruppe nur bei `osd.enabled || privacy-aktiv` (imp_osd.c:298–316) und pre-created ALLE Privacy-Region-Handles (343–352) – `available=1` impliziert also tatsächlich live-anwendbare Masken; `available=0` (Gruppe fehlt / Sim) impliziert persist-only. Deckt sich mit `imp_osd_privacy_apply` (481–499).
- **Härtungs-Notiz (L-2, unten):** Das Locking ist einseitig – `imp_osd_stop()` (imp_osd.c:515–546) reißt `g_os` OHNE `OSD_LOCK` ab. Im heutigen Lebenszyklus (main.c:143–152: `httpd_stop()` läuft VOR `g_hal->stop()`, ein Live-Rebuild-Pfad existiert nicht) ist das nur in einem Shutdown-Restfenster relevanter Natur (Details L-2); der Fix selbst ist damit nicht falsch, aber der Kommentar „a live streamer restart tears groups down while /control keeps serving" verspricht mehr Schutz, als die Stop-Seite einlöst.

### 1.3 Werte-Clamps (gegen T31-1.1.6-Header verifiziert)

| Key(s) | Clamp | Header-Domäne | Befund |
| --- | --- | --- | --- |
| brightness/contrast/saturation/sharpness/hue | 0..255 | `unsigned char`, „range 0 to 255, default 128" (imp_isp.h:561 u. a.) | ✓ |
| ae_compensation | 0..255 | „recommended value range 0 to 255" (imp_isp.h:835) | ✓ |
| sinter/temper/dpc/drc_strength | 0..255 | „value range is [0-255], default 128" (imp_isp.h:1315/1329/2028/2056) | ✓ |
| defog_strength | 0..255 | uint8-Pointer-API (hal_ingenic.c:342) | ✓ |
| highlight_depress | 0..10 | „[0-10], 0 = disable" (imp_isp.h:1257) | ✓ |
| backlight_compensation | 0..10 | „[0-10], 0 = disable" (imp_isp.h:1286) | ✓ |
| wb_rgain/wb_bgain | 0..65535 | `uint16_t` in `IMPISPWB` (imp_isp.h:985–987) | ✓ |
| max_again/max_dgain | 0..255 | `uint32_t`, kein dokumentiertes Maximum („0=1x, 32=2x, and so on", imp_isp.h:1164) | ✓ unbedenklich: 255 ≈ 8 Blendenstufen (≈256×) liegt jenseits jedes unterstützten Sensors; Defaults 160/80 unangetastet |
| audio.channels | 1..2 | neue Stereo-Domäne | ✓ |
| audio.ns | 0..3 | `IMP_AI_EnableNs` mode „0 ~ 3" (imp_audio.h:577) | ✓ (timps-Semantik 0=aus, 1..3=Level; NS_LOW=0 war schon vorher unerreichbar – kein Regress) |
| agc_target_dbfs / agc_compression_db | 0..31 / 0..90 | `IMPAudioAgcConfig` „[0, 31]" / „[0, 90]" (imp_audio.h:249–250) | ✓ (Defaults 10/0 innerhalb) |
| osd transparency | 0..255 | `fgAlhpa` uint8 (imp_osd.c:328/458) | ✓ (Default 255 unverändert) |

Alle `config_defaults()`-Werte (config.c:135–187) liegen innerhalb der Clamps – **kein Default und kein legitimer Wert wird verändert**. Enums/Bools (running_mode/anti_flicker/core_wb_mode/hflip/vflip) bleiben wie kommentiert ihren Konsumenten überlassen (libimp validiert); unverändert zum Vorzustand.

### 1.4 Warnungen & roi-Deprecation

- record.c:429–441: `static int aac_warned` → genau einmal pro Prozess, nur im (seltenen) Re-Subscribe-Pfad ausgewertet, verändert weder `sub_audio` noch den Ablauf. Codec-Name-Ausgabe deckt g711u/g711a/none korrekt ab. ✓
- srt.c:281–291: identisches Muster, vor der Streaming-Schleife, kein Flow-Einfluss; `m->a_ch`-Fallback (`ach>0?ach:1`) unverändert. ✓
- config.c:568–587: `roi_x/y/w/h` werden weiter geparst/persistiert (Kompatibilität), `k[4]`-Dispatch ist für alle vier Keys korrekt (Index 4 = `x|y|w|h`), Warnung einmalig (`static int roi_warned`) und nur bei ≠0-Wert. Kein Konsument liest die Felder (grep-verifiziert: nur config.c). ✓

---

## 2. Defekte

**Keine BROKEN-Befunde.** Es wurde kein Fehler gefunden, der Speichersicherheit, Streamkorrektheit oder Stabilität des aktuellen Standes real verletzt.

---

## 3. Frische Full-Codebase-Pass-Befunde (alle LOW / Härtung)

- **L-1 (Robustheit) — `audio.bitrate` ungeklemmt.** config.c:442 parst mit blankem `pint()`. hal_ingenic.c:1163–1164 rechnet `(uint32_t)bitrate_kbps*1000/g_ach`; ein absurder Konfigurationswert > ~4.29e6 kbps wrappt im uint32-Produkt (negativ ist durch `>0` abgefangen). Praktisch nur per Hand-Edit der Config erreichbar; faac lehnt Unsinn ohnehin ab. *Fix:* `pint_cl(val,8,320)` analog zu den übrigen M11-Clamps.
- **L-2 (Nebenläufigkeit, latent) — `imp_osd_stop()` ohne `OSD_LOCK`.** imp_osd.c:515–546 setzt `used=0`, `free()`t Buffer/Fonts und zerstört Regionen ohne die Sperre, die `imp_osd_apply`/`imp_osd_privacy_apply`/`imp_osd_group_active` schützt. Erreichbar nur im Shutdown: main.c stoppt httpd (Zeile 151) vor `g_hal->stop()` (152), aber `httpd_stop()` wartet im **Non-TLS-Build** nicht auf detachte `conn_thread`s (der Drain httpd.c:1053–1061 steht nur im `USE_TLS`-Zweig) – ein gerade laufender `/control`-Handler kann theoretisch in den Teardown hineinlesen (für `group_active` ein benignes Int-Race, für den vor-existierenden `imp_osd_apply`-Pfad ein UAF-Fenster von Millisekunden, einmal pro Prozessende). *Fix:* (a) Teardown-Schleife in `imp_osd_stop` unter `OSD_LOCK` stellen, (b) den `g_nconn`-Drain in `httpd_stop` auch ohne TLS ausführen. Kein Regress durch `fe92235` – der Commit hat das Fenster nur um einen (gelockten, harmlosen) Leser erweitert.
- **L-3 (Kosmetik/Sim) — hal_sim-Stereo-Advertise.** hal_sim.c:196–203 meldet `ach=2`, spielt aber eine fertig kodierte ADTS-Datei ab, deren tatsächliche Kanalzahl davon abweichen kann (ASC↔Frames-Mismatch nur im Host-Sim). Kein Gerätepfad betroffen.
- **L-4 (Kosmetik) — `ach` in `stream_loop` ungenutzt** (rtsp.c:502): wird befüllt, aber nur `ac`/`asr` konsumiert. Rein kosmetisch.

Netz-Parsing (RTSP-Interleaved-Reassembly rtsp.c:594–638, HTTP-Header/Body-Deadlines httpd.c:801–950, SRT-listen_cb), Auth (Nonce-Bindung, konstantzeitige Vergleiche auth.c:20–36/86–91/108–115), TLS (Min-1.2, Handshake-Timeout, fd-Registry/Drain), hub/fanqueue-Refcounting (publish-snapshot + `g_pushing`-Handshake hub.c:169–239, Drop-Oldest+GOP-Purge fanqueue.c:33–78), Encoder/OSD-Teardown-Reihenfolge (Unbind vor Destroy, `nbound`-Buchführung hal_ingenic.c:1607–1653/1718–1747), Config-Write-Pfad (mkstemp+fsync+dir-fsync+Writer-Mutex config.c:1095–1188) und die Integer-Größenmathematik (ms_buf mit sticky `err`, box_close-Guards) wurden erneut vollständig gelesen: **keine neuen Befunde**.

---

## 4. Status der bekannten offenen LOW-Punkte (aus CODE_AUDIT_2026-07-18)

| ID | Thema | Status heute |
| --- | --- | --- |
| N1 | record-Dauerwerte ungeklemmt | **behoben** – `segment_s` 0..86400, `pre_roll_s` 0..60, `post_roll_s` 0..300, `min_free_mb` 0..1048576 (config.c:605–610); 0 = dokumentiert „keine Rotation" |
| N2 | hvcC `numTemporalLayers=0` | **behoben** – vparam.c:172 emittiert `0x0B` (1 temporale Schicht, lengthSizeMinusOne=3) |
| N3 | OSD nur Latin-1 (UTF-8-Mehrbyte byteweise gerastert) | **offen** (msttf.c unverändert; nur Overlay-Optik, keine Stream-Konformität) |
| M5 | Digest: literale realm/uri-Prüfung (RFC 2617) | **offen** – auth.c:77–78 hasht die client-gelieferten realm/uri-Werte; die Server-Nonce-Bindung (auth.c:74) verhindert weiterhin Replay/Offline-Forge. Kein Auth-Bypass. |
| N4 | RTSP-Transportwerte | **behoben** – interleaved-Kanäle 0..255 (rtsp.c:413–414), client_port 0..65535 (430–431) |
| N5 | /control-Body-Timeout ⇒ Teilanwendung | **offen/akzeptiert** – httpd.c:935–952 parst nach Deadline den Torso (bereichsgebunden, `sanitize_val`-bereinigt, authentifiziert; clen>cap wird jetzt mit 413 abgelehnt, httpd.c:925–929) |
| N6 | APP()/snprintf-Zeiger-UB | **behoben** – alle Akkumulatoren nutzen das `o<cap?buf+o:buf, o<cap?cap-o:0`-Muster (control.c:548–551/569–572, httpd.c:560–563); es wird kein Out-of-Range-Zeiger mehr gebildet |
| L13 | RTP-int64-Produkt überläuft nach ~2,8 Jahren Uptime | **offen, dokumentiert deferred** (rtp.c:42–48) |

---

## 5. A/V-Stream-Konformität (statisch, aktueller Baum)

- **H.264:** avcC mit High-Family-Erweiterung (chroma_format/bit_depth per Exp-Golomb aus der realen SPS, vparam.c:112–142); SDP `profile-level-id`/`sprop-parameter-sets` aus SPS/PPS; RTP RFC 6184 Single-NAL/FU-A mit korrektem Marker (Lookahead statt fester NAL-Liste, rtp.c:128–145).
- **H.265:** hvcC mit echter PTL aus der SPS, `chromaFormat=1`, `numTemporalLayers=1` (N2 behoben), `array_completeness=1` konsistent zu hvc1 + Parameter-Set-Stripping (vparam.c:146–191); RTP RFC 7798 FU. MSE-`codecs`-String aus der realen PTL (vparam.c:202–231).
- **AAC:** ASC AOT2/2-Byte mit korrekter channelConfiguration 1 **oder 2** (aac.c:14–22); esds-Längenfelder für 2-Byte-ASC korrekt (0x19/0x11-Descriptor-Kette, fmp4.c:238–264); RTP RFC 3640 AU-Header auf JEDEM Fragment mit voller AU-Size (rtp.c:259–282); ADTS-Wrap im TS-Mux mit echtem sr-Index+ch und Guard gegen Index 15 (srt.c:74–91, 294–305); ADTS-Doppel-Header-Schutz via `aac_adts_strip` in allen drei Senken. Sample-count-getriebene Timestamps mit Gap-Resync in RTP (rtp.c:212–230) und fMP4 (fmp4.c:98–112) bleiben bei 2 Kanälen exakt (1024 Samples/Kanal/Frame).
- **G.711:** hart auf 8 kHz mono gepinnt inkl. echtem AI-Reinit beim faac-Fallback (hal_ingenic.c:1060, 1196–1224); statische PTs 0/8.
- **fMP4:** tfdt v1 (64 bit) strikt monoton, kontinuierliche Decode-Timeline mit Re-Anchor nur vorwärts; trun/tfhd/trex-Flags wie gehabt; sticky-`err`-Guards verhindern das Ausliefern korrupter Boxen.
- **Dual-Mono-Stereo** ist als 2-Kanal-AAC-LC voll spezifikationskonform (channelConfiguration=2, interleaved L=R) – Clients, die eine Stereo-Spur verlangen, bekommen eine gültige.

Der Vorgänger-Audit hatte die realen Streams (RTSP main/sub, HTTP-fMP4) mit 0/0 Decode-Fehlern gemessen; die seitherigen Änderungen berühren den Video-Bitstream nicht und den Audio-Pfad nur in der (oben bewiesenen) Kanal-Erweiterung. **Ein kurzer On-Device-Smoke-Test mit `audio.channels=2` (ffprobe: „stereo", A/V-Sync) wird als einzige noch ausstehende empirische Bestätigung empfohlen.**

---

## 6. Konfidenz pro Plattform

- **T31/C100 (1.1.6/2.1.0, ENC_NEW_API):** hoch – Header zeilengenau verifiziert, bisherige On-Device-QA-Läufe auf diesem Pfad.
- **T23 (1.3.0):** hoch – ABI-Tripwire für `fcrop` (fs_create) vorhanden, Header-Version im Makefile erzwungen; Stereo-Pfad ist SoC-unabhängig (Software-faac).
- **T10/T20/T21/T30 (classic API):** mittel – manueller CBR-Encoder-Pfad (enc_create) ist nur H.264 und wurde hier nur statisch geprüft; Audio-/Stereo-Pfad identisch zu T31 (reine `IMP_AI_*`-Basis-API).
- **T40/T41 (ISP_NEW_TUNING_API):** mittel-niedrig – eigene Tuning-/Flip-/FPS-Pfade vorhanden und plausibel, aber ohne Geräteverifikation; `hal_isp_total_gain` fällt dort designgemäß auf den /proc-Scrape zurück.
- **Host-Sim:** Syntax-/Warnungs-Check des Sim-Quellsatzes in dieser Prüfung sauber (gcc `-fsyntax-only`, nur bekannte, per `-Wno-misleading-indentation` unterdrückte Kosmetik); `timpsd-sim` liegt gebaut im Baum.

---

## Empfohlene Reihenfolge (Rest, nichts davon releaseblockierend)

1. **L-2** – `OSD_LOCK` in `imp_osd_stop` + Non-TLS-Drain in `httpd_stop` (kleinster Aufwand, schließt das letzte theoretische Shutdown-Fenster).
2. **L-1** – `audio.bitrate` klemmen (`pint_cl` 8..320).
3. On-Device-Smoke-Test `audio.channels=2` (ffprobe stereo + Sync).
4. **M5-Rest / N3 / L13** – wie gehabt nach Kapazität.
