# Adversariales Review – P-01 (Frame-Pool / `hub_publish_take`) + P-02 (`ms_stopgate`)

**Datum:** 2026-08-07
**Prüfgegenstand:** uncommitteter Diff im Worktree `perf-p01-p02` (Branch `perf/p01-p02-frame-copy`, Basis = `main` @ `68d6ec6`, 13 Dateien, +540/−142), Umsetzung von P-01 und P-02 aus `PERFORMANCE_AUDIT_2026-08-07.md` durch Opus.
**Methode:** vollständige Diff-Lektüre; Refcount-Handnachverfolgung ALLER Producer-Call-Sites (0-Sub- und N-Sub-Fall, alle Fehlerpfade); Leftover-Greps (`hub_publish`, `usleep`, `g_stop`, rohe `free()` auf Paketen); eigener `make sim` (warnungsfrei) **und** eigener ASan+UBSan-Host-Build mit Mehrclient-Last (RTSP TCP+UDP ×3, fMP4, MJPEG, 60× Snapshot-Hammer, 10× Reconnect-Churn, Continuous- UND Motion-Recording mit 3 s Pre-Roll) plus drei gemessenen SIGTERM-Shutdowns. Behauptungen nicht geglaubt, sondern reproduziert.

---

## Gesamturteil

**Freigabe für den dedizierten cam-A-Soak (T31).** Kein blockierender Befund. Die Refcount-Buchhaltung ist an allen 10 Producer-Call-Sites korrekt, beide Audit-Invarianten (0-Subscriber-Skip, `vparam_update`-Ordnung) halten der Tiefenprüfung stand, die Stopgate-Konvertierung ist an allen vier Threads race-frei. Vier neue Befunde, alle NIEDRIG/INFO, keiner soak-blockierend — R-01 (Pool-Ceiling vs. reale IDR-Größen) gehört aber ausdrücklich auf die **Soak-Messliste**.

Eigene Reproduktion: `make sim` warnungsfrei; ASan+UBSan-Lauf unter voller Last **0 Leaks / 0 UAF / 0 UB**; Shutdown-Latenzen selbst gemessen: **idle 0,28 s, mid-stream 0,28 s, Motion-Modus unter Last 0,20 s** — deckungsgleich mit Opus' Größenordnung (0,14–0,34 s), weit unter dem 3-s-Watchdog.

**Nicht durch den cam-A-Soak validierbar** (T23-Abschnitt unten): der `ROT_HAS_SW_90`-Software-Rotate-Pfad. Er ist im Diff **byte-identisch unangetastet** (kein Hunk zwischen hal_ingenic.c:1185–1744) und bleibt auf der unveränderten Copy-API — das Restrisiko ist ein reines Compile-Risiko, kein Logik-Risiko. Opus' behaupteter T23-`-fsyntax-only`-Lauf konnte hier mangels Toolchain **nicht** reproduziert werden → offener Punkt vor jedem T23-Rollout.

---

## P-01 – Verdikt je Behauptung

