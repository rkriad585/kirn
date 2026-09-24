#!/usr/bin/env bash
# Linux gate harness - bash port of the five PowerShell verification gates:
#   1. runall      - every examples/*.co through kirnrun (20s watchdog),
#                    honoring '# expect-exit: N' (POSIX exit codes are 8-bit,
#                    so N is compared as N % 256).
#   2. types       - tests/types/p*.co must check+run clean;
#                    tests/types/n*.co must fail with the '# expect:' substring.
#   3. negative    - tests/negative/n*.co must fail kirncheck with the
#                    '# expect:' substring.
#   4. vm_diff     - tree-walker (--no-vm) vs bytecode VM parity on
#                    examples matching '^[0-9]' (code + combined output).
#   5. conventions - rules/convention matrix for the `kirn` driver (run-entry
#                    resolution + pin.co package initializer).
# Exit 0 iff every gate passes, else 1.
set -u
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CC="$ROOT/build/kirncheck"
CR="$ROOT/build/kirnrun"
CO="$ROOT/build/kirn"
WATCH=20
for E in "$CC" "$CR" "$CO"; do
    [ -x "$E" ] || { echo "missing executable: $E" >&2; exit 1; }
done
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

fail=0
pass=0
report() { # status name detail
    if [ "$1" = PASS ]; then pass=$((pass+1)); else fail=$((fail+1)); fi
    printf '%4s  %s\n' "$1" "$2"
    [ -z "${3:-}" ] || printf '        %s\n' "$3"
}

echo "=== gate 1: runall (examples/*.co) ==="
for f in "$ROOT"/examples/*.co; do
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

echo "=== gate 2: types (tests/types) ==="
for t in "$ROOT"/tests/types/p*.co; do
    name="$(basename "$t")"; err="$("$CC" "$t" 2>&1)"; [ $? -eq 0 ] || { report FAIL "$name" "kirncheck rejected: $(echo "$err" | head -1)"; continue; }
    err="$("$CR" "$t" 2>&1)"; [ $? -eq 0 ] || { report FAIL "$name" "kirnrun exit nonzero: $(echo "$err" | head -1)"; continue; }
    report PASS "$name" "check + run clean"
done
for t in "$ROOT"/tests/types/n*.co "$ROOT"/tests/negative/n*.co; do
    name="$(basename "$t")"
    exp=$(sed -n 's/.*#\s*expect:\s*//p' "$t" | head -1 | sed 's/[[:space:]]*$//')
    if [ -z "$exp" ]; then report FAIL "$name" "missing '# expect:' comment line"; continue; fi
    out="$("$CC" "$t" 2>&1)"; code=$?
    if [ "$code" = 0 ]; then report FAIL "$name" "kirncheck exited 0, expected diagnostic '$exp'"; continue; fi
    if echo "$out" | grep -qF "$exp"; then report PASS "$name" "$exp"; else
        report FAIL "$name" "missing '$exp'; got: $(echo "$out" | head -1)"
    fi
done

echo "=== gate 3: negative (tests/negative) ==="
for t in "$ROOT"/tests/negative/n*.co; do
    name="$(basename "$t")"
    exp=$(sed -n 's/.*#\s*expect:\s*//p' "$t" | head -1 | sed 's/[[:space:]]*$//')
    if [ -z "$exp" ]; then report FAIL "$name" "missing '# expect:' comment line"; continue; fi
    out="$("$CC" "$t" 2>&1)"; code=$?
    if [ "$code" = 0 ]; then report FAIL "$name" "kirncheck exited 0, expected diagnostic '$exp'"; continue; fi
    if echo "$out" | grep -qF "$exp"; then report PASS "$name" "$exp"; else
        report FAIL "$name" "missing '$exp'; got: $(echo "$out" | head -1)"
    fi
done

echo "=== gate 4: vm_diff (examples ^[0-9], tree vs VM) ==="
hung=0
for f in "$ROOT"/examples/[0-9]*.co; do
    name="$(basename "$f")"
    a_out="$(mktemp "$TMP/a.XXXX")"; b_out="$(mktemp "$TMP/b.XXXX")"
    timeout "$WATCH" "$CR" --no-vm "$f" >"$a_out" 2>&1; a_code=$?
    timeout "$WATCH" "$CR" "$f" >"$b_out" 2>&1; b_code=$?
    if [ "$b_code" = 124 ]; then report FAIL "$name" "VM HANG"; hung=$((hung+1)); continue; fi
    sa="$a_code::$(cat "$a_out")"; sb="$b_code::$(cat "$b_out")"
    if [ "$sa" = "$sb" ]; then report PASS "$name" "tree=$a_code vm=$b_code"; else
        report FAIL "$name" "tree=$a_code vm=$b_code; tree: $(echo "$sa" | head -1), vm: $(echo "$sb" | head -1)"
    fi
