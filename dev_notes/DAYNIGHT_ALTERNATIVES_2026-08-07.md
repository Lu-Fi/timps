# Day/Night-Erkennung – Alternativen-Enumeration (Design-Exploration)

**Datum:** 2026-08-07
**Codestand:** `074e8f5` (v1.7.8), `src/daynight.c` = 1322 Zeilen
**Charakter:** reine Optionsraum-Kartierung, **keine Empfehlung, kein Code, kein Patch.** Grundlage für eine mögliche spätere Redesign-Entscheidung durch einen Menschen.
**Verifikation:** gegen den tatsächlichen Code (`src/daynight.c` vollständig gelesen), `src/hal/hal.h` (verfügbare Signal-APIs), `docs/sdk-feature-gaps.md` (Plattform-Verfügbarkeitsmatrix über alle 9 SoCs), `docs/wiki/Configuration-Reference.md` (daynight-Keys) und die CHANGELOG-Historie v1.7.3–v1.7.8 (warum jede Härtung eingeführt wurde).

---

## 0. Ausgangslage: was heute existiert und WARUM

Der Ist-Stand ist ein Hintergrund-Thread (`dn_thread`), der in `interval_ms` (Default 500 ms) pollt und drei Betriebsmodi kennt:

- **`sensor`** (Default): primär **ISP total_gain** über `hal_isp_total_gain()` (IMP `GetTotalGain`), Fallback = `/proc/jz/isp/isp-m0`-Scrape (Integrationszeit + Gains + Brightness). Hohe Gain = dunkel = Nacht. Der breite Totzonen-Spalt `total_gain_day_threshold`..`night_threshold` (300..3000) ist die Hysterese.
- **`time`**: reine Wanduhr, kein Sensor, **kein Probe**.
- **`sun`**: Sonnenstands-Berechnung (NOAA/Meeus) aus lat/lon + Offsets, kein Sensor, **kein Probe**.

Die gesamte Komplexität im `sensor`-Modus existiert wegen **einer** physikalischen Grundtatsache, die man beim Vergleich der Alternativen ständig im Kopf behalten muss:

> **Der einzige Umgebungslicht-Messwert des `sensor`-Modus (Gain/Luma) wird DURCH die gerade aktive optische+Tuning-Pipeline gemessen.** Im Nachtzustand ist der IR-Cut-Filter entfernt, die IR-LEDs an und die Nacht-AE-Tabelle aktiv. Ein dunkler Raum sieht dadurch für den Sensor „hell" aus (IR-Licht erreicht den Sensor). Die Gain-Messung im Nachtpfad ist also **kein verlässlicher Proxy für „ist es tatsächlich Tag"**.

Daraus folgt zwingend der **Probe**: um zu erfahren, ob es wirklich Tag ist, muss die Kamera physisch kurz in die Tag-Konfiguration (IR-Cut eingeschwenkt, IR-LEDs aus, Farbpipeline) versetzt werden, einen echten Tageslicht-Wert lesen und ggf. zurückschalten. **Dieser physische Probe-Klick (mechanisches IR-Cut-Relais, hörbar; dazu ~7–9 s ausgewaschenes Farbvideo) ist die zentrale Nutzer-Ärgernis-Quelle.** Die komplette Maschinerie – exponentielles Backoff, Brightening-Margin, Failure-Ratchet, Passive-Evidence-Skip, `probe_max_skip_s`-Außengrenze, Oszillations-Breaker, Baseline-Drift – dient ausschließlich dazu, die **Frequenz** dieses Klicks zu minimieren, **ohne** die Selbstheilungs-Eigenschaft (steckt die Kamera im falschen Modus fest, korrigiert sie sich) zu verlieren. Jeder Fix wurde durch reale Fleet-Incidents getrieben (v1.7.3 Flap-Loop, v1.7.4 Probe-Economy, v1.7.5 Dead-Zone-Adoption + Oszillations-Breaker, v1.7.6 Silent-Limbo, v1.7.8 `probe_max_skip_s` konfigurierbar).

Der Speicher-Footprint ist bereits **winzig**: ein paar Skalare + drei kleine Ringpuffer (`hist[10]`, `settle_hist[6]`, `osc_hist[3]` floats). Es gibt **keinen** großen Puffer zurückzugewinnen. Die 1322 Zeilen sind fast vollständig Logik + ausführliche Incident-Kommentare, **nicht** Datenstrukturen.

