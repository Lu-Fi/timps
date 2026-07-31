# timps — Optimierungs-Review: Speicher / Performance / Binärgröße (Fable 5)

_Stand: 2026-07-31 · Quelle: `src/` @ HEAD `d4ed99f` + Referenz-Build aus
`thingino-firmware-LuFi` (galayou_y4_t23n, uclibc, Build vom 31.07. 20:03).
Kein Bug-Hunt (dafür: `timps-review-fable5.md`, `timps-review-opus48-2026-07-31.md`) —
reines Optimierungs-Review entlang der drei Linsen Speicher, Hot-Path-Performance,
Binärgröße. Nichts wurde geändert._

## Referenz-Build (Messbasis, nicht geschätzt)

Aus `output/timps-package/galayou_y4_t23n_sc2336_atbm6062-3.10.14-uclibc-192.168.15.129/`:

```
build/timps-custom/timpsd   254 828 B  (with debug_info, not stripped)
target/usr/bin/timpsd       216 308 B  (stripped  <- DAS ist, was geflasht wird)

mipsel-linux-size:  text 208 277   data 2 560   bss 43 040
gzip -9 timpsd (stripped):  99 661 B   (~ Beitrag zur squashfs, rootfs gesamt 4,88 MB)
Build-Flags (aus build log): -Os -g0 -ffunction-sections -fdata-sections,
                             LDFLAGS mit -Wl,--gc-sections; USE_FAAC=1 USE_CONTROL=1
                             USE_DAYNIGHT=1 USE_TLS=1, Rest aus.
```

**Das "not stripped" von heute Nacht betrifft nur die Build-Verzeichnis-Kopie.**
Buildroot strippt beim Target-Install (216 308 B im Image, `file` sagt "stripped").
Die Differenz (38,5 KB = ~10 KB DWARF + ~23 KB symtab/strtab) kostet im Image nichts.
Kein Handlungsbedarf, nur Dokumentation.

Modul-Text-Größen (Objekte mit identischen Flags nachkompiliert, `size *.o`, vor gc-sections):

| Modul | .text | Modul | .text |
|---|---|---|---|
| config.o | **33 976** | daynight.o | 8 736 |
| hal_ingenic.o | 21 804 | fmp4.o | 8 328 |
| mp4/httpd.o | 16 116 | timelapse.o | 4 440 |
| control.o | 15 372 | hub.o | 4 348 |
| record.o | 10 644 | rtp.o | 4 220 |
| rtsp.o | 10 536 | vparam.o | 3 360 |
| msttf.o | 9 076 | osd_vars.o | 4 216 |
| imp_osd.o | 9 044 | Rest | < 3 000 je |

Größte Einzelfunktionen im gelinkten Binary (`nm --size-sort`): `set_kv` 10 864 B,
`config_get_kv` 9 648 B, `conn_thread` 8 756 B, `client_thread` 5 800 B,
`dn_thread` 4 548 B, `control_get_json` 4 172 B. Größte bss-Objekte: `g_cfg` 10 872 B,
`g_cfgtab` 4 896 B, `g_os` 4 680 B, `g_src` 3 552 B, `g_font_cache` 2 944 B.

---

## Vorweg: was bereits gut optimiert ist (geprüft, kein Handlungsbedarf)

Der Ist-Zustand ist für diese Hardwareklasse ungewöhnlich sauber; die großen Punkte
der Juli-Reviews sind gelandet und verifiziert:

- **Publish-Pfad kopie- und lock-minimal** (`hub.c:203-260`): unter dem Lock nur
  subs[]-Snapshot + vparam/fps; `pkt_new` (der einzige Full-Frame-memcpy) und alle
  Queue-Pushes danach; bei 0 Subscribern wird malloc+copy komplett übersprungen —
  der Producer läuft das Idle-Debounce-Fenster heap-still durch.
- **Kein Per-Frame-Heap-Churn mehr in fMP4/Record** (alt M1): persistente `ms_buf frag`
  je Verbindung bzw. je Recorder (`mp4/httpd.c:248`, `record.c:286`), mit
  `ms_buf_reset(&frag, 256*1024)` als Outlier-Shrink. Genau richtig für den
  24/7-musl/uclibc-Heap.
