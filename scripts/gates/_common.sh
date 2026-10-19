set -u
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
CC="$ROOT/build/kirncheck"
CR="$ROOT/build/kirnrun"
KIRN="$ROOT/build/kirn"
KN="$ROOT/build/kirnc"
WATCH=20
for E in "$CC" "$CR" "$KIRN" "$KN"; do
    [ -x "$E" ] || { echo "missing executable: $E" >&2; exit 1; }
done
# $CC is the CHECKER here, not a C++ compiler (gate 2/3 use it as kirncheck), so
# anything that needs to compile something needs this instead. Mirrors
# ensureRunner's search order in the driver.
CXX="${KIRN_CXX:-}"
if [ -z "$CXX" ]; then
    if command -v g++ >/dev/null 2>&1; then CXX=g++
    elif command -v clang++ >/dev/null 2>&1; then CXX=clang++
    else CXX=c++
    fi
fi
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

# --- shared result reporting ----------------------------------------------
# Each gate is a standalone script: it sources this file, runs its checks,
# then prints its own "GATE RESULT" and exits non-zero on failure. There is no
# aggregate driver, so no cross-gate pass/fail counters are kept here beyond
# the ones every gate reports itself.
fail=0
pass=0
hung=0
report() { # status name detail
    if [ "$1" = PASS ]; then pass=$((pass+1)); else fail=$((fail+1)); fi
    printf '%4s  %s\n' "$1" "$2"
    [ -z "${3:-}" ] || printf '        %s\n' "$3"
}
