# timps — Code-Review (Fable 5, 2. Pass)

_Stand: 2026-07-31 · Quelle: `src/` (~11k LoC C) · Basis: HEAD `d4ed99f` +
uncommittete Diffs auf `src/daynight.c` (neue Vor-Schalt-Hysterese) und
`src/hal/hal_ingenic.c` (GetISPRunningMode-Diagnose). Nichts wurde geändert —
reines Review._

Dritter Voll-Pass über `src/`, bewusst TIEFER angesetzt als die beiden
Vorgänger-Reviews. Schwerpunkt laut Auftrag: (1) der heute Nacht deployte,
raptor-inspirierte Hysterese-Umbau in `daynight.c`, (2) die weniger
beachteten Pfade (`mp4/httpd.c`, `mp4/fmp4.c`, `record.c`, `timelapse.c`,
`srt.c`, `codec/*`, `auth.c`, `tls.c`, `events.c`, `imp_motion.c`), (3) alles,
was aus unauthentifiziertem Netz-Input erreichbar ist, sowie
Cross-File-Invarianten und Fehlerpfade. Datei-/Zeilenangaben beziehen sich auf
den aktuellen Stand.

---

## 0. Was aus den Vorreviews geprüft wurde (nicht dupliziert)

Beide bestehenden Reviews wurden vollständig gelesen; ihre Befunde werden hier
**nicht** wiederholt, sondern nur der Status bestätigt, wo im Vorbeigehen
mit-geprüft:

- **H1 (RTSP-SETUP-fd-Leak) — bleibt BEHOBEN.** `rtsp.c:524-525` schließt ein
  bereits gebundenes UDP-Paar vor dem Rebind; der Backchannel-SETUP-Pfad
  (`:470-471`) ebenso. Auch ein wiederholtes SETUP *verschiedener* Tracks
  (v_udp/a_udp/bc_udp getrennt) leakt nicht. Vollständig.
- **H2 (OSD-`calloc` NULL-Deref) — bleibt BEHOBEN** (`msttf.c`, `if (cov && xint)`).
- **H3 (`hub_publish` Lock-Haltedauer) — bleibt BEHOBEN.** Der
  `g_pushing[]`/`g_push_done[]`-Handshake gegen `hub_unsubscribe()`
  (`hub.c:74-75,192-196,240,254-259`) ist mit genau einem Producer pro Source
  korrekt; kein UAF, kein Deadlock zwischen `g_act_lock` und `s->lock`.
- **opus48-Befunde N1–N3 (alter DN_REASSERT_MS-Einzelschuss):** durch den
  Hysterese-Umbau (siehe Abschnitt 1) inhaltlich überholt; die Beobachtung N2
  (kein Ground-Truth-Signal, ob der Set gelatcht hat) besteht sinngemäß fort,
  siehe unten. N4 (`play_write` fehlender `rn>0`-Guard) und N5
  (`S96onvif_discovery` im Repo) unverändert — **nicht** erneut aufgeführt.
- **H5 (Timelapse-Dauer-Subscription) / M5–M8 / L-Reihe** aus dem
  Fable-5-Review 07-17: nicht systematisch nachverifiziert (waren nicht der
  Auftrag dieses Passes), soweit gestreift unverändert.

Der Allokations-Sweep der beiden Vorreviews wurde stichprobenartig
nachvollzogen (`hal_ingenic.c:880/978/1472/1510/1539`, `fmp4.c` scratch-TLS,
`control.c` ctrl_changes/js) — alle NULL-geprüft. **Keine der drei alten Highs
ist zurückgekehrt.**

---

## 1. Status des `daynight.c`-Hysterese-Umbaus (heute Nacht deployt)

Der Umbau (Vor-Schalt-Hysterese `DN_HYSTERESIS_MS`, komponiert mit
`DN_SETTLE_MS` Cold-Start-Guard und `transition_s` Post-Switch-Dwell, plus
`DN_REASSERT_COUNT=2` als Defense-in-Depth) wurde intensiv gegen die vom
Auftrag genannten Klassen geprüft:

- **Interaktion Settle/Hysterese/Dwell — korrekt.** Die drei Guards sind sauber
  geANDet (`:513-537`). Während `settle` wird der Kandidat aktiv zurückgesetzt
  (`:515-516`), also nicht aus dem AE-Transienten geseedet; während `dwell`
  akkumuliert die Hysterese weiter, solange der Kandidat KONTINUIERLICH hält
  (jede Rück-Exkursion in die Dead-Zone trifft den `else`-Zweig `:538-543` und
  verwirft den Kandidaten). Kein Pfad gefunden, in dem ein Switch stumm fällt
  oder ein Kandidat dauerhaft hängen bleibt. Cold-Start braucht Settle(5s) +
  Hysterese(5s), das ist gewollt.
