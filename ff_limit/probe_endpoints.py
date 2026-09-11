"""Which unauthenticated x.com syndication endpoints still return a thread?

tweet-result gives one tweet plus its parent. To render a whole conversation we
need siblings (replies), so probe the older widget endpoints too.
"""
import json
import re
import urllib.error
import urllib.request

TID = "2094959836890877970"  # conversation_count = 7


def token(tid):
    digs = "0123456789abcdefghijklmnopqrstuvwxyz"
    x = (int(tid) / 1e15) * 3.141592653589793
    i, frac, s = int(x), x - int(x), ""
    if i == 0:
        s = "0"
    while i:
        s = digs[i % 36] + s
        i //= 36
    out = s + "."
    for _ in range(20):
        frac *= 36
        d = int(frac)
        out += digs[d]
        frac -= d
    return re.sub(r"(0+|\.)", "", out)


T = token(TID)
CANDIDATES = [
    ("tweet-result (known good)",
     f"https://cdn.syndication.twimg.com/tweet-result?id={TID}&lang=en&token={T}"),
    ("timeline/conversation (cdn)",
     f"https://cdn.syndication.twimg.com/timeline/conversation?id={TID}&lang=en"),
    ("timeline/conversation +token",
     f"https://cdn.syndication.twimg.com/timeline/conversation?id={TID}&lang=en&token={T}"),
    ("timeline/conversation (syndication.twitter)",
     f"https://syndication.twitter.com/timeline/conversation?id={TID}&lang=en"),
    ("tweets.json batch",
     f"https://cdn.syndication.twimg.com/tweets.json?ids={TID}&lang=en"),
    ("widgets/tweet (legacy)",
     f"https://cdn.syndication.twimg.com/widgets/tweet?id={TID}&lang=en"),
    ("oembed (publish)",
     f"https://publish.twitter.com/oembed?url=https://x.com/__alula/status/{TID}"),
    ("tweet-result w/ conversation feature",
     f"https://cdn.syndication.twimg.com/tweet-result?id={TID}&lang=en&token={T}"
     "&features=tfw_timeline_list%3A%3Btfw_tweet_result_migration_13%3Acontrol"),
]

for name, url in CANDIDATES:
    try:
        req = urllib.request.Request(url, headers={
            "User-Agent": "Mozilla/5.0 (Windows NT 10.0; Win64; x64) Firefox/144.0",
            "Accept": "application/json, text/javascript, */*",
            "Referer": "https://platform.twitter.com/",
        })
        r = urllib.request.urlopen(req, timeout=20)
        raw = r.read()
        note = ""
        try:
            d = json.loads(raw)
            if isinstance(d, dict):
                keys = sorted(d.keys())
                note = f"keys={keys[:8]}"
                # look for anything that smells like a list of tweets
                for k in ("timeline", "tweets", "globalObjects", "entries", "conversation"):
                    if k in d:
                        note += f"  <-- HAS '{k}'"
            elif isinstance(d, list):
                note = f"list of {len(d)}"
        except Exception:
            note = "non-JSON: " + raw[:60].decode("utf-8", "replace")
        print(f"[{r.status}] {name:<44} {len(raw):>7}B  {note}")
    except urllib.error.HTTPError as e:
        print(f"[{e.code}] {name:<44} {'-':>7}   {e.reason}")
    except Exception as e:
        print(f"[ERR] {name:<44} {'-':>7}   {type(e).__name__}: {e}")
