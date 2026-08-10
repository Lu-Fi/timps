#!/usr/bin/env python3
"""qa_html_report.py - turn a timps-qa.sh output directory into a self-contained HTML report.

Usage:
    scripts/qa_html_report.py <qa-output-dir> [-o report.html]

Reads summary.txt (the same PASS/WARN/FAIL/SKIP log the terminal shows, with
ANSI color codes intact) plus a handful of the raw artifacts the script also
drops in that directory (snapshot JPEGs, version.json, control.json) and
produces one self-contained .html file - no external assets, images embedded
as data: URIs - that can be opened directly in a browser or attached/emailed.

Everything this reads is already written by timps-qa.sh for every run; this
is purely a presentation layer over existing output, not a new data source.
"""
import argparse
import base64
import html
import json
import re
import sys
from pathlib import Path

ANSI_RE = re.compile(r"\x1b\[[0-9;]*m")
SECTION_RE = re.compile(r"^=== (.+?) ===$")
RESULT_RE = re.compile(r"^\[(PASS|WARN|FAIL|SKIP|skip)\]\s*(.*)$")

LEVEL_CLASS = {
    "PASS": "pass",
    "WARN": "warn",
    "FAIL": "fail",
    "SKIP": "skip",
    "skip": "skip",
}
LEVEL_LABEL = {
    "PASS": "PASS",
    "WARN": "WARN",
    "FAIL": "FAIL",
    "SKIP": "SKIP",
    "skip": "SKIP",
}


def strip_ansi(text: str) -> str:
    return ANSI_RE.sub("", text)


def parse_summary(path: Path):
    """Parse summary.txt into (header_lines, sections, totals, result_line).

    sections: list of {"title": str, "items": [ {"level": str|None, "text": str,
    "notes": [str, ...]} ]}
    A "note" is an indented, non-result line that immediately follows (and thus
    documents) the preceding result line - e.g. the "restored 22/22 ..." line
    under an "image: all 22 live key(s) applied" PASS.
    """
    raw = path.read_text(errors="replace")
    lines = [strip_ansi(l) for l in raw.splitlines()]

    header = []
    sections = []
    totals = {"PASS": 0, "WARN": 0, "FAIL": 0, "SKIP": 0}
    result_line = ""
    cur = None
    in_summary = False

    for line in lines:
        stripped = line.strip()
        if not stripped:
            continue

        m = SECTION_RE.match(stripped)
        if m:
            title = m.group(1)
            if title == "SUMMARY":
                in_summary = True
                cur = None
                continue
            in_summary = False
            cur = {"title": title, "items": []}
            sections.append(cur)
            continue

        if in_summary:
            tot_m = re.match(
                r"PASS=(\d+)\s+WARN=(\d+)\s+FAIL=(\d+)\s+SKIP=(\d+)", stripped
            )
            if tot_m:
                totals = {
                    "PASS": int(tot_m.group(1)),
                    "WARN": int(tot_m.group(2)),
                    "FAIL": int(tot_m.group(3)),
                    "SKIP": int(tot_m.group(4)),
                }
                continue
            if stripped.startswith("RESULT:"):
                result_line = stripped[len("RESULT:") :].strip()
                continue
            continue

        m = RESULT_RE.match(stripped)
        if m:
            level, text = m.group(1), m.group(2)
            level_key = "SKIP" if level == "skip" else level
            item = {"level": level_key, "text": text, "notes": []}
            if cur is None:
                cur = {"title": "(preamble)", "items": []}
                sections.append(cur)
            cur["items"].append(item)
            continue

        # non-result line: either a top-of-file header (before any section) or
        # an indented note following the last result item in the current section
        if cur is None:
            header.append(stripped)
        elif cur["items"]:
            cur["items"][-1]["notes"].append(stripped)
        else:
            # a bare info line with no preceding result in this section (e.g.
            # "main (ch0):" / "h264,video,1920,1080,...")
            cur["items"].append({"level": None, "text": stripped, "notes": []})

    return header, sections, totals, result_line


def b64_image(path: Path) -> str | None:
    if not path.is_file():
        return None
    ext = path.suffix.lstrip(".").lower()
    mime = {"jpg": "image/jpeg", "jpeg": "image/jpeg", "png": "image/png"}.get(
        ext, "application/octet-stream"
    )
    data = base64.b64encode(path.read_bytes()).decode("ascii")
    return f"data:{mime};base64,{data}"


def load_json_safe(path: Path):
    if not path.is_file():
        return None
    try:
        return json.loads(path.read_text(errors="replace"))
    except Exception:
        return None


