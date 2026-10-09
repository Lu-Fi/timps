#!/usr/bin/env node
// Host test for the crop geometry of the firmware's overlays page
// (thingino-firmware: package/timps/files/www/a/roi-crop.js).
//
//   node scripts/test_webui_crop.js <path/to/roi-crop.js> [control.json ...]
//
// Extracts the pure cropFit/cropDrag/cropFromZoom/zoomFromCrop/cropPct/
// cropScale/cropEffMin/cropZoomMax/cropNative by
// name; each extra argument is a saved GET /control whose caps.fcrop.sensor
// is used as one more sensor size and whose min_win/zoom_max/video sizes are
// checked against the zoom slider range. Exit 0 = all passed.
"use strict";
const fs = require("fs");

const js = process.argv[2];
if (!js || !fs.existsSync(js)) {
  console.error("usage: node test_webui_crop.js <path/to/roi-crop.js> [control.json ...]");
  process.exit(2);
}
const src = fs.readFileSync(js, "utf8");
let fails = 0, checks = 0;
function ok(cond, name) {
  checks++;
  if (!cond) { fails++; console.log("FAIL " + name); }
}

function body(name) {
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
  return src.slice(at, i + 1);
}
const names = ["cropFit", "cropDrag", "cropFromZoom", "zoomFromCrop", "cropPct", "cropScale", "cropEffMin", "cropZoomMax", "cropNative"];
const G = new Function(names.map(body).join("\n") + "\nreturn {" + names.join(",") + "};")();
const F = 64;

function valid(c, sw, sh) {
  return [c.x, c.y, c.w, c.h].every(v => Number.isInteger(v) && v % 2 === 0) &&
    c.w >= F && c.h >= F && c.x >= 0 && c.y >= 0 && c.x + c.w <= sw && c.y + c.h <= sh;
}

const sensors = [[1920, 1080], [2560, 1440], [1280, 720], [2304, 1296], [1600, 1200]];
process.argv.slice(3).forEach(f => {
  const j = JSON.parse(fs.readFileSync(f, "utf8"));
  const s = j.caps && j.caps.fcrop && j.caps.fcrop.sensor;
  ok(Array.isArray(s) && s[0] > 0 && s[1] > 0, f + ": caps.fcrop.sensor");
  ok(j.caps && j.caps.fcrop && "live" in j.caps.fcrop && "state" in j.caps.fcrop, f + ": caps.fcrop live/state");
  if (s && s[0] > 0) sensors.push(s);
  const fc = j.caps.fcrop, mn = fc.min_win;
  if (!Array.isArray(mn)) return;
  // slider end = the smallest window, not rounded and not capped at 8
  const em = G.cropEffMin(s[0], s[1], [Math.max(F, mn[0]), Math.max(F, mn[1])], +fc.zoom_max || 8);
  const zm = G.cropZoomMax(s[0], s[1], em), end = G.cropFromZoom(zm, 0.5, 0.5, s[0], s[1], em);
  ok(end.w === em[0] && end.h === em[1], f + ": slider end = min_win " + JSON.stringify(end));
  if (fc.zoom_max) ok(zm >= fc.zoom_max - 1e-9 && zm < fc.zoom_max + 0.01, f + ": slider max " + zm + " vs zoom_max " + fc.zoom_max);
  const sizes = Object.keys(j.video || {}).map(k => j.video[k]).filter((v, i) => v.enabled !== 0 && (!fc.zoomable || fc.zoomable[i]))
    .map(v => [v.width, v.height]);
  const zn = G.cropNative(s[0], s[1], sizes, zm);
  ok(zn >= 1 && zn <= zm, f + ": native threshold in range " + zn);
  if (fc.upscale_pct === 0) ok(zm - zn < 0.005, f + ": upscale_pct 0 = crop only");
  else if (fc.upscale_pct > 0) ok(zm - zn > 0.05, f + ": upscale_pct " + fc.upscale_pct + " has a zoom part");
});

let seed = 7;
const rnd = () => (seed = (seed * 1103515245 + 12345) % 2147483648) / 2147483648;

