# Unabhängige Code-Prüfung – timps

**Datum:** 2026-08-07
**Prüfumfang:** vollständiger `src/`-Baum (~17.700 LOC C) @ `main` (HEAD `074e8f5`, v1.7.8), Schwerpunkt auf allen Änderungen seit dem letzten Audit-Sweep (2026-07-26, `ad0364a`), insbesondere den **ungeprüften ~65 Commits 2026-07-27 bis 2026-08-06** (speaker.c, backchannel.c, Opus-RTSP, adaptive-drop, HTTP-Digest, Rotate-Härtung, Daynight-Probe-Economy) sowie den **taggleichen** Commits `10a192a`/`074e8f5` (C11-Data-Race-Härtung, v1.7.8).
**Grundlage:** drei unabhängige statische Durchläufe (① C11-Data-Race-Sweep über alle Worker-Threads, ② Fresh-Eyes-Review des neuen Codes `073f652..HEAD`, ③ Re-Verifikation aller Altbefunde aus `CODE_REVIEW.md`, `CODE_AUDIT_2026-07-18.md`, `SECURITY_AUDIT_2026-07-23.md`) plus eine eigene Tiefenprüfung des Data-Race-Commits `10a192a` und ein Build-/Regressionslauf (`make sim`, `make test-auth`).

Wie bei den Vorgänger-Audits gilt: die Commits vom 2026-08-06 stammen vom selben Agenten, der dieses Review beauftragt hat — sie wurden daher mit voller Skepsis unabhängig nachgeprüft, nicht nur abgenickt.

---

## Gesamturteil

Der Codestand ist weiterhin **sehr gut**. **Keine einzige Regression** gegenüber den früheren Audits; mehrere alte Restpunkte (N1, N2, N4, N6, M5-Rest) sind zwischenzeitlich **geschlossen**. Der große neue Code seit Ende Juli (Speaker/Backchannel, Opus, adaptive-drop, HTTP-Digest, Puffer-Wachstum, Rotate-Refusals) ist **speicher- und auth-sauber** — kein KRITISCH-, HOCH- oder speicherrelevanter MITTEL-Befund.

Der wichtigste neue Befund betrifft ausgerechnet den jüngsten Commit: die als „vollständiger C11-Data-Race-Sweep" deklarierte Härtung `10a192a` ist **unvollständig**. Das, was sie anfasst (audio.mute `_Atomic`, Daynight-Snapshot, OSD `.enabled`), ist **korrekt umgesetzt** — aber dieselbe Race-Klasse existiert unverändert weiter in record.c, timelapse.c, den Status-Accessoren und dem GET-/control-Pfad (Details unter A1). Praktisch ist das auf MIPS32 mit aligned Loads benigne; formal bleibt es UB, und der Anspruch „complete sweep" im Commit-Text stimmt nicht.

Builds: `make sim` kompiliert **warnungsfrei** unter `-Wall -Wextra` (Host, C11); `make test-auth` läuft **4/4 PASS** (RTSP fail-closed bestätigt). Cross-Build nutzt ebenfalls `-std=c11`, `_Atomic` ist damit auch auf dem Target sicher.

---

## 1. Verifikation der früheren Befunde (Status im aktuellen Code)

Alle Zeilennummern gegen HEAD `074e8f5` neu verifiziert (Mechanismus geprüft, nicht alte Zeilen geglaubt). **Keine Regression.**

