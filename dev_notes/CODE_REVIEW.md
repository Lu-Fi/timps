# Code Review – microstream

**Datum:** 2026-07-18
**Umfang:** vollständiges Review aller Quellen unter `src/` (~12.500 LOC C), Build-System (`Makefile`, `build.sh`)
**Ziel-Plattform:** Ingenic-SoCs (T31/T33/T40/T41), IMP-SDK, als root laufender, netzwerkexponierter Streaming-Daemon

## Gesamturteil

Für hand-geschriebenen Embedded-C-Netzwerkcode ist microstream **überdurchschnittlich sorgfältig und gehärtet**. Puffergrenzen werden fast überall explizit geprüft, es wurden bereits mehrere Audit-Runden absolviert (sichtbar an M-/L-/H-Kommentaren im Code), und viele klassische Fallen sind schon geschlossen: constant-time Auth-Vergleiche, `/dev/urandom`-Token und -Nonces, atomare Config-Persistenz (`mkstemp`+`rename`+`fsync`), Symlink-/`..`-Schutz beim Clip-Export, ein korrekt konstruiertes Producer-Consumer-System mit sauberem Lebensdauer-Handshake (hub/fanqueue), und ein bemerkenswert defensiver TTF-Parser.

**Es wurden keine kritischen Speicherkorruptions-Bugs im direkten Netzwerk-Eingabepfad gefunden.** Die realen Restrisiken konzentrieren sich auf drei Themen:

1. **Verfügbarkeit (DoS):** durchgängig fehlende Socket-Timeouts kombiniert mit kleinen Verbindungs-Caps → billiger, dauerhafter DoS von RTSP und HTTP.
2. **Stilles Weiterlaufen bei sicherheitsrelevanten Fehlern:** ignorierte Rückgaben bei SRT-Verschlüsselung und IMP-SDK-Aufrufen.
3. **Ungeprüfte Wertebereiche**, die über das (authentifizierte) `/control`-Interface live gesetzt werden können — bis hin zu potenzieller Heap-Korruption im OSD-Renderer.

Priorität vor einem Internet-exponierten Einsatz: die beiden **HOCH-DoS**-Punkte (Timeouts), der **SRT-Passphrase**-Fehlerpfad und die **`font_size`/OSD-Canvas**-Grenzen.

---

## Befunde nach Schweregrad

### HOCH

**H1 – Fehlende Socket-Timeouts → trivialer, dauerhafter Slot-Exhaustion-DoS (RTSP)**
`rtsp.c` (Control-Phase, ~616–635) und `net_sendall()` (`net.c:45`) blockieren unbegrenzt. Ein unauthentifizierter Angreifer, der `RTSP_MAX_CLIENTS` TCP-Verbindungen öffnet und schweigt (bzw. das TCP-Fenster auf 0 hält), belegt alle Slots dauerhaft (globaler Zähler `g_nclients`, gilt auch für RTSPS). RTSP ist damit für alle legitimen Clients tot.
*Fix:* `SO_RCVTIMEO`/`SO_SNDTIMEO` auf akzeptierten fds setzen (z. B. 30 s Control, 10–15 s Send), bei Timeout schließen; alternativ `poll()` mit Deadline.

**H2 – Fehlende Socket-Timeouts → DoS (HTTP)**
Zwei Stellen: der `/control`-Body-Nachlese-Loop (`httpd.c:878–882`) hat — anders als die Header-Phase — keinen Poll-Deadline; und alle `csend`-Pfade über `net_sendall` haben kein Sende-Timeout (`/stream.mp4` öffnen und nie lesen → Thread-Pinning). Bei `HTTP_MAX_CLIENTS 8` genügen 8 Verbindungen für kompletten HTTP-DoS. **Bei passwortlosem Setup unauthentifiziert erreichbar** (`http_check_auth` gibt dann 1 zurück).
*Fix:* Body-Loop denselben Poll-Deadline geben wie der Header-Phase; `SO_SNDTIMEO`/`SO_RCVTIMEO` in `accept_thread` setzen. Legitime Streaming-Clients lesen kontinuierlich und sind nicht betroffen.

