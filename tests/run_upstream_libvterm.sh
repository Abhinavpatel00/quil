#!/bin/sh
# Runs libvterm's own upstream test suite against the libvterm we vendor.
#
# The tests are pinned to upstream commit d8a68a8 (2023-08-03), the commit our
# vendored src/ is byte-identical to.  Vendoring dropped t/ entirely, so these
# were pulled from upstream; running a newer suite against older sources would
# test features we do not ship.
set -e
cd "$(dirname "$0")/.."
VTERM=tests/upstream/libvterm
HARNESS=build/t/harness

mkdir -p build/t
cc -std=c99 -O1 -Ithird_party/libvterm/include -Ithird_party/libvterm/src \
   "$VTERM/harness.c" build/vterm_*.o -o "$HARNESS"

pass=0; fail=0; failed=""
for t in "$VTERM"/[0-9]*.test; do
  if perl "$VTERM/run-test.pl" --executable "$HARNESS" "$t" >/dev/null 2>&1; then
    pass=$((pass+1))
  else
    fail=$((fail+1)); failed="$failed $(basename "$t")"
  fi
done
echo "libvterm upstream: $pass passed, $fail failed${failed:+ --$failed}"
[ "$fail" -eq 0 ]