| ID | Thema | Status |
| --- | --- | --- |
| H1 | RTSP-Socket-Timeouts | **bestätigt** – `net_set_timeouts(cfd,30,15)` (rtsp.c:1376) |
| H2 | HTTP-Socket-Timeouts | **bestätigt** – httpd.c:1296; Body-Deadline-Poll httpd.c:1234-1250 |
| H3 | SRT-Passphrase-Fehler | **bestätigt** – Abbruch, kein Klartext-Fallback (srt.c:433-443) |
| H4 | OSD-Canvas-Clamps | **bestätigt** – msttf.c:372-397; font_size 8..256 (config.c:745) |
| H5 | OSD-Region-Discard | **bestätigt** – imp_osd.c:273, 310 |
| M1 | TLS-Handshake-Timeout | **bestätigt** – tls.c:107 |
| M2 | TLS-Mindestversion | **geändert, äquivalent** – nur noch `#if MBEDTLS_VERSION_MAJOR<3` (3.x erzwingt ≥1.2 selbst, API entfernt) (tls.c:64-69) |
| M3 | TLS-Shutdown-Ordnung | **bestätigt** – Registry-`shutdown()` strikt vor `ms_tls_ctx_free()` (rtsp.c:1459-1485) |
| M4 | SRT streamid-Prüfung | **bestätigt** – srt.c:413-418, 431-432 |
| M6 | urandom statt `rand()` | **bestätigt** – rtp.c:15-27, 69-81; rtsp.c:754 |
| M7 | Recorder-fsync | **bestätigt** – record.c:327-352 |
| M9/M10 | OSD Retire-Ring / Item-Snapshot | **bestätigt** – imp_osd.c:32, 214-218, 226-233 |
| M11 | Numerik-Clamps | **geändert, äquivalent** – Umstellung auf tabellengetriebenes config.c (`1e586db`) trägt alle lo/hi-Clamps weiter (config.c:532-587, 710-718) |
| M12 | Daynight monoton | **bestätigt** – `ms_now_us()` durchgängig (daynight.c:592-593, 1197) |
| F-01 | `switch_cmd` via execlp | **bestätigt** – daynight.c:185-190; **kein einziges** `system()`/`popen()` mehr in `src/` |
| NEU-01 | `on_motion` Doppel-Fork | **bestätigt** – imp_motion.c:244-262; Kontext-Env aus `1f10c93` per `setenv()` im Grandchild, keine Shell |
| F-03/F-04 | Audio-/OSD-Clamps | **bestätigt** – config.c:509-521, 738-750 |
| F-08 | `hal_get()` NULL-Check | **bestätigt** – main.c:109-111, vor erster Nutzung inkl. Log |
| N1 (07-18) | Aufnahme-Dauerwerte | **BEHOBEN** – segment_s 0..86400, pre_roll 0..60, post_roll 1..300, min_free_mb geklemmt (config.c:642-649) |
| F3 | daynight-Numerik | **bestätigt** – alle geklemmt (config.c:678-702) |
| M5-Rest | Digest-uri-Prüfung | **BEHOBEN** – `strcmp(uri, req_uri)` gegen echtes Request-Target (auth.c:82, 133; rtsp.c:586, httpd.c:685; Commit `2b9a260`) |
| N2 (07-18) | hvcC numTemporalLayers | **BEHOBEN** – emittiert jetzt `0x0B` (vparam.c:180) |
| N4 (07-18) | RTSP-Transportwerte | **BEHOBEN** – interleaved 0..255, client_port 0..65535, `cp==0` → 461 (rtsp.c:816-843) |
| N5 (07-18) | /control-Torso bei Timeout | **teilweise** – oversize/negative Content-Length jetzt 413 (httpd.c:1221-1230); der Timeout-Torso wird weiterhin geparst (httpd.c:1252). Restrisiko unverändert gering (auth, bereichsgeprüft). |
| N6 (07-18) | APP()-Akkumulator-UB | **BEHOBEN** – `o<cap?buf+o:buf`-Muster (control.c:865-868; httpd.c:814-816); GET /control liefert 500 statt Torso-JSON (`878b940`) |
| F-02/F-12 | isp_path-Präfix | **offen (akzeptiert)** – weiterhin nur per Config-Datei setzbar (F_NOGET, nicht in DN_KEYS) |
| F-09 | getrandom()-Fallback | **offen (akzeptiert)** – urandom primär, dokumentiert schwacher Notfall-Fallback (auth.c:194, 214-218) |
| F-10 | UDP-Ports via rand() | **offen (akzeptiert)** – rtsp.c:788, 861, nicht sicherheitsrelevant |
| F-11 | send_resp-Stackpuffer | **geändert** – jetzt hdr[4096] mit hartem Truncation-Drop statt stillem Abschneiden (rtsp.c:517, 538; `f3160cd`); weiterhin Stack, akzeptiert |

