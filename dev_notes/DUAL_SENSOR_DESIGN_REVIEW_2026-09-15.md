# Review of DUAL_SENSOR_DESIGN_2026-09-15.md

Independent, adversarial review against `timps` `main` and `thingino-firmware-LuFi`
`ciao`. Every cited file/line/binary claim below was re-checked directly; nothing
is taken from the design's own text.

## 1. Verified accurate

- 30 distinct `IMP_ISP_Tuning_*` entry points in `hal_ingenic.c`; 19 `IMPVI_MAIN`
  sites all inside `ISP_NEW_TUNING_API` (`:700-906`, `:1185`, `:5077`).
- `nm -D libimp.so` (T23 1.3.0): 137 `IMP_ISP_MultiCamera*` symbols. 29 of the 30
  have a `MultiCamera_Tuning_` twin with the same name. The 30th,
  `SetIntegrationTime`, has neither a classic nor a multi symbol in T23's libimp,
  which is fine: it is `ISP_HAS_AE_IT_RANGE`, T10/T20/T21/T30 only.
- `IMP_ISP_AddSensor(IMPSensorInfo*)`, `IMP_ISP_EnableSensor(void)`,
  `IMP_ISP_MultiCamera_SetSwitchgpio`, `IMP_ISP_SetCameraInputMode`,
  `IMP_OSD_MultiCamera_SetRgnAttr_ISP` all present; header lines `:21-25`,
  `:8031-8051`, `:8069-8110` match (header is under
  `dl/thingino-raptor-hal/git/ingenic-headers/T23/1.3.0/en/imp/`, not
  `include/T23/...` as cited).
- All `config.c`, `config.h`, `hub.h`, `control.c`, `imp_osd.c`, `hal.h`,
  `daynight.c`, `isp_caps.h` line citations. The three hand-rolled `videoN.`
  sites are exactly `:1431`, `:1472`, `:1749`.
- Sensor drivers: `sc2336.c` is 25 fps; `sc2336ps0.c`/`ps1.c` are 15 fps max,
  refuse higher in `sensor_set_fps`, I2C 0x30/0x32 hardcoded per file.
- `tx-isp-debug.c`: `mipi_switch_gpio` default `GPIO_PA(7)`, `isp_config_hz` is a
  `const int` with no `module_param`. Kbuild links `1.3.0-double` firmware under
  `CONFIG_MULTI_SENSOR`; one `libimp.so` for both.
- `Config.soc.in` bare-int bug for `BR2_ISP_MIPI_SWITCH_GPIO` and
  `BR2_ISP_CONFIG_HZ`; `isp_param` needs bool+`_VALUE`; open-TX-ISP branch omits
  the mux param. All confirmed.
- ciao W8U profile is single-sensor with a dual `prudynt.json`; the delta to
  upstream `master` is exactly as listed.
- raptor caps: `max_enc_channels=6` ("vendor dual-sensor sample uses 6"),
  `max_osd_groups=2`, `max_isp_osd_regions=8`. raptor doc quotes accurate.
- `timps.mk`/`Makefile`: `USE_X=1` becomes `-DUSE_X` only when 1, so
  `#ifdef USE_MULTI_SENSOR` is the right test.

## 2. Inaccurate or resolved-by-this-review

**a. Encoder channel budget is wrong, and the default config does not fit.**
Compiled defaults (`config.c:334-335`, `:367`): `videoN.jpeg_enabled=1`,
`jpeg_chn = MS_MAX_VSTREAM+1+i`, `jpeg.imp_chn=2`. A single sensor today uses
encoder channels 0,1 (video), 2 (dedicated JPEG, own group 2), 3,4 (piggyback
JPEG). That is 5 channels and 3 groups, not the "2 video + 1 JPEG" the design
implicitly assumes. Two sensors with defaults = 10 channels.
`IMP_Encoder_CreateChn` begins `slti v0,a0,9` (chn < 9), so 10 is impossible at
the API level regardless of raptor's "6". §5.3's "turn sensor 1's JPEG off ->
5 with one spare" is arithmetic on the wrong base; the true minimum with sensor
0's defaults intact is 7.

