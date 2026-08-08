# Konfigurations-Vollständigkeits-Audit – timps

**Datum:** 2026-08-07
**Prüfumfang:** Vollständiger Lebenszyklus jedes Konfigurationsfelds: (1) tatsächliche Nutzung im Code, (2) Dokumentation in `timps.conf.example`, (3) Dokumentation in `docs/wiki/Configuration-Reference.md`, (4) QA-Abdeckung (`scripts/timps-qa.sh` Abschnitt 8b), (5) Clamp-Grenzen und Cross-Thread-Zugriffssicherheit (die heute für `audio.mute`/OSD-`.enabled` behobene C11-Data-Race-Klasse).
**Grundlage:** `src/config.c` (Deskriptor-Tabellen, 18 Sektionen), `src/config.h`, `src/control.c` (`control_apply_json`/`control_get_json`), alle Konsumenten in `src/` (HAL, Threads, Server), `timps.conf.example`, Wiki, QA-Skript. Stand: Branch `main`, `9a4c2c8` (v1.7.7).

Geprüft wurden **184 Deskriptor-Tabelleneinträge** plus 5 außerhalb der Tabellen behandelte Spezial-Keys (`general.syslog`, `motion.roi_x/y/w/h`) — instanziiert (video ×2, OSD-Items ×16, Privacy ×8) entsprechend mehr Datei-Keys.

---

## Gesamturteil

Die Konfigurationsschicht ist **strukturell sehr gut**: Die Deskriptor-Tabellen (B2-Refactoring vom 2026-07-31) machen Setter/Getter-Drift konstruktionsbedingt unmöglich, das Wiki (`Configuration-Reference.md`) ist **vollständig** — jeder der 184 Tabellen-Keys plus alle Spezial-Keys sind dort korrekt beschrieben, und es dokumentiert keinen Key, den der Code nicht kennt. Der Daynight-Thread ist mit seinem Ganzstruktur-Snapshot unter `config_str_lock` (daynight.c:696-702) das Musterbeispiel für den korrekten Lesepfad.

Es verbleiben aber **ein HOCH-Befund** (ungeklemmte `sensor.*`-Numerik ist per `/control` persistierbar — exakt die M11-Klasse, die für `videoN.*` bereits geschlossen wurde), **zwei tote Video-Keys** (`qp`, `max_gop` werden geparst, geklemmt, persistiert, dokumentiert — aber von keinem HAL je gelesen), die **fortbestehende C11-Race-Klasse** in drei weiteren Hintergrund-Threads (record, timelapse, speaker/AO) sowie spürbarer **Doku-Drift in `timps.conf.example`** (13 fehlende Keys, zwei faktisch falsche Kommentarblöcke).

---

## Status-Matrix pro Sektion

Legende: ✔ = vorhanden/erfüllt, ✖ = fehlt, `–` = nicht anwendbar (z. B. kein `/control`-Pfad → kein Live-Test möglich; `noget`-Sektionen sind per GET nicht lesbar und damit nicht round-trip-testbar), ⚠ = siehe Findings.
Spalten: **G**elesen im Code · **E**xample (`timps.conf.example`) · **W**iki · **QA** (8b bzw. Persist-Test) · **C**lamp/Race-Status.

### `general.*`

| Key | G | E | W | QA | C |
| --- | --- | --- | --- | --- | --- |
| loglevel | ✔ config.c:1139 | ✔ | ✔ | – (file-only) | ohne Clamp, unkritisch |
| imp_polling_timeout | ✔ hal_ingenic.c:1051,1806,2316 | ✔ | ✔ | – | ohne Clamp |
| osd_pool_size | ✔ hal_ingenic.c:614 (`* 1024` → **KB**) | ✔ (KB) | ⚠ „bytes“ (F-10) | – | ohne Clamp |
| syslog (Spezialcode) | ✔ config.c:989-992 | ✔ | ✔ | – | – |

### `sensor.*`