---

## 2. Tiefenprüfung der Commits vom 2026-08-06 (`10a192a`, v1.7.8)

Alle vier Teiländerungen unabhängig nachvollzogen:

- **`F_ATOMIC`/`audio.mute` — korrekt.** Feld ist `_Atomic int` (config.h:74), in der Feldtabelle geflaggt (config.c:517); `field_set()`/`field_get()` routen über `atomic_store`/`atomic_load` (config.c:826-832, 876-879). Alle drei direkten Leser (hal_ingenic.c:2349, hal_sim.c:141, control.c:1060) lesen das `_Atomic`-Feld direkt → implizite atomare Loads. Kein `int*`-Cast am Feld vorbei, kein nebenläufiger memcpy/Struct-Copy von `ms_audio_cfg`; die einzige Ganz-Struct-Kopie (`config_snapshot_boot`, config.c:26) läuft single-threaded vor Thread-Start. Default-Zuweisung config.c:262 ist pre-thread. Cross-Build nutzt `-std=c11` (build.sh:309) — `_Atomic` ist auf dem Target verfügbar.
- **Daynight-Snapshot — korrekt und verhaltensäquivalent.** Der Snapshot (`dncfg` + `running_mode`) wird am Schleifenkopf **vor jeder Verwendung** unter `config_str_lock()` gezogen (daynight.c:696-702); kein einziger `g_cfg.daynight.*`-Rest-Read mehr im Thread (verifiziert per grep: nur noch :588-591, :664, :699-700 — alle unter Lock). Die Signatur-Änderungen (`dn_day_trigger(dn,…)`, `dn_status_update(dn,…)`, `dn_switch(mode,why,cmd)`) sind an **allen** Aufrufstellen durchgezogen. Die Probe-Economy-Logik (v1.7.7) wurde bei der Gelegenheit mitgeprüft: Backoff-Cap-Arithmetik, Oszillations-Ring (Indizes nach Reset korrekt), Fail-Ratchet (`probe_fail_smooth` fängt korrekt den Pre-Probe-Nachtwert, da `smooth_tg` nur bei `cur==NIGHT` aktualisiert und erst **nach** der Buchung zurückgesetzt wird) — **kein Logikfehler gefunden**.
- **OSD `.enabled` in `refresh_text()` — korrekt.** Beide anderen Aufrufstellen (imp_osd.c:420 Setup, :570 item_apply) prüfen `enabled` weiterhin selbst vor dem Aufruf → der Check im Snapshot ist dort ein harmloser Doppelcheck; das Verstecken der Region bei Disable läuft unverändert über `ShowRgn` (imp_osd.c:566). **Kein Pfad überspringt den Check, kein Timing-/Verhaltensunterschied.**
- **Kleinänderungen:** aac.c-Warnung, vparam.c-/rtsp.c-Kommentare unbedenklich. **Aber:** die neuen `cmfc`/`cmf2`-Brands im ftyp sind fachlich fragwürdig → Befund A3.
- **`fad4f40` (v1.7.7) „persist clamped values":** korrekt — der kanonische AFTER-Read unter Lock speist HAL, SSE-Echo und Persist einheitlich; der ungate-te AFTER-Read (Kommentar zum osdN.*-Konvergenzfall) ist schlüssig begründet.

---

## 3. Neue Befunde

Kein KRITISCH, kein HOCH. Ein MITTEL (Klasse), Rest NIEDRIG/INFO.

