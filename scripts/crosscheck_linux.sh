#!/usr/bin/env bash
# crosscheck_linux.sh — Phase 1 verify criterion.
#
#   kirnc <verb> output  ==  <old tool> output     (byte for byte, plus exit code)
#
# The diff is on the BODY only, never on the argv line: "kirnlex --dump x.kn"
# and "kirnc lex --emit-tokens x.kn" are different command lines by
# construction, so comparing the recorded command would compare nothing.
#
# This is the harness that can fail. It is a real cross-check between two
# binaries, not a golden file that both sides write into: if someone "fixes"
# the shared code and breaks one side, this goes red.
set -uo pipefail
cd "$(dirname "$0")/.."
ROOT=$PWD
BIN=$ROOT/build

pass=0
fail=0
failed_desc=()

# cmp_case <desc> <old-exit> <old-out> <new-exit> <new-out>
cmp_case() {
    local desc=$1 a_rc=$2 a_out=$3 b_rc=$4 b_out=$5
    if [ "$a_out" = "$b_out" ] && [ "$a_rc" = "$b_rc" ]; then
        pass=$((pass + 1))
        [ -n "${VERBOSE:-}" ] && printf '  PASS  %-52s exit=%s\n' "$desc" "$a_rc"
        return 0
    fi
    fail=$((fail + 1))
    failed_desc+=("$desc")
    printf '  FAIL  %-52s old exit=%s new exit=%s\n' "$desc" "$a_rc" "$b_rc"
    diff <(printf '%s\n' "$a_out") <(printf '%s\n' "$b_out") \
        | head -"${DIFFLINES:-8}" | sed 's/^/          /'
    return 1
}

# run <desc> <oldcmd...> -- <newcmd...>
run() {
    local desc=$1
    shift
    local old=() new=()
    local seen=0 a
    for a in "$@"; do
        if [ "$a" = "--" ]; then seen=1; continue; fi
        if [ $seen -eq 0 ]; then old+=("$a"); else new+=("$a"); fi
    done
    local ao bo ar br
    ao=$("${old[@]}" 2>&1); ar=$?
    bo=$("${new[@]}" 2>&1); br=$?
    cmp_case "$desc" "$ar" "$ao" "$br" "$bo"
}

# run_name <desc> <old-name> <oldcmd...> -- <newcmd...>
#
# The same cross-check, but with the invoked program name normalised. Seven
# cases CANNOT be byte-identical and must not be: they print the tool's own
# name, so "usage: kirncheck <file.kn>" can never equal "usage: kirnc
# <file.kn>". That difference is correct -- a different tool should name itself
# -- so the criterion is relaxed to "identical once the program name is mapped
# across". This is still a real assertion: the exit code must match exactly and
# every other byte must match, so a genuine regression in the usage text or the
# error format still fails here.
run_name() {
    local desc=$1 oldname=$2
    shift 2
    local old=() new=()
    local seen=0 a
    for a in "$@"; do
        if [ "$a" = "--" ]; then seen=1; continue; fi
        if [ $seen -eq 0 ]; then old+=("$a"); else new+=("$a"); fi
    done
    local ao bo ar br
    ao=$("${old[@]}" 2>&1 | sed "s/\b$oldname\b/kirnc/g"); ar=${PIPESTATUS[0]}
    bo=$("${new[@]}" 2>&1); br=$?
    cmp_case "$desc [name-normalised]" "$ar" "$ao" "$br" "$bo"
}

for t in kirnc kirnlex kirnparse kirncheck kirnrun kirn; do
    if [ ! -x "$BIN/$t" ]; then
        echo "crosscheck: missing $BIN/$t (build first)" >&2
        exit 1
    fi
done

# ------------------------------------------------------------------ corpora
# The same three corpora the golden suite uses, so a divergence here cannot be
# explained by "we only checked the easy input".
FILES=()
for d in examples tests/negative stdlib; do
    [ -d "$d" ] && while IFS= read -r f; do FILES+=("$f"); done \
        < <(find "$d" -name '*.kn' -type f | sort)
done
echo "crosscheck: ${#FILES[@]} .kn files across examples/ tests/negative/ stdlib/"

