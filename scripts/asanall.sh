#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BDIR="$ROOT/build-asan"
RUNNER="${1:-$BDIR/kirnrun}"

if [ ! -f "$BDIR/CMakeCache.txt" ]; then
  cmake -S "$ROOT" -B "$BDIR" -DCMAKE_BUILD_TYPE=Debug -DKIRN_ASAN=ON
fi
cmake --build "$BDIR" -j"$(nproc)"

[ -x "$RUNNER" ] || { echo "runner not found: $RUNNER" >&2; exit 1; }

fail=0
count=0
for f in "$ROOT"/examples/*.kn; do
  [ -e "$f" ] || continue
  count=$((count+1))
  name="$(basename "$f")"
  out="$(ASAN_OPTIONS="detect_leaks=1:halt_on_error=1" "$RUNNER" "$f" 2>&1 || true)"
  ec=$?
  if echo "$out" | grep -q "AddressSanitizer\|LeakSanitizer\|runtime error"; then
    echo "ASAN: $name ERROR"
    echo "$out" | tail -40
    fail=1
    continue
  fi
  if [ $ec -ne 0 ]; then
    :
  fi
done
echo ""
if [ $fail -ne 0 ]; then
  echo "ASan failures detected"
  exit 1
else
  echo "ASan corpus: ALL PASS ($count examples)"
fi
