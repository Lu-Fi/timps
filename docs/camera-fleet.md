# Camera fleet inventory

Stand: 2026-08-02. Alle Kameras laufen mit dem `timps`-Daemon (branch `feat/adaptive-fmp4-drop`) auf thingino-firmware.

| Name | Standort | IP | Camera-Profil (`CAMERA=`) | SoC | Sensor | Subnetz / Router | PTZ |
|---|---|---|---|---|---|---|---|
| Garage | Garage | 192.168.241.190 | `wuuk_y0510_t31x_sc4336p_ssv6158` | T31 | sc4336p | 192.168.241.x ("c7" / TP-Link) | nein |
| cam-db | Dachboden | 192.168.10.145 | `cinnado_d1_t31l_sc2336_atbm6031` | T31 | sc2336 | 192.168.10.x ("Zyxcel") | ja |
| cam-schuppen | Schuppen | 192.168.241.102 | `cinnado_d1_t31l_sc2336_atbm6031` | T31 | sc2336 | 192.168.241.x ("c7") | ja |
| cam-sz | Schlafzimmer | 192.168.241.170 | `cinnado_d1_t31l_sc2336_atbm6031` | T31 | sc2336 | 192.168.241.x ("c7") | ja |
| cam-wohn | Wohnzimmer | 192.168.241.204 | `cinnado_d1_t31l_sc2336_atbm6031` | T31 | sc2336 | 192.168.241.x ("c7") | ja |
| cam-wintergarten | Wintergarten | 192.168.241.244 | `cinnado_d1_t31l_sc2336_atbm6031` | T31 | sc2336 | 192.168.241.x ("c7") | ja |
| cam-kinder-links | Kinderzimmer (links) | 192.168.10.124 | `cinnado_d1_t23n_sc2336_atbm6012bx` | T23 | sc2336 | 192.168.10.x ("Zyxcel") | ja |
| cam-kinder-rechts | Kinderzimmer (rechts) | 192.168.10.151 | `cinnado_d1_t31l_sc2336_atbm6031` | T31 | sc2336 | 192.168.10.x ("Zyxcel") | ja |
| cam-wohn-ofen (Wuuk) | Wohnzimmer | 192.168.10.224 | `wuuk_y0510_t31x_sc4336p_ssv6158` | T31 | sc4336p | 192.168.10.x ("Zyxcel") | nein |
| cam-wyze | Keller | 192.168.10.107 | `wyze_cam2_t20x_jxf23_rtl8189ftv` | T20 | jxf23 | 192.168.10.x ("Zyxcel") | nein |
| Galayou | Vorgarten | 192.168.15.129 | `galayou_y4_t23n_sc2336_atbm6062` | T23 | sc2336 | 192.168.15.x (eigenes Subnetz) | nein |

## Anmerkungen

- **SoC-Generationen**: T31 nutzt die neue IMP-Encoder-API (`IMP_Encoder_SetDefaultParam`), T20/T23 die ältere manuelle Attr-Konfiguration. Relevant für alle Feature-Entscheidungen, die IMP-SDK-Funktionen direkt aufrufen (siehe `Makefile`-Auswahl von `IMP_INC` je `PLATFORM`).
- **`cam-kinder-rechts` (.10.151)**: bislang in diesem Batch-Rebuild nicht angefasst ("untouched") — läuft noch auf einem älteren Build-Stand.
- **PTZ-Positionen** werden live über `motors -j` abgefragt und zusätzlich in `user/<profil>/<ip>/overlay/etc/thingino.json` (`pos_0`) persistiert, damit sie einen Reflash überleben.
- **Kein PTZ**: Garage, Wuuk/Wohnzimmer (cam-wohn-ofen), Wyze/Keller, Galayou — feste Montage bzw. Modell ohne Motoren.
- IP-Zuordnung und Camera-Profil stammen aus den Verzeichnissen unter `user/<profil>/<ip>/` im thingino-firmware-LuFi-Repo (`overlay/etc/hostname` als Namensquelle).
