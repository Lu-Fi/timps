# timps — Stream-Kompatibilitäts-Review (RTSP/RTP/SDP/ONVIF/HTTP)

**Datum:** 2026-07-31 · **Reviewer:** Claude (Fable 5) · **Stand:** `main` @ `d4ed99f`

**Scope:** Spec-Konformität und Interoperabilität mit der Breite realer NVR-/VMS-/
Player-Software (Frigate/go2rtc, Home Assistant, Synology Surveillance Station,
Blue Iris, iSpy/Agent DVR, Milestone, generische ONVIF-Profile-S-Clients, VLC/
ffmpeg/gstreamer, Mobile-Apps). **Kein** Security-/Memory-Safety-Review — dafür
existieren `timps-review-fable5.md` und `timps-review-opus48-2026-07-31.md`.

Geprüfte Dateien: `src/rtsp/rtsp.c`, `src/rtsp/rtp.c`, `src/rtsp/backchannel.c`,
`src/codec/nal.c`, `src/codec/vparam.c`, `src/codec/aac.c`, `src/mp4/httpd.c`,
`src/auth.c`, `src/net.c`, `docs/reviews/S96onvif_discovery` (ONVIF-Integration
der Firmware-Seite).

**Einordnung der Schweregrade:**
- **BRICHT CLIENTS** — Spec-Verstoß, der bestimmte reale Software nachweislich
  oder mit hoher Wahrscheinlichkeit scheitern lässt.
- **RISIKO** — Abweichung, die unter realistischen Bedingungen (VPN, Stromausfall,
  bestimmte Client-Klassen) zuschlägt.
- **KOSMETISCH** — Spec-Abweichung ohne erwartbare Praxisfolgen; nur der
  Vollständigkeit halber gelistet, bewusst NICHT dramatisiert.

---

## Zusammenfassung (priorisiert)