**H3 – SRT: Fehler von `SRTO_PASSPHRASE` wird ignoriert → stiller Betrieb ohne Verschlüsselung**
`srt.c:355` prüft die Rückgabe von `srt_setsockflag(..., SRTO_PASSPHRASE, ...)` nicht. libsrt verlangt 10–79 Zeichen; bei zu kurzer Passphrase schlägt der Aufruf fehl und der Listener läuft **unverschlüsselt und ohne Zugangskontrolle** weiter, obwohl der Nutzer glaubt, die Passphrase greife.
*Fix:* Rückgabe prüfen, bei Fehler den Listener nicht starten (`LOGE` + `return`), analog zum bind/listen-Fehlerpfad.

**H4 – Integer-Overflow / fehlende Obergrenze bei OSD-Canvas-Allokation → mögliche Heap-Korruption**
`msttf_render` (`msttf.c:378–384`) berechnet `W`/`H` ohne Obergrenze und alloziert `malloc((size_t)W*H*4)`, während die Füllschleifen in `int` rechnen (signed overflow = UB). `font_size` wird beim Parsen ungeprüft übernommen (`config.c:314: o->font_size=pint(val)`) und ist per `/control` **live** setzbar. Ein sehr großer Wert (oder ein korrupter Font mit `units_per_em=1` und großen Advances) führt zu Wrap/Unterallokation und Schreiben hinter den Puffer.
*Fix:* `pixel_h` in `msttf_render` hart klemmen (z. B. 8..512), `W`/`H` gegen ein Limit (z. B. 4096) prüfen, Größe in `uint64_t` berechnen (Überschreitung → `return -1`); `font_size` in `config.c` beim Parsen klemmen (z. B. 8..256); Schleifenindizes auf `size_t`.

**H5 – OSD-Region-Koordinaten nicht gegen Framegröße geklemmt → SDK-abhängiger OOB im Compositing**
`imp_osd.c` (~191/211/244): Ist die gerenderte OSD-Bitmap breiter/höher als der Stream (großes `font_size` auf Substream, oder `logo_w/logo_h` > Streamgröße), wird `ox=0` gesetzt, aber `rect.p1.x = x+w-1` liegt außerhalb des Frames. Auf mehreren T-SoCs schreibt IMP_OSD dann über die Bildgrenze hinaus. `setup_cover` macht den Clamp korrekt — `refresh_text`/Logo nicht.
*Fix:* `w`/`h` vor `SetRgnAttr` auf die Framegröße begrenzen (Region verwerfen/skalieren, wenn sie nicht passt).

**H6 – IMP-SDK-Rückgabewerte durchgängig ungeprüft → silent failures**
`hal_ingenic.c` (u. a. 498–499, 551, 823–826, 1305–1320): `IMP_FrameSource_SetChnAttr`, `IMP_Encoder_RegisterChn/CreateGroup`, `IMP_System_Bind`, die meisten `IMP_ISP_*` in `isp_init`, `Start/StopRecvPic`. Bei Fehlern läuft die Pipeline scheinbar weiter, liefert aber nie Frames — genau die Klasse Fehler, die im Audio-Pfad bewusst behandelt wird.
*Fix:* mindestens die Bind/Register/Create-Kette in `ing_start`/`jpeg_setup` prüfen und bei Fehler mit Teardown `return -1`.

### MITTEL

**M1 – TLS-Handshake ohne Timeout** (`tls.c:92–97`). Blockierender fd; Client, der nach TCP-Connect schweigt, hängt den Thread für immer (verschärft H1/H2). *Fix:* `SO_RCVTIMEO` vor `ms_tls_accept()` oder `mbedtls_ssl_conf_read_timeout()` + `mbedtls_net_recv_timeout`.

**M2 – Keine TLS-Mindestversion** (`tls.c:59–63`). `MBEDTLS_SSL_PRESET_DEFAULT` erlaubt unter mbedTLS 2.x noch TLS 1.0/1.1. *Fix:* `mbedtls_ssl_conf_min_version(..., TLS 1.2)`.

**M3 – Use-after-free auf TLS-Kontext beim Shutdown** (`rtsp.c:752–757`). `rtsp_stop()` wartet nur 500 ms bounded auf `g_nclients==0` und ruft dann `ms_tls_ctx_free()`; detachte Client-Threads (die wegen fehlender Timeouts unbegrenzt hängen können) referenzieren `conf`/`cert`/`drbg` danach weiter. *Fix:* echte Synchronisation (Refcount) oder joinbare Threads; mind. bounded sleep an ein hartes `shutdown()` der Client-fds koppeln.