| Behauptung | Verdikt | Nachweis |
| --- | --- | --- |
| `pkt_new()` unverändert, alle Bestands-Sinks unberührt | ✅ **BESTÄTIGT** | frame.c: identisches malloc+memcpy, nur `cap=len?len:1`, `pool=NULL`, `pnext=NULL` ergänzt. `pool==NULL` ⇒ `pkt_unref()` free()t exakt wie vorher. Grep über ganz `src/`: **kein** roher `free()` auf einem `ms_pkt` außerhalb frame.c; fanqueue (Push-auf-closed, Drop-oldest, GOP-Forward-Drop), record-Ring, rtsp/srt/httpd/timelapse nutzen ausnahmslos `pkt_unref` — pool-agnostisch korrekt. |
| Gemeinsamer Helper, kein Verhaltens-Drift | ✅ **BESTÄTIGT — echt geteilt** | `hub_publish()` UND `hub_publish_take()` rufen beide dieselbe `hub_prepare_locked()` (vparam/fps/kbps-Update + Subscriber-Snapshot + `g_pushing`-Raise, alles unter `s->lock`) und dieselbe `hub_finish_push()`. Kein duplizierter Code, der auseinanderlaufen könnte. |
| `vparam_update`-Ordnung erhalten | ✅ **BESTÄTIGT** | Take-Pfad übergibt `p->data/p->len` an `hub_prepare_locked` — zu diesem Zeitpunkt hält der Producer die einzige Referenz (noch kein Push erfolgt), also exakt dieselben Bytes am selben Punkt der Sequenz wie im Copy-Pfad. Video-only-Gate unverändert. |
| 0-Subscriber-Invariante (Audit-Invariante a) | ✅ **BESTÄTIGT** | `hub_publish_take` bei `nsub_snap==0`: `pkt_unref(p)` ⇒ letzter Ref ⇒ Pool-Rückgabe (kein free, kein malloc). Idle-Stop-Debounce-Fenster = Borrow+Return **desselben** Puffers. Einschränkung für >96-KB-Puffer → R-01. |
| Call-Site-Zählung „10 statt 11" | ✅ **BESTÄTIGT — 10 ist korrekt** | Eigener Grep auf `main`: exakt **10** Producer-Sites (hal_ingenic 7: Video 1159, sw-rot-Video 1488, sw-rot-JPEG 1519, JPEG 1874, Audio 2394/2417/2425; hal_sim 3: 107/142/160). Die „11" im früheren Opus-Report war eine Fehlschätzung. Disposition im Branch: **4 konvertiert** (Video+JPEG hardware, Video+JPEG sim), **6 bewusst auf Copy-Pfad** (2× T23 sw-rot, 4× Audio). Keine Site behandelt die jeweils andere API-Semantik falsch. |
| Pool-Sizing 4 × 96 KB, Ratchet-Frei­gabe | ✅ Werte bestätigt (`HUB_POOL_MAX_FREE=4`, `HUB_POOL_KEEP_CAP=96*1024`, beide `#ifndef`-überschreibbar), Logik korrekt (`nfree<max_free && cap<=keep_cap` unter `pool->lock`). **Aber → R-01.** |
| ASan-Lauf ohne Befund | ✅ **selbst reproduziert** (s. Gesamturteil), inkl. Motion-Pre-Roll-Ring (75 gepinnte Pakete ⇒ Pool-Overflow-zu-free-Zweig nachweislich exerziert). |

### Refcount-Handprüfung (der Kern dieses Reviews)

Alle Pfade einzeln nachgerechnet:

- **hal_ingenic video_thread (take):** `hub_pkt_get` ⇒ ref=1. (a) `need>MS_AU_BUF_MAX` ⇒ Drop VOR dem Get, kein Paket im Spiel. (b) Get-OOM ⇒ Drop+ReleaseStream, nichts geliehen. (c) Defensiv-Overflow ⇒ `pkt_unref(pk)` ✅. (d) Normal ⇒ `au_is_key` liest VOR der Übergabe; nach `hub_publish_take` fasst der Thread `pk`/`au` nachweislich nicht mehr an (T31-Bitrate-Block liest nur `st`). Take konsumiert: 0-Sub ⇒ unref/Pool; N-Sub ⇒ N×`pkt_ref` beim Push + 1× Producer-unref ⇒ Bilanz exakt N, jeder Sink unref't einmal. ✅
- **hal_ingenic jpeg_thread (take):** identische Struktur; Snapshot-`fwrite` liest den Puffer **vor** der Übergabe (Kommentar + Code stimmen überein). Overflow-Pfad unref't. ✅
- **hal_sim vid/jpg (take):** Get-NULL ⇒ Frame übersprungen, Zustandsfortschritt (`next+=step`, `aulen=0`) läuft trotzdem — kein Hänger, kein Leak. ✅
- **6 Copy-Sites:** unveränderte Borrowed-Buffer-Semantik, Caller behalten Ownership. ✅
- **`hub_publish_take(src, NULL, …)`** und ungültiges `src`: no-op bzw. unref — safe. ✅
- **Ein-Producer-pro-Source-Invariante** (Voraussetzung des `g_pushing`-Flags) bleibt gewahrt: `video_thread` XOR `sw_rot_thread` pro vchan (hal_ingenic 1736/2791), jpeg/audio je ein Thread.

### Neue Befunde P-01

