#!/usr/bin/env node
// Host test: the firmware WebUI's error texts cover src/err_codes.h.
//
//   node scripts/test_webui_errors.js <path/to/timps-errors.js> [src/err_codes.h]
//
// thingino-firmware: package/timps/files/www/a/timps-errors.js. Fails when a
// code has no text, the WebUI knows a code the daemon does not, or ERR_V
// differs. Exit 0 = all passed.
"use strict";
const fs = require("fs");
const vm = require("vm");

const js = process.argv[2], hdr = process.argv[3] || __dirname + "/../src/err_codes.h";
if (!js || !fs.existsSync(js)) {
  console.error("usage: node test_webui_errors.js <path/to/timps-errors.js> [err_codes.h]");
  process.exit(2);
}
const h = fs.readFileSync(hdr, "utf8");
const codes = [...h.matchAll(/\bERR_[A-Z_]+\s*=\s*(\d+)/g)].map((m) => +m[1]).filter((c) => c);
const errV = +(/#define\s+ERR_V\s+(\d+)/.exec(h) || [])[1];

let fails = 0, checks = 0;
function ok(cond, name) { checks++; if (!cond) { fails++; console.log("FAIL " + name); } }

for (const soc of ["t31x", "t20x", "t23n", "t41lq", ""]) {
  const win = { thinginoUIConfig: { device: { soc } } };
  vm.runInNewContext(fs.readFileSync(js, "utf8"), { window: win });
  const E = win.timpsErrors;
  ok(E && E.v === errV, soc + ": v " + (E && E.v) + " == ERR_V " + errV);
  for (const c of codes) {
    ok(E.known(c), soc + ": text for " + c);
    const t = E.text(c);
    ok(typeof t === "string" && t.length > 8 && !/^error /.test(t), soc + ": " + c + " -> " + t);
  }
  for (let c = 100; c < 600; c++)
    if (E.known(c)) ok(codes.includes(c), soc + ": " + c + " is in err_codes.h");
  ok(E.list({ "video1.rotation": 203 })[0].startsWith("video1.rotation: "), soc + ": list()");
}
console.log("test_webui_errors: " + (checks - fails) + "/" + checks + " checks passed (" + codes.length + " codes)");
process.exit(fails ? 1 : 0);
