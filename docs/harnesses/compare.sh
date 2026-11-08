#!/bin/bash
# Interactive RSS/PSS/heap for a given binary, 3 runs.
BIN=$1; LABEL=$2
pkill -f "quill --no-session" 2>/dev/null; pkill Xvfb 2>/dev/null; sleep 0.3
Xvfb :99 -screen 0 1280x800x24 >/dev/null 2>&1 </dev/null &
sleep 1
export DISPLAY=:99 XDG_RUNTIME_DIR=/tmp/xdg
mkdir -p /tmp/xdg; chmod 700 /tmp/xdg
Rss=(); Pss=(); PvD=(); Heap=()
for i in 1 2 3; do
  setsid $BIN --no-session assets/sample.c >/dev/null 2>&1 </dev/null &
  P=$!
  sleep 2.5
  if kill -0 $P 2>/dev/null; then
    read r p d <<< $(awk '/^Rss:|^Pss:|^Private_Dirty:/{printf "%d ",$2}' /proc/$P/smaps_rollup | awk '{print $1,$2,$3}')
    h=$(awk '/^[0-9a-f]+-[0-9a-f]+ .*\[heap\]$/{f=1} f&&/^Rss:/{printf "%d",$2; exit}' /proc/$P/smaps)
    Rss+=($r); Pss+=($p); PvD+=($d); Heap+=(${h:-0})
    kill -9 $P 2>/dev/null
  fi
  sleep 0.4
done
python3 - "$LABEL" "${Rss[*]}" "${Pss[*]}" "${PvD[*]}" "${Heap[*]}" <<'PY'
import sys
lab=sys.argv[1]
def s(v): 
    a=[int(x) for x in v.split()]; return sum(a)/len(a)/1024
print(f"{lab:<24}{s(sys.argv[2]):8.2f}{s(sys.argv[3]):8.2f}{s(sys.argv[4]):9.2f}{s(sys.argv[5]):8.2f}")
PY
pkill -f "quill --no-session" 2>/dev/null; pkill Xvfb 2>/dev/null
