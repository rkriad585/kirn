#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BDIR="$ROOT/build-tsan"
RUNNER="${1:-$BDIR/kirnrun}"

if [ ! -f "$BDIR/CMakeCache.txt" ]; then
  cmake -S "$ROOT" -B "$BDIR" -DCMAKE_BUILD_TYPE=Debug -DKIRN_TSAN=ON
fi
cmake --build "$BDIR" -j"$(nproc)"

[ -x "$RUNNER" ] || { echo "runner not found: $RUNNER" >&2; exit 1; }

fail=0
count=0
watch="${TSAN_WATCH:-60}"
for f in "$ROOT"/examples/*.kn; do
  [ -e "$f" ] || continue
  count=$((count+1))
  name="$(basename "$f")"
  if out="$(timeout "$watch" env TSAN_OPTIONS="second_deadlock_stack=1:halt_on_error=1" "$RUNNER" "$f" 2>&1)"; then
    ec=0
  else
    ec=$?
  fi
  if echo "$out" | grep -q "ThreadSanitizer\|data race\|lock-order-inversion"; then
    echo "TSAN: $name RACE"
    echo "$out" | tail -40
    fail=1
    continue
  fi
  if [ "$ec" -eq 124 ]; then
    echo "TSAN: $name TIMEOUT after ${watch}s"
    fail=1
    continue
  fi
  if [ $ec -ne 0 ]; then
    # only count as failure if tsan reported; some tests expect nonzero
    echo "TSAN: $name ec=$ec (no tsan report)"
  fi
done
echo ""
if [ $fail -ne 0 ]; then
  echo "TSan failures detected"
  exit 1
else
  echo "TSan corpus: ALL PASS ($count examples)"
fi
