#!/usr/bin/env bash
# Standalone gate script. Every gate in this directory runs on its own:
#     bash scripts/gates/1_runall.sh
# There is no aggregate driver - loading the shared setup (ROOT, binaries,
# report(), TMP) is done here unconditionally.
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/_common.sh"
GATE_START=${GATE_START:-$(date +%s)}
GATE_FILES=0

echo "=== gate 1: runall (examples/*.kn) ==="
for f in "$ROOT"/examples/*.kn; do
    name="$(basename "$f")"
    GATE_FILES=$((GATE_FILES+1))
    exp=$(sed -n 's/.*#\s*expect-exit:\s*\([0-9][0-9]*\).*/\1/p' "$f" | head -1)
    so="$(mktemp "$TMP/so.XXXX")"; se="$(mktemp "$TMP/se.XXXX")"
    timeout "$WATCH" "$CR" "$f" >"$so" 2>"$se"; code=$?
    so_txt=$(tr '\n' ' ' <"$so" | sed 's/ *$//')
    se_txt=$(tr '\n' ' ' <"$se" | sed 's/ *$//')
    if [ "$code" = 124 ]; then
        report FAIL "$name" "HANG (watchdog $WATCH s) stderr: $se_txt"
    elif [ -n "$exp" ]; then
        exp8=$((exp % 256))
        if [ "$code" = "$exp8" ]; then report PASS "$name" "exit $exp (% 256 = $exp8)"; else
            report FAIL "$name" "exit $code, expected $exp (% 256 = $exp8); stderr: $se_txt"
        fi
    elif [ "$code" = 0 ]; then
        report PASS "$name" "stdout: $so_txt"
    else
        report FAIL "$name" "exit $code; stderr: $se_txt"
    fi
done

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
