# timps v1.11.0 / v1.11.1 – Release (2026-10-25)

**Erledigt 2026-10-25 (mit Go):** v1.11.0 (f36c42c) und v1.11.1 (ec28a53)
getaggt, GitHub-Releases veröffentlicht, PRs #1873 (ciao) und #1874 (aperto)
pinnen v1.11.1 (Hash `7ab9c992…2059`). Hash-Methode: buildroot
`support/download/git` **mit `-r`** (Submodul `include/`) gegen
https://github.com/Lu-Fi/timps – ohne `-r` stimmt der Hash nicht.
Der Rest dieser Datei ist der Stand vor dem Tag.

## Stand

| Repo | Branch | Commit | Inhalt |
|---|---|---|---|
| timps | `dev/1.10.3` (Worktree `/mnt/NVMe/git/timps-1103`) | d30e5b1 | 45 Commits über `main`/v1.10.1, `main` kann vorgespult werden (fast-forward) |
| thingino-firmware | `pr/timps-v1.11.0-ciao` (Worktree `scratch-builds/rel1110/fw-ciao`) | d2f5eb706 | auf upstream/ciao: S95timps-Defaults, WebUI (Testbild, Crop/ROI-Hinweise, Encoder-Stats) |
| thingino-firmware | `pr/timps-v1.11.0-aperto` (Worktree `scratch-builds/rel1110/fw-aperto`) | bd79e5cea | dasselbe auf upstream/aperto, plus CGI-Fix (#1860 war nur ciao) |

Fehlt noch: der Pin-Commit (`TIMPS_VERSION = v1.11.0` + Hash). Den Hash
erst nach dem Tag-Push über `make timps-source` bilden: lokal nachgerechnet
weicht er schon für v1.10.1 vom eingecheckten Wert ab.

## Geprüft

- Host-Tests: alle 23 Ziele PASS (`test-auth` … `test-roi`, ROI-Tier-Matrix 17 Builds)
- Day/night replay: 64/64 Paare (inkl. neu `dawn_ir`, `fire`)
- Cross-Builds aus den Fleet-Build-Verzeichnissen, 0 Warnungen:
  T10 T20 T21 T23 T31 T40 T41, je Open Stack und Hersteller-Stack;
  Kontroll-Builds bitgleich mit den Kamera-Binaries.
- Größe gestrippt (Open Stack, gegenüber Fleet 97fbb7c): T10/T20/T21/T31 gleich,
  T23 +4096 B (Seitengrenze), T40 +4 B, T41 +20 B.
- Gerät: T41 (.32, OpenIMP 0f83744 + open-tx-isp next): Crop live 1920x1080
  ohne Neustart, ROI unter FixQP, DRC/DPC, Telemetrie v1 – OK.
- Gerät: Tag/Nacht-Änderung auf .26/.25/.224 seit 2026-10-24 10:35.

## Vor dem Tag offen

1. Auswertung Dämmerungstest (2026-10-25 10:07) – entscheidet über die
   Tag/Nacht-Änderung (8403ea8).
2. Fleet-Lauf mit einem Image aus OpenIMP next + timps v1.11.0-Kandidat,
   mindestens eine Nacht, ohne Hänger/Reboots (Regel: Release nur sauber).
3. `scripts/timps-qa.sh --profile standard` auf je einer T23, T31, T41.

## Ablauf nach Go

1. `git -C /mnt/NVMe/git/timps merge --ff-only dev/1.10.3`
2. `git tag -a v1.11.0 -m "timps v1.11.0"`; `git push origin main v1.11.0`
3. GitHub-Release aus dem CHANGELOG-Abschnitt [1.11.0]
4. In einem Firmware-Build: `make timps-source`, dann
   `sha256sum dl/timps/timps-v1.11.0-git4.tar.gz` → `timps.hash`
5. Pin-Commit „package/timps: update to v1.11.0“ auf beiden PR-Branches
6. Branches in den Fork pushen, PRs gegen themactep `ciao` und `aperto`

## PR-Text (Entwurf, ciao)

**package/timps: update to v1.11.0**

- timps v1.11.0: T40/T41 encoder ROI and crop (AutoZoom) with OpenIMP,
  T40/T41 I2D rotation, OpenIMP test pattern with motion-test scoring,
  T41 DRC/DPC strength, encoder telemetry, day/night colour trigger,
  jump bar from learnt k, steadier dawn behaviour, boot check without the
  2-minute wait. Full list in the timps CHANGELOG.
- WebUI: test pattern card on the preview page, crop note for the T41
  AutoZoom path, ROI shown inactive under FixQP where the encoder ignores it,
  encode time and keyframes in the stream stats.
- S95timps treats an unset `webrtc.enabled` and unset certificate paths the
  way timps does, so WebRTC gets its certificate on a default config.

(aperto: zusätzlich der Fix für das Speichern der Tag/Nacht-Steuerung, der
auf ciao schon als #1860 drin ist.)

🤖 Generated with [Claude Code](https://claude.com/claude-code)
