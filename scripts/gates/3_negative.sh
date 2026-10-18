echo "=== gate 3: negative (tests/negative) ==="
for t in "$ROOT"/tests/negative/n*.kn; do
    name="$(basename "$t")"
    exp=$(sed -n 's/.*#\s*expect:\s*//p' "$t" | head -1 | sed 's/[[:space:]]*$//')
    if [ -z "$exp" ]; then report FAIL "$name" "missing '# expect:' comment line"; continue; fi
    out="$("$CC" "$t" 2>&1)"; code=$?
    if [ "$code" = 0 ]; then report FAIL "$name" "kirncheck exited 0, expected diagnostic '$exp'"; continue; fi
    if echo "$out" | grep -qF "$exp"; then report PASS "$name" "$exp"; else
        report FAIL "$name" "missing '$exp'; got: $(echo "$out" | head -1)"
    fi
done
