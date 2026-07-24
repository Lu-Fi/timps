# timps-Package — Fable-Review & Fixes

Adversariales Review des thingino-Buildroot-Pakets `package/timps/` (Integration, nicht
der timps-C-Quelle). Alle Fixes liegen **ausschließlich im timps-Package**; nichts in
Fremdpaketen. Ein Punkt (ONVIF-`select`) liegt in der Core-`Config.in` und ist bewusst
**nicht** angefasst → separater PR.

Verifikation: alle geänderten Shell-Scripts `sh -n` sauber, beide JS `node --check` sauber,
`timps.mk` mit balancierten `define/endef` (7/7) und `ifeq/endif` (14/14).

---

## Behoben (im Package)

### Build / `timps.mk`
- **Version-Binding (High).** `TIMPS_BUILD_CMDS` übergab kein `VERSION=`. Bei
  `SITE_METHOD`/`OVERRIDE_SRCDIR` fehlt `.git` → `git describe` → Binary meldete `0.1.0`
  statt `v1.3.9`. Fix: `VERSION=$(TIMPS_VERSION)` an `make` übergeben (die Makefile-Zeilen
  `-DMS_VERSION='"$(VERSION)"'` binden es ins Binary).
- **`/nfs`-Dev-Artefakt (High).** `cp … /nfs/timpsd` aus den Install-Cmds entfernt
  (nicht-hermetisch, upstream-Blocker).
- **Gate-Konsistenz (High).** preview- und send2-Hook zusätzlich auf `TIMPS_CONTROL`
  gegated (`WEBUI+DEV_IPCAM+CONTROL` bzw. `WEBUI+CONTROL`). Verhindert das kaputte
  `WEBUI=y, CONTROL=n`-Image (preview/send2 zeigten sonst auf ein wegkompiliertes
  `/control`).
- **`motion.on_motion` (High).** `timps-motion` wird jetzt **unbedingt** aus
  `TIMPS_INSTALL_TARGET_CMDS` installiert (aus dem send2-Hook entfernt), damit die in
  `timps.conf` fest hinterlegte `on_motion`-Kommandozeile immer auflöst → kein
  `system()`-Spam auf Nicht-send2-Images. Das Script no-op't sauber ohne send2-Infra.
- **`TIMPS_LICENSE = MIT`** ergänzt (legal-info). `TIMPS_LICENSE_FILES` bleibt offen, bis
  das Upstream-Repo eine LICENSE-Datei hat (im Kommentar vermerkt).
- **Photosensing-Seite (Low).** Bei `TIMPS_DAYNIGHT=y` werden jetzt auch
  `config-photosensing.html` + `.js` entfernt (vorher nur der Nav-Link) — kein per-URL
  erreichbares Waisen-Config-Panel mehr.

### Init / Config
- **`timps.conf`:** `srt.enabled = 0` (unauth. SRT-Listener nicht mehr per Default offen);
  Hinweis-Kommentar an `http.port`, dass Ändern/`https=1` die `/mjpeg`- und
  `/onvif/image.cgi`-Proxies bricht.
- **`S95timps`:** `status`-Case ergänzt; sauberes `stop`/`restart` mit Warten auf echten
  Prozess-Exit (IMP/rmem-Teardown-Race behoben); optionale TLS-Cert-Generierung bei
  `http.https=1`/`rtsp.tls=1`, wenn Cert/Key fehlen.
- **`generate-tls-certs.sh`:** akzeptiert jetzt `[cert] [key]`-Argumente (vorher feste
  Pfade ignoriert), passend zur S95-Integration.
- **`S96onvif_discovery`:** Config-Parser an den Daemon angeglichen (Inline-Kommentar nur
  bei führendem Whitespace strippen, Quotes entfernen) → korrekte ONVIF-Creds bei
  Passwörtern wie `p#ss` / gequoteten Werten; RTSP-Fallback-Port `8554 → 554`; Tempfile
  atomar via `mktemp` statt `mktemp -u` + Redirect (Symlink-TOCTOU als root).

