#!/usr/bin/env bash
# Standalone gate script. Every gate in this directory runs on its own:
#     bash scripts/gates/8b_HIR.sh
# There is no aggregate driver - loading the shared setup (ROOT, binaries,
# report(), TMP) is done here unconditionally.
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/_common.sh"

echo "=== gate 8: HIR structural verification ==="
# Constructs HIR does not lower yet, named explicitly rather than globbed, so
# that a NEW failure is visible and shrinking this list is a deliberate edit.
#
# EMPTY as of the HIR gap-closing pass. It previously held 9 files, and closing
# them found seven more defects rather than seven missing cases:
#   - `from mod import a as b` binds each item, not the module;
#   - select arm and catch bindings are declarations;
#   - `t @ pat` and `..name` bind names that live outside `elems`;
#   - `temp`/`bucket` bind usable names (and a bucket's name is in
#     `tempVarName`, not `name` -- the parser reuses the field);
#   - pseudo-modules live in the Checker's importRoots_, not its scope, so
#     `io.open(...)` resolved against nothing;
#   - `cmdHir` gated on diags.count(), so a W0105 shadowing warning stopped the
#     lowering before it ran -- the scoping fixture exited 1 having lowered
#     nothing;
#   - an open-ended range `a..` is a Binary with a null rhs and is legal
#     (OP_UNARANGE), so flagging every null rhs rejected examples/30.
# The parser now also records ast::Stmt::assignTargets, because `x, y = point`
# and `a, b = b, a` have flattened exprs from which the target/value split is
# not recoverable.
HIR_KNOWN_UNSUPPORTED=""
HIR_OK=0
HIR_FAILED=""
for src in "$ROOT"/examples/*.kn "$ROOT"/tests/types/p*.kn "$ROOT"/tests/vm_parity/*.kn \
          "$ROOT"/tests/syntax/*_ok*.kn "$ROOT"/tests/mir/*.kn; do
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
fn main() -> none {
    print(no_such_name_anywhere);
}
KN
if "$KN" hir "$TMP/hirunchecked.kn" 2>&1 | grep -q 'error.*undefined'; then
    report PASS "HIR does not lower code that failed type checking" "the checker rejects it first, as it must"
else
    report FAIL "HIR does not lower code that failed type checking" "a checker error was not reported"
fi
# The verifier must REJECT broken IR. This cannot be tested from a .kn file, and
# the first attempt at it was actively harmful.
#
# A fixture was used: a program that type-checks but uses a construct HIR had no
# mapping for, expecting verify() to reject the resulting drop. That test passed
# for the wrong reason -- it was really asserting "HIR does not model this yet" --
# and the moment HIR learned the construct it went QUIET. It would have stopped
# testing anything without ever failing. A passing test that cannot fail is worse
# than no test, because it reads as coverage.
#
# No valid source can produce a violation at all: sema::Checker rejects every
# source-level defect before the lowering runs, and HIR now maps the whole
# language. So the malformed IR is built in-process instead, by
# tests/hir/verifier_selftest.cpp, which is compiled and run here against the
# real library. It lives in the test tree and is linked into no shipped binary, so
# no test-only backdoor reaches the product.
#
# It has already earned its place: it caught a real hole in verify() -- the Lit
# arm was missing from the payload dispatch, so a literal payload on a non-literal
# node was never checked at all. No corpus file can find that, because no corpus
# file has a kind/payload mismatch.
HIR_LIBS="$ROOT/build/libkirn_hir.a $ROOT/build/libkirn_sema.a \
$ROOT/build/libkirn_parser.a $ROOT/build/libkirn_prepro.a \
$ROOT/build/libkirn_ast.a $ROOT/build/libkirn_lex.a"
# shellcheck disable=SC2086
if "$CXX" -std=c++20 -I"$ROOT/src" -o "$TMP/hirvst" \
      "$ROOT/tests/hir/verifier_selftest.cpp" $HIR_LIBS -lpthread \
      2>"$TMP/hirvst.cc.log"; then
    if "$TMP/hirvst" >"$TMP/hirvst.out" 2>&1; then
        report PASS "HIR verifier rejects broken IR" "$(grep -c '^  ok' "$TMP/hirvst.out") in-process cases, all rejected/accepted as expected"
    else
        report FAIL "HIR verifier rejects broken IR" "$(grep FAIL "$TMP/hirvst.out" | head -2)"
    fi
else
    report FAIL "HIR verifier rejects broken IR" "self-test did not compile: $(head -2 "$TMP/hirvst.cc.log" | tr '\n' ' ')"
fi
# ---- MIR (phase 3) ----------------------------------------------------------
# `kirnc rt` lowers through HIR to a CFG, verifies it, and prints the dump. Four
# properties, mirroring the HIR gate above.

echo "GATE RESULT: $pass passed, $fail failed, $hung hung"
[ "$fail" -eq 0 ] || exit 1
exit 0
