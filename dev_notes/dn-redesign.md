# Day/night: the simple engine (1.10.1)

Replaces the engine of 1.10.0 (`src/daynight.c` decision thread, `dn_idle.h`,
`dn_reach.h`, `dn_proven.h`, the 72-scenario corpus). Principle (user,
2026-10-11): just day/night switching, simple, stable, reliable; an occasional
wrong switch is acceptable, getting stuck, pendulum switching and unbounded
time in the wrong mode are not. Development history: branch `dev/dn-redesign`.

## 1. How the camera switches

> By day the camera measures how much it has to amplify the picture. If it
> needs more than `night_gain` for 30 seconds (`day_confirm_s`) or cannot
> amplify any further, it switches to night: IR-cut filter open, infrared
> light on. When it is very dark (twice `night_gain`, or at the limit) it
> switches after 6 seconds.
>
> At night it cannot judge the room through its own infrared light. So every
> 45 minutes, and at once when the night picture gets `jump_pct` (20 %)
> brighter than at the last look for 20 seconds, it switches its infrared light
> off for 8 seconds and looks. Since 1.10.2 also when the colour of the night
> picture changes by `color_pct` (3 %) for 20 seconds: a weak lamp that hardly
> brightens the infrared picture still tints it. If the room is bright enough without it, it
> switches to the day picture (one click of the IR-cut filter) and measures:
> below `day_gain` it stays day, otherwise it goes back to night (a second
> click) and remembers how much darker the room without infrared has to get
> before it asks again. A sudden brightening forgets that: it is new light.
> When the night picture stays darker than at the last look for a minute
> (the light it was taken under has gone), that darker level becomes the
> reference and the next look relearns the bar.
> After failed checks the next one waits 1, 2, 4, 8 … minutes.
>
> If no check has seen the day picture for `heartbeat_max_s` (12 h), it checks
> anyway, so it cannot stay stuck in night. Cameras whose infrared light
> cannot be switched on its own do not look; they check when the night picture
> gets twice as bright and every `heartbeat_s` (4 h).
>
> Every start checks with the day picture (one click, two in the dark). While
> nobody watches at night (`ir_idle_off`), the infrared light stays off and the
> camera keeps looking; the first viewer gets the light at once.

## 2. Mechanisms, each with the family pair that fails without it

Ablation: each mechanism compiled out, the whole family corpus re-run
(prototype `src/dn_simple.inc` on `dev/dn-redesign`, `-DSD_NO_<X>=1`).

