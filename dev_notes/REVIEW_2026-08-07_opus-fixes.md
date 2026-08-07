# Adversariales Review – Opus-Fixes zu den Audits vom 2026-08-07

**Datum:** 2026-08-07
**Prüfgegenstand:** uncommitteter Diff im Worktree `agent-a879c6c77c61abd29` (Basis = `main` @ `074e8f5`, 16 Dateien, +542/−162), erstellt von Opus als Umsetzung von `CODE_AUDIT_2026-08-07.md` (A1/A2/A3/A6), `config-completeness-audit-2026-08-07.md` (F-01…F-10), `PERFORMANCE_AUDIT_2026-08-07.md` (P-03/P-04/P-08) und dem sdk-feature-gaps-Refresh.
**Methode:** vollständige Diff-Lektüre, gezielte Hand-Nachprüfung aller fünf von Opus selbst markierten Risikostellen, Leftover-Greps auf jede Snapshot-Refaktorierung, eigener Build (`make sim`, `make test-auth`) **und** eigener `-fsyntax-only`-Lauf der drei Nicht-Sim-Dateien gegen die T31-1.1.6-Header (Behauptungen nicht geglaubt, sondern reproduziert).

---

## Gesamturteil

**Merge-fähig.** Kein blockierender Befund. Alle im Opus-Report behaupteten Fixes sind im Diff tatsächlich und korrekt vorhanden; die fünf Risikostellen halten der Tiefenprüfung stand. Die explizit übersprungenen Posten (P-01, P-02, SDK #5/#6) sind sauber übersprungen — **kein** halb angefangener Code, keine toten Reste. Zwei neue NIEDRIG/INFO-Befunde (R-01, R-02), nichts davon merge-blockierend.

Builds selbst verifiziert: `make sim` **warnungsfrei** (`-Wall -Wextra`, C11), `make test-auth` **PASS=4 FAIL=0 SKIP=2** (identisch zur Audit-Baseline). `imp_osd.c`/`hal_ingenic.c`/`speaker.c` bestehen `-fsyntax-only` gegen T31-Header warnungsfrei (eigener Lauf, nicht der von Opus).

---

## Verdikt je Posten

### Tier 1

| Posten | Verdikt | Nachweis |
| --- | --- | --- |
| A2 – OSD-Rotation auf `g_cfg_boot` (5 Stellen) | ✅ **BESTÄTIGT** | grep: **kein** `g_hcfg->video[..].rotation`-Rest in imp_osd.c; alle 5 Stellen (osd_rot_place :207, refresh_text :268, setup_logo :313, setup_cover :359, Setup-Warnung :411) auf `g_cfg_boot`. `g_cfg_boot` via `imp_osd.h → ../config.h` sichtbar; `config_snapshot_boot()` (main.c:107) läuft vor HAL-Setup und vor `record_start`/`timelapse_start` (main.c:160-161) — keine Read-before-Snapshot-Lücke. |
| A2-Scope-Erweiterung (5 statt 3 Stellen) | ✅ **korrekt, kein Overreach** | `setup_cover` (Privacy-Höhen-Clamp + Even-Align) und die Setup-Warnung hängen von der Rotation ab, die der **laufende** Encoder produziert — exakt dieselbe restart-only-Semantik und dieselbe Race-Klasse wie die drei im Audit genannten Stellen. Das Audit hat die zwei schlicht übersehen; die Erweiterung ist die vollständigere Umsetzung desselben Fixes. |
| F-01 – `sensor.*`-Clamps | ✅ **BESTÄTIGT** | i2c_addr 0–0x7F (7-Bit-Adressraum vollständig), fps 0–120, width/height 0–8192; lo=0 erhält die „0 = auto“-Semantik. Clamp greift tabellengetrieben für Datei-Load **und** POST. |
| A1/F-02/F-03 – C11-Sweep-Vervollständigung | ✅ **BESTÄTIGT** | Leftover-Greps sauber: alle verbliebenen `g_rc->record.*`/`g_tc->timelapse.*`-Reads liegen **innerhalb** von Lock-Blöcken (Strings :180/:231-232/:123/:160-161; Status/Clip-Ints in den neuen Lock-Blöcken :604-607/:650-651/:349-351). `rec_thread`-Snapshot korrekt durch `want_run`/`want_write`/`motion_recent`/`seg_open`/`prune_free` gefädelt (Signaturen an allen Aufrufstellen nachgezogen, `(void)rc` im `!USE_CONTROL`-Stub). timelapse analog. speaker.c/hal_ingenic.c/rtsp.c/Status-Accessoren: Lock+Copy vorhanden, Rest der jeweiligen Funktion nutzt konsistent die lokale Kopie. `control_get_json`: **kein** `c->image.*`/`c->audio.*`-Rest (grep leer). |
| `apply_mu` in `control_apply_json` | ✅ **BESTÄTIGT — deadlockfrei** | Vollständige Return-Enumeration der Funktion (control.c:357-659): exakt **drei** Ausgänge — (1) `!json`-Early-Return **vor** dem Lock, (2) OOM-Pfad mit `unlock` vor `return` (:425), (3) Funktionsende mit `unlock` (:658). Kein `goto`, kein weiterer `return`. Lock-Ordnung geprüft: `apply_mu → config_str_lock` bzw. `apply_mu → speaker-g_lock → config_str_lock`; `config_str_lock` ist rekursiv und umschließt nirgends einen Call in speaker/control zurück — keine Inversion. |
| A6 – zwei Kommentare | ✅ **BESTÄTIGT** | config.c-Doktrin-Kommentar ersetzt (widerspricht config.h nicht mehr), control.c `on_motion` jetzt korrekt fork+execlp. |

### Tier 2

| Posten | Verdikt | Nachweis |
| --- | --- | --- |
| F-04 – qp/max_gop als RESERVED statt Verdrahtung | ✅ **BESTÄTIGT, begründete Entscheidung** | Kein HAL-Konsument (Audit-Gegenprobe F-04 bestätigt: `rcAttr.maxGop = v->gop`), Verdrahtung bräuchte den Rate-Control-Umbau aus SDK-Gap #1 — ohne Hardware nicht verifizierbar; die `motion.roi_*`-Analogie trägt. Warn-Mechanik funktioniert: set_kv-Video-Branch, `key+7` ist für `video0.`/`video1.` (je 7 Zeichen) korrekt, One-Shot via `static`. **Aber → R-01.** |
| F-05 – 12 daynight-Keys im Example | ✅ **BESTÄTIGT** | Alle 12 vorhanden; Beispielwerte deckungsgleich mit `config_defaults()` (5/120/20/3600/43200, mode=sensor). |
| F-06 – adaptive_drop | ✅ **BESTÄTIGT** (Code-Default 1, config.c:224) |
| F-07 – Speaker-Block/aec/opus | ✅ **BESTÄTIGT** |
| F-08 – QA-Erweiterungen | ✅ **BESTÄTIGT, Custom-Block-Begründung trägt** | Der generische `lv_section` wäre für den TIME/SUN-Pfad tatsächlich falsch: `mode` wird als `mode` gePOSTet, echot aber als `dn_mode` (Read-back-Mismatch), `time_*_start` sind ≤5-Zeichen-HH:MM-Puffer (das 8-Zeichen-`qa_probe` würde trunkieren), lat/long sind Floats. Format-Kompatibilität handverifiziert: `%g` emittiert `52.5`/`13.5` → String-Vergleiche matchen; `T_DNMODE` parst/emittiert `sensor|time|sun` symmetrisch. Interruption-Safety: `LV_PENDING` wird **vor** dem Probe-POST scharfgestellt und erst nach gelandetem Restore entwaffnet; die etablierten `EXIT`/`INT`/`TERM`-Traps (`lv_restore_pending`, qa:834-836) greifen — Konvention eingehalten. spk-Gate ist echt: `caps.audio` listet `spk_volume` nur unter `USE_PLAY||USE_BACKCHANNEL` (control.c AUD_CAPS). |
| F-09 – 4 Clamps | ✅ **BESTÄTIGT** (0–1, 0–2, 0–1, 8000–96000) |
| F-10 – 3 Doku-Fixes | ✅ **BESTÄTIGT** (Wiki 1–300, Wiki KB, Example 1024; QA-Spec-Range auf 1–300 nachgezogen) |

### Tier 3

| Posten | Verdikt |
| --- | --- |
| A3 – cmfc/cmf2 entfernt | ✅ **BESTÄTIGT** — sauberer Revert mit korrekter Begründung im Kommentar; `box_close` rechnet die ftyp-Größe ohnehin nach, keine Size-Buchhaltung betroffen. |

### Tier 4

| Posten | Verdikt | Nachweis |
| --- | --- | --- |
| P-08 – FQ_MAX_BYTES nur T10/T20/T21 | ✅ **BESTÄTIGT per `make -n`** | T20: `-DFQ_MAX_BYTES=1048576` vorhanden; **T31: 0 Treffer, T41: 0 Treffer**; T21: 1 Treffer; T10-Branch trägt die Zeile (Makefile:92). `fanqueue.c:14` hat den `#ifndef`-Guard → `-D` kollidiert nicht. `make sim` unberührt (kein `$(PLATFORM_CFLAGS)` im sim-Target). Keine Regression für T31/C100/T40/T41. |
| P-03/P-04 – now-Cache + Ctl-Poll-Gate | ✅ **BESTÄTIGT** | `polled`-Flag schützt den `errno`-Check korrekt: ungepollt ist `n=-1, polled=0` → weder `n==0`-Break noch errno-Auswertung — der Stale-errno-Break ist konstruktionsbedingt unmöglich; eine tote Verbindung wird beim nächsten Poll (≤~150 ms) erkannt, eine lebendige nie fälschlich gekappt. TEARDOWN-Latenz worst case ~150 ms (50-ms-Gate + `now` bis zu `pop_ms`=100 ms alt, da vor dem Pop gelesen) — weit unter jedem RTSP-Keepalive-Fenster. Backchannel pollt via `ctl_poll_every` jede Iteration; `s->have_bc` nur unter `#ifdef USE_BACKCHANNEL` referenziert. Die TEARDOWN-BYE-Timestamps holen sich korrekt ein frisches `ms_now_us()` (`bnow`). |

### Übersprungene Posten (Negativ-Verifikation)

| Posten | Verdikt |
| --- | --- |
| P-01 (`hub_publish_take`) | ✅ **sauber NICHT angefasst** — kein Diff in hub.c/frame.c, keine API-Reste. Begründung (11 statt 3 Call-Sites, UAF-Risiko ohne Hardware-Soak) ist nachvollziehbar und deckt sich mit der Audit-Einstufung „mittel". |
| P-02 (Stop-Condvar) | ✅ sauber nicht angefasst (im Audit selbst hinter P-01 einsortiert). |
| SDK #5/#6 | ✅ sauber nicht angefasst — kein `GetChnEvalInfo`/`SetAe_IT_MAX`-Code, keine neuen `PLATFORM_`-Conditionals im Diff. |

---

## Neue Befunde (durch den Fix eingeführt bzw. sichtbar geworden)

| ID | Schwere | Befund |
| --- | --- | --- |
| R-01 | 🟢 NIEDRIG | **Example widerspricht der neuen F-04-Warnung.** `timps.conf.example:221/223` liefert `video0.max_gop = 60` und `video0.qp = 35` weiterhin als aktive Beispielzeilen aus. Da die neue One-Shot-Warnung auch beim **Datei-Load** feuert (set_kv ist der gemeinsame Pfad), loggt jede aus dem Example abgeleitete Config bei jedem Boot „reserved and IGNORED“. Nur das Wiki wurde als RESERVED markiert, das Example nicht. *Fix (Follow-up):* beide Zeilen im Example auskommentieren bzw. mit „reserved, no effect“ annotieren. Kein Merge-Blocker (die Warnung ist inhaltlich korrekt und genau einmal pro Prozess). |
| R-02 | ⚪ INFO | **QA-`dir`-Probe nutzt einen relativen Pfad.** `lv_section record … "dir str"` POSTet `qa_probe` als Live-`record.dir`/`timelapse.dir`. Läuft auf dem QA-Gerät währenddessen eine aktive Aufnahme in eine Segment-Rotation, entstünde kurzzeitig `/qa_probe/<host>/records/…` auf dem Rootfs (Daemon-cwd `/`). Fenster ist klein (POST→GET→Restore), Rotation nur am Keyframe — praktisch unwahrscheinlich, aber ein absoluter `/tmp/...`-Probewert wäre die sauberere Wahl. |
| R-03 | ⚪ INFO | **Neue Lock-Kadenz im Recorder:** der Per-Pass-Snapshot nimmt `config_str_lock` einmal pro Schleifeniteration ≈ einmal pro Paket (25–75/s). Haltezeit ist eine Struct-Copy (sub-µs), Kontention nur während /control-Writes — deckungsgleich mit der P-09-Einschätzung des Perf-Audits, kein Handlungsbedarf. Nur dokumentiert, damit ein späterer Profiler-Fund nicht überrascht. |
| R-04 | ⚪ INFO | `hal_ingenic.c:2490-2491` (`speaker_set_volume(g_hcfg->audio.spk_volume)` im Apply-Pfad) liest weiterhin lockfrei — das ist jetzt **korrekt**, weil `apply_mu` Apply-vs-Apply serialisiert und der Lesende damit derselbe Thread ist, der den Wert soeben geschrieben hat (kein nebenläufiger Writer mehr). Ein kurzer Kommentar an der Stelle („safe: serialized by apply_mu“) würde das gegen künftige Audits absichern. |
| R-05 | ⚪ INFO | Die QA-TIME/SUN-Probe kann auf einer echten Kamera einen einmaligen IR-Cut-Toggle auslösen (mode=sun mit Berlin-Koordinaten, je nach Uhrzeit), der beim Restore zurückschwingt. Das ist die vom Audit (F-08) explizit angeforderte Abdeckung; transient und akzeptiert. |

Positiv hervorzuheben (über die Audits hinaus korrekt): die Pre-Roll-Warnung in `rec_thread` liest `bitrate_kbps`/`fps` jetzt aus `g_cfg_boot` statt live — für restart-only-Keys die richtige Quelle (Warnung reflektiert, was der Encoder tatsächlich produziert) und nebenbei racefrei; im Opus-Report nicht einmal erwähnt.

---

## Empfehlung

**Mergen.** R-01 als Mini-Follow-up (zwei Example-Zeilen auskommentieren) direkt danach oder im nächsten Doku-Pass; R-02/R-04 nach Kapazität. Vor dem nächsten Release wie üblich ein QA-Lauf gegen eine echte Kamera (Sektion 8b inkl. der neuen TIME/SUN- und spk-Blöcke), da `imp_osd.c`/`hal_ingenic.c`/`speaker.c` bauartbedingt nur syntax-geprüft sind.