| ID | Schwere | Thema | Ort |
| --- | --- | --- | --- |
| A1 | 🟠 MITTEL | C11-Sweep unvollständig: live-mutable Ints weiterhin lockfrei gelesen | record.c, timelapse.c, imp_osd.c, speaker.c, rtsp.c, daynight.c, control.c |
| A2 | 🟡 MITTEL | OSD liest `video[si].rotation` aus dem **Live**-`g_cfg` statt `g_cfg_boot` | imp_osd.c:202, 263, 308 |
| A3 | 🟢 NIEDRIG | `cmfc`/`cmf2`-Brands auf gemuxtem A/V-fMP4 nicht CMAF-konform | fmp4.c:366-371 |
| A4 | 🟢 NIEDRIG | Digest-Nonce-Ring (32) durch unauth. 401-Spam verdrängbar | httpd.c:619 |
| A5 | 🟢 NIEDRIG | Digest-`uri=`-Strictness: Kompatibilitätsrisiko für path-only-Clients | auth.c:82, 133 |
| A6 | ⚪ INFO | Zwei veraltete Kommentare widersprechen dem Code | config.c:42-45; control.c:583 |

**A1 — Lock-freie Reads live-mutabler Config-Ints (die eigentliche Sweep-Lücke).** `10a192a` behauptet einen vollständigen Data-Race-Sweep; tatsächlich bleiben ganze Klassen übrig (Schreiber ist immer der /control-Conn-Thread unter `config_str_lock`):
- **record.c (größter Einzelposten):** der Recorder-Thread liest *alle* `record.*-Ints* pro Durchlauf lockfrei — `enabled`/`mode` in `want_run()`/`want_write()` (record.c:391, 398-399), `channel`/`pre_roll_s` (:416-441), `post_roll_s` (:380, per-Paket im Motion-Modus), `segment_s` (:536-537), `min_free_mb` (:173-193), `record.audio` (:260, 469-471). Der Kommentar „re-read live every pass" (:416) dokumentiert das Muster sogar — es ist exakt das „Load-hoistbar-aus-der-Schleife"-Szenario, mit dem der Commit selbst die `_Atomic`-Umstellung von `audio.mute` begründet (config.h:74). *Fehlerbild:* formal UB; der Compiler dürfte z. B. `record.enabled` aus der Schleife hoisten und ein Live-Disable nie sehen.
- **timelapse.c:** analog `channel`/`enabled` (:267-268), `interval_s` (:291), `keep_days` (:116) — Strings sind korrekt gelockt, nur die Ints wurden übersehen.
- **speaker.c/hal_ingenic.c:** `spk_enabled`/`spk_volume`/`spk_gain` in `ao_ensure()` (speaker.c:96-103, ausdrücklich „Read live from g_cfg"), `audio.aec` in `hal_ao_open()` (hal_ingenic.c:3133) — kalt (pro AO-Open).
- **rtsp.c:** `audio.bitrate_kbps`/`samplerate`/`channels` in SDP/PLAY (rtsp.c:438, 458, 938) — Restart-only-Keys werden von `config_apply_kv` trotzdem ins **Live**-`g_cfg` geschrieben, der Read raced also; schlimmstes Praxisbild ein gemischter `b=AS:`-Wert.
- **Status-Accessoren (GET /control/SSE-Threads):** `daynight_get_status`/`daynight_sun_status` (daynight.c:1267-1290, inkl. zweier **float**-Reads `sun_latitude/longitude` — floats können auf MIPS auch praktisch tearen, wenn der Compiler sie in zwei Zugriffe zerlegt), `record_get_status` (record.c:582-584), `timelapse` (:333-334), Sim-Stub `motion_get_status` (imp_motion.c:521-529), sowie der gesamte Int-Teil von `control_get_json` (control.c:~1037-1110).
- **Concurrent-POST-Klasse:** `control_apply_json` ist **nicht serialisiert** (httpd.c:1252, ein Thread pro Verbindung), und `hub_control()` läuft *nach* `config_str_unlock()` (control.c:289/327). Zwei gleichzeitige POSTs → Apply-Pfad von A liest `g_cfg` lockfrei gegen die gelockten Writes von B (hal_ingenic.c:436, 591, 2041ff, 2490-2491, 2607-2611, 2678-2708; imp_motion.c:118-152; imp_osd.c:344, 554, 595-601).

*Einordnung:* alles formal UB, praktisch auf MIPS32 (aligned 32-bit-Loads) benigne und teils seit Jahren latent — daher MITTEL für die Klasse, nicht HOCH. *Fix-Empfehlung:* (1) record/timelapse: Section-Snapshot unter Lock einmal pro Pass (Daynight-Muster) oder die 4-5 Schleifen-Gates `_Atomic`; (2) ein einzelner Mutex um `control_apply_json` kollabiert die gesamte Concurrent-POST-Klasse; (3) Speaker/AEC/SDP/Status-Accessoren: je ein kurzer Lock+Copy. Ursache ist vermutlich der **veraltete Kommentar config.c:42-45** („Ints are not covered … no tearing to worry about"), der dem neuen, korrekten Kommentar config.h:472-476 direkt widerspricht (→ A6).

**A2 — OSD nutzt Live-Rotation statt Boot-Snapshot.** `osd_rot_place()`/`refresh_text()`/Logo-Pfad lesen `g_cfg.video[si].rotation` (imp_osd.c:202, 263, 308) aus dem **Live**-Config. `videoN.rotation` ist restart-only (`VID_REST`) — genau dafür wurde `g_cfg_boot` eingeführt (Commit `288047a`). Doppeldefekt: (a) dieselbe Race-Klasse wie A1, (b) *funktional*: ein `/control`-Write auf `videoN.rotation` re-platziert/re-paddet ab der nächsten Text-Aktualisierung das OSD für eine Rotation, die der laufende Encoder **nicht** produziert — Overlay sitzt bis zum Neustart falsch. *Fix:* die drei Stellen auf `g_cfg_boot` umstellen (behebt Race und Desync zugleich).

**A3 — CMAF-Brands auf Multi-Track-fMP4.** `10a192a` fügt `cmfc`/`cmf2` in die compatible-brands des ftyp ein (fmp4.c:366-371), erklärtes Ziel: strenge Validatoren (Bento4) zufriedenstellen. CMAF (ISO/IEC 23000-19) verlangt aber **einen Track pro CMAF-Datei**; dieser Muxer schreibt Video- **und** Audio-trak in dasselbe Movie (fmp4.c:376-377). Die Brand-Behauptung ist damit für den A/V-Fall falsch — genau die adressierten strikten Validatoren würden sie monieren. Browser ignorieren compatible-brands, daher rein kosmetisch. *Fix:* Brands nur setzen, wenn genau ein Track aktiv ist (Video-only-Streams wären konform), oder wieder entfernen.

**A4 — Nonce-Ring-Eviction (unauthentifiziert erreichbar).** Jede 401-Antwort (auch fehlgeschlagene Basic-Versuche) prägt und speichert eine neue Digest-Nonce im 32er-Ring (httpd.c:619). >32 unauth. Requests rotieren den Ring und verdrängen ausstehende legitime Challenges → ehrlicher Digest-Client bekommt `stale=true` und braucht eine Extra-Runde. Selbstheilend, kein Bypass, Basic unberührt — inhärente Eigenschaft eines begrenzten Rings, nur zur Kenntnis.

**A5 — Digest-uri-Strictness.** Der neue `strcmp(uri, req_uri)` (auth.c:82, 133) schließt Cross-URI-Replay korrekt, lehnt aber Clients ab, die (RFC-widrig, aber verbreitet) nur den Pfad statt der vollen `rtsp://…`-URI in `uri=` senden; `extract_url` trunkiert zudem still bei 512. Kompatibilitäts-, kein Sicherheitsrisiko — bei Interop-Problemen mit exotischen NVRs hier zuerst suchen.