for (const [sw, sh] of sensors) {
  const tag = sw + "x" + sh;
  const a = sh / sw;
  // cropFit
  ok(JSON.stringify(G.cropFit({ x: -50, y: -9, w: 9999, h: 9999 }, sw, sh, F)) ===
    JSON.stringify({ x: 0, y: 0, w: sw, h: sh }), tag + " fit clamps to sensor");
  let c = G.cropFit({ x: sw, y: sh, w: 10, h: 3 }, sw, sh, F);
  ok(c.w === F && c.h === F && c.x === sw - F && c.y === sh - F, tag + " fit min 64 and inside");
  c = G.cropFit({ x: 101, y: 33, w: 641, h: 359 }, sw, sh, F);
  ok(valid(c, sw, sh), tag + " fit even");

  // sliders round trip
  for (const z of [1, 1.5, 2, 3.7, 8]) {
    for (const p of [0, 0.25, 0.5, 1]) {
      c = G.cropFromZoom(z, p, p, sw, sh, F);
      ok(valid(c, sw, sh), tag + " zoom " + z + " valid");
      ok(Math.abs(c.h / c.w - a) < 0.02 || c.w === F || c.h === F, tag + " zoom " + z + " keeps aspect");
      const v = G.zoomFromCrop(c, sw, sh, 8);
      if (z > 1) ok(Math.abs(v.z - z) < 0.05 && Math.abs(v.px - p * 100) <= 1, tag + " zoom " + z + " round trip");
    }
  }
  const v0 = G.zoomFromCrop({ x: 0, y: 0, w: 0, h: 0 }, sw, sh, 8);
  ok(v0.z === 1 && v0.px === 50 && v0.py === 50, tag + " empty window = zoom 1 centred");

  // drag move: delta, then clamped at the edges, size unchanged
  const o = G.cropFromZoom(2, 0.5, 0.5, sw, sh, F);
  c = G.cropDrag(o, "move", 100, -40, sw, sh, F);
  ok(c.x === o.x + 100 && c.y === o.y - 40 && c.w === o.w && c.h === o.h, tag + " move delta");
  c = G.cropDrag(o, "move", 1e5, 1e5, sw, sh, F);
  ok(c.x === sw - o.w && c.y === sh - o.h && c.w === o.w, tag + " move clamped bottom-right");
  c = G.cropDrag(o, "move", -1e5, -1e5, sw, sh, F);
  ok(c.x === 0 && c.y === 0, tag + " move clamped top-left");

  // drag size: aspect kept, top-left fixed, bounded
  const g = Math.round(sw / 8);
  c = G.cropDrag(o, "size", g, g * a, sw, sh, F);
  ok(c.x === o.x && c.y === o.y && Math.abs(c.w - (o.w + g)) <= 2 && Math.abs(c.h / c.w - a) < 0.01, tag + " size grow");
  c = G.cropDrag(o, "size", 1e5, 0, sw, sh, F);
  ok(valid(c, sw, sh) && c.x === o.x && c.y === o.y && (c.x + c.w >= sw - 2 || c.y + c.h >= sh - 2), tag + " size capped at sensor");
  c = G.cropDrag(o, "size", -1e5, -1e5, sw, sh, F);
  ok(valid(c, sw, sh) && Math.min(c.w, c.h) === F, tag + " size floor 64");

  for (let i = 0; i < 400; i++) {
    const s = G.cropFit({ x: rnd() * sw, y: rnd() * sh, w: rnd() * sw, h: rnd() * sh }, sw, sh, F);
    const m = rnd() < 0.5 ? "move" : "size";
    const d = G.cropDrag(s, m, (rnd() - 0.5) * 2 * sw, (rnd() - 0.5) * 2 * sh, sw, sh, F);
    ok(valid(s, sw, sh) && valid(d, sw, sh), tag + " random " + m + " " + JSON.stringify(s) + " -> " + JSON.stringify(d));
  }

  // preview/map mapping: % of the full-sensor surface, px round trip at 390 px
  const p = G.cropPct(o, sw, sh), bw = 358, bh = bw * a;
  ok(Math.abs(p.left + p.width / 2 - 50) < 0.2 && Math.abs(p.top + p.height / 2 - 50) < 0.2, tag + " pct centred");
  ok(Math.abs(p.left * bw / 100 * sw / bw - o.x) < 1e-6 && Math.abs(p.height * bh / 100 * sh / bh - o.h) < 1e-6, tag + " pct<->sensor round trip");
  const full = G.cropPct({ x: 0, y: 0, w: sw, h: sh }, sw, sh);
  ok(full.left === 0 && full.top === 0 && full.width === 100 && full.height === 100, tag + " full window = 100%");
}