---

## Teil A — Alternative MESS-SIGNALE (Ersatz für das ISP-total_gain-Reading selbst)

Diese Sektion listet **nur das Entscheidungssignal** auf, unabhängig vom Regelalgorithmus (Teil B). Kernfrage bei jedem: *Ist das Signal downstream der IR-Pipeline (→ braucht weiterhin Probe) oder unabhängig davon (→ probe-frei möglich)?*

### A1 — ISP total_gain (Ist-Zustand, Referenz)
- **Signal:** `hal_isp_total_gain()` / `/proc`-Scrape der Gains.
- **Plattform:** IMP-API auf allen außer T40/T41 + Sim; dort greift der `/proc`-Scrape-Fallback. Faktisch fleetweit nutzbar.
- **Pipeline-abhängig?** JA. → Probe unvermeidbar für Nacht→Tag.
- **Komplexität/RAM/CPU:** Referenz. Sehr günstig (ein API-Call/Tick, Scrape gedrosselt auf `DN_SCRAPE_MS`).
- **Schwäche:** AGC-Rauschen im Dunkeln (IR-AGC-Hunting) → ist die Wurzel des ganzen Smoothing/Ratchet-Apparats.

### A2 — AGC/Belichtungszeit (Integration Time) statt Gain
- **Signal:** `SENSOR Integration Time` / `Max Integration Time` aus demselben `/proc`-Scrape (heute nur für den Brightness-Fallback genutzt). Idee: **Belichtungszeit sättigt bei Dunkelheit am Maximum, BEVOR die Gain hochläuft** – das kombinierte „total exposure" (Integrationszeit × Gain) ist ein monotonerer, am dunklen Ende **weniger verrauschter** Lichtproxy als Gain allein.
- **Plattform:** Integrationszeit steht im `/proc`-Dump breit zur Verfügung; die neuen Tuning-APIs (`GetIntegrationTime`) laut Feature-Gaps auf alten Chips (T20/T21/T30) vorhanden.
- **Pipeline-abhängig?** JA (gleiche Optik/AE wie A1). → Probe bleibt.
- **Komplexität/RAM/CPU:** ~gleich; evtl. **weniger** Smoothing-Code nötig, weil das Signal ruhiger ist. Könnte `DN_SMOOTH_ALPHA`/Ratchet-Aufwand reduzieren.
- **Verlust:** nichts Grundlegendes; behebt die Probe-Frage NICHT. Marginaler, aber ehrlicher Robustheitsgewinn an der Rauschquelle.

### A3 — AE-Luma (ISP-Zielhelligkeit)
- **Signal:** `hal_isp_ae_luma()` (raptors `ae_luma`).
- **Plattform:** **nur T21/T23/T31/C100** (`IMP_ISP_Tuning_GetAeLuma`); fehlt auf T20/T10/T30/T40/T41 → **nicht fleetweit**.
- **Pipeline-abhängig?** JA – und schlimmer: AE-Luma wird von der AE-Schleife **auf einen Sollwert geregelt**, ist im eingeschwungenen Zustand also ~konstant unabhängig vom Umgebungslicht. Als **Primärsignal schlechter** als Gain; taugt höchstens als Sekundär-Bestätigung.
- **Fazit:** kein Kandidat als Primärsignal; Probe bleibt.

### A4 — Frame-Luma / Luma-Histogramm aus echtem Videoframe
- **Signal:** `IMP_FrameSource_SnapFrame` (laut Feature-Gaps **auf allen 9 Plattformen** verfügbar) → echtes YUV-Frame greifen, Luma-Histogramm rechnen. Reicher als der eine AE-Luma-Skalar (unterscheidet „gleichmäßig dunkel" von „hell mit dunklen Regionen").
- **Plattform:** universell (SnapFrame überall).
- **Pipeline-abhängig?** JA – das Frame ist das Ergebnis derselben Pipeline. Im Nachtmodus ist es bereits mono/IR-beleuchtet; ein Histogramm daraus ist genauso wenig ein Tageslicht-Proxy wie A1. → Probe bleibt für Nacht→Tag.
- **Komplexität/RAM/CPU:** **deutlich MEHR** – Frame-Grab (großer Puffer, VBM/rmem-Kosten), Y-Ebene scannen (per-Tick CPU über Zehntausende Pixel), 256-Bin-Histogramm. Erster echter Speicher-**Mehr**verbrauch im ganzen Vergleich.
- **Verlust/Gewinn:** kein Probe-Vorteil; teuer. Nur sinnvoll als Zusatzfeature (s. A6).