| Key | G | E | W | QA | C |
| --- | --- | --- | --- | --- | --- |
| model | ✔ hal_ingenic.c:606-609 | ✔ | ✔ | ✖ (POSTbar, kein Persist-Test) | Registry überschreibt (config.c:1183-1189) ✔ |
| i2c_addr | ✔ hal_ingenic.c:611 | ✔ | ✔ | ✖ | **ungeklemmt** ⚠ F-01 |
| fps | ✔ hal_ingenic.c:674-677 | ✔ | ✔ | ✖ | **ungeklemmt** ⚠ F-01 |
| width | ✔ hal_ingenic.c:744 | ✔ | ✔ | ✖ | **ungeklemmt** ⚠ F-01 |
| height | ✔ hal_ingenic.c:745 | ✔ | ✔ | ✖ | **ungeklemmt** ⚠ F-01 |

### `image.*` (22 Keys)

Alle 22 Keys werden in `hal_ingenic.c` gelesen (ISP-Caps-gegated, stichprobenverifiziert u. a. `anti_flicker` :479-518, `alc_gain` :2046, WB/NR/DRC-Blöcke), alle 22 sind in Example **und** Wiki dokumentiert, alle 22 sind in QA 8b abgedeckt (`lv_section image`, timps-qa.sh:918-926) inkl. Clamp-Regressionstest für `brightness` (:1008-1009). Clamps: 19/22 sauber (0–255, 0–10, 0–65535).

| Abweichler | Befund |
| --- | --- |
| running_mode, anti_flicker, core_wb_mode | **ohne Clamp** (lo==hi, config.c:481-482,493) obwohl live-POSTbar → F-09 |

### `audio.*` (22 Keys)

| Key | G | E | W | QA | C |
| --- | --- | --- | --- | --- | --- |
| enabled, codec, samplerate, channels, bitrate | ✔ hal_ingenic.c:2800ff, rtsp.c | ✔ (codec-Kommentar ohne `opus` ⚠ F-07) | ✔ | codec via Opus-Probe (qa:1050-1062); Rest ✖ | samplerate ungeklemmt (F-09) |
| volume, gain, alc_gain | ✔ :2040-2046 | ✔ | ✔ | ✔ qa:933-934 | ✔ 0–100/0–31/0–7 |
| high_pass, agc, agc_target_dbfs, agc_compression_db, ns | ✔ :2052-2061 | ✔ | ✔ | ✖ (restart-only, bewusst) | ✔ |
| **mute** | ✔ `_Atomic`, hal_ingenic.c:2349, hal_sim.c:141 | ✔ | ✔ | ✔ | ✔ `F_ATOMIC` — **korrekt behoben** |
| force_stereo | ✔ hal_ingenic.c:2809, hal_sim.c:202 | ✔ | ✔ | ✖ | ✔ |
| spk_enabled | ✔ speaker.c:96 | ⚠ Kommentar falsch (F-07) | ✔ | ✖ | **Race** ⚠ F-02 |
| spk_volume, spk_gain | ✔ speaker.c:102-103, hal_ingenic.c:2490-2491 | ⚠ (F-07) | ✔ | ✖ (live, ungetestet → F-08) | **Race** ⚠ F-02 |
| backchannel, backchannel_codec, backchannel_rate | ✔ main.c:141-144 | ✔ | ✔ | ✖ | ✔ 8000–48000 |
| **aec** | ✔ hal_ingenic.c:3133 | **✖ fehlt komplett** (F-07) | ✔ | ✖ (F-08) | **Race** ⚠ F-02 |

### `jpeg.*` (7 Keys)

Alle gelesen (hal_ingenic.c:1771-1980, u. a. `snapshot_path` :1882), alle in Example + Wiki. `noget`-Sektion, kein `/control`-Pfad → QA `–`. Clamps ✔ (imp_chn ungeklemmt, intern, file-only — ok).

### `rtsp.*` (7 Keys)

Alle gelesen (`enabled` main.c, `port`/`mtu` rtsp.c:957-978, user/pass rtsp.c+httpd.c, `tls`/`tls_port` rtsp.c:1419-1443). Example ✔ (mtu auskommentiert mit Erklärung), Wiki ✔, QA `–` (file-only). Clamps ✔ (Ports 1–65535, mtu 548–1472).

### `http.*` (11 Keys)

