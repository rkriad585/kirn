#!/usr/bin/env bash
# Standalone gate script. Every gate in this directory runs on its own:
#     bash scripts/gates/4_vm_diff.sh
# There is no aggregate driver - loading the shared setup (ROOT, binaries,
# report(), TMP) is done here unconditionally.
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/_common.sh"
GATE_START=${GATE_START:-$(date +%s)}
GATE_FILES=0

echo "=== gate 4: vm_diff (tree vs VM) ==="
hung=0
for f in "$ROOT"/examples/*.kn "$ROOT"/tests/vm_parity/*.kn; do
    name="$(basename "$f")"
    GATE_FILES=$((GATE_FILES+1))
    a_out="$(mktemp "$TMP/a.XXXX")"; b_out="$(mktemp "$TMP/b.XXXX")"
    timeout "$WATCH" "$CR" --no-vm "$f" >"$a_out" 2>&1; a_code=$?
    timeout "$WATCH" "$CR" "$f" >"$b_out" 2>&1; b_code=$?
    if [ "$b_code" = 124 ]; then report FAIL "$name" "VM HANG"; hung=$((hung+1)); continue; fi
    sa="$a_code::$(cat "$a_out")"; sb="$b_code::$(cat "$b_out")"
    if [ "$sa" = "$sb" ]; then report PASS "$name" "tree=$a_code vm=$b_code"; else
        report FAIL "$name" "tree=$a_code vm=$b_code; tree: $(echo "$sa" | head -1), vm: $(echo "$sb" | head -1)"
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