- **On-Demand-Pipeline konsequent**: Producer blocken idle auf Condvar (`act_wait`),
  Encoder/Framesource via `fs_use`/`fs_unuse` + 2-s-Debounce; OSD-Updater rendert
  nur bei aktiven Konsumenten (`osd_needed()`), `osd_vars.c` cached alle
  /proc-/sys-Reads mit ~1-s-TTL; Retired-OSD-Puffer werden nach ~2 s Idle freigegeben.
- **AU/JPEG-Puffer wachsen bedarfsgetrieben** mit Pre-Size aus den Pack-Längen vor
  dem Kopieren (kein Frame-Verlust beim Wachsen), Kappen `MS_AU_BUF_MAX` =
  `MS_JPEG_BUF_MAX` = 1 MB nach dem 820-KB-JPEG-Befund von heute Nacht.
- **Keine Rekursion in Hot-Paths**, keine >8-KB-Stack-Arrays in Streaming-Threads
  (größte: `acc[4096]` int16 = 8 KB im Audio-Thread, `srt.c` `adts[8192]` nur bei
  USE_SRT; `aac[8192]` ist `static __thread`, nicht Stack).
- **TCP-interleaved RTP**: Header+Payload in einem `send` (halbierte Syscalls,
  Kommentar `rtsp.c:93-96`); Sende-Fehler brechen die AU sofort ab (L3).
- **Fanqueue-Overflow** droppt kopflose GOPs vorwärts statt sie einzeln
  auszutröpfeln; `record.c` hat bereits ein Byte-Budget fürs Pre-Roll-Ring
  (`RING_MAX_BYTES` 4 MB).
- **gc-sections wirkt**: `srt.o` kompiliert bei USE_SRT=0 zu 64 B; die USE_*-Gates
  (TLS/SRT/BACKCHANNEL/PLAY/ROTATE) produzieren nachweislich byte-identische bzw.
  minimale Builds.

Die folgenden Befunde sind das, was danach noch übrig ist — sortiert nach
realem, bezifferbarem Nutzen.

---

## 1. Speicher-Footprint

### S1 — AU-Puffer des 1080p-Video-Threads startet mit 1 MB, obwohl der Grow-Pfad existiert
`hal/hal_ingenic.c:877-881`

`au_cap = w*h/2` ⇒ bei 1920×1080 = 1 036 800 B, geklemmt auf `MS_AU_BUF_MAX` =
**1 MB upfront-malloc** pro Hauptstream-Thread — dauerhaft, denn der Puffer wächst nur.
Typische H.264/H.265-AUs bei 2-6 Mbit/s: P-Frames 5-40 KB, IDRs 60-400 KB. Der
~0,5-Byte/Pixel-Startwert stammt aus der Zeit, als Überlauf = Frame-Drop war; seit dem
Grow-Fix wird **vor** dem Kopieren aus `st.pack[i].length` präzise nachalloziert
(`:973-985`) — ein kleiner Startwert verliert also keine Frames mehr, er kostet nur
in den ersten Sekunden ein paar reallocs.

- **Kosten heute:** ~1 MB RSS für den Mainstream (Substream sitzt eh am 128-KB-Floor);
  auf einem 64-MB-T10/T21 sind das ~1,5 % des Gesamt-RAM für Luft, die nie gebraucht wird.
- **Empfehlung:** Startgröße = `MS_AU_BUF_MIN` (128 KB) statt `w*h/2`; das
  resolutionsbasierte Sizing nur noch als MAX-Kappe behalten. Für den JPEG-Pfad
  (`jpeg_thread`, `:1536-1539`) **nicht** übernehmen — dort sind 800-820 KB reale
  Frames belegt (Galayou Y4, q75 Tageslicht), das W×H/2-Sizing ist dort gerechtfertigt;
  höchstens auch dort lazy starten und auf den beobachteten Peak wachsen lassen.