**A6 — Veraltete Kommentare.** (a) config.c:42-45 behauptet noch, Int-Reads bräuchten keinen Lock — direkter Widerspruch zu config.h:472-476 und mutmaßliche Ursache von A1; (b) control.c:583 nennt für `on_motion` noch `system()`, der Code nutzt längst fork/execlp. Beide angleichen.

---

## 4. Fresh-Eyes-Ergebnis neuer Code (2026-07-27 → 2026-08-06) — geprüft und sauber

Alle gezielt nachgeprüften Punkte des neuen Codes sind korrekt umgesetzt (Auswahl, je verifiziert an aktueller Zeile):

- **speaker.c:** `rs_fit`-Wachstum exakt auf Worst-Case 8k→48k begrenzt (`SPK_RS_MAX`=49152), `ms_resample` clamp auf `out_cap`, `rn>0`-Guards an beiden Schreibstellen (speaker.c:137, 377); Sound-Namen via `sound_path()` gegen `/`, `..`, `S_ISREG` validiert (control.c:60).
- **backchannel.c:** M-B1-Headroom hält (cap 8192, 2048-Block-Gate; libhelix **ohne SBR** gebaut, 1024-Samples-Konstante verifiziert); `rtp_payload_off` (CSRC/Extension/Padding) vollständig bounds-geprüft (backchannel.c:105).
- **rtsp.c:** Content-Length-Konsum beidseitig geklemmt und durch H2-Timeouts begrenzt (rtsp.c:1161, 1317); `send_resp` hart gegen Truncation (517, 538); `sendmmsg`-Batch ohne Double-Free/UAF, ENOSYS-Fallback korrekt; Idle-Reaping keyed auf echte TCP-Writes.
- **httpd.c Digest (RFC 7616):** nc streng steigend, TTL, req-uri-Bindung, constant-time Hex-Vergleich, Nonce-Tabelle unter Mutex.
- **rtp.c/Opus:** MTU geklemmt [548,1472], alle Puffer `RTP_MTU_MAX+32`; Opus: kein Split, obuf 4096 ≫ 1275-Max, 48k-Klock/1920-Ticks korrekt per RFC 7587; SDP `opus/48000/2` + `sprop-stereo=0` korrekt.
- **hal_ingenic.c:** AU-/JPEG-Puffer-Wachstum size_t-sauber mit 1-MB-Kappe, Realloc-Fehler droppt statt crasht; alle Rotate-Refusals (T23 sw, T31 FS) kehren **vor** jeglicher IMP-Ressourcenanlage um bzw. räumen in umgekehrter Reihenfolge auf — kein Leak.
- **fanqueue.c Byte-Cap / timelapse JIT-Subscribe:** Buchhaltung auf allen Pfaden konsistent, kein Leak/Deadlock.
- **265befb (Silent-Limbo-Sweep):** keine eingeschleppten OOB/UAF.
- **osd_vars `{fpsN}`:** Kanalindex doppelt begrenzt (0-9 **und** `<MS_MAX_VSTREAM`).