### A5 — IR-Cut-Relais-Rückmeldung (Hardware-Feedback-Pin)
- **Signal:** ein GPIO, der die **physische IR-Cut-Filterposition** zurückliest.
- **Plattform:** **Existiert im timps-HAL NICHT.** Der Moduswechsel ist heute fire-and-forget über ein externes Board-Skript (`switch_cmd day|night`); es gibt keine Sense-Leitung und keine HAL-Funktion dafür. Thingino-Boards treiben den IR-Cut per GPIO, exportieren aber i.d.R. keinen Readback. → praktisch **nicht verfügbar**.
- **Was es lösen würde:** nur die Frage „steht der Filter, wo ich denke?" (könnte den `DN_REASSERT`-Safety-Net ersetzen) – **NICHT** die Frage „ist es draußen Tag?". Löst das Probe-Problem also gar nicht.
- **Fazit:** irrelevant für die Kernfrage; zusätzlich hardware-seitig nicht vorhanden.

### A6 — Farb-Sättigung / Chroma-Energie des Frames
- **Signal:** SnapFrame → U/V-Chroma-Energie messen. Bei echtem Tag erzeugt die Farbpipeline reale Farbe (hohe Chroma); bei tatsächlich dunkler, IR-beleuchteter Szene ist das Bild selbst im Tagpfad nahezu graustufig (Chroma ≈ 0).
- **Plattform:** universell (SnapFrame).
- **Pipeline-abhängig?** TEILWEISE. Nützlich vor allem für die **Tag→Nacht-Richtung** (erkennt „ausgewaschenes/farbloses Tagbild" → sollte Nacht sein). Für **Nacht→Tag nutzlos**: im Nachtmodus ist der Stream ohnehin mono, Chroma immer ~0 – „Nachtpipeline bei Tageslicht" ist per Chroma nicht von „Nachtpipeline im Dunkeln" unterscheidbar. → Nacht→Tag-Probe bleibt.
- **Komplexität/RAM/CPU:** MEHR (Frame-Grab + UV-Scan), wie A4.
- **Verlust:** kein voller Probe-Ersatz; nur ein halbes Signal.

### A7 — Unabhängiger Umgebungslichtsensor (LDR/ALS auf ADC/I2C)
- **Signal:** ein separater Fotowiderstand/ALS, dessen Reading **NICHT** hinter der IR-Pipeline liegt. Viele Thingino-Kameras haben einen LDR an einem ADC/GPIO-Pin, den die board-eigenen daynight-Skripte lesen.
- **Plattform:** **board-abhängig, NICHT universell** – kein einheitlicher HAL-Zugriff heute; müsste pro Board ein ADC-Read implementiert werden. Auf Boards ohne LDR gar nicht möglich.
- **Pipeline-abhängig?** **NEIN** – das ist der springende Punkt: ein pipeline-unabhängiges Umgebungslicht-Signal.
- **Probe?** **Entfällt komplett** – man misst das Umgebungslicht direkt, unabhängig vom aktuellen Modus. Damit fällt die gesamte Backoff/Ratchet/Skip-Maschinerie weg.
- **Komplexität/RAM/CPU:** Entscheidungskern **viel einfacher** (simpler Schmitt-Trigger auf LDR-Wert); dafür neuer HAL-Code pro Board + Kalibrierung LDR→Schwelle.
- **Verlust:** keine Selbstheilungs-/Oszillations-Sonderfälle mehr nötig (das LDR lügt nicht über IR). ABER: Verfügbarkeit nicht garantiert → könnte nur als **bevorzugtes Signal MIT Gain-Fallback** eingesetzt werden, nie als alleiniger Ersatz fleetweit.

### A8 — Externe Zeit-/Sonnenquelle als „Signal" (kein Sensor)
- **Signal:** Wanduhr (`time`) bzw. Sonnenstand (`sun`) – beide bereits implementiert.
- **Pipeline-abhängig?** N/A – **komplett sensorunabhängig.**
- **Probe?** **Nie nötig.** Selbstheilung kommt automatisch von der nächsten Zeit-/Sonnenkante.
- **Verlust:** blind für tatsächliches Licht – Keller/Cloud/Innenraum-Kunstlicht/Verdeckung werden ignoriert. (Details unter B1/B2/B3.)

### A9 — „Shadow-Read": nur IR-LEDs kurz aus, IR-Cut NICHT bewegen
- **Signal:** kein neues Sensorsignal, sondern ein **billigerer Probe-Ersatz**. Wenn das Board die IR-LEDs getrennt vom mechanischen IR-Cut schalten kann, killt man kurz nur die IR-Beleuchtung (im Nachtpfad bleibend): im echt dunklen Raum läuft die Gain weiter hoch (bestätigt Dunkelheit), im tatsächlich hellen Raum bleibt sie moderat (sichtbares Licht vorhanden).
- **Plattform:** **board-abhängig** – setzt getrennte LED-Steuerung im `switch_cmd`-Skript voraus.
- **Probe?** Ersetzt den **hörbaren Relais-Klick** durch lautloses LED-Schalten – der Klick verschwindet, die kurze Bildstörung (dunkleres Bild für 1–2 s) bleibt.
- **Komplexität:** ~gleich wie heute, aber ein zusätzlicher Board-Skript-Vertrag nötig.
- **Verlust:** kein Robustheitsverlust; potentiell **größter Usability-Gewinn** bei kleinstem Umbau – aber nur wo Hardware/Skript mitspielt.

---

## Teil B — Alternative ARCHITEKTUREN / ALGORITHMEN

### B1 — Nur Sonnenstand (`sun`), Sensor komplett aus
- **Kernidee:** astronomische Berechnung entscheidet allein; kein Sensor, kein Probe. Bereits vorhanden.
- **Signal:** A8 (Sonnenstand).
- **Probe?** Nie.
- **Code:** **massiv weniger** – der gesamte sensor-Block, Baseline, Probes, Oszillation, Boot-Settle entfielen (nur `dn_sun_times`/`dn_sun_target` + Switch-Maschinerie bleiben). Grob <200 statt 1322 Zeilen.
- **RAM/CPU:** minimal (eine Berechnung/Tick, kein `/proc`, kein IMP-Call).
- **Verlust:** Keller/fensterloser Raum, dichte Bewölkung, verdeckte Kamera, Innenraum mit Kunstlicht → alle falsch. Kein Reagieren auf tatsächliches Licht. Selbstheilung trivial (Uhr).

### B2 — Nur Uhrzeit (`time`)
- Wie B1, noch simpler (keine Astro-Mathematik), aber ohne saisonale Anpassung. Gleiche Verluste, kein Probe.

### B3 — Sonne/Zeit als PRIMÄR, Sensor nur als begrenzte Korrektur (Hybrid)
- **Kernidee:** Der Sonnen-/Zeitplan bestimmt den Basiszustand. Der Sensor darf nur **innerhalb eines Fensters** korrigieren: z. B. „Nacht erzwingen, wenn der Raum während des geplanten Tages dunkel wird" oder nahe Dämmerung feinjustieren. Tag wird zur geplanten Sonnenaufgangskante **immer** wieder betreten, egal was der Sensor sagt.
- **Signal:** A8 primär + A1 sekundär.
- **Probe?** **Kein periodischer Reconfirm-Probe nötig** – die Selbstheilung liefert die Uhr: ein falscher Nacht-Latch löst sich spätestens an der nächsten Sonnenkante ohne je zu proben. Ein Probe wäre höchstens optional für eine schnellere Tag-Korrektur mitten in der Nacht (selten).
- **Code:** **deutlich weniger** als heute – Backoff/Ratchet/Skip/`probe_max_skip_s` entfallen weitgehend, weil die Uhr das „stuck forever"-Problem löst. Baseline/Oszillation nur noch im engen Korrekturfenster relevant.
- **RAM/CPU:** ~heutig oder geringer.
- **Verlust:** reagiert nicht mehr voll frei auf beliebiges Licht (z. B. ein Raum, der um 14:00 dunkel gemacht wird und um 15:00 wieder hell → im engen Fenster evtl. träge). Keller ohne sinnvolle Geokoordinaten bleibt problematisch. **Attraktivste probe-arme Architektur, die Sensor-Reaktivität weitgehend behält.**

### B4 — Fester Dual-Threshold / Schmitt-Trigger, KEINE adaptive Baseline, KEIN Probe
- **Kernidee:** Das, was `daynight.c` vor der ganzen Härtung war: feste `day`/`night`-Gain-Schwellen, breite Totzone, kein Baseline-Drift, kein Probe, kein Smoothing-Apparat.
- **Signal:** A1 (oder A2).
- **Probe?** **Keiner.**
- **Code:** **drastisch weniger** (grob 200–300 Zeilen).
- **RAM/CPU:** minimal.
- **Verlust – erheblich und konkret:** (1) `day_gain_pct`-Adaptivbaseline weg → Räume mit schwachem Kunstlicht, das die feste Schwelle nie unterschreitet, bleiben ewig Nacht (genau die zwei Incidents vom 2026-08-02). (2) **Keine Selbstheilung** – ein Nacht-Latch, der wegen des Pipeline-Problems (A0) nie unter die Tag-Schwelle kommt, bleibt für immer Nacht (Dead-Zone-/Silent-Limbo-Klasse kehrt zurück). (3) IR-Reflexions-Oszillation ungebremst. Beantwortet Q2 mit „ja, ohne Probe" – aber um den Preis der Robustheit, die durch Incidents erkauft wurde.

### B5 — Regelungstechnischer Schätzer (Tiefpass/Kalman auf log-Licht) statt Threshold+Hysterese
- **Kernidee:** Den ad-hoc-Smoothing-Zoo (EMA `smooth_tg`, Baseline-Drift, Boot-Settle-Stabilitätsfenster, Hysterese-Kandidat) durch **einen** prinzipiellen Schätzer ersetzen, der eine Umgebungslicht-Schätzung mit Varianz führt und schaltet, wenn die Schätzung eine Schwelle mit Konfidenz kreuzt.
- **Signal:** A1/A2 (unverändert das gain-Reading).
- **Probe?** **Bleibt** – der Schätzer ändert das Mess-Problem (A0) nicht, nur die Filterung.
- **Code:** der Entscheidungs-**Kern** könnte kompakter/eleganter werden (Boot-Settle + Smoothing + Hysterese in einem Filter vereint). ABER: sobald Selbstheilungs-Probe + Oszillations-Breaker + Passive-Skip wieder angeflanscht werden (die bleiben nötig), ist die Netto-LOC-Ersparnis ein Wash. Risiko: die genau auf Incidents getunten Sonderfälle gehen im „saubereren" Modell verloren und müssen neu erkämpft werden.
- **RAM/CPU:** ~gleich (alles ist ohnehin schon float; MIPS ohne FPU ist kein Blocker, da der Ist-Code schon floatet).
- **Verlust:** potentiell Regressionsrisiko bei den Feinheiten; kein Funktionsgewinn.

### B6 — Frame-Content-Klassifikator (Chroma + Luma-Histogramm + Gain, hand-getunt)
- **Kernidee:** SnapFrame-Thumbnail + mehrere Merkmale (A4+A6) zu einer robusteren Tag/Nacht-Klassifikation kombinieren.
- **Signal:** A4+A6 (+A1).
- **Probe?** **Bleibt** für Nacht→Tag (A0/A6-Argument).
- **Code/RAM/CPU:** **MEHR** auf allen Achsen (Frame-Grab + Multi-Feature/Tick).
- **Verlust/Gewinn:** kein Probe-Vorteil bei höheren Kosten. Nicht lohnend.

### B7 — Delegation an das OS (thingino `daynightd` / Board-Sensor)
- **Kernidee:** timps macht keine Erkennung, konsumiert nur den vom OS gesetzten Modus.
- **Probe?** Aus timps-Sicht keiner (verlagert).
- **Code:** in timps am wenigsten.
- **Verlust:** timps verliert Integration/Kontrolle und dupliziert genau das, wovon der native Port wegkam (`daynightd`-Formel wurde bewusst hereingeholt). Eher Delegation als Algorithmus.

---

## Teil C — Direkte Antwort auf die zwei Leitfragen

### Frage 1: Gibt es eine Alternative, die den Code MEANINGFULLY VEREINFACHT oder SIGNIFIKANT SPEICHER SPART, ohne viel echte Funktionalität zu verlieren?

**Speicher: praktisch nein – es gibt nichts zu sparen.** Der Ist-Footprint ist bereits vernachlässigbar (einige Skalare + drei winzige Ringpuffer, `hist[10]`/`settle_hist[6]`/`osc_hist[3]`). Kein Kandidat spart nennenswert RAM; die frame-basierten Signale (A4/A6/B6) würden als einzige den Verbrauch sogar **erhöhen** (SnapFrame-Puffer). „Signifikant Speicher sparen" ist am realen Ist-Zustand schlicht ein Nicht-Ziel.

**Code-Größe: ja, aber nur durch Aufgabe von incident-erkaufter Robustheit.** B4 (fester Schmitt-Trigger) und B1/B2 (nur Sonne/Zeit) sind dramatisch kürzer – opfern aber Selbstheilung, Dim-Room-Recovery und Oszillationsschutz, also genau die Funktionalität, für die die Zeilen existieren. **Der einzige Kandidat, der real vereinfacht UND wenig echte Funktion verliert, ist B3 (Sonne/Zeit primär, Sensor als begrenzte Korrektur):** die Uhr übernimmt die Selbstheilung, wodurch Backoff/Ratchet/Skip/`probe_max_skip_s` weitgehend entfallen – zum Preis geringerer freier Licht-Reaktivität und der Notwendigkeit sinnvoller Geodaten. B5 (Kalman/Tiefpass) macht den Kern eleganter, ist aber netto ein LOC-Wash und rein ein Refactor ohne Funktionsgewinn.

### Frage 2: Gibt es eine Alternative, die OHNE jeglichen Probe-Intervall (kein periodischer erzwungener physischer Moduswechsel) funktioniert?

**Ja – aber nur, indem das Entscheidungssignal aus einer Quelle kommt, die NICHT downstream der IR-Pipeline liegt.** Das ist der Kern: der `sensor`-Modus braucht den Probe zwingend, weil sein einziges Signal (Gain/Luma, ebenso Frame-Luma/Histogramm/Chroma) durch die aktive Nacht-Optik gemessen wird und ein dunkler Raum darin „hell" aussieht. Probe-freie Optionen:

1. **Sonne/Zeit** (B1/B2, bereits implementiert) – null Probe, aber blind für tatsächliches Licht (Keller/Cloud/Innenraum).
2. **Sonne/Zeit primär + Sensor-Korrektur** (B3) – **die attraktivste probe-freie Architektur, die Sensor-Reaktivität behält:** ein Fehl-Latch löst sich an der nächsten Sonnenkante ohne je zu proben; ein dunkel werdender Raum kann direkt auf Nacht gezwungen werden.
3. **Unabhängiger Umgebungslichtsensor (LDR/ALS, A7)** – die sauberste Lösung: pipeline-unabhängiges Signal → kein Probe, kein Backoff-Apparat – aber **nicht fleetweit verfügbar** (board-abhängig, kein HAL-Zugriff heute), also nur als bevorzugtes Signal mit Gain-Fallback denkbar.
4. **Fester Dual-Threshold (B4)** – kein Probe, aber verliert Selbstheilung und Dim-Room-Recovery.

**Nicht probe-frei machbar** sind hingegen alle rein bild-/gain-basierten Ansätze (A1/A2/A4/A6, B5/B6): sie messen alle durch dieselbe Pipeline und können „Nachtpipeline bei Tageslicht" nicht von „Nachtpipeline im Dunkeln" trennen, ohne physisch in den Tagpfad zu schalten. Der billigste Kompromiss ohne echten Signalwechsel ist **A9 (Shadow-Read: nur IR-LEDs kurz aus)** – das eliminiert den hörbaren Relais-Klick (nicht die kurze Bildstörung) und erfordert nur board-seitig getrennte LED-Steuerung.
