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

**Two passes, and the order matters.** An independent adversarial review was run
on a *different* model (`fable`), as the pass above was. While it was still out,
§7's author ran a self-review as a fallback. The independent pass then returned
and found a **silent wrong-sensor write** that the self-review had not — which
is precisely the blind spot a self-review structurally cannot cover, and the
same reason the first review of this document existed.

Both are recorded. §1–§6 below are the independent pass; §7 keeps the four
self-review findings it did not duplicate. Where the two overlap the independent
wording stands. Every `file:line` in §1–§6 was re-verified by §7's author before
the fixes were applied; the four load-bearing ones are re-checked explicitly in
§8.

---

## 1. Verified accurate (independent pass)

- `caps.webrtc` and `caps.rotation` are genuinely omitted when compiled out
  (`control.c:1405-1417`, `:1497-1504`). The other five caps entries (`record`,
  `timelapse`, `motion`, `privacy`, `backchannel`) use `available:0` instead —
  so "the convention" is one of two, but it is the right one to copy.
- `caps` is emitted first because "the CGI bridges scan for the last occurrence
  of a key" (`control.c:1294-1296`). `caps.http_max_clients` is at `:1369-1373`.
  `osd0`/`osd1` are sibling top-level keys (`:1655-1668`).
- `flattenBody()`'s whitelist is exactly `(sec === "video" || sec === "privacy")`
  (`timps-api.js:140`). `{"sensor":{"1":{"model":"x"}}}` flattens to
  `sensor.1.model`; `{"image1":{"brightness":5}}` flattens to
  `image1.brightness`; `computeCorrections()` drops any applied key not in
  `sent` (`:155`). All as claimed.
- `streamer-encoder.js:26-31` is a two-entry body-id lookup; both encoder pages
  are 175 lines with 23 `stream0_`/`stream1_` ids and `data-stream="ch0"` at
  `:30`. `timps-preview.js:66-76` and `:691-693` parse `chN` generically.
  `streamer-osd.js:17` is `/^page-streamer-osd([01])$/`. `streamer-sensor.js:3-10`
  says what §7.3a says it says; nothing in `www/a/*.js` POSTs a `sensor.*` key.
  `streamer-image.js` FIELD_MAP has 15 entries, none sensor-indexed; `REVERSE`
  is `"image." + key` (`:191-197`). `config-photosensing.js` INT_FIELDS at
  `:84-88`, both `fillTimps` (`:112`) and `collectTimps` (`:165`) guard
  `if (!el) return;` and look up `$("daynight_" + k)`, so a
  `<select id="daynight_sensor">` is the right id.
- `preview.html`: `#ms-stream` at `:165-168`; the three ternaries at `:847`,
  `:1067`, `:1836`; `streamLabel()` at `:507-509`; the pool-hang comment at
  `:795-805`; `whepTeardown` rationale at `:888-892`; WHEP profile loop at
  `:1481-1485`; WHEP-ignores-selector message at `:2063-2068`; watchdog
  `:2166-2248`; `:2250` clamp; `.ms-video-wrap` `:67-74`; pointer-events block
  `:104-123`. All line numbers correct.
- `motors-ui/preview-motors-settings.js:545-563` anchors off `#ms-stats-toggle`'s
  parent and `insertBefore(#ms-connect)`. The mockup copies this faithfully.
- `x/ch0.jpg` is a literal `0|1` whitelist on both `$0` and `chn=` (`:32-34`,
  `:44-49`); installed under four names by `timps.mk:549-553` plus two ONVIF
  symlinks. `httpd.c:794` passes `strict = (q != NULL)`; `hub_pick_jpeg_src`
  tier 1 needs `g_cfg_boot.video[chn].jpeg_enabled` (`hub.c:88-90`), so `chn=3`
  under the §5.3.4 default is `-1` → 404 → the CGI's 502.
- `config.c:341-376`: video2 1920×1080/3000 kbps, video3 640×360/512 kbps, 15
  fps under the flag (`:349-353`), piggyback JPEG q75 @ 5 fps (`:358`),
  `video3.jpeg_enabled=0` (`:375`). The uplink and pixel-rate arithmetic
  (17.07 %, 11.1 %) is correct.
