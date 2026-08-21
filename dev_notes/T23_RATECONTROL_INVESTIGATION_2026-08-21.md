# T23 does not drop the bitrate in quiet scenes; T31 does

Reported from the field: two cameras in adjacent rooms, same sensor, same
settings, and only the T31 one gets cheap when nothing moves. Question: SoC
limitation or misconfiguration?

Answer: the rate controller on the classic SoCs targets a *rate*, not a
*quality*, and has no static-scene detection at all. Nothing in timps was
misconfigured. But three of its knobs were hardcoded, and one of them turned
out to be a usable lever — so the practical answer is "both, in different
proportions than expected".

## The two cameras

| | cam-kinder-links | cam-kinder-rechts |
|---|---|---|
| SoC | T23 (`cinnado_d1_t23n_sc2336_atbm6012bx`) | T31 (`cinnado_d1_t31l_sc2336_atbm6031`) |
| Sensor | sc2336 1920x1080 | sc2336 1920x1080 |

Encoder config identical on both: h264, 1920x1080, 25 fps, bitrate 2000,
rc_mode cbr, gop 50, max_gop 60, profile 2, qp 35, min_qp 20, max_qp 45. ISP
identical (sinter/temper/dpc/drc/defog all 128, max_again 160, max_dgain 80).
Light comparable at measurement time: T23 exposure 44 / total_gain 257 /
ae_luma 61, T31 exposure 55 / total_gain 256 / ae_luma 61. "The T23 scene is
just noisier" does not survive those numbers.

## Method

RTSP pull of ch0, `ffmpeg -c copy` into 5-second segments, so the figures are
what the encoder emitted, not what a decoder made of it. Values in kbit/s.

The mean is the least interesting number here. What separates the two SoCs is
the **spread within one level**: the series alternate between an I-frame-heavy
window and a lighter one (5 s windows over a 2 s GOP, so the IDR count
alternates 3/2), and scene response shows up as variation *inside* each of
those two levels.

## Measurements

    T31 cbr                : 1390 1086 1645  828  830 1495  734  788 1972  808  835 1387   mean 1149
    T23 cbr                : 2521 1695 2505 1671 2489 1673 2520 1683 2512 1676 2510 1645   mean 2091
    T23 vbr  quality_lvl=2 : 2048 1366 2049 1364 2041 1363 2052 1378 2058 1368 2052 1373   mean 1709
    T23 vbr  quality_lvl=5 : 1887 1089 1631 1091 1632 1091 1629 1089 1629 1090 1631 1092   mean 1381
    T23 vbr  quality_lvl=7 : 1547  988 1483  997 1483 1003 1481  990 1485  990 1483  990   mean 1243
    T23 smart quality_lvl=2: 2418 1372 2052 1376 2062 1376 2056 1369 2058 1373 2063 1370   mean 1745
    T23 smart quality_lvl=7: 1489 1484  991 1488  989 1485  990 1486  990 1487  991 1314   mean 1265
    T23 fixqp=42           :  462  242  341  200  295  199  300  189                       mean  278

Within-level spread:

| series | spread, high level | spread, low level |
|---|---|---|
| T31 cbr | 37.1% | 41.7% |
| T23 cbr | 1.3% | 3.0% |
| T23 vbr quality_lvl=2 | 0.8% | 1.1% |
| T23 vbr quality_lvl=7 | 4.4% | 1.5% |
| T23 smart quality_lvl=7 | 12.0% | 0.2% |
| T23 fixqp=42 | 30.1% | 46.7% |

## What that establishes

**Not a hardware limit.** At a fixed qp the same T23 encodes the same scene at
278 kbit/s, a seventh of what cbr delivers, and the output varies by 30-47%
with scene content. The encoder can do it and the scene does change.

**The controller flattens it.** Under cbr, vbr and smart the within-level
spread collapses to 0.2-4%. The rate controller is holding a rate and erasing
the variation the sensor delivers. T31's controller does not — it keeps 37-42%.

**`smart` is not a static-scene mode**, at least not here. It tracks `vbr`
almost exactly (1745/1265 against 1709/1243 at quality_lvl 2/7) with the same
flattened spread. The T23 header documents it as nothing more than "Smart
method".

## Hypotheses that measurement refuted

Both were derived cleanly from the SDK header and both are wrong. Worth
recording, because the header reads convincingly in each case.

**The `qualityLvl` floor.** The header states
`minBitRate = maxBitRate * quality[qualityLvl]` with
`quality[] = {0.8, 0.7, 0.6, 0.5, 0.4, 0.3, 0.2, 0.1}`, so the hardcoded 2
implies a floor at 60% of 2000 = 1200 kbit/s — and the low windows sat at
1365-1377, close enough to look like proof. It isn't: at quality_lvl 7 the
floor would be 200, yet the low windows only reach ~990. The floor is not
binding at any setting we measured. `quality_lvl` moves the operating point
(1709 -> 1243, -27%), but not by lifting a floor.

An earlier proportionality check did fit the floor reading — dropping the
target from 2000 to 500 under vbr moved the mean 1715 -> 536 — but that only
shows the controller tracks `maxBitRate`, which every hypothesis predicts.

**The `changePos` threshold.** The header says qp is adjusted "when bitrate
exceeds changepos*maxBitRate", suggesting the controller has no reason to save
below 80% of target and that lowering the threshold would unlock further
reduction. Measured at quality_lvl 7: 80 -> 1264, 65 -> 1264, 50 -> 1251. The
knob does nothing on this SoC.