| Mechanism | Fails without it |
| --- | --- |
| Day→night on `night_gain` or the AE at its limit | plateau-pan, lamp_nojump-pan |
| ... started by the raw reading, not only the smoothed one | lamp_jump-t10 (light off 53 s > 40 s) |
| ... 6 s when very dark (K1) | (user wish; light off 33 s → 9 s where deep) |
| Timer look every 45 min | lamp_nojump-pan/-eufy, eufy_dark_bulb |
| Jump: lit level `jump_pct` below the last look for 20 s → look now, forget the bar | lamp_jump-t10/-eufy, foreign_ir_lamp-pan/-t10, check_then_light, isp_stuck |
| Look after a jump asks below 2 × `day_gain` | dawn-schuppen (dark reading stays above `day_gain` in daylight) |
| Scene bar B after a failed check: `dark × day_gain / day`, at least `0.5 × dark` | steady_night-eufy (12 switches, bound 2) |
| ... `0.85 × dark` when the check read the AE at its limit or above 4 × `night_gain` (Eufy: 40960 without a reported limit) | eufy_dark_bulb |
| A check without a fresh look learns B from a look 30 s later | lamp_nojump-pan/-eufy |
| Lit level 10 % above the reference, not below it for 60 s → it is the reference, B relearned at the next look | ref_stuck-t10/-pc420 (field 2026-10-11: a short foreign light failed a check, the cellar lamp 30 min later never switched); ref_noisy-pan (device test 2026-10-12: the level wobbled 173-181 around the first rule's 179) |
| Lockout 1, 2, 4 … min after failed checks | dawn-wohn (9 switches, bound 8) |
| Guarantee check after `heartbeat_max_s` | blind_look-eufy |
| Readback: one edge through the other mode, held 5 s | isp_stuck-pan |
| `running_mode` re-asserted twice, 8 s apart, after each switch (not after the boot check) | (hardware lesson; the harness ISP does not react to it) |
| Idle-Dark as a look that lasts | idle-pan, idle-t10 |
| Colour trigger (1.10.2): lit colour `color_pct` off the reference for 20 s → look | lamp_color-pan/-eufy (light on 2699 s, bound 60 s) |
| ... no colour look while a jump hold runs (the jump forgets the bar, a colour look keeps it) | lamp_jump-eufy (1800 s wrong), check_then_light-eufy (1129 s) |
| ... at most every 5 min | person-pan/-eufy (27/22 looks, bound 6) |
| ... gap doubling up to 45 min, reset by a timer look or a switch to night | person-pan/-eufy (8 looks, bound 6) |

Removed (no family needs them): the IR-ratio verdict and permission, the 60 s
check floor after a jump with `retry_why`, the boot look, desync adoption, the
trend EMA pair, the night-reference ratchet, the proven-night gate,
filter/noir/dark cost vetoes, heartbeat deferral, the plateau learning, the
threshold diagnostic.

Candidates from `dev_notes/DAYNIGHT_FASTSWITCH_FLAPGUARD_2026-10-11.md`:
- K1 fast day→night when very dark: **adopted** (6 s, not within 8 s of the
  engine's own switch); light off 33 s → 9 s on deep scenes; `cloud` family
  (dips to the middle of the band) does not switch.
- K2 shorter jump hold: **adopted at 20 s, not 8 s**. 8 s and 15 s turned 12 s
  light bursts (`transient` family) into failed checks (+2 clicks) and
  delayed dawn-schuppen past its bound; 20 s: light on 52 s → 42 s, bursts
  ignored.
- K3 Idle-Dark reading counts as a look: already so (leaving idle restarts the
  look timer); no extra code.
- K4 the guarantee never waits behind the lockout: kept, also for no-LED
  cameras.

## 3. Bounds and measurements (`make test-dn-replay`, 45 pairs; 1.10.1 shipped 48, 1.10.2 has 54, see section 8)

| Bound | Measured (new engine) |
| --- | --- |
| Dark time ≤ 0.3 % of a night hour | 8 s / 45 min = 0.30 % steady; 0.33 % over 4 h incl. the learning look after boot |
| Single look ≤ 5 s if provable | 8 s: no data shows the AE settled in less (the Eufy looks read stable values after 8 s); the sim's AE is instant and proves nothing |
| Clicks on a typical day ≤ 6 | dusk + dawn after boot: wohn 6, schuppen 3, t10 2 |
| Clicks worst case per day ≤ 30 | 28: 11 failed checks (lockout 0, 1, 3, 7 … 1023 min) + 2 guarantee checks + 2 real switches; genuine light changes on top |
| Light on with a jump → day ≤ 60 s, also right after a failed check | max 42 s (check_then_light 42 s, foreign_ir_lamp 32 s) |
| Light on without a jump → day ≤ 1 look interval | max 2436 s (< 2700 s) |
| Light off → night ≤ 40 s | max 33 s; 9 s where deep (K1) |
| No pendulum (two switches < 2 min without a light change) | 0 events |
| Dawn → day ≤ 30 min | max 71 s after the day pipeline went below `day_gain` |
| Dusk → night ≤ 5 min | max 34 s |

## 4. Comparison on the same families and profiles

thingino-daynightd and raptor are **models** (`scripts/dn-families/others.py`):
daynightd's ev_log2 is modelled as 823.5 × exposure index (its 50 % day
threshold on `day_gain`), raptor's ae_luma as on-target unless the reading is
railed or above 4 × `night_gain`; both with an instant AE. Legacy is the
1.10.0 engine of `dev/dn-bright-room`, run in the harness.

| | new | legacy 1.10.0 | daynightd (model) | raptor luma (model) |
| --- | --- | --- | --- | --- |
| pairs within bounds | 45/45 | 33/45 | 22/42 | 20/42 |
| dark time, steady night | 0.33 % | 0.06 % | 0 | 0.89 % |
| switches, whole corpus | 141 (43 failed checks) | 104 (26) | 1766 | 181 |
| light on with a jump → day, max | 42 s | 3299 s | 3 s | 119 s |
| light off → night, max | 33 s | 53 s | 6299 s | 1499 s |
| dusk/dawn after crossing, max | 71 s | 53 s | 2760 s | 3299 s |
| pendulum events | 0 | 3 | 1646 | 0 |

Legacy fails the Eufy lamp cases, the Pan lamp without a jump, no-LED Pan and
the blind look (night for the whole run); daynightd oscillates in every IR-lit
cellar (its night reading is its own illuminator); raptor misses dusk on
cameras whose AE has reserve and goes day under foreign infrared.

## 5. Known limits

- Dim lit rooms (day pipeline between `day_gain` and `night_gain`) stay in
  their mode; raise `day_gain` per camera.
- A lamp that moves neither the illuminator-lit level nor (1.10.2) its colour
  waits up to 45 min, as does one on a SoC without AWB statistics or switched
  on while colour looks are spaced out; `{"daynight":{"probe":1}}` looks at
  once.
- Other cameras' infrared costs one failed check (two clicks) per light change.
- Every boot in the dark costs a click pair.
- 8 s of dark night video every 45 min.
- No-LED cameras see small light changes only every `heartbeat_s` (4 h).

## 6. Corpus

`scripts/dn-families/`: `families.json` (21 families: dusk, dawn, lamp with and
without a jump, lamp after a failed check, foreign IR (+ lamp), boot dark /
dark from day / lit, plateau, no LED, blind look, Idle-Dark, ISP stuck, steady
night, flicker, transient, cloud, Eufy dark → bulb, failed check → light),
`profiles/*.json` (pan, eufy, t10, wohn, schuppen, t41, t23, cam05, shed: day /
lit / dark triples or daylight curves from logs, the IR campaign of
2026-08-18/19, the user's Eufy measurements and incident scenarios; assumed
values marked), `run.py` (one harness scenario per pair, parallel, behaviour
only: mode against the truth from the day-pipeline value, longest wrong run,
switches, light-on/off latency, pendulum, LED timeline), `others.py`,
`compare.py`. 45 pairs, ~6-8 min with 8 jobs, deterministic (no noise); since
1.10.2 26 families, 54 pairs, colour noise seeded per 2 s (section 8).

## 7. Firmware WebUI changes needed (thingino `package/timps/files/www`)

- `config-photosensing.html` / `a/config-photosensing.js`: remove the fields
  `probe_confirm_s`, `probe_min_gap_s`, `boot_probe`, `diagnose_thresholds`
  (INT_FIELDS/BOOL_FIELDS) and the "Fixed internals" line
  (`probe_jump_pct`, `ref_delay_s`, `boot_settle_s`, FIXED_FIELDS); add
  `jump_pct` (5-70 %, "Look at once when the night picture gets this much
  brighter"); relabel `heartbeat_s` ("Check interval without switchable IR
  light") and `heartbeat_max_s` ("Longest night without a check"); drop
  `probe_min_gap_s` from the duration hints (`durHints`).
- "Live" line: `night_baseline` is now the IR-lit level at the last look and
  `day_trigger` the bar the reading **without** IR must fall below (not a bar
  on the lit level); reword "night reference" / "probe below".
- `preview.html` and the tuning graph in `config-photosensing.js` draw
  `day_trigger` against the lit exposure: drop that line or label it
  "look bar (IR off)".
- Idle-Dark card: unchanged keys and status (`daynight.ir_idle`); the text
  about "a direct switch to day at the first viewer" no longer applies (a
  check happens while idle instead).
- `tool-sensor-data.js` (day_gain/night_gain/history_s/interval_ms): no change.

## 8. Colour trigger (1.10.2)

Measured 2026-10-11 (branch `dbg/light-signals`, IR on, night, the HA bulb in
front of the camera, `scratch-builds/dbglight/dbg-light.csv`): at 30 % the Eufy
(T40 vendor) keeps its exposure index (221 → 221) while the global B/G of
`GetAwbStatistics` drops 4 % (R/G +2 %); the Pan (T20 OpenIMP, zone ioctl)
moves −4..−8 % in exposure and +5..6 % in B/G. At 100 % both move far (B/G
−36 % / +29..44 %). An unchanged scene stays within 0.5 % (Pan B/G ±0.65 %).
The sign differs per platform, so the trigger is the larger of |ΔR/G| and
|ΔB/G| against the reference.

Mechanism (`src/daynight.c`, `dn_thread` S_NIGHT):

- Reference: the first sample in S_NIGHT at least 8 s after leaving any other
  state (a look, a check, a switch, Idle-Dark) with the AE stable (4 samples).
  Re-taken after every look, so a lamp that stays on, or a person who stays,
  becomes the new normal.
- Trigger: the deviation above `color_pct` for 20 s (the jump hold), not
  locked out, not while a jump hold runs → a look with `why = "color"`. The
  look decides as always; a colour look keeps the scene bar B (a jump forgets
  it), so a person in view cannot buy a failed check where B is learnt.
- Rate: the next colour look waits `c_gap` (5 min), which doubles after each
  colour look up to the look interval (45 min) and resets at a timer look or
  a switch to night.
- Worst case dark time: under persistent colour churn 4 colour looks in the
  first hour after a reset (5, 10, 20 min apart: 32 s, 0.9 % of the hour),
  then one per 45 min in place of the timer look (0.30 %); an adversarial
  pattern that is quiet at every timer look restarts the doubling: about 5
  looks per 80 min, ≈ 30 s/h (0.83 %). Never a click by itself.
- Availability: `hal_isp_awb_ratio()`, Q12 R/G and B/G from per-zone
  statistics, weak-bound: T40 vendor and T41 (`GetAwbStatistics`, 15×15 u32),
  T23/T31/C100 vendor (`GetAwbZone`, 15×15 u8 means), T10/T20 OpenIMP
  (`T2X_CID_AWB_ZONE` ioctl on libimp's `/dev/video0` fd; the pinned OpenIMP's
  `GetAwbZone` sends a CID the T20 driver rejects). Not: T40 OpenIMP (the open
  driver's statistics path is a stub), T23/T31 OpenIMP (`GetAwbZone` took
  three pointers before openimp e7dadd6, which the pin lacks: a stack
  overrun), T10/T20/T21 vendor (no zones; hist/GOL are white-region filtered
  or the AWB's own estimate). Read every 5 s, only at night (and up to 30 times
  before the first success): one read measured 1.4 ms on the Eufy and 1.7 ms
  on the Pan (device test 2026-10-12), the 20 s hold needs no more; 3
  failures in a row = `color_trigger: "unavailable"`.
  Only the Eufy and the Pan are measured; the others are unverified.
- Corpus: profiles `pan`/`eufy` carry `color` (base, shift per unit lamp,
  noise) and per-scene `lamp`/`tint`; the harness serves `awb_lit` (and far
  different colours with the LED off or in day, so a reading taken there
  shows). New families `lamp_color` (day within 32 s), `person` (walk-throughs
  ignored, 4 rate-limited colour looks, no click), `foreign_ir_color` (another
  camera's IR, colour unchanged: no colour look). `lamp_nojump` runs with
  `color_pct = 0` and keeps testing the timer path. 54 pairs, all within
  bounds; the 48 pairs of v1.10.1 keep their switch total (154) and looks
  98 → 96 (one look less in dawn-wohn/-schuppen, timing); faster light-on:
  lamp_again-eufy 919 → 31 s, eufy_dark_bulb 2438 → 32 s.