- **Nutzen:** ~0,6-0,9 MB RSS je 1080p-Stream steady-state (Puffer endet beim
  beobachteten IDR-Peak, typ. 256-400 KB, statt 1 MB). Eine Zeile, kein Risiko.

### S2 — Fanqueues deckeln Pakete, nicht Bytes (Rest von alt-M10)
`rtsp/rtsp.c:35` (QCAP 64) · `mp4/httpd.c:36` (64) · `record.c:45` (128) · `srt.c:42` (128)

Die Payloads sind refcounted, nicht kopiert — aber ein stehender/gedrosselter Consumer
pinnt bis QCAP volle AUs. Bei 4-6 Mbit/s ≈ 20-30 KB/AU-Schnitt (IDRs deutlich größer):
64 Slots ≈ **1,5-2,5 MB je gestalltem Client**, 128 Slots (Record bei SD-Stall,
SRT) das Doppelte. Worst case 8 RTSP + 8 HTTP Clients gleichzeitig gestallt:
theoretisch > 20 MB gepinnt. `record.c` zeigt mit `RING_MAX_BYTES` bereits das Muster.

- **Empfehlung:** `fanqueue` um ein optionales Byte-Budget erweitern (Zähler in
  push/pop, drop-oldest bis unter Budget — exakt die `ring_push`-Logik aus
  `record.c:96-101`). Sinnvolle Defaults: ~2 MB je Video-Queue, per `-D` überschreibbar
  wie die QCAPs.
- **Nutzen:** Worst-Case-Speicher pro langsamem Client hart gedeckelt statt
  bitraten-abhängig; auf 64-MB-Targets der Unterschied zwischen "OOM-Killer" und
  "ein Client ruckelt".

### S3 — Keine expliziten Thread-Stackgrößen (~15-20 Threads)
kein `pthread_attr_setstacksize` im Baum (geprüft); Threads: je Video/JPEG/Audio,
OSD, Motion, Daynight, Record, Timelapse, 2× Accept, je Client RTSP/HTTP/SSE …

uClibc-ng-NPTL nimmt ohne Attribut `RLIMIT_STACK` (üblich 8 MB) als Stackgröße —
pro Thread 8 MB **VA** (RSS nur berührte Seiten, real also wenige KB/Thread;
das ist kein akuter RAM-Fresser). Trotzdem: gemessene Stack-Spitzen liegen bei
~16 KB (`client_thread`: buf 4 K + req 4 K + sdp 2,6 K + hdr 3 K).

- **Empfehlung:** ein zentrales `ms_thread_create()` mit 64-128 KB Stack (Streaming-
  Threads) bzw. 256 KB (conn_threads mit TLS-Handshake). 
- **Nutzen:** ~120 MB weniger VA (fork in `dn_switch` kopiert weniger Page-Tables),
  deterministisches Verhalten auf Small-RAM-Targets, Stack-Overflows werden per
  Guard-Page sichtbar statt zufällig. Klein, aber billig und standard für embedded.

### S4 — TLS: 16+16 KB I/O-Puffer je Verbindung (mbedTLS-Default, USE_TLS=1 ist Default)
Staging-`mbedtls/ssl.h`: `MBEDTLS_SSL_IN/OUT_CONTENT_LEN` = 16384

Jede HTTPS/RTSPS-Verbindung kostet ~34 KB Heap nur für die SSL-Puffer; 16 gleichzeitige
TLS-Clients ≈ 0,5 MB. timps sendet selbst nie Records > 4 KB am Stück zwingend
(Fragmente lassen sich chunken), und die Clients senden nur kleine Requests.

- **Empfehlung (buildroot-Ebene, nicht timps):** `MBEDTLS_SSL_OUT_CONTENT_LEN=4096`
  ist gefahrlos (Sender bestimmt seine Record-Größe; `ms_tls_write` müsste >4-K-Writes
  chunken). `IN_CONTENT_LEN` reduzieren nur mit Max-Fragment-Length-Extension —
  sonst Inkompatibilität mit Clients, die volle 16-K-Records schicken. Vorher prüfen,
  wer im Image mbedtls noch nutzt.