- **Millisekunden-Timestamp-Mathematik — kein Overflow/Wraparound.** Alle
  Zeitgrößen (`ms_now_us()/1000`, `last_switch_ms`, `pending_since_ms`,
  `settle_until_ms`, `reassert_at_ms`, `night_entered_ms`) sind `int64_t`-ms
  auf `CLOCK_MONOTONIC`; die Produkte `transition_s*1000` etc. bleiben weit
  unter INT64_MAX. Über realistische Uptimes (Jahre) tritt kein Überlauf ein.
- **Thread-Safety des Hysterese-Kandidaten — korrekt.** `pending_target`,
  `pending_since_ms`, `reassert_*`, `cur`, `night_baseline` sind allesamt
  Stack-Locals von `dn_thread` und werden von keinem anderen Thread berührt.
  `dn_status_update`'s Notify-Statics werden nur vom Sampling-Thread berührt.
- **Reassert bei zwischenzeitlichem Auto-Aus — benigne.** Der Reassert-Block
  (`:374-388`) läuft vor dem `enabled`-Check, überspringt aber bei
  `!dn->enabled` das `hub_control` und zählt nur den Countdown herunter. Eine
  manuelle Übernahme im Post-Switch-Fenster wird also NICHT überschrieben.
  Verifiziert korrekt.

**Verbleibende Schwäche (fort aus opus N2, jetzt gegen den neuen Code):** Der
Reassert (`:381`) und `isp_apply_image` (der neue Diagnose-Readback,
`hal_ingenic.c` Diff) haben weiterhin KEIN belastbares Signal, ob
`SetISPRunningMode` tatsächlich gelatcht hat — der Diff-Kommentar sagt selbst,
`GetISPRunningMode` ist nur ein Userspace-Echo. Die Hysterese adressiert
opus-N1 (Set landet nicht mehr mitten in der Gain-Rampe) plausibel, aber die
Overnight-Log-Prüfung kann Wirksamkeit weiterhin nicht *beweisen*. Empfehlung
unverändert: die `ISP Runing Mode :`-Zeile aus `/proc/jz/isp/isp-m0` (wird in
`dn_brightness()` `:102-103` ohnehin schon geparst) nach dem Switch/Reassert
zurücklesen und bei Abweichung WARNen. Kein neuer Bug — bekannte
Beobachtbarkeitslücke.

---

## Kritisch / High (neu)

**Keine neuen High-Findings.** Insbesondere wurde KEIN speicherunsicherer Pfad
aus unauthentifiziertem Netz-Input gefunden: die vor-Auth erreichbaren Parser
(RTSP `OPTIONS`/`extract_path`/`hdr_int`, HTTP `sscanf("%7s %255s")`,
`http_cors`-Origin-Reflexion) sind alle längenbegrenzt und schneiden an
CR/LF; die Bitstream-Parser (`codec/nal.c`, `codec/vparam.c`,
`backchannel.c` RTP/AAC) sind durchgehend bounds-gecheckt (Exp-Golomb-Reader
klemmt am `nbits`-Ende, `decode_aac` hat den `BC_AAC_MAX_BLK`-Headroom-Guard,
`rtp_payload_off` prüft `off>=len`). SRT/MPEG-TS-Muxer (`srt.c`) rechnet
Section-/PES-/AF-Längen korrekt (`send_pes` `hdr[19]` reicht, `adts[8192]`
lehnt Überlänge via `frame_len>out_cap` ab).

---

## Mittel

**F1 — DN-Zeitfenster-Strings werden zur Laufzeit über `/control` mutiert, aber
im Detektions-Thread OHNE `config_str_lock` gelesen; die Sperr-Invariante in
`config.c` behauptet fälschlich Vollständigkeit.**
`daynight.c:441` ↔ `config.c:677-678` ↔ `config.c:27-31`

`daynight.time_night_start`/`time_day_start` (`config.h:188-189`, je `char[6]`)
sind über `/control` setzbar (`control.c:361-364` →
`config_apply_kv` → `copystr()`/`strncpy`, in `control.c` unter
`config_str_lock`). Gelesen werden sie im Sampling-Thread durch
`dn_time_target(dn->time_night_start, dn->time_day_start, …)` (`daynight.c:441`)
**ohne** den Lock. Das ist ein echter Data-Race: `copystr` schreibt erst
`strncpy(dst, src, n-1)` und terminiert danach `dst[n-1]=0` — im Fenster
dazwischen kann der Leser ein nicht-terminiertes 6-Byte-Feld sehen, worauf
`sscanf("%d:%d")` über das Array hinaus in das benachbarte Struct-Feld
(`sun_latitude`) hineinliest (bounded, aber UB), bzw. für ein Sample einen
falsch geparsten HH:MM-Wert nimmt.

