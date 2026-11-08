#!/bin/sh
# Compare the config keys defined in src/config.c with the keys documented in
# docs/ai/config-keys.md. Counting as in config-keys.md §23: indexed keys once
# per pattern (video<N>, osd<S>.<N>, privacy<S>.<N>, roi<S>.<N>), the explicit
# set_kv() keys and the retired daynight.* names included; aliases and the
# timps.conf.example keys (active and commented out) separately.
# Exit 0 = in sync, 1 = a list is non-empty, 2 = parse error.
# usage: scripts/check-config-keys.sh [repo-root]
ROOT=${1:-$(cd "$(dirname "$0")/.." && pwd)}
exec python3 - "$ROOT" <<'PY'
import re, sys

def die(msg):
    sys.stderr.write("check-config-keys: %s\n" % msg)
    sys.exit(2)

def read(rel):
    try:
        with open(root + "/" + rel, encoding="utf-8") as f:
            return f.read()
    except OSError as e:
        die(str(e))

root = sys.argv[1]
src = read("src/config.c")
doc = read("docs/ai/config-keys.md")

def strip_comments(s):
    s = re.sub(r"/\*.*?\*/", "", s, flags=re.S)
    return re.sub(r"//[^\n]*", "", s)

c = strip_comments(src)

INDEXED = {"video_fields": "video<N>.", "osd_item_fields": "osd<S>.<N>.",
           "privacy_fields": "privacy<S>.<N>.", "roi_fields": "roi<S>.<N>."}
prefix = dict(INDEXED)
for pfx, tbl in re.findall(r'SEC\(\s*"([a-z]+\.)"\s*,[^;]*?,\s*(\w+_fields)\s*\)', c):
    prefix[tbl] = pfx

def norm(k):
    for pat, rep in ((r"^video\d\.", "video<N>."), (r"^osd\d\.\d\.", "osd<S>.<N>."),
                     (r"^osd\d\.", "osd<S>.<N>."), (r"^privacy\d\.\d\.", "privacy<S>.<N>."),
                     (r"^roi\d\.\d\.", "roi<S>.<N>.")):
        k, n = re.subn(pat, rep, k)
        if n:
            break
    return k

table, alias = {}, set()
for tbl, body in re.findall(r"static const cfg_field (\w+)\[\]\s*=\s*\{(.*?)\n\};", c, re.S):
    if tbl not in prefix:
        die("no section prefix for table %s" % tbl)
    for name, al in re.findall(r'\bFS?\s*\(\s*"(\w+)"\s*,\s*(?:"(\w+)"|0)', body):
        table[prefix[tbl] + name] = tbl
        if al:
            alias.add(prefix[tbl] + al)
if not table:
    die("no cfg_field table found in src/config.c")

m = re.search(r"static void set_kv\(.*?\n\}\n", c, re.S)
if not m:
    die("set_kv() not found")
body = m.group(0)
explicit, retired, cur = set(), set(), ""
tok = re.compile(r'strncmp\(key,\s*"(\w+\.)"|strcmp\(key,\s*"([\w.]+)"\)|'
                 r'strcmp\(k(?:ey\+\d+)?,\s*"(\w+)"\)|gone\[\]\s*=\s*\{(.*?)\}', re.S)
for t in tok.finditer(body):
    p, full, leaf, gone = t.groups()
    if p:
        cur = p
    elif full:
        explicit.add(norm(full))
    elif leaf:
        explicit.add(norm(cur + leaf))
    elif gone:
        retired.update(cur + g for g in re.findall(r'"(\w+)"', gone))
explicit -= set(table)
source = set(table) | explicit | retired

documented, doc_alias = set(), set()
sec_pfx, in_tab, in_retired, in_alias = "", False, False, False
for line in doc.splitlines():
    if line.startswith("## "):
        h = re.search(r"`([\w<>.]+\.)\*`", line)
        sec_pfx = h.group(1) if h else ""
        in_retired = False
        in_alias = line.startswith("## 21.")
    elif line.startswith("### "):
        in_retired = "Retired `daynight.*`" in line
    if line.startswith("| Key | Type"):
        in_tab = True
        continue
    if not line.startswith("|"):
        in_tab = False
    cell = re.match(r"\|\s*`([\w<>.]+)`\s*\|", line)
    if in_tab and cell:
        k = cell.group(1)
        documented.add(k if "." in k else sec_pfx + k)
    elif in_alias and cell:
        doc_alias.add(cell.group(1))
    if in_retired and not line.startswith("#"):
        documented.update("daynight." + r for r in re.findall(r"`(\w+)`", line)
                          if r + "." not in prefix.values())

example = []
for line in read("timps.conf.example").splitlines():
    m = re.match(r"[#;\s]*([a-z][a-z0-9_]*\.[a-z0-9_.]+)\s*=", line)
    if m:
        example.append(norm(m.group(1)))
exmiss = sorted(set(example) - documented - doc_alias)

missing = sorted(source - documented)
extra = sorted(documented - source)
amiss = sorted(alias - doc_alias)
aextra = sorted(doc_alias - alias)
print("source:     %d canonical keys (%d table, %d set_kv, %d retired), %d aliases"
      % (len(source), len(table), len(explicit), len(retired), len(alias)))
print("documented: %d canonical keys, %d aliases" % (len(documented), len(doc_alias)))
print("timps.conf.example: %d key lines (active and commented out)" % len(example))
for title, lst in (("missing from docs/ai/config-keys.md", missing),
                   ("documented but not in src/config.c", extra),
                   ("aliases missing from §21", amiss),
                   ("aliases in §21 but not in src/config.c", aextra),
                   ("timps.conf.example keys not documented", exmiss)):
    print("%s: %d" % (title, len(lst)))
    for k in lst:
        print("  " + k)
sys.exit(1 if missing or extra or amiss or aextra or exmiss else 0)
PY