Alle gelesen (`preview_chn` httpd.c:1011 — nachgelagert validiert :1012, `adaptive_drop` :289, `token`/`token_file` main.c:75-83+httpd, `https`/`tls_cert`/`tls_key` httpd.c:1341/rtsp.c:1437). Example ✔ **aber** `adaptive_drop`-Text veraltet (⚠ F-06). Wiki ✔ (Default 1 korrekt). QA `–`. Clamps ✔ (preview_chn ungeklemmt, aber jeder Konsument validiert nach — ok).

### `events.*` (3 Keys)

Alle gelesen (httpd.c:871,896 + enabled-Gate). Example ✔, Wiki ✔, QA `–`. `max_clients` ungeklemmt, aber am Nutzungsort abgesichert (`>0 ?: 8`) — ok.

### `srt.*` (6 Keys)

Alle gelesen (srt.c:277,454 + streamid/passphrase/latency). Example ✔, Wiki ✔, QA `–`. `channel` wird am Nutzungsort gegen Boot-Snapshot/enabled validiert (srt.c:277ff) — ok.

### `osd.*` (5 globale Keys)

Alle gelesen (`enabled` imp_osd.c:389-417, `monitor_stream` :479-480, `font_path` :392, `vars_file` :241, `supersample` :381). Example ✔, Wiki ✔. QA: nur `osd.enabled` ist POSTbar (restart-only) — kein Test, akzeptabel. `monitor_stream` ist `T_INT` statt `T_CHAN` (ungeklemmt), Konsument `hub_get_fps()` fängt ungültige Indizes ab — ok.

### `osd<S>.<N>.*` (14 Item-Keys)

Alle gelesen (imp_osd.c Ganz-Item-Snapshots :231-233,:301-303; sw-rotate-Pfad hal_ingenic.c:1362 ebenso unter Lock). Example ✔ (Logo-Felder + font_path als Kommentarblock). Wiki ✔. QA: alle 8 Live-Leaf-Keys via `lv_section osd0.0` (qa:938-940) ✔; type/logo* file-only `–`. Race: **sauber** — der v1.7.7-Fix (`.enabled` aus dem Snapshot statt lock-frei, imp_osd.c:234-239) ist verifiziert.

### `privacy<S>.<N>.*` (6 Keys)

Alle gelesen (imp_osd.c:388,449, setup_cover). Example ✔ (Kommentarblock), Wiki ✔, QA ✔ (alle 6, qa:944-945). Geometrie ungeklemmt, aber Apply-Pfad läuft im POST-Thread und der HAL verwirft Out-of-Frame-Regionen — ok.

### `motion.*` (9 + 4 Legacy-Keys)

| Key | G | E | W | QA | C |
| --- | --- | --- | --- | --- | --- |
| enabled, monitor_stream, sensitivity | ✔ imp_motion.c | ✔ | ✔ | ✔ qa:955-956 | ✔ (T_CHAN, 0–255) |
| cols, rows | ✔ imp_motion.c | ✔ | ✔ | ✖ bewusst (Budget-Clamp, qa:948) | ✔ Spezialcode config.c:944-964 |
| cooldown_ms, hold_ms, skip_frames | ✔ imp_motion.c:214ff,376,302 | ✔ | ✔ | – (file-only) | ✔ 250-Floor (M3) |
| on_motion | ✔ imp_motion.c:214,238 | ✔ | ✔ | – | `F_NOGET` korrekt (Security) |
| roi_x/y/w/h | **tot — dokumentiert** (B6, Warnung config.c:977-984) | – | ✔ als deprecated | – | – |

### `record.*` (10 Keys)

Alle gelesen (record.c). Example ✔ (Kommentarblock), Wiki ✔ **aber** post_roll_s-Range-Drift (⚠ F-10). QA: segment/pre/post/min_free/audio/name ✔ (qa:974-976), enabled/channel/mode bewusst ausgelassen, **`dir` (live-settable String) ungetestet** (F-08). **Race:** Strings unter Lock (record.c:177-179,228-231 — vorbildlich), aber **alle Int-Felder lock-frei im rec_thread** (⚠ F-02).

### `timelapse.*` (6 Keys)

Alle gelesen (timelapse.c). Example ✔, Wiki ✔. QA: interval_s/keep_days/name ✔ (qa:980-981), enabled/channel bewusst ausgelassen, `dir` ungetestet (F-08). **Race:** Strings unter Lock ✔, **Ints lock-frei im Thread** (⚠ F-02).

