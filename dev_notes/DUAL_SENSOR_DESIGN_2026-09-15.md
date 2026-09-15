# Dual-sensor support on T23 — design for the Jooan W8-U

**Date:** 2026-09-15 · **Status:** design only, no code, no hardware yet
**Target board:** Jooan W8-U (`jooan_w8u_t23n_sc2336_atbm6132u`), T23N, 2× SC2336P,
ATBM6132U, 8 MB flash. Ordered; arrives in a few days.
**Branches:** `timps` `main`, `thingino-firmware-LuFi` `ciao`.

This is a planning document written before the hardware is in hand. Everything
below is either cited to a file we can read today or explicitly marked as
unverified. Where the earlier feasibility pass and the sources disagree, the
sources win and the correction is called out.

---

## 0. Corrections to the feasibility pass

Five things the feasibility summary got wrong or half-right. They matter
because three of them change the plan.

**a. The 19 `IMPVI_MAIN` call sites are not the T23 dispatch surface.**
All nineteen live inside `#ifdef ISP_NEW_TUNING_API` in
`src/hal/hal_ingenic.c:700-906` and `:1185`, `:5077` — that is the **T40/T41**
reworked tuning API (`src/isp_caps.h:35-37`). T23 falls through to the `#else`
classic branch and passes no sensor argument at all. The T23 work is a *third*
branch, not a modification of those nineteen. The real surface is the 30
distinct `IMP_ISP_Tuning_*` entry points the classic path uses.

**b. Every one of those 30 has a `MultiCamera` twin.** Checked one by one
against `include/T23/1.3.0/en/imp/imp_isp.h`: all 30 exist as
`IMP_ISP_MultiCamera_Tuning_<same name>(IMPVI_NUM num, …)`. Confirmed in the
shipped binary too — `nm -D` on
`thingino-firmware-LuFi/dl/ingenic-lib/git/T23/lib/1.3.0/uclibc/5.4.0/libimp.so`
finds 137 `IMP_ISP_MultiCamera*` symbols including every one we need. This is
much better news than "some of the API is there": the dispatch can be a
mechanical macro substitution (§3.3).

**c. `daynight.c`'s state is not thread-local — it is better than that.**
The ~60 automaton scalars are ordinary **function locals** in `dn_thread()`
(`src/daynight.c:1124-1195`). A second instance is a second `pthread_create`.
What is *not* per-thread: `g_thr`/`g_gate`/`g_started`, the `g_st_*` status
block behind `g_st_mu`, `g_int_hwm` (`:250`), `g_probe_req`/`g_ir_unusable`
(`:770`, `:777`), and two `static`s inside `dn_read()` itself (`:386-393`,
commented "single-caller: the detection thread" — a comment a second thread
makes false).

**d. `sc2336` single-sensor is 1080p25 on T23, not 30.**
`dl/ingenic-sdk/git/3.10.14/sensor-src/t23/sc2336.c:499` declares
`.fps = 25 << 16 | 1`, and 81 MHz / (2250 × 1440) = 25.0 exactly. The regs
array is *named* `..._30fps_mipi`, which is where the 30 came from; the name is
a misnomer. `sc2336p.c` does carry a real 30 fps mode but defaults to mode 0,
which is 15. The s0/s1 halves of the claim are correct and then some — see
§5.1.

**e. The `PA7/PB22/PB30` mux-GPIO restriction is not in the tree.**
No whitelist anywhere in `Config.soc.in`, the ISP driver, or the docs. The only
distinguished value is the driver *default*,
`static int mipi_switch_gpio = GPIO_PA(7);`
(`dl/ingenic-sdk/git/3.10.14/isp/t23/tx-isp-debug.c:27-31`), and `GPIO_PA(7)`
is 7. The restriction may still exist inside the closed
`libt23-firmware.a` — we cannot see it — but nothing we ship asserts it. Treat
it as folklore until measured.

---

## 1. Scope

**One hard constraint, stated first and designed against throughout: with the
feature disabled, a single-sensor camera must be bit-for-bit unaffected.** Not
"equivalent", not "no known regressions" — the same code paths, the same
on-disk config spelling, the same `/control` output, and a `timpsd` whose size
delta against today is zero.

The gate:

```
config BR2_PACKAGE_TIMPS_MULTI_SENSOR
	bool "Multi-sensor (dual camera) support — T23 only"
	depends on BR2_SOC_FAMILY = "t23"
	depends on BR2_THINGINO_IMAGE_SENSOR_QTY > 1
	default n
```

Naming and shape follow `BR2_PACKAGE_TIMPS_WEBRTC`
(`package/timps/Config.in:111-135`), which is the closest existing analogue: an
opt-in feature with hard platform dependencies and a long help text. It reaches
the build the same way everything else does, one line in
`package/timps/timps.mk` next to `:226`:

```make
USE_MULTI_SENSOR=$(if $(BR2_PACKAGE_TIMPS_MULTI_SENSOR),1,0) \
```

and lands in `src/isp_caps.h` as a capability alongside the existing ones:

```c
/* T23 1.3.0 only: IMP_ISP_MultiCamera_Tuning_* (IMPVI_NUM, val) — the classic
 * argument shapes with a leading sensor index. Verified present in the shipped
 * libimp for all 30 tunings the classic path uses. */
#if defined(PLATFORM_T23) && defined(USE_MULTI_SENSOR)
#define ISP_HAS_MULTICAM 1
#endif
```

`depends on BR2_SOC_FAMILY = "t23"` is not conservatism. T32/T40/T41 also have
multi-sensor hardware, but they reach it through `IMP_ISP_Tuning_*(IMPVI_NUM,
&val)` — the `ISP_NEW_TUNING_API` shape — which is a *different* port with
different argument marshalling and no board to test it on. Scoping v1 to T23
keeps the diff honest.

### 1.1 Dimensioning

```c
/* src/config.h */
#ifdef USE_MULTI_SENSOR
#define MS_MAX_SENSOR 2
#else
#define MS_MAX_SENSOR 1
#endif
#define MS_VSTREAM_PER_SENSOR 2
#define MS_MAX_VSTREAM (MS_MAX_SENSOR * MS_VSTREAM_PER_SENSOR)   /* 2 or 4 */
```

Streams stay **globally numbered** 0..3, and `sensor = stream / 2`. This is the
single most important structural decision in the document, because it is what
makes the feasibility pass's "scales for free" list actually free: OSD,
rotation, RTSP, fMP4, WebRTC, SRT, record and timelapse are all already indexed
by stream and all already bounded by `MS_MAX_VSTREAM`. Raising the bound is the
whole change for them.

One consequence to audit rather than assume: `src/hub.h:10-15` derives its
non-video slot numbers from the constant —

```c
#define HUB_AUDIO_SRC   MS_MAX_VSTREAM
#define HUB_JPEG_SRC    (MS_MAX_VSTREAM+1)
#define HUB_NJPEG       (1+MS_MAX_VSTREAM)
```

— so audio and JPEG shift from slots 2/3 to 4/5 when the flag is on. These are
in-memory indices, not wire or on-disk values, so the shift is invisible; but
every place that persists or transmits a hub slot number needs a read-through
before M2 closes.

That read-through has since been done for the HAL and found one real instance:
`video_thread` uses the *encoder channel number* as a hub source id in seven
places, which is only correct while `imp_chn == stream index`. See §5.4.

---

## 2. Config schema

This is the hardest design problem, and it is a *compatibility* problem more
than a mechanical one.

### 2.1 What exists

`src/config.c` has two ways of describing config, and the second one is not an
accident:

**Table-driven scalar sections** — `g_sections[]` at `:1215-1238`, entries built
by `SEC(prefix, noget, base_off, fields)`. Each entry is one prefix, one base
offset into `ms_config`, one field table. `section_find()` (`:1258`) does a
linear prefix match and `field_find()` (`:1242`) matches the remainder. Clean,
and it cannot express an array, because `cfg_section` carries a single
`base_off`.

**Indexed sections, hand-rolled** — three of them, each following the same
shape: a small prefix parser that returns the remainder plus indices, then the
*same* `field_find()` against a dedicated table, then a manual `field_set()`
into the array element.

| indexed section | parser | key shape |
|---|---|---|
| OSD items | `osd_key()` | `osdS.I.field` |
| privacy regions | `privacy_key()` | `privacyS.I.field` |
| video streams | *inline*, no parser | `videoN.field` |

The video one is the odd one out: it has no helper, just three literal
`!strncmp(key,"video0.",7) || !strncmp(key,"video1.",7)` branches, at
`src/config.c:1431` (`field_for_key`), `:1472` (`set_kv`) and `:1749`
(`config_get_key`). Each one re-derives the index as `key[5]-'0'`. That is the
pattern the feasibility pass called "hand-rolled ad hoc in three places", and
it is right.

### 2.2 Proposal: generalise, then reuse

Do **not** invent a fourth pattern. Extend `cfg_section` so it can describe an
indexed section, migrate `videoN.` onto it, and then `sensorN.`/`imageN.` are
new *data*, not new *code*:

```c
typedef struct {
    const char     *prefix;    /* "sensor" (indexed) or "sensor." (scalar) */
    const char     *alias;     /* NEW: accepted spelling for index 0, or NULL */
    unsigned char   plen;
    unsigned char   noget;
    unsigned short  base_off;
    const cfg_field *fields;
    unsigned char   nfields;
    unsigned char   count;     /* 0 = scalar; >0 = indexed, key is <prefix><N>. */
    unsigned short  stride;    /* sizeof one element when count > 0 */
} cfg_section;
```

`section_find()` grows one branch: when `count > 0`, match the prefix, then
require a digit `< count` and a dot, and return both the field remainder and
the index. The three call sites collapse from a literal `video0./video1.`
comparison to `base_off + idx * stride`. Net diff for the video migration is
**negative** — it deletes more than it adds — and it changes no behaviour, which
is exactly what makes it testable without the W8U (§8, M1).

### 2.3 Wire and on-disk format

The question the feasibility pass posed: does `sensor0.*` become canonical with
`sensor.*` as an alias, or something else?

**Answer: `sensor.*` stays canonical for sensor 0 when the feature is off, and
`sensor0.*` becomes canonical when it is on. Each is an accepted alias for the
other in both directions, always.**

The asymmetry with `videoN.` is deliberate and justified by the installed base,
not by taste. `video` has *never* had an unindexed spelling — `video0.`/`video1.`
are the only form the format has ever had, so there is nothing to be compatible
with. `sensor.`/`image.` are different: they appear in every `/etc/timps.conf`
on the fleet, in `timps.conf.example:785` and friends, in
`docs/wiki/Configuration-Reference.md`, in the WebUI CGI bridge, and in the
`/control` JSON as `{"sensor":{…}}` / `{"image":{…}}`
(`src/control.c:964`, `:729`). Renaming them outright is a breaking change for
every existing camera in exchange for nothing.

The two alternatives were considered and rejected:

- *`sensor.*` as a "shared" section that both sensors inherit, with `sensor0.`/
  `sensor1.` as overrides.* This is a third pattern, it introduces merge
  semantics (what does a partially-overridden section mean?), and merge
  semantics in a config file is a support burden forever.
- *Hard rename to `sensor0.` with a migration on first boot.* Rewrites every
  camera's config file on upgrade, and `config_write_keys()` fsyncs — flash wear
  for a cosmetic win.

The alias mechanism needed is small, and half of it already exists.
`cfg_field.alias` plus `key_canonical()` (`src/config.c:1268`) exist precisely
so that a config file holding a pre-rename spelling is *rewritten in place*
rather than having the canonical key appended beside it — the comment at
`:1249-1257` is explicit that the append behaviour "is a good way to lose an
afternoon". A section-level `alias` is the same idea one level up, and it lets
`key_canonical()` do its existing job unchanged.

For `/control` JSON, **no new pattern at all is needed**. `video` already uses
an index-keyed object — `{"video":{"0":{…},"1":{…}}}`, handled at
`src/control.c:927-950`. `sensor` and `image` gain the same optional shape,
with the flat `{"sensor":{"model":…}}` continuing to mean sensor 0. That is a
copy of an existing handler, and it keeps the WebUI's `timps-api.js` transport
untouched.

### 2.4 What `sensorN.` must carry that `sensor.` does not

`sensorN.width`/`height` stop being optional. raptor's own troubleshooting notes
(`gtxaspec/raptor-docs`, `27-multi-sensor.md`, "Green lines on sub streams") say
the ISP reports `sensor resolution(0x0)` under the dual-sensor drivers, so the
framesource has to be told the input geometry from config. timps already knows
this failure mode for the single-sensor case — `src/isp_caps.h:125-130` says
`ISP_HAS_SENSOR_ATTR` is "used to declare the framesource input resolution when
the sensor driver reports 0x0 to the framesource (e.g. sc2336)". So this is an
existing path being made mandatory, not new machinery. The one real change is
that `g_isp_sensor_w/h` auto-detect must not overwrite a configured value with
zero.

---

## 3. HAL layer

### 3.1 State

`src/hal/hal_ingenic.c:202-205`:

```c
static IMPSensorInfo    g_sensor;
static const ms_config *g_hcfg;
static int              g_isp_sensor_w, g_isp_sensor_h;
```

`g_sensor` and `g_isp_sensor_w/h` become `[MS_MAX_SENSOR]`. `g_hcfg` stays
scalar — it points at the whole config, which is still one object. With
`MS_MAX_SENSOR == 1` these are one-element arrays and the compiler emits the
same code.

### 3.2 Init sequence

Borrowed in structure from raptor's `HAL_T23_MULTICAM` branch in
`dl/thingino-raptor-hal/git/src/hal_common.c:1240-1266`, with one deliberate
divergence.

```
IMP_ISP_Open()
for each sensor i:  IMP_ISP_AddSensor(&g_sensor[i])      /* old-style, no IMPVI */
IMP_ISP_EnableSensor()                                    /* once, enables all */
IMP_OSD_SetPoolSize() / IMP_ISP_Tuning_SetOsdPoolSize()
IMP_System_Init()
IMP_ISP_EnableTuning()
```

Three things worth writing down because they are easy to get wrong:

- **T23 keeps the classic `AddSensor`/`EnableSensor` signatures even in
  multi-sensor mode.** `IMP_ISP_AddSensor(&info)` takes no `IMPVI_NUM`, and a
  *single* `IMP_ISP_EnableSensor()` enables everything registered. This is the
  opposite of T32/T40/T41, where both are per-`IMPVI_NUM`. raptor's file has
  both branches side by side and they do not resemble each other.
- **We do not call `IMP_ISP_MultiCamera_SetSwitchgpio()`.** raptor's HAL has
  the call, but raptor's own board documentation says to leave it off on T23
  (`27-multi-sensor.md`: "`[mipi_switch] enabled = false` — the T23 ISP kernel
  module handles GPIO switching internally via the `mipi_switch_gpio` module
  parameter"). The mux is the kernel's, configured at `insmod` time. Two owners
  for one GPIO is a bug waiting to happen; take the structure of raptor's
  branch and drop the call.
- **We do not call `IMP_ISP_SetCameraInputMode()`.** That API is the *splicing*
  path — `IMPISPDualSensorSplitJoint joint_mode` plus `outstride[2]`
  (`imp_isp.h:8094-8097`), which glues two sensors into one wide frame. We want
  pass-through: two independent pipelines. The header's own note at `:8103`
  says the API "can only be used for concatenation if it is not pass-through".
  Explicitly out of scope; worth a comment in the code so nobody adds it later
  thinking it is required.

Teardown mirrors it: `DisableTuning`, `System_Exit`, `DisableSensor()`,
`DelSensor(&g_sensor[i])` per sensor in reverse, `ISP_Close`.

### 3.3 Tuning dispatch

30 distinct entry points, all with a verified 1:1 `MultiCamera` twin (§0b). The
mechanism should make the zero-regression guarantee **structural** rather than
reviewed:

```c
#ifdef ISP_HAS_MULTICAM
#define ISPT(fn, si, ...) IMP_ISP_MultiCamera_Tuning_##fn((IMPVI_NUM)(si), __VA_ARGS__)
#else
#define ISPT(fn, si, ...) ((void)(si), IMP_ISP_Tuning_##fn(__VA_ARGS__))
#endif
```

With the flag off, `ISPT(SetBrightness, si, u)` expands to
`IMP_ISP_Tuning_SetBrightness(u)` — character-for-character today's call, with
the index compiled away. That is a much stronger guarantee than 30 hand-edited
call sites reviewed for regressions, and it is why this shape is worth the
macro.

`isp_apply_image()` (`src/hal/hal_ingenic.c:693`) then takes a sensor index and
reads `g_hcfg->image[si]` instead of `g_hcfg->image`. The `ISP_NEW_TUNING_API`
branch above it is untouched.

The three read-side helpers in `src/hal/hal.h` gain indexed variants, following
raptor's `_n` convention (`10-hal-api.md`):

```c
int hal_isp_total_gain_n(int sensor, uint32_t *gain);
int hal_isp_ae_luma_n   (int sensor, uint32_t *luma);
int hal_isp_exposure_n  (int sensor, hal_isp_expo *out);
```

with the existing three kept as `_n(0, …)` wrappers so nothing outside
`daynight.c` changes.

### 3.4 The chn-0 latch

`fs_kick_chn0()` (`:611`) hardcodes framesource channel 0 as "the ISP's
direct-output channel", and its comment says so. Under two sensors there are
two such channels — 0 and 3. It becomes `fs_kick_chn(int fs_chn)` and the four
callers (`:4396`, `:4397`, `:4645`) pass the channel belonging to the sensor
whose `running_mode`/flip actually changed. The `g_hcfg->video[0].enabled`
guard generalises to the corresponding stream.

### 3.5 Lock granularity

Keep **one** `g_isp_lock` (`:459`) across both sensors in v1.

The temptation is a lock per sensor, on the theory that two sensors' tuning is
independent. We have no evidence for that: the `MultiCamera` calls go through
the same libimp ISP context and we cannot see whether libimp serialises
internally. Splitting the lock on a guess converts a latency question into a
corruption question. The cost of not splitting is that a `/control` apply for
sensor 1 can wait behind one for sensor 0 — measurable, and worth measuring at
M5 before deciding.

---

## 4. Day/night

### 4.1 Recommendation: one automaton per sensor, one IR owner

**Per sensor:** measurement (`dn_read`), the whole EMA/probe/verdict automaton,
and the ISP `running_mode` decision. The running mode genuinely *is* per-sensor
— `IMP_ISP_MultiCamera_Tuning_SetISPRunningMode(IMPVI_NUM, …)` is in the
shipped libimp.

**Shared, because the hardware is shared:** the IR-cut filter and the
illuminator.

The reasoning for splitting the automaton is not symmetry, it is that the two
lenses see different scenes. One is fixed wide, one is on the PTZ head pointed
wherever it was last moved. A shared EMA over both would be wrong for at least
one of them at any given moment, and the redesign note
(`DAYNIGHT_REDESIGN_2026-08-17.md`) is built entirely around the exposure index
tracking *one* scene. The cost is low: §0c established that the automaton is
already ~60 function locals, so a second instance is a second thread.

The reasoning for *not* splitting the actuators is that there is only one of
each. From the board's own `thingino.json` (identical on `ciao` and upstream
`master`):

