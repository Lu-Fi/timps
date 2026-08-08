# timps – Performance- & Ressourcen-Audit 2026-08-07

**Datum:** 2026-08-07
**Prüfumfang:** ausschließlich Performance, Speicher-Footprint, CPU-/Syscall-Last, Thread-/Wakeup-Budget und Binärgröße. **Kein** Security-/Korrektheits-Audit (laufen parallel, siehe `CODE_AUDIT_2026-07-18.md` / `SECURITY_AUDIT_2026-07-23.md`).
**Grundlage:** statische Analyse des vollständigen `src/` (~19.900 LOC C) auf `main`, Stand v1.7.7. Keine Live-Messung; Kostenabschätzungen aus Code-Struktur, Allokationsmustern und den bekannten Plattformkosten (MIPS 3.10 **ohne vDSO** → jeder `clock_gettime`/`recv` ist ein echter Syscall à ~2–5 µs, vgl. Kommentar rtsp.c:87-94).

---

## Gesamturteil

Der Code ist für ein Embedded-Daemon **bereits ungewöhnlich weit durchoptimiert**. Praktisch jede „klassische“ Fundstelle eines solchen Audits ist schon adressiert und im Code als frühere Optimierungsrunde dokumentiert:

- **Zero-Copy-Fanout** über refcountete Pakete (`frame.c`), Publish-Skip bei 0 Abonnenten (hub.c:247), Byte-Budget statt nur Slot-Cap pro Queue (fanqueue.c:14-16, S2).
- **Syscall-Batching**: TCP-interleaved RTP in einem `write` (rtsp.c:148-165), UDP-RTP per `sendmmsg`-Batch (P3, rtsp.c:87-143), SRT-TS in 1316-B-Blöcken statt 188-B-Einzelsends (srt.c:130-141), Recorder über gepufferte `FILE*`-Writes mit asynchronem `sync_file_range` (record.c:326-340).
- **On-Demand-Pipelines**: Encoder/Framesource/Audio blocken idle auf einer Condvar (`act_wait`, hal_ingenic.c:280-300), OSD-Updater rastert nur bei Konsumenten (imp_osd.c:458-469), `/events` ist condvar-getrieben statt gepollt (events.c).
- **Allokations-Hygiene**: AU-/JPEG-Puffer wachsen bedarfsgesteuert statt Worst-Case-vorab (hal_ingenic.c:979-987), persistente Fragment-Scratchbuffer mit Soft-Shrink (record.c:298-299, fmp4.c:450, `ms_buf_reset`), Font-Cache statt Mehrfach-Parse (imp_osd.c:87-121), /proc-Leser mit 1-s-TTL-Cache (osd_vars.c:63-143), ISP-Scrape gedrosselt (P2, daynight.c:389-391).
- **Explizite Thread-Stacks** (S3, util.h:55-74) statt 8-MB-RLIMIT-Default; Binary mit `-Os -ffunction-sections -Wl,--gc-sections` + strip (build.sh:299-352), alle schweren Abhängigkeiten (TLS/SRT/FAAC/Opus/TTF) compile-gated.

Die real gemessene Basis (QA-Soak 2026-07-18: ~5 MB RSS stabil, fps-stabil bis 4 Clients) bestätigt das. **Es gibt keinen „low-hanging“ Groß-Gewinn mehr.** Die verbleibenden Punkte unten sind inkrementell: ein mittlerer struktureller Kandidat (P-01), ein Wakeup-Budget-Thema (P-02) und eine Handvoll kleiner Syscall-/Kopier-Trimmer. Nichts davon blockiert den Produktiveinsatz auf T10/T20-Klasse-Hardware.

---

## Befunde