Verschärfend: der Doku-Kommentar in `config.c:20-35` zählt die
lock-geschützten Felder EXPLIZIT und ABSCHLIESSEND auf (osd-Text,
video.rtsp_path, sensor.model, record/timelapse dir/name) — und **vergisst
genau diese beiden DN-Strings**. Damit ist es die Art latenter Falle, die
künftige Maintainer (und beide Vorreviews, deren M3 nur die aufgezählte Menge
prüfte) übersehen. `switch_cmd`/`isp_path` sind demgegenüber sicher: sie werden
NICHT über `/control` exponiert, nur beim Config-Load (single-threaded)
gesetzt.

Realweltliches Risiko ist gering (nur relevant bei `daynight.mode=time` UND
gleichzeitigem Live-Edit des Fensters, Effekt i.d.R. ein transient
falsch geparstes Sample, das die Hysterese ohnehin abfängt) — aber es ist eine
klare Verletzung der selbst dokumentierten Invariante.
_Fix:_ In `dn_thread` beide Strings unter `config_str_lock` in lokale Puffer
snapshotten (wie es `record.c`/`timelapse.c` für dir/name tun), und die beiden
Felder in die `config.c`-Doku-Liste aufnehmen.

**F2 — Ge-`accept()`-te TCP-Client-Sockets (RTSP + HTTP) sind nicht
`FD_CLOEXEC`; von `daynight`/`motion` geforkte Board-Skripte erben Kopien
aller Live-Client-/Media-Sockets.**
`rtsp.c:841` + `mp4/httpd.c:1001` ↔ `net.c:14-17,67,74` ↔ `daynight.c:158-165`,
`imp_motion.c:165-179`