| # | Befund | Schwere | Betroffene Clients |
|---|--------|---------|--------------------|
| A1 | Fehlerantworten ohne `CSeq` | BRICHT CLIENTS | live555-basierte NVR-Stacks, strikte Parser |
| A2 | Requests während PLAY werden teils **gar nicht** beantwortet (PAUSE, SET_PARAMETER, …) | BRICHT CLIENTS | VLC (Pause-Button), NVRs mit SET_PARAMETER-Keepalive |
| A3 | Kein RTSP-Session-Timeout ⇒ UDP-Sessions leaken für immer; 8-Client-Cap läuft voll | RISIKO (hoch) | alle UDP-Transport-Clients nach Stromausfall/Netz-Partition (ffmpeg-Default!) |
| B1 | RTP_MTU=1400 hart kodiert ⇒ IP-Fragmentierung über VPN/WireGuard | RISIKO | jede Remote-Anbindung über Tunnel (WG-MTU 1420), „läuft im LAN, bricht remote" |
| D1 | ONVIF: Backchannel-URI wird nicht exportiert (Script-Kommentar veraltet) | RISIKO | HA-ONVIF-Zweiweg-Audio, ONVIF-Talkback generell |
| D2 | ONVIF: Port-Fallback 8554 im Script vs. Daemon-Default 554 | RISIKO | jede ONVIF-Auto-Discovery bei Conf ohne explizites `rtsp.port` |
| A4 | Multicast-/kaputte Transport-Header ⇒ kein `461`, stattdessen UDP an Port 0 | RISIKO (niedrig) | Milestone/Axis-Station bei Multicast-Konfiguration |
| B2 | RTCP SR ohne SDES/CNAME (kein Compound-Packet) | KOSMETISCH–RISIKO | strikte RTCP-Stacks, ONVIF-Testtools |
| C1 | DESCRIBE kann SDP **ohne** `sprop-parameter-sets`/fmtp liefern (Cold-Start >500 ms) | RISIKO (niedrig) | HW-Decoder-NVRs, Clients ohne Inband-SPS-Recovery |
| E1 | HTTP nur Basic-Auth (kein Digest) | RISIKO (niedrig) | Digest-only-Snapshot-Clients (ONVIF Core verlangt Digest) |
| D3 | ONVIF-Profile: Auflösung/Codec werden für timps nie synchronisiert | RISIKO (niedrig) | Synology/Milestone-Auto-Konfiguration |
| E2 | HEAD-Requests bekommen vollen Body; ein Verbindungs-Pool für Streams+Snapshots | KOSMETISCH | Snapshot-Poller unter Last (503 „busy") |
| A5–A9, B3–B4, C2–C4 | diverse Kleinigkeiten | KOSMETISCH | — |

**Explizit geprüft und in Ordnung** (Abschnitt F): FU-A/Single-NAL-Packetization
nach RFC 6184, RFC-3640-AAC inkl. AU-Header auf jedem Fragment, statische PTs für
G.711, Digest+Basic auf RTSP, `RTP-Info` mit exakten seq/rtptime, RTCP-SR vom
korrekten Quellport, ONVIF-Backchannel-SDP (`a=sendonly`, `551`+`Unsupported`),
Marker-Bits, TCP-Interleaved-Framing inkl. Client-`$`-Frames im Kontrollkanal.

---

## A — RTSP-Protokollebene (`src/rtsp/rtsp.c`)

### A1 — Fehlerantworten ohne `CSeq`-Echo. **BRICHT CLIENTS**

Alle Fehlerpfade senden rohe Statuszeilen ohne `CSeq` (RFC 2326 §12.17 verlangt
CSeq in **jeder** Response):

- `rtsp.c:408` — DESCRIBE-404: `"RTSP/1.0 404 Not Found\r\n\r\n"`
- `rtsp.c:436` — SETUP-404
- `rtsp.c:535` — SETUP-500
- `rtsp.c:556` — PLAY-455 (auch ohne das dort sinnvolle `Allow:`)
- `rtsp.c:573` — 405 Method Not Allowed
- `rtsp.c:765` — 503 (Hub voll)

Clients, die Responses über CSeq ihren Requests zuordnen (alles, was von live555
abstammt — diverse NVR-Firmwares, Happytime-basierte Tools, einige Mobile-SDKs;
gstreamer loggt Warnungen), können die Antwort keinem Request zuordnen und laufen
in ihren Timeout statt sauber zu fehlern. Da mehrere dieser Pfade zusätzlich die
Verbindung schließen, wird aus einem sauberen „404, nächste URL probieren" ein
„Verbindungsabbruch nach Timeout" — genau der Unterschied zwischen einem NVR, der
die zweite Stream-URL automatisch probiert, und einem, der die Kamera als „offline"
markiert.

_Fix:_ zentrale `send_err(s, cseq, code, reason, extra)`-Hilfe; jede Response
(auch 4xx/5xx) trägt `CSeq` und — wo eine Session existiert — `Session`.
Aufwand: klein, rein mechanisch.

### A2 — Requests während PLAY werden teils gar nicht beantwortet. **BRICHT CLIENTS**

`stream_loop()` (`rtsp.c:732-744`) parst im PLAY-Zustand nur `TEARDOWN`,
`GET_PARAMETER` und `OPTIONS`. **Jeder andere Request wird kommentarlos aus dem
Puffer entfernt — es geht keinerlei Response raus.** RTSP ist strikt
request/response; eine verschluckte Anfrage ist schlimmer als jede Fehlerantwort.

Konkret betroffen:
- **VLC**: der Pause-Button sendet `PAUSE`. Vor PLAY gäbe es wenigstens ein 405
  (ohne CSeq, siehe A1); während PLAY kommt **nichts** — VLC hängt bis zu seinem
  Timeout und reißt dann die ganze Session ab.
- **SET_PARAMETER-Keepalive**: einige Clients (u. a. ODM-Varianten, ältere
  ONVIF-NVRs, manche Axis-kompatible Stacks) pingen mit `SET_PARAMETER` statt
  `GET_PARAMETER`/`OPTIONS`, teils ohne vorher `Public:` auszuwerten. Keine
  Antwort ⇒ Client wertet die Session als tot ⇒ zyklische Reconnects, die wie
  „Stream instabil" aussehen, obwohl die Medien fließen.
- Re-`SETUP` (Track nachträglich hinzufügen) und `DESCRIBE` mid-session: selten,
  aber gleiches Muster.

_Fix:_ im PLAY-Parser einen Default-Zweig: unbekannte/nicht unterstützte Methoden
mit `501 Not Implemented` (oder `455`) + `CSeq` + `Session` beantworten;
`SET_PARAMETER` idealerweise wie `GET_PARAMETER` als Keepalive-200 behandeln
(Body darf ignoriert werden). `PAUSE` für eine Live-Quelle korrekt mit `501`
beantworten — das ist spec-konform (RFC 2326 §10.6: PAUSE ist für Live optional)
und lässt VLC sofort weiterlaufen statt zu hängen.
Zusätzlich: `Content-Length` eines `GET_PARAMETER`/`SET_PARAMETER`-Bodys
konsumieren — heute würde ein Body als Beginn des nächsten Requests
fehlinterpretiert und desynct den Parser (`rtsp.c:734`, `strstr("\r\n\r\n")`
ohne Body-Behandlung; gleicher Blindfleck im Pre-PLAY-Parser
`client_thread():792`).

### A3 — Kein Session-Timeout: verwaiste UDP-Sessions leaken für immer. **RISIKO (hoch)**

Der SETUP-Response-Header ist `Session: <id>` **ohne** `;timeout=` — per RFC 2326
gilt dann der Default von 60 s. timps setzt aber serverseitig **gar kein**
Timeout durch:

- Im PLAY-Zustand wird der Kontrollkanal nur mit `MSG_DONTWAIT` gepollt
  (`rtsp.c:710`) — das `SO_RCVTIMEO` aus `net_set_timeouts()` greift dort nie.
- `net.c` setzt kein `SO_KEEPALIVE`; eine TCP-Verbindung zu einem hart
  ausgefallenen Host (Stromausfall, Netz-Partition, NVR-Kernel-Panic) bleibt
  ESTABLISHED, ohne dass je ein Byte fließt.
- Bei **UDP-Transport** schlägt auch der Medienversand nie fehl: `sendto()` auf
  einem unverbundenen UDP-Socket bekommt keine ICMP-Fehler zugestellt
  (`sink_send()`, `rtsp.c:117`). RTCP-RRs werden nie gelesen (der RTCP-Socket
  `udp[1]` wird gebunden, aber nirgends `recvfrom()`t), können also auch nicht
  als Liveness-Signal dienen.

Ergebnis: Jede UDP-Session, deren Client ohne TEARDOWN verschwindet, läuft
**unbegrenzt** weiter — belegt einen von `RTSP_MAX_CLIENTS` (=8) Slots, hält den
Encoder wach und schaufelt RTP ins Leere. TCP-interleaved heilt sich über das
`SO_SNDTIMEO` (15 s) selbst; UDP nicht. Und UDP ist kein Exot: **ffmpeg (und
damit iSpy/Agent DVR, Shinobi, viele Wrapper) probiert per Default UDP zuerst**;
VLC ebenso. Ein NVR, der ein paarmal hart neu startet (oder eine flackernde
WLAN-Strecke), füllt die 8 Slots — danach bekommt jeder neue Client 503, bis
timps neu gestartet wird. Frigate/go2rtc ist per `rtsp_transport: tcp`-Konvention
meist auf TCP und daher im eigenen Test nie aufgefallen.

_Fix:_
1. `Session: %s;timeout=60` senden (kostenlos, informativ korrekt),
2. in `stream_loop()` einen `last_rtsp_activity`-Zeitstempel führen (jeder
   Kontrollkanal-Request und jedes Client-`$`-Frame zählt) und die Session nach
   ~2× timeout ohne Aktivität **bei UDP-Transport** abbauen. Optional RTCP-RRs
   vom `udp[1]`-Socket als Aktivität mitzählen (nicht parsen, Empfang reicht).
   TCP-interleaved kann von der Prüfung ausgenommen bleiben (Send-Timeout deckt
   es ab).

### A4 — Multicast/defekte Transport-Header: kein `461`, stattdessen RTP an Port 0. **RISIKO (niedrig)**

`SETUP` kennt nur zwei Fälle: `strstr(tr,"TCP")` ⇒ interleaved, sonst UDP-Unicast
(`rtsp.c:490-544`). Ein `Transport: RTP/AVP;multicast` (Milestone, Axis Camera
Station und einige Enterprise-VMS bieten Multicast als Option an) fällt in den
Unicast-Zweig, findet kein `client_port=`, antwortet **200 OK** mit
`client_port=0-0` und schickt RTP per `sendto()` an Port 0. Der Client sieht
einen „erfolgreichen" SETUP und wundert sich über schwarzes Bild — die am
schwersten zu diagnostizierende Fehlerklasse.

_Fix:_ Transport-Angebote, die weder `interleaved` noch `client_port=` (Unicast)
enthalten, mit `461 Unsupported Transport` beantworten. Ebenso `cp==0` nach
Parsen als Fehler werten. Mehrere Transport-Angebote in einem Header
(kommasepariert, Client bietet z. B. `multicast,unicast`) wenigstens per
`strstr("client_port=")` über den Gesamtheader abfangen — das tut der Code heute
zufällig schon richtig.

### A5 — `Session`-Header des Clients wird nie validiert. **KOSMETISCH**

Jeder Request mit beliebiger (oder fehlender) Session-ID wird akzeptiert; es gibt
genau eine Session pro TCP-Verbindung. Für alle bekannten Clients irrelevant
(die Session-ID läuft ohnehin nur über diese eine Verbindung), spec-seitig
(§12.37: `454 Session Not Found`) aber unsauber. Kein Handlungsbedarf, solange
keine Session-Wiederaufnahme über neue Verbindungen geplant ist.

### A6 — Kein `Content-Base` in der DESCRIBE-Response; kein Session-Level `a=control:*`. **KOSMETISCH**

Clients lösen die relativen `a=control:trackID=N` dann gegen die Request-URL auf
(RFC 2326 §C.1.1 erlaubt genau das als Fallback), und `SETUP`-Pfade werden hier
ohnehin per `strstr("trackID=1")` gematcht — funktioniert mit ffmpeg, live555,
gstreamer, VLC. `Content-Base` + `a=control:*` hinzuzufügen ist trotzdem billig
und nimmt exotischen Parsern (ältere Dahua/Hik-Bridges, die die Basis strikt aus
`Content-Base` lesen) die letzte Stolperkante. prudynt/live555 senden beides.

### A7 — TCP-interleaved ohne `interleaved=`-Angabe: Kanalkollision. **KOSMETISCH**

Fehlt im SETUP der `interleaved=`-Parameter, defaulten beide Tracks auf 0-1
(`rtsp.c:492-496`) — Video- und Audio-Track landen auf denselben Kanälen. Real
senden alle bekannten Clients den Parameter; falls nicht, wäre der Fix, den
nächsten freien Kanal (0-1, dann 2-3) zu vergeben.

### A8 — `GET_PARAMETER` vor SETUP liefert `Session: ` mit Leerwert. **KOSMETISCH**

`rtsp.c:564-567` baut den Header auch, wenn `s->session[0]==0`. Ein leerer
Header-Wert ist formal zulässig, sieht aber nach Bug aus; Header nur senden,
wenn eine Session existiert.

### A9 — `find_video_by_path()`-Fallback auf Stream 0 für jede unbekannte URL. **Bewusste Lenienz — ok, dokumentieren**

Ein Tippfehler in der URL liefert klaglos den ersten aktivierten Stream statt
404 (`rtsp.c:218-219`). Das ist kompatibilitätsfreundlich (viele Kameras tun
das), kann aber Debugging-Verwirrung stiften („warum sehe ich den Main- statt
Substream?"). Kein Fix nötig, nur als Verhalten in README/docs festhalten.

---

## B — RTP/RTCP (`src/rtsp/rtp.c`)

### B1 — `RTP_MTU 1400` hart kodiert ⇒ IP-Fragmentierung über Tunnel. **RISIKO**

`rtp.c:11`. Single-NAL-Pakete werden bis 1400 Bytes RTP-Gesamtgröße gebaut, FU-A
entsprechend. Dazu kommen 8 B UDP + 20 B IP = **1428-Byte-IP-Pakete**. Das passt
in Ethernet-1500, aber nicht in:

- WireGuard (MTU 1420) — der Standard-Remote-Zugriff auf Heimkameras,
- OpenVPN/tap-Setups (~1400-1460),
- IPv6-Minimum-Pfade und manche LTE/CGN-Strecken,
- PPPoE (1492) geht sich knapp aus.

Die Folge ist IP-Fragmentierung; bei UDP-RTP über verlustbehaftete Strecken
(WLAN + Tunnel) potenziert ein verlorenes Fragment den Paketverlust, und manche
Firewalls droppen Fragmente komplett — das klassische „läuft im LAN perfekt,
remote nur graue Artefakte". Der TCP-Pfad ist nicht betroffen (Segmentierung
macht der Kernel). Genau die Client-Population, die der Maintainer nicht testet
(Remote-VLC/HA über VPN), trifft es.

_Fix:_ `rtsp.mtu`-Config-Key (Default z. B. 1400 fürs LAN, dokumentierter
Hinweis auf 1200 für VPN); ein Wert um 1200 kostet <2 % Overhead und ist die
übliche Wahl (WebRTC nutzt 1200 aus demselben Grund). Nebenbefund: der
G.711-Fragmentierer (`rtp.c:305`) budgetiert `chunk` gegen `RTP_MTU` **ohne**
die 12 Header-Bytes — mit 320-Byte-Frames unerreichbar, aber beim Umbau gleich
mitziehen.

### B2 — RTCP-SR ist kein Compound-Packet (SDES/CNAME fehlt). **KOSMETISCH bis RISIKO (niedrig)**

`rtp_maybe_sr()` (`rtp.c:346-356`) sendet einen nackten 28-Byte-SR. RFC 3550
§6.1 verlangt: *jedes* RTCP-Paket ist ein Compound-Packet, das ein SDES mit
CNAME enthält. Praktisch tolerieren ffmpeg/VLC/gstreamer/go2rtc den Solo-SR
(damit auch Frigate/HA), aber:

- ONVIF-Konformität-Tools und strikte VMS-RTCP-Stacks (Genetec-Klasse) flaggen es,
- Clients, die CNAME zur A/V-Track-Korrelation über SSRC-Wechsel hinweg nutzen,
  verlieren dieses Signal (bei timps' stabilen SSRCs folgenlos).

Dass der SR überhaupt regelmäßig (1 s) und vom korrekten Quellport (RTCP-Socket,
`rtsp.c:113-117`) kommt, ist bereits besser als bei vielen Kamera-Firmwares —
der häufige „Stream unhealthy trotz Bild"-Fall ist damit abgedeckt.

_Fix:_ an den SR ein minimales SDES anhängen (SSRC + CNAME `timps@<ip>`,
gepaddet): ~20 Zeilen, ein `memcpy` mehr.

### B3 — RTCP-RRs werden nie gelesen; kein BYE bei Teardown. **KOSMETISCH**

Der RTCP-Empfangssocket wird nie ausgelesen (Kernel-Buffer läuft voll und droppt
— harmlos). Kein adaptives Verhalten nötig; aber siehe A3: RR-Empfang wäre das
natürliche Liveness-Signal für UDP-Sessions. Ein RTCP-BYE beim TEARDOWN wäre
höflich, kein Client braucht es zwingend.

### B4 — Video-RTP-Timestamps aus Publish-Wallclock. **Hinweis, kein Fehler**

`pts_to_ts()` leitet Video-Timestamps aus dem Publish-Zeitpunkt ab — sie tragen
also Scheduling-Jitter (Audio ist seit dem Sample-Counter-Umbau davon befreit).
Player mit engem Jitter-Buffer glätten das; auffällig würde es erst bei stark
schwankender Encoder-Latenz. Sauberer wäre ein Frame-Counter × Frame-Dauer bei
festem FPS, aber es gibt keinen Client-Report, der das erfordert. Beobachten,
nicht umbauen.

---

## C — SDP-Generierung (`gen_sdp()`, `vparam_sdp_fmtp()`)

### C1 — DESCRIBE kann SDP ohne `a=fmtp` (sprop/profile-level-id) liefern. **RISIKO (niedrig)**

`handle_request()` wartet max. 50×10 ms = 500 ms auf Parameter-Sets
(`rtsp.c:412`); sind sie dann nicht da (Encoder-Cold-Start auf langsamen SoCs,
gerade umgeschalteter Codec), fehlt die komplette fmtp-Zeile — kein
`sprop-parameter-sets`, kein `profile-level-id`, kein `packetization-mode`
(RFC 6184 §8.1: alles optionale Parameter, das SDP bleibt *formal* gültig).

ffmpeg/VLC/gstreamer recovern über Inband-SPS/PPS (der Ingenic-Encoder wiederholt
sie vor jedem IDR, und `hub_request_idr()` wird bei PLAY erneut getriggert).
Aber Hardware-Decoder-NVRs und einige Mobile-SDKs initialisieren den Decoder
strikt aus dem SDP und zeigen sonst dauerhaft schwarz — und weil es nur beim
Cold-Start passiert, sieht es wie ein sporadischer Client-Bug aus.

_Fix:_ Wartefenster auf ~2 s erhöhen ist die 1-Zeilen-Variante; sauberer:
solange keine Params da sind, `DESCRIBE` mit `503` + `Retry-After: 1`
beantworten statt ein degradiertes SDP zu liefern. Die 500 ms-Schleife läuft
ohnehin blockierend im Client-Thread, längeres Warten kostet nichts Neues.

### C2 — `o=- 0 0 IN IP4 <ip>` — statische Origin-Session-ID. **KOSMETISCH**

RFC 4566 §5.2 will ein global eindeutiges (sess-id, sess-version)-Paar (NTP-
Timestamp-Empfehlung). Kein bekannter RTSP-Client wertet es aus (relevant wäre
es bei SDP-Re-Announcements, die timps nie macht). Nice-to-have: Boot-Timestamp
einsetzen.

### C3 — Kein `a=range:npt=now-`, kein `b=AS` fürs Video, kein `a=framerate`/`a=framesize`. **KOSMETISCH**

Alles optionale Attribute. Auflösung/FPS holen sich moderne Clients aus dem SPS
(korrekt geliefert). Einzig ältere ONVIF-NVR-Auto-Konfiguratoren lesen gern
`a=framerate`; wer D3 (unten) fixt, deckt diese Klasse besser über die
ONVIF-Profile ab als über SDP-Attribute. Bewusst kein Alarm: das als
„Kompatibilitätslücke" zu verkaufen wäre übertrieben.

### C4 — Audio-fmtp/ASC: korrekt. **OK**

`mpeg4-generic`-fmtp trägt alle RFC-3640-Pflichtparameter für AAC-hbr
(`sizelength=13;indexlength=3;indexdeltalength=3;config=<ASC>`), der 2-Byte-ASC
aus `aac_asc()` ist korrekt aufgebaut (AAC-LC, Rate-Index, Kanäle), die
rtpmap-Clockrate entspricht der tatsächlichen HAL-Samplerate, und die Payload-
Types im SDP sind exakt die, die `stream_loop()` dann sendet (96/97 dynamisch,
0/8 statisch für PCMU/PCMA — statische PTs korrekt ohne fmtp-Zwang). Keine
Abweichung gefunden.

---

## D — ONVIF-Integration (`docs/reviews/S96onvif_discovery` + onvif_simple_server)

timps implementiert selbst kein ONVIF; Discovery/Device/Media-Services kommen
von thinginos `wsd_simple_server`/`onvif_simple_server`, gefüttert über das
`S96onvif_discovery`-Init-Script. Die Kompatibilität des ONVIF-*Pfads* hängt
also an diesem Script — das (per Opus-Review N5) versehentlich unter
`docs/reviews/` liegt, aber inhaltlich die maßgebliche Integrationsquelle ist.

### D1 — Backchannel wird für timps nicht nach `onvif.json` exportiert. **RISIKO**

`configure_timps_onvif_profiles()` (Script Z. 196-210) setzt Stream- und
Snapshot-URLs, aber **kein** `audio.backchannel.uri` — mit dem inzwischen
falschen Kommentar *„No RTSP audio backchannel: timps is one-way"*. Der Daemon
hat den Backchannel längst (`src/rtsp/backchannel.c`, `Require:`-Handling, SDP
`a=sendonly`, 551-Fallback — alles ONVIF-Streaming-Spec-konform, §5.3).

Folge: Clients, die Zweiweg-Audio über ONVIF **entdecken** (Home Assistant
ONVIF-Integration, ONVIF Device Manager, Profile-T-Talkback in Surveillance
Station), sehen keine AudioOutput-Konfiguration und bieten die Funktion nie an.
Nur Clients mit handkonfiguriertem `Require:`-Header (go2rtc mit
`#backchannel=1`) erreichen sie — exakt das Maintainer-Setup. Der Raptor-Zweig
desselben Scripts (Z. 121) setzt die URI.

_Fix:_ analog Raptor `jct … set audio.backchannel.uri "rtsp://%s:<port>/<ep0>"`
setzen, wenn `audio.backchannel` in timps.conf aktiv ist (und der Build
USE_BACKCHANNEL hat — ggf. über einen `timps -V`/Feature-Probe prüfen).

### D2 — Port-Fallback-Mismatch: Script nimmt 8554 an, Daemon-Default ist 554. **RISIKO**

`timps_rtsp_port()` (Script Z. 164-171) fällt ohne `rtsp.port`-Eintrag auf
**8554** zurück; `config.c:168` defaultet aber auf **554**. Solange die
ausgelieferte `timps.conf` den Port explizit setzt (das Beispiel tut es), ist
das latent — aber jede Conf, in der die Zeile fehlt/auskommentiert ist, liefert
via ONVIF `GetStreamUri` eine URL auf einen Port, auf dem nichts lauscht. Der
Nutzer sieht: RTSP-Direkteingabe funktioniert, ONVIF-Discovery „findet die
Kamera, aber kein Bild" — die im Auftrag explizit genannte Failure-Mode-Klasse.
Gleiches Muster droht bei `http.port` (Script-Fallback 8880 == Daemon-Default —
hier zufällig konsistent).

_Fix:_ Script-Fallback auf 554 angleichen (bzw. beide Defaults aus einer Quelle
ziehen).

### D3 — Profile-Metadaten (Auflösung/Codec/FPS) werden für timps nie gesetzt. **RISIKO (niedrig)**

Der timps-Zweig schreibt nur URLs; `profiles.streamN.width/height/type` bleiben,
was immer in `onvif.json` steht (der Raptor-Zweig liest sie wenigstens für sein
WebRTC-Profil aus). Ändert der Nutzer in timps.conf die Auflösung oder schaltet
video1 auf H.265, meldet `GetProfiles`/`GetVideoEncoderConfiguration` weiter die
alten Werte. NVRs, die daraus Empfangs-Pipelines oder Substream-Wahl ableiten
(Synology SS validiert Profilangaben, Milestone nutzt sie für die
Stream-Auswahl), treffen falsche Entscheidungen; H.265 als „H264" annonciert
kann die Aufnahme komplett verhindern.

_Fix:_ im Script `videoN.width/height/codec/fps` aus timps.conf lesen und in die
Profile spiegeln (Werte stehen flach in der Conf, `timps_conf_get` existiert
schon).

### D4 — Snapshot-URI setzt `videoN.jpeg=true` voraus. **KOSMETISCH**

`snapurl` zeigt auf `/snapshot.jpg?chn=N`; ist `videoN.jpeg` deaktiviert,
antwortet timps 404 (`jpeg_src_from_path()` gibt für explizites `chn=` bewusst
keinen Fallback). ONVIF-Clients zeigen dann „Snapshot nicht verfügbar" trotz
konfigurierter URI. Entweder im Script nur bei aktiviertem Kanal-JPEG schreiben,
oder in `jpeg_src_from_path()` für `chn=` auf den dedizierten `jpeg.*`-Kanal
zurückfallen statt hart 404.

---

## E — HTTP-Snapshot & -Media (`src/mp4/httpd.c`)

### E1 — Nur Basic-Auth, kein HTTP-Digest. **RISIKO (niedrig)**

`http_check_auth()` kennt ausschließlich Basic; die 401-Challenge bietet nur
`Basic` an. Preemptives Basic (was praktisch alle NVR-Snapshot-Poller senden —
Frigate, HA, Blue Iris) funktioniert, ebenso Challenge-Response-Basic. Aber die
ONVIF Core Spec (§5.12) verlangt von Geräten HTTP-Digest-Unterstützung, und
Clients, die aus Policy-Gründen kein Basic über Klartext-HTTP senden (Milestone
in Default-Härtung, einige Enterprise-Setups, ODM mit „secure only"), scheitern
am Snapshot — RTSP dagegen kann Digest (`auth_rtsp_digest`, korrekt inkl.
Server-Nonce-Bindung). Die MD5-Infrastruktur ist also schon da.

_Fix:_ `auth_rtsp_digest()` für HTTP wiederverwenden (Methode="GET", eigene
Nonce pro Verbindung, zweite `WWW-Authenticate: Digest`-Zeile in der
401-Antwort — RTSP-seitig existiert das Muster bereits in `rtsp_send_401`).

### E2 — HEAD (und jede andere Methode) wird wie GET behandelt. **KOSMETISCH**

`conn_thread()` parst die Methode, prüft sie aber außer für `OPTIONS`
(CORS-Preflight) und `GET`-vs-Rest bei `/control` nie: `HEAD /snapshot.jpg`
liefert den vollen JPEG-Body — RFC-7231-Verstoß. Monitoring-Probes und manche
NVR-„URL testen"-Buttons nutzen HEAD; sie funktionieren dadurch *scheinbar*,
messen aber falsche Größen/Zeiten. Fix: bei HEAD Header senden, Body
unterdrücken.

### E3 — Snapshot: Header/Format korrekt, Parallelität sauber, aber ein geteilter Pool. **OK mit Hinweis**

`Content-Type: image/jpeg` + exakte `Content-Length` + `Connection: close` —
korrekt, auch der Truncation-Guard. Jeder Snapshot-Request hat seine eigene
Hub-Subscription; parallele Snapshots sind untereinander und gegen Streams
unproblematisch (kein globaler Zustand). Zwei Praxis-Hinweise:

1. **Gemeinsamer `HTTP_MAX_CLIENTS`(=8)-Pool** für fMP4-Streams, MJPEG, SSE
   (`/events`) **und** Snapshots: ein paar offene Player-Tabs + HA-SSE + Frigate
   genügen, damit periodische Snapshot-Poller sporadisch 503 „busy" sehen. Da
   Snapshots kurzlebig sind, wäre ein kleines separates Kontingent (oder
   Vorrang) für `/snapshot.jpg` robuster.
2. **Cold-Start-Frame-Qualität:** Der zweistufige Aufweck-Mechanismus ist
   funktional korrekt, gibt aber den **ersten** Frame zurück, den der frisch
   gestartete Framesource/JPEG-Encoder liefert — vor AE/AWB-Settle kann das
   ein über-/unterbelichteter Frame sein (deckt sich mit dem heute gefundenen
   Daynight-Bug, wo genau so ein Transient-Frame den Gain-Messwert verfälschte).
   Für Clients, die Snapshots archivieren oder zur Bewegungs-Vorschau nutzen
   (HA-Benachrichtigungs-Thumbnails!), lohnt ein „Frame verwerfen, wenn die
   Quelle jünger als ~300 ms ist"-Guard bzw. das Zurückgeben des zweiten Frames
   im Cold-Start-Pfad.

### E4 — MJPEG-Endpoint. **OK**

Boundary-Handling (inkl. Übernahme einer vom Proxy vorgegebenen Boundary,
Zeichen-Sanitizing), `multipart/x-mixed-replace`, per-Part `Content-Length`,
Truncation-Abbruch statt Desync — konform; auch Chrome/HA-kompatibel.

---

## F — Explizit geprüft und konform (damit es nicht doppelt auditiert wird)

- **RFC 6184 (H.264):** `packetization-mode=1` annonciert; gesendet werden
  Single-NAL und FU-A mit korrekten S/E-Bits, NRI-Übernahme und Marker nur auf
  dem letzten Paket des AU. STAP-A wird nicht *gesendet* — das ist konform:
  Mode 1 verpflichtet nur den **Empfänger** zu STAP-A-Support, nicht den Sender.
  Der NAL-Iterator (nal.c) verkraftet 3- und 4-Byte-Startcodes und leere NALs;
  kein 64-NAL-Cap mehr.
- **RFC 7798 (H.265):** FU (Typ 49) mit korrektem 2-Byte-PayloadHdr +
  LayerId/TID-Rekonstruktion; SDP mit sprop-vps/sps/pps.
- **RFC 3640 (AAC):** AU-Header auf **jedem** Fragment mit der vollen AU-Größe
  (der frühere Verstoß ist gefixt und im Code dokumentiert); 13-Bit-Size-Guard.
- **G.711:** statische PTs 0/8, 8 kHz-Clock, Marker nur zum Talkspurt-Beginn.
- **RTP-Header/SSRC:** Version 2, zufällige SSRC/seq/ts_base aus /dev/urandom
  (RFC-3550-gerecht), Audio-Timeline sample-getrieben mit Gap-Resync — die
  Frigate/go2rtc-Reconnect-Klasse (SR/Timestamp-Drift) ist adressiert.
- **RTCP SR:** 1-s-Intervall, korrektes NTP↔RTP-Mapping aus demselben Anker wie
  die Media-Timestamps, Versand vom RTCP-Quellport (port-strikte Empfänger).
- **RTP-Info im PLAY-200:** exakte seq/rtptime beider Tracks — besser als viele
  Kamera-Originalfirmwares; live555-abgeleitete NVRs sind versorgt.
- **Auth:** RTSP Digest (RFC 2069-Stil, konsistent ohne qop-Angebot,
  Server-Nonce-Bindung, constant-time) + Basic-Fallback, doppelte
  WWW-Authenticate-Zeilen — kompatibel mit ffmpeg/VLC/NVR-Standardverhalten.
  OPTIONS ohne Auth beantwortet (wichtig für Discovery-Probes).
- **ONVIF-Backchannel-Protokollteil:** `Require: www.onvif.org/ver20/backchannel`
  erkannt, 551 + `Unsupported:` wenn nicht verfügbar, SDP-m-Line mit
  `a=sendonly` + eigenem trackID, RTP-Empfang UDP und TCP-interleaved, Absender-
  IP-Check — Streaming-Spec-konform (nur die *Discovery* fehlt, siehe D1).
- **TCP-Interleaved-Robustheit:** `$`-Framing in einem write (kein Torn-Header),
  Client-RTCP-Frames im Kontrollkanal werden längenkorrekt übersprungen, ohne
  nachfolgende Requests zu verschlucken; Abbruch bei Torn-Write.

---

## Priorisierte Empfehlung

1. **A1 + A2** (ein gemeinsamer Umbau der Response-Pfade): CSeq überall,
   Default-501 im PLAY-Parser, SET_PARAMETER-Keepalive, PAUSE→501,
   GET_PARAMETER-Body konsumieren. Kleiner, rein mechanischer Patch mit der
   größten Client-Breitenwirkung.
2. **A3**: `;timeout=60` annoncieren + UDP-Session-Reaper. Verhindert das
   schleichende Slot-Sterben bei ffmpeg/VLC-Klasse-Clients.
3. **D1 + D2 + D3** (Script-only, kein Daemon-Code): Backchannel-URI, 554-
   Fallback, Profil-Metadaten. Macht den ONVIF-Auto-Discovery-Pfad erst
   vollwertig.
4. **B1**: `rtsp.mtu`-Knopf (Default belassen, README-Absatz „Remote/VPN").
5. **C1** (Wartefenster/503), **B2** (SDES anhängen), **E1** (HTTP-Digest) nach
   Gelegenheit.
6. Rest ist kosmetisch und sollte keine Release-Priorität verdrängen.
