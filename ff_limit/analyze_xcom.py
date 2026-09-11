"""What inside an x.com tab is eating the RAM?

Breaks explicit/window-objects/top(<site>...) down by the component after the
window(...) segment, so DOM vs layout vs JS vs images is directly comparable,
and contrasts x.com against the other sites in the same report.
"""
import collections
import gzip
import json
import re
import sys
import urllib.parse

MB = 1024 * 1024
path = sys.argv[1] if len(sys.argv) > 1 else r"C:\Users\zigmaster\Downloads\memory-report.json.gz"
reports = json.load(gzip.open(path, "rt", encoding="utf-8"))["reports"]

TOP = re.compile(r"^explicit/window-objects/top\((.*?), id=\d+\)/(.*)$")


def host_of(url):
    url = url.replace("\\", "/")
    if url.startswith("moz-extension://"):
        return "moz-extension"
    return urllib.parse.urlparse(url).netloc or url


def bucket(rest):
    """Collapse the per-window tail into a comparable category."""
    # strip the 'active/window(...)/' or 'cached/window(...)/' prefix
    rest = re.sub(r"^(active|cached)/window\([^)]*\)/?", "", rest)
    rest = re.sub(r"^(active|cached)/?", "", rest)
    if not rest:
        return "(window itself)"
    parts = rest.split("/")
    if parts[0] == "js-zone" or parts[0].startswith("js-"):
        return "js-zone/" + (parts[1] if len(parts) > 1 else "")
    if parts[0] == "layout":
        return "layout/" + (parts[1] if len(parts) > 1 else "")
    if parts[0] == "dom":
        return "dom/" + (parts[1] if len(parts) > 1 else "")
    return "/".join(parts[:2])


per_site = collections.defaultdict(collections.Counter)
site_total = collections.Counter()
site_tabs = collections.defaultdict(set)

for r in reports:
    if r["units"] != 0:
        continue
    m = TOP.match(r["path"])
    if not m:
        continue
    h = host_of(m.group(1))
    b = bucket(m.group(2))
    per_site[h][b] += r["amount"]
    site_total[h] += r["amount"]
    site_tabs[h].add(m.group(1))

FOCUS = "x.com"
n = len(site_tabs[FOCUS])
print("=" * 76)
print(f"INSIDE {FOCUS}: {site_total[FOCUS]/MB:.0f} MB across {n} tabs "
      f"({site_total[FOCUS]/MB/n:.0f} MB/tab)")
print("=" * 76)
print(f"{'component':<38}{'total':>9}{'per tab':>10}{'share':>8}")
print("-" * 76)
for b, v in per_site[FOCUS].most_common(18):
    if v < MB:
        break
    print(f"{b[:37]:<38}{v/MB:>8.0f}M{v/MB/n:>9.1f}M{100*v/site_total[FOCUS]:>7.1f}%")

print()
print("=" * 76)
print("PER-TAB COST BY SITE, SAME COMPONENTS  (MB per tab)")
print("=" * 76)
sites = [s for s, _ in site_total.most_common() if len(site_tabs[s]) >= 1][:6]
cats = [b for b, _ in per_site[FOCUS].most_common(9)]
hdr = f"{'component':<30}" + "".join(f"{s[:11]:>12}" for s in sites)
print(hdr)
print("-" * len(hdr))
for c in cats:
    row = f"{c[:29]:<30}"
    for s in sites:
        t = max(len(site_tabs[s]), 1)
        row += f"{per_site[s][c]/MB/t:>12.1f}"
    print(row)
row = f"{'TOTAL / tab':<30}"
for s in sites:
    t = max(len(site_tabs[s]), 1)
    row += f"{site_total[s]/MB/t:>12.1f}"
print("-" * len(hdr))
print(row)

print()
print("=" * 76)
print("HEAVIEST SINGLE x.com TABS")
print("=" * 76)
tab_tot = collections.Counter()
for r in reports:
    if r["units"] != 0:
        continue
    m = TOP.match(r["path"])
    if m and host_of(m.group(1)) == FOCUS:
        tab_tot[m.group(1).replace("\\", "/")] += r["amount"]
for u, v in tab_tot.most_common(8):
    print(f"{v/MB:>7.0f}M  {u[:88]}")