- **Nutzen:** ~12 KB je TLS-Verbindung (OUT allein), ~24+ KB mit MFL.

### S5 — Positivbefund Per-Subscriber-Overhead: bereits schlank
RTSP-`session` ≈ 1,2 KB + Fanqueue-Slots (64×4 B) + 2 KB `ctl`-Puffer im Stack;
`hub_source.subs[16]` = 64 B. Bei HUB_MAX_SUBS=16 gibt es hier nichts Nennenswertes
zu holen — der reale Per-Client-Speicher ist die Queue-Payload (S2), nicht der State.

---

## 2. Performance (Hot Paths)

### P1 — Timelapse hält die JPEG-Pipeline 24/7 am Laufen (alt-H5: **unverändert offen**)
`timelapse.c:181-232`

Der einzige große offene Punkt aus dem Juli-Review. `tl_thread` subscribed einmal und
bleibt subscribed, solange `timelapse.enabled=1` ⇒ `hub_active(src)` dauerhaft wahr ⇒
`jpeg_thread` hält Framesource + HW-JPEG-Encoder rund um die Uhr aktiv
(`hal_ingenic.c:1561`), beim Piggyback-Source inklusive der **Video**-Framesource des
Parent-Streams — um bei `interval_s` von Minuten/Stunden genau ein Frame pro Intervall
zu behalten. Das ist exakt die Dauerlast (ISP-→DDR-Bandbreite, Encoder, 5 fps
JPEG-Encodes), die das On-Demand-Design sonst überall vermeidet, und auf Kameras ohne
weitere 24/7-Consumer der größte einzelne CPU/Strom-Posten.

- **Kosten:** Duty-Cycle 100 % statt < 5 %: pro Intervall werden `interval_s × fps`
  JPEGs encodet und alle bis auf eines weggeworfen (bei 15-min-Intervall, 5 fps:
  4 500 Encodes pro verwendetem Frame).
- **Empfehlung:** Just-in-time subscriben wie `snapshot_jpg()` (`mp4/httpd.c:338-383`):
  zum Shot-Zeitpunkt subscriben, 1 Frame poppen (bounded wait, beim Piggyback der
  gleiche Parent-Video-Wake-Trick), unsubscriben; die 2-s-Idle-Debounce fängt
  Intervalle < ~5 s ab, sodass kurze Intervalle nicht in Start/Stop-Churn laufen.
- **Nutzen:** Encoder/Framesource/ISP-Last zwischen Shots = 0; auf Single-Core-SoCs
  direkt messbar in Load und Wärme.

### P2 — Daynight: /proc-ISP-Scrape 2×/s, obwohl das Ergebnis meist verworfen wird
`daynight.c:396-400`

`dn_thread` ruft **jeden** Tick (Default `interval_ms=500`) `dn_brightness()` auf:
fopen + zeilenweises fgets mit je ~7 `strstr` + sscanf über den ISP-Proc-Dump — und
überschreibt das Gain-Ergebnis direkt danach mit `hal_isp_total_gain()` (IMP-API),
wenn die verfügbar ist (Normalfall auf T23/T31). Der Brightness-Wert aus dem Scrape
speist dann nur noch die Statusanzeige (`/control`, `/events`), nicht die Entscheidung;
in den TIME/SUN-Modi wird er für die Entscheidung nie gebraucht.

- **Kosten:** 2 fopen/parse pro Sekunde, 24/7, inkl. kernelseitiger ISP-Registerlesen
  für den Proc-Dump — klein (<1 % CPU), aber konstant und komplett vermeidbar.
- **Empfehlung:** Scrape nur, wenn (a) die Gain-API fehlschlägt (Fallback-Pfad) oder
  (b) als Status-Refresh gedrosselt auf z. B. alle 5-10 s. Entscheidungspfad bleibt
  bei der 500-ms-API-Abfrage.
- **Nutzen:** ~95 % der Proc-Parsing-Arbeit des Daemons im Leerlauf entfällt.

### P3 — UDP-RTP: ein `sendto` pro Paket; `sendmmsg` würde die Syscalls je AU bündeln
`rtsp/rtsp.c:117` (sink_send, UDP-Zweig) · `rtp.c` Fragmentierschleifen

