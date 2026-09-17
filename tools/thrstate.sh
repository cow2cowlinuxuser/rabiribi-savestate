#!/bin/bash
# Show liveness / hang state of a process and every one of its threads.
# Usage: thrstate.sh <pid> [sample-seconds]
p="$1"
secs="${2:-0}"

[ -d "/proc/$p" ] || { echo "pid $p is gone"; exit 1; }

echo "=== process ==="
ps -o pid,stat,etime,pcpu,rss,nlwp,wchan:28 -p "$p"

snap() {
  printf "%-8s %-4s %-28s %-20s %10s\n" TID ST WCHAN NAME JIFFIES
  for t in /proc/$p/task/*/; do
    tid=$(basename "$t")
    line=$(cat "$t/stat" 2>/dev/null) || continue
    rest=${line#*) }
    st=${rest%% *}
    set -- $rest
    jif=$(( ${12} + ${13} ))          # utime + stime
    w=$(cat "$t/wchan" 2>/dev/null)
    n=$(cat "$t/comm" 2>/dev/null)
    printf "%-8s %-4s %-28s %-20s %10s\n" "$tid" "$st" "${w:--}" "$n" "$jif"
  done
}

echo
echo "=== threads ==="
snap | tee /tmp/thr.a

echo
echo "=== state histogram ==="
for t in /proc/$p/task/*/; do
  line=$(cat "$t/stat" 2>/dev/null) || continue
  rest=${line#*) }
  echo "${rest%% *}"
done | sort | uniq -c | sed 's/$//'

cat <<'LEGEND'

  R  running / runnable      - on CPU or waiting for CPU
  S  interruptible sleep     - normal blocking wait (poll, futex, read)
  D  uninterruptible sleep   - stuck in kernel I/O; THIS is real "not responding"
  t  traced / stopped        - SIGSTOP or under a debugger
  Z  zombie                  - dead, waiting to be reaped
LEGEND

if [ "$secs" -gt 0 ] 2>/dev/null; then
  echo
  echo "=== sampling ${secs}s to see which threads actually burn CPU ==="
  sleep "$secs"
  snap > /tmp/thr.b
  join -j 1 \
    <(tail -n +2 /tmp/thr.a | awk '{print $1, $5, $4}' | sort -k1,1) \
    <(tail -n +2 /tmp/thr.b | awk '{print $1, $5}' | sort -k1,1) \
    | awk '{d=$4-$2; printf "%-8s %-20s delta=%-6s %s\n", $1, $3, d, (d>0 ? "ALIVE (running code)" : "idle / blocked")}'
fi