| # | Schwere | Befund |
| --- | --- | --- |
| R-01 | 🟡 NIEDRIG (Perf, nicht Korrektheit) | **`HUB_POOL_KEEP_CAP=96 KB` liegt UNTER der realen Mainstream-IDR-Größe.** hal_ingenic.c dokumentiert selbst „observed IDR peak 256–400 KB" (1080p @ 3000 kbps, dem Default aus `timps.conf.example`); 1080p-JPEGs überschreiten 96 KB ebenfalls regelmäßig. Folge: der für den IDR geratchte Puffer wird bei der Rückgabe IMMER ge-free()t ⇒ pro GOP (~1×/s) ein realloc-Grow + free, bei JPEG-Quellen ggf. pro Frame. **Wichtig:** Die eigentliche P-01-Ausbeute — Wegfall der zweiten Vollkopie — bleibt für IDRs vollständig erhalten (Assembly erfolgt direkt in den Pool-Puffer, Übergabe zero-copy); nur das *Recycling* ist für die Großframes ausgehebelt. Churn sinkt trotzdem von ~25 Paaren/s auf ~1/GOP. Empfehlung: auf dem cam-A-Soak RSS + Fragmentierung messen und ggf. `keep_cap` für Video-Quellen anheben (per `-D` bereits möglich); der 384-KB-Idle-Deckel war ein bewusster 32-MB-SoC-Trade-off und ist als Default vertretbar. |
| R-02 | 🟡 NIEDRIG (Perf) | `pkt_pool_get` wächst per `realloc()` — kopiert bis zu 96 KB **stale Payload**, obwohl `len` beim Get immer 0 ist und der Inhalt komplett überschrieben wird. `free`+`malloc` wäre strikt billiger. Kosmetisch, ~1×/GOP relevant (verstärkt R-01 minimal). |
| R-03 | ⚪ INFO | jpeg_thread publiziert jetzt **immer** (altes `jc->active \|\| hub_active`-Gate entfernt). Zustell-Semantik nachweislich äquivalent (das Gate übersprang nur den jetzt eliminierten malloc+copy; 0-Sub-Take ist Borrow+Return), Kosten pro Idle-Frame: 1× `s->lock` + Pool-Roundtrip — vernachlässigbar. Zweitens: Snapshot-Schreiben (blockierend, SD) rückt VOR den Publish — der MJPEG-Frame des Snapshot-Ticks kommt nun ~SD-Latenz später statt der Folgeframe; neutral, gleicher Thread. |

---

## P-02 – Verdikt je Behauptung

| Behauptung | Verdikt | Nachweis |
| --- | --- | --- |
| Folgt dem fanqueue/events-Muster | ✅ **BESTÄTIGT** | `ms_stopgate` nutzt exakt die `condattr`+`CLOCK_MONOTONIC`-Konstruktion aus events.c:22-26/fanqueue.c:27-29 (NTP-Step-fest). |
| Stop-Race-Freiheit | ✅ **BESTÄTIGT** | `ms_stopgate_stop()`: `lock → stop=1 → broadcast → unlock` — **dasselbe** Mutex wie der Prädikat-Check des Waiters. `ms_stopgate_wait()`: Prädikat vor dem ersten Wait (already-stopped-Fall) UND in der Schleife (Spurious-Wake/Timeout). Fensterfreie Konstruktion; für alle 4 Threads gilt zusätzlich `!ms_stopgate_stopped()` am Schleifenkopf. Kein Shutdown-Hang-Fenster gefunden. |
| Shutdown-Latenzen | ✅ **selbst gemessen:** 0,28 s idle / 0,28 s mid-stream / 0,20 s Motion-Last (ASan-Build, also konservativ). Plausibilisiert Opus' 0,14–0,34 s vollständig. |
| record/timelapse: 1-s-Poll betrifft NUR Idle-Enable | ✅ **BESTÄTIGT** | record.c: der 1-s-Wait liegt ausschließlich im `!want_run()`-Zweig (Recorder disabled, kein Abo, kein Pre-Roll zu verlieren); laufende Aufnahme taktet über `fanqueue_pop(&q,200)` mit `want_write()` **pro Paket** — Schreib-/Motion-Kadenz unverändert. timelapse.c: Capture-Zeitpunkt hängt an `next_us`, der Wait ist `min(left, 1 s)` — Intervalltreue unverändert, nur Disabled-Poll von 300 ms auf 1 s. |
| imp_osd: nur Idle-Wakeups geändert | ✅ **BESTÄTIGT** | Alte Schleife: Render + 10×100 ms ⇒ 1-s-Kadenz aktiv. Neu: Render + 1×1000 ms ⇒ identische 1-s-Kadenz aktiv, 1/10 der Wakeups. Einziger Verlust: der Sub-Sekunden-Refresh beim Client-Connect aus Idle (Timestamp bis ~1 s stale, kosmetisch) — im Code sauber als Trade-off dokumentiert, `hub_set_activity_cb`-Kopplung bewusst und nachvollziehbar verworfen. |
| main.c korrekt unangetastet | ✅ **BESTÄTIGT** | `while (g_run) sleep(1)` + Signal-Handler: `pthread_cond_broadcast` ist nicht async-signal-safe (POSIX), ein Handler DARF die Gate nicht wecken; `sleep()` kehrt bei Signal mit EINTR sofort zurück — der bestehende Loop ist bereits die korrekte Konstruktion. Opus' Begründung hält. |