// scale factor of the window per stream
{
  ok(G.cropScale({ w: 1920, h: 1080 }, 1920, 1080) === 1, "scale native");
  ok(G.cropScale({ w: 1280, h: 720 }, 1920, 1080) === 1.5, "scale upscaled 1.5");
  ok(G.cropScale({ w: 1920, h: 1080 }, 640, 360) < 1, "scale sub stream downscaled");
}

// caps.fcrop.min_win [w, h] per axis (T31 p=100 on 1080p, T20 sub stream)
for (const [mw, mh] of [[960, 540], [640, 360], [1920, 1080]]) {
  const sw = 1920, sh = 1080, m = [mw, mh], tag = "min_win " + mw + "x" + mh;
  let c = G.cropFit({ x: 0, y: 0, w: 100, h: 100 }, sw, sh, m);
  ok(c.w === mw && c.h === mh, tag + " fit floor per axis");
  c = G.cropFromZoom(8, 0.5, 0.5, sw, sh, m);
  ok(c.w >= mw && c.h >= mh && valid(c, sw, sh), tag + " zoom 8 kept at the minimum");
  c = G.cropDrag(G.cropFit({ x: 0, y: 0, w: sw, h: sh }, sw, sh, m), "size", -1e5, -1e5, sw, sh, m);
  ok(c.w >= mw && c.h >= mh && valid(c, sw, sh), tag + " size floor");
}

// zoom slider range and the native (no upscaling) threshold
{
  const sw = 2560, sh = 1440, main = [[1920, 1080], [640, 360]];
  // T31 upscale_pct 0: min_win = main stream, zoom_max 1.33 -> crop only
  let em = G.cropEffMin(sw, sh, [1920, 1080], 1.33), zm = G.cropZoomMax(sw, sh, em);
  ok(em[0] === 1920 && em[1] === 1080 && Math.abs(zm - 4 / 3) < 1e-9, "up0 slider 1..1.333 " + zm);
  ok(zm - G.cropNative(sw, sh, main, zm) < 0.005, "up0 no zoom part");
  let c = G.cropFromZoom(zm, 0.5, 0.5, sw, sh, em);
  ok(c.w === 1920 && c.h === 1080 && G.cropScale(c, 1920, 1080) === 1, "up0 end native 1920x1080");
  // upscale_pct 100: min_win 960x540, zoom_max 2.66
  em = G.cropEffMin(sw, sh, [960, 540], 2.66); zm = G.cropZoomMax(sw, sh, em);
  ok(em[0] === 960 && em[1] === 540 && Math.abs(zm - 8 / 3) < 1e-9, "up100 slider 1..2.667 " + zm);
  const zn = G.cropNative(sw, sh, main, zm);
  ok(Math.abs(zn - 4 / 3) < 1e-9, "up100 native at 1.333 " + zn);
  c = G.cropFromZoom(zm, 0.5, 0.5, sw, sh, em);
  ok(c.w === 960 && c.h === 540 && G.cropScale(c, 1920, 1080) === 2, "up100 end 960x540 upscaled 2x");
  c = G.cropFromZoom(zn, 0.5, 0.5, sw, sh, em);
  ok(c.w === 1920 && G.cropScale(c, 1920, 1080) === 1, "up100 threshold window native");
  c = G.cropFromZoom(1.8, 0.5, 0.5, sw, sh, em);
  ok(G.cropScale(c, 1920, 1080) > 1.3 && G.cropScale(c, 640, 360) < 1, "up100 1.8x: main upscaled, sub not");
  ok(G.cropNative(sw, sh, [[640, 360]], zm) === zm, "sub only zoomed: no zoom part up to 2.667");
  ok(G.cropNative(sw, sh, [], zm) === zm, "no zoomed stream: threshold at the end");
  ok(G.cropNative(1920, 1080, [[1920, 1080]], 2) === 1, "stream = sensor: zoom from 1x");
  // no caps: 64x64 floor, slider ends at 8; zoom_max tighter than min_win wins
  em = G.cropEffMin(sw, sh, [64, 64], 8);
  ok(G.cropZoomMax(sw, sh, em) === 8, "no zoom_max: 8x");
  em = G.cropEffMin(sw, sh, [64, 64], 2.5);
  ok(em[0] === 1024 && em[1] === 576 && G.cropZoomMax(sw, sh, em) === 2.5, "zoom_max 2.5 below min_win " + em);
  ok(G.cropZoomMax(sw, sh, [sw, sh]) === 1, "min_win = sensor: no zoom");
}

console.log((fails ? "FAILED " : "ok ") + (checks - fails) + "/" + checks);
process.exit(fails ? 1 : 0);