| ID | Sev | Thema | Datei:Zeile | Geschätzter Impact | Aufwand |
|----|-----|-------|-------------|--------------------|---------|
| P-01 | 🟠 MITTEL | Doppelte Frame-Kopie + malloc/free pro publiziertem Frame | hal_ingenic.c:1122-1139 → hub.c:249 → frame.c:7-17 | ~0,5–1,5 MB/s memcpy + ~50–100 malloc/free/s pro aktivem Stream eingespart; grob 1–3 % CPU auf T21-Klasse, weniger Heap-Fragmentierung auf 32-MB-SoCs | mittel |
| P-02 | 🟠 MITTEL | Idle-Wakeup-Budget ~25/s durch Slice-Sleeps statt Condvar-Stop | imp_osd.c:513, daynight.c:289-296, record.c:454, timelapse.c:273/287, main.c:167 | ~20 Wakeups/s (≈40 Kontextwechsel/s) im Leerlauf entfernbar | klein–mittel |
| P-03 | 🟡 NIEDRIG | Mehrfache `clock_gettime`-Syscalls pro Play-Loop-Iteration (kein vDSO!) | rtsp.c:1042-1117 (`ms_now_us` 3-5×/Iter.), analog httpd.c-Streamloops | ~100–200 Syscalls/s bei 2 Clients à 25 fps → ~0,5–1 ms CPU/s | klein |
| P-04 | 🟡 NIEDRIG | Control-Socket-Poll (`MSG_DONTWAIT`-recv) jede Iteration = 1 EAGAIN-Syscall pro Medienframe | rtsp.c:1128 | ~40–50 Syscalls/s pro RTSP-Client vermeidbar (Gate auf ~50 ms) | klein |
| P-05 | 🟡 NIEDRIG | TS-Paketierung kopiert PES-Payload byteweise statt `memcpy` | srt.c:263-265 | ~0,5 MB/s/Client byteweise auf MIPS ≈ <1 % CPU; mit memcpy ~4–8× schneller | klein |
| P-06 | 🟡 NIEDRIG | MJPEG-Multipart: 2 `send()` pro Frame (Part-Header + JPEG) | httpd.c:580-581 | 1 Syscall/Frame gespart (~10–25/s pro MJPEG-Client) via zusammenhängendem Puffer/`writev` | klein |
| P-07 | ℹ️ INFO | UDP-Video-Session: 23,5 KB `rtp_batch` (16×1472) + Batch-Zwischenkopie | rtsp.c:95-101, 190 | ~24 KB Heap pro UDP-Video-Client; bewusster Tausch gegen ~170 gesparte Syscalls pro IDR – **so lassen** | – |
| P-08 | ℹ️ INFO | Fanqueue-Worst-Case-Pinning 2 MB pro gestalltem Client | fanqueue.c:14-16 | 8 HTTP + 8 RTSP + 8 SRT gestallt = theoretisch ~48 MB; auf T10/T20/T21-Builds `-DFQ_MAX_BYTES=1048576` (und ggf. QCAP-Halbierung) empfohlen | trivial (Buildflag existiert) |
| P-09 | ℹ️ INFO | `config_str_lock`-Kadenz nach den Daynight/OSD-Härtungen (Review-Frage) | daynight.c:696-701, imp_osd.c:230-233 | 2 Locks/s (284-B-Copy) + ≤16 Locks/s (428-B-Copies); Haltezeit sub-µs, Kontention nur während /control-Writes. **Generation-Counter lohnt nicht** | – |
| P-10 | ℹ️ INFO | TTF-Neu-Rasterung 1×/s pro Sekunden-auflösendem Text-Item | imp_osd.c:222-294, msttf.c | Bereits gegated (osd_needed, Change-Detection, supersample=2). Minuten-Auflösung im Template macht sie quasi gratis – Doku-Hinweis, kein Codefix | – |
| P-11 | ℹ️ INFO | Thread-Inventar: ~13–16 Basis-Threads (Vollausbau) + 1/Client, Conn-Stacks 256 KB VA | util.h:72-74, div. | RSS-Anteil gering (Stacks lazy-faulted); mbedTLS-Kontext ~30–40 KB je TLS-Conn ist der eigentliche Per-Client-Posten | – |
| P-12 | ℹ️ INFO | Binärgröße | build.sh:299-352 | `-Os` + gc-sections + strip + Feature-Gates: keine erkennbare Bloat-Quelle; tabellenloses CRC32 (srt.c:108-117) spart bewusst 1 KB Flash gegen Zyklen nur auf PSI-Pfaden | – |

---

### P-01 – Frame-Publish: zweite Vollkopie + Heap-Churn pro Frame

Der heißeste verbleibende Pfad. Pro encodiertem Frame passiert heute:

1. `video_thread` assembliert die IMP-Packs in den persistenten AU-Puffer (`memcpy`, hal_ingenic.c:1137) – **nötig** (Startcode-Fix, verstreute Packs).
2. `hub_publish` → `pkt_new` **mallockt und kopiert die komplette AU nochmal** (frame.c:7-17), plus `free` nach dem letzten `unref`.

Bei 25 fps × 2 Streams + 25 Audio-Frames/s sind das mit aktiven Clients ~75 malloc/free-Paare/s und bis ~1,5 MB/s zusätzliche memcpy-Bandbreite – auf einem 32-MB-SoC zusätzlich Dauer-Heap-Churn in Frame-Größenklasse (4 KB…400 KB), die klassische Fragmentierungsquelle für uClibc-Allocatoren.

**Vorschlag:** eine `hub_publish_take()`-Variante, die einen bereits refcounteten Puffer übernimmt: der Producer assembliert die AU direkt in ein `ms_pkt` (bzw. in einen aus einem kleinen Per-Source-Pool recycelten Puffer) statt in `au[]`. Achtung auf zwei bestehende Invarianten: (a) der 0-Subscriber-Skip (hub.c:247) darf nicht zum Per-Frame-malloc-bei-0-Subs werden – der Producer-Puffer muss wiederverwendbar bleiben, wenn niemand hört; (b) `vparam_update` liest die AU unter `s->lock` vor dem Push. Darum „mittel“ und nicht „klein“: API-Änderung an hub + 3 Producer-Pfaden (Video, JPEG, Audio). Der Gewinn ist der größte noch verfügbare Einzelposten.