## Still unexplained

Why the rate settles near 990 kbit/s in the low windows and will not go lower,
although `max_qp` is 45 and qp 42 costs 278. Neither the qualityLvl floor nor
the changePos threshold accounts for it, and both `min_qp` (20) and `max_qp`
(45) have been at their defaults throughout — the controller picks an operating
point inside that span and we do not know by what rule.

Two things to do rather than form a third header-derived hypothesis:

1. Sweep `min_qp` (20 -> 30 -> 38). If the rate follows, the controller is
   quality-seeking and `min_qp` is the real lever on T23 — a key we have had
   all along.
2. Call `IMP_Encoder_GetChnAttrRcMode()` and look at what the encoder actually
   holds. timps calls none of the runtime rc APIs (no hits in `src/` for
   `SetChnAttrRcMode`, `GetChnAttrRcMode`, `SetChnInitQP`,
   `SetChnMaxPictureSize`), so we have never verified that our writes arrive
   unaltered.

## Change made

`8f3c84c` exposes `videoN.quality_lvl` (0..7), `videoN.change_pos` (50..100)
and `videoN.i_bias_lvl` (-3..3). They were literals in the VBR/CBR fills,
duplicated across the H264, H265 and T23 sw-rotate paths. Defaults are the
previous literals (2/80/0), so an untouched config is unchanged.

Two things had to be added that the field table alone did not cover:

- `control.c`'s video status JSON keeps its own hand-written field list in two
  duplicated branches. Without editing it the keys were writable and
  persistable but invisible in `/control` — verified against the simulator
  before the fix.
- The ENC_NEW_API SoCs have no equivalent fields, so they now warn once instead
  of accepting the keys and doing nothing. That is the failure mode that hid
  the `min_qp`/`max_qp` gap until `0a8bb9f`.

## Practical outcome

On T23, `rc_mode = vbr` with `quality_lvl = 7` delivers 1243 kbit/s against
2091 under the shipped cbr default — 41% saved. It is a lower operating point,
not scene adaptation: busy scenes pay the same reduced quality. Decoded frames
at the three settings differ by about 7% in PNG size (2.01 / 1.93 / 1.88 MB),
against 1.24 MB for the fixqp=42 frame, so the visible cost is far smaller than
the fixed-qp comparison would suggest. Left running on cam-kinder-links for
evaluation; not yet in any user overlay, so a flash reverts it.

## Open items

Tracked in the session task list; the encoder-related ones:

- Warn on the silent `smart` -> `capped_quality` substitution on the new API
  (`hal_ingenic.c:1123` falls through with no log, while the mirror-image
  substitution on the classic path deliberately warns once).
- Reword the new-API warning from `8f3c84c`: it claims the three keys cannot
  work there, but `IMP_Encoder_SetChnQpIPDelta` exists on T31 and C100 (absent
  on T40/T41), so `i_bias_lvl` is merely unwired.
- Wire `i_bias_lvl` on T31/C100 through that call.
- Expose `flucLvl` for H265 (range [0,4], the last classic-path literal with a
  documented domain).
- `frmQPStep` (3), `gopQPStep` (15), `gopRelation` (0) and `staticTime` (2)
  remain hardcoded. No documented ranges; deliberately left alone.
- `/control` swallows unknown keys when a POST also contains a valid one:
  `{"quality_lvl":7,"quality_level":5}` returns `ok:true, accepted:1`. Only an
  all-unknown POST returns 422. An `ignored:[...]` field would close that.
- On the new API `uMaxPSNR`, `uMaxBitRate`, `eRcOptions` and `uMaxPictureSize`
  are unreachable and their SDK defaults are not even readable. `uMaxPSNR` is
  the knob `capped_quality` is named after. Header-derived, unverified.
- `videoN.bitrate` is a hard ceiling on the classic path (`maxBitRate`) but only
  a target on the new one (`uTargetBitRate`, with `uMaxBitRate` left at the SDK
  default). Same key, different meaning per SoC.

## Settled in passing

The SDK headers disagree about `qualityLvl` under Smart: the en T23 1.1.2 and
1.3.0 Smart structs claim range 0..6, higher = better quality, while every zh
fassung and every VBR struct in every version say 0..7, lower = better, with
the minBitRate formula. The measurement decides it: under `smart`, quality_lvl
2 gives 1745 kbit/s and 7 gives 1265 — same direction as vbr. The en Smart text
is a translation error.

## Follow-up: min_qp is the actual lever (2026-08-21, later)

Swept `min_qp` on cam-kinder-links under vbr, quality_lvl held at the default
2 so min_qp was the only variable:

    min_qp=20: 2052 1379 2426 1358 2054 1370 2064 1389 2071 1386 2086 1287   mean 1743
    min_qp=30: 1000 1523 1398  941 1412  942 1406  936 1401  934 1401  882   mean 1181
    min_qp=38:  305  203  830  175  198  131  198  131  198  132  198  129   mean  235

That settles the "still unexplained" section above: the T23 controller is
quality-seeking, not rate-seeking. It picks the best quality `min_qp` allows
and the bitrate is a consequence, not a target. `min_qp=38` even undercuts the
fixqp=42 reference (278) - consistent with a controller that is free to go
finer than 38 in quiet passages under vbr, where fixqp cannot.

`min_qp` and `max_qp` have been configurable since before this investigation
started. The lever was always there.