- `util.h:150` names `-DHTTP_MAX_CLIENTS=4` as an example; default 16 (`:170`).
- `assemble_plugins.py` globs `var/www/a/plugins/*.webui.json` sorted (`:25`,
  `:470`), injects every manifest's `scripts` into every `**/*.html` (`:355`,
  `:378`, `:288-306`), bakes `nav` (`:221-258`), hard-fails on duplicate `pages`
  (`:114-129`) and warns on duplicate `cgi` (`:132-146`). §7.8's hazard is real,
  and a second conditionally-installed manifest with its own `name` is a clean
  mechanism.
- `MS_VSTREAM_PER_SENSOR` and `MS_SENSOR_OF_VSTREAM` exist (`config.h:17-19`).
  `hal_sensor_count()` does not (D2 is honest).

## 2. Inaccurate or overstated (independent pass)

**a. §7.5's "the POST side needs no new machinery" is false, and the sibling-key
shape has a cross-sensor write hazard §7.5 did not see.** `control.c:733-734`:

```c
const char *se, *sb = find_obj(json, end, "image", &se);
apply_ctrl_fields(&sc, ch, "image", sb?sb:json, sb?se:end, img_tbl, nimg);
```

When the body has no `"image"` object, every image field is looked up across the
**whole body**. `find_field` (`:271-296`) skips string-literal contents but never
tracks braces, so a POST of `{"image1":{"brightness":5}}` matches
`"brightness":5` inside `image1` and applies `image.brightness=5` to **sensor 0**.
That is true today, and it stays true after a D3 `image1` handler is added,
because `sb` for `"image"` is still NULL. Nested `{"image":{"1":{…}}}` (§2.3's
shape) has the same defect one level down: `get_val` over the `image` object's
range also finds leaves inside `"1":{}`. So neither shape is safe without a
depth-aware scan or a guard on the legacy flat path; the argument from
`timps-api.js` alone does not decide it. **This is the most serious finding
here: a silent write to the wrong sensor's ISP.**

**b. `sensor1.*` is not graded as restart-required.**
`key_is_restart_section()` (`control.c:237-240`) matches `videoN.` and
`"sensor."` only; `caps.restart` (`:1335`) lists `"sensor"`. A `sensor1.model`
write would return no `deferred` entry and the WebUI would not flag a restart.
Not in §7.9. (Note `videoN.` is already bounded by `MS_MAX_VSTREAM`, so
video2/video3 grade correctly for free once the bound rises.)

**c. §7.2's "widening `([01])` to `([0-3])` plus two `STREAM_NAME` entries is the
entire JS change" is wrong.** `streamer-osd.js:20` is `var OTHER = 1 - S;`, used
by the "both streams" scope at `:179` (`"osd" + OTHER` → `osd-1`), `:237`
(`bootEnabled[-1]`) and the "Also applied to" label at `:342`. An `osd2` page
would POST to `osd-1`.

**d. §7.2's "`caps.privacy.available` … already computed per stream" is wrong.**
`control.c:1387-1392` ORs `imp_osd_group_active(s)` over all streams into one
`pav`. It cannot tell `streamer-osd2.html` whether *its* group exists.