**M4 – SRT `SRTO_STREAMID` auf dem Listener ist wirkungslos als Zugriffskontrolle** (`srt.c:353`). Streamid ist Caller-seitig; die streamid akzeptierter Sockets wird nie geprüft. *Fix:* `srt_listen_callback()` registrieren und eingehende streamid gegen `cfg->srt.streamid` vergleichen.

**M5 – Digest-Auth: `realm` und `uri` aus dem Client ungeprüft** (`auth.c:52–92`). Client-`realm` fließt in HA1 statt `AUTH_REALM` zu erzwingen; `uri` wird nie gegen die Request-URI verglichen (RFC-2617-Pflicht); kein `qop`/`cnonce`/`nc`. Praktische Auswirkung begrenzt (Nonce per-Connection zufällig, pro 401 erneuert), aber die uri-Prüfung kostet nichts. *Fix:* `strcmp(realm, AUTH_REALM)` und `strcmp(uri, request_uri)` verlangen.

**M6 – Vorhersagbare Zufallswerte aus `rand()` (Seed `time^pid`)** (`main.c:84`, `rtsp.c:366`, `rtp.c:41–43`). Session-ID, SSRC, Start-Sequenznummer, ts_base vorhersagbar → senkt Hürde für Off-Path-RTP-Injection im UDP-Transport. Token/Digest-Nonces sind bereits auf urandom umgestellt. *Fix:* `getrandom()`/`auth_gen_token()` auch hierfür.

**M7 – Recorder: kein `fsync`, `fclose` ungeprüft → Datenverlust bei Stromausfall** (`record.c:281–287`). `fclose(w_fp)` kann den letzten Flush nicht schreiben (SD voll/gezogen), Rückgabe ignoriert; nie `fsync()` → bis zu `segment_s` Sekunden Aufnahme im Page-Cache verlierbar. *Fix:* `fclose`-Rückgabe prüfen; periodisch `fflush`+`fsync(fileno(w_fp))` (z. B. alle 5–10 s / N Fragmente).

**M8 – Ressourcen-Leak / inkonsistenter Zustand in `ing_start`/`ing_stop`-Fehlerpfaden** (`hal_ingenic.c:1298–1332`, 1394–1431). Schlägt `fs_create`/`enc_create` für Stream i>0 fehl, kehrt `ing_start` mit `-1` zurück, ohne bereits erzeugte FrameSources/Encoder/Groups/Threads abzubauen; `g_nv` zählt nur Slots mit erfolgreichem Thread, sodass zuvor angelegte IMP-Kanäle in `ing_stop` übersprungen (geleakt) werden. *Fix:* Kanal-Teardown von Thread-Existenz entkoppeln; im Fehlerfall sauber freigeben (oder `ing_stop` idempotent + aufrufen).

**M9 – OSD `retired`-Doppelpufferung schützt Use-after-free nicht zuverlässig** (`imp_osd.c:158–201`). Nur genau ein alter Puffer wird zurückgehalten; ohne echtes „Puffer nicht mehr gelesen"-Signal der IMP-Pipeline kann `SetRgnAttr` unter Last einen neuen Puffer setzen, während die HW den gerade freigegebenen liest. *Fix:* Ringtiefe erhöhen (2–3) oder an Frame-Latenz koppeln.

**M10 – Config-Felder in `refresh_text` lockfrei gelesen, während `/control` schreibt** (`imp_osd.c:161ff`). Nur `it->text` wird unter `config_str_lock` gesnapshottet; `font_size/x/y/color/outline/enabled` lockfrei → inkonsistente Kombinationen möglich (in Verbindung mit H4 relevant). *Fix:* gesamten Item-Snapshot unter `config_str_lock` ziehen.

