#!/usr/bin/env node
// Host test for the Idle-Dark card on the firmware's timps photosensing page.
// The page lives in thingino-firmware: package/timps/files/www/a/
// config-photosensing.js (+ ../config-photosensing.html).
//
//   node scripts/test_webui_idle.js <path/to/config-photosensing.js> [control.json ...]
//
// Extracts the pure idleView() by name and checks it per state; each extra
// argument is a saved GET /control document (e.g. from the sim) that must
// give a visible card. Exit 0 = all passed.
"use strict";
const fs = require("fs");
const path = require("path");

const js = process.argv[2];
if (!js || !fs.existsSync(js)) {
  console.error("usage: node test_webui_idle.js <path/to/config-photosensing.js> [control.json ...]");
  process.exit(2);
}
const src = fs.readFileSync(js, "utf8");
let fails = 0, checks = 0;
function ok(cond, name) {
  checks++;
  if (!cond) { fails++; console.log("FAIL " + name); }
}

function extract(name) {
  const at = src.indexOf("function " + name + "(");
  if (at < 0) throw new Error(name + " not found");
  let i = src.indexOf("{", at), depth = 0, q = null;
  for (; i < src.length; i++) {
    const c = src[i];
    if (q) { if (c === "\\") i++; else if (c === q) q = null; continue; }
    if (c === '"' || c === "'") q = c;
    else if (c === "{") depth++;
    else if (c === "}" && --depth === 0) break;
  }
  return new Function(src.slice(at, i + 1) + "\nreturn " + name + ";")();
}
const idleView = extract("idleView");

const CAPS = { daynight: { available: 1, ir_idle: 1 } };
const CAPS0 = { daynight: { available: 1, ir_idle: 0 } };
function dn(ii, mode) {
  return { mode: mode === undefined ? 1 : mode,
           ir_idle: Object.assign({ cfg: 1, available: 1, reason: "", dark: 0, watched: 0, linger_left_s: 0 }, ii) };
}

// hidden
ok(idleView(undefined, CAPS, 0) === null, "no status");
ok(idleView({ mode: 1 }, CAPS, 0) === null, "older timps without ir_idle");
ok(idleView(dn({ cfg: 0, available: 0, reason: "keepalive" }), CAPS0, 0) === null, "caps 0, key off: hidden");

// shown although caps 0: the key is on, so say why and let it be switched off
let v = idleView(dn({ cfg: 1, available: 0, reason: "keepalive" }), CAPS0, 0);
ok(v && v.chip === "not available: no keepalive" && v.cls === "warning", "caps 0, key on: chip");
ok(v && /general\.fs_keepalive/.test(v.why) && /T23\/T41/.test(v.why), "keepalive fix text");
ok(idleView(dn({ cfg: 0, available: 0, reason: "disabled" }), {}, 0) !== null, "caps without daynight: shown");
ok(idleView(dn({ cfg: 0, available: 0, reason: "disabled" }), null, 0) !== null, "caps null: shown");

v = idleView(dn({ cfg: 0, available: 0, reason: "disabled" }), CAPS, 0);
ok(v && v.chip === "off" && v.cls === "secondary", "key off");

const reasons = {
  disabled: [/switching off/, /daynight\.enabled/],
  schedule: [/calendar mode/, /daynight\.mode = auto/],
  keepalive: [/no keepalive/, /general\.fs_keepalive/],
  no_illuminator: [/no switchable IR light/, /irprobe_cmd/],
  white_light: [/white light/, /White light/],
  hook: [/hook/, /idle.*verb/],
};
Object.keys(reasons).forEach(function (r) {
  const w = idleView(dn({ available: 0, reason: r }), CAPS, 0);
  ok(w && w.chip.indexOf("not available: ") === 0 && reasons[r][0].test(w.chip), r + ": chip " + (w && w.chip));
  ok(w && reasons[r][1].test(w.why), r + ": fix text");
  ok(w && w.cls === "warning", r + ": class");
});
v = idleView(dn({ available: 0, reason: "weird" }), CAPS, 0);
ok(v && v.chip === "not available: weird", "unknown reason passed through");
v = idleView(dn({ available: 0, reason: "" }), CAPS, 0);
ok(v && v.chip === "not available: unknown", "empty reason");

v = idleView(dn({ dark: 1 }), CAPS, 0);
ok(v && v.chip === "IR off (idle)" && v.cls === "primary", "dark");
v = idleView(dn({ dark: 1, watched: 1 }), CAPS, 0);
ok(v && v.chip === "IR off (idle)", "dark wins over a stale watched");
v = idleView(dn({}, 0), CAPS, 0);
ok(v && v.chip === "ready (day)", "day");
v = idleView(dn({ watched: 1, linger_left_s: 60 }), CAPS, 0);
ok(v && v.chip === "IR on · watched" && !v.countdown, "watched");
v = idleView(dn({ linger_left_s: 42 }), CAPS, 0);
ok(v && v.chip === "IR on · off in 42 s" && v.countdown, "linger");
v = idleView(dn({ linger_left_s: 42 }), CAPS, 10.2);
ok(v && v.chip === "IR on · off in 32 s", "linger counts down with age");
v = idleView(dn({ linger_left_s: 42 }), CAPS, 50);
ok(v && v.chip === "IR on · going dark" && !v.countdown, "linger over by age");
v = idleView(dn({ linger_left_s: 0 }), CAPS, 0);
ok(v && v.chip === "IR on · going dark", "linger 0");
v = idleView({ mode: "1", ir_idle: { cfg: "1", available: "1", dark: "1" } }, CAPS, 0);
ok(v && v.chip === "IR off (idle)", "string numbers");
v = idleView(dn({ cfg: true, available: true, dark: true }), CAPS, 0);
ok(v && v.chip === "IR off (idle)", "booleans");

// the page's markup the code binds to
const html = path.join(path.dirname(js), "..", "config-photosensing.html");
if (fs.existsSync(html)) {
  const h = fs.readFileSync(html, "utf8");
  ["dn-idle-card", "dn-idle-chip", "dn-idle-why", "daynight_ir_idle_off", "daynight_ir_idle_linger_s"]
    .forEach(function (id) { ok(h.indexOf('id="' + id + '"') !== -1, "html id " + id); });
  ok(/id="dn-idle-card"[^>]*hidden/.test(h), "card hidden until a status says otherwise");
  ok(/id="daynight_ir_idle_linger_s"[^>]*data-min="0"[^>]*data-max="3600"/.test(h), "linger range 0..3600");
  ok(/850 nm/.test(h) && /nobody is watching/.test(h), "850 nm note");
} else {
  ok(false, "html not found next to the js: " + html);
}

process.argv.slice(3).forEach(function (f) {
  const j = JSON.parse(fs.readFileSync(f, "utf8"));
  const w = idleView(j.daynight, j.caps, 0);
  ok(w !== null && typeof w.chip === "string", f + ": card shown");
  console.log(f + ": " + (w ? w.chip + " | " + w.why : "hidden"));
});

console.log((fails ? "FAILED " : "passed ") + (checks - fails) + "/" + checks);
process.exit(fails ? 1 : 0);
