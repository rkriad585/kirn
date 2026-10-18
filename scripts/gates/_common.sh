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

fail=0
pass=0
report() { # status name detail
    if [ "$1" = PASS ]; then pass=$((pass+1)); else fail=$((fail+1)); fi
    printf '%4s  %s\n' "$1" "$2"
    [ -z "${3:-}" ] || printf '        %s\n' "$3"
}

# --- gate selector (POWER_PLAN phase tooling) ---------------------------
# Optional args narrow which gates run, e.g.:  gates_linux.sh 6 2b
# No args runs every gate (identical to the original behaviour). Sourcing a
# file without args keeps the caller's positional parameters, so $#/$* here
# are the driver's arguments.
SELECTORS=""
if [ "$#" -gt 0 ]; then SELECTORS=" $* "; fi
hung=0   # normally re-zeroed inside gate 5; bound here so any subset works
want() {
    [ -z "$SELECTORS" ] && return 0
    case "$SELECTORS" in *" $1 "*) return 0 ;; esac
    return 1
}