### send2 / motion
- **`send2common`:** Schreib-Tempfiles (Snapshot/Video-Kopie) atomar via `mktemp` (statt
  `mktemp -u` + `cp`/`curl >`, Symlink-Follow als root), `/tmp`-gepinnt; das Clip-Ziel
  bleibt name-only (record_clip legt es mit `O_EXCL|O_NOFOLLOW` an), aber `/tmp`-gepinnt,
  damit es record_clips `/tmp/`-Präfix-Prüfung erfüllt.
- **`timps-motion`:** Snapshot-Ziel atomar (wie oben); Clip-Ziel name-only + `/tmp`-gepinnt;
  Warten auf die Kind-Prozesse via `wait` statt `kill -0`-Polling (kein Hang bei
  PID-Recycling).

### WebUI-Overlay
- **`json-recordings.cgi`:** Dateiname JSON-escaped (`\`/`"`) — bricht JSON nicht mehr und
  kein Inject; Symlink-Guard (`! -L`) auf Serve- und Delete-Pfad.
- **`recordings.js`:** Segmentname via `textContent` statt `innerHTML` (Stored-XSS über
  SD-Dateinamen behoben).
- **`privacy.js` „apply to both":** spiegelt beim Aktivieren nur **enabled** Regionen
  (vorher wurden disabled Slots als `enabled:0` auf den anderen Stream geschrieben und
  löschten dort eigenständige Masken); gespiegelte Box wird gegen `otherW/otherH`+`MIN`
  geclamped (kein 1-px-Überlauf, kein Unterschreiten der Mindestkante).

---

## Nicht behoben — bewusst

- **ONVIF wird für timps nie selektiert (High) — außerhalb des Pakets.** Die Top-Level
  `Config.in` listet im `select BR2_PACKAGE_THINGINO_ONVIF …` nur prudynt/raptor, nicht
  timps. Fix wäre `|| BR2_PACKAGE_TIMPS` dort — das ist Core, kein Package-File →
  **eigener PR**. Alternativ per Board `BR2_PACKAGE_THINGINO_ONVIF=y` in der defconfig.
- **`/mjpeg`- & `/onvif/image.cgi`-Proxy fest auf `:8880` (Medium).** Ein sauberer Fix
  (Boot-Zeit-Regenerierung) bräuchte einen httpd-Neustart, weil busybox httpd die Config
  nur beim Start liest und S95 nach httpd startet — mehr Risiko als Nutzen. Stattdessen im
  `timps.conf` dokumentiert.
- **`telegram-cam-register` (Low/Nit).** MQTT-Passwort als `-P`-Argv ist in `ps` sichtbar
  — busybox `mosquitto_pub` bietet keine bessere Option; Single-Root-Gerät. `json_escape`s
  `s/\n/…/`-Zeile ist bei zeilenweisem sed ein No-op, aber harmlos (Eingaben sind
  einzeilig; `"`/`\`-Escaping funktioniert). Kein Code-Churn.
- **`json-recordings.cgi` Symlink auf ext-SD (Low).** Final-Komponente jetzt per `! -L`
  geschützt; eine Kette über zwischenliegende Symlink-Verzeichnisse bleibt theoretisch
  (FAT — der Normalfall — kann keine Symlinks halten).

---

## Empfehlung für PRs
1. **timps-Package-PR** — alle „Behoben"-Änderungen oben (nur `package/timps/`).
2. **Core-PR (klein)** — `|| BR2_PACKAGE_TIMPS` im ONVIF-`select` der Top-Level `Config.in`.
3. **cameras-exp-PR** — die Streamer-Auswahl (`BR2_PACKAGE_THINGINO_STREAMER_TIMPS=y`) in
   den Board-defconfigs (gehört nicht in den Package-PR).
