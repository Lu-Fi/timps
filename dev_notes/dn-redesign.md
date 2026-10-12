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
> off for 8 seconds and looks. If the room is bright enough without it, it
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

## 3. Bounds and measurements (`make test-dn-replay`, 45 pairs)

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
- A lamp that does not move the illuminator-lit level waits up to 45 min
  (Wyze Pan in the cellar); `{"daynight":{"probe":1}}` looks at once.
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
`compare.py`. 45 pairs, ~6-8 min with 8 jobs, deterministic (no noise).

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
