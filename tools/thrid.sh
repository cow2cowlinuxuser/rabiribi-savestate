#!/bin/bash
# Identify what a process's threads are and what they're blocked on.
pid="$1"
echo "nproc=$(nproc)  nlwp=$(ls /proc/$pid/task | wc -l)"
echo
echo "=== thread inventory: name, state, wchan, lifetime CPU jiffies, start-jiffy ==="
printf "%-8s %-4s %-22s %-20s %8s %12s\n" TID ST WCHAN NAME CPU STARTED
for t in /proc/$pid/task/*/; do
  tid=$(basename "$t")
  line=$(cat "$t/stat" 2>/dev/null) || continue
  rest=${line#*) }
  set -- $rest
  st=$1
  cpu=$(( ${12} + ${13} ))
  start=${20}
  printf "%-8s %-4s %-22s %-20s %8s %12s\n" \
    "$tid" "$st" "$(cat $t/wchan 2>/dev/null)" "$(cat $t/comm 2>/dev/null)" "$cpu" "$start"
done | sort -k6,6n

echo
echo "=== any audio-related threads or libs mapped? ==="
grep -oiE '[^/]*(pulse|alsa|audio|dsound|mmdev|xaudio|snd)[^/ ]*' /proc/$pid/maps 2>/dev/null | sort -u | head -20 || echo "(none)"

echo
echo "=== sockets open to a sound server? ==="
ls -l /proc/$pid/fd 2>/dev/null | grep -icE 'socket' | sed 's/^/total sockets: /'
