"""Which JS object classes make up an x.com tab, and how does that compare to
other sites in the same report."""
import collections
import gzip
import json
import re
import urllib.parse

MB = 1024 * 1024
R = json.load(gzip.open(r"C:\Users\zigmaster\Downloads\memory-report.json.gz",
                        "rt", encoding="utf-8"))["reports"]

TOP = re.compile(r"^explicit/window-objects/top\((.*?), id=\d+\)/")
CLS = re.compile(r"/classes/class\((.*?)\)/(.*)$")


def host(u):
    u = u.replace("\\", "/")
    if u.startswith("moz-extension://"):
        return "moz-extension"
    return urllib.parse.urlparse(u).netloc or u


by_class = collections.defaultdict(collections.Counter)   # site -> class -> bytes
by_kind = collections.defaultdict(collections.Counter)    # site -> leaf -> bytes
tabs = collections.defaultdict(set)

for r in R:
    if r["units"] != 0:
        continue
    t = TOP.match(r["path"])
    if not t:
        continue
    h = host(t.group(1))
    tabs[h].add(t.group(1))
    c = CLS.search(r["path"])
    if c:
        by_class[h][c.group(1)] += r["amount"]
        by_kind[h][c.group(2)] += r["amount"]

n = len(tabs["x.com"])
tot = sum(by_class["x.com"].values())
print("=" * 72)
print(f"x.com JS objects: {tot/MB:.0f} MB over {n} tabs = {tot/MB/n:.0f} MB/tab")
print("=" * 72)
print(f"{'JS class':<28}{'total':>9}{'per tab':>10}{'share':>8}")
print("-" * 72)
for k, v in by_class["x.com"].most_common(10):
    print(f"{k[:27]:<28}{v/MB:>8.0f}M{v/MB/n:>9.1f}M{100*v/tot:>7.1f}%")

print()
print(f"{'stored where':<28}{'total':>9}{'per tab':>10}")
print("-" * 72)
for k, v in by_kind["x.com"].most_common(8):
    print(f"{k[:27]:<28}{v/MB:>8.0f}M{v/MB/n:>9.1f}M")

print()
print("=" * 72)
print("JS OBJECT BYTES PER TAB, BY SITE")
print("=" * 72)
rows = []
for h, cnt in by_class.items():
    t = len(tabs[h])
    if t:
        rows.append((sum(cnt.values()) / MB / t, h, t, sum(cnt.values()) / MB))
for per, h, t, total in sorted(rows, reverse=True):
    print(f"{h[:34]:<36}{total:>8.0f}M over {t:>3} tabs = {per:>7.1f}M/tab")