def render_html(qa_dir: Path, header, sections, totals, result_line) -> str:
    esc = html.escape
    cam = ""
    ts = ""
    if header:
        m = re.search(r"cam=(\S+)", header[0])
        if m:
            cam = m.group(1)
        m2 = re.search(r"profile=(\S+)", header[0])
        profile = m2.group(1) if m2 else ""
        # trailing tokens after "out=..." are the date
        parts = header[0].split()
        ts = " ".join(parts[-6:]) if len(parts) >= 6 else ""
    else:
        profile = ""

    version_info = load_json_safe(qa_dir / "version.json")
    reported_version = None
    if isinstance(version_info, dict):
        reported_version = version_info.get("version")

    overall = "fail" if totals["FAIL"] else ("warn" if totals["WARN"] else "pass")
    overall_label = {
        "pass": "PASS",
        "warn": "PASS WITH WARNINGS",
        "fail": "FAIL",
    }[overall]

    snap0 = b64_image(qa_dir / "snap_0.jpg")
    snap1 = b64_image(qa_dir / "snap_1.jpg")

    body_sections = []
    for sec in sections:
        rows = []
        for item in sec["items"]:
            level = item["level"]
            text = esc(item["text"])
            notes_html = ""
            if item["notes"]:
                notes_html = "<ul class='notes'>" + "".join(
                    f"<li>{esc(n)}</li>" for n in item["notes"]
                ) + "</ul>"
            if level is None:
                rows.append(f"<div class='info-line'>{text}{notes_html}</div>")
            else:
                cls = LEVEL_CLASS[level]
                label = LEVEL_LABEL[level]
                rows.append(
                    f"<div class='result-row {cls}'>"
                    f"<span class='badge {cls}'>{label}</span>"
                    f"<span class='result-text'>{text}</span>"
                    f"{notes_html}</div>"
                )
        counts = {"PASS": 0, "WARN": 0, "FAIL": 0, "SKIP": 0}
        for item in sec["items"]:
            if item["level"] in counts:
                counts[item["level"]] += 1
        badge_bits = []
        for lv in ("PASS", "WARN", "FAIL", "SKIP"):
            if counts[lv]:
                badge_bits.append(
                    f"<span class='mini-badge {LEVEL_CLASS[lv]}'>{counts[lv]} {lv}</span>"
                )
        body_sections.append(
            f"<section class='qa-section'>"
            f"<h2>{esc(sec['title'])} {''.join(badge_bits)}</h2>"
            f"<div class='section-body'>{''.join(rows)}</div>"
            f"</section>"
        )

    snaps_html = ""
    if snap0 or snap1:
        cells = []
        if snap0:
            cells.append(
                f"<figure><img src='{snap0}' alt='chn0 snapshot'><figcaption>Kanal 0 (Hauptstream)</figcaption></figure>"
            )
        if snap1:
            cells.append(
                f"<figure><img src='{snap1}' alt='chn1 snapshot'><figcaption>Kanal 1 (Substream)</figcaption></figure>"
            )
        snaps_html = f"<section class='qa-section'><h2>Snapshots</h2><div class='snap-grid'>{''.join(cells)}</div></section>"

    return f"""<!doctype html>
<html lang="de">
<head>
<meta charset="utf-8">
<title>timps QA Report — {esc(cam)}</title>
<style>
  :root {{
    --bg: #0f1115; --panel: #171a21; --border: #2a2f3a; --text: #e6e8ec; --muted: #9aa3b2;
    --pass: #2ecc71; --warn: #f5a623; --fail: #e74c3c; --skip: #6c7a91;
  }}
  * {{ box-sizing: border-box; }}
  body {{
    background: var(--bg); color: var(--text); font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, sans-serif;
    margin: 0; padding: 2rem; line-height: 1.5;
  }}
  .wrap {{ max-width: 980px; margin: 0 auto; }}
  header.report-header {{
    background: var(--panel); border: 1px solid var(--border); border-radius: 12px;
    padding: 1.5rem 2rem; margin-bottom: 1.5rem;
  }}
  h1 {{ margin: 0 0 0.25rem; font-size: 1.5rem; }}
  .meta {{ color: var(--muted); font-size: 0.9rem; }}
  .meta code {{ color: var(--text); }}
  .verdict {{
    display: inline-block; margin-top: 1rem; padding: 0.4rem 1rem; border-radius: 999px;
    font-weight: 700; font-size: 0.95rem; letter-spacing: 0.02em;
  }}
  .verdict.pass {{ background: rgba(46,204,113,0.15); color: var(--pass); border: 1px solid var(--pass); }}
  .verdict.warn {{ background: rgba(245,166,35,0.15); color: var(--warn); border: 1px solid var(--warn); }}
  .verdict.fail {{ background: rgba(231,76,60,0.15); color: var(--fail); border: 1px solid var(--fail); }}
  .totals {{ display: flex; gap: 1rem; margin-top: 1rem; flex-wrap: wrap; }}
  .totals .tot {{ font-size: 0.85rem; color: var(--muted); }}
  .totals .tot b {{ color: var(--text); font-size: 1.1rem; }}
  section.qa-section {{
    background: var(--panel); border: 1px solid var(--border); border-radius: 10px;
    padding: 1rem 1.5rem; margin-bottom: 1rem;
  }}
  section.qa-section h2 {{
    font-size: 1rem; margin: 0 0 0.75rem; display: flex; align-items: center; gap: 0.5rem; flex-wrap: wrap;
    border-bottom: 1px solid var(--border); padding-bottom: 0.5rem;
  }}
  .result-row {{ padding: 0.35rem 0; display: flex; align-items: baseline; gap: 0.6rem; flex-wrap: wrap; }}
  .result-text {{ flex: 1; min-width: 200px; }}
  .info-line {{ color: var(--muted); font-size: 0.9rem; padding: 0.2rem 0; font-family: ui-monospace, monospace; }}
  .badge {{
    font-size: 0.7rem; font-weight: 700; padding: 0.15rem 0.5rem; border-radius: 5px; letter-spacing: 0.03em;
    flex-shrink: 0;
  }}
  .badge.pass {{ background: rgba(46,204,113,0.15); color: var(--pass); }}
  .badge.warn {{ background: rgba(245,166,35,0.15); color: var(--warn); }}
  .badge.fail {{ background: rgba(231,76,60,0.18); color: var(--fail); }}
  .badge.skip {{ background: rgba(108,122,145,0.18); color: var(--skip); }}
  .mini-badge {{ font-size: 0.7rem; font-weight: 600; padding: 0.1rem 0.45rem; border-radius: 999px; }}
  .mini-badge.pass {{ background: rgba(46,204,113,0.12); color: var(--pass); }}
  .mini-badge.warn {{ background: rgba(245,166,35,0.12); color: var(--warn); }}
  .mini-badge.fail {{ background: rgba(231,76,60,0.15); color: var(--fail); }}
  .mini-badge.skip {{ background: rgba(108,122,145,0.15); color: var(--skip); }}
  ul.notes {{ margin: 0.2rem 0 0.2rem 0; padding-left: 1.1rem; color: var(--muted); font-size: 0.85rem; width: 100%; }}
  .result-row.fail .result-text {{ color: var(--fail); font-weight: 600; }}
  .result-row.warn .result-text {{ color: var(--warn); }}
  .snap-grid {{ display: flex; gap: 1rem; flex-wrap: wrap; }}
  .snap-grid figure {{ margin: 0; max-width: 320px; }}
  .snap-grid img {{ width: 100%; border-radius: 8px; border: 1px solid var(--border); display: block; }}
  .snap-grid figcaption {{ font-size: 0.8rem; color: var(--muted); margin-top: 0.35rem; text-align: center; }}
  footer {{ color: var(--muted); font-size: 0.8rem; text-align: center; margin-top: 2rem; }}
</style>
</head>
<body>
<div class="wrap">
  <header class="report-header">
    <h1>timps QA Report</h1>
    <div class="meta">
      Kamera: <code>{esc(cam)}</code> &middot; Profil: <code>{esc(profile)}</code>
      {f" &middot; Version: <code>{esc(reported_version)}</code>" if reported_version else ""}
      <br>{esc(ts)}
    </div>
    <div class="verdict {overall}">{esc(overall_label)}</div>
    <div class="totals">
      <div class="tot"><b>{totals['PASS']}</b> PASS</div>
      <div class="tot"><b>{totals['WARN']}</b> WARN</div>
      <div class="tot"><b>{totals['FAIL']}</b> FAIL</div>
      <div class="tot"><b>{totals['SKIP']}</b> SKIP</div>
    </div>
  </header>
  {snaps_html}
  {''.join(body_sections)}
  <footer>Erzeugt aus {esc(str(qa_dir))}/summary.txt</footer>
</div>
</body>
</html>
"""


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("qa_dir", type=Path, help="timps-qa.sh output directory")
    ap.add_argument("-o", "--output", type=Path, default=None, help="output .html path (default: <qa_dir>/report.html)")
    args = ap.parse_args()

    summary_path = args.qa_dir / "summary.txt"
    if not summary_path.is_file():
        print(f"error: {summary_path} not found", file=sys.stderr)
        sys.exit(1)

    header, sections, totals, result_line = parse_summary(summary_path)
    out_html = render_html(args.qa_dir, header, sections, totals, result_line)

    out_path = args.output or (args.qa_dir / "report.html")
    out_path.write_text(out_html)
    print(f"wrote {out_path} ({len(out_html)} bytes)")


if __name__ == "__main__":
    main()
