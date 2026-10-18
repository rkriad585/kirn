echo "=== gate 2b: syntax (tests/syntax) ==="
for t in "$ROOT"/tests/syntax/*_ok*.kn; do
    [ -e "$t" ] || continue
    name="$(basename "$t")"
    err="$("$CC" "$t" 2>&1)"; [ $? -eq 0 ] || { report FAIL "$name" "kirncheck rejected: $(echo "$err" | head -1)"; continue; }
    if echo "$err" | grep -q 'warning'; then
        report FAIL "$name" "clean positive file emitted a warning: $(echo "$err" | grep -m1 'warning')"; continue
    fi
    t_out="$(mktemp "$TMP/syn.XXXX")"; v_out="$(mktemp "$TMP/syn.XXXX")"
    timeout "$WATCH" "$CR" --no-vm "$t" >"$t_out" 2>&1; tc=$?
    timeout "$WATCH" "$CR" "$t" >"$v_out" 2>&1; vc=$?
    if [ "$tc" = 0 ] && [ "$vc" = 0 ] && [ "$(cat "$t_out")" = "$(cat "$v_out")" ]; then
        report PASS "$name" "check + tree == VM"
    else
        report FAIL "$name" "tree rc=$tc, VM rc=$vc; tree: $(tr '\n' ' ' <"$t_out" | head -c 80)"
    fi
done
for t in "$ROOT"/tests/syntax/*_err*.kn; do
    [ -e "$t" ] || continue
    name="$(basename "$t")"
    exp=$(sed -n 's/.*#\s*expect:\s*//p' "$t" | head -1 | sed 's/[[:space:]]*$//')
    if [ -z "$exp" ]; then report FAIL "$name" "missing '# expect:' comment line"; continue; fi
    out="$("$CC" "$t" 2>&1)"; code=$?
    if [ "$code" = 0 ]; then report FAIL "$name" "kirncheck exited 0, expected diagnostic '$exp'"; continue; fi
    if echo "$out" | grep -qF "$exp"; then report PASS "$name" "$exp"; else
        report FAIL "$name" "missing '$exp'; got: $(echo "$out" | head -1)"
    fi
done
