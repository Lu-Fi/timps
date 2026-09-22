# timps – Funktionsübersicht

**timps** („Tiny IMP Streamer") ist ein schlanker RTSP-/fMP4-/MJPEG-Streamer für
IP-Kameras mit Ingenic-SoC unter [thingino](https://github.com/themactep/thingino-firmware).
Er ist in reinem C direkt auf die Hersteller-Bibliothek `libimp` aufgesetzt – ohne
live555, libconfig, libwebsockets oder libschrift – und ist eine Alternative zu
prudynt-t / raptor.

* Quellcode: <https://github.com/Lu-Fi/timps>
* Dokumentation: `docs/wiki/` im Repo (Home, Architecture, Configuration-Reference,
  HTTP-Control-API, Streaming-Protocols, Day-Night, Audio, Motion-Detection,
  Recording-Timelapse, Rate-Control-\*, Building, Logging, Testing-QA,
  Platform-SDK-Support)
* Stand dieses Dokuments: `main`, Version v1.9.18 (2026-09-15);
  gestripptes `timpsd` ca. 360 KB (mipsel). Mit **seit v1.9.19 (unveröffentlicht)**
  markierte Aussagen stehen bereits im Quellcode, aber in noch keinem Release –
  auf einer v1.9.18-Kamera gilt jeweils das vorher beschriebene Verhalten.
* Konfiguration: eine flache Textdatei `/etc/timps.conf` im Format `key = value`
* Ein einziges Binary (`/usr/bin/timpsd`), gestartet über `/etc/init.d/S95timps`

---

## Streaming & Protokolle

* **RTSP-Server (Eigenimplementierung, kein live555)** – Video + Audio,
  `rtsp.enabled`, `rtsp.port` (Default 554). Pfade sind pro Stream frei wählbar
  (`video0.rtsp_path = /ch0`, `video1.rtsp_path = /ch1`).
* **Zwei Videostreams** (`video0.*` = Haupt-, `video1.*` = Substream), jeweils
  H.264 oder H.265 (`videoN.codec = h264 | h265`), eigene Auflösung, FPS,
  Bitrate und Ratenkontrolle.
* **RTP-MTU einstellbar** (`rtsp.mtu`, 548–1472, Default 1200) – überlebt
  WireGuard-/OpenVPN-/PPPoE-Pfade ohne IP-Fragmentierung.
* **Browser-Vorschau über fragmentiertes MP4 (MSE)** – `http://<ip>:8880/`
  bzw. `/stream.mp4`, inklusive Ton. Stream wählbar über `http.preview_chn`.
* **Adaptives Frame-Dropping pro fMP4-Client** (`http.adaptive_drop = 1`):
  ein langsamer Client friert allein auf seinem letzten Bild ein und steigt am
  nächsten Keyframe sauber wieder ein – Encoder und andere Abonnenten
  (Frigate, Aufnahme, weitere Zuschauer) bleiben unberührt.
* **JPEG & MJPEG** – `/snapshot.jpg` und `/stream.mjpeg` (`jpeg.enabled`,
  `jpeg.width/height/quality/fps/imp_chn`), optional zusätzlich als Datei
  (`jpeg.snapshot_path`).
* **JPEG in Stream-Auflösung** – ein zusätzlicher JPEG-Encoder kann auf jedem
  Videostream mitlaufen (`video0.jpeg = true`, `videoN.jpeg_quality/_fps/_chn`)
  und wird über `/snapshot.jpg?chn=N` bzw. `/stream.mjpeg?chn=N` abgerufen –
  ohne zusätzlichen rmem-Bedarf, da die Framesource geteilt wird.
* **WebRTC / WHEP (optional, `USE_WEBRTC`)** – `POST /webrtc/whep` nimmt das
  SDP-Angebot des Browsers entgegen und antwortet als ICE-lite-Peer;
  DTLS-SRTP (`SRTP_AES128_CM_HMAC_SHA1_80`), H.264-Video plus G.711-Audio,
  `DELETE /webrtc/whep/<id>` beendet die Sitzung sofort. Keys: `webrtc.enabled`
  (0/1/2, Default 2), `webrtc.port`, `webrtc.port_max`, `webrtc.channel`.
  Maximal 4 gleichzeitige Sitzungen. Bewusste Grenzen: nur LAN/VPN (ein
  einziger Host-Kandidat, kein STUN/TURN, kein IPv6), kein Opus/AAC/H.265,
  kein NACK/FEC, keine Staukontrolle.
* **SRT-Ausgabe (optional, `USE_SRT`)** – MPEG-TS über SRT, als Listener
  (`srt.mode = listener`, Default, `ffplay srt://<ip>:9000`) oder als Caller
  (`srt.mode = caller` + `srt.host`, für Kameras hinter NAT, mit eigenem
  Reconnect-Backoff 1 s→30 s). Weitere Keys: `srt.port`, `srt.channel`,
  `srt.latency_ms`, `srt.streamid`, `srt.passphrase`. Link-Statistik (RTT,
  Loss, Retransmits) alle 10 s im Log und unter `srt` in `GET /control`.
* **On-Demand-Encoding** – jeder Stream (H.264/H.265, Audio, JPEG/MJPEG) wird
  nur erzeugt, solange mindestens ein Client zusieht. Im Leerlauf praktisch
  0 % CPU. Nicht konfigurierbar, das ist das Grundverhalten.

## Encoder & Ratenkontrolle

* **Ratenkontrollmodi** `videoN.rc_mode = cbr | vbr | fixqp | smart |
  capped_vbr | capped_quality`, dazu `bitrate`, `gop`, `profile` (0 Baseline,
  1 Main, 2 High), `qp`, `min_qp`, `max_qp`, `buffers`.
* **Klassische SoC-Ratenkontrolle (T10–T30)** zusätzlich über `quality_lvl`
  (0–7), `change_pos` (50–100), `i_bias_lvl` (−3…3) und `fluc_lvl` (nur H.265).
* **Live-Anwendung eines Teils der Encoder-Keys** ohne Neustart, je nach SoC:
  T10–T30 `rc_mode/bitrate/qp/min_qp/max_qp/quality_lvl/change_pos/i_bias_lvl`;
  T31/C100 `bitrate/min_qp/max_qp/i_bias_lvl`; T40 `bitrate/min_qp/max_qp`;
  T41 `bitrate/min_qp/max_qp`. `qp` (fixqp) ist auf den Neu-API-SoCs
  (T31/C100/T40/T41) bewusst **nicht** live – auf einem T31X gemessen
  (2026-08-22) bewegt der Live-Pfad den Bitstrom überhaupt nicht, während der
  Boot-Pfad zwischen qp 25 und 42 um den Faktor 6,4 spreizt.
  Was dieser Build live kann, steht in
  `caps.video_live`; was pro Request *nicht* live ging, listet die POST-Antwort
  unter `deferred_keys`.
* **Encoder-Telemetrie** in `GET /control` unter `encoder.<n>` – inklusive
  `encoder.<n>.rc`: das, was der Encoder wirklich hält (Rückleseweg über
  `IMP_Encoder_GetChnAttrRcMode`), getrennt von der konfigurierten Sollgröße.
  Dazu Backlog-Zähler und `queue_drops` je Stream.
* **Nachvollziehbare Queue-Überläufe** (*seit v1.9.19, unveröffentlicht*):
  `queue_drops` zählt Verwürfe in der Warteschlange eines *Konsumenten*. Eine
  Sammelmeldung nennt jetzt höchstens einmal pro 60 s je (Konsumentenart,
  Stream) auch die Art – `rec`, `rtsp`, `mp4`, `webrtc`, `srt`:
  `chn=0 rec: 12 queue overflows in the last 60s`. Zusätzlich sind die
  IDR-Anforderungen zur Fehlerbehebung **pro Stream** auf 1/s begrenzt (vorher
  pro Konsument), sodass mehrere langsame Clients den gemeinsamen Encoder nicht
  mehr vervielfacht mit Keyframes belasten; eine unterdrückte Anforderung geht
  nicht verloren, sondern wird nachgeholt – es sei denn, vorher wird ohnehin
  ein Keyframe veröffentlicht, dann entfällt sie (ein Überlauf kostet so ein
  erzwungenes IDR, nicht zwei). Kein Konfigurationsschlüssel.
* **Bildrotation (optional, `USE_ROTATE`)** – `videoN.rotation = 0|90|180|270`.
  Hardware-90/270 auf T40/T41 (I2D) und T31 (FrameSource), Software-90/270 auf
  T23 (`USE_SW_ROTATE`, CPU-intensiv, nur H.264). Echte per-Kanal-180°-Drehung
  nur auf T40/T41; sonst stattdessen `image.hflip` + `image.vflip`.

## Bild & ISP-Tuning

* Vollständiges ISP-Tuning über `image.*`, alles live setzbar und persistent:
  `brightness`, `contrast`, `saturation`, `sharpness`, `hue`, `hflip`, `vflip`,
  `anti_flicker`, `running_mode` (Tag/Nacht-Pipeline), `ae_compensation`,
  `max_again`, `max_dgain`, `sinter_strength` / `temper_strength` (räumliche /
  zeitliche Rauschunterdrückung), `dpc_strength`, `defog_strength`,
  `drc_strength`, `highlight_depress`, `backlight_compensation`,
  `core_wb_mode`, `wb_rgain`, `wb_bgain`.
* **Kein blindes Setzen**: welche dieser Regler der jeweilige SoC wirklich
  unterstützt, meldet `caps.image` in `GET /control` – die WebUI graut den Rest
  aus. Nicht unterstützte Werte werden trotzdem gespeichert.
* **Sensor-Autoerkennung** – `sensor.model/i2c_addr/fps/width/height` dürfen
  fehlen; timps liest sie dann aus der Kernel-Registry
  `/proc/jz/sensor/sensor0/`. Eine Konfigurationsdatei passt damit für viele
  Kameras. *Seit v1.9.19 (unveröffentlicht)* wird die **automatisch** ermittelte
  `sensor.fps` bei **30** gedeckelt (manche Treiber melden eine Rate, die ihr
  Takt nicht liefert – der GC2053 meldet 40 bei einem 30-fps-Modus); ein
  explizit gesetzter Wert gilt unverändert. Zusätzlich wird die gesetzte Rate
  zurückgelesen und protokolliert: `sensor fps: requested N, driver holds n/d,
  set rc=R` (WARN, wenn der Treiber etwas anderes hält). `videoN.fps` ist nur
  die Rate *dieses Streams* und setzt nie die Sensorrate.
* Optionale Deckelung der AE-Integrationszeit (`image.ae_it_max_us`) für
  Szenen, in denen die Automatik zu lange belichtet.

## Day/Night (automatische Tag-/Nachtumschaltung)

Ersetzt thingino's eigenständigen `daynightd` (der Firmware-Paketbau
deaktiviert `S97daynightd`, wenn `USE_DAYNIGHT` an ist).

* **Messgröße ist der Belichtungsindex**, nicht die reine Verstärkung:
  `D = total_gain × (Integrationszeit / max. Integrationszeit)` – höher =
  dunkler. Dadurch bleibt die Messung auch dann aussagekräftig, wenn `total_gain`
  bei 256 (1×) am Boden liegt.
* **Asymmetrisches Vorgehen**: Tag→Nacht ist eine ehrliche Messung (IR-Sperrfilter
  zu, Beleuchtung aus). Nacht→Tag wird *nur* durch eine Probe entschieden, denn
  nachts misst die Kamera teilweise ihr eigenes IR-Licht.
* **Stille IR-Probe** (`daynight.irprobe_cmd`, Default `timps-irprobe`, Aufruf
  als `<cmd> on|off`): IR-Beleuchtung kurz aus, Verhältnis der Messwerte
  vergleichen – kostet kein Klicken des IR-Cut-Filters. Erst wenn das nicht
  geht oder mehrdeutig ist, greift die hörbare IR-Cut-Probe.
* **Schwellen**: `daynight.day_gain` (Default 768 = 3×, darunter ist Tag) und
  `daynight.night_gain` (Default 4096 = 16×, darüber ist Nacht). Alte Namen
  `total_gain_day_threshold`/`total_gain_night_threshold` funktionieren weiter.
* **Ökonomie der Proben**: `day_confirm_s` (30), `probe_min_gap_s` (600),
  `probe_confirm_s` (15), `heartbeat_s` (14400 = 4 h) und `heartbeat_max_s`
  (43200 = 12 h für Szenen, die sich nachweislich nicht ändern), `boot_probe`,
  `interval_ms` (2000).
* **Kalender optional**: `daynight.mode = auto | schedule`, festes Zeitfenster
  (`time_night_start`/`time_day_start`) oder echter Sonnenauf-/-untergang aus
  `sun_latitude`/`sun_longitude` mit Offsets. In `auto` plant der Kalender nur
  die Proben, er entscheidet nicht – das ist der Grund, warum Keller und
  künstlich beleuchtete Räume weiter funktionieren.
* **Umschaltung über Board-Skript**: `daynight.switch_cmd` (Default `daynight`)
  wird als `<cmd> day|night` per `fork()+execlp()` aufgerufen (nie `system()`).
  Das Skript treibt IR-Cut und IR-LEDs und meldet den Modus über
  `POST /control {"image":{"running_mode":0|1}}` zurück.
* **Diagnose**: `daynight.diagnose_thresholds` warnt nach drei
  aufeinanderfolgenden fehlgeschlagenen Proben einmal pro Daemon-Lauf, wenn nie
  eine Probe Tag bestätigt hat; `daynight.trace_path` schreibt (nur auf tmpfs!)
  eine CSV-Entscheidungsspur; `daynight.history_s` (Default 0 = aus, max. 48 h)
  hält eine RAM-Serie, die die WebUI-Tuningkurve über
  `GET /control?dn_history=1` abruft – auch für Stunden, in denen kein Tab offen
  war. Zusätzlich meldet der Status `isp_desync`, wenn der entschiedene Modus
  und der ISP-Rücklesewert dauerhaft auseinanderlaufen.
* Manueller Betrieb jederzeit über `daynight.enabled = 0` plus
  `image.running_mode` bzw. den WebUI-Umschalter.

## Bewegungserkennung

* **Raster aus IMP_IVS-Move-ROIs** über den Frame von `motion.monitor_stream`;
  `motion.cols` × `motion.rows` Zellen melden Bewegung einzeln – die WebUI legt
  daraus ein Live-Overlay über die Vorschau.
* `motion.enabled`, `motion.sensitivity` (0–255, auf die SDK-Stufen 0–4
  abgebildet), `motion.hold_ms` (Nachleuchten einer Zelle, Default 800),
  `motion.skip_frames` (jeder N-te Frame, Default 5),
  `motion.cooldown_ms` (Default 5000). `hold_ms` und `skip_frames` sind
  POST-bar und werden live über die gebündelte IVS-Raster-Neusynchronisation
  am Ende des Requests angewandt; `cooldown_ms` und `on_motion` sind bewusst
  nur in der Konfigurationsdatei setzbar.
* **Hook** `motion.on_motion` (thingino-Default `/usr/sbin/timps-motion`) wird
  ratenbegrenzt und ohne Shell per `posix_spawn()` gestartet – bewusst nur aus
  der Konfigurationsdatei setzbar, nicht über HTTP.
* Zellbudget des SDK (`IMP_IVS_MOVE_MAX_ROI_CNT`, meist 52) wird gemeldet als
  `caps.motion.max_cells`; der Status enthält `active[]`, `last_ms` und ein
  `stalled`-Flag, wenn IVS keine Ergebnisse mehr liefert.
* **Push statt Polling**: jede Rasteränderung geht verlustfrei über
  `GET /events?stream=motion` (32-Einträge-Ring mit Cursor pro Verbindung).

## Aufnahme & Timelapse

* **Lokale Aufnahme** (`USE_RECORD`) als fragmentiertes MP4 auf SD-Karte:
  `record.enabled`, `record.channel`, `record.mode = continuous | motion`,
  `record.dir`, `record.name` (strftime), `record.segment_s`,
  `record.pre_roll_s`, `record.post_roll_s`, `record.min_free_mb`,
  `record.audio`. Ablage unter `<dir>/<hostname>/records/<name>.mp4`,
  Segmentwechsel immer am Keyframe.
* **Sauberer Schnitt statt Decoder-Müll nach einem Queue-Überlauf**
  (*seit v1.9.19, unveröffentlicht*): Ist die Aufnahme-Warteschlange
  übergelaufen, beziehen sich alle noch anstehenden P-Frames auf Bilder, die
  nicht in der Datei stehen. Bis v1.9.18 wurden sie trotzdem geschrieben (bis zu
  eine ganze GOP sichtbarer Rest nach der Lücke); jetzt pausiert das Segment und
  setzt erst am nächsten Keyframe wieder ein – dieselbe Logik wie
  `http.adaptive_drop` bei den fMP4-Clients. Die Lücke wird dadurch etwas
  länger, das Bild danach aber sauber; im `motion`-Modus wird zusätzlich der
  Pre-Roll-Ring verworfen. Audio pausiert mit. Ein Überlauf, der nur **Audio**
  getroffen hat, löst nichts davon aus – die GOP ist intakt.
* **Ehrliches Speicherplatz-Management**: alte Segmente werden geprunt, bis
  `min_free_mb` frei sind – ist der Wert für die Karte unerreichbar, verweigert
  der Recorder die Aufnahme und begründet das in `record.last_error`, statt alle
  vorhandenen Aufnahmen zu löschen und das Ziel trotzdem zu verfehlen.
  Status enthält außerdem `write_errors`, `manual_off` und
  `motion_gate_enabled`.
* **Manuelles Start/Stopp** über `POST /control {"record":{"active":1|0}}`
  (der Aufnahmeknopf der WebUI) sowie **Einzelclip auf Zuruf**
  (`{"record":{"clip":"/tmp/x.mp4","seconds":6}}`) – das ist der Weg, über den
  send2/Telegram Bewegungsvideos holen.
* **Timelapse** (`USE_TIMELAPSE`): periodische JPEGs nach
  `<dir>/<hostname>/timelapses/<name>.jpg`, `timelapse.enabled/channel/dir/
  name/interval_s/keep_days`, atomar geschrieben (tmp + rename), ältere Bilder
  werden nach `keep_days` geprunt. Alle Keys live über `/control`.

## Audio

* **Aufnahme-Codecs** `audio.codec = aac | pcmu | pcma | opus | none`
  (AAC über libfaac, Opus über `USE_STREAM_OPUS`, RFC 7587).
  `audio.samplerate` (8000–96000, Default 16000), `audio.bitrate`,
  `audio.channels` (1 mono, 2 = simuliertes Dual-Mono-Stereo, nur AAC).
* **Zweiter G.711-Encode für WebRTC** – `audio.codec2 = pcmu` kodiert dasselbe
  PCM ein zweites Mal auf eine eigene Hub-Quelle, damit eine WHEP-Sitzung Ton
  bekommt, während RTSP, fMP4-Vorschau und Aufnahme beim AAC-Primärstream
  bleiben.
* **Mikrofonkette live**: `audio.volume`, `audio.gain`, `audio.alc_gain`
  (analoge PGA, nur T21/T31/C100), `audio.mute`. Nur beim nächsten Start
  wirksam (trotz POST-Barkeit): `high_pass` (Default an), `agc`,
  `agc_target_dbfs`, `agc_compression_db`, `ns` – libimp würde sonst seinen
  eigenen Record-Thread unter den Füßen wegziehen.
* **Lautsprecher nativ über IMP_AO** – timps besitzt das Ausgabegerät selbst,
  kein externer `/bin/iac`. `audio.spk_enabled`, `audio.spk_volume`,
  `audio.spk_gain`.
* **ONVIF-Audio-Backchannel** (`USE_BACKCHANNEL`): ein RTSP-Client spricht auf
  den Kameralautsprecher. `audio.backchannel`, `audio.backchannel_codec =
  pcmu | pcma | aac` (AAC braucht `USE_BC_AAC` / libhelix-aac),
  `audio.backchannel_rate`. Genau ein Sprecher zur Zeit.
* **Browser-Gegensprechen `/talk`** (`USE_BC_WS`): RFC-6455-WebSocket,
  Mikrofon des Browsers → Lautsprecher der Kamera, G.711 µ-law in 20-ms-Frames.
  `audio.talk_ws` ist dreiwertig: 0 aus, 1 nur über TLS (`wss://`),
  2 auch über `ws://`.
* **Echokompensation** `audio.aec = 1` (greift beim nächsten AO-Open, nur
  sinnvoll wenn Mikrofon und Lautsprecher gleichzeitig laufen).
* **System-Sound-Queue** (`USE_PLAY`): FIFO unter `/run/timps/audio_out` mit
  demselben `PLAY`/`STOP`-Protokoll, das prudynt/raptors `/usr/sbin/play`
  spricht – WLAN-Captive-Portal-Ansagen, Update-Gong, Home-Assistant-TTS.
  WAV, rohes PCM16 und G.711 immer; Ogg-Opus mit `USE_PLAY_OPUS`.
  Abspielen über `POST /control {"speaker":{"play":"chime_1.wav"}}`, die
  verfügbaren Dateien listet `caps.play.sounds`.

## OSD & Privatsphäre

* **Eigener TrueType-Rasterizer** (kein libschrift), bis zu 8 Overlays
  **pro Videostream**: Keys `osd<S>.<N>.<feld>` (z. B. `osd0.0.text`,
  `osd1.2.x`), alte `osd<N>.<feld>`-Keys gelten weiter für alle Streams.
* Item-Typen `text` und `logo`; Felder `enabled`, `text`, `x`, `y`,
  `font_size` (absolute Pixel), `color`/`transparency` (0xAARRGGBB),
  `outline` + `outline_color` (Lesbarkeitskontur), `logo`/`logo_w`/`logo_h`,
  optionaler Font pro Item.
* **Positionierung**: 0 = zentriert auf der Achse, positiv = Abstand von
  links/oben, negativ = von rechts/unten.
* **Platzhalter**: strftime-Tokens plus `{hostname} {ip} {mac} {fps} {bitrate}
  {uptime}`, stream-bezogen `{fpsN}`/`{bitrateN}`, sowie beliebige eigene
  `{name}` aus `osd.vars_file` (z. B. `/tmp/timps_osd.vars`), die ein Skript
  schreiben kann.
* Globale OSD-Keys: `osd.enabled`, `osd.monitor_stream`, `osd.font_path`,
  `osd.supersample` (Kantenglättung 1–4, Default 2), `osd.hinting`
  (geometrisches Autohinting für kleine Schrift, Laufzeit-Default 1, wirkt aber
  nur, wenn `BR2_PACKAGE_TIMPS_OSD_HINTING` den Pass einkompiliert hat).
  Beide sind per `/control` setzbar, greifen aber erst nach einem Neustart.
* **Privatsphäre-Masken**: bis zu 4 Rechtecke pro Stream
  (`privacy<S>.<N>.{enabled,x,y,w,h,color}`), als IMP-OSD-Cover-Regionen,
  live verschiebbar. Verfügbarkeit meldet `caps.privacy`.

## Netzwerk & Sicherheit

* **RTSP-Digest-Auth** (`rtsp.user`/`rtsp.pass`) und **HTTP Digest/Basic**
  (`http.user`/`http.pass`, fällt auf die RTSP-Zugangsdaten zurück) – eigene
  MD5-Implementierung, kein OpenSSL.
* **Wichtig zum Auslieferungszustand**: solange *beide* Benutzernamen leer sind,
  sind die Medienendpunkte (RTSP-Stream, `/snapshot.jpg`, `/stream.mjpeg`,
  `/stream.mp4`) ohne Authentifizierung im Netz erreichbar. `/control` und
  `/events` sind davon ausgenommen: ohne konfigurierte Zugangsdaten antworten
  sie jedem Nicht-Loopback-Client mit `403`. Das thingino-Paket liefert
  `thingino`/`thingino` als Voreinstellung mit.
* **Token-Auth** für `/control`, `/events` und die HTTP-Medienendpunkte:
  ein zufälliges **Per-Boot-Token** in `http.token_file` (Default
  `/run/timps.token`, Modus 0640, damit die lokale WebUI es lesen kann) und
  optional ein dauerhaftes Geheimnis `http.token` für Fernautomatisierung.
  Übergabe als `X-Timps-Token:`-Header oder `?token=` (für `<img>`, `<video>`
  und `EventSource`, die keine Header setzen können). Ein Token schaltet nie
  RTSP frei.
* **TLS (optional, `USE_TLS`, mbedTLS)** – `http.https` ist dreiwertig:
  0 nur Klartext, **1 = beide Schemata auf demselben Port** (Klassifikation
  per erstem Byte der Verbindung: 0x16 = TLS-Handshake), 2 = nur TLS
  (Klartext bekommt `426 Upgrade Required`). Dazu `http.tls_cert`/`http.tls_key`
  und optional RTSPS über `rtsp.tls` / `rtsp.tls_port` (Default 322).
  Fehlt das Zertifikat, wird der Listener gar nicht erst gebunden – nie
  stillschweigend auf Klartext heruntergestuft. Session-Tickets (RFC 5077)
  sind an, der Ticket-Key rotiert alle 12 h.
* **Zertifikat-Teilung mit der WebUI**: `S95timps` verlinkt auf Images mit WebUI
  auf deren `uhttpd`-Zertifikat, damit `:443` und `:8880` dasselbe Zertifikat
  zeigen – sonst müsste der Browser Selbstsigniertes pro Port einzeln
  vertrauen, was für `fetch()`-Unterressourcen (Safari!) gar nicht möglich ist.
* **CORS**: Medienendpunkte senden `Access-Control-Allow-Origin: *`;
  `/control` und `/events` spiegeln stattdessen den `Origin:`-Header
  (mit `Vary: Origin`, ohne Credentials).
* Konfigurierbare Client-Obergrenzen werden gemeldet, statt dass ein Client sie
  durch Ausprobieren findet: `caps.rtsp_max_clients`, `caps.http_max_clients`,
  `events.max_clients`.

## API & Steuerung

* **`GET /control`** – ein JSON-Dokument mit dem kompletten aktuellen Zustand:
  `version` (git-describe des Binaries), `caps`, `image`, `audio`, `sensor`,
  `video`, `osd`/`osd0`/`osd1`, `privacy`, `daynight`, `motion`, `encoder`,
  `queue_drops`, `record`, `timelapse`, `srt`, `tls`, `last_errors`.
* **`caps`-Objekt** – was *dieses* Binary auf *diesem* SoC kann:
  `caps.image`/`caps.audio` (Liste der wirklich unterstützten Regler),
  `caps.osd`, `caps.restart`, `caps.video_live`, `caps.motion`,
  `caps.privacy`, `caps.rotation`, `caps.record`, `caps.timelapse`,
  `caps.backchannel` (inkl. `talk_ws`), `caps.play`, `caps.webrtc`.
  Eine UI soll daran verzweigen, nicht an Versionsnummern.
* **`POST /control`** – verschachteltes JSON pro Sektion; jeder erkannte Wert
  wird live angewandt (wo möglich) **und** in `/etc/timps.conf` zurückgeschrieben
  (nur die geänderten Keys, atomar per tmp+rename). Die Antwort ist immer
  gleich geformt: `accepted`, `changed`, `rejected`, `not_persisted`,
  `deferred`/`deferred_keys`, `ignored` und `applied` (Echo der *effektiven*,
  also ggf. geklemmten Werte). Fehlerfälle sind unterscheidbar:
  `400 not_json`, `422 unknown_fields` (Key kennt dieser Build nicht),
  `409 values_rejected` (Keys richtig, Werte falsch).
* **Unter-Endpunkte**: `GET /control?fields=1` (Inventar aller POST-baren
  Felder, direkt aus denselben Tabellen erzeugt, die der POST benutzt),
  `?stats=1` (schlanker Zahlensatz für die Stats-Karte statt des 8-KB-Dokuments),
  `?dn_history=1[&last=N|&since=S][&max=N]` (Day/Night-Serie mit Cursor-Paging).
* **`GET /events`** – Server-Sent-Events statt Polling:
  `?stream=motion,daynight,stats,config`. `motion` und `daynight` senden beim
  Verbinden einmal den vollen Zustand, `stats` tickt alle `events.stats_ms`
  (Default 2000 ms), `config` meldet Änderungen anderer Clients.
  Keepalive-Ping alle ~12 s, `events.max_clients` (Default 8).
* **Alias-Toleranz**: alte Key-Schreibweisen (`video0.mode`, `record.segment`,
  `daynight.total_gain_day_threshold`, `osd0.0.stroke`, das flache
  `{"force_mode":"night"}`) werden weiterhin angenommen und auf den
  kanonischen Namen abgebildet.
* **Gehaltene Fehlerlage**: `last_errors` in `GET /control` hält den letzten
  WARN/ERROR je Modul fest – der 64-KB-Syslog-Ring der Kamera recycelt sich
  sonst, bevor jemand nachsieht.

## WebUI-Integration (thingino)

* timps ist ein **Plugin der thingino-WebUI**: `timps.webui.json` landet unter
  `/var/www/a/plugins/`, die Seiten und Skripte werden beim Firmware-Bau in den
  WebUI-Baum installiert (ca. 45 Dateien).
* Eigene bzw. ersetzte Seiten: `preview.html` (nativer MSE/fMP4-Player mit
  WebRTC-Option, Bewegungs-Overlay, Talk-Knopf, Steuerleiste),
  `streamer-main/-config/-image/-sensor/-substream/-osd0/-osd1.html`,
  `config-audio.html`, `config-motion.html`, `config-photosensing.html`,
  `config-privacy.html`, `tool-record.html`, `tool-timelapse.html`,
  `tool-sensor-data.html` (Day/Night-Tuningkurve), `recordings.html`,
  `timelapse-player.html`.
* Die Vorschauen holen `/stream.mjpeg?chn=N` und `/snapshot.jpg?chn=N` direkt
  vom timps-Port (mit `?token=`), ohne Proxy-CGI.
* Brücken-CGIs unter `/var/www/x/` (`timps-imp.cgi`, `timps-token.cgi`,
  `json-heartbeat.cgi`, `json-recordings.cgi`, `json-timelapse.cgi` …);
  ONVIF-Snapshot-URLs (`/onvif/image.cgi`, `image1.cgi`) zeigen auf timps.
* **WebRTC-Fähigkeitsprüfung im Browser**: `preview.html` baut die Antwort nach,
  die timps senden würde, und testet sie gegen eine Wegwerf-`RTCPeerConnection` –
  kann der Browser sie nicht (Firefox akzeptiert nur Baseline), erscheint die
  Option deaktiviert mit Begründung statt eines schwarzen Players.

## PTZ & Motoren

timps selbst steuert keine Motoren – Pan/Tilt kommt im thingino-Ökosystem aus
`thingino-motors`. Was das timps-Paket beisteuert, ist die **Oberfläche**:
Joystick-Overlay in der Vorschau, `config-motors.html` und die zugehörigen
CGIs werden aus dem timps-Paket installiert, wenn `thingino-motors` und timps
gemeinsam gewählt sind, und auf Builds mit `BR2_PACKAGE_THINGINO_MOTORS_WS`
wird die WebSocket-Variante (statt CGI-Polling) aktiviert.

## Plattform-Unterstützung

* **SoCs**: T10, T20, T21, T23, T30, T31, T40, T41, C100.
* **Zwei Encoder-API-Generationen**: neu (`IMP_Encoder_SetDefaultParam`) auf
  T31/C100/T40/T41, klassisch (manuelle `IMPEncoderChnAttr`-Struktur) auf
  T10/T20/T21/T23/T30. Das erklärt die unterschiedliche Bedeutung von
  `videoN.bitrate`: klassisch eine harte Obergrenze, neu ein Zielwert.
* **ISP-Tuning-API** weicht nur auf T40/T41 ab (`ISP_NEW_TUNING_API`), was dort
  einige `image.*`-Regler entfallen lässt – nachzulesen in `caps.image`.
* **Audio**: `IMP_AI_SetAlcGain` (`audio.alc_gain`) nur auf T21/T31/C100.
* **Rotation**: Hardware-I2D auf T40/T41, FrameSource-Rotate auf T31,
  Software auf T23, gar nicht auf T10/T20/T21/T30/C100.
* **Host-Simulation** ohne Hardware: `make sim` baut `timpsd-sim`, das Dateien
  statt des ISP einspeist (`sim.video0`, `sim.audio`, …) – praktisch für
  WebUI- und API-Arbeit am Schreibtisch.

## Betrieb, Logging & Diagnose

* **Gestufte Protokollierung**: `general.loglevel` (0 err … 3 debug) plus
  `general.debug_modules` – eine Namensliste (MAIN, CONFIG, CTRL, DAYNIGHT,
  HAL_ING, HTTP, HUB, MOTION, OSD, REC, RTSP, SRT, TL, TLS, AAC …), die einzelne
  Module auf DEBUG hebt, ohne alles andere laut zu machen. Live über `/control`
  umschaltbar, was bei Fehlersuche entscheidend ist. `general.syslog = 1`
  schreibt zusätzlich nach `logread`.
* **Version aus der Ferne prüfbar**: `GET /control` liefert `version` als
  git-describe-String des laufenden Binaries – genau der Test, der „Firmware
  geflasht" von „neue Binärdatei läuft wirklich" trennt.
* **QA-Skript** `scripts/timps-qa.sh --cam <ip> [--profile quick|standard|load|
  soak|drift]`: automatisierter Durchlauf über Streams, Ratenkontrolle,
  Bewegung, Audio, Aufnahme, Day/Night und Feldinventar, inklusive
  HTML-Report (`scripts/qa_html_report.py`).
* **Day/Night-Werkzeuge**: `dn-isp-probe.sh`, `dn-isp-report.py`,
  `dn-replay.py` und ein Regressionskorpus aus 30 aufgezeichneten
  Realszenarien unter `scripts/dn-scenarios/`.
* **Optionale Diagnose-Skripte im Image** (`BR2_PACKAGE_TIMPS_DIAG_TOOLS`,
  Default aus): `timps-selftest`, `timps-logcat-ship`, `timps-dn-isp-log`.
* Host-Tests für heikle Bausteine: `make test-srtp`, `test-stun`, `test-fmp4`,
  `test-config`, `test-fanqueue`, `test-hub-pool`, `test-hub-idr`.

## Build-Optionen (thingino / Buildroot)

Paket: `package/timps` – Auswahl über *Streamer Packages → Streamer → timps*
(`BR2_PACKAGE_THINGINO_STREAMER_TIMPS`). Jede Kconfig-Option wird auf ein
`USE_*`-Makro abgebildet.

| Kconfig | Schalter | Default | Wirkung |
| --- | --- | --- | --- |
| `BR2_PACKAGE_TIMPS_FAAC` | `USE_FAAC` | y | AAC-Audio über libfaac |
| `BR2_PACKAGE_TIMPS_CONTROL` | `USE_CONTROL` | y | `/control` + `/events` (~15 KB) |
| `BR2_PACKAGE_TIMPS_DAYNIGHT` | `USE_DAYNIGHT` | y | native Tag/Nacht-Erkennung |
| `BR2_PACKAGE_TIMPS_RECORD` | `USE_RECORD` | y | SD-Aufnahme (~11 KB) |
| `BR2_PACKAGE_TIMPS_TIMELAPSE` | `USE_TIMELAPSE` | y | Timelapse (~4 KB) |
| `BR2_PACKAGE_TIMPS_TLS` | `USE_TLS` | y | HTTPS + RTSPS (mbedTLS) |
| `BR2_PACKAGE_TIMPS_WEBRTC` | `USE_WEBRTC` | y | WHEP-Endpunkt (braucht TLS + CONTROL) |
| `BR2_PACKAGE_TIMPS_SRT` | `USE_SRT` | n | MPEG-TS über SRT (libsrt) |
| `BR2_PACKAGE_TIMPS_STREAM_OPUS` | `USE_STREAM_OPUS` | n | Opus als `audio.codec` (libopus) |
| `BR2_PACKAGE_TIMPS_BACKCHANNEL` | `USE_BACKCHANNEL` | n | ONVIF-Backchannel |
| `BR2_PACKAGE_TIMPS_BC_AAC` | `USE_BC_AAC` | n | AAC auf dem Backchannel (libhelix-aac) |
| `BR2_PACKAGE_TIMPS_BC_WS` | `USE_BC_WS` | y auf TLS-Builds | `/talk` für Browser (~8 KB) |
| `BR2_PACKAGE_TIMPS_PLAY` | `USE_PLAY` | n | System-Sound-Queue + `/usr/sbin/play` |
| `BR2_PACKAGE_TIMPS_PLAY_OPUS` | `USE_PLAY_OPUS` | n | Ogg-Opus im Play-Queue (opusfile) |
| `BR2_PACKAGE_TIMPS_ROTATE` | `USE_ROTATE` | n | Bildrotation |
| `BR2_PACKAGE_TIMPS_SW_ROTATE` | `USE_SW_ROTATE` | n | Software-90/270 auf T23 |
| `BR2_PACKAGE_TIMPS_OSD_HINTING` | `USE_OSD_HINTING` | n | OSD-Autohinting (~2 KB) |

Dazu die Sammeloption **„Audio backchannel preset"** (MANUAL / FULL / MINIMAL),
die Backchannel, Play-Queue und das Format der thingino-Sounds konsistent
zusammenschaltet – FULL passt auf T31-Boards, MINIMAL auch auf ein T20 mit
~5 MB Rootfs.

Direktes Cross-Kompilieren ohne Firmware-Baum ist ebenfalls möglich:
`make PLATFORM=T31 CROSS_COMPILE=mipsel-linux-` (IMP-Header kommen als
Git-Submodul aus `gtxaspec/ingenic-headers`).

## Wo steht was

Diese Datei ist eine Feature-Übersicht, keine Nachschlagetabelle. Für Details:

* **`docs/ai/reference.md`** – die englische Gesamtübersicht für den
  Support-Assistenten: Build-Gating, Endpunkte, Authentifizierung, `/control`,
  Tag/Nacht, Fehlersuche.
* **`docs/ai/config-keys.md`** – jeder Konfigurationsschlüssel einzeln, gegen
  `src/config.c` geprüft: Default, Wertebereich, live oder Neustart. Bei
  Abweichungen zwischen dieser Datei und `config-keys.md` gilt
  `config-keys.md`; bei Abweichungen zum Quelltext gilt der Quelltext.
* **`docs/ai/troubleshooting.md`** – Log-Wörterbuch, Status- und Fehlercodes,
  typische Fehlkonfigurationen und eine Liste plausibel klingender, aber
  falscher Antworten.