### P-02 – Wakeup-Konsolidierung im Leerlauf

Die Stop-Responsiveness wird heute durchgehend über Slice-Sleeps erkauft: OSD 100 ms (10 Wakeups/s, **auch völlig idle**, nur um `osd_needed()` binnen 100 ms zu bemerken – das sind zusätzlich 4 Mutex-Paare pro Wake), Daynight 200-ms-Slices (~5/s bei 500-ms-Intervall), Recorder/Timelapse je 300 ms (~3,3/s auch bei disabled). Summe im Leerlauf ~25 Wakeups/s ≈ 50 Kontextwechsel/s auf dem Core, den sich timpsd mit ISP/Encoder-Firmware teilt. Kein gemessener Hotspot, aber unnötig: dieselbe Stop-Latenz liefert eine `pthread_cond_timedwait`-basierte Stop-Condvar (Muster existiert bereits in `fanqueue`/`events`) bei **einem** Wakeup pro echtem Intervall; der OSD-Idle-Fall kann den vorhandenen `hub_set_activity_cb` als Wecksignal nutzen statt zu pollen. Nutzen: Idle-Wakeups von ~25/s auf <5/s, spürbar eher im Power-/Scheduling-Verhalten als in %CPU.

### P-03/P-04 – Syscall-Trimmer im RTSP-Play-Loop

Ohne vDSO kostet jedes `ms_now_us()` einen echten Syscall. `stream_loop` ruft es pro Iteration mehrfach (Drop-Check, SR-Check, Aktivität) und pollt zusätzlich jede Iteration den Control-Socket nonblocking – bei laufendem Video ist das je Client und Frame ~1 EAGAIN-`recv` plus 3–5 `clock_gettime`. Ein einmal pro Iteration gecachtes `now` und ein zeitgegatetes Control-Poll (alle ~50 ms statt jede Iteration; TEARDOWN-Latenz bleibt <50 ms) sparen zusammen grob 100–150 Syscalls/s pro aktivem Client bei zwei Zeilen Änderung. Klein, aber praktisch gratis.

---

## Explizit geprüft, **kein** Handlungsbedarf

- **`snprintf` in Hot-Loops:** keine gefunden. Formatierung existiert nur in Request-/Setup-Pfaden, OSD-Expansion (1×/s, gecacht) und Logzeilen; RTP/TS/fMP4-Header werden binär über `wr_be*` gebaut.
- **`/control`-GET-JSON:** 18-KB-Heap-Puffer + einmaliger Build pro Request (httpd.c:1187-1204) – bei WebUI-Raten irrelevant; Dauer-Poll ist durch die SSE-Push-Architektur (events.c, condvar) ohnehin obsolet.
- **O(n²)-Muster:** keine. Alle Client-/Subscriber-Listen sind flache Arrays ≤16, Iterationen linear; Fanqueue-Drops amortisiert O(1).
- **Recorder-I/O:** gepuffertes `fwrite` + `REC_SYNC_US`-fflush + asynchrones `sync_file_range`, fsync nur am Segmentende – korrekt dimensioniert, kein Coalescing-Bedarf.
- **fMP4/HTTP-Streampfad:** ein `csend` pro fertigem Fragment (httpd.c:415), persistenter Scratch (fmp4.c:450-475) – bereits gebatcht.
- **Daynight:** Gain via IMP-API 2×/s (billig), /proc-Scrape auf 5 s gedrosselt (P2), Struct-Snapshot 284 B/Poll unter Lock (siehe P-09) – die Probe-Economy-Änderungen von v1.7.7 haben keine messbare Laufzeitkostenseite.
- **Stack/TLS-Budgets:** Audio-Worker ~12 KB `__thread` + 8 KB `acc[]` (dokumentiert, hal_ingenic.c:2377-2384), RTP-Paketpuffer ~1,5 KB/Frame auf Conn-Stacks (256 KB) – alles innerhalb der S3-Budgets; `rtsp.c` `hdr[3072]` bleibt der bekannte F-11-Nit.

## Empfohlene Reihenfolge

1. **P-08** – Buildflag für kleine SoCs setzen (1 Zeile in der Plattform-Buildconfig, sofortiger Worst-Case-RAM-Gewinn).
2. **P-03 + P-04** – Syscall-Trimmer (je wenige Zeilen, risikofrei, messbar per `strace -c`).
3. **P-01** – `hub_publish_take()`/Pufferpool, mit `make sim` + QA-Soak verifizieren (RSS + %CPU vorher/nachher).
4. **P-02** – Stop-Condvar-Konsolidierung, zusammen mit P-01 in einem Refactor-Fenster.
5. P-05/P-06 nach Kapazität.

Keine dieser Positionen ist dringend; der Daemon ist in seinem heutigen Zustand für alle Zielplattformen ressourcenseitig tragfähig.
