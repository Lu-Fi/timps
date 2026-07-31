# timps — Code-Review (Opus 4.8)

_Stand: 2026-07-31 · Quelle: `src/` (~11k LoC C) · Basis: HEAD `d4ed99f` + ein
uncommitteter Diff auf `src/daynight.c` (DN_REASSERT). Nichts wurde geändert —
reines Review._

Frischer Voll-Pass über `src/`. Schwerpunkt laut Auftrag: (1) Re-Verifikation der
drei offenen High-Findings H1/H2/H3 aus dem Fable-5-Review vom 2026-07-17, (2)
die vier seit `59eeb1b` gelandeten Commits (`04d10b0`, `8927583`, `761716f`,
`d4ed99f`) und (3) der noch nicht committete `DN_REASSERT_MS`-Mechanismus in
`daynight.c`. Datei- und Zeilenangaben beziehen sich auf den aktuellen Stand.

---

## 0. Re-Verifikation H1 / H2 / H3

**H1 — RTSP-fd-Leak bei wiederholtem SETUP → BEHOBEN.** `rtsp/rtsp.c:514-525`
Der UDP-Pfad schließt jetzt ein bereits gebundenes Paar für denselben Track,
bevor `net_bind_udp_pair()` neu bindet (`if (udp[0]>=0){close(udp[0]);udp[0]=-1;}`
… analog `udp[1]`). Der Kommentar referenziert H1 explizit. Da `net_bind_udp_pair()`
`udp[0]/udp[1]` nur bei Erfolg schreibt, bleibt der Zustand auch bei fehl-
geschlagenem Rebind konsistent (-1). Der Backchannel-SETUP-Pfad (`:470-471`) hat
denselben Schutz. **Vollständig gefixt, korrekt umgesetzt.**

**H2 — `calloc` ohne NULL-Check im OSD-Rasterizer → BEHOBEN.** `hal/msttf.c:431-451`
`cov=calloc((size_t)bw*bh,1)` wird jetzt geprüft; die gesamte Rasterisierung
läuft nur noch unter `if (cov && xint)` (`:451`), und `xint` selbst wird nur bei
`cov && maxint>0` alloziert. Bei OOM wird der Glyph sauber übersprungen (polys
werden weiter befreit, Rendering läuft mit dem nächsten Zeichen weiter). Als
Bonus wurde derselbe Codepfad um einen zweiten, separaten Bug bereinigt (M16:
der Scanline-Crossing-Puffer war ein festes `float[128]`, das bei >128 Kanten
pro Scanline stumm überlief und die Even-Odd-Paarung zerstörte — jetzt auf die
tatsächliche Punktzahl dimensioniert). **Vollständig gefixt.**

**H3 — `hub_publish` hält den Lock über die gesamte Veröffentlichung → BEHOBEN,
und besser als vom alten Review vorgeschlagen.** `hub.c:203-260`
`hub_publish()` snapshottet jetzt unter `s->lock` nur noch `subs[]` und
aktualisiert vparam/fps; das teure `pkt_new()` (malloc + bis ~1 MB memcpy) und
sämtliche `fanqueue_push()` laufen **nach** dem Unlock (`:245-252`). Zusätzlich:
- Bei `nsub_snap==0` wird malloc+copy **komplett übersprungen** (`:243`) — der
  Producer läuft während des ganzen Idle-Stop-Debounce-Fensters ohne Heap-Churn.
- Die durch das Push-außerhalb-des-Locks entstehende Use-after-free-Race gegen
  `hub_unsubscribe()` (Push hat `q` gesnapshottet, Caller zerstört `q` direkt
  nach Rückkehr) ist mit einem `g_pushing[]`/`g_push_done[]`-Handshake
  geschlossen (`:74-75`, `:192-196`, `:240`, `:254-259`): `hub_unsubscribe`
  wartet unter `s->lock` per Condvar, bis ein in-flight Push für die Quelle
  fertig ist. Ein Producer pro Source macht den einfachen Busy-Flag ausreichend.

Das ist eine substanzielle, korrekt abgesicherte Umsetzung — nicht nur der im
alten Review skizzierte „snapshot + push outside", sondern inklusive der daraus
folgenden Lebensdauer-Race. **Vollständig gefixt.**