### Randfälle geprüft

- **`record_clip` (Control-Thread) liest `g_gate`:** (1) Recorder nie gestartet ⇒ `!g_rc`-Guard returnt vor der Schleife. (2) Thread-Create-Fehler ⇒ `record_start` ruft explizit `ms_stopgate_stop` — die Gate ist gestoppt, `record_clip` dreht keine Endlosschleife (Äquivalent zum alten `g_run=0`). Timelapse-Fehlerpfad analog. Sauber mitgedacht. ✅
- **`dn_sleep` in inneren Zweigen** (daynight 754/790/935/1226): kehrt nach Stop sofort zurück, jeder Zweig endet in `continue` zum Schleifenkopf-Check — kein Spin, exakt die alte Slice-Semantik. ✅
- **Leftover-Grep:** kein `usleep` mehr in den 4 konvertierten Dateien, kein `g_stop`/altes `g_run`-Flag-Relikt. ✅

### Neue Befunde P-02

| # | Schwere | Befund |
| --- | --- | --- |
| R-04 | 🟡 NIEDRIG (Portabilität) | `ms_stopgate_init()` wird bei Restart (`record_start` nach `record_stop`, dito daynight/imp_osd/timelapse) auf ein bereits initialisiertes Mutex/Cond erneut aufgerufen — nach POSIX-Buchstabe undefiniert (Re-Init ohne `destroy`). Praktisch harmlos auf NPTL/uClibc-ng (statische Objekte, zum Init-Zeitpunkt nachweislich keine Waiter — der Join ist abgeschlossen — und kein Ressourcen-Leak), und der Header dokumentiert „Safe to call again on restart" als bewusste Entscheidung. Sauberer wäre ein `ms_stopgate_rearm()`, das nur `stop=0` unter dem Lock setzt. Kein Soak-Blocker. |

---

## Was der cam-A-Soak NICHT abdeckt (vor T23-Rollout offen)

1. **T23 `ROT_HAS_SW_90`-Pfad:** im Diff unangetastet (0 Hunks in 1185–1744, beide Sites 1488/1519 weiter auf der unveränderten Copy-API) — Logik-Risiko ≈ 0. ABER: hal_ingenic.c wurde substanziell editiert; Opus' behaupteter `-fsyntax-only`-Lauf gegen T23-Header wurde hier **nicht reproduziert** (Toolchain nicht im Worktree). ⇒ Vor jedem T23-Build: `./build.sh` bzw. `-fsyntax-only` gegen T23-Header einmal selbst laufen lassen.
2. **R-01-Messung:** reale IDR-Größen vs. 96-KB-Ceiling auf Hardware-Bitraten — Soak-Checkliste: RSS-Verlauf, `logread` auf die `dropping frame`/OOM-Warnungen (dürfen nie feuern), ideal einmal Heap-Churn vorher/nachher.
3. **uClibc-Allocator:** der ASan-Lauf ist glibc/x86 — Fragmentierungsverhalten des uClibc-Allocators unter dem neuen 1/GOP-Muster zeigt nur der Soak.
4. **Audio-Pfade** sind unkonvertiert (bewusst) — keine Soak-Anforderung über den Normalbetrieb hinaus.

## Soak-Empfehlung

cam-A (T31), Standard-QA (`scripts/timps-qa.sh --profile soak`) plus: ≥48 h mit aktivem Recording (Motion + Pre-Roll, pinnt Pool-Pakete), periodisch RSS + `dropping frame`-Grep. Abbruchkriterium: jede `AU exceeds max buffer`/`no memory for AU packet`-Meldung oder RSS-Drift >10 %.
