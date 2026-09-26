#!/usr/bin/env bash
# Linux gate harness - bash port of the seven PowerShell verification gates:
#   1. runall      - every examples/*.kn through kirnrun (20s watchdog),
#                    honoring '# expect-exit: N' (POSIX exit codes are 8-bit,
#                    so N is compared as N % 256).
#   2. types       - tests/types/p*.kn must check+run clean;
#                    tests/types/n*.kn must fail with the '# expect:' substring.
#   3. negative    - tests/negative/n*.kn must fail kirncheck with the
#                    '# expect:' substring.
#   4. vm_diff     - tree-walker (--no-vm) vs bytecode VM parity on
#                    examples matching '^[0-9]' (code + combined output).
#   5. conventions - rules/convention matrix for the `kirn` driver (run-entry
#                    resolution + pin.kn package initializer).
#   6. test        - `kirn test .` runs the stdlib/pet *_test.kn suite (9
#                    tests, must report '9 passed, 0 failed').
#   7. pets        - .pet bundle round-trip: pack a lib, install the bundle
#                    into a consuming project's pets/ dir, consume it at run.
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

echo "=== gate 2: types (tests/types) ==="
for t in "$ROOT"/tests/types/p*.kn; do
    name="$(basename "$t")"; err="$("$CC" "$t" 2>&1)"; [ $? -eq 0 ] || { report FAIL "$name" "kirncheck rejected: $(echo "$err" | head -1)"; continue; }
    err="$("$CR" "$t" 2>&1)"; [ $? -eq 0 ] || { report FAIL "$name" "kirnrun exit nonzero: $(echo "$err" | head -1)"; continue; }
    report PASS "$name" "check + run clean"
done
for t in "$ROOT"/tests/types/n*.kn "$ROOT"/tests/negative/n*.kn; do
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