> **Fazit: Alle drei offenen High-Findings sind behoben.** Keiner ist noch
> präsent. H3 wurde robuster gelöst als vom Fable-5-Review vorgeschlagen.

---

## Kritisch / High (neu)

**N1 — DN-Reassert: ein einzelner Schuss mit fixem 5-s-Offset kann exakt die
Race treffen, gegen die er gedacht ist.** `daynight.c:344-355, 475-479`
(`DN_REASSERT_MS 5000`, uncommitted)

Die Prämisse des Mechanismus (im Kommentar `:282-296`): der `SetISPRunningMode`
mitten in der ISP-internen gain-basierten Transition wird verworfen, ein Set im
*eingeschwungenen* Zustand latcht zuverlässig. Der Reassert feuert deshalb einmal
`DN_REASSERT_MS` (=5 s) nach jedem Switch.

Das Problem: 5 s ist ein fester Rateschätzer, die ISP-Transition ist aber
variabel lang. Bei einem **echten** Dämmerungsübergang ändert sich das Licht
langsam; die AE-/Gain-Rampe der ISP läuft dabei über zig Sekunden. Feuert der
Reassert bei +5 s, während die ISP noch mitten in ihrer gain-basierten Umschaltung
steckt, wird `SetISPRunningMode` **genauso verworfen** wie der ursprüngliche
Aufruf. Ausgerechnet der reale Dusk/Dawn-Fall (der eigentliche Auslöser) ist der,
in dem +5 s am ehesten *nicht* „steady state" ist. Ein Einzelschuss auf festem
Offset ist gegen eine variabel lange Transition nicht robust — der Mechanismus
kann in genau dem Szenario, für das er gebaut wurde, wirkungslos bleiben.

Verschärfend: die Bestätigung „auf Hardware verifiziert, dass er *feuert*" sagt
nichts über die *Wirksamkeit* aus (siehe N2 — es gibt gar kein Signal, ob der
Reassert-Set gelatcht hat). Die für morgen früh geplante Log-Prüfung wird „re-
asserted" sehen, egal ob die Race geschlagen wurde.

_Fix:_ Statt eines festen Einzelschusses die ISP zurücklesen und bounded
nachtreiben: `/proc/jz/isp/isp-m0` „Runing Mode" wird in `dn_brightness()`
(`:102-103`) ohnehin schon geparst — den Reassert so lange (mit Backoff, z. B.
+5 s / +15 s / +30 s, Deckel ~60 s) wiederholen, bis die ISP die angeforderte
Pipeline meldet, statt genau einmal blind zu feuern. Alternativ eine kurze Serie
von Reasserts. **Das ist die wichtigste Beobachtung am neuen Code.**

**N2 — DN-Reassert loggt bedingungslos Erfolg; es existiert kein Signal, ob der
Set tatsächlich gelatcht hat.** `daynight.c:349-352` + `hal/hal_ingenic.c:402-407,
441-447`

Der Reassert loggt `LOGI "re-asserted running_mode=%d after switch settle"`
unabhängig davon, ob die ISP den Modus übernommen hat. Das in `d4ed99f`
hinzugefügte rc-basierte `LOGW` in `isp_apply_image` hilft hier **nicht**: der
Fehlermodus, den dieses ganze Feature adressiert, ist laut Commit-Beschreibung
selbst ein Set, der `0` zurückgibt (stumm verworfen). rc==0 ⇒ kein WARN ⇒ der
Operator bekommt eine beruhigende Erfolgs-Logzeile, auch wenn die ISP den
Reassert stumm ignoriert hat. Damit ist die Log-Ausgabe **kein** Beweis, dass
die Race geschlagen wurde — genau die Aussage, die die morgige Prüfung treffen
soll, lässt sie nicht zu.

_Fix:_ Nach dem Reassert die tatsächliche ISP-Mode-Zeile aus `/proc/jz/isp/isp-m0`
zurücklesen und bei Abweichung von der angeforderten Pipeline ein WARN loggen —
das ist (da der API-Returncode hier unzuverlässig ist) das einzige verfügbare
Wirksamkeits-Signal. Erst damit wird die geplante Overnight-Prüfung aussagekräftig.

---

## Mittel

**N3 — DN-Reassert übergibt ein `val`, das `ing_control` für `image.*`-Keys
ignoriert; die Korrektheit hängt still an bereits-gesetztem `g_cfg`.**
`daynight.c:349` ↔ `hal/hal_ingenic.c:2131-2140`

