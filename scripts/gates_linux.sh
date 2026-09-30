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
KN="$ROOT/build/kirnc"
WATCH=20
for E in "$CC" "$CR" "$CO" "$KN"; do
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

echo "=== gate 4: vm_diff (tree vs VM) ==="
hung=0
for f in "$ROOT"/examples/*.kn "$ROOT"/tests/vm_parity/*.kn; do
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
mkdir -p "$P/b/code"; printf '[package]\nname="b"\nmain="start.kn"\n' > "$P/b/kirn.toml"
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
# 7. stale coco.toml -> actionable rename hint (manifest renamed in this release)
mkdir -p "$P/e/code"; printf '[package]\nname="e"\nmain="code/main.kn"\n' > "$P/e/coco.toml"
printf 'def main() { print("E"); }' > "$P/e/code/main.kn"
r=$(kirn_run "$P/e" run "$P/e"); c=${r%%|*}; out=${r#*|}
[ "$c" = 0 ] && echo "$out" | grep -q "coco.toml" && echo "$out" | grep -q "kirn.toml" && report PASS "stale coco.toml -> rename hint" || report FAIL "stale coco.toml -> rename hint" "$out"

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
(cd "$T7" && "$CO" new pet mypet >/dev/null 2>&1)
printf 'pub def hello(who: string) -> string { return "hi from pets, " + who + "!"; }\n' > "$T7/mypet/code/pin.kn"
if (cd "$T7/mypet" && "$CO" build) >/dev/null 2>&1; then
    PET=$(find "$T7/mypet/build" -name '*.pet' | head -1)
    if [ -z "$PET" ]; then report FAIL "pack .pet" "no .pet produced in build/"
    elif [ ! -f "$T7/mypet/kirn.toml" ]; then report FAIL "pack .pet" "kirn.toml not scaffolded"
    elif [ -e "$T7/mypet/coco.toml" ]; then report FAIL "pack .pet" "stale coco.toml present"
    else
        report PASS "pack .pet" "$(basename "$PET")"
        (cd "$T7" && "$CO" new app >/dev/null 2>&1)
        if (cd "$T7/app" && "$CO" install "$PET" >/dev/null 2>&1); then
            if [ -f "$T7/app/pets/mypet/code/pin.kn" ] && [ -f "$T7/app/pets/mypet/kirn.toml" ]; then report PASS "install .pet" "pets/mypet"
            else report FAIL "install .pet" "destination pets/mypet missing"; fi
            printf 'import mypet;\ndef main() { print(mypet.hello("world")); }\n' > "$T7/app/code/main.kn"
            out7=$(cd "$T7/app" && timeout "$WATCH" "$CO" run "$T7/app" 2>&1); c7=$?
            if [ "$c7" = 0 ] && echo "$out7" | grep -q "hi from pets, world!"; then report PASS "consume .pet" "$out7"
            else report FAIL "consume .pet" "rc=$c7: $out7"; fi
        else report FAIL "install .pet" "kirn install failed"; fi
    fi
else report FAIL "pack .pet" "kirn build pet failed"; fi

echo "=== gate 8: bundle build (P1 - standalone binary, no toolchain) ==="
T8="$TMP/bd"; mkdir -p "$T8/pets/greet/code"
printf 'pub def hey(who: string) -> string { return "hi from pets, " + who + "!"; }\n' > "$T8/pets/greet/code/pin.kn"
printf 'import greet;\nimport math;\ndef main() {\n    print(greet.hey("bundle"));\n    print(math.floor(3.7));\n}\n' > "$T8/main.kn"
out8=$(cd "$T8" && timeout 240 "$CO" build main.kn -o out 2>&1); c8=$?
if [ "$c8" = 0 ] && [ -x "$T8/out" ]; then
    # real native binary (ELF magic), not a script
    magic=$(head -c4 "$T8/out")
    if [ "$magic" = "$(printf '\177ELF')" ]; then
        report PASS "bundle is ELF binary" "not a script"
        out8r=$(cd "$T8" && timeout "$WATCH" ./out 2>&1); c8r=$?
        if [ "$c8r" = 0 ] && echo "$out8r" | grep -q "hi from pets, bundle!" && echo "$out8r" | grep -q "3.0"; then
            report PASS "bundle runs (imports+stdlib)" "$out8r"
        else report FAIL "bundle runs (imports+stdlib)" "rc=$c8r: $out8r"; fi
        out8b=$(cd "$T8" && KIRN_VERBOSE=1 timeout 240 "$CO" build main.kn -o out2 2>&1); c8b=$?
        if [ "$c8b" = 0 ] && ! echo "$out8b" | grep -q '\[cmd\]'; then
            report PASS "rebuild needs no toolchain" "cached runner reused"
        else report FAIL "rebuild needs no toolchain" "rc=$c8b: $out8b"; fi
        # positive KIRN_VERBOSE check: a COLD runner warmup must emit [cmd],
        # proving the renamed switch is live (not a silent no-op - Homebrew
        # HOMEBREW_NO_ENV_FILTERING-shaped gate rot guard).
        T8c="$TMP/pv"; rm -rf "$T8c"; mkdir -p "$T8c/build" "$T8c/tools"
        cp "$CO" "$T8c/build/kirn"
        cp -r "$ROOT/src" "$T8c/src"
        cp "$ROOT/tools/runner.cpp" "$T8c/tools/runner.cpp"
        printf 'def main() { print("verbose check"); }\n' > "$T8c/main.kn"
        out8c=$(cd "$T8c/build" && KIRN_VERBOSE=1 timeout 300 ./kirn build ../main.kn -o ../out-verb 2>&1); c8c=$?
        if [ "$c8c" = 0 ] && echo "$out8c" | grep -q '\[cmd\]'; then
            report PASS "KIRN_VERBOSE logs [cmd] on cold runner warmup" "positive probe"
        else report FAIL "KIRN_VERBOSE logs [cmd] on cold runner warmup" "rc=$c8c: $out8c"; fi
    else report FAIL "bundle is ELF binary" "got magic: $(head -c2 "$T8/out")"; fi
else report FAIL "bundle build" "rc=$c8: $out8"; fi

echo ""
# ================================================== phase 2: the preprocessor
# The phase-2 verify criterion is "macros cannot capture caller locals", which is
# a property of expansion rather than a snapshot, so it is asserted here and not
# only recorded in the goldens. A golden would happily record a macro that DID
# capture a caller local; this gate fails if one ever does.
echo "=== phase 2: macro hygiene ==="

# A macro body's free identifier is alpha-renamed, so it must not resolve to a
# caller's local of the same name. The renamed name is what the checker must
# reject -- if the macro could capture, this would type-check and print 101.
cat > "$TMP/capture.kn" <<'KN'
{.def BAD() = helper + 1.}
def main() { helper = 100; print(BAD()); }
KN
out=$("$CC" "$TMP/capture.kn" 2>&1); code=$?
if [ "$code" = 1 ] && printf '%s' "$out" | grep -q "undefined variable '__kirnpre"; then
    report PASS "macro cannot capture a caller local" "renamed to __kirnpre*, checker rejects (exit 1)"
else
    report FAIL "macro cannot capture a caller local" "exit $code; out: $(printf '%s' "$out" | tr '\n' ' ')"
fi

# The mirror property: a macro that introduces a binding must neither see the
# caller's local nor leak its own. WITH_LOCAL prints 14 from its own tmp = 7 and
# the caller still prints its own 1.
out=$("$CR" "$ROOT/tests/macros/hygiene_leak.kn" 2>&1); code=$?
if [ "$code" = 0 ] && [ "$(printf '%s' "$out" | tr '\n' ' ')" = "14 1" ]; then
    report PASS "macro binding does not leak to caller" "macro 14, caller 1"
else
    report FAIL "macro binding does not leak to caller" "exit $code; out: $(printf '%s' "$out" | tr '\n' ' ')"
fi

# Parameters must bind at the CALL SITE, which is what makes a macro useful:
# hygiene_capture.kn passes the caller's helper as a parameter and gets 101.
out=$("$CR" "$ROOT/tests/macros/hygiene_capture.kn" 2>&1); code=$?
if [ "$code" = 0 ] && [ "$(printf '%s' "$out" | tr '\n' ' ')" = "101" ]; then
    report PASS "macro parameters bind at the call site" "GOOD(helper) -> 101"
else
    report FAIL "macro parameters bind at the call site" "exit $code; out: $(printf '%s' "$out" | tr '\n' ' ')"
fi

# Conditional compilation must actually elide, not blank: a name defined only in
# a dead branch must be undefined at the use site, and defined when the branch is
# live. Tested through the command line so no extra fixture is needed.
out=$("$KN" -E -DFEATURE=1 "$ROOT/tests/macros/cmdline.kn" 2>&1)
if printf '%s' "$out" | grep -q 'feature on' && ! printf '%s' "$out" | grep -q 'feature off'; then
    report PASS "conditional elides the dead branch" "-DFEATURE=1 selects the live branch"
else
    report FAIL "conditional elides the dead branch" "out: $(printf '%s' "$out" | tr '\n' ' ')"
fi

# -U must be able to undo a -D, and order must matter.
out=$("$KN" -E -DGONE=1 -UGONE "$ROOT/tests/macros/cmdline.kn" 2>&1)
if ! printf '%s' "$out" | grep -q 'gone on'; then
    report PASS "-U undoes a -D" "-DGONE=1 -UGONE leaves GONE undefined"
else
    report FAIL "-U undoes a -D" "GONE still defined"
fi
out=$("$KN" -E -UGONE -DGONE=1 "$ROOT/tests/macros/cmdline.kn" 2>&1)
if printf '%s' "$out" | grep -q 'gone on'; then
    report PASS "-D/-U are order-sensitive" "-UGONE -DGONE=1 keeps GONE defined"
else
    report FAIL "-D/-U are order-sensitive" "GONE unexpectedly undefined"
fi

# Macro bodies must COMPOSE: a macro may name another macro defined earlier.
# This is the gate that would have caught the alpha-renaming over-application
# that renamed INNER inside OUTER's body, making composition impossible.
cat > "$TMP/comp.kn" <<'KN'
{.def INNER. 5.}
{.def OUTER. INNER + 1.}
def main() { print(OUTER); }
KN
out=$("$KN" -E "$TMP/comp.kn" 2>&1)
# OUTER must have expanded all the way through INNER to the literal 5, with no
# private renamed name anywhere. A greps for the *surviving* name would be
# checking the opposite of the intent: correct composition removes INNER.
flat=$(printf '%s' "$out" | tr '\t' ' ')
if printf '%s' "$flat" | grep -q 'Int 3:20 5' && ! printf '%s' "$flat" | grep -q '__kirnpre'; then
    report PASS "macro bodies compose" "OUTER -> INNER -> 5, no private names"
else
    report FAIL "macro bodies compose" "out: $(printf '%s' "$out" | tr '\n' ' ')"
fi

# Direct self-reference is rejected at definition time with a message that names
# the cause, rather than expanding until something else gives up.
cat > "$TMP/self.kn" <<'KN'
{.def A. A.}
def main() { print(A); }
KN
out=$("$KN" -E "$TMP/self.kn" 2>&1); code=$?
if [ "$code" = 1 ] && printf '%s' "$out" | grep -q 'expands to itself'; then
    report PASS "direct self-reference is rejected" "definition-time error, exit 1"
else
    report FAIL "direct self-reference is rejected" "exit $code; out: $(printf '%s' "$out" | tr '\n' ' ')"
fi

# Mutual recursion is STRUCTURALLY impossible, not merely bounded: a body may
# only name a macro that already existed, so the reference graph only ever points
# backwards. The observable consequence is that PING does not expand to a
# renamed private name and the program simply fails to compile.
cat > "$TMP/rec.kn" <<'KN'
{.def PING. PONG.}
{.def PONG. PING.}
def main() { print(PING); }
KN
# kirncheck, not -E: -E only preprocesses and so exits 0 on a name that will not
# resolve. The point being asserted is that the reference graph only ever points
# backwards, so PING resolves to a private name instead of recursing, and the
# program is rejected when it is type-checked.
out=$(timeout "$WATCH" "$CC" "$TMP/rec.kn" 2>&1); code=$?
if [ "$code" = 124 ]; then
    report FAIL "mutual recursion cannot hang" "HANG (watchdog ${WATCH}s)"
elif [ "$code" = 1 ] && printf '%s' "$out" | grep -q '__kirnpre'; then
    report PASS "mutual recursion cannot hang" "PING -> private name, rejected at check (exit 1, no hang)"
else
    report FAIL "mutual recursion cannot hang" "exit $code; out: $(printf '%s' "$out" | tr '\n' ' ')"
fi

# A bundled build must REFUSE a macro-using program rather than ship a binary
# that cannot start. This is the section-5.6 bug class recurring one branch down:
# a bundle is raw source plus a prebuilt launcher that re-lexes it, and the
# launcher does not preprocess, so without this guard `kirn build` returned 0 and
# produced an executable that exited 65 when run.
cat > "$TMP/macbuild.kn" <<'KN'
{.def LIMIT. 3.}
def main() { print(LIMIT); }
KN
out=$("$CO" build "$TMP/macbuild.kn" -o "$TMP/macbuild.bin" 2>&1); code=$?
if [ "$code" = 65 ] && printf '%s' "$out" | grep -q 'preprocessor macros'; then
    report PASS "bundled build refuses macro sources" "exit 65 with a diagnostic (no broken artefact)"
else
    report FAIL "bundled build refuses macro sources" "exit $code; out: $(printf '%s' "$out" | tr '\n' ' ')"
fi
# ...and a macro-free program must still bundle, so the guard is not just
# refusing everything.
out=$("$CO" build "$ROOT/examples/01_hello.kn" -o "$TMP/hello.bin" 2>&1); code=$?
if [ "$code" = 0 ] && [ -x "$TMP/hello.bin" ] && "$TMP/hello.bin" 2>/dev/null | grep -q 'Hello, World!'; then
    report PASS "bundled build still works without macros" "01_hello.kn bundles and runs"
else
    report FAIL "bundled build still works without macros" "exit $code; out: $(printf '%s' "$out" | tr '\n' ' ')"
fi

# ---- HIR (phase 3) ---------------------------------------------------------
# `kirnc hir` lowers to HIR, runs the structural verifier, and prints the dump.
# Two properties, and the second is the one that matters:
#
#  1. Every file HIR claims to support verifies. The known-unsupported set is
#     pinned below rather than left to a glob, so adding a construct that breaks
#     HIR lowering shows up as a new name in the failure output, and shrinking
#     the list is a deliberate edit.
#  2. The verifier FAILS when handed a broken IR. A verifier that only ever
#     passes is indistinguishable from no verifier, and the first version of
#     hir_verify.cpp had a hand-written table of variant indices that was wrong in
#     nine places -- it was caught only because it ran against real input. So the
#     gate pins that a mutation of a known-good dump is still rejected, which is
#     the only way to know the checks have teeth.
# Constructs HIR does not lower yet. Each is a real language form whose
# declaring form is still an Item::Unhandled or an unlowered pattern, so names it
# binds do not resolve. Named explicitly so a NEW failure is visible.
HIR_KNOWN_UNSUPPORTED="17_tuples_swap.kn 20_defer_panic.kn 23_select_multiplex.kn 29_arena_allocation.kn 30_capstone_wordcount.kn 31_advanced_patterns.kn 38_patterns_power.kn 40_keywords.kn scoping.kn"
echo "=== gate 8: HIR structural verification ==="
HIR_OK=0
HIR_FAILED=""
for src in "$ROOT"/examples/*.kn "$ROOT"/tests/types/p*.kn "$ROOT"/tests/vm_parity/*.kn; do
    [ -e "$src" ] || continue
    base="$(basename "$src")"
    case " $HIR_KNOWN_UNSUPPORTED " in
        *" $base "*) continue ;;
    esac
    if "$KN" hir "$src" >/dev/null 2>&1; then
        HIR_OK=$((HIR_OK+1))
    else
        HIR_FAILED="$HIR_FAILED $base"
    fi
done
if [ -z "$HIR_FAILED" ]; then
    report PASS "HIR verifies on every supported corpus file" "$HIR_OK files lowered, verified and dumped"
else
    report FAIL "HIR verifies on every supported corpus file" "failed:$HIR_FAILED"
fi
# ...and the verifier REJECTS, rather than passing everything. A verifier that
# only ever passes is indistinguishable from no verifier.
#
# The input is a real corpus file whose declarations HIR does not lower yet, so
# this is genuine broken output rather than an injected fault. An earlier version
# of this test used a hand-written file with an undefined name, which was wrong:
# sema::Checker rejects that first, so HIR never runs and the test proved nothing
# about the verifier. It also pins the ordering that matters -- HIR must not
# lower code that failed type checking.
cat > "$TMP/hirunchecked.kn" <<'KN'
def main() {
    print(no_such_name_anywhere);
}
KN
if "$KN" hir "$TMP/hirunchecked.kn" 2>&1 | grep -q 'error.*undefined'; then
    report PASS "HIR does not lower code that failed type checking" "the checker rejects it first, as it must"
else
    report FAIL "HIR does not lower code that failed type checking" "a checker error was not reported"
fi
first_unsupported=""
for src in "$ROOT"/examples/*.kn "$ROOT"/tests/types/p*.kn "$ROOT"/tests/vm_parity/*.kn; do
    [ -e "$src" ] || continue
    base="$(basename "$src")"
    case " $HIR_KNOWN_UNSUPPORTED " in *" $base "*) first_unsupported="$src"; break ;; esac
done
if [ -n "$first_unsupported" ]; then
    if "$KN" hir "$first_unsupported" 2>&1 | grep -q 'verification failed'; then
        report PASS "HIR verifier rejects broken output" "$(basename "$first_unsupported"): violations reported, exit 65"
    else
        report FAIL "HIR verifier rejects broken output" "$(basename "$first_unsupported") produced no verification diagnostic"
    fi
else
    report FAIL "HIR verifier rejects broken output" "no known-unsupported file to test against"
fi
# The dump must be deterministic: a golden comparison is only meaningful if the
# text does not move between runs. Two runs, byte for byte.
"$KN" hir "$ROOT/examples/05_loops_ranges.kn" > "$TMP/h1" 2>&1
"$KN" hir "$ROOT/examples/05_loops_ranges.kn" > "$TMP/h2" 2>&1
if cmp -s "$TMP/h1" "$TMP/h2"; then
    report PASS "HIR dump is deterministic" "two runs byte-identical"
else
    report FAIL "HIR dump is deterministic" "dump differs between runs"
fi

echo "GATE RESULT: $pass passed, $fail failed, $hung hung"
[ "$fail" -eq 0 ]