echo "=== gate 4: vm_diff (examples ^[0-9], tree vs VM) ==="
hung=0
for f in "$ROOT"/examples/[0-9]*.kn; do
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
# 1. code/main.kn entry
mkdir -p "$P/a/code"; printf 'def main() { print("A"); }' > "$P/a/code/main.kn"
r=$(kirn_run "$P/a" run "$P/a"); c=${r%%|*}; out=${r#*|}
[ "$c" = 0 ] && echo "$out" | grep -q "A" && report PASS "code/main.kn entry" || report FAIL "code/main.kn entry" "$out"
# 2. manifest main wins over code/main.kn
mkdir -p "$P/b/code"; printf '[package]\nname="b"\nmain="start.kn"\n' > "$P/b/coco.toml"
printf 'def main() { print("Bmain"); }' > "$P/b/start.kn"
printf 'def main() { print("Bcode"); }' > "$P/b/code/main.kn"
r=$(kirn_run "$P/b" run "$P/b"); c=${r%%|*}; out=${r#*|}
[ "$c" = 0 ] && echo "$out" | grep -q "Bmain" && report PASS "manifest main preferred" || report FAIL "manifest main preferred" "$out"
# 3. pin.kn as run entry
mkdir -p "$P/c"; printf 'pub def main() { print("C"); }' > "$P/c/pin.kn"
r=$(kirn_run "$P/c" run "$P/c"); c=${r%%|*}; out=${r#*|}
[ "$c" = 0 ] && echo "$out" | grep -q "C" && report PASS "pin.kn as run entry" || report FAIL "pin.kn as run entry" "$out"
# 4. no entry -> fix-it error
mkdir -p "$P/d/code"; printf 'pub def u() -> int { return 1; }' > "$P/d/code/util.kn"
r=$(kirn_run "$P/d" run "$P/d"); c=${r%%|*}; out=${r#*|}
[ "$c" != 0 ] && echo "$out" | grep -q "no entry point" && report PASS "no entry -> fix-it error" || report FAIL "no entry -> fix-it error" "$out"
# 5. pin.kn runs once + pub surface
mkdir -p "$P/proj/pets/greet/code"
printf 'var loads = 0;\nloads = loads + 1;\npub def load_count() -> int { return loads; }\npub def hi(who: string) -> string { return "hi " + who; }\n' > "$P/proj/pets/greet/code/pin.kn"
printf 'import greet;\nimport greet as g2;\nimport greet as g3;\ndef main() {\n    print("count=", greet.load_count());\n    print(greet.hi("world"));\n}\n' > "$P/proj/main.kn"
r=$(kirn_run "$P/proj" run "$P/proj"); c=${r%%|*}; out=${r#*|}
[ "$c" = 0 ] && echo "$out" | grep -q "count= 1" && report PASS "pin.kn runs once (count=1)" || report FAIL "pin.kn runs once (count=1)" "$out"
[ "$c" = 0 ] && echo "$out" | grep -q "hi world" && report PASS "pin.kn pub surface" || report FAIL "pin.kn pub surface" "$out"
# 6. pin.kn discovered without manifest
mkdir -p "$P/proj2/pets/nomanifest/code"
printf 'pub def poke() -> string { return "poked"; }\n' > "$P/proj2/pets/nomanifest/code/pin.kn"
printf 'import nomanifest;\ndef main() { print(nomanifest.poke()); }\n' > "$P/proj2/main.kn"
r=$(kirn_run "$P/proj2" run "$P/proj2"); c=${r%%|*}; out=${r#*|}
[ "$c" = 0 ] && echo "$out" | grep -q "poked" && report PASS "pin.kn discovered w/o manifest" || report FAIL "pin.kn discovered w/o manifest" "$out"

echo "=== gate 6: test (stdlib via kirn driver) ==="
rm -f "$ROOT/.io_tmp_test.txt"   # io_test.kn uses append-only scratch; no unlink primitive
out6=$(cd "$ROOT" && timeout "$WATCH" "$CO" test . 2>&1); c6=$?
rm -f "$ROOT/.io_tmp_test.txt"
if [ "$c6" = 0 ] && echo "$out6" | grep -q "9 passed, 0 failed"; then
    report PASS "kirn test stdlib/pet" "$(echo "$out6" | grep -c 'PASS') tests"
else
    report FAIL "kirn test stdlib/pet" "rc=$c6: $(echo "$out6" | tail -1)"
fi

echo "=== gate 7: pets (.pet pack -> install -> consume) ==="
T7="$TMP/rt"; mkdir -p "$T7"
(cd "$T7" && "$CO" new lib mypet >/dev/null 2>&1)
printf 'pub def hello(who: string) -> string { return "hi from pets, " + who + "!"; }\n' > "$T7/mypet/code/pin.kn"
if (cd "$T7/mypet" && "$CO" build) >/dev/null 2>&1; then
    PET=$(find "$T7/mypet/build" -name '*.pet' | head -1)
    if [ -z "$PET" ]; then report FAIL "pack .pet" "no .pet produced in build/"
    else
        report PASS "pack .pet" "$(basename "$PET")"
        (cd "$T7" && "$CO" new app >/dev/null 2>&1)
        if (cd "$T7/app" && "$CO" install "$PET" >/dev/null 2>&1); then
            if [ -f "$T7/app/pets/libs/mypet/code/pin.kn" ]; then report PASS "install .pet" "pets/libs/mypet"
            else report FAIL "install .pet" "destination pets/libs/mypet missing"; fi
            printf 'import mypet;\ndef main() { print(mypet.hello("world")); }\n' > "$T7/app/code/main.kn"
            out7=$(cd "$T7/app" && timeout "$WATCH" "$CO" run "$T7/app" 2>&1); c7=$?
            if [ "$c7" = 0 ] && echo "$out7" | grep -q "hi from pets, world!"; then report PASS "consume .pet" "$out7"
            else report FAIL "consume .pet" "rc=$c7: $out7"; fi
        else report FAIL "install .pet" "kirn install failed"; fi
    fi
else report FAIL "pack .pet" "kirn build lib failed"; fi

echo ""
echo "GATE RESULT: $pass passed, $fail failed, $hung hung"
[ "$fail" -eq 0 ]