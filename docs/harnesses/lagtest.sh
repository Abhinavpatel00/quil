#!/bin/bash
# Measure how long the app keeps scheduling frames after a single 'p'.
BIN=$1; VFXCFG=$2
pkill -f "quill --no-session" 2>/dev/null; pkill Xvfb 2>/dev/null; sleep 0.3
Xvfb :99 -screen 0 1280x800x24 >/dev/null 2>&1 </dev/null &
sleep 1
export DISPLAY=:99 XDG_RUNTIME_DIR=/tmp/xdg; mkdir -p /tmp/xdg; chmod 700 /tmp/xdg
setsid $BIN --no-session assets/sample.c $VFXCFG >/tmp/q.out 2>&1 </dev/null &
P=$!; sleep 2.5
if ! kill -0 $P 2>/dev/null; then echo "died"; tail -3 /tmp/q.out; pkill Xvfb; exit 1; fi
# sample CPU jiffies over a quiet window (baseline) then after a 'p'
read_utime() { awk '{print $14+$15}' /proc/$P/stat; }
u0=$(read_utime); sleep 1.0; u1=$(read_utime)
baseline=$((u1-u0))
xdotool key --window $(xdotool search --name Quill | head -1) y y 2>/dev/null
sleep 0.3
xdotool key p 2>/dev/null
u2=$(read_utime); sleep 1.0; u3=$(read_utime)
after=$((u3-u2))
echo "$(printf '%-22s idle %3d jiffies/s   after-p %3d jiffies/s' "$(basename $(dirname $BIN))/$(basename $BIN)" $baseline $after)"
kill -9 $P 2>/dev/null; pkill Xvfb 2>/dev/null
