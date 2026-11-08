#!/bin/bash
BIN=$1; CFG=$2; LABEL=$3
pkill -f "quill --no-session" 2>/dev/null; pkill Xvfb 2>/dev/null; sleep 0.3
Xvfb :99 -screen 0 1280x800x24 >/dev/null 2>&1 </dev/null &
sleep 1
export DISPLAY=:99 XDG_RUNTIME_DIR=/tmp/xdg; mkdir -p /tmp/xdg; chmod 700 /tmp/xdg
setsid $BIN --no-session assets/sample.c --config "$CFG" >/dev/null 2>&1 </dev/null &
P=$!; sleep 2.5
u0=$(awk '{print $14+$15}' /proc/$P/stat); sleep 2.0; u1=$(awk '{print $14+$15}' /proc/$P/stat)
echo "$(printf '%-28s %3d jiffies/s idle' "$LABEL" $(( (u1-u0)/2 )))"
kill -9 $P 2>/dev/null; pkill Xvfb 2>/dev/null