```json
"ir850": 59,
"ircut": "50 49",
"white": 60,
"sensor_1": 18,
"sensor_2": 16,
"sensor_switch": 7
```

One `ir850`, one `ircut` pin pair (the usual H-bridge forward/reverse), one
`white` LED. Two independent automata both firing `daynight.switch_cmd` at one
H-bridge is relay chatter, and it is the kind of bug that shows up as
mysterious mechanical clicking at dusk rather than as a clean failure.

So: a new `daynight.ir_master` (default 0). Only that sensor's automaton may run
`switch_cmd` and `irprobe_cmd`. The other runs a **measure-and-follow**
automaton: it keeps its own AE state and sets its own ISP running mode, but it
treats the IR-cut/illuminator state as an input it cannot change. Config stays
single-valued for `switch_cmd`/`irprobe_cmd` — correctly, because the hardware
is — and gains one integer.

### 4.2 Unconfirmed: which head the IR hardware serves

The GPIO map proves there is exactly one IR-cut and one illuminator. It does
**not** say which lens they sit behind.

The presence of a `white` LED on GPIO 60 *alongside* a single `ir850` is the
signature of the common dual-lens arrangement: one full-colour lens with white
floodlight and no IR-cut, one IR lens with IR-cut and 850 nm illuminator. If
that is what the W8U is, then `ir_master` must be the IR lens's index, and the
other sensor's day/night should probably be disabled outright — flipping a mode
for a filter it does not have is worse than doing nothing.

**Do not guess this before the camera is on the bench.** It is a five-minute
check with `gpio` and a phone camera, and getting it wrong means shipping a
day/night state machine that fights hardware that is not there.

### 4.3 The part most likely to be underestimated: `dn_read()`

`dn_read()` (`src/daynight.c:343`) has two data sources, and the *primary* one
does not survive two sensors.

**Source 1, the IMP API** — `hal_isp_total_gain()` and `hal_isp_exposure()`.
Becomes per-sensor for free via §3.3's `_n` variants.

**Source 2, the `/proc/jz/isp/isp-m0` scrape** (`:386-461`). This is the
problem, in three ways:

1. raptor's troubleshooting section says that under the `-double` firmware,
   `cat /proc/jz/isp/isp-m0` shows **both sensors**. timps's parser is a flat
   `while (fgets(...))` with a `got` bitmask that `break`s the moment it has all
   nine fields (`:461`). With two sensors' worth of fields in one file it
   silently reads whichever block comes first — *for both threads*. No error, no
   warning, just two automata steering off one sensor's exposure.
2. The two `static`s at `:386-393` (`used`, `since_reprobe`) carry the comment
   "single-caller: the detection thread". A second thread makes that comment
   false and the cache racy.
3. `g_int_hwm` (`:250`) is a per-sensor AE high-water mark held in a file-scope
   scalar.

**The fix is to stop scraping under multi-sensor and drive both automata off the
IMP API.** `hal_isp_expo` already carries `it_lines` and `it_max_lines` — the
exposure index — plus `again`/`dgain` from `GetEVAttr` (`src/hal/hal.h:47-56`),
and `IMP_ISP_MultiCamera_Tuning_GetExpr`/`GetEVAttr`/`GetAeLuma`/`GetTotalGain`
are all confirmed in the shipped `libimp.so`.

The one thing only the scrape supplies is `o->headroom`, derived at `:483-497`
from `MAX SENSOR analog gain − SENSOR analog gain` (+ the ISP digital pair).
That feeds `dn_clipped()` and the probe logic. It is reconstructible: the
maxima are timps's *own* config values, the ones it wrote via
`SetMaxAgain`/`SetMaxDgain` from `image.max_again`/`max_dgain`, so
`headroom ≈ (image.max_again − ex.again) + (image.max_dgain − ex.dgain)` in the
SDK's units. That equivalence needs checking against a live single-sensor
camera before it is relied on — it is the kind of unit mismatch that produces a
plausible-looking wrong number.

There is precedent for exactly this cutover: `:513-517` already prefers
`ratio_imp` over the scrape where the scrape's maximum is an estimate, with the
measurement that motivated it recorded inline. This extends that decision rather
than reversing it.

---

## 5. Encoder and channel plan

> **Post-review correction, 2026-09-15.** §5.2's closing claim and the whole of
> the original §5.3/§5.4 were wrong, and wrong in a way that would have been
> discovered at M4 instead of M2. `DUAL_SENSOR_DESIGN_REVIEW_2026-09-15.md`
> §2a/§2b caught it: the first draft costed a single sensor at "2 video + 1
> JPEG" when the compiled default is five encoder channels, and it did not
> notice that `chn = grp = v->imp_chn` puts sensor 1's framesource numbers
> straight on top of sensor 0's piggyback-JPEG encoder channels. The review
> stopped at the diagnosis; §5.2–§5.4 and the new §5.6 below are the worked-out
> resolution, re-verified against `config.c`, the T23 1.3.0 headers and a
> disassembly of the shipped `libimp.so`. §5.1 is unchanged and §5.5 keeps its
> original argument with one added caveat. Two of the sketched fixes did **not**
> survive verification and are called out where they died: the sequential `grp`
> allocator (unnecessary, §5.3.5) and "give sensor 1 its own dedicated JPEG
> channel" (there is no such thing to give — §5.6). §9's risks 2 and 3 are
> rewritten to match, and a new risk 3b comes out of the disassembly.

### 5.1 Framerate

The 15 fps constraint is real and it is enforced in three independent places.

`sc2336ps0.c:49` and `sc2336ps1.c:47` both set `SENSOR_OUTPUT_MAX_FPS 15`, each
declares exactly one 1920×1080 window, and each hard-refuses anything higher in
`sensor_set_fps()` (`sc2336s0.c:791`, `sc2336s1.c:796`:
`if (newformat > (SENSOR_OUTPUT_MAX_FPS << 8) || …) return -1;`). The shipping
upstream W8U profile sets `FPS="15"` on all four streams and both sensors.

So a dual-sensor timps profile should default **every** encoder to 15 fps, and
`sensorN.fps` should clamp to 15 under the gate rather than silently accepting a
25 that the driver will reject. Note that this is a real regression against the
same board in single-sensor mode, where `sc2336p` can reach 30 — worth saying
out loud in the Kconfig help text, because "I turned on dual sensor and my fps
halved" is otherwise a support question.

**Still unknown:** whether time-multiplexing costs more than the nominal 15 each.
Nobody has published a measurement. See §9.

### 5.2 Framesource channels

The `sensor_idx * 3` mapping is confirmed **from the SDK header**, not only from
raptor. `IMPISPCameraInputMode.outstride`'s comment at
`include/T23/1.3.0/en/imp/imp_isp.h:8096` reads

> `[0]: main stream f0 f3 splicing span [1]: secondary stream f1 f4 splicing span`

and the `IMPISPDualSensorSplitJoint` enum above it is annotated `//fs0 is
spliced with fs3` and `//fs1 is spliced with fs4`. Even though we are not using
the splicing path, those comments are the vendor stating the channel layout.

| | main | sub | third |
|---|---|---|---|
| sensor 0 | fs 0 | fs 1 | fs 2 |
| sensor 1 | fs 3 | fs 4 | fs 5 |

timps's existing allocation (`video0.imp_chn=0`, `video1.imp_chn=1`,
`jpeg.imp_chn=2` — `timps.conf.example:261,385,399`) sits exactly inside
sensor 0's block, so **no framesource channel that exists today is renumbered**.
Sensor 1 claims 3/4/5, of which the plan below uses 3 and 4.