# ------------------------------------------------------------------ lex verb
# kirnlex has no --emit-tokens; its dump is on by default via --dump.
for f in "${FILES[@]}"; do
    run "lex --dump $f"  "$BIN/kirnlex" --dump "$f" -- \
                       "$BIN/kirnc" lex --emit-tokens "$f"
    run "lex $f"         "$BIN/kirnlex" "$f"      -- \
                       "$BIN/kirnc" lex "$f"
done
# A directory walk: exercises collect() + sort + the N/M denominator.
for d in examples tests/negative stdlib; do
    [ -d "$d" ] && run "lex <dir> $d" "$BIN/kirnlex" "$d" -- "$BIN/kirnc" lex "$d"
done
# --dump is a no-op for >1 file, so it must stay a no-op in kirnc too.
run "lex --dump <dir> (suppressed)" "$BIN/kirnlex" --dump examples -- \
                                 "$BIN/kirnc" lex --emit-tokens examples

# --------------------------------------------------------------- parse verb
for f in "${FILES[@]}"; do
    run "parse --ast $f" "$BIN/kirnparse" --ast "$f" -- "$BIN/kirnc" parse --emit-ast "$f"
    run "parse $f"        "$BIN/kirnparse" "$f"      -- "$BIN/kirnc" parse "$f"
done

# --------------------------------------------------------------- check verb
for f in "${FILES[@]}"; do
    run "check $f"         "$BIN/kirncheck" "$f"      -- "$BIN/kirnc" check "$f"
    run "check --plain $f" "$BIN/kirncheck" --plain "$f" -- "$BIN/kirnc" check --plain "$f"
done

# --------------------------------------------------- error and usage paths
# The exit-code matrix is where the old tools disagree with each other, so
# these are pinned individually rather than assumed.
TMP=$(mktemp -d "${TMPDIR:-/tmp}/kirn-xcheck.XXXXXX")
trap 'rm -rf "$TMP"' EXIT
printf 'def main() -> none {\n  print(undefined_name);\n}\n' > "$TMP/ty.kn"
printf 'def main( {\n}\n' > "$TMP/syn.kn"

for f in "$TMP/ty.kn" "$TMP/syn.kn"; do
    run "check <error> $(basename "$f")" "$BIN/kirncheck" "$f" -- "$BIN/kirnc" check "$f"
done
# The cases below print the invoked tool's name, so they are name-normalised
# rather than byte-identical. Everything else in this script is byte-exact.
run_name "check missing file"  kirncheck "$BIN/kirncheck" "$TMP/nope.kn" -- "$BIN/kirnc" check "$TMP/nope.kn"
run_name "parse missing file"  kirnparse "$BIN/kirnparse" "$TMP/nope.kn" -- "$BIN/kirnc" parse "$TMP/nope.kn"
# kirnlex's missing-file message has no tool prefix at all ("<path>: cannot read
# file"), so it is byte-identical and belongs in the strict group above.
run      "lex missing file"    "$BIN/kirnlex" "$TMP/nope.kn" -- "$BIN/kirnc" lex "$TMP/nope.kn"
run_name "lex no args"         kirnlex   "$BIN/kirnlex"   -- "$BIN/kirnc" lex
run_name "parse no args"       kirnparse "$BIN/kirnparse" -- "$BIN/kirnc" parse
run_name "check no args"       kirncheck "$BIN/kirncheck" -- "$BIN/kirnc" check
# kirnlex never had -h, so it treats "-h" as a path. That quirk must survive
# into `kirnc lex` byte for byte: same message, same exit code.
run      "lex -h (not a flag)" "$BIN/kirnlex" -h -- "$BIN/kirnc" lex -h
run_name "parse --help"        kirnparse "$BIN/kirnparse" --help -- "$BIN/kirnc" parse --help
run_name "check --help"        kirncheck "$BIN/kirncheck" --help -- "$BIN/kirnc" check --help

echo
echo "CROSSCHECK RESULT: $pass passed, $fail failed"
if [ "$fail" -ne 0 ]; then
    printf '  diverged: %s\n' "${failed_desc[@]}" | head -20
    exit 1
fi