**b. "No renumbering of anything that exists today" (§5.2) is false.**
timps sets `chn = grp = v->imp_chn` (`:4706`), so sensor 1's video streams on
`imp_chn` 3/4 get encoder channels 3/4, which are already the default piggyback
JPEG channels of video0/video1. This collides in the encoder namespace. Must be
settled at M2 (dimensioning), not discovered at M4.

**c. `NR_MAX_ENC_GROUPS` is not unknowable.** `IMP_Encoder_CreateGroup` starts
with `slti v0,a0,9`: groups 0..8. Risk #2 is closed; `grp` decoupling is
harmless but not needed for range reasons. Also §5.4's "groups 0,1,3,4" omits
the dedicated JPEG groups (2, and 5 for sensor 1).

**d. "T23 has 2 OSD groups" is raptor's table, not a libimp bound.**
`IMP_OSD_CreateGroup` also checks `a0 < 9`. raptor's "main streams only (T23
hardware limitation)" sentence is in the ISP-OSD section. The 2-group limit may
be real deeper down (IPU), but it is unverified and cheaply testable today on a
single-sensor T23 by creating group 2 on the dedicated JPEG channel.

**e. VIC IRQ fix (risk #9) is present.** The ciao T23 build tree's
`arch/mips/xburst/soc-t23/common/irq.c:198-200` carries the `1<<30` /
`do_IRQ(30)` hunk from `38eae569d`.

**f. `g_isp_sensor_w/h` already refuses zero.** `:1206` only stores
`sa.width>0 && sa.height>0`; `:1283` falls back to `sensor.width/height`.
§2.4's "one real change" is already how it works.

**g. Minor:** `fs_kick_chn0` callers are `:4396`, `:4645`, `:4882` (`:4397` is a
comment); `timps.conf.example:785` is `daynight.isp_path`, not a `sensor.` key;
`dn_thread` has 17 locals per its own comment, not ~60.

## 3. Gaps the design does not mention

- **Per-sensor state it missed:** `fs_use()` chn-0 relatch (`:504`,
  `chn == 0` re-applies hflip/vflip/running_mode) and the AE IT-max supervisor
  (`g_ae_it_frames/_next_us/_fails/_warned`, `:1001-1004`, armed on the chn-0
  edge). Both are single-sensor by construction and are not in §3.1/§3.4.
- **Privacy masks.** They register into the same OSD group as text OSD
  (`imp_osd.c:512`). Whatever the OSD-group outcome is, substreams on the
  second sensor silently lose privacy masks. Safety-relevant; needs a loud log
  and a documented statement, not the same treatment as a missing timestamp.
- **Runtime sensor count.** M2's gate says "flag on with `sensor_count=1`" but
  no such key, nor any rule for how the daemon decides one vs two sensors at
  runtime (`sensor1.model` empty? explicit count?), appears anywhere in §2.
- **`GET /control` shape when the flag is on.** §2.3 specifies the POST shape
  only. Existing consumers read `{"sensor":{"model":..}}` flat. Say explicitly
  that GET keeps the flat object for sensor 0 and adds `"1":{...}`.
- **QA harness.** `scripts/timps-qa.sh` has ~150 `video0/video1/ch0/ch1/sensor.`
  references and no notion of a third/fourth stream. A dual profile needs a
  harness extension, and M2's "QA clean on an existing camera" gate is the
  only place QA is mentioned.
- **AddSensor order.** raptor's T23 branch carries the vendor note "I2C bus 0
  sensor should be added last" and then loops 0..n-1 anyway. Unresolved; note
  it for the bench.
- **`jpeg_chn` default shifts under the flag.** `MS_MAX_VSTREAM+1+i` becomes
  5+i; a flag-on build changes the compiled default even at one sensor.

## 4. The "zero effect when disabled" constraint

§1 says "same code paths ... size delta against today is zero". §8's M1 refactors
`cfg_section` and the three `videoN.` sites for every build, so the daemon is
not the same code after M1; the zero-delta gate is measured at M2 against
post-M1. That is a reasonable engineering choice, but the document should say
that the invariant is "behaviour-identical after M1, byte-identical after M2",
not "bit-for-bit unaffected". `ISPT()` with the flag off expands to
`((void)(si), IMP_ISP_Tuning_X(...))`, which is not "character-for-character"
but does produce identical code; the M2 size gate is the real check.
`image[MS_MAX_SENSOR]` touches ~32 access sites in all builds; one-element
arrays keep the layout, so the size gate should hold.

## 5. Risk ranking

Agree #1 (real dual fps / CPU / memory) is top and only M3' answers it.
Disagree on the order below it: the encoder-channel budget and channel
numbering (2a/2b) should be #2, because it is a design error with a hard API
wall (chn < 9), not a "default to decide on measurement". The `isp-m0` scrape
(#4) is correctly identified and the IMP-API cutover is the right fix; the
`headroom` reconstruction is the risky part of it. Add OSD-group/privacy
behaviour as its own item. #2 (`NR_MAX_ENC_GROUPS`) and #9 (VIC) are closed.

## 6. M3' as pre-hardware validation

Sound for the layer that matters most: kernel + `-double` blob + mux + sensor
drivers + libimp multi-init + FS 3/4 + real fps. It does not exercise what
timps v1 will do differently: raptor's W8U profile runs ISP OSD, so IPU OSD
groups with four streams are untested by M3' unless raptor's config is flipped
to IPU OSD for the session; and raptor's ring table names `jpeg0/jpeg1` per
sensor, so enabling all of them during M3' is a free probe of the real encoder
channel budget. Dump `/proc/jz/isp/isp-m0` while there.

## Verdict

Sound in structure (global stream numbering, the `ISPT()` macro, config-section
generalisation, one IR owner, IMP-API day/night cutover), and M0/M1/M3' can
proceed as written. Do not start M2 until §2a/§2b are redesigned: the encoder
channel plan must be rebuilt from the real default of five channels per sensor,
with piggyback JPEG off by default under the flag and sensor 1's encoder
channels placed where they do not collide with `videoN.jpeg_chn`.

---

# Review 2 — §7 (WebUI), 2026-09-15 evening

**Provenance, stated first because it changes how much this is worth.** An
independent reviewer was spawned on a *different* model (`fable`), as the pass
above was, with the same brief: re-verify every citation, answer three specific
questions, attack the recommendation. It produced no output inside the working
window and was abandoned. **What follows is therefore an adversarial
SELF-review by the author of §7, and should be discounted accordingly** — it
cannot catch what its author did not think to look for, which is exactly what
the pass above did when it found §5's channel-budget error. §7 has not had an
independent read. Treat its verdict as provisional until it does.

Every file:line below was re-opened and checked; nothing is taken from §7's own
text.

## 1. Verified accurate

- `caps.webrtc`/`caps.rotation` really are OMITTED, not zeroed, when compiled
  out — `control.c:1496-1504` (the whole `APP` is inside `#ifdef USE_WEBRTC`)
  and `:1405-1417`. The consumer side reads the absence as its own verdict:
  `preview.html:1460-1467`, `if (!w) { whepOption.remove(); … return "absent"; }`.
  So §7.1's convention claim holds.
- `timps-api.js:140` is a literal two-element whitelist:
  `var pfx = (sec === "video" || sec === "privacy") ? sec + k : sec + "." + k;`
  §7.5's central argument stands — `{"image1":{…}}` flattens to
  `image1.brightness` through the generic arm with no edit to that file.
- M2d really did land the section aliasing §7.5 depends on:
  `config.c:1271-1276`, `SECIA("sensor","sensor.",…,MS_MAX_SENSOR,…)` and the
  same for `image`. `image1.brightness` is already a canonical key on a
  flag-on build; `image.brightness` is the element-0 alias.
- `control.c:1655-1670` emits `"osd0"`/`"osd1"` as sibling top-level keys, so
  §7.5's "precedented in the same document" is literal, not analogous.
- All four `preview.html` line citations in §7.6.1: `:847`, `:1067`, `:1836`
  are the three `streamSelect.value === "1" ? "1" : "0"` reads; `:507-509` is
  `streamLabel`; `:795-805` is the connection-pool comment; `:1480-1485` is the
  profile loop; `:2063-2068` is the WHEP-ignores-the-selector message.
- `streamer-encoder.js:26-31` is a two-entry body-id map with
  `var P = "stream" + idx + "_"`, and the field ids in
  `streamer-main.html`/`streamer-substream.html` carry that prefix. §7.2's
  correction of the original sketch is right: this is not bound-parameterised.
- `streamer-sensor.js:3-10` states in its own header that the page has no
  timps-settable fields; `timps-preview.js:928-962` fetches
  `/x/json-sensor-info.cgi`. A grep of the whole www tree finds no `sensor.*`
  POST anywhere. §7.3a holds.
- `x/ch0.jpg` hard-codes `ch1.*|dl1.*|image1.*) CHN=1` and a `chn=0|chn=1`
  query whitelist; `timps.mk:545-560` installs the one script under four names
  plus two ONVIF symlinks. §7.7 holds.
- `httpd.c:790-795` — `hub_pick_jpeg_src(cfg, chn, q != NULL)`, i.e. strict iff
  a query string was present; `hub.c:83-97` is the three-tier priority §7.7
  describes. `config.c:375` sets `video[3].jpeg_enabled=0` under the flag.
- `config.c:341-376` gives the bitrate/resolution numbers §7.6.3 computes from,
  and `:358` the `jpeg_fps=5`. `util.h:150-170` names `-DHTTP_MAX_CLIENTS=4`
  for low-RAM boards against a default of 16.
- `streamer-osd.js:17` is `/^page-streamer-osd([01])$/`; widening it is a
  one-character change as §7.2 says.
- `timps-preview.js:691-695` parses `data-stream="chN"` generically. `ch2`/`ch3`
  need nothing there.

## 2. Inaccurate or overstated

**a. §7.1's opening sentence blurs a three-tier invariant into two.** "A camera
that does not announce it executes not one line of the code below" is true, but
the honest statement is the one §4 of the review above already forced on §1:

| build | `GET /control` | rendered UI |
|---|---|---|
| flag OFF (the fleet) | **byte-identical** | identical |
| flag ON, `count:1` | **NOT identical** — carries a new `caps.sensors` key | identical |
| flag ON, `count:2` | new keys | new controls |

§7.1 does say the middle row renders identically, and that is the property M2's
own acceptance gate needs — but the section leads with a claim that is only
true of the first row. Say "byte-identical on a flag-off build,
render-identical on a flag-on build that finds one sensor".

**b. `hal_sensor_count()` does not exist.** §7.1's code block calls it as if it
were available. It is listed as dependency D2, but a reader skimming the code
block would not know that. No symbol of that name is in `src/hal/hal.h`.

**c. §7.6.1's `selectedChn()` is a real latent bug as specified, not a safe
rename.** The three sites it replaces yield a **String**. `preview.html:1086`
then does

```js
const label = chn === "1" ? "sub" : "main";
```

a strict comparison against a string literal. A `selectedChn()` that "parses and
range-clamps" naturally returns a Number — which is what the mockup does — and
that comparison then silently evaluates false for every stream, so the real-time
player's status line reads "main" forever. The other two consumers coerce and
would never show it: `:1070` is `"?chn=" + chn`, `:850` is `j.video[chn]`.
**`selectedChn()` must return a String, or `:1086` must be rewritten in the same
change.** §7.6.1 says neither. This is exactly the class of thing §7.6.1 told
the reviewer to check rather than take on faith, and it did not survive the
check.

**d. §7.4's "acceptable" understates the cost of loading the helper
everywhere.** `timps.webui.json`'s `"scripts"` array is injected into every page
at assembly time, so every page load on every single-sensor camera fetches and
parses `a/timps-sensor-select.js` to define one unused global. No rendered or
behavioural delta, but it is one extra request per page load and ~2 KB of flash
on an 8 MB part — a fleet that counts flash should be told the number, and told
that gating the install in `timps.mk` is available.

**e. §7.6.3's cost table is incomplete in the direction that matters most: it
prices the client and the uplink and says nothing about the camera.** timps
encodes **on demand**. `hal_ingenic.c:2088-2091`:

```c
/* on-demand: encode while there are consumers. The hub subscriber
 * count is the truth source (level, not edge) … */
int want = vc->active || hub_active(vc->si);
```

and `:440-445` records the measurement that motivated it — leaving a framesource
enabled with no clients was **~19 % idle CPU**. `httpd.c:797-800` says the same
thing for the JPEG path ("subscribing to the JPEG hub source is what WAKES that
encoder"). So opening the inset does not ride along on an encoder that was
running anyway: it **starts** sensor 1's substream framesource and encoder
channel, on a T23 whose dual-sensor CPU headroom §9.1 says nobody has measured.

That is plausibly a larger cost than the +17 % uplink and the +11 % decode
combined, and §7.6.3 omitted it entirely. It does not overturn the
recommendation — it strengthens "default off" and it makes the M6b gate a
*camera* CPU measurement, not only a client one — but the table is wrong as
written and should gain the row. (The mockup's summary card was corrected to
carry it; §7.6.3 itself still needs the edit.)

**f. "+11 % decode" is a bound presented as an estimate.** Pixel rate is a
proxy: it ignores per-frame overhead, a second `MediaSource`+`SourceBuffer`'s
fixed cost, and compositing a second painted surface every frame. The
*conclusion* (a client that decodes the main pane has ~9× headroom for the
inset) survives all of those; the number should be labelled as the bound it is.

## 3. Gaps

- **§7.9's dependency table is complete for the daemon and incomplete for the
  build.** D1–D8 cover `/control` and the proc node. Nothing in it covers the
  *conditional manifest* §7.8 identifies as a hazard: "gate the new nav /
  `pages` / `cgi` entries in `timps.mk`" is stated as a recommendation in prose
  but never lands in the table as a thing that must exist before M6b. A
  flag-off manifest diff of zero is an M6a gate; it is written into §8's gate
  column but not into §7.9.
- **No statement about what the second sensor means to the stats table's
  `/events?stream=stats` frames.** §7.6.1 handles `streamLabel()`, but
  `stats_json` emits one entry per *running* stream and `ensureStatsRow()`
  (`preview.html:544-577`) builds rows lazily from whatever arrives. That
  probably just works for four streams — but "probably just works" is not in the
  document either way, and the stats card is the one place a user would look to
  confirm both sensors are alive.
- **Privacy masks on the second sensor are not mentioned in §7 at all.** The
  review above (§3) flagged them as safety-relevant and §10 carries them as not
  started; `config-privacy.html` is in the nav and is not in §7's page
  inventory. Whatever §5.5's OSD-group outcome is, the privacy page will render
  controls for streams 2/3 that may silently do nothing, and §7 should say which
  page tells the user that.
- **ONVIF is declared out of scope in §7.7 without saying who owns it.** Two
  sensors and one ONVIF profile list is a real question for a camera that is
  also an NVR source; "out of scope" is the right call for M6, but it needs an
  owner and a milestone, not just an exclusion.

## Answers to the three questions the brief asked

**(a) Does anything here require a `GET /control` shape change beyond the
capability flag, and is it flagged?** **Yes, and yes — with one omission.**
Three shape changes are required and all three are in §7.9: `sensor1`/`image1`
objects (D3), the `image1.*` spelling on the config SSE (D4), and
`daynight.sensor` inside the `daynight` object (D5). D3 is additionally marked
as a **proposed amendment to §2.3** that needs that section's owner to accept or
reject, rather than being slipped in. The omission is D2: §7.1 *calls*
`hal_sensor_count()` in a code block as though it existed, and only the table
says it does not. Nothing in §7 is built on an assumed `/control` field that is
absent from the table.

Worth noting the amendment cuts *against* §2.3 rather than quietly extending it,
so it cannot be adopted by default: if §2.3's nested shape is kept instead,
`timps-api.js:140`'s whitelist must grow a `sensor`/`image` arm, and that is an
edit to a file every single-sensor camera loads.

**(b) Is there truly zero code path that executes or renders differently for a
single-sensor camera?** **Rendering: yes. Execution: no, and §7 should stop
saying "zero".** The honest list of what a flag-off camera does differently
after M6a:

1. Fetches and parses one extra ~2 KB script per page load (§7.4 / finding d).
2. Runs `selectedChn()` instead of a ternary at three sites in `preview.html`
   — behaviourally identical *only if* finding (c) is fixed; as specified it is
   a live bug.
3. Runs `streamLabel()` with one extra branch that is never taken.
4. Carries one more entry in `config-photosensing.js`'s `INT_FIELDS` whose
   `getElementById` returns `null` and is guarded on both the fill and collect
   side (`:112`, `:165`) — genuinely inert.
5. `caps.sensors` is absent, so `GET /control` is byte-identical — verified as a
   property of `#if MS_MAX_SENSOR > 1`, the same mechanism the M1/M2 510-key
   harness already measures.

Not on the list, deliberately: the PiP toggle (created in JS only under the
gate, so the shipped markup is untouched), the extra `chN.jpg` copies and the
nav/manifest entries (all gated in `timps.mk`, and a zero manifest diff is
M6a's own gate), and `timpsd`'s size (no C in M6a except the `caps` emission,
which compiles out).

One thing this self-review cannot settle and flags as an open verification:
`caps.sensors` sits in the same document as the flat `sensor` object, and
`control.c:1294-1296` warns that the CGI bridges scan for the *last* occurrence
of a key. §7.1 says to verify that against the bridge before landing and names
`caps.multisensor` as the fallback. That instruction is correct and must not be
skipped.

**(c) Is the PiP CPU/bandwidth claim justified or asserted?** **Half justified,
and it was missing its biggest term.** The uplink number is arithmetic on
`config.c:341-376` defaults and is solid: 512 on 3000 is +17 %, and the
alternatives (second main stream, MJPEG on chn2, MJPEG on chn3) are each
rejected against a specific, citable reason rather than a preference. The
connection-pool argument is the strongest part of the section because it rests
on a *measured* failure recorded in the page itself (`preview.html:795-805`),
not on a prediction.

The decode number is a bound dressed as an estimate (finding f), and the table
omitted the camera-side encoder cost entirely (finding e) — which, given
`hal_ingenic.c:440-445`'s ~19 % measurement, is very likely the dominant term.
Corrected, the tradeoff still favours the recommendation, but for a different
reason than §7.6.3 gives: the inset is affordable on the *client* by a wide
margin, and its real risk is on the *camera*, which is precisely why it must be
opt-in and why M6b's gate has to be a camera CPU measurement on the W8U.

## Verdict

The structure is right and the two corrections to the original sketch are both
real: `streamer-encoder.js` is not bound-parameterised (§7.2), and
`streamer-sensor.html` has nothing to select (§7.3a). The `caps.sensors` gate,
the sibling `sensorN`/`imageN` shape, and "picker always, PiP opt-in on the
substream" are all defensible and cited.

Do not start M6a until (c) — `selectedChn()`'s return type versus
`preview.html:1086` — is settled, and until §7.6.3 gains the camera-side
encoder row from (e). Both are one-line fixes to the document; neither changes
the recommendation.

And: this section is a self-review. It should be re-run by an independent model
before M6a lands, because the one thing a self-review structurally cannot do is
find the author's blind spot — which is what this document's first review
existed to do, and did.