The first draft ended this paragraph with "no renumbering of anything that
exists today", which is only true of framesources. `imp_chn` is *also* used as
the encoder channel and the encoder group (`hal_ingenic.c:4706`), and there the
claim is false — see §5.3.

Bounds are fine: `MS_FS_MAXCHN` is 8 (`hal_ingenic.c:434`, so fs_use/fs_unuse
accept 0..7) and `imp_chn` is clamped 0..8 in `config.c:853`. The comment there
records the empirical finding that "libimp's own bound is chn<9"; confirmed
directly — `IMP_FrameSource_CreateChn` opens with `sltiu v0,a0,9`
(`libimp.so` T23 1.3.0, `0x884bc`). The clamp is one wider than
`MS_FS_MAXCHN`; pre-existing, harmless (channel 8 silently no-ops in `fs_use`),
not worth touching here.

### 5.3 Encoder channels, groups and the collision

#### 5.3.1 Four namespaces, not one

The first draft (and the code it described) treats "the channel number" as a
single thing. The SDK does not. `imp_system.h:40-60` defines the model:

> *Group is the smallest unit of data input … Channel usually refers to a single
> functional unit … For FrameSource, one Channel outputs one single original
> image, Channel FrameSource is actually Group … Channel as a functional unit,
> usually requires Register to Group (except FrameSource), in order to receive
> data.*

and `:64-66` defines what a bind is made of:

> *the srcCell's two parameters (deviceID, groupid) … while dstCell only
> deviceID and groupID are valid, outputID is meaningless as a data entry.*

So `IMP_System_Bind(&{DEV_ID_FS, f, 0}, &{DEV_ID_ENC, g, 0})` connects
**framesource channel `f`** to **encoder group `g`**. The encoder *channel*
number does not appear in a bind at all — it appears only in
`IMP_Encoder_CreateChn(encChn, …)` and in `RegisterChn(encGroup, encChn)`, which
is what attaches it to a group. The header even spells out the two-channels-in-
one-group case timps already relies on: *"in the Encoder's Group registers two
Channels, so Encoder Group is a two-way output: H264 and JPEG"*
(`imp_system.h:69-70`).

That the two are genuinely separate namespaces is not just prose. Disassembling
the shipped `libimp.so` (T23 1.3.0, `mipsel-linux-objdump -d`):

| entry point | address | bound check |
|---|---|---|
| `IMP_FrameSource_CreateChn` | `0x884bc` | `sltiu v0,a0,9` — FS chn < 9 |
| `IMP_Encoder_CreateGroup` | `0x4cbe0` | `slti v0,a0,9` — grp < 9 |
| `IMP_Encoder_CreateChn` | `0x4e174` | `slti v0,a0,9` — chn < 9 |
| `IMP_Encoder_RegisterChn` | `0x50a38`, `0x50a4c` | **two independent checks**: `a0` (grp) < 9 *and* `a1` (chn) < 9 |
| `IMP_OSD_CreateGroup` | `0xab848` | `slti v0,a0,9` — OSD grp < 9 |

`RegisterChn` range-checking both arguments separately, then indexing a group
table by `a0*16` (`0x50aa4`) and a channel table by `a1*16` (`0x50a54`), is the
decoupling proof: the two numbers are looked up in different tables and nothing
requires them to be equal. **`IMP_Encoder_CreateChn(5, …)` inside a group that
is bound from framesource channel 3 is exactly what the API is shaped for.**

**One caveat found in the same disassembly, and it is the only thing in this
section that is not fully settled.** `RegisterChn` has a *conditional* second
stage (`0x50a88`-`0x50b6c`) that runs only when the channel record's word at
`+2608` is positive **and** its byte at `+168` is non-zero — both set somewhere
inside `CreateChn`, from attributes we cannot see. When that stage does run it
enforces a fixed group↔channel pairing: group 0 accepts channels {0,2}, group 1
accepts {1,3}, group 2 accepts {4,5}, and groups ≥ 3 are rejected outright.

It demonstrably does *not* run on the channels timps creates today — the
shipping default registers channel 3 into group 0 and channel 4 into group 1,
both of which that stage would refuse, and the fleet streams. So the guard is
false for our encoder attributes. But the plan below puts channels into groups 3
and 4, which that stage would reject if anything ever flipped the guard true.
**Treat it as a live check at M4, not a proven non-issue:** if
`IMP_Encoder_RegisterChn` fails for video2/video3 with everything else correct,
this is the first place to look. There is no renumbering that would rescue it —
sensor 0 already occupies groups 0, 1 and 2, which are the only ones that stage
permits — so the answer would have to be finding which encoder attribute sets
the guard and not setting it.

This also closes the first draft's `NR_MAX_ENC_GROUPS` unknown (§9 risk 2): the
symbol is `@ref`'d throughout `imp_encoder.h` but defined in no header we
vendor, and the binary says the bound is 9 for groups, channels and OSD groups
alike. The disassembly further shows a group record with **two** channel slots
(`0x50ab0`/`0x50abc`, taken at `0x50be0`/`0x50be4`), matching the header's
H264+JPEG example — so "one video + its piggyback JPEG per group" is the
maximum, which is exactly what the plan below uses.

#### 5.3.2 What one sensor actually costs today

Re-checked against `src/config.c` directly rather than taken from the review:

| # | encoder | fs chn | enc grp | enc chn | source |
|---|---|---|---|---|---|
| 1 | video0 (main) | 0 | 0 | 0 | `config.c:329` `v->imp_chn=i`; `:4706` `chn=grp=imp_chn` |
| 2 | video1 (sub) | 1 | 1 | 1 | ditto |
| 3 | dedicated JPEG | 2 | 2 | 2 | `config.c:367` `c->jpeg.imp_chn=2`; `hal_ingenic.c:3405,3417,3423,3431` — one number used as fs chn, group, channel *and* both bind cells |
| 4 | piggyback JPEG on video0 | (shares 0) | 0 | 3 | `config.c:334-335` `jpeg_enabled=1`, `jpeg_chn=MS_MAX_VSTREAM+1+i` |
| 5 | piggyback JPEG on video1 | (shares 1) | 1 | 4 | ditto |

**Five encoder channels and three encoder groups per sensor, on a default
build.** The review's numbers are correct. Note item 3: `jpeg.enabled` defaults
to 0 (`config.c:366`), so the dedicated channel is usually not *created* — but
its channel number 2 is reserved by the schema either way, and any layout that
hands 2 to something else breaks the moment an operator sets `jpeg.enabled=1`.

#### 5.3.3 The collision

`hal_ingenic.c:4706` — `int chn=v->imp_chn, grp=v->imp_chn;` — makes the
encoder channel number equal to the framesource channel number. §5.2 fixes
sensor 1's framesources at 3/4/5 (hardware, not our choice). So sensor 1's
video streams would ask for encoder channels 3 and 4, which are already
sensor 0's piggyback-JPEG channels. `IMP_Encoder_CreateChn` on an existing
channel fails; the second sensor simply never encodes.

This is not a budget problem that a default can mitigate. It is a numbering
problem, and it is why the original §5.3's "turn sensor 1's JPEG off, 5 with one
spare" was arithmetic on the wrong base.

#### 5.3.4 The verified layout

The rule that makes this work is already latent in the config defaults — it just
has to be *stated* and then used for the encoder channel instead of `imp_chn`:

```
enc_chn(video i)        = i                       /* 0 .. MS_MAX_VSTREAM-1 */
enc_chn(dedicated JPEG) = MS_MAX_VSTREAM
enc_chn(piggyback on i) = MS_MAX_VSTREAM + 1 + i  /* today's jpeg_chn formula */
                                                  /* = config.c:335, unchanged */
enc_grp                 = the framesource channel it is bound from  /* unchanged */
```

Total = `2*MS_MAX_VSTREAM + 1`: **5 at one sensor, 9 at two** — exactly the
budget the hardware allows, with the piggyback defaults deciding how much of it
is actually claimed.

With the flag **off** (`MS_MAX_VSTREAM == 2`) that yields 0, 1 / 2 / 3, 4 —
character-for-character the numbers in §5.3.2. `jpeg.imp_chn=2` turns out to
have been `MS_MAX_VSTREAM` in disguise all along. Nothing renumbers.

With the flag **on** (`MS_MAX_VSTREAM == 4`):

| stream / encoder | fs chn | enc grp | enc chn | hub src | default |
|---|---|---|---|---|---|
| video0 main (s0) | 0 | 0 | 0 | 0 | on |
| video1 sub (s0) | 1 | 1 | 1 | 1 | on |
| video2 main (s1) | **3** | **3** | **2** | 2 | on |
| video3 sub (s1) | **4** | **4** | **3** | 3 | on |
| dedicated JPEG (s0) | 2 | 2 | **4** | `HUB_JPEG_SRC`=5 | off (as today) |
| piggyback JPEG on video0 | (0) | 0 | **5** | 6 | on |
| piggyback JPEG on video1 | (1) | 1 | **6** | 7 | on |
| piggyback JPEG on video2 | (3) | 3 | **7** | 8 | **on** — see §5.6 |
| piggyback JPEG on video3 | (4) | 4 | **8** | 9 | **off** |

