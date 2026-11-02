#!/usr/bin/env bash
# Standalone gate script. Every gate in this directory runs on its own:
#     bash scripts/gates/7_pats.sh
# There is no aggregate driver - loading the shared setup (ROOT, binaries,
# report(), TMP) is done here unconditionally.
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/_common.sh"
GATE_START=${GATE_START:-$(date +%s)}
GATE_FILES=0

echo "=== gate 7: pats (.pat pack -> install -> consume) ==="
T7="$TMP/rt"; mkdir -p "$T7"
(cd "$T7" && "$KIRN" new pat mypat >/dev/null 2>&1)
printf 'pub fn hello(who: Str) -> Str { return "hi from pats, " + who + "!"; }\n' > "$T7/mypat/code/pin.kn"
if (cd "$T7/mypat" && "$KIRN" build) >/dev/null 2>&1; then
    PAT=$(find "$T7/mypat/build" -name '*.pat' | head -1)
    if [ -z "$PAT" ]; then report FAIL "pack .pat" "no .pat produced in build/"
    elif [ ! -f "$T7/mypat/kirn.toml" ]; then report FAIL "pack .pat" "kirn.toml not scaffolded"
    else
        report PASS "pack .pat" "$(basename "$PAT")"
        (cd "$T7" && "$KIRN" new app >/dev/null 2>&1)
        if (cd "$T7/app" && "$KIRN" install "$PAT" >/dev/null 2>&1); then
            if [ -f "$T7/app/pats/mypat/code/pin.kn" ] && [ -f "$T7/app/pats/mypat/kirn.toml" ]; then report PASS "install .pat" "pats/mypat"
            else report FAIL "install .pat" "destination pats/mypat missing"; fi
            printf 'import mypat;\nfn main() -> none { print(mypat.hello("world")); }\n' > "$T7/app/code/main.kn"
            out7=$(cd "$T7/app" && timeout "$WATCH" "$KIRN" run "$T7/app" 2>&1); c7=$?
            if [ "$c7" = 0 ] && echo "$out7" | grep -q "hi from pats, world!"; then report PASS "consume .pat" "$out7"
            else report FAIL "consume .pat" "rc=$c7: $out7"; fi
        else report FAIL "install .pat" "kirn install failed"; fi
    fi
else report FAIL "pack .pat" "kirn build pat failed"; fi

GATE_END=$(date +%s)
GATE_DUR=$((GATE_END - GATE_START))
GATE_M=$((GATE_DUR / 60))
GATE_S=$((GATE_DUR % 60))
GATE_PARFILE=0
if [ "${GATE_FILES:-0}" -gt 0 ]; then
  GATE_PARFILE=$(awk -v d="$GATE_DUR" -v f="$GATE_FILES" "BEGIN{printf \"%.2f\", d*1000.0/f}")
else
  GATE_FALLBACK=$((pass + fail))
  if [ "$GATE_FALLBACK" -gt 0 ]; then
    GATE_PARFILE=$(awk -v d="$GATE_DUR" -v f="$GATE_FALLBACK" "BEGIN{printf \"%.2f\", d*1000.0/f}")
  fi
fi
echo "GATE RESULT: $pass passed, $fail failed, $hung hung, ${GATE_PARFILE}ms par file, ${GATE_M}m:${GATE_S}s took"
[ "$fail" -eq 0 ] || exit 1
exit 0
