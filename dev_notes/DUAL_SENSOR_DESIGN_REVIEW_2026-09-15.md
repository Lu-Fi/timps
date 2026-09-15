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
