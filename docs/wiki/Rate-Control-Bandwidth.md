# Why T23 Needs More Bandwidth Than T31

A question that comes up when running the same encoder settings across a
mixed fleet: two cameras, same sensor, same `video0.*` config, and one of
them (a T31) drops to a fraction of its bitrate the moment the room goes
still, while the other (a T23) barely moves. Is the T23 just a worse chip?

No. The two SoC families run genuinely different rate controllers, and the
difference is by design, not a defect. This page gives the short version;
the full method and raw data are in
`dev_notes/T23_RATECONTROL_INVESTIGATION_2026-08-21.md` if you want to
verify it yourself.

## Two controller generations

[Platform & SDK Support](Platform-SDK-Support.md#two-encoder-api-generations)
already documents that `src/hal/hal_ingenic.c` builds against two different
Ingenic encoder APIs:

- **Classic** (T10, T20, T21, **T23**, T30) — a hand-assembled rate-control
  struct (`IMPEncoderChnAttr`). This controller is built to hold a **rate**:
  give it a bitrate and a mode (`cbr`/`vbr`/`smart`/...) and it adjusts QP to
  stay near that number, frame by frame, regardless of what is in the frame.
- **New generation** (**T31**, C100, T40, T41) —
  `IMP_Encoder_SetDefaultParam`-based configuration. Its `vbr`/`smart`/
  `capped_quality` modes are built to hold a **quality level** and let the
  bitrate float with scene content — a static scene costs little, a busy one
  costs much more, and the configured bitrate acts as a ceiling rather than a
  target.

That is the whole story in one sentence: **the classic controller targets a
rate; the new one reacts to what the sensor is looking at.** Everything
measured below is a consequence of that split, not of one chip being more
capable than the other.

## The comparison

Two cameras, same sensor (sc2336, 1920x1080), same encoder config (h264,
1080p, 25 fps, bitrate 2000, gop 50/60, profile high, qp 35, min_qp 20,
max_qp 45), same ISP tuning, comparable light at measurement time (AE
readings within a few percent of each other on both cameras). The only
difference is the SoC: T23 vs. T31.

RTSP was pulled and copied into 5-second segments (`ffmpeg -c copy`), so the
figures below are what each encoder actually emitted, not what a decoder
made of it afterwards.

### Why spread, not the mean

The mean bitrate is the least interesting number here, and quoting it alone
would hide the effect rather than show it. Each measurement window
alternates between an I-frame-heavy 5-second slice and a lighter one — a
side effect of a 5-second sampling window over a 2-second GOP, where the
number of IDR frames it contains alternates between 3 and 2. Any reaction
to scene content therefore shows up as *variation within one of those two
levels*, not as a change in the overall average. That is why the numbers
below are reported as within-level spread.

### Within-level spread, same scene, same config

| Series | Spread |
| --- | --- |
| T31, `cbr` | 37–42% |
| T23, `cbr` | 0.2–4% |
| T23, `vbr` (quality_lvl 2 or 7) | 0.2–4% |
| T23, `smart` (quality_lvl 2 or 7) | 0.2–4%, except one outlier window at 12% |
| T23, fixed QP (`fixqp 42`) | 30–47% |

T31 varies by roughly 40% between otherwise-identical windows of the same
level. T23 varies by low single digits — under **every** rate-control mode
it offers, `cbr`, `vbr`, and `smart` alike.

The fixed-QP row is what makes this a controller property rather than a
sensor or SoC property: put the *same T23* into `fixqp` and give up on rate
control entirely, and the *same scene* now produces a 30–47% swing — right
in T31's range. The chip sees the difference between a still room and a
moving one perfectly well. The classic rate controller is specifically
built to erase that difference and hold a number instead.

Mean bitrates for context: T31 `cbr` averaged 1149 kbit/s over this run,
T23 `cbr` averaged 2091 kbit/s — nearly double, at the same configured
target. That gap is a real, practical bandwidth cost, but it is downstream
of the spread finding above, not the cause of it.

## What you can actually do about it

You cannot make the T23's classic controller scene-adaptive — that
capability lives in the SDK generation the SoC uses, not in a setting.
What you *can* do is move its fixed operating point down.

Switching from `rc_mode = cbr` (the shipped default) to `rc_mode = vbr`
with `quality_lvl = 7` lowered the mean from 2091 to 1243 kbit/s on the
same scene — a 41% reduction. Read that carefully: **this is not scene
adaptation.** The within-level spread stays at low single digits at
`quality_lvl = 7` just as it did at the default — busy scenes and quiet
scenes both settle near the new, lower number. `quality_lvl` moves where
the whole series sits, uniformly; it does not make the T23 behave like a
T31. See [Rate Control Parameters](Rate-Control-Parameters.md) for what
each of the classic controller's knobs does and what it costs in image
quality.

## Two hypotheses that looked right and were not

Both of the following read convincingly from the Ingenic SDK header
comments, and both failed against measurement. They are recorded here so
nobody re-derives and re-believes them from the header a second time.

**"`quality_lvl` sets a hard bitrate floor."** The SDK header states
`minBitRate = maxBitRate * quality[quality_lvl]`, with the `quality[]` table
running from 0.8 down to 0.1 as `quality_lvl` goes from 0 to 7. For the
hardcoded default of 2 (0.6), that predicts a floor at 60% of the 2000
kbit/s target, i.e. 1200 kbit/s — and the measured low windows sat at
1363–1378, close enough to look like confirmation. It is not: at
`quality_lvl = 7` the same formula predicts a floor of 200 kbit/s, but the
measured low windows only ever reached about 990. The floor from the
header formula is never actually binding at any setting that was measured.

**"`change_pos` sets the threshold above which the controller starts
saving."** The header describes `change_pos` as the percentage of the
target bitrate above which QP gets adjusted, which reads as "lowering it
should unlock savings sooner." Measured at `quality_lvl = 7`: `change_pos`
80, 65, and 50 produced 1264, 1264, and 1251 kbit/s respectively — no
measurable effect at all.

Whatever actually determines the T23's lower bound (measurement puts it
around 990 kbit/s in this setup, well above the 278 kbit/s the same scene
costs at a fixed QP of 42) is still unexplained — see
[Rate Control Parameters](Rate-Control-Parameters.md#what-is-still-unexplained).

## See also

- [Rate Control Parameters](Rate-Control-Parameters.md) — every rate-control
  field, its range, which SoCs honour it, and what it costs.
- [Platform & SDK Support](Platform-SDK-Support.md) — the two encoder-API
  generations this page's argument rests on.
- [Configuration Reference](Configuration-Reference.md#videon--per-stream-encoder-settings) —
  the `video<N>.*` config keys (`bitrate`, `rc_mode`, `min_qp`, `max_qp`, ...).
- `dev_notes/T23_RATECONTROL_INVESTIGATION_2026-08-21.md` — full method, the
  complete per-window data, and the open questions this page simplifies.