---

## 5. Build & Regression (Host)

| Prüfung | Ergebnis |
| --- | --- |
| `make sim` (`-std=c11 -Wall -Wextra`, USE_CONTROL/DAYNIGHT/RECORD/TIMELAPSE) | **baut warnungsfrei** |
| `make test-auth` (Sim-Harness, RTSP/HTTP fail-closed) | **PASS=4 FAIL=0 SKIP=2** (Skips designbedingt: Loopback-Trust, Backend-503 nach Auth) |

Kein Kamera-Flashen im Rahmen dieses Reviews (source-only); die Hardware-Verifikation der v1.7.7-Features ist laut Commit-Log separat erfolgt.

---

## Empfohlene Reihenfolge

1. **A1 (record.c/timelapse.c-Kern):** Schleifen-Gates (`record.enabled/mode/channel`, `timelapse.enabled/interval_s`) snapshotten oder `_Atomic` machen — die einzige Position mit realem Fehlhoisting-Potenzial.
2. **A2:** imp_osd.c-Rotation auf `g_cfg_boot` — behebt einen echten Funktionsfehler (Live-Rotation-Edit verschiebt OSD) gleich mit.
3. **A1 (Rest):** ein Mutex um `control_apply_json` (kollabiert die Concurrent-POST-Klasse), danach die kalten Einzel-Reads (speaker/aec/SDP/Status) je per Lock+Copy; Kommentar config.c:42-45 korrigieren (A6).
4. **A3:** `cmfc`/`cmf2` nur bei Single-Track setzen oder entfernen.
5. **A4/A5:** nach Kapazität; A5 nur bei realen Interop-Beschwerden.

Keine dieser Positionen blockiert einen Produktiveinsatz; v1.7.8 kann aus Sicht dieses Audits ausgerollt werden.
