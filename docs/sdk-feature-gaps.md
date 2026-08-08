# Ingenic IMP SDK feature-gap analysis

Datum: 2026-08-02, zuletzt aufgefrischt 2026-08-07. Von Fable erstellt: unabhängiger Abgleich aller `IMP_*`-Deklarationen in den vendorten SDK-Headern (`include/<SoC>/<Version>/<Sprache>/imp/*.h`, Submodul `gtxaspec/ingenic-headers`) gegen die tatsächlich in `src/` aufgerufenen IMP-Funktionen — über **alle 9 von timps unterstützten Plattformen** (T10, T20, T21, T23, T30, T31, T40, T41, C100), nicht nur T31.

## Refresh 2026-08-07 (wöchentlicher Re-Check, Stand `074e8f5` / v1.7.8)

Vollständige Neuprüfung gegen den aktuellen Code (Commits `f8a7b21`..`074e8f5`, d.h. alles seit dem 2026-08-02-Pass inkl. v1.7.6 Silent-Limbo-Sweep `265befb`, v1.7.7 Probe-Economy/Opus `fad4f40` und der C11-Data-Race-Härtung `10a192a` vom 2026-08-06).

**1) Die 3 ✅-Implementierungen sind intakt, keine Regression:**

- **#3 `IMP_IVS_SetParam` (`ef821a0`)** — `imp_motion_set_sensitivity()` (`src/hal/imp_motion.c` ~466-498) unverändert; der Commit-Deferral-Pfad (`g_motion_sense_pending` → `ing_control_commit`) in `src/hal/hal_ingenic.c` (~2566-2660) unverändert. Der v1.7.6-Stall-Watchdog (`motion_note_miss`, cycled `IMP_IVS_StopRecvPic`/`StartRecvPic`) ist orthogonal dazu und berührt den SetParam-Pfad nicht; beide neuen IVS-Aufrufe sind in allen 9 Header-Sets deklariert (verifiziert). Keine Plattform-Gates nötig (IVS universell) — korrekt.
- **#7a/#7b Encoder-Telemetrie (`98edd6a`)** — `hal_enc_stats()` (`hal_ingenic.c` ~3038) mit dem `IMPEncoderCHNStat`-Alias für die alte Generation (Zeile ~62) unverändert; die drei T31-only-Gates für `GetChnAveBitrate` (`#if defined(PLATFORM_T31)` bei ~996, ~1160, ~3056) alle noch vorhanden und korrekt.
- **#2 `IMP_AI_EnableAec` (`b5ed25d`)** — AEC-Block in `hal_ao_open()` (~3125-3140) samt `g_aec_on`-Teardown-Symmetrie unverändert; die v1.7.6/v1.7.7-Änderungen an `backchannel.c`/`speaker.c` haben ihn nicht angefasst.

Querprobe: `git diff f8a7b21..HEAD -- src/` enthält **kein einziges** geändertes/neues `PLATFORM_`-Conditional — die gesamte Plattform-Gating-Landschaft (27 Conditionals in `hal_ingenic.c`) ist seit dem 2026-08-02-Pass unangetastet.

**2) C11-Härtung (`10a192a`) ist über alle 9 SoCs plattform-sicher:**

- `audio.mute` ist jetzt `_Atomic int` (`config.h`), gelesen/geschrieben nur via `atomic_load`/`atomic_store` in `config.c` (F_ATOMIC) — **keine RMW-Operationen**, nur Load/Store auf `int`-Breite.
- Alle 9 Plattformen bauen mit derselben thingino-GCC-15-Toolchain (`build.sh`: `xburst1` für T10/T20/T21/T23/T30/T31/C100, `xburst2` für T40/T41; musl oder uClibc). `stdatomic.h` ist ein Compiler-Header (libc-unabhängig), und `_Atomic int`-Load/Store ist auf MIPS32 nativ lock-frei (sync + lw/sw, kein `-latomic` nötig). `-std=c11` war schon vorher der Makefile-Default. Einziger theoretischer Stolperstein: ein manuell via `CROSS_COMPILE` untergeschobener Uralt-Compiler (GCC < 4.9) — das war aber schon vor dieser Änderung eine unsupported Konfiguration.
- Der Daynight-Per-Poll-Snapshot (`ms_daynight_cfg`-Struct-Copy unter `config_str_lock()`) ist reines portables C ohne Atomics — plattform-neutral. Ebenso der OSD-`refresh_text()`-Umbau.

**3) Zurückgestellte Items — frische Feasibility-Einschätzung:**

