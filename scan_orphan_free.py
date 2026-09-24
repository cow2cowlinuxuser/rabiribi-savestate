#!/usr/bin/env python3
"""
scan_orphan_free.py - find the gameheap "free of <addr> refused ... a block of
ours ... the game allocated after the save" lines across the shim logs, grouped
by the session date each one falls in, so we can tell whether this corruption
signature is NEW (only recent sessions) or LONG-STANDING (shows up in old logs).

The address in the line varies every time, so we match the *shape* of the line,
not a specific address. By default we scan the Rabi-Ribi install folder's *.log
and *.txt (which holds both the current logs and the .prev*/.churn history).

Examples:
    # everything, to see the earliest date the pattern ever appears
    python scan_orphan_free.py

    # focus on the two days in question
    python scan_orphan_free.py --from 2026-09-23 --to 2026-09-24

    # a couple of lines of context around each hit
    python scan_orphan_free.py --context 2

    # a different phrase (regex); (...) group 1 is reported as the address
    python scan_orphan_free.py --pattern "free of ([0-9A-Fa-f]{4,}) refused"
"""

import argparse
import glob
import os
import re
import sys
from datetime import date, datetime

DEFAULT_DIR = r"C:\Program Files (x86)\Steam\steamapps\common\Rabi-Ribi"

# Session banner, e.g. "===== session 2026-09-23 19:05:06, pid 33140 ====="
SESSION_RE = re.compile(r"=====\s*session\s+(\d{4})-(\d{2})-(\d{2})\s+([0-9:]+)")

# The orphan-free line. The address is captured as group 1; the rest of the
# phrase is what makes it distinctive without pinning the exact wording.
DEFAULT_PATTERN = (
    r"free of ([0-9A-Fa-f]{4,}) refused.*"
    r"(?:header is gone|allocated after the save)"
)


def parse_ymd(s):
    return datetime.strptime(s, "%Y-%m-%d").date()


def scan_file(path, pat, dfrom, dto, context):
    """Return a list of hit dicts for one file, tracking the session date each
    hit falls under by remembering the most recent session banner."""
    hits = []
    cur_date = None
    cur_ts = None
    try:
        with open(path, "r", encoding="utf-8", errors="replace") as fh:
            lines = fh.readlines()
    except OSError as e:
        print(f"  (could not read {path}: {e})", file=sys.stderr)
        return hits

    for i, line in enumerate(lines):
        sh = SESSION_RE.search(line)
        if sh:
            cur_date = date(int(sh.group(1)), int(sh.group(2)), int(sh.group(3)))
            cur_ts = f"{sh.group(1)}-{sh.group(2)}-{sh.group(3)} {sh.group(4)}"
            continue
        m = pat.search(line)
        if not m:
            continue
        # Date filter only when we actually know the session date; a log with no
        # banners keeps all hits (marked "unknown") so nothing is silently lost.
        if cur_date is not None:
            if dfrom and cur_date < dfrom:
                continue
            if dto and cur_date > dto:
                continue
        ctx = []
        if context:
            lo = max(0, i - context)
            hi = min(len(lines), i + context + 1)
            ctx = [lines[j].rstrip("\n") for j in range(lo, hi) if j != i]
        hits.append(
            {
                "file": path,
                "line": i + 1,
                "session_date": cur_date.isoformat() if cur_date else "unknown",
                "session_ts": cur_ts or "unknown",
                "addr": (m.group(1) if m.groups() else "").upper(),
                "text": line.rstrip("\n"),
                "context": ctx,
            }
        )
    return hits


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--dir", default=DEFAULT_DIR, help="folder to scan")
    ap.add_argument("--glob", default="*.log;*.txt",
                    help="semicolon-separated filename patterns")
    ap.add_argument("--from", dest="dfrom", default=None, help="YYYY-MM-DD (session date)")
    ap.add_argument("--to", dest="dto", default=None, help="YYYY-MM-DD (session date)")
    ap.add_argument("--pattern", default=DEFAULT_PATTERN, help="regex; group 1 = address")
    ap.add_argument("--context", type=int, default=0, help="lines of context per hit")
    ap.add_argument("--samples", type=int, default=4, help="sample lines printed per file")
    args = ap.parse_args()

    dfrom = parse_ymd(args.dfrom) if args.dfrom else None
    dto = parse_ymd(args.dto) if args.dto else None
    pat = re.compile(args.pattern)

    files = []
    for g in args.glob.split(";"):
        files.extend(glob.glob(os.path.join(args.dir, g.strip())))
    files = sorted(set(files), key=lambda p: os.path.getmtime(p))
    if not files:
        print(f"No files matched {args.glob} under {args.dir}")
        return

    per_file, per_date, all_hits = {}, {}, []
    for path in files:
        hits = scan_file(path, pat, dfrom, dto, args.context)
        if hits:
            per_file[path] = hits
            all_hits.extend(hits)
            for h in hits:
                per_date[h["session_date"]] = per_date.get(h["session_date"], 0) + 1

    rng = ""
    if dfrom or dto:
        rng = f"  (session dates {dfrom or 'start'} .. {dto or 'end'})"
    print(f"Scanned {len(files)} file(s) under {args.dir}")
    print(f"Pattern: {args.pattern}{rng}")
    print(f"Total matches: {len(all_hits)}\n")

    if not all_hits:
        print("No occurrences in range. If you expected some, drop --from/--to "
              "(a log with no session banners can't be date-filtered).")
        return

    print("== by session date (earliest first) ==")
    for d in sorted(per_date):
        print(f"  {d:>12} : {per_date[d]}")
    print()

    print("== by file (oldest mtime first) ==")
    for path in files:
        if path not in per_file:
            continue
        hs = per_file[path]
        mt = date.fromtimestamp(os.path.getmtime(path)).isoformat()
        dates = ", ".join(sorted({h["session_date"] for h in hs}))
        print(f"  {os.path.basename(path):<34} mtime {mt}  {len(hs):>4} hit(s)  "
              f"[{dates}]")
    print()

    print(f"== sample lines (up to {args.samples} per file) ==")
    for path in files:
        if path not in per_file:
            continue
        print(f"-- {os.path.basename(path)} --")
        for h in per_file[path][: args.samples]:
            print(f"   L{h['line']} [{h['session_ts']}] addr={h['addr']}")
            print(f"       {h['text'][:200]}")
            for c in h["context"]:
                print(f"     | {c[:200]}")
    print()

    dated = [h for h in all_hits if h["session_date"] != "unknown"]
    uniq_addr = sorted({h["addr"] for h in all_hits if h["addr"]})
    if dated:
        e = min(dated, key=lambda h: h["session_date"])
        l = max(dated, key=lambda h: h["session_date"])
        print(f"EARLIEST dated occurrence: {e['session_date']} "
              f"({os.path.basename(e['file'])} L{e['line']})")
        print(f"LATEST   dated occurrence: {l['session_date']} "
              f"({os.path.basename(l['file'])} L{l['line']})")
    print(f"Distinct addresses seen: {len(uniq_addr)}"
          + (f"  e.g. {', '.join(uniq_addr[:8])}" if uniq_addr else ""))
    n_unknown = sum(1 for h in all_hits if h["session_date"] == "unknown")
    if n_unknown:
        print(f"({n_unknown} hit(s) had no session banner above them - date shown "
              f"as 'unknown'; judge those by the file's mtime.)")


if __name__ == "__main__":
    main()
