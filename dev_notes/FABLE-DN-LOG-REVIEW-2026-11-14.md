# Day/night engine: fleet log review 2026-11-10 .. 2026-11-14

Reviewer: Fable (independent read of the syslog extracts, no camera touched).
Data: `scratchpad/dnlogs/<host>.txt`, 20 cameras, 38 907 lines; times below are
syslog receipt times. Code read: `src/daynight.c` at v1.12.0 (= v1.12.1, the
file is identical in both tags), at a7cba61, and the uncommitted working tree.

During this review the working tree was committed as 832e6f7 ("daynight: one
look-skip test, desync gaps, darker-scene debounce"); "the working tree" below
means that state, and my sim builds are of it.

Versions in the window: the nine living-area cameras log no start line except
cam-vorne (v1.12.0, one boot 11-12 18:54) and cam-sz (v1.12.0, 69 scripted
restarts 11-11/11-12, then v1.12.1-7-ga7cba61 from 11-13 17:15). The message
set of the others (`colour moved ... while the scene got darker` followed by a
look without a skip test) fits v1.12.0/v1.12.1, not a7cba61. Test cameras run
v1.12.0, v1.12.1, a7cba61 and several test builds.

## 0. Summary

On the living-area cameras the engine made no wrong lasting switch in these
4.5 days: every dusk switched at 4100-4500 (cameras with the default
`night_gain`) between 16:43 and 17:22, every dawn ended in day, and I found no
day -> night -> day flapping that was not a real light going on and off (the
kinder cameras, both seeing the same events). What it still costs:

| cost on the 9 living-area cameras, 4.5 days | count |
| --- | --- |
| successful day checks | 80 |
| failed day checks (2 clicks, 8-18 s colour picture in the dark), boot checks excluded | 74 |
| looks executed (8 s IR off) | 488 |
| looks skipped by the skip test | 519 |

Against the dawn report of 11-07: looks per camera and morning fell from 10 to
3.2-5.4, failed checks per morning from 1.9 to 0.84 (section 4).

Findings, ranked by benefit against risk:

1. **A look whose dark reading is at or above `day_gain` still asks for a
   check; none of the 107 such checks in the fleet gave day** (17 on the
   living-area cameras = 23 % of their failed checks; 90 on the test cameras).
   The bar comes from `2 x day_gain` ("nothing failed yet"), from the F list,
   or from a bar learned as `0.85 x N` after `B = 0`. Proposal: cap the look
   bar at `1.3 x day_gain / min(K, 1)` (13 of the 17 and 58 of the 90). The
   replay corpus has two k 0.8 profiles that rule out a cap at 1.0, and 1.3
   costs `dawn_ir-sz` one failed check; 1.5 passes everything I ran but only
   takes 4 and 32. Not fixed in the working tree.
2. **A check judges with no reading (`exposure -1`) after an ISP readback
   cycle and returns "day"** (cam-vanhua-t55a, v1.12.0, twice). One-line guard.
   Not fixed in the working tree.
3. **After a restart at night `B` stays 0 for the whole night** when every
   look is railed: the skip test is off (`B != 0.0f`), and the first unrailed
   look at dawn learns a bar unrelated to `day_gain` (cam-sz 11-12: asks at
   dark 9197 with `day_gain` 3000, check 26608). Not fixed in the working
   tree; finding 1's cap removes the failed check, a second line the blinks.
4. **Colour-on-darker looks: 91 on the living-area cameras, none asked for a
   check.** 68 are removed by the skip test that a7cba61 and the working tree
   apply (confirmed in the field on cam-sz after its update: 9 of 9 skipped).
   23 remain on cameras whose IR-lit level sits below `day_gain`
   (cam-kinder-*, cam-wohn-ofen); no proposal I would stand behind.
5. **The first check of almost every dawn fails on six cameras** (wohn,
   wohn-ofen, sz, vorne, schuppen, wintergarten: 28 of 30 mornings). K is clamped at 2.0
   while these cameras measure k 2.1-3.3 at dawn every day. Proposal: a k kept
   for creeping looks only; what-if removes 20 of 38 morning failed checks,
   defers 3 day switches by one dawn step. Medium risk, needs a replay family.
6. **cam-wohn-ofen in the evening: 22 failed checks in four evenings (10 on
   11-10).** The day pipeline reads a constant ~5300-5600 (between `day_gain`
   3500 and `night_gain` 8000) while the dark reading moves 400-1700 with the
   stove. This is the known `fire` case; the lockout works as designed (never
   more than 4/h) but the evening still clicks. No low-risk engine fix found;
   options in section 3.6.
7. **Desync re-switch repeats every ~33 s** on cam-secuplug (test build
   full1112-a7cba61: 7 times in 6 min) because `ds_done` is cleared by the
   first tick on which the ISP agrees. Proposal: back-off. Low priority (test
   camera, cause of the desync is outside timps).
8. **55 of the 74 failed checks were judged at 8-13 s, where the smoother
   still carries 34 % of the first sample after the switch.** The data do not
   prove that waiting would have changed a verdict (section 3.8); needs one
   trace before any change.

## 1. Method

Each log was reduced to events (switch, check, look, skip, reference move,
warning, boot). Scripts and reduced logs are in
`scratchpad/fable-dn/` (`st.py`, `chk.py`, `over.py`, `cap.py`, `kc.py`,
`rep.py`, `*.s`). A look -> check pair is the `look ... -> check` line and the
next `check (...)` line; "fast" means the verdict came <= 13 s after the
switch, "slow" > 13 s (the 10 s extra wait of `SD_DAY_HOLD_MS`).

`day_gain` per camera is read from the `check: day, exposure X < day_gain`
lines: wohn 768, wohn-ofen 3500, sz 3000, kinder-links 1500, kinder-rechts
1200, vorne 768, schuppen 1500, db 768, wintergarten 768.

## 2. Living-area cameras, per camera

First day switch (non-boot check that found day, 05-13 h) and first night
switch after 14 h (`switching to night (exposure N)`):

| camera | day 11-10 | 11-11 | 11-12 | 11-13 | 11-14 | night 11-10 | 11-11 | 11-12 | 11-13 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| wohn | 08:13 | 09:47 | 09:05 | 08:51 | 08:36 | 17:11 (4170) | 17:22 (6762) | 23:27 (7555) | 16:58 (4197) |
| wohn-ofen | 08:34 | 11:17 | 08:59 | 08:51 | 08:57 | 16:46 (8139) | 15:57 (8267) | 16:45 (10644) | 16:55 (8220) |
| sz | 07:55 | 08:05 | 08:09 | 07:47 | 08:10 | 16:56 (6144) | 20:38 (32662) | restarts | 17:00 (12607) |
| kinder-links | 07:36 | 05:21 | 07:27 | 08:09 | 07:44 | 17:03 (4500) | 16:54 (4237) | 17:00 (4249) | 16:52 (4390) |
| kinder-rechts | 07:36 | 05:21 | 07:27 | 08:11 | 07:42 | 17:05 (4190) | 17:01 (4162) | 17:06 (4170) | 17:07 (4160) |
| vorne | 07:38 | 07:48 | 07:45 | 07:41 | 07:51 | 17:16 (4466) | 17:06 (4238) | 17:09 (4385) | 17:10 (4238) |
| schuppen | 09:12 | 08:53 | 09:19 | 08:27 | 09:03 | 16:43 (4149) | 16:00 (4176) | 15:39 (4356) | 16:32 (4114) |
| db | 08:04 | 09:00 | 08:13 | 08:09 | 08:19 | 16:58 (4170) | 16:51 (4226) | 16:57 (4182) | 16:46 (4126) |
| wintergarten | 07:34 | 07:45 | 07:41 | 07:40 | 07:40 | 17:19 (4187) | 17:11 (4150) | 17:15 (4173) | 17:16 (4182) |

Reading: the dusk switches are consistent between cameras and days and happen
just above `night_gain`; nothing early or late. 11-11 was a dark morning on
every indoor camera (wohn 09:47, db 09:00, wohn-ofen 11:17), so those late
days are weather, not the engine. The 05:21 on both kinder cameras is a lamp.

Per morning (04-13 h, mean over 5 mornings) and over the whole window:

| camera | looks / morning | skipped / morning | failed checks / morning | failed checks total | of those fast (<= 13 s) | day checks total |
| --- | --- | --- | --- | --- | --- | --- |
| wohn | 4.2 | 11.0 | 1.2 | 8 | 5 | 5 |
| wohn-ofen | 5.4 | 3.2 | 1.4 | 29 | 20 | 7 |
| sz | 4.8 | 4.6 | 1.0 | 10 | 10 | 5 |
| kinder-links | 3.6 | 0 | 0.4 | 3 | 0 | 20 |
| kinder-rechts | 3.6 | 0 | 0.2 | 5 | 4 | 21 |
| vorne | 3.6 | 12.8 | 1.0 | 5 | 5 | 5 |
| schuppen | 3.6 | 8.4 | 1.0 | 7 | 5 | 7 |
| db | 4.4 | 5.4 | 0.4 | 2 | 1 | 5 |
| wintergarten | 3.2 | 8.0 | 1.0 | 5 | 5 | 5 |

Looks by reason and what they led to (whole window):

| camera | jump: night / check day / check night | near: day / night | colour (plain): night / check | colour-on-darker: night | timer |
| --- | --- | --- | --- | --- | --- |
| wohn | 5 / 2 / 3 | 3 / 4 | 2 / 1 (night) | 15 | 0 |
| wohn-ofen | 59 / 3 / 24 | 4 / 4 | 23 / 1 (night) | 15 | 0 |
| sz | 16 / 3 / 8 | 2 / 2 | 3 / 0 | 7 | 20 (17 after restarts) |
| kinder-links | 13 / 18 / 1 | 2 / 1 | 9 / 1 (night) | 6 | 4 |
| kinder-rechts | 7 / 19 / 5 | 1 / 0 | 18 / 1 (day) | 8 | 4 |
| vorne | 5 / 2 / 3 | 3 / 2 | 0 / 0 | 16 | 4 |
| schuppen | 14 / 5 / 4 | 1 / 3 | 9 / 1 (day) | 19 | 2 |
| db | 7 / 3 / 0 | 2 / 2 | 3 / 0 | 1 | 9 |
| wintergarten | 6 / 3 / 2 | 2 / 3 | 0 / 0 | 4 | 0 |

Colour looks in total: 163, of which 2 ended in a (correct) day switch
(kinder-rechts 11-13 18:56:49 `check (color): day, exposure 923 < 1200`,
schuppen 11-12 16:35:59 `check (color): day, exposure 1357 < 1500`).

Notes per camera:

- **wohn**: clean nights; in 21-06 h only the 9 colour-on-darker looks ran
  (62 looks skipped). Every morning has one failed check before day (two on
  11-14: 08:21:59 `check (near): night, exposure 882`, 08:27:48 `... 805`,
  day 08:36:16 at 702). One failed check from a learned bar on 11-13 20:56
  (finding 1). One lamp check 11-13 19:55:10 (dark 380 -> 3793): legitimate
  ask, the lamp reads k 10.
- **wohn-ofen**: section 3.6.
- **sz**: 69 orderly restarts (`shutting down` logged) on 11-11 11:37-15:26
  and 11-12 01:49-03:30 / 10:05-10:39: not field behaviour. At night each
  restart is a boot check into the dark (`check (boot): night, exposure
  46336`, 17 times) plus a timer look 38 s later that reads the rail (dark
  41772) and learns nothing (finding 3). Recurring dim light: lit 2717-2763
  against a reference of 4451-4701, dark 3952-4315, seen 13 times at night
  (11-11 21:40, 11-12 00:11, 18:41, 19:33, 21:04, 23:22, 11-13 04:15, 07:17,
  18:43, 19:36, 21:37, 22:25, 11-14 00:40); five of them asked for a check at
  dark > `day_gain` with bar 6000 and failed (finding 1), the other eight
  were looks that ended night.
- **kinder-links / kinder-rechts**: 5-7 day switches per day, all lamps; both
  cameras agree to the second (e.g. 11-11 00:25:38 day on both, night 34 s
  later; 11-13 21:36:56 / 21:36:55). Day for 22-46 s happened 6 times
  (kinder-rechts 11-11 20:20:24 46 s, 20:36:18 26 s, 11-12 19:02:31 22 s):
  the light was on for about a minute; the engine needs ~46 s to follow it
  (20 s jump hold, 8 s look, 18 s check) and then 6 s to leave. Four clicks for
  one minute of light is the design (`SD_JUMP_HOLD_MS` 20 s), not a fault;
  whether a light must hold longer than 20 s before the camera follows is a
  product decision.
- **vorne**: one boot (v1.12.0, 11-12 18:54:46 `check (boot): night, exposure
  8192`). An outdoor light goes on and off in the evening: the jump look is
  skipped (`look (jump) skipped: lit 4385, bar 1536`), the light goes off, the
  reference follows and `colour moved 41.1 % while the scene got darker`
  starts a look at lit 5864, dark 63165 (11-12 19:23:54; same at 20:18:19,
  20:55:06, 11-13 18:40:47, 20:18:18, 21:11:00). Fixed by the skip test in
  a7cba61 and the working tree.
- **schuppen**: on 11-12 18:02-23:14 a recurring light (lit 1440-2070, dark
  2080-3044 against `day_gain` 1500) caused 10 looks and 2 failed checks
  (18:02:30 `exposure 2896` from dark 2377 at bar 3000; 23:14:48 `exposure
  2461` from dark 2080 at bar 2139): finding 1. 11-11 16:00 night, 16:18 day
  (`check (jump): day, exposure 1172`), 16:24 night at 8167: a light in the
  shed for five minutes, followed correctly as far as the log shows.
- **db**, **wintergarten**: one failed check on most mornings, otherwise
  nothing to report.

## 3. Findings

### 3.1 Checks asked with the dark reading at or above `day_gain` (all versions)

The look reading (night pipeline, IR off) and the check reading (day pipeline)
see the same light; the check reads k times the look. A check asked at
`dark >= day_gain` gives day only if k < 1.

| dark reading of the look that asked | living-area: night / day | test cameras: night / day |
| --- | --- | --- |
| `dark >= day_gain` | 17 / 0 | 90 / 0 |
| `day_gain / K <= dark < day_gain` (K as logged) | 10 / 4 | 52 / 4 |
| `dark < day_gain / K` | 47 / 76 | 478 / 775 |

Where the bar came from in the 17 living-area cases:

- `2 x day_gain` because K is 1.0 after a restart and nothing has failed:
  cam-sz 11-11 21:40:42 `look (jump): ... dark 4315 bar 6000 -> check`,
  `check (jump): night, exposure 5178`; again 11-12 07:42 (4374 -> 11927),
  18:41 (4080 -> 5051), 21:04 (4156 -> 32267), 23:22 (4040 -> 5027), 11-13
  18:43 (4075 -> 5036, on a7cba61); cam-schuppen 11-12 18:02 (2377 -> 2896).
- `0.9 x` an older failed dark reading (F list): cam-vorne 11-10 07:23:45
  `dark 1147 bar 1278` -> 2683, 11-11 07:40:59 `dark 922 bar 1032` -> 1953,
  11-14 07:28:06 `dark 1000 bar 1132` -> 3564; cam-wintergarten 11-10 07:24:41
  `dark 920 bar 960` -> 2320; cam-sz 11-13 07:31 (3086, bar 3636 -> 4542),
  11-14 07:38 (3118, bar 3668 -> 12686); cam-schuppen 11-12 23:14.
- A bar learned as `0.85 x N` after `B = 0`: cam-wohn 11-13 20:46:29 `look
  (color): lit 1275 dark 1541 bar 1310 -> night` (the learning look), then
  20:56:12 `look (color): lit 1100 dark 1294 bar 1310 -> check`, `check
  (color): night, exposure 2242` with `day_gain` 768; cam-sz 11-12 07:35:02
  `look (near): lit 3522 dark 9197 bar 12000 -> check`, `check (near): night,
  exposure 26608` (bar = the `SD_NEAR` cap, 4 x 3000 / 1.0); cam-vorne 11-13
  07:28:45 `dark 1258 bar 1536` -> 2956.

The same rule on the test cameras, e.g. cam-wyze-pan 11-13 (v1.12.0, cellar
with foreign IR): nine checks from dark 835-1381 at bar 1536, all `exposure
3687`.

**Proposal.** In `sd_look_bar()`, after the `SD_NEAR` clamp:

```c
float cap = SD_ASK_MAX * dn->day_gain / fminf(K, 1.0f);   /* SD_ASK_MAX 1.3 */
if (bar > cap) bar = cap;
```

The same bar feeds the skip test, so looks whose lit level is already above it
stop blinking as well.

Why not 1.0, which the field table suggests: the day pairs of this week do
contain k below 1. cam-wyze-pan has 34 of 113 day pairs at k 0.83-0.99 (4
below 0.9); elsewhere it is rare (pc420-1 8 of 111, wyze-cam2 4 of 73), and
the five pairs below 0.8 in the fleet all have a dark reading far under
`day_gain` (the light changed between look and check). So 0.8 is the floor
to plan for, and 1 / 0.8 = 1.25.

What-if on the logs (`cap.py`; a look is "newly skipped" if `lit x 0.9 >=`
the capped bar, a check "not asked" if `dark >=` the capped bar):

| variant | living-area: looks newly skipped | failed checks avoided (of 74) | day switches lost | test cameras: failed checks avoided (of 620) | day switches affected |
| --- | --- | --- | --- | --- | --- |
| cap 1.0 | 18 | 17 | 0 | 89 | 2 |
| cap 1.3 | 5 | 13 | 0 | 58 | 2 |
| cap 1.5 | 1 | 4 | 0 | 32 | 1 |
| cap 1.5 until the first pair, then 1.0 | 14 | 11 | 0 | 87 | 1 |

The affected day switches: cam-garage 11-13 11:42:55 `look (jump): lit 1235
dark 160` (the lit value is still falling when the jump fires; the look would
be skipped, the reference set to 1235 and the next jump 20-30 s later looks:
a delay, not a loss) and cam-jooan-a6m 11-12 23:02:46 (dark 50 at bar 50, a
rounding tie in my script).

Replay (my own sim builds from a scratch copy of the working tree; 30 pairs,
section 6), reruns of the sensitive pairs at low host load included:

| variant | result | failed checks, where they changed against base |
| --- | --- | --- |
| cap 1.0 | fails `dawn-schuppen` (2760 s to day, bound 1800 s) and `ref_noisy-pan` (599 s, bound 60 s), both reproducibly; `dawn_slow-schuppen` 2040 s (base 215 s) | fewer in 8 pairs |
| cap 1.0 once a pair exists (2 x before, as today) | fails `ref_noisy-pan` (599 s) reproducibly; everything else identical to base | none |
| cap 1.5 until the first pair, then 1.0 | fails `ref_noisy-pan` (599 s) reproducibly | fewer in 5 pairs |
| cap 1.3 | fails `dawn_ir-sz` on the click budget (9 switches, budget 7; 4 failed checks instead of 3), reproducibly; `dawn_field-wgarten` 520 s wrong mode (base 397 s, passes) | fewer in `dawn-wohn` 3 -> 2, `dawn-schuppen` 2 -> 1, `dawn_field-wohnw` 3 -> 2, `dawn_slow-wohn` 3 -> 2, `dawn_slow-schuppen` 3 -> 1, `dawn_slow-sz` 4 -> 3, `basement_k15-pan` 2 -> 1, `living_room_evening-wohn` 3 -> 2, `fire-ofen` 6 -> 5; more in `dawn_ir-sz` 3 -> 4 |
| cap 1.5 | passes the 13 pairs that moved under any other variant | fewer in `dawn-wohn` 3 -> 2, `living_room_evening-wohn` 3 -> 2, `fire-ofen` 6 -> 5, `dawn_slow-schuppen` 3 -> 2, `dawn_slow-sz` 4 -> 3 |

The two profiles that break under 1.0 are k 0.8 scenes: the shed at
`day_gain` 768 (the camera runs 1500 now) and cam-wyze-pan at `day_gain` 100
after a bulb with k above 1 has set K. A cap that depends on the learned K is
therefore not safe either: K is a median over lamps of different k.

My recommendation: 1.3, after looking at why `dawn_ir-sz` takes a fourth
failed check with it (1.0 and 1.5 both keep it at 3, so this looks like a
sequence effect in that timeline, not a trend), and after a full-corpus run.
If that family cannot be made to pass, 1.5 is the step without a replay
regression; it still removes the learned-bar cases above 1.5 x (cam-sz 9197)
and a third of what 1.0 would take on the test cameras.

Risk: a scene with k < 0.77 on a camera whose K is not below 1 waits until
the dark reading falls to `1.3 x day_gain` (a later day, never a missed one;
the guarantee check still exists). Families: `dawn-schuppen`, `ref_noisy-pan`,
`dawn_ir-sz`.

Working tree: not fixed.

### 3.2 A check with no reading returns "day" (v1.12.0; same code in the tree)

cam-vanhua-t55a (T41, v1.12.0), 11-11:

```
19:02:01 look (jump): lit 69 dark 64 bar 1536 k 1.00 -> check
19:02:01 switching to day (jump) ...
19:02:27 running_mode=1 never followed the switch to day
19:02:29 ISP did not follow the switch to day - one cycle
19:02:29 switching to night (isp readback) ... exp=32391
19:02:36 switching to day (isp readback) ... exp=22836
19:02:37 k 0.70 (pair 64 -> -1), jump bar 1536
19:02:37 check (jump): day, exposure -1 < 768
19:02:45 switching to night (exposure 63442)
```

The same at 19:27:02 (`check (jump): day, exposure -1 < 768`). The room read
63442 in the day pipeline.

Mechanism: the cycle block sets `s = -1.0f` after the readback switch and
falls through to the state machine in the same tick (the smoother update is
above it). `S_CHECK` has no `s <= 0` guard (`S_LOOK` has one: `if (now < due ||
s <= 0.0f) break;`), `due + DN_VERIFY_MS` has passed during the blocked hook
and the cycle, so it records the pair `-1 / 64` (clamped to K 0.70) and
`-1 < day_gain` is "day". Cost here: 8 s of day in the dark until the deep
path leaves, and K 0.70 in the history, which raises `day_gain / K` and with
it every later bar.

**Proposal.** In `S_CHECK`: `if (now < due || s <= 0.0f) break;`, and when the
cycle ends while `st == S_CHECK`, re-arm `due = now + DN_PROBE_SETTLE_S *
1000`. Expected effect: the check after a readback cycle reads the day
pipeline for a full settle before it judges. Risk: none I can see; without a
reading the engine already idles elsewhere. Family: `isp_stuck` with a check
running during the cycle (the current timeline may not hit it; a variant with
`isp_sticky` during a lamp check would).

In the same log the re-assert pushed the stale config value against the
engine: `re-asserting running_mode=1 after switch` twice while the engine was
checking in day (the hook had not updated `image.running_mode` yet). The
re-assert should be skipped when `(rm != 0) != (cur == DN_NIGHT)`; today it
only warns (`never followed`) after having POSTed the wrong mode. Seen again
on cam-secuplug 11-12 13:47:58 (`re-asserting running_mode=0 after switch (0
left)` while standing on night, followed 2 s later by `ISP did not follow the
switch to night - one cycle`). Low risk, small benefit.

### 3.3 `B = 0` for a whole night after a restart in the dark (all versions)

After a boot check that found night `B` is 0 ("learn at the next look"). The
learning look only learns when the dark reading is not clipped (`learn = ok &&
B == 0.0f`). In a dark room the look rails, so `B` stays 0 until dawn. While
`B == 0` the skip test is off (`B != 0.0f && s * SD_SKIP >= ...`).

- cam-vorne after the boot of 11-12 18:54 (v1.12.0): the first unrailed look
  is at dawn.
- cam-sz 11-12: 17 restarts 01:49-03:30, each followed by `look (timer): lit
  47xx dark 41772 bar 3000 k 1.00 -> night`; at 07:26:59 `look (color): lit
  4404 dark 24166 bar 12000` learns `0.85 x 24166`, clamped by `SD_NEAR` to
  12000; at 07:35:02 the near trigger asks at dark 9197 and the check reads
  26608.

**Proposal.** (a) Finding 1's cap bounds the learned bar. (b) In the skip
test treat `B == 0` like an unset bar instead of switching the test off:
`float b0 = B == 0.0f ? -1.0f : B;` and call `sd_look_bar()` with `b0` there.
(c) After a boot check that read above `SD_FAR x night_gain` or exhausted,
set `B = -1` and `look_at = now + look_ms` instead of `B = 0` and a look after
`DN_REF_DELAY_S`: that look cannot learn (cam-sz: 17 of 17 read the rail).
Risk of (c): after a restart under foreign IR the first look comes 45 min
(or 6 h with colour) later instead of 38 s; the jump and colour triggers still
work. Families: `boot_dark`, `boot_dark_from_day`, `lamp_after_boot`.

### 3.4 Colour-on-darker looks

`colour moved N % while the scene got darker` followed by a look, living-area
cameras: 91, outcome `-> night` 91, `-> check` 0. With the skip test (`lit x
0.9 >= bar`) 68 of them do not run. a7cba61 has that test, the working tree
has it in the common place; cam-sz after its update to a7cba61 logged 9 such
events and skipped 9.

The 23 that remain are on cam-wohn-ofen (7), cam-kinder-links (6),
cam-kinder-rechts (7) and three single ones: there the IR-lit level is below
`day_gain`, so the skip test never applies, e.g. cam-kinder-links 11-12
19:02:12 `look (jump): lit 680 dark 1459 bar 1014 -> night`, then the light
goes off and 19:03:23 `look (color): lit 1162 dark 5787 bar 1500 -> night`
(same pair at 19:25/19:26 and 20:11/20:12). The colour moved because the lamp
went off. Telling "lamp off" from "lamp on while foreign IR went off" (the
case this look exists for, cam-wyze-pan) would need the colour of the pure-IR
scene as a second reference; I do not propose that on 23 looks in 4.5 days.

On the test cameras the same trigger did lead to checks (cam-wyze-pan 2,
ing-wyze-cam2 8, cam-secuplug 3, cam-cinnado-t41 2), so it is doing its job
there.

### 3.5 The first check of the dawn (all versions)

Morning pairs (look dark -> check exposure, seconds from switch to verdict):

| camera | failed first check, per morning | day check, per morning |
| --- | --- | --- |
| vorne | 1147 -> 2683 (9 s), 922 -> 1953, 760 -> 1736, 1258 -> 2956, 1000 -> 3564 | 302 -> 560, 292 -> 767, 309 -> 639, 306 -> 641, 202 -> 361 |
| wintergarten | 920 -> 2320, 743 -> 1695, 586 -> 1518, 485 -> 1287, 423 -> 1157 | 279 -> 573, 279 -> 646, 261 -> 498, 263 -> 571, 249 -> 604 |
| wohn | 334 -> 820, 364 -> 1238, 356 -> 1250, 359 -> 1434, 356 -> 882 | 293 -> 748, 204 -> 673, 208 -> 544, 171 -> 511, 235 -> 702 |

wintergarten shows the F list at work: the asking level falls 920, 743, 586,
485, 423 over five mornings, one failed check each, toward the ~280 where day
actually comes. wohn asks at ~360 every morning because the floor
`day_gain / K` with K clamped at 2.00 is 384, and its day comes at 171-293.
The engine logs `k 2.00` on these cameras all week while the slow (18 s)
pairs read k 2.46-3.30 (wohn), 1.79-2.63 (vorne), 1.91-2.43 (wintergarten),
2.31-3.7 (wohn-ofen, daylight pairs).

My report of 11-07 advised against a learned prior k because the low-k rooms
(kinder, db: LED lamps, k 1.0-1.5) would get later days. That objection is to
one k for lamps and daylight alike. A k kept for creeping looks only does not
have it: lamps are steps (`sd_stepped`), dawn is a creep.

**Proposal.** A second median `Kc` over the last three pairs whose look had
`creep` set, clamped to 0.7 .. 4.0 (not 2.0), reset with the thread; in
`sd_look_bar()` for `creep` looks: `bar = min(bar, 1.15 x day_gain / Kc)` once
two creep pairs exist. `K`, `SD_K_MAX` and the lamp branch stay as they are.

What-if (`kc.py`; the log does not record `creep`, so all 05-12 h trigger
checks stand in for it, which over-counts on the kinder cameras where morning
lamps are steps):

| margin | morning failed checks avoided (of 38) | day checks that would have been deferred (of 51) |
| --- | --- | --- |
| 1.15 | 20 | 5 (2 of them kinder lamps = steps, not affected in the real rule) |
| 1.30 | 15 | 3 |

The three real deferrals at 1.15: cam-wohn-ofen 11-12 08:59 (dark 1155, bar
1144), cam-sz 11-11 08:05 (1397, bar 1197), cam-schuppen 11-11 08:53 (631, bar
528). A deferred look asks again when the level has crept below the bar; at
the dawn rates in these logs (cam-wohn: lit 2739 -> 1315 in 13 min) that is a
few minutes.

Risk: medium. k drifts within a dawn in both directions (11-07 report), so a
`Kc` from yesterday's last pair can be 30 % high; the margin covers half of
that. A wrong creep classification puts a lamp under the creep bar. Not
replayed by me; families `dawn_field`, `dawn_trace`, `dawn_slow`, `dawn_ir`
cover it, and a new one is needed that runs two dawns in one thread lifetime
(the memory only pays from the second morning).

Working tree: not addressed.

### 3.6 cam-wohn-ofen, evenings

Failed jump/near/colour checks between 15 h and 01 h: 11-10 ten (18:14:00,
18:34:05, 18:40:52, 19:43:18, 20:35:27, 20:37:50, 20:42:17, 21:09:36,
21:45:11, 23:47:43), 11-11 four (18:57:26, 18:58:47, 19:48:53, 23:58:03),
11-12 seven (18:53:24, 18:54:41, 19:15:20, 20:18:28, 21:14:36, 23:20:45 and
00:39:46 after midnight), 11-13 one (19:54:00). For example:

```
10 20:35:18 look (jump): lit 1195 dark 1575 bar 1750 -> check   check: night, exposure 5321
10 20:37:35 look (jump): lit  658 dark  732 bar 1036 -> check   check: night, exposure 5328
10 20:41:58 look (jump): lit  372 dark  407 bar  481 -> check   check: night, exposure 5225
```

12 of the 20 fast failed checks on this camera read 5321-5587 for dark
readings between 417 and 1701 (and the slow one at 20:42:17 reads 5225 from
dark 407). The day pipeline does not move with the dark
reading: the light that moves the look is the stove (the `fire` profile,
"k 11-38"), and the room's visible level sits at ~5300-5600, inside the
band `day_gain` 3500 .. `night_gain` 8000. When a lamp is really on the check
passes: 11-13 19:55:08 `look (jump): lit 360 dark 363`, `check (jump): day,
exposure 3107 < 3500`, day until 23:05.

The engine behaves as designed: `Kf` from the last failed check predicts the
next one, the fire's k rises with every step (3.4, 7.3, 12.8 in the three
lines above), so each prediction is too low, and the lockout (1, 2, 4, 8, 16
min, "a jump waits out a lockout") lets the next step ask the moment it ends
(20:37:35 is 8 s after the 2 min lockout, 20:41:58 is 8 s after the 4 min
one).