**M11 – Config-Parser: fehlende Bereichsvalidierung numerischer Werte** (`config.c:278–301` u. a.). `width/height/fps/gop/qp/bitrate/…/rtsp.port/http.port` via `pint()` (strtol ohne Fehlerprüfung, „abc"→0) ungeprüft; inkonsistent, da `motion.*`/`record.channel`/`osd.supersample` sauber geklemmt werden. Über `/control` persistierbar → fehlerhafter Wert kann Stream dauerhaft brechen (HAL-init-Fail → `main` return 1 → Crash-Loop unter Respawn). *Fix:* Clamping analog zu `motion.*` (fps 1..sensor-max, qp 1..51, port 1..65535).

**M12 – Tag/Nacht-Logik nutzt Wanduhr statt CLOCK_MONOTONIC** (`daynight.c:286`, 302–305). `time(NULL)*1000` für Dwell/Baseline; NTP-Rücksprung nach Boot macht Deltas negativ → Umschaltung blockiert, Vorsprung hebelt Mindest-Verweilzeit aus. Rest des Codes nutzt bereits monotone Zeit. *Fix:* `ms_now_us()/1000`.

**M13 – `config_get_kv` deckt `record.*` nicht ab → Dedup wirkungslos, Flash-Verschleiß** (`config.c:623–838`). control.c setzt `record.*` via `/control`, aber die Change-Detection meldet sie immer als „unknown" → jeder POST der Record-Seite schreibt `/etc/timps.conf` neu (jffs2-Verschleiß, den die Dedup eigentlich verhindern soll). *Fix:* `record.*`-Zweig in `config_get_kv` ergänzen.

**M14 – Build: keine Härtungsflags** (`Makefile:93–95`, `build.sh:273–283`). Kein `-D_FORTIFY_SOURCE=2`, kein `-fstack-protector-strong`, kein `-Wl,-z,relro,-z,now`. `-no-pie` ist durch non-PIC-Vendor-Archive erzwungen (nachvollziehbar). Für einen als root laufenden Netzdienst fehlt Compiler-Defense-in-depth; `-Wno-stringop-truncation` unterdrückt zudem global eine nützliche Warnklasse. *Fix:* mind. für musl-Builds SSP + FORTIFY + RELRO aktivieren.

**M15 – Toolchain-Download ohne Integritätsprüfung** (`build.sh:85–104`). Cross-Toolchain-Tarball wird von GitHub geladen und ausgeführt ohne SHA256/Signatur — im Gegensatz zu den vorbildlich commit-gepinnten Repos. Supply-Chain-Risiko. *Fix:* SHA256 pinnen und nach Download verifizieren.

**M16 – msttf: Scanline-Schnittpunkte hart auf 128 begrenzt** (`msttf.c:437`). Weitere Kreuzungen werden verworfen; bei ungerader behaltener Zahl versagt das Even-Odd-Füllen → falsch gerenderte Zeile bei dichten Konturen (kein Speicherfehler). *Fix:* `xint` dynamisch wachsen lassen oder `nx` auf gerade Zahl runden.

### NIEDRIG (Auswahl)

- **L1 – RTSP Client-Cap check-then-act nicht atomar** (`rtsp.c:669`): Cap um 1 überschreitbar. Fix: `__sync_add_and_fetch` vor Annahme.
- **L2 – `send_resp()` klemmt `n` nicht auf Puffergröße** (`rtsp.c:273`): latent, aktuell unerreichbar; `n` nach snprintf cappen.
- **L3 – RTP: Sink-Fehler in `emit()` verworfen** (`rtp.c`): restliche NALs werden trotz totem Client paketiert (CPU-Verschwendung).
- **L4 – SRT doppeltes `srt_close(g_ls)`** (`srt.c:393` vs. 413): Race auf Socket-ID-Wiederverwendung.
- **L5 – `record_clip` `fclose` ungeprüft** (`record.c:545`): abgeschnittener Clip als Erfolg gemeldet.
- **L6 – `seg_open` fwrite-Fehler lässt Stub-Datei liegen** (`record.c:230`): `unlink(path)` im Fehlerpfad ergänzen.
- **L7 – TLS-gepufferte Daten unsichtbar für `poll()`** (`httpd.c:757`, nur TLS-Builds): `mbedtls_ssl_get_bytes_avail` vor poll prüfen.
- **L8 – Signalhandler erst nach Dienst-Start installiert** (`main.c:112`): SIGINT/SIGTERM während IMP-init → Abbruch ohne HAL-Teardown. Handler vor `init()` registrieren.
- **L9 – `config_load` zerhackt Zeilen >511 Zeichen still** (`config.c:868`): fehlendes `\n` erkennen, Rest verwerfen + `LOGW`.
- **L10 – `record.name`/`timelapse.name`/`*.dir` via `/control` ohne `..`-Prüfung** (record.c:192, timelapse.c:126): authentifizierter Nutzer kann außerhalb des records-Baums schreiben (kein Privilegiensprung, aber Hardening — `..`-Prüfung wie in `record_clip` übernehmen).
- **L11 – `hub->nsub` für Logging nach Lock-Freigabe gelesen** (`hub.c:161/183`): benigner Race.
- **L12 – `ms_base64`-Deklaration außerhalb des Include-Guards** (`util.h:51`): harmlos, aber Versehen.
- **L13 – `strftime` mit teils nutzerkontrolliertem Format** (`osd_vars.c:200`): kein Speicherfehler, aber unerwartete Ausgabe; `%`-Konversionen ggf. whitelisten.
- **L14 – `fmp4` `mfhd`-seq (32-bit) / `a_timescale<<16`** (fmp4.c:369/261): nur bei >2 Jahren Dauerverbindung bzw. >65535 Hz relevant; dokumentieren/klemmen.
- **L15 – `hal_sim` deckt piggyback-JPEG nicht ab** (hal_sim.c:199): Test-Backend nicht deckungsgleich mit `hal_ingenic`.

---

## Explizit geprüft und in Ordnung

- **md5.c** – korrekte, saubere MD5-Implementierung.
- **Auth-Kern** – constant-time-Vergleiche, urandom-Token/-Nonces, korrekte Puffergrößen; `/control` und `/events` hinter globalem Auth-Gate + „local only" wenn kein User konfiguriert.
- **HTTP-Parsing** – gebundenes `sscanf`, CR/LF-Schnitt (kein Response-Splitting), Truncation-Guards; **kein Path-Traversal** (Server liefert keine Dateien vom Dateisystem).
- **fMP4-Boxstruktur** – Box-Größen-Patching gegen OOM-Teilbäume abgesichert, esds/trun/trex/tfhd korrekt, tfdt sauber 64-bit, `aac_adts_strip` bounds-safe.
- **hub/fanqueue/frame** – Publish/Unsubscribe-Handshake verhindert Use-after-free, konsistentes Refcounting, durchdachtes Overflow-Verhalten (drop-oldest + GOP-Pruning + IDR-Request), Producer blockiert nie.
- **NAL/vparam/aac/g711** – bounds-sicher, defensive Exp-Golomb-Reader.
- **config_write_keys** – atomar (mkstemp+rename, fsync auf Datei und Verzeichnis, Writer-Mutex) — vorbildlich für jffs2.
- **msttf-Bounds-Checks** – konsequent defensiv gegen korrupte Fonts (loca/glyf/cmap-Grenzen, Rekursionslimit bei Composite-Glyphs, OOM-Behandlung). Ausnahme siehe H4/M16.
- **record_clip** – Pfadvalidierung (`/tmp/`-Prefix + `..`-Filter), Sekunden-Clamp, Serialisierung per trylock.
- **`on_motion`/`switch_cmd` (`system()`)** – bewusst nicht über `/control` setzbar → kein Injection-Vektor, solange die Config-Datei vertrauenswürdig ist.

---

## Empfohlene Reihenfolge der Behebung

1. **H1, H2, M1** – Socket-/Handshake-Timeouts (`SO_RCVTIMEO`/`SO_SNDTIMEO` + Body-Poll-Deadline). Höchster Impact bei geringstem Aufwand.
2. **H3, M4** – SRT-Passphrase-Fehler behandeln, streamid serverseitig prüfen.
3. **H4, H5, M10, M11** – Wertebereiche klemmen (`font_size`, Config-Numerik), OSD-Region gegen Framegröße begrenzen, Item-Snapshot unter Lock.
4. **H6, M8** – IMP-Rückgaben prüfen und Fehlerpfade sauber aufräumen.
5. **M7** – `fsync`/`fclose`-Prüfung im Recorder (Datenintegrität).
6. **M14, M15** – Build-Härtung + Toolchain-Integrität.
7. Restliche MITTEL/NIEDRIG nach Kapazität.