Budgets, all against the verified bound of 9:

- **Encoder channels:** 8 of 9 claimed with the defaults above; channel 8 is the
  spare, claimed only if an operator turns on `video3.jpeg`. Sensor 0's five
  channels keep their exact current numbers.
- **Encoder groups:** 5 of 9 (0–4), each holding at most two channels. Group 5
  would only appear if sensor 1 ever got its own dedicated-JPEG framesource,
  which §5.6 argues against.
- **Framesource channels:** 5 of 8 (`MS_FS_MAXCHN`) — 0, 1, 2 on sensor 0 and
  3, 4 on sensor 1. Sensor 1's third framesource (5) is left unused.
- **OSD groups:** a fourth, independent namespace, indexed by *stream* (0–3) —
  see §5.5. It consumes no encoder budget.

Read the table by row, never by number. With the flag on, encoder channel 2 is
video2's while encoder group 2 is the dedicated JPEG's — they are different
objects that happen to share a digit, which is precisely the confusion the old
`chn = grp = imp_chn` line created and §5.4 removes. Any log line, `/control`
field or QA assertion that prints "channel N" has to say *which* namespace it
means.

raptor's `max_enc_channels = 6` is a raptor policy number ("vendor dual-sensor
sample uses 6"), not a hardware bound; the hardware bound is the 9 above. The
plan fits inside 9 and does not fit inside 6, so the two disagree — worth
knowing at M3′, when raptor's own firmware is on the bench and its `jpeg0`/
`jpeg1` ring entries can be turned on to probe where the real wall is.

#### 5.3.5 Where the sketch was wrong and stayed wrong

Two candidate fixes were considered and did **not** survive verification:

- *A sequential `grp` allocator (original §5.4).* Unnecessary. Groups are bound
  by 9, and `grp = fs_chn` already yields 0, 1, 2, 3, 4 — all in range, all
  distinct, and byte-identical to today's behaviour for sensor 0. Adding an
  allocator would change the flag-off build for no benefit and break §1's
  zero-delta gate. **Dropped.**
- *A second dedicated JPEG channel for sensor 1 (encoder channel 7, own
  framesource 5).* There is no `jpeg1.*` section to configure it, no second
  `HUB_JPEG_SRC` slot to publish it on, and `hal_ingenic.c:4994` identifies the
  dedicated channel by the sentinel `jc->src == HUB_JPEG_SRC`. It is three new
  mechanisms to reach a place one existing mechanism already reaches. **Dropped
  in favour of a piggyback — see §5.6.**

### 5.4 Decoupling `chn` from `imp_chn` — the real scope

The question the plan turns on is whether `chn = grp = v->imp_chn` is an
architectural assumption or a single-sensor shortcut. Counted, not guessed.

**It is a shortcut, but it has leaked into three distinct meanings.** `vchan`
(`hal_ingenic.c:245-246`) carries one `chn` field that is used as all of:

| meaning | refs | examples |
|---|---|---|
| encoder channel | 15 | `IMP_Encoder_StartRecvPic/StopRecvPic/PollingStream/GetStream/ReleaseStream/RequestIDR/GetChnAveBitrate(vc->chn)` |
| framesource channel | 16 | `fs_use/fs_unuse/fs_teardown(vc->chn)`, `IMP_FrameSource_GetFrame/ReleaseFrame/SetFrameDepth/DestroyChn(vc->chn)`, `IMPCell{DEV_ID_FS, c, 0}` |
| **hub source id** | 7 | `hub_active`, `hub_pool_trim`, `hub_pkt_get`, `hub_publish`, `hub_publish_take`, `act_ready_vchan` (`:424`) |
| log text / assignment / lookup | 24 | format arguments, `:2011`, `:2969`, `:4251`, `:4772` |

62 references in total. The third row is the one that would have bitten
silently: `hub_active(vc->chn)` works today only because `imp_chn` happens to
equal the stream index; with sensor 1's video2 on encoder channel 2 and
framesource channel 3 the same expression reaches whichever hub slot that number
lands on.

The corresponding right answer already exists in the same file. `jchan`
(`:3389`) *already* carries `chn` (encoder), `fs_chn` and `src` (hub) as three
separate fields, and `jpeg_thread` uses each correctly. So the refactor is:
**make `vchan` look like `jchan`.**

Concretely, and this is the whole list:

1. `vchan` gains `fs_chn` and an unconditional `si` (it exists today only under
   `ROT_HAS_SW_90`, `:283`). Split the 62 references above by the table's rows;
   the 24 log/misc ones can go to `si` for readability.
2. `hal_ingenic.c:4706` — `int chn=v->imp_chn, grp=v->imp_chn;` becomes
   `int fs_chn=v->imp_chn, grp=v->imp_chn, chn=<enc_chn per §5.3.4>;`, and
   `vc->chn/grp/fs_chn/si` are recorded at `:4772`.
3. `jpeg_setup()` (`:3403-3444`) splits its single `chn` into `jfs`
   (= `cfg->jpeg.imp_chn`, also the group, also both bind cells) and `jenc`
   (= `MS_MAX_VSTREAM`). Seven call sites inside one function.
4. `jpeg_attach()` (`:3460`) takes the parent's framesource channel explicitly
   instead of being handed `grp` and passing it to `jpeg_chan_start` as
   `fs_chn` (`:3472`) — correct today only because the two are equal.
5. Both teardown paths: the bring-up failure path (`:4918-4936`) and `ing_stop`
   (`:4992-5008`) each build `IMPCell{DEV_ID_FS, …}` and call
   `IMP_FrameSource_DestroyChn` / `fs_teardown` on the encoder channel number.
   ~6 lines each.
6. `rc_live_vchan()` (`:4231`) finds a stream's `vchan` by comparing
   `video[si].imp_chn` against `vc->chn` — becomes `vc->si == si`.
7. `video_thread`'s T31-only `ave_gop` lookup (`:2008-2015`) does the same
   reverse lookup; same fix. Not on the T23 path, but it is the same latent bug.
8. `fs_kick_chn0()` → `fs_kick_chn(fs_chn)` (already §3.4), and `fs_use()`'s
   `chn == 0` relatch (`:504`) becomes "is this sensor's direct-output
   framesource", i.e. 0 or 3 — the gap the review flagged in §3.1/§3.4.

That is one struct, one declaration line, two small functions, two teardown
blocks and two reverse lookups — contained, mechanical, and with
`MS_MAX_SENSOR == 1` every derived number is identical to today's, so the M2
zero-size-delta gate is a real check on it rather than a hope.

Recommended shape, so flag-off is structurally rather than reviewably
unchanged: one macro per encoder-channel kind, e.g.

```c
#ifdef USE_MULTI_SENSOR
#define MS_ENC_CHN_VIDEO(v,i)  (i)
#define MS_ENC_CHN_JPEG(cfg)   MS_MAX_VSTREAM
#else
#define MS_ENC_CHN_VIDEO(v,i)  ((v)->imp_chn)      /* today, verbatim */
#define MS_ENC_CHN_JPEG(cfg)   ((cfg)->jpeg.imp_chn)
#endif
```

The `#else` arms preserve the current behaviour even for the non-default case of
an operator who has hand-set `imp_chn` to something other than the stream index
— which the unconditional form would quietly change.

### 5.5 OSD groups — a hard wall

**T23 has 2 OSD groups.** raptor's caps: `.max_osd_groups = 2` for T23 (and for
every classic SoC; only T40/T41 get 4).

*(Post-review note: the review is right that this is raptor's table, not a
libimp bound — `IMP_OSD_CreateGroup` itself accepts 0..8, §5.3.1. The 2 may be
an IPU limit one layer down, or it may be raptor policy. It is cheaply testable
today on a single-sensor T23 by creating OSD group 2; do that before M4 rather
than designing around an unverified number. Either way OSD groups are a
namespace of their own and consume no encoder channel or group.)*

timps assigns one group per stream, by identity: `s->grp = stream_idx`
(`src/hal/imp_osd.c:438`), then `IMP_OSD_CreateGroup(s->grp)` at `:454`. With
four streams that is `CreateGroup(2)` and `CreateGroup(3)` — past the limit, and
the existing error path just logs and disables OSD for that stream.

This is corroborated from the field rather than inferred: the shipping upstream
W8U profile sets `BR2_PACKAGE_THINGINO_RAPTOR_CONF_OSD_ISP_OSD_TRUE=y`. raptor
ships this exact board on **ISP OSD**, and its docs say why — with dual sensor,
"OSD appears on main streams only (T23 hardware limitation)".

Plan:

- **v1:** allocate OSD groups from a pool of `max_osd_groups`, main stream per
  sensor only (groups 0 and 1). Substreams get no hardware OSD, logged once, in
  the same shape as the existing rotation-refusal warnings at
  `imp_osd.c:443-451` — which is a pattern this codebase already uses for
  "the hardware refused, here is what you got instead".
- **v2, optional:** ISP OSD. `IMP_OSD_MultiCamera_SetRgnAttr_ISP` is present in
  the shipped `libimp.so` (verified by `nm`), raptor exposes it at
  `src/hal_osd.c:634-635`, and T23 allows 8 ISP OSD regions. Main streams only.
  Worth its own note, not part of this design.

### 5.6 Snapshot and timelapse for the second sensor

This is where the sketched layout was checked and **failed**, so it is written
out rather than asserted.

`hub_pick_jpeg_src(cfg, chn, strict)` (`src/hub.c:83-97`, contract in
`src/hub.h:155-168`) has a three-tier priority:

1. the JPEG encoder piggybacked on video stream `chn` — requires
   `g_cfg_boot.video[chn].enabled && .jpeg_enabled`;
2. if `strict`, give up (`-1`); otherwise the dedicated `jpeg.*` channel, if
   `cfg->jpeg.enabled`;
3. otherwise **any** enabled `videoN` with a piggyback encoder, lowest index
   first.

The two consumers differ: `/snapshot.jpg?chn=N` and `/stream.mjpeg?chn=N` pass
`strict = (a query string was present)` (`src/mp4/httpd.c:794`), while
`timelapse.c:185` always passes `strict = 0`.

Now apply that to a sensor-1 stream that has no piggyback encoder:

- `timelapse.channel=2` → tier 1 misses, tier 2 misses (`jpeg.enabled` defaults
  0, `config.c:366`), tier 3 hits `video0` — **sensor 0's picture, silently,
  filed under a timelapse the operator configured for sensor 1.** Wrong camera,
  no error, no log that says so.
- With `jpeg.enabled=1` it is the same outcome one tier earlier: the dedicated
  channel lives on framesource 2, which is *sensor 0's* third framesource
  (§5.2).
- `/snapshot.jpg?chn=2` is strict, so it 404s instead. Correct but useless.

So the sketch's "sensor 1 still gets snapshot/timelapse via its own dedicated
JPEG channel" does not hold, for a more basic reason than the fallback logic:
**there is no per-sensor dedicated JPEG channel to fall back to.** `ms_config`
carries exactly one `jpeg.*` section, the hub carries exactly one
`HUB_JPEG_SRC` slot, and `ing_stop` identifies the dedicated channel by the
sentinel `jc->src == HUB_JPEG_SRC` (`hal_ingenic.c:4994`). Adding a second would
mean a new config section, a new hub slot, a sensor-aware `hub_pick_jpeg_src()`
and a new teardown sentinel.

**The correct mechanism is the one that already exists: a piggyback JPEG on
sensor 1's main stream.** `jpeg_attach()` registers a JPEG encoder into the
video stream's own group, sharing its framesource — its comment (`:3445-3447`)
notes it "costs no extra rmem (no new framesource buffers), only the encoder
channel itself". Because it publishes on `HUB_JPEG_SRC_N(2)`, tier 1 matches for
`chn = 2` and both consumers get sensor 1's actual picture:

- `timelapse.channel=2` → `HUB_JPEG_SRC_N(2)`, right scene;
- `/snapshot.jpg?chn=2` → same, strict-safe;
- `hub_pick_jpeg_src`'s `chn < MS_MAX_VSTREAM` guard already admits 2 and 3 once
  `MS_MAX_VSTREAM` is 4; the hub's arrays are all sized `HUB_NSRC`
  (`hub.c:8,168,208`), so they scale with the constant.

Hence the §5.3.4 default: **`video2.jpeg_enabled = 1` under the flag**, using
encoder channel 7. Turning it off instead would leave sensor 1 with no correct
JPEG source at all and would hand its timelapse sensor 0's frames, which is
worse than spending a channel that is otherwise spare. `video3.jpeg_enabled = 0`
keeps encoder channel 8 free.

One residual, to be documented rather than fixed in v1: with the flag on, an
unqualified `/snapshot.jpg` (no `?chn=`) still resolves to sensor 0 via tiers
2/3. That is the right default — sensor 0 is the camera — but §7's
`www/x/ch0.jpg` sibling for sensor 1 should be `ch2.jpg` mapping to
`?chn=2`, not a second unqualified path.

---

## 6. Prerequisite fixes (thingino, not timps)

Four items, in `thingino-firmware-LuFi` on `ciao`. None of them are timps code;
all of them should land before M3.

### 6.1 `BR2_ISP_MIPI_SWITCH_GPIO` emits nothing — fix

`Config.soc.in:478-485` declares it as a bare `int` with `default 7`. Its
consumer is `isp_param` in `thingino.mk:341-345`:

```make
isp_param = $(strip \
  $(if $(filter y,$(BR2_$(1))),$(2)=$(BR2_$(1)_VALUE)))
```

which needs a **bool gate plus a `_VALUE` companion**. `$(filter y,7)` is empty,
so the `$(if)` never fires and `thingino.mk:410` produces the empty string.
Nothing reaches `/etc/modules.d/20-isp`.

The correct shape is three lines away in the same file —
`menuconfig BR2_ISP_IVDC_MEM_LINE` (bool) + `config BR2_ISP_IVDC_MEM_LINE_VALUE`
(int) at `Config.soc.in:511-519`. Convert to match.

**This does not block the W8U.** Its mux is on GPIO 7 (`"sensor_switch": 7`),
which is the driver default. It blocks every *other* dual-sensor board, and it
is why no profile in the tree sets the symbol — setting it would do nothing.

### 6.2 `BR2_ISP_CONFIG_HZ` — do **not** fix it the same way; delete it

`Config.soc.in:545-552` has the identical declaration bug. But the fix is not
the same, because the parameter does not exist:

```c
/* dl/ingenic-sdk/git/3.10.14/isp/t23/tx-isp-debug.c:69-71 */
#ifdef CONFIG_MULTI_SENSOR
const int isp_config_hz = CONFIG_HZ;
#endif
```

A `const int` initialised from the kernel's own `CONFIG_HZ`, with **no
`module_param()` anywhere in the SDK**. Making the Kconfig emit
`isp_config_hz=…` would convert a silent no-op into an `insmod` failure — a
strictly worse outcome, and exactly the trap someone fixing §6.1 by pattern-
matching would fall into.

Correct fix: delete the symbol and its references at `thingino.mk:420` and
`ingenic-sdk.mk:172`. Note that `configs/cameras-exp/besder_w9q_t23n_os02n10_aic8800u/`
sets `BR2_ISP_CONFIG_HZ=200000000` today (twice, duplicated) and is unaffected
only because it does nothing.

### 6.3 The open-TX-ISP branch drops the parameter entirely

`package/ingenic-sdk/ingenic-sdk.mk` has two T23 branches. Line 172
(`BR2_PACKAGE_OPEN_TX_ISP` not set) includes `$(ISP_MIPI_SWITCH_GPIO)`; line 170
(open TX-ISP) **omits it**. So even after §6.1, a board on the open-source ISP
driver cannot be told its mux GPIO. Smaller than the others, but it belongs in
the same change.

### 6.4 `ciao`'s W8U profile is single-sensor and self-contradictory

`configs/cameras/jooan_w8u_t23n_sc2336_atbm6132u/` exists on `ciao` — the brief
said it did not. But it is a **single-sensor** profile: `BR2_SENSOR_1_NAME="sc2336p"`,
no `BR2_THINGINO_IMAGE_SENSOR_QTY`, no `BR2_SENSOR_2_NAME`, no
`BR2_THINGINO_RMEM_MB`. So `CONFIG_MULTI_SENSOR` is not set and the *standard*,
not `-double`, ISP firmware links — while the same directory ships a
`prudynt.json` declaring `"multi_sensor": {"enabled": true, "count": 2}`. It
asks for dual sensor from a build that cannot do it.

The delta to upstream `master`'s working dual profile is small and exact:

```
+BR2_THINGINO_IMAGE_SENSOR_QTY=2
+BR2_SENSOR_2_NAME="sc2336ps1"
+BR2_THINGINO_RMEM_MB=34
-BR2_SENSOR_1_NAME="sc2336p"
+BR2_SENSOR_1_NAME="sc2336ps0"
-BR2_ISP_IVDC_MEM_LINE=y            /* upstream drops IVDC for the dual build */
-BR2_ISP_IVDC_MEM_LINE_VALUE=540
```

plus replacing upstream's `..._RAPTOR_CONF_*` lines with timps equivalents.
`BR2_ISP_DIRECT_MODE_0=y` and `BR2_ISP_MEMOPT_0=y` are already right on both.

Note the `-double` firmware selection is automatic from
`BR2_THINGINO_IMAGE_SENSOR_QTY != 1` — `ingenic-sdk.mk:73-74` sets
`CONFIG_MULTI_SENSOR=1`, and `isp/t23/Kbuild:1-13` then links
`3.10.14/sdk/t23/1.3.0-double/libt23-firmware.a` instead of the plain one. Both
blobs are present on disk. There is **no** `-double` variant of `libimp` — the
doubling is entirely kernel-side.