`net_cloexec()` wird konsequent auf die LISTENER (`net_listen_tcp`, `net.c:67`)
und die UDP-Media-Sockets (`net_udp_socket`, `:74`) angewandt — der
net.c-Kommentar nennt exakt diesen Zweck ("child processes we spawn don't
inherit our listen/media sockets"). Die per `accept()` angenommenen
TCP-Client-fds bekommen aber **nie** `FD_CLOEXEC`: `accept()` setzt es nicht
(kein `accept4(SOCK_CLOEXEC)`), und weder `net_set_timeouts()` noch
`net_set_nodelay()` fügen es hinzu. `daynight.c:dn_switch()` (jeder Tag/Nacht-
Wechsel) und `imp_motion.c` `on_motion` (jedes Motion-Event nach Cooldown)
forken+exec'en Board-Skripte; der Kindprozess erbt damit für seine Laufzeit
Kopien SÄMTLICHER offener RTSP-/HTTP-Streaming-Sockets.

Folge: schließt timps einen Client (Disconnect), bleibt die TCP-Verbindung
halb-offen (kein FIN), solange das Skript lebt. Bei kurzen `color`-Skripten
harmlos; ein langsames/daemonisierendes `on_motion` (z. B. Telegram-Upload)
hält die Sockets sekundenlang und kann auf einer 24/7-Kamera-Flotte lingernde
Verbindungen ansammeln — genau die Klasse, gegen die net.c bewusst schützt,
hier aber unvollständig.
_Fix:_ direkt nach `accept()` in beiden Accept-Schleifen `FD_CLOEXEC` auf `cfd`
setzen (bzw. `accept4(SOCK_CLOEXEC)` wo verfügbar). Ein kleiner
`net_set_cloexec(fd)`-Export aus net.c hält es konsistent.

---

## Niedrig

**F3 — DN-numerische Keys über `/control` unbegrenzt (`pint`/`pflt` ohne
Clamp).** `config.c:683-691` (`total_gain_*`, `threshold_*`, `hysteresis`,
`day_gain_pct`, `baseline_delay_s`, `transition_s`).
Andere Keys nutzen `pint_cl()` (M11), diese nicht. Ein negativer/absurder Wert
degradiert die Detektion still (z. B. `transition_s<0` ⇒ Dwell nie aktiv,
`day_gain_pct=0` ⇒ Adaptiv-Baseline deaktiviert). Kein Crash, kein Overflow
(die Konsumenten in `daynight.c` guarden `interval_ms>0`, `day_gain_pct>0`),
aber ein Footgun ohne unteres/oberes Geländer. _Fix:_ analog `pint_cl` mit
sinnvollen Ranges klemmen, damit der zurückgelesene/persistierte Wert
idempotent bleibt.

**F4 — `gethostname()`-Rückgabe ungeprüft; bei überlangem Hostnamen ggf.
nicht-terminierter Puffer.** `record.c:177,231`, `record.c:511`-Nähe,
`timelapse.c:118,156`.
`char host[64]="camera"; gethostname(host,sizeof host);` — Linux terminiert bei
einem Hostnamen ≥64 Byte nicht garantiert, das nachfolgende
`snprintf("%s",host)` läse dann über das Array hinaus. In der Praxis begrenzt
`HOST_NAME_MAX` das, daher niedrig; sauber wäre `if (gethostname(host,sizeof
host)!=0) host[0]=0; host[sizeof host-1]=0;`.

**F5 — `play_write()` fehlt weiterhin der `rn>0`-Guard (opus N4), hier nur zur
Vollständigkeit re-bestätigt, nicht dupliziert.** `speaker.c:349`.

---

## Verifiziert in Ordnung (neu bzw. tiefer geprüft)

- **fMP4-Muxer (`fmp4.c`):** `box_close`/`fragment`/`fmp4_init_segment`
  behandeln `ms_buf`-`err` sticky und korrekt (kein OOB-Patch nach
  fehlgeschlagenem Grow); `pts_track_time` hält tfdt strikt monoton, die
  M2-Reanchor-Logik springt nur vorwärts; die `stsd`-Samplerate ist gegen
  16.16-Overflow geklemmt (`:278`). Der pthread-TLS-Scratch (`scratch_get`) ist
  pro Thread isoliert — `rec_thread`, jeder HTTP-`stream_mp4` und `record_clip`
  laufen auf eigenen Threads, kein geteilter Puffer.
- **`mp4/httpd.c`:** Header-/Body-Akkumulation mit bounded Poll-Deadline
  (H2-Fix), negatives/überlanges `Content-Length` wird als 413 abgewiesen
  (`:935`), Token/CORS/Basic-Gate korrekt gestaffelt, SSE-`sse_emit` droppt
  Überlänge statt zu truncaten. `snapshot_jpg` Piggyback-Arithmetik
  (`vsrc = src-(HUB_JPEG_SRC+1)`) stimmt mit `HUB_JPEG_SRC_N` überein;
  Unsubscribe-Reihenfolge (Helfer zuerst) vermeidet UAF via
  `hub_unsubscribe`-Handshake.
- **`record.c`/`timelapse.c`:** Pfad-Validierung (`..`-Komponente + absoluter
  Name) korrekt; `find_oldest`/`prune_old` depth-bounded + `lstat`
  (folgt keinen Symlinks); Short-Write/`fsync`-Fehler schließen das Segment
  statt "healthy" zu lügen; `record_clip` mit `O_EXCL|O_NOFOLLOW`, `/tmp/`-
  Restriktion und `trylock`-Single-Slot. SD-voll/-entfernt-Szenarien sauber.
- **`srt.c`:** `SRT_MAX_CLIENTS`-Cap + Drain vor `srt_cleanup`, Listener-Close
  genau einmal (`__sync_lock_test_and_set`), Passphrase-Pflichtprüfung
  (verweigert unverschlüsselten Start), streamid-Callback. TS-Bit-Twiddling
  (PAT/PMT/PES/AF/CRC) stimmt.
- **`codec/*`:** `nal_iter` überspringt Zero-Length-NALs (kein AU-Abbruch),
  `aac_adts_strip` bounds-gecheckt, `deemulate`/Exp-Golomb OOB-sicher,
  `g711`-Tabellen in-bounds.
- **`auth.c`/`tls.c`:** Basic/Digest konstant-Zeit (equal-length compare),
  Digest-Nonce muss server-issued sein (Replay-Schutz), Nonce/Token aus
  `/dev/urandom`; mbedTLS-Handshake mit `SO_RCVTIMEO`-Bound, TLS>=1.2 erzwungen.
- **`events.c`/`imp_motion.c`:** Motion-Snapshot-Ring lossless mit
  Per-Connection-Cursor + Lap-Handling, Config-Tabelle mit Evict-Resync;
  `imp_motion` `on_motion` via double-fork+execlp (kein `system()`, keine
  Zombies), voller Rollback bei Thread-Create-Fehler.
- **`hub.c`/`fanqueue.c`:** Publish-Snapshot-außerhalb-Lock + Push-Handshake
  race-frei; Drop-oldest inkl. Vorwärts-Drop bis Keyframe korrekt refcount-t.

---

## Empfohlene Reihenfolge

1. **F1** — kleiner, klar umrissener Lock-Fix + Doku-Korrektur; schließt eine
   real dokumentierte Invarianten-Lücke.
2. **F2** — `FD_CLOEXEC` auf akzeptierte Sockets; ein Zweizeiler pro
   Accept-Schleife, verhindert lingernde Client-Verbindungen bei
   Motion-/Daynight-Skripten.
3. **Beobachtbarkeit** des DN-Reasserts (isp-m0-Readback + WARN) vor dem
   nächsten Overnight-Test, damit die Prüfung aussagekräftig wird.
4. **F3, F4, F5** — Kleinkram/Härtung.