`hub_control("image.running_mode","1"/"0")` übergibt einen Wert, aber `ing_control`
verwirft `val` für alle `image.*`-Keys komplett und wendet aus `g_cfg` an
(`isp_apply_image` liest `im->running_mode`; Kommentar `:2132-2133`:
„config_apply_kv runs before hub_control, so the HAL applies from the config").
Der Reassert treibt also **den aktuellen g_cfg-Wert**, nicht `reassert_mode`.

Im Normalfluss ist das korrekt: `dn_switch()` startet das Board-`color`-Script
synchron via `fork()`+`waitpid()` (`:158-170`), dessen `/control`-POST setzt
`g_cfg.image.running_mode`, bevor der Reassert bei +5 s überhaupt scharf wird.
Aber es ist ein latenter Trap:
- Postet das `color`-Script eines Boards `running_mode` **nicht** (toggelt nur
  ircut/IR-LED) oder schlägt der POST fehl, ist `g_cfg.image.running_mode` stale
  — der Reassert treibt den falschen/veralteten Modus und loggt trotzdem
  `reassert_mode` als sei es korrekt.
- Setzt der User zwischen Switch und +5 s-Reassert manuell einen anderen
  `running_mode` via `/control` (bei weiterhin aktivem Auto), wendet der Reassert
  den g_cfg-Wert (die manuelle Wahl) an, loggt aber `reassert_mode` — Log und
  Realität divergieren.

Das tote `val`-Argument verleitet künftige Maintainer zu der Annahme, der Reassert
steuere den angewandten Modus. Tut er nicht.

_Fix:_ Entweder den Reassert `g_cfg` schreiben lassen (via `config_apply_kv` unter
`config_str_lock`), damit `val` und angewandter Wert konsistent sind — oder die
`val`-Fiktion fallenlassen und dokumentieren, dass der Reassert den aktuellen
Config-Modus re-appliziert.

---

## Niedrig

**N4 — `play_write()` fehlt der `rn>0`-Guard, den `speaker_write_pcm()` bekommen
hat; bei Erst-OOM des Resample-Puffers `hal_ao_write(NULL,0)`.**
`rtsp/speaker.c:347-349` ↔ `:111-113`

Der Backchannel-Pfad wurde in `04d10b0` mit `if (rn > 0) hal_ao_write(...)`
abgesichert, der Play-Pfad nicht: `int rn = ms_resample(...); int rc =
hal_ao_write(g_rs, rn);`. Ist die allererste Audio-Operation ein Play und
scheitert `rs_fit()`s `realloc` (OOM), bleibt `g_rs==NULL`, `g_rs_cap==0`,
`rcap==0` ⇒ `ms_resample` gibt 0 zurück (`out_cap<=0`-Guard) ⇒
`hal_ao_write(NULL, 0)`. Ob das sicher ist, hängt an der HAL-`hal_ao_write`-
Implementierung. Sehr unwahrscheinlicher Pfad, aber eine grundlose Inkonsistenz.
_Fix:_ Denselben Guard spiegeln: `int rc = (rn>0) ? hal_ao_write(g_rs,rn) : 0;`.

**N5 — Fremdes Shell-Script versehentlich ins Repo committet.**
`docs/reviews/S96onvif_discovery` (337 Zeilen), eingebracht durch `761716f`
(„fix(daynight): ignore transient gain reading"). Ein `S96onvif_discovery`-
Init-Script hat in einem daynight-Commit unter `docs/reviews/` nichts zu suchen —
offensichtlich versehentlich mit-gestaged. Kein Laufzeit-Bug, reine Repo-Hygiene.
_Fix:_ Aus dem Baum entfernen (bzw. an den korrekten Package-Ort verschieben,
falls beabsichtigt).

---

## Status ausgewählter Alt-Findings (Stichprobe, nicht erschöpfend)

Neben H1/H2/H3 im Vorbeigehen mit-geprüft:

- **M9 (SRT ohne Client-Limit) — BEHOBEN.** `srt.c:34-35, 439-447`: `SRT_MAX_CLIENTS`
  (=8) mit `g_srt_clients`-Zähler (sync-builtins), analog RTSP/HTTP. Cap wird vor
  Thread-Start geprüft und abgelehnt.
- **M2 (`gen_sdp` snprintf-Akkumulation) — BEHOBEN.** `rtsp/rtsp.c:256-325`: jeder
  Schritt jetzt `if (n>=0 && n<(int)sizeof(body))`-guarded; der `size_t`-Underflow
  von `sizeof(body)-n` ist ausgeschlossen.
- **L2/L9 (snprintf-Truncation als Sendelänge) — teilweise BEHOBEN** in
  `send_resp` (`rtsp.c:334-346`, Clamp auf `sizeof hdr-1`).
- **H5 (Timelapse hält Hub-Subscription dauerhaft) — WEITERHIN PRÄSENT.**
  `timelapse.c:210-217`: der Thread subscribed einmal und pollt danach in einer
  200-ms-Schleife durchgehend (`fanqueue_pop(&q,200)`), solange aktiviert — der
  JPEG-Encoder/die Framesource laufen damit 24/7, obwohl pro `interval_s`
  (Minuten/Stunden) nur ein Frame geschrieben wird. Exakt der im Fable-5-Review
  beschriebene Zustand; nicht gefixt. Empfehlung unverändert: just-in-time
  sub-/unsubscribe pro Aufnahme wie `snapshot_jpg()`.

(Die übrigen M-/L-Findings des Fable-5-Reviews wurden in diesem Pass nicht
systematisch nachverifiziert.)

---

## Verifiziert in Ordnung (neu geprüfte Bereiche)

- **`04d10b0` (Resample-Scratch) korrekt.** `speaker.c:47-64` `rs_fit()` dimensioniert
  vor jedem Convert auf die tatsächliche Ausgabe (`nsamp*out_rate/src_rate+2`,
  gedeckelt `SPK_RS_MAX=8192*6`); `+2`-Slack ≥ `ms_resample`s `(int)(n*ratio)`-
  Floor, damit kein Clamp/Tail-Drop mehr. OOM-Fallback (alten Puffer behalten)
  sauber.
- **`8927583` (JPEG-Buffer-Cap) korrekt.** `hal_ingenic.c:1596-1625`: `MS_JPEG_BUF_MAX`
  auf 1 MB (= `MS_AU_BUF_MAX`); Pre-Sizing per `jneed`-Summe + `realloc`, Last-
  Resort-Overflow-Guard droppt statt Truncat zu publishen. Der AU-Pfad
  (`:964-1009`) ist identisch abgesichert und vermeidet die frühere IDR-Overflow-
  Stall-Spirale bewusst.
- **`d4ed99f` (Re-Drive auf No-Change-Re-POST) korrekt** für den `/control`-Pfad:
  `control.c:205-207` re-treibt nur `image.running_mode` bei No-Change (kein
  Flash-Persist), alle anderen Keys behalten den Skip; die rc-Surface in beiden
  HAL-Branches ist harmlos. (Wirksamkeits-Caveat gegen die eigentliche Race:
  siehe N1/N2.)
- **`frame.c`** (pkt_new/ref/unref): NULL-Checks vollständig, `__sync`-Refcount
  korrekt, kein Double-Free.
- **Allokations-Sweep** über `src/` (malloc/calloc/realloc): die restlichen
  Fundstellen (`msttf.c:398/516`, `imp_osd.c:142/178`, `osd_text.c:18`,
  `hal_ingenic.c:1135/1463/1501/1530`, `tls.c`, `mp4/httpd.c`, `rtsp.c:859/897`)
  sind alle NULL-geprüft.

---

## Empfohlene Reihenfolge

1. **N1 + N2** — der Kern des heute deployten DN-Reassert: Wirksamkeit gegen die
   reale Transition nicht garantiert **und** nicht beobachtbar. Vor dem
   Overnight-Check zumindest N2 (Rücklesen/WARN) einbauen, sonst ist die Prüfung
   nicht aussagekräftig; N1 (bounded Retry mit Rücklesen) macht den Mechanismus
   erst robust.
3. **N3** — `val`-Kopplung entschärfen/dokumentieren (verhindert stille Fehl-
   Reasserts bei abweichendem g_cfg).
4. **H5** (Alt-Finding, weiter offen) — Idle-Last/Flash-Wear.
5. **N4, N5** — Kleinkram/Hygiene.