Recommendation: add the dual profile under a **new** name rather than converting
the existing one, so the single-sensor W8U build stays available as a control
for A/B testing during M3–M5.

---

## 7. WebUI

Shallow by design — per the feasibility pass this is the low-risk half, and
§1.1's global stream numbering is what makes it so.

**Scales by raising a bound.** `streamer-main.html` (video0),
`streamer-substream.html` (video1) and `streamer-encoder.js` are already
parameterised by stream index. Streams 2 and 3 reuse the same pages with a
different index behind a sensor tab strip. `streamer-osd0.html`/`osd1.html`
follow the same shape and are bounded by §5.5's two-group reality rather than
by the JS.

**Needs real thought.** Three pages currently render exactly one sensor's worth
of fields:

- `streamer-sensor.html` / `streamer-sensor.js` → `sensorN.`
- `streamer-image.html` / `streamer-image.js` → `imageN.`
- `config-photosensing.html` / `config-photosensing.js` → day/night

Each gains a sensor selector that switches which prefix the form posts to.
Because §2.3 gives `/control` the `{"sensor":{"0":{…},"1":{…}}}` shape already
used by `video`, `timps-api.js` needs no transport change.

`config-photosensing.html` is the one that needs more than a selector. §4.1
splits day/night into per-sensor measurement and a *shared* IR actuator, and the
page has to show that distinction — `switch_cmd`, `irprobe_cmd` and the new
`ir_master` belong in a "shared, one per camera" section, visually separate from
the per-sensor thresholds. Without that, an operator will set `switch_cmd` on
the sensor 1 tab and file a bug when nothing clicks.

`preview.html` needs a second video element or a stream picker, and the
snapshot endpoint at `www/x/ch0.jpg` needs a `ch2` sibling.

---

## 8. Staged plan

Sequenced the way WebRTC and the day/night ring buffer actually got built —
several small landable changes, each with its own regression gate — rather than
one branch that is unreviewable by the time it works.

**The ordering recommendation that matters most: run M3 before M1 and M2.**
M0 and M3 together need no timps code at all — they validate the *hardware
path* using the streamer that already supports it. If two time-multiplexed
SC2336s on a 64 MB T23 with 34 MB rmem do not deliver usable simultaneous
streams, that is discovered in an afternoon instead of after the config
refactor. See §9.

| | milestone | needs W8U? | gate |
|---|---|---|---|
| **M0** | thingino prerequisites: §6.1, §6.2, §6.3, §6.4 | no | `output/.../target/etc/modules.d/20-isp` contains the expected params; dual profile builds |
| **M3′** | *hardware validation with the existing streamer* — flash the upstream dual profile (raptor or prudynt), confirm both sensors enumerate, both FS channels deliver, measure real fps and free rmem | **yes** | two live streams, measured fps recorded |
| **M1** | config schema refactor: `count`/`stride` on `cfg_section`, migrate `videoN.` off its three hand-rolled sites | no | byte-identical `timps.conf` round-trip and identical `GET /control` on a live T23 (cam-kinder-links). Net diff negative. |
| **M2** | the gate and the dimensioning: Kconfig, `USE_MULTI_SENSOR`, `ISP_HAS_MULTICAM`, `MS_MAX_SENSOR`, `sensorN.`/`imageN.` + aliases, the `ISPT()` macro layer, the encoder-channel decoupling (§5.4), OSD group pool | no | flag **off**: `timpsd` size delta 0, QA clean on an existing camera. flag **on** with `sensor_count=1`: still streams, and every derived channel/group number matches §5.3.4's flag-off column. |
| **M4** | second sensor in the HAL: array-ised state, per-sensor init, `fs_kick_chn()`, video2/video3 on fs3/fs4 (enc chn 2/3), RTSP ch2/ch3 | yes | four streams; measured per-stream fps and CPU; the §5.3.4 channel/group table confirmed live, including the 9th-channel probe (risk 3) |
| **M5** | per-sensor ISP and day/night: `hal_isp_*_n()`, per-sensor `imageN.`, second `dn_thread`, `daynight.ir_master`, the scrape→IMP cutover (§4.3) | yes | both sensors track their own scene through a full dawn, no IR-cut chatter; §4.2 answered |
| **M6** | WebUI (§7), `timps.conf.example`, wiki, CHANGELOG | partly | — |

Buildable and fully testable **without** the camera: M0, M1, M2. That is the
majority of the config and plumbing risk, and all of it can be regression-tested
against the existing single-sensor T23 fleet, which is the only way to actually
honour §1's constraint rather than assert it.

Genuinely needs hardware: M3′, M4, M5 — i.e. everything that depends on a
number nobody has measured.

Per the project's standing practice, record `timpsd` before/after sizes at every
milestone, and especially the flag-off delta at M2, which must be zero.

---

## 9. Open risks and unknowns

Carried forward honestly. None of these are resolved by assumption; each names
what would resolve it.

**1. Actual dual-mode framerate. (highest)** The drivers cap at 15 and refuse
more (§5.1), but whether two time-multiplexed sensors each sustain 15 — or
whether the alternation costs more — is unmeasured, by us or by anyone. → M3′.

**2. `NR_MAX_ENC_GROUPS` on T23. — CLOSED 2026-09-15.** Still undefined in any
header we vendor, but the bound is readable in the binary:
`IMP_Encoder_CreateGroup` range-checks `< 9` (§5.3.1). The planned layout uses
groups 0–4. No sequential allocator is needed and `grp` stays equal to the
framesource channel, as today.

**3. Encoder channel budget. — REPLACED 2026-09-15.** The original entry costed
a sensor at 3 channels and quoted raptor's `max_enc_channels = 6` as the wall.
Both were wrong: the real per-sensor default is 5 channels (§5.3.2) and the real
wall is `IMP_Encoder_CreateChn`'s `< 9` (§5.3.1). What remains is genuinely
tight — the §5.3.4 defaults claim 8 of 9, leaving one spare, and enabling
`video3.jpeg` claims the ninth. raptor's 6 is a policy number, not a hardware
one, but it is the number a vendor sample apparently stayed inside, so the
possibility that something below libimp's argument check runs out before 9 is
*not* closed. → probe at M3′ by enabling raptor's own `jpeg0`/`jpeg1` ring
entries on all four streams; confirm at M4.

**3b. `RegisterChn`'s conditional group↔channel pairing. (new 2026-09-15)**
A guarded second stage in `IMP_Encoder_RegisterChn` would restrict groups to
0/1/2 with fixed channel sets (§5.3.1). It provably does not fire for the
channels timps creates today, and the whole dual-sensor layout depends on it
continuing not to fire. Cheap to check the moment four streams come up. → M4.

**4. `/proc/jz/isp/isp-m0` under `-double`.** raptor says both sensors appear in
it; the exact layout is unverified, and timps's parser silently reads whichever
block comes first (§4.3). Also affects the `headroom ≈ max_again − again`
reconstruction, which needs validating against a single-sensor camera first. →
M5, but the scrape output is worth dumping at M3′ while the board is on the
bench for other reasons.

**5. Which head carries the IR-cut and illuminator.** One of each exists; whose
they are is unknown (§4.2). Five-minute check on arrival. Determines whether
`ir_master` is 0 or 1 and whether the other sensor's day/night runs at all.

**6. `g_isp_lock` granularity.** One lock across both sensors, deliberately
(§3.5). Whether that costs measurable `/control` latency is unknown; measure at
M5 before splitting, and do not split on theory.

**7. I2C address collision — does *not* apply to this board.** thingino issue
#1557 is about two sensors sharing one address on one bus. The W8U's drivers
hardcode different addresses in separate files: `sc2336ps0.c:43` is `0x30`,
`sc2336ps1.c:41` is `0x32`, and upstream's defconfig states both. There is no
runtime knob — the file split *is* the mechanism. Still worth an `i2cdetect` on
arrival to confirm the second SC2336's ID-select pin is strapped for 0x32, since
nothing in the build can compensate if it is not.

**8. PWM availability.** raptor's docs list "PWM channels 0-3 available (needed
for frame sync timing)" as a T23 dual-sensor requirement. The W8U sets
`BR2_THINGINO_PWM_ENABLE=y`, and its motors are plain GPIO steppers
(`"is_spi": false`, `"gpio_pan": "53 52 54 51"`) so they are not competing —
but the IR/white LEDs on 59/60 and `ingenic-pwm` have not been checked against
this. → before M3′.

**9. Kernel VIC IRQ priority.** raptor's docs name a "VIC IRQ 30 priority fix in
kernel (commit `38eae569d` in thingino-linux)" as a hard requirement for T23
dual-sensor. Whether `ciao`'s kernel carries it is **unchecked**. Cheap to
verify, and a plausible explanation for "second sensor produces no frames" if
M3′ fails. → before M3′.

