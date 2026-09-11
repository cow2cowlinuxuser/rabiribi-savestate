"""Summarize an about:memory report: who holds the RAM, and is any of it a leak?

Usage: python analyze_report.py <memory-report.json.gz>
"""
import collections
import gzip
import json
import sys

MB = 1024 * 1024


def mb(n):
    return n / MB


path = sys.argv[1] if len(sys.argv) > 1 else r"C:\Users\zigmaster\Downloads\memory-report.json.gz"
d = json.load(gzip.open(path, "rt", encoding="utf-8"))
reports = d["reports"]

# kind: 0=NONHEAP 1=HEAP 2=OTHER ; units: 0=BYTES 1=COUNT 2=COUNT_CUMULATIVE 3=PERCENTAGE
byproc = collections.defaultdict(dict)   # proc -> {path: amount} for singleton paths
explicit = collections.Counter()         # proc -> sum of explicit/ leaves
subtree = collections.Counter()          # "explicit/<a>/<b>" -> bytes, all processes
ghosts = collections.Counter()

for r in reports:
    proc, p, amt, units = r["process"], r["path"], r["amount"], r["units"]
    if units != 0:  # non-byte counters
        if p == "ghost-windows":
            ghosts[proc] += amt
        continue
    if p in ("resident", "resident-unique", "vsize", "heap-allocated",
             "heap-unclassified", "explicit", "private", "js-main-runtime-gc-heap-committed"):
        byproc[proc][p] = amt
        if p != "explicit":
            continue
    if p.startswith("explicit/"):
        explicit[proc] += amt
        parts = p.split("/")
        key = "/".join(parts[1:3]) if len(parts) > 2 else parts[1]
        subtree[key] += amt

print("=" * 78)
print("PER PROCESS")
print("=" * 78)
print(f"{'process':<42}{'resident':>10}{'explicit':>10}{'unclass':>9}{'ghosts':>7}")
print("-" * 78)
tot_res = tot_exp = 0
rows = []
for proc, m in byproc.items():
    res = m.get("resident", 0)
    exp = explicit.get(proc, 0)
    unc = m.get("heap-unclassified", 0)
    rows.append((res, proc, exp, unc))
for res, proc, exp, unc in sorted(rows, reverse=True):
    tot_res += res
    tot_exp += exp
    print(f"{proc[:41]:<42}{mb(res):>9.0f}M{mb(exp):>9.0f}M{mb(unc):>8.0f}M{ghosts.get(proc,0):>7}")
print("-" * 78)
print(f"{'TOTAL':<42}{mb(tot_res):>9.0f}M{mb(tot_exp):>9.0f}M")
print(f"{'':<42}{tot_res/1024**3:>9.2f}G{tot_exp/1024**3:>9.2f}G")

print()
print("=" * 78)
print("WHERE IT GOES  (explicit/<category>, summed across all 17 processes)")
print("=" * 78)
for k, v in subtree.most_common(28):
    if v < 8 * MB:
        break
    bar = "#" * int(mb(v) / 40)
    print(f"{mb(v):>8.0f}M  {k:<46}{bar}")

print()
print("=" * 78)
print("CACHES -- testing the 'prefs told it to hoard' theory")
print("=" * 78)
CACHE_KEYS = [
    ("network/memory-cache", "browser.cache.memory.capacity = 2 GB"),
    ("images/", "image.cache.size = 10 GB(!)"),
    ("media/", "media.memory_caches_combined_limit_kb = 3 GB"),
    ("preload/", ""),
    ("window-objects/", ""),
    ("js-non-window/", ""),
]
agg = collections.Counter()
for r in reports:
    if r["units"] != 0 or not r["path"].startswith("explicit/"):
        continue
    tail = r["path"][len("explicit/"):]
    for key, _ in CACHE_KEYS:
        if tail.startswith(key):
            agg[key] += r["amount"]
for key, note in CACHE_KEYS:
    print(f"{mb(agg[key]):>8.0f}M  explicit/{key:<24}{note}")

print()
print("=" * 78)
print("TOP 20 INDIVIDUAL REPORTS")
print("=" * 78)
big = sorted(
    (r for r in reports if r["units"] == 0 and r["path"].startswith("explicit/")),
    key=lambda r: -r["amount"],
)[:20]
for r in big:
    who = r["process"].split("(")[0].strip()
    print(f"{mb(r['amount']):>8.0f}M  [{who:<16}] {r['path'][:100]}")

print()
print("=" * 78)
print("LEAK INDICATORS")
print("=" * 78)
tg = sum(ghosts.values())
print(f"ghost-windows total: {tg}")
if tg:
    for p, c in ghosts.most_common():
        if c:
            print(f"    {c:>4}  {p}")
    print("  ^ documents closed but never collected. Real leak, usually an extension.")
else:
    print("  none -- no leaked documents. The memory is live, not leaked.")

# top-level window count vs ghost
wins = collections.Counter()
for r in reports:
    if r["units"] == 1 and r["path"] in ("ghost-windows", "top-level-windows"):
        wins[r["path"]] += r["amount"]
print(f"other window counters: {dict(wins)}")