Bei 4 Mbit/s sind das ~360 sendto/s pro UDP-Client (ein 200-KB-IDR allein = ~143
Pakete in Folge). Auf Kernel 3.10/MIPS ohne vDSO kostet jeder Syscall spürbar
(~2-5 µs) ⇒ grob 0,1-0,3 % CPU je Client — plus je ein `write` pro Paket beim
TCP-Zweig (dort immerhin schon Header+Payload gebündelt).

- **Empfehlung (niedrige Priorität):** die Fragmentierschleifen auf ein kleines
  `mmsghdr`-Array (16-32 Pakete, ~46 KB Stack- oder per-Track-Puffer) umstellen und
  je AU mit 1-2 `sendmmsg` rausschreiben; TCP analog die $-Frames einer AU in einem
  ~32-KB-Sammelpuffer koaleszieren.
- **Nutzen:** 10-30× weniger Syscalls im Videopfad; lohnt erst bei mehreren
  gleichzeitigen Clients oder hohen Bitraten — vorher messen.

### P4 — Positivbefunde (geprüft, nichts zu tun)
- Kein snprintf/sscanf/JSON im Per-Frame-Pfad; `/control`-JSON (18-K-Heap-Puffer)
  nur per Request, SSE-Stats condvar-getaktet (`events_wait`), Keepalive 15 s.
- Per Frame: ~3 Mutex-Ops (publish + push je Sub + pop) plus 2-4 `clock_gettime`
  (kein vDSO auf 3.10-MIPS, aber bei ≤100 Aufrufen/s über alle Quellen irrelevant).
- `osd_thread` idle: 10 Wakeups/s für den `osd_needed()`-Check — vernachlässigbar,
  Event-Steuerung über den Activity-Callback wäre Kosmetik.
- Blocking-I/O sauber eingezäunt: SD-Writes mit `sync_file_range` statt fsync im
  Record-Thread (M-3-Fix aktiv), Header/Body-Reads mit Poll-Deadlines, Sende-Timeouts
  gegen tote Clients.
- `msttf`-Rasterizer: Span-Fill rechnet Pixelgrenzen jetzt integer (`pxa/pxb`,
  `msttf.c:483-485`, alt-H4 adressiert), Rendering nur bei Textänderung und nur bei
  aktiven Konsumenten; `supersample=2` Default ist der richtige Kompromiss.

---

## 3. Binärgröße

Kontext: 216 KB stripped, ~100 KB komprimiert in der squashfs — timps ist bereits
klein; hier geht es um die letzten sinnvollen Prozente, nicht um Not.

### B1 — Target-Binary ist gestrippt (Nachtbefund entkräftet)
Siehe Messbasis oben: `target/usr/bin/timpsd` = stripped, 216 308 B. Die
"not stripped"-Kopie lebt nur unter `build/timps-custom/`. Kein Flash-Verlust.

### B2 — config.c ist mit Abstand das größte Modul: Tabelle statt strcmp-Kaskaden
`config.c` (33 976 B .text; `set_kv` 10 864 B + `config_get_kv` 9 648 B gelinkt)

`set_kv`/`config_get_kv`/`config_defaults` sind drei parallel gepflegte
if/else-strcmp-Ketten über ~200 Keys — zusammen ~21 KB Code (10 % des Binaries),
und jede neue Option wächst an drei Stellen. Eine deskriptorbasierte Tabelle
(`{key, offset_of, type, min, max, default}`) ersetzt alle drei Funktionen durch
~2 KB generischen Code + ~3-4 KB rodata-Tabelle und macht Setter/Getter/Defaults
per Konstruktion konsistent (nimmt nebenbei der alt-M3-Klasse "Reader ohne Lock"
eine zentrale Stelle zum Absichern).

- **Nutzen:** geschätzt **12-15 KB .text** (~6-8 KB komprimiert im Image); größter
  Einzelhebel im Binary. Aufwand: mittel (mechanisch, aber breit; gut testbar via
  `config_get_kv`-Roundtrip über alle Keys).

