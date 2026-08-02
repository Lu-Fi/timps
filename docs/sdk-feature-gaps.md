# Ingenic IMP SDK feature-gap analysis

Datum: 2026-08-02. Von Fable erstellt: unabhängiger Abgleich aller `IMP_*`-Deklarationen in den vendorten SDK-Headern (`include/<SoC>/<Version>/<Sprache>/imp/*.h`, Submodul `gtxaspec/ingenic-headers`) gegen die tatsächlich in `src/` aufgerufenen IMP-Funktionen — über **alle 9 von timps unterstützten Plattformen** (T10, T20, T21, T23, T30, T31, T40, T41, C100), nicht nur T31.

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
| 2 | `IMP_AI_EnableAec` (+`GetFrameAndRef`) | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ |
| 3 | `IMP_IVS_SetParam`/`GetParam` | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ |
| 4a | `SetAeWeight` (AE-Zonen-Gewichtung) | ✓ | ✓ | ✓ | ✓ | ✓ | ✓* | ✓* | ✓ |
| 4b | `AE_SetROI` | ✓ | ✓ | ✓ | ✓ | ✓ | – | – | ✓ |
| 5 | `SetFrontCrop`/`SetAutoZoom` (ePTZ) | ✓/✓ | ✓/✓ | –/– | ✓/✓ | –/– | –/✓ | –/✓ | –/– |
| 6 | `SetAe_IT_MAX`/`SetAeMin` (Belichtungs-Obergrenze) | ✓/✓ | ✓/✓ | –/✓ | ✓/✓ | –/– | –/– | –/– | –/– |
| 7a | `IMP_Encoder_Query` (Buffer/Stream-Stats) | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ |
| 7b | `GetChnAveBitrate`/`GetChnEvalInfo` | ✓ | – | – | – | – | – | – | – |
| 8 | `SetbufshareChn` | ✓ | ✓ | – | – | – | ✓ | ✓ | – |
| 9 | `IMP_ISP_WDR_ENABLE` | ✓ | ✓ | – | – | –† | ✓ | ✓ | –† |
| 10 | `IMP_IVS_CreateBaseMoveInterface` | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ |
| 11 | `IMP_FrameSource_SnapFrame` | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ |
| 12 | `SetChnEntropyMode` (CABAC-Toggle) | ✓ | – | – | – | – | – | – | – |

\* T40/T41 brauchen die neue Signatur (`IMPVI_NUM` als erstes Argument).
† T20/T30 haben stattdessen `SetWDRAttr` (andere API, gleiches Feature — siehe N2).

## Revidierte Rangfolge (flottengewichtet: T31 = primär, T20 = Wyze, T23 = Kinderzimmer/Galayou)

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

Die separate Motion-Detection-Tiefenprüfung (M1–M3 + Sensitivity-Dedup) wurde bereits umgesetzt (Commits `0385aca`, `2d9a66d`, `833ab73`, `8b02945`). Finding #3 dieser Liste (`IMP_IVS_SetParam`) würde M2s verbleibendes Rebuild-Problem an der Wurzel lösen (Sensitivity ändern ohne vollen Stop/Destroy/Recreate-Zyklus) — aktuell nur durch Batching gemildert, nicht eliminiert.