| Item | Neubewertung 2026-08-07 |
|---|---|
| #1 Live-Encoder-Reconfig | **Unverändert Kandidat Nr. 1.** `video*.*`/`sensor.*` sind weiterhin explizit restart-only (`ing_control`: "persisted, applies on restart"). Die v1.7.7-Arbeit "persist clamped values" hat nur die Control-Persistenz berührt, keinen Konflikt geschaffen. Header-Verfügbarkeit (1a/1b/1c) unverändert. |
| #4a/#4b AE-Zonen | Unverändert verfügbar, unverändert zurückgestellt — kein neuer Code, der es leichter/schwerer macht. |
| #5 `GetChnEvalInfo` | **Leichter geworden:** das Telemetrie-Gerüst (`hal_enc_stat` + read-only `"encoder"`-Objekt in `GET /control`) existiert jetzt; T31-only-Eval-Felder wären eine kleine, gut gegateter Erweiterung statt eines neuen Subsystems. Aufgestiegen zum besten Kleinkandidaten. |
| #6 `SetAe_IT_MAX`/`SetAeMin` | **Relevanter geworden:** die v1.7.7-Probe-Economy (`fad4f40`) bekämpft Symptome des nächtlichen AGC-Hochlaufs (Backoff/Ratchet/Oszillation) in der Daynight-Logik — eine Belichtungs-Obergrenze würde eine Wurzelursache adressieren. Zudem liefert `src/isp_caps.h` inzwischen das etablierte Muster für saubere Caps-Gates (z.B. neues `ISP_HAS_AE_IT_MAX`). Aufwand bleibt mittel (drei API-Familien je Generation, s. Rangfolge #6). |
| #8 `SetbufshareChn` | Unverändert (T31/C100/T40/T41), unverändert zurückgestellt. |
| #9 `IMP_ISP_WDR_ENABLE` | Unverändert gespaltene API, unverändert zurückgestellt. |
| #12 `SetChnEntropyMode` | Unverändert T31-only, geringer Nutzen, zurückgestellt. |
| N1–N4 | Unverändert. N4-Doku-Nit (toter "T10/T20 3.9.0"-Kommentar zu `MOTION_MAX_CELLS=4`) besteht weiter (`config.c` ~312, `motion_caps.h`). |

Frischer Declared-vs-Called-Diff (Symbol-Extraktion pro Plattform-Header-Set neu generiert, T31: 260 deklarierte-aber-ungenutzte Symbole): **keine neuen Kandidaten-Familien** jenseits der bestehenden Findings — der seit 2026-08-02 hinzugekommene Code (Opus-RTSP-Streaming via libopus, Stall-Watchdogs, Probe-Economy) nutzt keine neuen IMP-APIs außer dem bereits etablierten `IMP_IVS_Stop/StartRecvPic`-Paar.

**4) Zwei-Generationen-Split weiterhin exakt korrekt:**

- `ENC_NEW_API` wird unverändert bei `hal_ingenic.c:55` aus `PLATFORM_T31||C100||T40||T41` abgeleitet (5 Nutzungsstellen: 910/1123/1854/1909/2718).
- `ISP_NEW_TUNING_API` kommt unverändert nur für T40/T41 aus `src/isp_caps.h:35-37`; die `IMPVI_*`-Sensor-Sonderpfade (`hal_ingenic.c` ~437-571, ~623-691, ~2995) sind unverändert.
- Die Makefile-Header-Pins (Zeilen ~54-81) sind byte-identisch zum 2026-08-02-Stand; die einzigen Makefile-Änderungen seit dem Pass betreffen `USE_STREAM_OPUS` (nicht IMP-bezogen).

Die Verfügbarkeitsmatrix unten gilt unverändert weiter.

## Umsetzungsstatus (Priorisierungs-Pass 2026-08-02)

Ein Feasibility-Pass hat aus dieser Liste **genau 3** Funde als jetzt sicher umsetzbar ausgewählt und implementiert (alles Übrige bleibt bewusst zurückgestellt):

- **#3 — `IMP_IVS_SetParam`/`GetParam` (Live-Sensitivity):** ✅ IMPLEMENTIERT in `ef821a0`. Reine `motion.sensitivity`-Änderungen aktualisieren das laufende IVS-`sense[]` in-place statt Stop/Destroy/Recreate; bei jedem Fehlschlag (Get/SetParam nonzero, Kanal nicht aktiv) Fallback auf den bestehenden vollen Rebuild.
- **#7a/#7b — Encoder-Telemetrie:** ✅ IMPLEMENTIERT in `98edd6a`. `IMP_Encoder_Query` (alle 9) als read-only `"encoder"`-Objekt in `GET /control`; auf T31 zusätzlich `IMP_Encoder_GetChnAveBitrate` (im Encode-Thread gecached, da die API einen frisch geholten Stream braucht — kein Standalone-Query). `GetChnEvalInfo` bleibt ungenutzt.
- **#2 — `IMP_AI_EnableAec` (Backchannel-AEC):** ✅ IMPLEMENTIERT in `b5ed25d`. Neuer Opt-in-Config-Key `audio.aec` (Default OFF); wird beim nächsten AO-Open aktiviert, wenn AI und AO beide laufen. Nur der einfache `IMP_AI_EnableAec`-Pfad, nicht die `EnableAecRefFrame`-Variante.