### B3 — Record + Timelapse sind nicht abschaltbar (~15 KB für SD-lose Kameras)
`record.o` 10 644 B + `timelapse.o` 4 440 B, immer gelinkt

Beide werden unconditional aus `main.c`/`control.c` referenziert — gc-sections kann
sie nicht entfernen. Viele Deployments (reine RTSP-Feeds ohne SD-Slot) brauchen
beides nie.

- **Empfehlung:** `USE_RECORD ?= 1` / `USE_TIMELAPSE ?= 1` nach dem exakten Muster
  von USE_SRT (Stubs für die control/status-Hooks), Buildroot-Knöpfe in `Config.in`
  analog `BR2_PACKAGE_TIMPS_CONTROL` ("Disable to save ~15KB").
- **Nutzen:** ~15 KB .text (~7 KB Flash) für die Minimal-Konfiguration; auf einem
  5-MB-T20-Rootfs (siehe Config.in-Preset-Kommentar) zählt das.

### B4 — Kleinvieh (nur der Vollständigkeit halber)
- `font8x16` (1 520 B rodata) + `osd_text.c` (2 204 B): Bitmap-Fallback bleibt auch
  dann im Binary, wenn das Image immer einen TTF mitliefert (thingino-fonts ist
  per Config.in `select`ed). Ein `USE_BITMAP_FONT`-Gate spart ~3,7 KB — nur sinnvoll,
  wenn B3 ohnehin angefasst wird.
- `control.o` 15,4 KB: bereits per `USE_CONTROL` abschaltbar und in Config.in mit
  "~15KB" dokumentiert — Angabe stimmt mit der Messung überein.
- Keine großen eingebetteten Datenblobs gefunden: .rodata gesamt nur 24 KB (SDP/
  HTML-Player-Strings, Caps-Listen); nichts, was Auslagern lohnt.
- Bibliotheken: IMP-Libs shared (Buildroot-Override korrekt), mbedTLS shared und
  default-on — der Flash-Preis dafür liegt außerhalb von timpsd und ist eine
  Image-Entscheidung, kein timps-Problem.

---

## Priorisierte Kurzliste

| # | Befund | Aufwand | Nutzen |
|---|---|---|---|
| 1 | P1 Timelapse-JIT-Subscribe (alt-H5, offen) | klein | Encoder/ISP-Dauerlast → ~0 zwischen Shots |
| 2 | S1 AU-Puffer lazy statt 1 MB upfront | trivial | ~0,6-0,9 MB RSS je 1080p-Stream |
| 3 | S2 Fanqueue-Byte-Budget | klein | Worst-Case-Pinning gedeckelt (MB statt zig MB) |
| 4 | B2 config.c tabellengetrieben | mittel | ~12-15 KB .text, 3 Pflegestellen → 1 |
| 5 | B3 USE_RECORD/USE_TIMELAPSE | klein | ~15 KB .text optional |
| 6 | P2 Daynight-Scrape drosseln | trivial | konstante 2-Hz-Parse-Last weg |
| 7 | S3 Thread-Stacks explizit | klein | VA/Robustheit, kleiner RSS-Effekt |
| 8 | S4 mbedTLS OUT-Puffer 4 K | klein (buildroot) | ~12 KB je TLS-Conn |
| 9 | P3 sendmmsg-Batching | mittel | ~0,1-0,3 % CPU je UDP-Client — erst messen |

**Gesamturteil:** Für diese Hardwareklasse ist timps bereits nah am Optimum — die
Juli-Befunde (Lock-Halten im Publish, Per-Frame-Heap-Churn, OSD-Rasterizer,
Idle-CPU) sind nachweislich gefixt, und die Architektur (On-Demand überall,
refcounted Fan-out, ein Copy pro Frame in den Hub) ist die richtige. Was bleibt,
ist ein echter Ausreißer (Timelapse-Dauerlast, P1), zwei billige RAM-Gewinne
(S1, S2) und ein lohnender Flash-Refactor (B2). Danach ist der Rest Feilen.