### `daynight.*` (23 Keys)

Alle 23 gelesen (daynight.c, verifiziert inkl. `probe_max_skip_s` :848, `boot_*` :589-590/771-772, `sun_*` :280-284). Wiki ✔ vollständig (inkl. der Probe-Economy-Semantik von 2026-08-04/05). QA ✔ für alle 9 Live-Numerik-Keys inkl. Clamp-Regressionstests für `day_gain_pct`/`probe_max_skip_s` (qa:962-968, 1010-1012). Clamps ✔ (F3-Sweep vollständig). Thread-Lesepfad ✔ (Snapshot unter Lock).

| Lücke | Befund |
| --- | --- |
| Example: mode, time_night_start, time_day_start, sun_latitude, sun_longitude, sun_sunrise_offset_min, sun_sunset_offset_min, boot_settle_s, boot_settle_max_s, boot_stable_pct, night_reconfirm_s, probe_max_skip_s | **12 Keys fehlen komplett** ⚠ F-05 |
| QA: mode, time_*, sun_* (7 live-POSTbare Keys) | ungetestet ⚠ F-08 |
| GET-Pfad `daynight_sun_status()`/`daynight_get_status()` | lock-freie Reads ⚠ F-03 |

### `video<N>.*` (21 Keys, ×2 Streams)

| Key | G | E | W | QA | C |
| --- | --- | --- | --- | --- | --- |
| enabled, codec, width, height, fps, bitrate, rc_mode, gop, profile, min_qp, max_qp, rotation, buffers, rtsp_path, imp_chn, jpeg*, jpeg_quality, jpeg_fps, jpeg_chn | ✔ hal_ingenic.c (u. a. :913-945, :1635-1638, :744-768) | ✔ (video1 nur Teilmenge — Muster erkennbar, ok) | ✔ | bitrate-Persist ✔ qa:1018-1027, rotation ✔ qa:1089ff | ✔ M11-Clamps |
| **qp** | **✖ NIRGENDS gelesen** | ✔ | ✔ („Fixed/initial QP“) | ✖ | ⚠ F-04 |
| **max_gop** | **✖ NIRGENDS gelesen** (`rcAttr.maxGop = v->gop`, hal_ingenic.c:939; ebenso :1635) | ✔ | ✔ | ✖ | ⚠ F-04 |
| buffers_explicit (Struct-Member ohne Key) | ✔ hal_ingenic.c:762-768 | – | – | – | korrekt: reines Runtime-Flag |

### `sim.*` (4 Keys)

Alle gelesen (hal_sim.c). Example ✔, Wiki ✔, QA `–` (host-only). Kosmetik: `config_defaults()` initialisiert `sim_jpeg` nicht explizit (config.c:356) — durch das `memset` abgedeckt, kein Fehler.

---

## Findings

### 🟠 F-01 – `sensor.*`-Numerik per `/control` persistierbar, aber ungeklemmt (M11-Klassen-Lücke)

| Feld | Wert |
|------|------|
| **Datei** | `src/config.c:463-469` (`sensor_fields`: alle `T_INT` mit lo==hi=0), `src/control.c:411-413` + :573-575 (POSTbar) |
| **Schweregrad** | 🟠 Hoch |

