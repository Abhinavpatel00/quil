#!/usr/bin/env python3
"""Sample peak memory of a quill run, and attribute it to mappings.

Usage: measure.py <label> -- <command...>
Samples /proc/<pid>/smaps_rollup while the child runs; at exit it also
records the final per-mapping breakdown so shared driver pages can be
separated from the app's own memory.
"""
import os, subprocess, sys, time, collections, re

def rollup(pid):
    d = {}
    try:
        with open(f"/proc/{pid}/smaps_rollup") as f:
            for line in f:
                k, _, v = line.partition(":")
                v = v.strip().split()
                if v and v[0].isdigit():
                    d[k] = int(v[0])
    except (FileNotFoundError, ProcessLookupError):
        return None
    return d

def mappings(pid):
    """Return list of (name, size_kb, rss_kb, pss_kb) from /proc/<pid>/smaps."""
    out = []
    try:
        f = open(f"/proc/{pid}/smaps")
    except (FileNotFoundError, ProcessLookupError):
        return []
    name, size, rss, pss = None, 0, 0, 0
    with f:
        for line in f:
            parts = line.split()
            if len(parts) >= 5 and "-" in parts[0] and re.fullmatch(r"[rwxps-]{4}", parts[1]):
                if name is not None:
                    out.append((name, size, rss, pss))
                a, b = parts[0].split("-")
                size = (int(b, 16) - int(a, 16)) // 1024
                name = parts[5] if len(parts) > 5 else "[anon]"
                rss = pss = 0
            elif line.startswith("Rss:"):
                rss = int(parts[1])
            elif line.startswith("Pss:"):
                pss = int(parts[1])
    if name is not None:
        out.append((name, size, rss, pss))
    return out

def main():
    label = sys.argv[1]
    cmd = sys.argv[sys.argv.index("--") + 1:]
    p = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    peak = collections.defaultdict(int)
    samples = 0
    maps = []
    while p.poll() is None:
        r = rollup(p.pid)
        if r:
            samples += 1
            if r.get("Rss", 0) > peak["Rss"]:
                maps = mappings(p.pid)   # snapshot at the peak, while still alive
            for k in ("Rss", "Pss", "Private_Dirty", "Private_Clean", "Shared_Clean", "Anonymous"):
                if k in r and r[k] > peak[k]:
                    peak[k] = r[k]
        time.sleep(0.002)
    print(f"\n=== {label} ===")
    print(f"  samples={samples}")
    print(f"  peak RSS          {peak['Rss']/1024:8.2f} MiB")
    print(f"  peak PSS          {peak['Pss']/1024:8.2f} MiB")
    print(f"  peak Private_Dirty{peak['Private_Dirty']/1024:8.2f} MiB   <-- app's own, unshareable")
    print(f"  peak Anonymous    {peak['Anonymous']/1024:8.2f} MiB")
    # aggregate by library
    agg = collections.defaultdict(lambda: [0, 0, 0])
    for name, size, rss, pss in maps:
        key = os.path.basename(name) if name.startswith("/") else name
        agg[key][0] += size; agg[key][1] += rss; agg[key][2] += pss
    top = sorted(agg.items(), key=lambda kv: -kv[1][2])[:12]
    print(f"  {'mapping':<28}{'RSS':>10}{'PSS':>10}")
    for name, (size, rss, pss) in top:
        if pss or rss:
            print(f"  {name:<28}{rss/1024:9.2f}M{pss/1024:9.2f}M")

if __name__ == "__main__":
    main()