**10. Flash and memory budget.** `FLASH_SIZE_MB=8`, and the dual build adds a
second sensor module plus a larger `-double` firmware blob. `rmem` goes to 34 MB
on a 64 MB part against timps's single-sensor default of 23 MB, leaving ~30 MB
for everything else. Both are arithmetic that only the real image settles. →
M3′.

---

## References

- `src/config.c:1215-1238` (`g_sections[]`), `:1431`/`:1472`/`:1749` (the three
  `videoN.` sites), `:1242-1290` (`field_find`/`section_find`/`key_canonical`)
- `src/config.h:8` (`MS_MAX_VSTREAM`), `:131-136` (`ms_sensor_cfg`), `:533`
- `src/hal/hal_ingenic.c:202-205`, `:245-246` (`vchan`), `:283` (`vc->si`),
  `:424` (`act_ready_vchan`), `:434`, `:459`, `:504` (the chn-0 relatch),
  `:611` (`fs_kick_chn0`), `:693-906` (`isp_apply_image`), `:2008-2015`,
  `:3389` (`jchan`'s already-split chn/fs_chn/src), `:3403-3444`
  (`jpeg_setup`), `:3460-3476` (`jpeg_attach`), `:4231` (`rc_live_vchan`),
  `:4706` (`chn = grp = imp_chn`), `:4918-4936` and `:4992-5008` (teardown)
- `src/config.c:329`, `:334-335` (`jpeg_chn`), `:366-367` (`jpeg.*` defaults),
  `:853` (the `imp_chn` clamp)
- `src/hub.c:8`/`:83-97` (`hub_pick_jpeg_src`), `src/hub.h:155-168` (its
  documented priority order), `src/mp4/httpd.c:790-795`, `src/timelapse.c:185`
- `include/T23/1.3.0/en/imp/imp_system.h:40-70` (Device/Group/Output/Channel and
  what a bind is made of), `:264-281` (`IMP_System_Bind`);
  `imp_encoder.h:552-643` (`CreateGroup`/`CreateChn`/`RegisterChn`, the
  `NR_MAX_ENC_GROUPS` vs `NR_MAX_ENC_CHN` split)
- `libimp.so` T23 1.3.0 disassembly (`mipsel-linux-objdump -d`): `0x884bc`,
  `0x4cbe0`, `0x4e174`, `0x50a38`/`0x50a4c`, `0xab848` — the `< 9` bounds
- `dev_notes/DUAL_SENSOR_DESIGN_REVIEW_2026-09-15.md` §2a/§2b/§2c (the channel
  accounting error this document's §5 was rewritten to fix)
- `src/hal/hal.h:14-57` (the three `hal_isp_*` and `hal_isp_expo`)
- `src/hal/imp_osd.c:438` (`s->grp = stream_idx`), `:454`
- `src/daynight.c:343-530` (`dn_read`), `:1124-1195` (the automaton's locals)
- `src/isp_caps.h:35-37`, `:118-130`
- `include/T23/1.3.0/en/imp/imp_isp.h:21-25` (`IMPVI_NUM`), `:8031-8051`
  (`IMPUserSwitchgpio`), `:8069-8097` (joint modes, the f0/f3 comment),
  `:8110-8126`
- `thingino-firmware-LuFi/Config.soc.in:478-485`, `:511-519`, `:545-552`
- `thingino-firmware-LuFi/thingino.mk:341-345` (`isp_param`), `:410`, `:420`
- `thingino-firmware-LuFi/package/ingenic-sdk/ingenic-sdk.mk:73-84`, `:170-172`
- `thingino-firmware-LuFi/dl/ingenic-sdk/git/3.10.14/isp/t23/tx-isp-debug.c:27-31`,
  `:69-71`; `isp/t23/Kbuild:1-13`
- `thingino-firmware-LuFi/dl/ingenic-sdk/git/3.10.14/sensor-src/t23/sc2336.c:499`,
  `sc2336s0.c:54,336,513,791`, `sc2336s1.c:54,520,796`, `sc2336p.c:899-924`,
  `sc2336ps0.c:43,49`, `sc2336ps1.c:41,47`
- `thingino-firmware-LuFi/dl/thingino-raptor-hal/git/src/hal_common.c:1204-1310`
  (the `HAL_T23_MULTICAM` init branch), `src/hal_caps.c` T23 block
  (`max_enc_channels`, `max_osd_groups`), `src/hal_osd.c:634-635`
- `gtxaspec/raptor-docs` `27-multi-sensor.md`, `12-hal-caps.md`, `10-hal-api.md`
- `configs/cameras/jooan_w8u_t23n_sc2336_atbm6132u/` on `ciao` and on
  `themactep/thingino-firmware` `master`

---

## 10. Implementation status (prototype, 2026-09-15 evening)

Branches: `timps` `dual-sensor-m1-cfg-section`, `thingino-firmware-LuFi`
`dual-sensor-m0-kconfig`. Both pushed. No PR - this is pre-hardware.

### Landed

| | what | where | verified how |
|---|---|---|---|
| M0 | `BR2_ISP_MIPI_SWITCH_GPIO` plumbing, `BR2_ISP_CONFIG_HZ` removed (§6.1-6.3) | `35be9dfb2` | build |
| M1 | `cfg_section` indexed sections, `videoN.` migrated (§2.2) | `d4864fb` | 510 `/control` keys identical |
| M2a | `vchan` chn/fs_chn/si split (§5.4), `MS_ENC_CHN_*`, `fs_kick_chn()` | `22fc966` | as M1, + QA |
| M2b | Kconfig gate `BR2_PACKAGE_TIMPS_MULTI_SENSOR`, default n (§1) | `0b968e8a7` | flag-on T23 build |
| M2c | `MS_MAX_SENSOR`, `MS_FS_CHN_VIDEO`, encoder layout + `_Static_assert`s (§5.3.4) | `89e2de1` | as M1 |
| M2d | `sensorN.`/`imageN.` on the alias mechanism (§2.2-2.3) | `395caa3` | as M1, + alias round-trip |
| M4a | per-sensor `g_sensor[]`, AddSensor loop, `ISPT()` over all 30 tunings (§3.1-3.3) | `54e788f` | as M1, + symbol check |

All flag-off verification was on cam-garage (T31X/sc4336p, 192.168.10.21):
`/control` 510 keys identical to M1's baseline at every step, `/etc/timps.conf`
byte-identical across restarts, `timps-qa.sh --profile quick` PASS=124 WARN=2
FAIL=0 (both warnings environmental: residual WiFi UDP loss, and
`record.mode=1` with motion off). `timpsd` 368892 -> 368996 bytes, +104 total,
all of it in M2a - every later step was byte-neutral.

Flag-on is **build- and link-verified only**: it compiles for T23 with
`USE_MULTI_SENSOR=1` and links, and all 24 `IMP_ISP_MultiCamera_Tuning_*`
symbols it references are defined in the shipped `dl/ingenic-lib/.../T23/lib/
1.3.0/uclibc/5.4.0/libimp.so`. Nothing has executed. §9's risks all stand.

### Not started

- **M3'** (hardware validation) and everything downstream of it.
- The W8U dual profile (§6.4) - deliberately, it needs the board.
- **Day/night per sensor** (§4): still one `dn_thread` on sensor 0.
  `daynight.ir_master` does not exist, and `dn_read`'s `/proc/jz/isp/isp-m0`
  scrape is untouched, so under `-double` it would feed both automata one
  sensor's exposure (§4.3).
- **`/control` shape for sensor 1** (§2.3): GET emits the flat
  `{"sensor":{...}}`/`{"image":{...}}` for sensor 0 only; there is no
  `"1":{...}`. POST reaches sensor 0 through the section alias. A `sensor1.`
  key set in `/etc/timps.conf` IS parsed and applied - only the JSON surface
  is missing.
- **OSD group pool** (§5.5): `imp_osd.c` still does `s->grp = stream_idx`, so
  streams 2/3 would ask for OSD groups 2/3. Whether that works is the
  unverified `max_osd_groups` question; the existing error path disables OSD
  for the stream rather than failing.
- **Privacy masks on sensor 1** (review §3): same OSD group, same open
  question, and safety-relevant.
- **QA harness** (review §3): `timps-qa.sh` knows video0/video1 only.
- WebUI (§7), `timps.conf.example`, wiki, CHANGELOG.

### First things to check when the W8U arrives

1. §4.2 - which lens the single IR-cut and single `ir850` sit behind.
2. §9.7 - `i2cdetect` for 0x30 and 0x32.
3. Risk 3b - `IMP_Encoder_RegisterChn` for video2/video3 (groups 3 and 4).
   The guarded second stage would reject groups >= 3; it provably does not
   fire for the channels timps creates today, and the whole layout depends on
   it continuing not to. If RegisterChn fails with everything else correct,
   look here first. No renumbering rescues it - sensor 0 already holds groups
   0, 1 and 2.
4. Risk 1 - the actual per-sensor framerate.
