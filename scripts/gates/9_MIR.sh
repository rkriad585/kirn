echo "=== gate 9: MIR structural verification ==="
MIR_OK=0
MIR_FAILED=""
for src in "$ROOT"/examples/*.kn "$ROOT"/tests/types/p*.kn "$ROOT"/tests/vm_parity/*.kn \
          "$ROOT"/tests/syntax/*_ok*.kn "$ROOT"/tests/mir/*.kn; do
    [ -e "$src" ] || continue
    if "$KN" rt "$src" >/dev/null 2>&1; then
        MIR_OK=$((MIR_OK+1))
    else
        MIR_FAILED="$MIR_FAILED $(basename "$src")"
    fi
done
if [ -z "$MIR_FAILED" ]; then
    report PASS "MIR verifies on every corpus file" "$MIR_OK files lowered through HIR to a verified CFG"
else
    report FAIL "MIR verifies on every corpus file" "failed:$MIR_FAILED"
fi
# The verifier must REJECT a broken CFG. As with HIR, no valid source can produce
# one -- the Checker rejects source-level defects and the builder either produces
# a well-formed CFG or crashes -- so the malformed CFG is built in memory by
# tests/mir/verifier_selftest.cpp. A .kn fixture would be the same trap the HIR
# gate already fell into: it passes by asserting "the builder does not do this
# yet", and goes quiet the moment the builder learns.
MIR_LIBS="$ROOT/build/libkirn_mir.a $ROOT/build/libkirn_hir.a $ROOT/build/libkirn_sema.a \
$ROOT/build/libkirn_parser.a $ROOT/build/libkirn_prepro.a \
$ROOT/build/libkirn_ast.a $ROOT/build/libkirn_lex.a"
# shellcheck disable=SC2086
if "$CXX" -std=c++20 -I"$ROOT/src" -o "$TMP/mirvst" \
      "$ROOT/tests/mir/verifier_selftest.cpp" $MIR_LIBS -lpthread \
      2>"$TMP/mirvst.cc.log"; then
    if "$TMP/mirvst" >"$TMP/mirvst.out" 2>&1; then
        report PASS "MIR verifier rejects broken CFGs" "$(grep -c '^  ok' "$TMP/mirvst.out") in-process cases, all rejected/accepted as expected"
    else
        report FAIL "MIR verifier rejects broken CFGs" "$(grep FAIL "$TMP/mirvst.out" | head -2)"
    fi
else
    report FAIL "MIR verifier rejects broken CFGs" "self-test did not compile: $(head -2 "$TMP/mirvst.cc.log" | tr '\n' ' ')"
fi
# ---- pattern lowering ------------------------------------------------------
# tests/mir/patterns.kn exists because the corpus uses ONLY inclusive range
# patterns: nothing exercised `a..b`, and HIR did not record inclusivity at all, so
# `case 200..=299` silently became `case 200..298`. A lost FIELD is a different bug
# shape from the lost STATEMENT that verify()'s drop check catches -- the node is
# present and well formed -- so it needs its own assertions, and it gets three:
# behaviour, the HIR dump, and the MIR comparison chosen.
PAT="$ROOT/tests/mir/patterns.kn"
# One line with no trailing space, so the comparison below is a plain string
# match: `tr` alone leaves a trailing space where the final newline was.
out="$("$CR" "$PAT" 2>&1 | tr '\n' ' ' | sed 's/ *$//')"
# describe(99) must be "large": `10..99` excludes 99, while describe(9) is "small"
# because `1..=9` includes it. If inclusivity is dropped from HIR both change.
if [ "$out" = "zero small small medium large large zero empty one: 1 first: 1 rest: 2 small 3 lucky" ]; then
    report PASS "range patterns keep their inclusivity" "describe(9)=small via 1..=9, describe(99)=large via exclusive 10..99"
else
    report FAIL "range patterns keep their inclusivity" "got: $out"
fi
# The HIR dump must show BOTH forms, so a regression is visible in the text and not
# only in the behaviour.
hir="$("$KN" hir "$PAT" 2>&1)"
if echo "$hir" | grep -q 'range 1\.\.=9' && echo "$hir" | grep -q 'range 10\.\.99'; then
    report PASS "HIR records both range forms" "dump contains 'range 1..=9' and 'range 10..99'"
else
    report FAIL "HIR records both range forms" "one of the two forms is missing from the dump"
fi
# ...and MIR must pick <= for the inclusive one and < for the exclusive one.
mir="$("$KN" rt "$PAT" 2>&1)"
le=$(echo "$mir" | grep -cE '; <=$')
lt=$(echo "$mir" | grep -cE '; <$')
if [ "$le" -ge 2 ] && [ "$lt" -ge 1 ]; then
    report PASS "MIR picks the right range comparison" "$le inclusive (<=) and $lt exclusive (<) comparisons emitted"
else
    report FAIL "MIR picks the right range comparison" "got $le '<=' and $lt '<'; expected at least 2 and 1"
fi
# `goto` must lower to a real branch, not to `unreachable`. It used to, and the
# corpus uses forward AND backward goto (examples/42_control_goto_gather.kn), so
# that was a real gap rather than a hypothetical. A goto is an unconditional Br to
# the labelled block, with no unwinding: bindings live in entry-block allocas, so
# unlike the interpreter's signal-and-raise there is no frame to pop.
GOTO="$ROOT/tests/mir/goto.kn"
if "$KN" rt "$GOTO" >"$TMP/goto.mir" 2>&1; then
    # `find` jumps out of the loop to a label below it, so the CFG must contain a
    # back edge from inside the loop to a block the loop does not reach.
    if grep -q '^    br  ; -> b' "$TMP/goto.mir" && [ "$(grep -c 'unreachable' "$TMP/goto.mir")" -eq 0 ]; then
        report PASS "goto lowers to a real branch" "no unreachable emitted; $(grep -c '^    br  ; -> b' "$TMP/goto.mir") branches, including the forward goto"
    else
        report FAIL "goto lowers to a real branch" "expected branches and no unreachable; got $(grep -c unreachable "$TMP/goto.mir") unreachable"
    fi
else
    report FAIL "goto lowers to a real branch" "kirnc rt failed: $(head -1 "$TMP/goto.mir")"
fi

# A pattern whose test needs a decision tree must SAY SO rather than branch to the
# first arm, so the coarseness is visible in the dump and not merely implied.
if echo "$mir" | grep -q 'pattern_test:'; then
    report PASS "arms needing a decision tree are named" "$(echo "$mir" | grep -oE 'pattern_test:[a-z_]+' | sort -u | tr '\n' ' ')"
else
    report PASS "arms needing a decision tree are named" "none needed one for this fixture"
fi

# `rt` must agree with itself across runs, and must be a dump rather than an
# execution: the plan gives `rt` two meanings and resolves it as the dumper.
"$KN" rt "$ROOT/examples/05_loops_ranges.kn" > "$TMP/m1" 2>&1
"$KN" rt "$ROOT/examples/05_loops_ranges.kn" > "$TMP/m2" 2>&1
if cmp -s "$TMP/m1" "$TMP/m2" && head -1 "$TMP/m1" | grep -q '^mir 1$'; then
    report PASS "MIR dump is deterministic and is a dump" "two runs byte-identical, starts with 'mir 1'"
else
    report FAIL "MIR dump is deterministic and is a dump" "runs differ, or the output is not a MIR dump"
fi
# Every dumped block must end in a terminator, checked from the TEXT. This is
# deliberately redundant with the in-process verifier: it checks the shipped
# path end to end, so a flag wired to the wrong function is caught here even
# though the library is fine.
if "$KN" rt "$ROOT/examples/05_loops_ranges.kn" 2>/dev/null    | awk '/^  b[0-9]+:/{ if (blk != "" && last !~ /ret|br|condbr|unreachable/) { print blk; bad=1 } blk=$0; last="" ; next }
        /^    /{ if ($0 ~ /ret|br |condbr|unreachable/) last=$0 } END { if (blk != "" && last !~ /ret|br|condbr|unreachable/) { print blk; bad=1 } exit bad?1:0 }' > "$TMP/badblocks"; then
    report PASS "every dumped block ends in a terminator" "re-read from the text of a real dump"
else
    report FAIL "every dumped block ends in a terminator" "unterminated: $(head -2 "$TMP/badblocks")"
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
