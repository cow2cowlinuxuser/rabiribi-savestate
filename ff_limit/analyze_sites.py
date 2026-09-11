"""Aggregate an about:memory report by site, and audit what the cache prefs
actually achieved."""
import collections
import gzip
import json
import re
import sys
import urllib.parse

MB = 1024 * 1024
path = sys.argv[1] if len(sys.argv) > 1 else r"C:\Users\zigmaster\Downloads\memory-report.json.gz"
reports = json.load(gzip.open(path, "rt", encoding="utf-8"))["reports"]

TOP = re.compile(r"^explicit/window-objects/top\((.*?), id=\d+\)")

site = collections.Counter()
tabs = collections.defaultdict(set)
for r in reports:
    if r["units"] != 0:
        continue
    m = TOP.match(r["path"])
    if not m:
        continue
    url = m.group(1).replace("\\", "/")
    if url.startswith("moz-extension://"):
        host = "moz-extension (addon pages)"
    elif url in ("none", "about:blank"):
        host = "(no document)"
    else:
        host = urllib.parse.urlparse(url).netloc or url
    site[host] += r["amount"]
    tabs[host].add(m.group(0))

print("=" * 74)
print("MEMORY BY SITE  (explicit/window-objects, all processes)")
print("=" * 74)
print(f"{'site':<34}{'total':>9}{'tabs':>6}{'per tab':>10}")
print("-" * 74)
grand = 0
for host, amt in site.most_common(22):
    n = len(tabs[host])
    grand += amt
    print(f"{host[:33]:<34}{amt/MB:>8.0f}M{n:>6}{amt/MB/max(n,1):>9.0f}M")
print("-" * 74)
total_all = sum(site.values())
total_tabs = sum(len(v) for v in tabs.values())
print(f"{'ALL WINDOW OBJECTS':<34}{total_all/MB:>8.0f}M{total_tabs:>6}{total_all/MB/max(total_tabs,1):>9.0f}M")

print()
print("=" * 74)
print("CACHE AUDIT: pref ceiling vs bytes actually held")
print("=" * 74)
cache_paths = collections.Counter()
for r in reports:
    if r["units"] != 0:
        continue
    p = r["path"]
    if "cache" in p.lower() and r["amount"] > 0:
        # collapse to the first three components
        cache_paths["/".join(p.split("/")[:3])] += r["amount"]
for p, v in cache_paths.most_common(18):
    print(f"{v/MB:>8.1f}M  {p}")

print()
CEIL = [
    ("browser.cache.memory.capacity", 2097152 * 1024, "in-memory network cache"),
    ("media.memory_caches_combined_limit_kb", 3145728 * 1024, "media cache"),
    ("media.memory_cache_max_size", 1048576 * 1024, "per-media cache"),
    ("image.cache.size", 10485760, "image cache (bytes, = 10 MB not 10 GB)"),
    ("browser.cache.disk.capacity", 8192000 * 1024, "DISK cache, not RAM"),
]
print("configured ceilings:")
for name, val, note in CEIL:
    print(f"  {name:<40} {val/MB:>10.0f}M   {note}")

print()
print("=" * 74)
print("SCRIPT DATA (duplicated per content process)")
print("=" * 74)
sd = collections.Counter()
for r in reports:
    if r["units"] == 0 and "script-data" in r["path"]:
        sd[r["process"]] += r["amount"]
for p, v in sd.most_common():
    print(f"{v/MB:>8.0f}M  {p}")
print(f"{sum(sd.values())/MB:>8.0f}M  TOTAL compiled-JS across processes")