**e. §7.6.1 misses three more read sites of the same `chn`.** Beyond the three
ternaries, `chn === "1" ? "sub" : "main"` appears at `preview.html:1086`,
`:2017`, `:2018`. A `selectedChn()` that returns a number (as the mockup's did)
makes all three false for the sub stream: a single-sensor camera would log and
display "Connected (main stream)" while showing the sub stream. Not
"total-equivalent" unless it returns a string or those sites change.

**f. §7.6.3's connection arithmetic undercounts.** Each preview tab opens
`/stream.mp4`, `/events?stream=motion` (`preview-motion.js:237`) and, with the
stats card open, `/events?stream=stats` (`preview.html:664`) — all on port 8880.
`util.h:162` says "3+ slots per open tab". Two tabs with PiP is 8, not "exactly
at the wall" of 6.

**g. §7.6.3's "4 on the low-RAM boards in this fleet" is unsubstantiated.**
`-DHTTP_MAX_CLIENTS=4` appears only as a comment example (`timps/Makefile:24`,
`util.h:150`); `timps.mk` sets no `MAX_CLIENTS`, nor does anything under `user/`
or `configs/`. Every fleet camera runs 16 unless a per-profile fragment I could
not find says otherwise.

**h. §7.6.3 omits the camera-side cost that is not uplink.** Encoders and
framesources are on-demand (`hal_ingenic.c:79-90`, `:376`; `hub.c:281-297`). The
inset wakes encoder channel 3 / fs 4 that nobody else is pulling, and costs an
HTTP worker thread plus a fanqueue (`util.h:154`). The table prices only bytes
on the wire. *(The self-review reached the same conclusion independently, from
`hal_ingenic.c:440-445`'s ~19 %-idle-CPU measurement and `httpd.c:797-800`.)*

**i. §7.6.5's default corner collides with native controls.**
`preview.html:213` is `<video … controls …>`. A bottom-right inset sits on the
native control bar's fullscreen/PiP end. Top corners avoid it; the default
should not be `br`.

**j. Citation errors.** §7.1's quote *"key absent → USE_WEBRTC=0, no endpoint
exists here → remove it"* is `preview.html:1440`, not `control.c:1437-1447`
(that range is the backchannel comment). D6 cites `config.c:375` for
`video2.jpeg_enabled=1`; `:375` is `video[3].jpeg_enabled=0` — video2's 1 is the
loop default at `:358`. §7.7's "~2.5 KB" is 3557 bytes.

**k. §7.5's "quiet wrong answer, which is worse [than a crash]" overstates the
corrections miss.** Corrections are informational by contract
(`timps-api.js:176-178`); the miss is a lost toast and an un-snapped slider. The
real "quiet wrong answer" is finding (a), which §7.5 does not mention.

**l. §7.7's flash-cost argument is weaker than stated.** The extra
`ch2.jpg`/`dl2.jpg` are byte-identical copies; mksquashfs deduplicates identical
files by default, so unless thingino passes `-no-duplicates` the cost is a
directory entry, not 2×3.5 KB. Gating the install on the flag is still right,
for the "502-only endpoint" reason.

## 3. Gaps (independent pass)

- **The `caps.sensors`/`"sensor"` collision worry can be closed, not deferred.**
  No shipped CGI scans `/control` for `"sensor"`: `timps-imp.cgi:96` parses only
  its own `POST_DATA` for `"val"`, `timps-heartbeat.sh` reads nothing keyed,
  `json-sensor-info.cgi` reads `/proc`. The bridges `control.c:1294` refers to
  are the `json-prudynt*.cgi` files `timps.mk:522-528` purges.
- **D5 (`daynight.sensor`) must be flag-gated on the daemon side** or the
  510-key GET harness breaks on every flag-off build (`fillTimps` reads `dn[k]`
  from the `daynight` object; if `cfg_fields_daynight` gains `sensor`
  unconditionally, the key appears). §7.9's D5 row does not say this; §4.1's
  "compiled out entirely" implies it. Same for `hal_sensor_count()` — it must
  not exist in flag-off builds, or `timpsd` grows.
- **Global-script injection is a per-page delta on every camera.**
  `assemble_plugins.py:355/378` injects manifest `scripts` into every HTML file
  including `401.html`. `a/timps-sensor-select.js` would be one more
  cache-busted fetch per page load fleet-wide. Only two pages use it and both
  are timps-owned; a `<script>` tag in `streamer-image.html` and
  `config-photosensing.html` is zero-delta everywhere else. §7.4's choice of the
  manifest is unmotivated and worse.
- **A cheaper inset than trimmed MSE exists and is unpriced.**
  `preview.html:1853-1854` already falls back to
  `video.src = "/stream.mp4?chn=…"` progressive playback. A
  `<video muted autoplay playsinline src=…>` inset is ~15 lines, no
  MediaSource, no pump/evict, nothing shared. Its cost is uncontrolled live-edge
  drift and browser-managed buffering. For an orientation inset that may be
  acceptable; it should be measured on the bench before 120 lines of MSE are
  written. A polled `<img src=/snapshot.jpg?chn=2>` at ≤ 0.5 fps is a third
  option that holds **no** long-lived 8880 connection — the cost §7.6.3 itself
  ranks first — at roughly comparable bandwidth.
- `hub_pick_jpeg_src` gates on `g_cfg_boot`, so a live `video3.jpeg_enabled=1`
  does not make `ch3` work until restart. §7.7 is right to ship `ch2` only;
  worth one sentence.

## 4. The three questions (independent pass)

**(a) GET/POST shape dependencies beyond the flag.** §7.9 is incomplete.
Missing: (1) a POST handler for `image1`/`sensor1` objects **and** a guard on the
legacy whole-body image scan (`control.c:733-734`) — without both, sensor-1
writes land on sensor 0; (2) `key_is_restart_section` and `caps.restart`
accepting `sensor1` (`:237-240`, `:1335`); (3) D5's `daynight.sensor` being
`#if`-gated so flag-off GET stays key-identical; (4) `hal_sensor_count()`
likewise. D1–D4 as listed are honest; the picker's use of `video["2"]`/`["3"]`
needs nothing new. §7.9 also does not say that (1) invalidates §7.5's "no new
machinery" claim.

**(b) Zero code-path delta for single-sensor cameras.** Not quite. Honest
statement:

- `selectedChn()` — equivalent only if it returns a string or
  `:1086`/`:2017`/`:2018` are updated (2e).
- `streamLabel()` — fine if the two existing strings are the un-gated arms.
- `INT_FIELDS` — inert, verified.
- Manifest `scripts` injection — one extra fetch per page on every camera;
  avoidable (§3).
- `caps.sensors` naming — no collision, verified negative.
- New nav/pages/cgi and `chN.jpg` copies — zero-delta only if gated in
  `timps.mk`; §7.8 says so; the assembler supports a second manifest cleanly.
- `timpsd` size — zero only if D2 and D5 are `#if`-gated, which §7 does not
  state.
- On-disk: `config_write_keys` writes the raw key (`config.c:2210`) even though
  `key_canonical` flips to `image0.` at compile-time `count==2` (`:1362-1368`),
  so flag-on/count:1 keeps `image.*` on disk and on the wire (`echo_add` uses
  the raw key, `control.c:508`). §7.1's count/max split holds at the UI level.

**(c) The CPU/bandwidth claim.** Arithmetic correct; comparison incomplete.
Uplink +17 % is right for bytes. "Decode Mpx/s" is a loose proxy: decode is
hardware on anything that plays 1080p; the per-stream browser cost is the fetch
reader + `appendBuffer` loop, which runs per fragment (`fmp4.h:56` suggests
per-frame), so JS-side overhead is closer to +100 % of a small number than
+11 %. Camera side omits the on-demand encoder wake, the worker thread and the
fanqueue (2h). The pool argument is real (`preview.html:795-805` is a measured
hang) but the count is 3–4 per tab, not 2–3 (2f). The `HTTP_MAX_CLIENTS=4` claim
is unsupported (2g). Conclusion survives — sub is far cheaper than main — but
"with real numbers rather than an assertion" is only true of the uplink column.

## 5. §7.5's amendment and §7.6's recommendation (independent pass)

§7.5: the `imageN` sibling shape is the better wire spelling (matches
`osd0`/`osd1`, matches the config key, no `timps-api.js` edit) — **but it must
ship with the legacy-scan guard or it is worse than §2.3's shape, not better.**
§2.3's nested shape does not depend on anything the amendment breaks; the
amendment changes only the JSON surface. **Accept it conditionally.**

§7.6: picker-always is right. PiP opt-in is defensible but under-explored: price
the progressive `<video>` and polled-snapshot variants first, default to a top
corner, and add `touch-action: none`. The "wide lens tells you where to point
the PTZ" use case (§7.6.4) is the strongest argument in the section and is
sound.

## 6. Mockup `dual-sensor-preview-mockup.html` (independent pass)

Faithful where it claims to be: the Bootstrap 5.3.8 hash matches
`thingino-webui/files/www/tool-send2.html:13`; card/header markup and
`.ms-video-wrap` rules match `preview.html:66-140`, `:150-231`; button anchoring
matches `preview-motors-settings.js:545-563`; `.jst`/`#motor` carry no z-index in
`main.css:851-870`, so `z-index:5` does stack above them. Drag/snap math and
`swap()`'s role preservation are correct; pointer-capture release and
`pointercancel` are handled; `boot()`'s teardown is complete.

Bugs: no `touch-action: none` on `.ms-pip` — touch drag scrolls the page on a
phone; `transition: inset` never animates (`auto` ↔ px), dead; `selectedChn()`
returns a number (2e); `labelFor(chn)` under `!c` returns "Main stream", not
`streamLabel()`'s "Main (chn0)", so it does not model the stats-table claim;
`http_max_clients` is present in the fake control and unused; the
visibilitychange/pagehide teardown §7.6.3 calls mandatory is not modelled.

## 7. Self-review findings the independent pass did not duplicate

- **The zero-delta claim has three tiers, not two** — flag-off is
  byte-identical, flag-on/`count:1` carries a new `caps.sensors` key and is only
  *render*-identical, flag-on/`count:2` is new. §7.1 led with a claim true only
  of the first row. Same correction the review above made to §1
  ("behaviour-identical after M1, byte-identical after M2"). Fixed: §7.1 now
  carries the table.
- **The stats table is unaddressed either way.** §7.6.1 handles `streamLabel()`,
  but `stats_json` emits one entry per *running* stream and `ensureStatsRow()`
  (`preview.html:544-577`) builds rows lazily from whatever arrives. That
  probably just works for four streams — but the stats card is the one place a
  user would look to confirm both sensors are alive, and "probably" is not in
  the document.
- **ONVIF is excluded without an owner.** §7.7 declares the
  `onvif/image.cgi`/`image1.cgi` symlinks out of scope, which is the right call
  for M6, but two sensors against one ONVIF profile list is a real question for
  a camera that is also an NVR source. It needs an owner and a milestone, not
  just an exclusion.
- **Mockup: the unguarded `localStorage` read at module scope killed the whole
  IIFE** when the page was loaded from an opaque origin — `SecurityError`, not
  `null`. Fixed with `lsGet`/`lsSet`/`ssSet` wrappers. Worth recording because
  the real `preview.html` reads storage unguarded at `:349`, `:372`, `:390`,
  `:459`, `:690`: harmless over `http://`, but the same failure mode hits anyone
  who opens a saved copy of the page.

## 8. Re-verification of the four load-bearing findings

Checked directly by §7's author before applying the fixes, because these are the
ones that change the plan:

- **2a** — `control.c:733-734` is verbatim as quoted, and `find_field`
  (`:279-296`) is a literal-to-literal scan with **no brace tracking** (its own
  comment says it skips string *contents*, nothing about nesting). The
  wrong-sensor write is real.
- **2b** — `control.c:235-240` matches `videoN.` (bounded by `MS_MAX_VSTREAM`)
  and `sensor.` only. Confirmed.
- **2d** — `control.c:1387-1392` is a loop ORing into one `pav`. Confirmed.
- **2e** — `grep -n 'chn === "1"' preview.html` returns exactly `:1086`,
  `:2017`, `:2018`. Confirmed.
- **2g** — `grep -rn HTTP_MAX_CLIENTS` across both repos' build files returns
  only `timps/Makefile:24`, a comment. Confirmed; §7.6.3's claim was wrong.
- **2j** — `stat -c %s x/ch0.jpg` = 3557. Confirmed.

## Verdict

§7's reading of the WebUI files is accurate at the line level and its structural
calls — one gate key, sibling `imageN`, two more encoder pages, picker-always —
are right. It is **wrong about the daemon's POST path**: the legacy whole-body
image scan (`control.c:733-734`) turns the proposed `image1` shape into a silent
write to sensor 0, and neither that nor the `sensor1` restart-grading gap was in
§7.9.

Do not start M6a until the legacy-scan guard and the `sensor1` restart grading
are designed. The rest — OSD `OTHER`, the per-stream privacy signal, the three
`chn === "1"` sites, the SSE connection count, manifest-script injection, the
D2/D5 flag gates, the default corner — are corrections to make in the same
rewrite pass. All of them have been applied to §7 as of commit following this
one; the two blockers are recorded as D11/D12 and are **not** fixed, only
specified.
