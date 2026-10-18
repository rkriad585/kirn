echo "=== gate 1: runall (examples/*.kn) ==="
for f in "$ROOT"/examples/*.kn; do
    name="$(basename "$f")"
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
