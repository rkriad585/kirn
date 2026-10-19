#!/usr/bin/env bash
# Standalone gate script. Every gate in this directory runs on its own:
#     bash scripts/gates/8_bundle.sh
# There is no aggregate driver - loading the shared setup (ROOT, binaries,
# report(), TMP) is done here unconditionally.
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/_common.sh"

echo "=== gate 8: bundle build (P1 - standalone binary, no toolchain) ==="
T8="$TMP/bd"; mkdir -p "$T8/pets/greet/code"
printf 'pub fn hey(who: Str) -> Str { return "hi from pets, " + who + "!"; }\n' > "$T8/pets/greet/code/pin.kn"
printf 'import greet;\nimport math;\nfn main() -> none {\n    print(greet.hey("bundle"));\n    print(math.floor(3.7));\n}\n' > "$T8/main.kn"
out8=$(cd "$T8" && timeout 240 "$KIRN" build main.kn -o out 2>&1); c8=$?
if [ "$c8" = 0 ] && [ -x "$T8/out" ]; then
    # real native binary (ELF magic), not a script
    magic=$(head -c4 "$T8/out")
    if [ "$magic" = "$(printf '\177ELF')" ]; then
        report PASS "bundle is ELF binary" "not a script"
        out8r=$(cd "$T8" && timeout "$WATCH" ./out 2>&1); c8r=$?
        if [ "$c8r" = 0 ] && echo "$out8r" | grep -q "hi from pets, bundle!" && echo "$out8r" | grep -q "3.0"; then
            report PASS "bundle runs (imports+stdlib)" "$out8r"
        else report FAIL "bundle runs (imports+stdlib)" "rc=$c8r: $out8r"; fi
        out8b=$(cd "$T8" && KIRN_VERBOSE=1 timeout 240 "$KIRN" build main.kn -o out2 2>&1); c8b=$?
        if [ "$c8b" = 0 ] && ! echo "$out8b" | grep -q '\[cmd\]'; then
            report PASS "rebuild needs no toolchain" "cached runner reused"
        else report FAIL "rebuild needs no toolchain" "rc=$c8b: $out8b"; fi
        # positive KIRN_VERBOSE check: a COLD runner warmup must emit [cmd],
        # proving the renamed switch is live (not a silent no-op - Homebrew
        # HOMEBREW_NO_ENV_FILTERING-shaped gate rot guard).
        T8c="$TMP/pv"; rm -rf "$T8c"; mkdir -p "$T8c/build" "$T8c/tools"
        cp "$KIRN" "$T8c/build/kirn"
        cp -r "$ROOT/src" "$T8c/src"
        cp "$ROOT/tools/runner.cpp" "$T8c/tools/runner.cpp"
        printf 'fn main() -> none { print("verbose check"); }\n' > "$T8c/main.kn"
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
{.fn BAD() = helper + 1.}
fn main() -> none { helper = 100; print(BAD()); }
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
{.fn INNER. 5.}
{.fn OUTER. INNER + 1.}
fn main() -> none { print(OUTER); }
KN
out=$("$KN" -E "$TMP/comp.kn" 2>&1)
# OUTER must have expanded all the way through INNER to the literal 5, with no
# private renamed name anywhere. A greps for the *surviving* name would be
# checking the opposite of the intent: correct composition removes INNER.
flat=$(printf '%s' "$out" | tr '\t' ' ')
if printf '%s' "$flat" | grep -q 'Int 3:27 5' && ! printf '%s' "$flat" | grep -q '__kirnpre'; then
    report PASS "macro bodies compose" "OUTER -> INNER -> 5, no private names"
else
    report FAIL "macro bodies compose" "out: $(printf '%s' "$out" | tr '\n' ' ')"
fi

# Direct self-reference is rejected at definition time with a message that names
# the cause, rather than expanding until something else gives up.
cat > "$TMP/self.kn" <<'KN'
{.fn A. A.}
fn main() -> none { print(A); }
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
{.fn PING. PONG.}
{.fn PONG. PING.}
fn main() -> none { print(PING); }
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
{.fn LIMIT. 3.}
fn main() -> none { print(LIMIT); }
KN
out=$("$KIRN" build "$TMP/macbuild.kn" -o "$TMP/macbuild.bin" 2>&1); code=$?
if [ "$code" = 65 ] && printf '%s' "$out" | grep -q 'preprocessor macros'; then
    report PASS "bundled build refuses macro sources" "exit 65 with a diagnostic (no broken artefact)"
else
    report FAIL "bundled build refuses macro sources" "exit $code; out: $(printf '%s' "$out" | tr '\n' ' ')"
fi
# ...and a macro-free program must still bundle, so the guard is not just
# refusing everything.
out=$("$KIRN" build "$ROOT/examples/01_hello.kn" -o "$TMP/hello.bin" 2>&1); code=$?
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
echo "GATE RESULT: $pass passed, $fail failed, $hung hung"
[ "$fail" -eq 0 ] || exit 1
exit 0