Der Rest der Liste (v.a. #1 Live-Encoder-Reconfig, #4 AE-Zonen, #5 restliche Telemetrie, #6 Belichtung, N1–N4) ist explizit NICHT Teil dieses Passes.

## Methodik

Header-Version pro Plattform, exakt wie in `Makefile` (Zeilen ~54-81) gepinnt:

| Plattform | Header-Pfad |
|---|---|
| T31 | `include/T31/1.1.6/en` |
| C100 | `include/C100/2.1.0/en` |
| T21 | `include/T21/1.0.33/zh` |
| T23 | `include/T23/1.3.0/en` |
| T30 | `include/T30/1.0.5/zh` |
| T40 | `include/T40/1.2.0/zh` |
| T41 | `include/T41/1.2.0/zh` |
| T20 | `include/T20/3.12.0/zh` |
| T10 | `include/T20/3.12.0/zh` (T10 nutzt dieselben Header wie T20) |

Wichtige strukturelle Erkenntnis: Die Flotte deckt zwei SDK-Generationen ab, und `hal_ingenic.c` unterscheidet bereits an mehreren Stellen danach:
- **Encoder**: `#if defined(PLATFORM_T31)||defined(PLATFORM_C100)||defined(PLATFORM_T40)||defined(PLATFORM_T41)` wählt den neuen `IMP_Encoder_SetDefaultParam`-Pfad; T20/T21/T23/T30 nutzen die ältere manuelle Attr-Konfiguration.
- **ISP**: `ISP_NEW_TUNING_API` (nur T40/T41) verlangt eine andere Funktionssignatur (`IMPVI_NUM` + Pointer-Argumente) für viele Tuning-Aufrufe.

Jede Feature-Idee unten muss also ggf. die Zwei-Generationen-Fallunterscheidung berücksichtigen, die im Code bereits etabliert ist.

## Verfügbarkeitsmatrix

| # | Finding | T31 | C100 | T21 | T23 | T30 | T40 | T41 | T20/T10 |
|---|---|---|---|---|---|---|---|---|---|
| 1a | `SetChnBitRate`/`SetChnGopLength`/`SetChnQpBounds` | ✓ | ✓ | – | – | – | ✓ | ✓ | – |
| 1b | `SetChnFrmRate` (live fps) | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ |
| 1c | `SetChnAttrRcMode` (live rc/bitrate, altes Struct) | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | – | ✓ |
| 2 | `IMP_AI_EnableAec` (+`GetFrameAndRef`) — ✅ `b5ed25d` | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ |
| 3 | `IMP_IVS_SetParam`/`GetParam` — ✅ `ef821a0` | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ |
| 4a | `SetAeWeight` (AE-Zonen-Gewichtung) | ✓ | ✓ | ✓ | ✓ | ✓ | ✓* | ✓* | ✓ |
| 4b | `AE_SetROI` | ✓ | ✓ | ✓ | ✓ | ✓ | – | – | ✓ |
| 5 | `SetFrontCrop`/`SetAutoZoom` (ePTZ) | ✓/✓ | ✓/✓ | –/– | ✓/✓ | –/– | –/✓ | –/✓ | –/– |
| 6 | `SetAe_IT_MAX`/`SetAeMin` (Belichtungs-Obergrenze) | ✓/✓ | ✓/✓ | –/✓ | ✓/✓ | –/– | –/– | –/– | –/– |
| 7a | `IMP_Encoder_Query` (Buffer/Stream-Stats) — ✅ `98edd6a` | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ |
| 7b | `GetChnAveBitrate`/`GetChnEvalInfo` — ✅ `98edd6a` (nur `GetChnAveBitrate`) | ✓ | – | – | – | – | – | – | – |
| 8 | `SetbufshareChn` | ✓ | ✓ | – | – | – | ✓ | ✓ | – |
| 9 | `IMP_ISP_WDR_ENABLE` | ✓ | ✓ | – | – | –† | ✓ | ✓ | –† |
| 10 | `IMP_IVS_CreateBaseMoveInterface` | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ |
| 11 | `IMP_FrameSource_SnapFrame` | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ |
| 12 | `SetChnEntropyMode` (CABAC-Toggle) | ✓ | – | – | – | – | – | – | – |

\* T40/T41 brauchen die neue Signatur (`IMPVI_NUM` als erstes Argument).
† T20/T30 haben stattdessen `SetWDRAttr` (andere API, gleiches Feature — siehe N2).

## Revidierte Rangfolge (flottengewichtet: T31 = primär, T20 = Wyze, T23 = zwei Testeinheiten)

1. **Live-Encoder-Reconfig** — auf allen 9 Plattformen umsetzbar über das bereits etablierte Zwei-Generationen-Muster: neue Generation → `SetChnBitRate`/`GopLength`/`QpBounds`; alte Generation + T41-Ausnahme → `SetChnAttrRcMode`; universell → `SetChnFrmRate`. Größter ungenutzter Hebel — z.B. Substream-Bitrate drosseln, wenn die SD-Karte hinterherhinkt, ohne aktive Clients zu kappen.
2. **AEC für den Backchannel** (`IMP_AI_EnableAec`) — alle 9 Plattformen, ein einziger Codepfad. Backchannel/Gegensprechanlage existiert bereits, aber ohne AEC hört die Gegenseite sich selbst über das Mikro zurück.
3. **`IMP_IVS_SetParam`** für Live-Sensitivity — alle 9 Plattformen; behebt zusätzlich den IVS-Rebuild-Churn aus dem Motion-Detection-Review (mittlerweile bereits über Batching gemildert, siehe `2d9a66d`).
4. **AE-Zonen-Gewichtung** (`SetAeWeight`, alle 9; `AE_SetROI` nur klassische API) — klassisches "Fenster im Hintergrund zu hell"-Problem lösbar. Braucht auf T40/T41 die neue Signatur.
5. **Encoder-Telemetrie** via `IMP_Encoder_Query` (alle 9) + T31-exklusive `GetChnEvalInfo`-Extras für die `/events`-Stats.
6. **Belichtungs-Obergrenze / Nacht-FPS-Schutz** — je Generation: `SetAe_IT_MAX` (T31/C100/T23), `SetIntegrationTime`+`SetAeStrategy` (T20/T21/T30), `SetAeScenceAttr` (T40/41).
7. **Encoder-Paket für die Budget-Kameras (N1)** — v.a. `SetChnROI` (Motion-Grid-gesteuerte Qualität) und `SetJpegeQl` auf T20/T23.
8. **ePTZ** (`SetFrontCrop`/`SetAutoZoom`) — T31/C100/T23 (+T40/41 Zoom); nicht auf dem Wyze verfügbar.
9. **WDR** — gespaltene API: `WDR_ENABLE` (T31/C100/T40/41) vs. `SetWDRAttr` (T20/T30); T21/T23 kein WDR.
10. **Privacy-Mosaik** (`IMP_OSD_SetMosaic`) — T23/T40/T41.
11. **`SetbufshareChn`** (rmem-Ersparnis) — T31/C100/T40/T41.
12. Universelle Kleinigkeiten: BaseMove-Interface, `SnapFrame`, `UpdateRgnAttrData`, `GetCPUInfo`, `FlushStream`; T31-exklusiv: `SetChnEntropyMode`.

## Neue plattformspezifische Funde (in der ersten, T31-fokussierten Analyse übersehen)

**N1 — Alte-Generation-Encoder-Features (T20/T10, T21, T23, T30 — also Wyze und die T23-Kameras), auf T31 nicht vorhanden:**
- `IMP_Encoder_SetChnROI` — Pro-Region-QP-Boost im Encoder. Besonders wertvoll hier: timps berechnet bereits ein Motion-Grid — aktive Zellen an `SetChnROI` zu füttern würde die Bitrate genau dort priorisieren, wo Bewegung ist.
- `IMP_Encoder_SetChnColor2Grey` — Encoder-seitiges Graustufen für Nachtmodus, entkoppelt von den ISP-Running-Mode-Latch-Problemen.
- `IMP_Encoder_InsertUserData` — SEI-Metadaten im Stream (Uhrzeit/Motion-Info für NVRs).
- `IMP_Encoder_SetJpegeQl` (T20/T21/T23/T30 + T41) — JPEG-Qualitätsregler; aktuell keine Qualitätskontrolle für Snapshot/Piggyback-JPEG auf diesen Chips.
- `SetSuperFrameCfg`, `SetChnDenoise`, `SetChnHSkip` (hierarchisches Frame-Skip für sehr schwache Verbindungen), `SetMbRC` — Bitraten-Robustheit für SD-Recording + SRT auf den Budget-Kameras.
- **Nur T23:** `IMP_Encoder_RequestGDR`/`SetGDRCfg` (Gradual Decoder Refresh — freundlicher als IDR-Bursts auf verlustbehafteten SRT-Links), `SetChnInitQP`, `Setframelossthd`, `SetDirectModeAttr` (Low-Latency FS→Encoder), plus eine vollständige `IMP_ISP_MultiCamera_*`-API (Nische).

**N2 — Alte-Generation-ISP-Features (T20/T10, T21, T30):** `SetIntegrationTime`/`GetIntegrationTime` + `SetAeStrategy` sind das Alte-Generation-Äquivalent zu Finding #6 — "Nacht-Belichtung deckeln" ist also auch auf dem Wyze umsetzbar, nur über einen anderen Aufruf. `SetSceneMode`/`SetColorfxMode` (Szenen-Presets, Farbeffekte inkl. Graustufen), `SetWDRAttr` (T20/T30-Pendant zu #9), feinere Rauschunterdrückung (`SetSinterDnsAttr`/`SetTemperDnsAttr`), `SaveAllParam`.

**N3 — T40/T41-Extras über T31 hinaus** (flotten-sekundär, aber "kostenlos" sobald diese Boards dazukommen):
- `IMP_OSD_SetMosaic` — **auch auf T23** — Verpixelung statt der aktuellen opaken Privacy-Rechtecke.
- `IMP_ISP_StartNightMode`, `SetFaceAe` (gesichtspriorisierte Belichtung), `SetAeScenceAttr`/`SetAeExpList`/`GetAeExprInfo` (löst #6 für T40/41).
- T41: `IMP_ISP_LDC_INIT/SetAttr` — Objektiv-Verzeichnungskorrektur für Weitwinkel.
- ISP-seitiges OSD, `WDR_OPEN`/`SetWdrOutputMode`, Audio `EnableAlgo`/`SetDigitalGain`, `SetChnQpBoundsPerFrame`/`SetChnMaxPictureSize` (granulare T41-RC).

**N4 — Kompatibilitäts-/Korrektheitsnotizen zu den alten Chips** (geprüft, größtenteils bereits korrekt behandelt): T10/T20-H.264-only-Guard existiert bereits (`hal_ingenic.c:849-853`); `GetSensorAttr`-Abwesenheit auf T10/T20/T21/T30 ist bereits abgefangen; die T23-`fcrop`-Header-Versions-Stolperfalle ist bereits durch einen Compile-Time-Tripwire abgesichert (siehe `Makefile`-Kommentar bei der T23-Header-Auswahl); `IMP_Encoder_GetFd` fehlt auf T20, ein Poll-fd-Stream-Loop wäre also nie flottenweit möglich. Ein Doku-Nit: Kommentare in `motion_caps.h`/`config.c` begründen den `MOTION_MAX_CELLS=4`-Fallback mit "T10/T20 3.9.0" — einer Header-Version, die das Makefile nie auswählt (es pinnt T20 3.12.0, das 52 Zellen hat); toter Zweig, harmlos.

## Header ohne nennenswerte Funde

`imp_common.h`, `imp_utils.h` (nur Typen/Makros), `imp_log.h` (eigenes Logging vorhanden), `imp_emu_framesource.h` (Test-Frame-Injection), `imp_decoder.h` (Video-/JPEG-Decode — eine Kamera braucht keinen Decode-Pfad), `imp_dmic.h` (digitales Mikrofon-Array — die Flotte nutzt analoges AI), ADEC/AENC-Software-Codec-Registrierung in `imp_audio.h` (timps kodiert selbst via faac/G.711). Auf T23/T40/T41 zusätzlich: `MultiCamera`- bzw. `ExternInject`/`DMIC`-Familien — nichts, was ein Single-Sensor-Streaming-Daemon braucht.

## Bezug zu bereits umgesetzten Fixes

Die separate Motion-Detection-Tiefenprüfung (M1–M3 + Sensitivity-Dedup) wurde bereits umgesetzt (Commits `0385aca`, `2d9a66d`, `833ab73`, `8b02945`). Finding #3 dieser Liste (`IMP_IVS_SetParam`) löst M2s verbleibendes Rebuild-Problem inzwischen an der Wurzel (Sensitivity ändern ohne vollen Stop/Destroy/Recreate-Zyklus) — ✅ umgesetzt in `ef821a0`, mit Fallback auf den Batching-Rebuild bei jedem SDK-Fehlschlag.