Die M11-Härtung („ein Nonsens-fps/width macht die HAL-Init kaputt, main beendet sich, die Respawn-Schleife crasht endlos") hat `videoN.*` (64–4096, 1–120), Ports und `daynight.*` geklemmt — **`sensor.i2c_addr/fps/width/height` aber nicht**, obwohl die Sektion per `{"sensor":{...}}` POSTbar ist und persistiert. Ein einziger fehlerhafter Client-POST (`sensor.width = -5` oder `70000`) landet ungeprüft in `/etc/timps.conf`; beim nächsten Boot gewinnt der Config-Wert über die Registry (config.c:1199-1202 prüfen nur `== 0`), fließt in die ISP-Init (hal_ingenic.c:674-677, 744-745) und kann die Kamera in genau die dokumentierte Respawn-Crash-Schleife bringen. `sensor.model` ist durch den Registry-Override (config.c:1183-1189) geschützt — die Numerik nicht.

**Empfehlung:** Clamps analog videoN (`fps` 0–120, `width`/`height` 0–8192, `i2c_addr` 0–0x7F; 0 = „auto" muss erlaubt bleiben).

### 🟡 F-02 – C11-Data-Race-Klasse besteht in drei weiteren Threads fort

| Feld | Wert |
|------|------|
| **Dateien** | `src/record.c:380, 391-399, 416-418, 429-441, 469-471, 499, 536-537, 630, 648` · `src/timelapse.c:116, 267-268, 291` · `src/rtsp/speaker.c:96, 102-103` · `src/hal/hal_ingenic.c:3133` |
| **Schweregrad** | 🟡 Mittel |

Exakt die in v1.7.7 für `audio.mute` (Audio-Worker) und OSD-`.enabled` (Updater) behobene Klasse — die eigene Konvention in config.h:472-476 („ein Live-Int muss entweder unter dem Lock gelesen werden oder `_Atomic` sein") wird von drei Hintergrund-Lesern weiterhin verletzt, jeweils nur für die **Int**-Felder (die Strings derselben Sektionen werden vorbildlich unter Lock kopiert):

- **rec_thread** liest `record.enabled/mode/channel/segment_s/pre_roll_s/post_roll_s/min_free_mb/audio` lock-frei pro Schleifendurchlauf, während der `/control`-Writer sie unter `config_str_lock` mutiert.
- **timelapse-Thread**: `enabled/channel/interval_s/keep_days` ebenso.
- **speaker/AO-Pfad** (Backchannel-Session- bzw. Play-Thread): `spk_enabled/spk_volume/spk_gain` in `ao_ensure()` und `aec` in `hal_ao_open()` — alle vier live- bzw. POSTbar.

Praktisch heute benign (aligned Loads; die opaken Lock-Aufrufe je Iteration verhindern Hoisting), formal aber UB derselben Klasse. **Empfehlung:** entweder Sektions-Snapshot unter Lock am Schleifenkopf (Daynight-Muster, daynight.c:696-702) oder `F_ATOMIC` für die betroffenen Felder.

### 🟡 F-03 – GET-Pfad-Dumps lesen Live-Ints/Floats lock-frei (Conn-Thread vs. Conn-Thread)

| Feld | Wert |
|------|------|
| **Dateien** | `src/control.c:786-843` (`control_daynight_json`: 15 Live-Numerik-Felder), `:873` (`g_cfg.motion.monitor_stream`), `:1037-1070` (image/audio-Dump), `src/daynight.c:1267-1268, 1281-1289` |
| **Schweregrad** | 🟡 Mittel |

`GET /control` läuft auf einem anderen Conn-Thread als ein gleichzeitiger POST; die numerischen Dumps nehmen den Lock nur für Strings. Gleiche formale Klasse wie F-02, geringere praktische Relevanz (Einmal-Reads, kein Loop-Hoisting möglich). Der ältere Kommentar config.c:42-45 („Everything else … needs no lock") widerspricht der neueren F_ATOMIC-Doktrin in config.h:472-476 — der Kommentar sollte angepasst oder die Reads vereinheitlicht werden.

### 🟡 F-04 – `videoN.qp` und `videoN.max_gop` sind tot (write-only), aber überall dokumentiert

| Feld | Wert |
|------|------|
| **Datei** | `src/config.c:716,718` (Tabelleneinträge mit Clamps), Gegenprobe: kein Read in `src/hal/` — `rcAttr.maxGop`/`yin.maxGop` werden aus `v->gop` gespeist (`hal_ingenic.c:939, 1635`), `v->qp` wird nirgends konsumiert (FIXQP-Modus setzt keinen Init-QP aus der Config) |
| **Schweregrad** | 🟡 Mittel |

Beide Keys werden geparst, geklemmt (1–51 bzw. 1–1000), persistiert, per GET geechot, sind in `timps.conf.example` (Zeilen 224-225) mit Default-Werten und im Wiki mit Wirkbeschreibung („Fixed/initial QP", „Max GOP length") dokumentiert — ein Nutzer, der `rc_mode = fixqp` mit `qp = 30` konfiguriert, bekommt **stillschweigend keinerlei Wirkung**. Entweder die Werte im HAL anschließen (FIXQP-Init-QP, maxGop) oder die Keys als „reserviert/ohne Funktion" kennzeichnen wie `motion.roi_*`.

### 🟡 F-05 – `timps.conf.example`: 12 daynight-Keys fehlen komplett

| Feld | Wert |
|------|------|
| **Datei** | `timps.conf.example:431-456` (daynight-Block) |
| **Schweregrad** | 🟡 Mittel |

Der Block dokumentiert nur den Stand vor den TIME/SUN-Modi und der Probe-Economy: `mode`, `time_night_start`, `time_day_start`, `sun_latitude`, `sun_longitude`, `sun_sunrise_offset_min`, `sun_sunset_offset_min`, `boot_settle_s`, `boot_settle_max_s`, `boot_stable_pct`, `night_reconfirm_s`, `probe_max_skip_s` fehlen sämtlich — gerade die Keys, die nach den Vorfällen vom 2026-08-02…05 die feldrelevanten Stellschrauben sind. Das Wiki hat alle 12; die Beispieldatei ist die auf dem Gerät ausgelieferte Referenz.

### 🟡 F-06 – `timps.conf.example`: `http.adaptive_drop`-Kommentar behauptet veralteten Default

| Feld | Wert |
|------|------|
| **Datei** | `timps.conf.example:86-94` vs. `src/config.c:218-223` |
| **Schweregrad** | 🟡 Mittel |

Die Beispieldatei nennt das Feature „EXPERIMENTAL / opt-in: default OFF until verified on real hardware" und setzt `= 0` — der Code-Default ist seit der Hardware-Verifikation `1` (config.c:218, „hardware-verified"). Das Wiki ist korrekt (Default 1). Wer die Beispieldatei unverändert kopiert, **deaktiviert** damit unbeabsichtigt ein inzwischen empfohlenes Verhalten und liest eine falsche Risikoeinschätzung.

### 🟡 F-07 – `timps.conf.example`: Speaker-Block faktisch falsch, `audio.aec` fehlt, `opus` fehlt

| Feld | Wert |
|------|------|
| **Datei** | `timps.conf.example:253-278` vs. `src/rtsp/speaker.c:89-103`, `src/hal/hal_ingenic.c:2490-2491, 3126-3141` |
| **Schweregrad** | 🟡 Mittel |

Drei Punkte im Audio-Block: (1) „timps has no audio-OUT (AO) pipeline, so the spk_* speaker keys are stored for the WebUI but ignored" — auf `USE_PLAY`/`USE_BACKCHANNEL`-Builds (der timps-Standardausstattung) ist das falsch: `spk_enabled` ist das Master-Gate des physischen Lautsprechers, `spk_volume`/`spk_gain` werden live angewandt. (2) `audio.aec` fehlt in der Beispieldatei vollständig (im Wiki dokumentiert). (3) Der `audio.codec`-Kommentar (`aac | pcmu | pcma | none`) verschweigt das `opus`-Token für `USE_STREAM_OPUS`-Builds (v1.7.7-Feature).

### 🔵 F-08 – QA 8b: Lücken bei live-settbaren Keys

| Feld | Wert |
|------|------|
| **Datei** | `scripts/timps-qa.sh:933-934, 962-968, 974-981` |
| **Schweregrad** | 🔵 Niedrig |

Live per `/control` erreichbar, aber ohne `lv_section`-Round-Trip: `daynight.mode`, `daynight.time_night_start/time_day_start`, die vier `daynight.sun_*`-Keys (7 Keys — der gesamte TIME/SUN-Pfad ist ungetestet), `audio.spk_volume/spk_gain/aec` (build-abhängig, per `caps.audio` erkennbar und damit skippbar) sowie die Live-Strings `record.dir`/`timelapse.dir` (`name` wird getestet, `dir` nicht — dabei ist `dir` der Pfad-Traversal-sensible der beiden). Die bewussten Auslassungen (motion.cols/rows, record.enabled/mode/channel, daynight.enabled) sind im Skript nachvollziehbar begründet und in Ordnung.

### 🔵 F-09 – Live-POSTbare Keys ohne Clamp

| Feld | Wert |
|------|------|
| **Datei** | `src/config.c:481-482, 493, 503` |
| **Schweregrad** | 🔵 Niedrig |

`image.running_mode`, `image.anti_flicker`, `image.core_wb_mode` (live) und `audio.samplerate` (persist) haben lo==hi (kein Clamp). Praktisch entschärft: der HAL behandelt `anti_flicker` ternär (hal_ingenic.c:479-481), `running_mode` boolesch; ein absurder `samplerate` scheitert erst bei der AI-Init nach Restart (hal_ingenic.c:2115 warnt). Der Vollständigkeit halber klemmen (0–1, 0–2, 0–1, 8000–96000).

### 🔵 F-10 – Kleinere Doku-Drifts

| Fundstelle | Befund |
|------|------|
| Wiki `record.post_roll_s` „0–300" · `timps-qa.sh:975` „post_roll_s int 0 300" | Clamp ist **1**–300 (config.c:644-648, Finding-1-Floor). Ein POST von 0 liest als 1 zurück; der QA-Spec-Range suggeriert 0 als gültig (Flip-Midpoint trifft die 0 nie — latent, nicht akut). |
| Wiki `general.osd_pool_size` „(bytes)" | Einheit ist **KB** (`* 1024`, hal_ingenic.c:614); Example sagt korrekt KB. |
| `timps.conf.example:11` `osd_pool_size = 1000` | Code-Default 1024 — zulässig als Beispielwert, aber inkonsistent zur „Default"-Erwartung. |
| `timps.conf.example` video1-Block | max_gop/qp/min_qp/max_qp/buffers/jpeg_quality/jpeg_fps nur für video0 gezeigt — Muster erkennbar, akzeptiert. |

---

## Explizit geprüft und **in Ordnung**

- **Kein Wiki-Geist-Key:** jeder in `Configuration-Reference.md` dokumentierte Key existiert in den Tabellen bzw. im Spezialcode; die deprecated `motion.roi_*` sind korrekt als ignoriert markiert (Code-Warnung config.c:977-984 deckungsgleich).
- **Kein Example-Geist-Key:** alle Keys in `timps.conf.example` werden geparst (inkl. Aliase `video0.jpeg`, `general.syslog`).
- **Struct-Member ohne Key:** nur `ms_vstream_cfg.buffers_explicit` (reines Runtime-Flag, korrekt so) und `motion.roi_*` (Legacy-Spezialcode). Keine „vergessenen" Felder.
- **`F_NOGET`/`noget`-Semantik** stimmt mit dem dokumentierten Dedup-Verhalten überein; `on_motion`/`switch_cmd`/`isp_path` bleiben korrekt unlesbar und nicht POSTbar (Security-Einstufung aus dem Audit 2026-07-23 unverändert gültig).
- **`audio.mute`-Fix (v1.7.7)** verifiziert: `_Atomic` im Struct (config.h:74-83), `F_ATOMIC`-Store/Load (config.c:826-833, 876-878), Reader hal_ingenic.c:2349 + hal_sim.c:141. **OSD-`.enabled`-Fix** verifiziert (imp_osd.c:234-239, hal_ingenic.c sw-rotate analog).
- **Daynight-Thread-Lesepfad** (Ganzstruktur-Snapshot unter Lock je Iteration) ist das Referenzmuster für die F-02-Behebung.

---

## Zusammenfassung nach Kategorie

| Kategorie | Anzahl | Schwerste |
| --- | --- | --- |
| Undokumentiert (Example) | 13 Keys fehlend + 2 falsche Kommentarblöcke + 1 veralteter Default-Text | F-05/F-06/F-07 (mittel) |
| Ungetestet (live, QA 8b) | ~12 Keys (davon 7 = kompletter TIME/SUN-Pfad) | F-08 (niedrig) |
| Tot / write-only | 2 neu (`videoN.qp`, `videoN.max_gop`) + 4 bekannt-dokumentiert (`motion.roi_*`) | F-04 (mittel) |
| Racy (C11-Klasse) | 16 konkrete Felder in 3 Hintergrund-Threads + GET-Pfad-Dumps | F-02/F-03 (mittel) |
| Clamp-Lücken | 4 `sensor.*` (POSTbar!) + 4 kleinere | **F-01 (hoch)** |