done

echo "=== gate 5: conventions (kirn driver) ==="
kirn_run() { # dir -> prints "$code|$out"
    local d="$1"; shift
    local so se c
    so="$(mktemp "$TMP/c.XXXX")"; se="$(mktemp "$TMP/e.XXXX")"
    (cd "$d" && timeout "$WATCH" "$CO" "$@") >"$so" 2>"$se"; c=$?
    printf '%s|%s%s' "$c" "$(cat "$so")" "$(cat "$se")"
}
P="$TMP/conv"; mkdir -p "$P"
# 1. code/main.co entry
mkdir -p "$P/a/code"; printf 'def main() { print("A"); }' > "$P/a/code/main.co"
r=$(kirn_run "$P/a" run "$P/a"); c=${r%%|*}; out=${r#*|}
[ "$c" = 0 ] && echo "$out" | grep -q "A" && report PASS "code/main.co entry" || report FAIL "code/main.co entry" "$out"
# 2. manifest main wins over code/main.co
mkdir -p "$P/b/code"; printf '[package]\nname="b"\nmain="start.co"\n' > "$P/b/coco.toml"
printf 'def main() { print("Bmain"); }' > "$P/b/start.co"
printf 'def main() { print("Bcode"); }' > "$P/b/code/main.co"
r=$(kirn_run "$P/b" run "$P/b"); c=${r%%|*}; out=${r#*|}
[ "$c" = 0 ] && echo "$out" | grep -q "Bmain" && report PASS "manifest main preferred" || report FAIL "manifest main preferred" "$out"
# 3. pin.co as run entry
mkdir -p "$P/c"; printf 'pub def main() { print("C"); }' > "$P/c/pin.co"
r=$(kirn_run "$P/c" run "$P/c"); c=${r%%|*}; out=${r#*|}
[ "$c" = 0 ] && echo "$out" | grep -q "C" && report PASS "pin.co as run entry" || report FAIL "pin.co as run entry" "$out"
# 4. no entry -> fix-it error
mkdir -p "$P/d/code"; printf 'pub def u() -> int { return 1; }' > "$P/d/code/util.co"
r=$(kirn_run "$P/d" run "$P/d"); c=${r%%|*}; out=${r#*|}
[ "$c" != 0 ] && echo "$out" | grep -q "no entry point" && report PASS "no entry -> fix-it error" || report FAIL "no entry -> fix-it error" "$out"
# 5. pin.co runs once + pub surface
mkdir -p "$P/proj/coco_libs/greet/code"
printf 'var loads = 0;\nloads = loads + 1;\npub def load_count() -> int { return loads; }\npub def hi(who: string) -> string { return "hi " + who; }\n' > "$P/proj/coco_libs/greet/code/pin.co"
printf 'import greet;\nimport greet as g2;\nimport greet as g3;\ndef main() {\n    print("count=", greet.load_count());\n    print(greet.hi("world"));\n}\n' > "$P/proj/main.co"
r=$(kirn_run "$P/proj" run "$P/proj"); c=${r%%|*}; out=${r#*|}
[ "$c" = 0 ] && echo "$out" | grep -q "count= 1" && report PASS "pin.co runs once (count=1)" || report FAIL "pin.co runs once (count=1)" "$out"
[ "$c" = 0 ] && echo "$out" | grep -q "hi world" && report PASS "pin.co pub surface" || report FAIL "pin.co pub surface" "$out"
# 6. pin.co discovered without manifest
mkdir -p "$P/proj2/coco_libs/nomanifest/code"
printf 'pub def poke() -> string { return "poked"; }\n' > "$P/proj2/coco_libs/nomanifest/code/pin.co"
printf 'import nomanifest;\ndef main() { print(nomanifest.poke()); }\n' > "$P/proj2/main.co"
r=$(kirn_run "$P/proj2" run "$P/proj2"); c=${r%%|*}; out=${r#*|}
[ "$c" = 0 ] && echo "$out" | grep -q "poked" && report PASS "pin.co discovered w/o manifest" || report FAIL "pin.co discovered w/o manifest" "$out"

echo ""
echo "GATE RESULT: $pass passed, $fail failed, $hung hung"
[ "$fail" -eq 0 ]