Options, none of which I can recommend without a trial:

- Engine, small: after two failed checks in a row within 10 min whose
  exposures agree within 15 % while their dark readings differ by more than
  30 %, set the lockout to `SD_LOCK_MAX_MS` at once. In these logs that saves
  11-10 20:42 and at most one or two per evening. It delays a real lamp by up
  to 16 min in that state. Family: `fire-ofen` (max_switches 12 today).
- Engine, larger: treat "same check exposure, different dark reading" as
  proof that the look does not predict the check and stop jump asks for the
  `Kf` window (1 h). On 11-10 that turns 10 failed checks into 8. A lamp in
  that hour is only seen by the colour trigger or the timer. I would not do
  this.
- Configuration (the user's call): the room's evening level of ~5300-5600 is
  1.5-1.6 x `day_gain`. With `day_gain` around 6000 those checks would stay
  in day, i.e. a colour picture at that exposure for the evening, and the band
  to `night_gain` 8000 becomes narrow. That is a picture-quality decision, not
  a fix.

What is fixed by 3.1: nothing here (all dark readings are below `day_gain`).
What a7cba61/the tree fix here: 8 of the 15 colour-on-darker looks.

### 3.7 Desync re-switch loop (test build full1112-a7cba61, cam-secuplug)

```
12 13:47:42 ISP is in day while auto day/night stands on night - switching back
12 13:49:01 ISP is in day while ... - switching back
12 13:50:12 ... 13:51:33 ... 13:52:06 ... 13:52:39 ... 13:53:12 (restart 13:53:30)
```

Seven board switches in 5.5 min, 33 s apart at the end. `ds_done` is cleared
on the first tick with `sm.isp == cur`; the ISP follows the re-switch, falls
back to day about 20 s later (why is not in the log: the board script failed
on this build, `'daynight night' failed (rc=1)` 7 times that afternoon, and
another session was testing the T10 driver), and the 10 s timer starts again.
On v1.12.1-7-ga7cba61 the same camera shows a single re-switch 1.5-2.5 min
after each boot (19:49:28, 19:54:37, 19:59:45, 20:04:54), each followed by a
scripted restart, so it never got to repeat.

The working tree keeps the same reset (`if (sm.isp == cur) ds_at = ds_done =
0;`), so "once" still means once per episode.

**Proposal.** Keep a count: double the wait after each re-switch (10 s, 20 s,
... capped at `SD_LOCK_MAX_MS`), and clear the count only after the ISP has
agreed for 60 s. Effect: at most ~6 re-switches in the first 10 min and one
per 16 min after that. Risk: a second hand-switch within a minute is corrected
later than the first. Family: `hand_switch` with the hand switch repeated
every 30 s.

Not enough data to say whether timps contributes to the fallback on this
build; a test camera under driver test.

### 3.8 When the check reads (all versions): not proven

Facts from the code: at the switch to day `s` is reset and then smoothed with
alpha 0.30 from the first sample on; at `due` (8 s, four samples at the 2 s
interval) the first sample after the switch still has weight 0.7^3 = 34 %. A
reading of at least `SD_NEAR_MISS x day_gain` (1.5) is final at once; anything
lower waits 10 s more. So every night verdict above 1.5 x `day_gain` is the
8 s value.

Facts from the logs: 55 of the 74 failed checks on the living-area cameras
were fast. 28 of the 74 read between 1.5 and 2.0 x `day_gain` (wohn-ofen 14,
sz 5, wohn 3, schuppen 3, wintergarten 3). Pairs at similar dark readings on
the same camera differ by the timing:

| camera | fast | slow |
| --- | --- | --- |
| sz | 11-10 07:41 dark 1848 -> 5529 (9 s, night) | 11-13 07:47 dark 1876 -> 2765 (19 s, day) |
| wohn | 11-13 08:09 dark 359 -> 1434 (8 s) | 11-14 08:21 dark 356 -> 882 (19 s) |
| wintergarten | k 2.28-2.74 (five mornings, 8-9 s) | k 1.91-2.43 (same mornings, 18-19 s, 5-10 min later) |

My 11-07 report has one measured case (schuppen: 1157 at 8 s, 867 at 18 s).

Why this is not a finding with a fix yet: k also drifts within a dawn by the
same 1.2-1.4 x (11-07 report), the fast and the slow pair of one morning are
5-40 min apart, and the cam-wohn-ofen values that looked like a transient
(a constant ~5400) turned out to be the room (3.6; the 19 s check 11-10
20:42:17 reads 5225 as well). I replayed "every unrailed check waits the full
18 s" (section 6): no family improved, `stale_check-eufy` got one more failed
check, and every truly failed check shows colour in the dark for 10 s longer.

**Proposal.** Do not change it now. Record one dawn with `daynight.trace` on
cam-wintergarten or cam-wohn and one evening on cam-wohn-ofen and look at the
2 s samples of the day pipeline in the 30 s after each check. If the 8 s value
is above the 18 s value by more than 20 %, discard the samples of the first
half of the settle (`s = -1.0f` once at `due - 4 s`) rather than waiting
longer; that costs no time.

## 4. What of the 11-07 dawn report shows in the new data

| item of 11-07 | then (125 mornings) | now (45 mornings, 9 cameras) |
| --- | --- | --- |
| looks per camera and morning | 10 (8.4-16.3 on seven cameras) | 3.2-5.4 |
| failed checks per camera and morning | 1.9 | 0.84 (0.2-1.4) |
| skip-look for jump looks (3.1) | proposed | 0-12.8 skips per morning; no skipped level was later contradicted in the logs I can check |
| skip-look for colour looks (3.1) | proposed | works for plain colour looks; the colour-on-darker path bypassed it until a7cba61 (finding 3.4) |
| near trigger (3.3) | proposed after cam-wohn lost 60 min | 41 near looks asked: 20 day, 21 night. It brings day within 4-14 min of the first failed check on wohn 11-10 and 11-14; the failed ones are mostly the first check of the morning asked at a bar that was too high (3.1, 3.5) |
| near-miss check waits for the AE (3.6) | proposed | `SD_NEAR_MISS` 1.5 covers the checks below 1.5 x; 28 failed checks read 1.5-2.0 x (3.8) |
| "first failed check of the morning is the price of measuring k" | 107 of 238 | still there: 28 of 30 mornings on six cameras (3.5); 8 of them are asked at or above `day_gain` and need no k at all (3.1) |
| "no prior k" | advised against | revised for creeping looks only (3.5) |
| cam-wohn day lost after a near miss | 60 min on 11-07 | not seen: 4, 50, 40, 42, 14 min from the first failed check to day; the long ones follow a check that read k 3.4-4.0, and the level then really had to fall to 171-208 |

## 5. Considered and rejected

- **"Same lit level as a look that just ended night: skip the look."** It
  would remove the repeats on cam-sz (lit ~2720, eight looks), cam-schuppen
  11-12 evening and cam-wohn-ofen. What-if (`rep.py`, 10 % tolerance, 6 h):
  100 of 444 living-area trigger looks skipped, but 5 of them led to day
  (kinder lamps at lit ~605, where an earlier look at lit 645-688 had read
  dark 1300-1460); on the test cameras 80 day switches among 380. At 3 %
  tolerance still 1 and 47. The lit level under IR does not predict the dark
  reading; the same conclusion as the additive IR model on 11-07.
- **Capping the bar at exactly `day_gain`**, always or once a pair exists:
  fails the k 0.8 profiles `dawn-schuppen` and `ref_noisy-pan` (3.1).
- **Waiting 18 s for every check**: 3.8.
- **A longer jump hold for short lights (kinder)**: it would trade four
  clicks per one-minute light for a later day under every lamp. Product
  decision, no evidence either way in the logs.

## 6. Replay runs

Built from a scratch copy of the working tree (`scratchpad/fable-dn/tree`),
`make sim USE_CONTROL=1 SIM_CFLAGS="-DMS_CLOCK_SCALE=60"`, variants: base
(unchanged tree), four bar caps (3.1), and "wait" (every unrailed check waits
18 s, 3.8). 30 pairs each; the first round ran five binaries in parallel while
another corpus run was active on the host (load average 17), so the sensitive
pairs were rerun at low load.

Columns: switches / failed checks / looks / longest wrong-mode run in s.

| pair | base | cap 1.0 | cap 1.3 | cap 1.5 | cap 1.5 then 1.0 | wait |
| --- | --- | --- | --- | --- | --- | --- |
| basement_k15-pan | 6 / 2 / 3 / 32 | 4 / 1 / 2 / 32 | 4 / 1 / 2 / 30 | 6 / 2 / 3 / 31 | 6 / 2 / 3 / 31 | 6 / 2 / 3 / 33 |
| dawn-schuppen | 5 / 2 / 5 / 42 | **FAIL** 2 / 1 / 2 / 2760 | 3 / 1 / 2 / 33 | 5 / 2 / 4 / 95 | 5 / 2 / 4 / 96 | 5 / 2 / 5 / 43 |
| dawn-wohn | 7 / 3 / 6 / 130 | 5 / 2 / 4 / 69 | 5 / 2 / 4 / 4 | 5 / 2 / 5 / 51 | 5 / 2 / 5 / 50 | 7 / 3 / 6 / 123 |
| dawn_field-wgarten | 5 / 2 / 5 / 397 | 5 / 2 / 4 / 398 | 5 / 2 / 5 / 521 | 5 / 2 / 5 / 521 | 5 / 2 / 5 / 520 | 5 / 2 / 5 / 397 |
| dawn_field-wohnw | 7 / 3 / 6 / 95 | 5 / 2 / 4 / 92 | 5 / 2 / 4 / 93 | 7 / 3 / 5 / 96 | 7 / 3 / 5 / 96 | 7 / 3 / 6 / 95 |
| dawn_ir-sz | 7 / 3 / 8 / 331 | 7 / 3 / 7 / 98 | **FAIL** 9 / 4 / 9 / 59 (budget 7) | 7 / 3 / 7 / 97 | 7 / 3 / 7 / 97 | 7 / 3 / 8 / 332 |
| dawn_slow-schuppen | 7 / 3 / 5 / 215 | 2 / 1 / 1 / 2040 | 3 / 1 / 2 / 212 | 5 / 2 / 4 / 93 | 5 / 2 / 4 / 92 | 7 / 3 / 5 / 216 |
| dawn_slow-sz | 8 / 4 / 6 / 8 | 6 / 3 / 4 / 0 | 6 / 3 / 4 / 0 | 6 / 3 / 4 / 8 | 8 / 4 / 5 / 8 | 8 / 4 / 6 / 18 |
| dawn_slow-vorne | 5 / 2 / 4 / 1033 | 5 / 2 / 5 / 277 | 5 / 2 / 5 / 515 | 5 / 2 / 4 / 754 | 5 / 2 / 5 / 696 | 5 / 2 / 4 / 1042 |
| dawn_slow-wohn | 7 / 3 / 7 / 127 | 5 / 2 / 5 / 91 | 5 / 2 / 6 / 41 | 7 / 3 / 6 / 127 | 7 / 3 / 6 / 128 | 7 / 3 / 7 / 127 |
| dawn_trace-wohn1107 | 9 / 4 / 7 / 249 | 9 / 4 / 5 / 249 | 9 / 4 / 7 / 243 | not run | 9 / 4 / 6 / 249 | 9 / 4 / 7 / 249 |
| fire-ofen | 12 / 6 / 9 / 8 | 10 / 5 / 10 / 8 | 10 / 5 / 10 / 8 | 10 / 5 / 10 / 8 | 10 / 5 / 9 / 8 | 12 / 6 / 9 / 18 |
| living_room_evening-wohn | 8 / 3 / 4 / 33 | 6 / 2 / 3 / 32 | 6 / 2 / 3 / 32 | 6 / 2 / 4 / 33 | 6 / 2 / 4 / 31 | 8 / 3 / 5 / 31 |
| ref_noisy-pan (run alone) | 5 / 2 / 3 / 34 | **FAIL** 2 / 1 / 1 / 599 | 3 / 1 / 3 / 33 | 5 / 2 / 3 / 33 | **FAIL** 4 / 2 / 2 / 599 | 5 / 2 / 3 / 36 |
| stale_check-eufy | 4 / 2 / 3 / 16 | 4 / 2 / 3 / 16 | 4 / 2 / 3 / 16 | not run | 4 / 2 / 3 / 16 | 6 / 3 / 3 / 18 |

Unchanged in all variants: `basement_foreign_ir-t10`, `dawn-t10`,
`dawn_trace-schuppen1107` (looks 3 -> 4 with cap 1.0 and 1.3),
`eufy_dark_bulb-eufy`, `ext_ir_cycle-sz`, `foreign_ir-pan`, `foreign_ir-t10`,
`lamp_again-eufy`, `lamp_jump-*`, `lock_cap-t10`, `plateau-*`.

Reruns at low load (base, cap 1.0, cap 1.3; `dawn-schuppen`, `ref_noisy-pan`,
`dawn_ir-sz`, `dawn_field-wgarten`) gave the same numbers as the table, so the
failures marked above are not noise. `ref_noisy-pan` is fragile on its own:
the unchanged base failed it once under load with 4 / 2 / 2 / 599 and passed
it three times when run alone; the "wait" value for it is from the loaded
round. The variant "cap 1.0 once a pair exists, 2 x before" equals base in
every pair except `ref_noisy-pan` (fails alone). "cap 1.5" was run on the 13
pairs that moved under another variant.

The sim has no model of an AE that is still moving at 8 s (except
`stale_check`), so the "wait" column says nothing about finding 3.8 beyond
"it does not help where the AE is modelled as settled".

No variant was run against the full corpus (another run was in progress);
that is the next step before 3.1 goes in.

## 7. Files

- Report: `dev_notes/FABLE-DN-LOG-REVIEW-2026-11-14.md` (this file).
- Scratch (not in the repository):
  `/tmp/claude-1000/-mnt-NVMe-git-timps/7bbfd4d2-d8c5-469b-b55e-1c86102f33ba/scratchpad/fable-dn/`
  with the reduced logs (`*.s`), the scripts, the patched `dn_capA.c` (1.0),
  `dn_capB.c` (1.3), `dn_capC.c` (1.5), `dn_capK.c`, `dn_capM.c`, `dn_wait.c`,
  the sim binaries and `res*_*.txt`.
- Nothing in `src/` was edited; no camera was contacted.
