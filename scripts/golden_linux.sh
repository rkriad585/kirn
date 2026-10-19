#!/usr/bin/env bash
# golden_linux.sh - capture (--record) or verify (default) the observable
# behavior of every driver/tool against scripts/golden/.
#
# Why this exists: the gate scripts (scripts/gates/*.sh) assert only exit codes,
# one stdout==stdout comparison. It stays green while whitespace, ordering,
# stderr wording, and the kirnrun-vs-`kirn run` resolver divergence all change
# freely. Phase 0 (driver-core extraction) is a behavior-preserving refactor,
# so it needs an oracle that actually pins bytes.
#
#   bash scripts/golden_linux.sh --record   # rewrite scripts/golden/ from the
#                                           # current binaries
#   bash scripts/golden_linux.sh            # compare, print a unified diff
#
# Exit: 0 when every snapshot matches, 1 otherwise.

set -uo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
BIN="$ROOT/build"
GOLD="$ROOT/scripts/golden"

RECORD=0
[ "${1:-}" = "--record" ] && RECORD=1

pass=0
fail=0

# normalize - scrub the few things that legitimately vary between runs: the
# absolute repo path, the throwaway fixture directory, and ISO timestamps.
# Everything else is the contract: wording, ordering, whitespace, which stream a
# line went to, and the exit code.
#
# TMPDIR is set by the caller before the fixtures are created; scrubbing it is
# what keeps the run/panic probes from flapping on a fresh mktemp name.
#
# The last rule scrubs the TOOLCHAIN column of `kirn targets`. That column is a
# live probe: resolveCrossCxx() honours $KIRN_CXX_<TRIPLE> and $KIRN_CXX, then
# falls back to spawning each candidate from PATH and asking for --version. So
# the recorded value is a property of the machine, not of the program, and a
# snapshot of it fails on a runner that has a different cross toolchain
# installed. The TARGET and BINARY columns stay pinned, because those ARE the
# contract. This is a deliberate split: a snapshot records behaviour, and
# "the host toolchain actually works" is an environment assertion -- that one
# belongs in the compile gate (scripts/gates/8_bundle.sh), which really does build.
normalize() {
  sed -e "s#$ROOT#<ROOT>#g" \
      -e "s#${TMPDIR:-/nonexistent}#<TMP>#g" \
      -e 's/[0-9]\{4\}-[0-9]\{2\}-[0-9]\{2\}T[0-9:.+-]*Z\?/<TIMESTAMP>/g'
}

# normalize_targets - additionally scrub the TOOLCHAIN column of `kirn targets`.
# Applied only to that one snapshot: the pattern is anchored to a target triple
# in column 1 with a literal <name> binary in column 2, so it cannot touch a
# source path (an earlier, broader version of this rule matched every
# "*-amd64  ...  " line and broke 198 unrelated snapshots -- which is itself the
# argument for keeping environment scrubbing narrow and explicit).
normalize_targets() {
  normalize | sed -E 's/^((linux|darwin|windows)-(amd64|arm64)[[:space:]]+<name>(\.exe)?[[:space:]]+).*/\1<TOOLCHAIN>/'
}

# snapshot <command...> - run a command and render its entire observable
# behavior: the argv, stdout+stderr interleaved as a user sees them in a
# terminal, and the exit status. Capturing the two streams together is
# deliberate: a refactor that moves a diagnostic from stdout to stderr is a
# behavior change and must be caught.
snapshot() {
  local out rc
  out=$("$@" 2>&1)
  rc=$?
  {
    printf '$ %s\n' "$*"
    printf '%s\n' "$out"
    printf '[exit %s]\n' "$rc"
  } | normalize
}

# same, for `kirn targets`, whose toolchain column is a live environment probe
snapshot_targets() {
  local out rc
  out=$("$@" 2>&1)
  rc=$?
  {
    printf '$ %s\n' "$*"
    printf '%s\n' "$out"
    printf '[exit %s]\n' "$rc"
  } | normalize_targets
}

check() {
  local name="$1"; shift
  local body file
  body=$(snapshot "$@")
  file="$GOLD/$name.txt"
  if [ "$RECORD" = 1 ]; then
    mkdir -p "$GOLD"
    printf '%s\n' "$body" > "$file"
    return
  fi
  if [ ! -f "$file" ]; then
    printf 'MISSING %s (no golden file; run with --record)\n' "$name"
    fail=$((fail + 1))
    return
  fi
  if printf '%s\n' "$body" | diff -u "$file" - > /tmp/golden.diff 2>&1; then
    printf 'PASS    %s\n' "$name"
    pass=$((pass + 1))
  else
    printf 'FAIL    %s\n' "$name"
    sed 's/^/        /' /tmp/golden.diff | head -40
    fail=$((fail + 1))
  fi
}

# check_targets <name> <cmd...> - as check(), but scrubs the environment-dependent
# toolchain column (see normalize_targets). Used for `kirn targets` only, so the
# narrow scrub can never be applied to an unrelated snapshot by accident.
check_targets() {
  local name="$1"; shift
  local body file
  body=$(snapshot_targets "$@")
  file="$GOLD/$name.txt"
  if [ "$RECORD" = 1 ]; then
    mkdir -p "$GOLD"
    printf '%s\n' "$body" > "$file"
    return
  fi
  if [ ! -f "$file" ]; then
    printf 'MISSING %s (no golden file; run with --record)\n' "$name"
    fail=$((fail + 1))
    return
  fi
  if printf '%s\n' "$body" | diff -u "$file" - > /tmp/golden.diff 2>&1; then
    printf 'PASS    %s\n' "$name"
    pass=$((pass + 1))
  else
    printf 'FAIL    %s\n' "$name"
    sed 's/^/        /' /tmp/golden.diff | head -40
    fail=$((fail + 1))
  fi
}

# ---------------------------------------------------------------- preflight
for t in kirn kirnc kirnlex kirnparse kirncheck kirnrun; do
  if [ ! -x "$BIN/$t" ]; then
    echo "missing executable: $BIN/$t" >&2
    echo "build first:  cmake -S . -B build && cmake --build build -j \$(nproc)" >&2
    exit 1
  fi
done

# ------------------------------------------------------------- stage oracles
# Every example and every negative test goes through all three front-end oracles.
# The tool-specific exit codes (0/1/2, plus 70 for a panic) are part of the
# contract, so snapshot() records them rather than this script asserting on them.
for f in "$ROOT"/examples/*.kn "$ROOT"/tests/negative/*.kn; do
  [ -e "$f" ] || continue
  rel=${f#"$ROOT"/}
  rel=${rel//\//_}
  check "lex_${rel}"   "$BIN/kirnlex"   --dump "$f"
  check "parse_${rel}" "$BIN/kirnparse" --ast   "$f"
  check "check_${rel}" "$BIN/kirncheck"         "$f"
done

# ------------------------------------------------------------- preprocessor
# Phase 2. `kirnc -E` is the stop-at-stage dump: preprocess only, print the
# expanded token stream, in the same `KIND\tline:col\ttext` format cmdLex
# already uses. These are the phase's byte-level oracle, and they are recorded
# against kirnc rather than a new binary so the -E and lex dumps stay directly
# comparable.
for f in "$ROOT"/tests/macros/*.kn; do
  [ -e "$f" ] || continue
  rel=${f#"$ROOT"/}
  rel=${rel//\//_}
  check "prepro_${rel}"        "$BIN/kirnc" -E "$f"
  check "prepro_check_${rel}"  "$BIN/kirnc" check "$f"
done

# -D/-U: the defined-table from the command line, both spellings, and the
# left-to-right ordering that lets a later -U undo an earlier -D.
check "prepro_d_define"    "$BIN/kirnc" -E -DFEATURE=1 "$ROOT/tests/macros/cmdline.kn"
check "prepro_d_separated" "$BIN/kirnc" -E -D FEATURE=1 "$ROOT/tests/macros/cmdline.kn"
check "prepro_d_undef"     "$BIN/kirnc" -E -DGONE=1 -UGONE "$ROOT/tests/macros/cmdline.kn"
check "prepro_d_undef_rerun" "$BIN/kirnc" -E -UGONE -DGONE=1 "$ROOT/tests/macros/cmdline.kn"

# --------------------------------------------------------------- kirnrun
# Three distinct exit paths worth pinning: success (program's own output),
# a type error (exit 1, diagnostics), and a panic (exit 70).
#
# The fixture directory is deliberately deterministic (no mktemp suffix) so the
# recorded argv is byte-stable across runs; normalize() scrubs TMPDIR itself.
export TMPDIR=/tmp/kirn-golden.$$
rm -rf "$TMPDIR"
mkdir -p "$TMPDIR"
TMP="$TMPDIR"

# ---------------------------------------------------------- native codegen
# M11. The `--native` scalar backend either lowers a function to C++ or leaves
# it to the interpreter. KIRN_DUMP_NATIVE makes `kirn build --native` print the
# emitted translation unit and stop before the toolchain, so this pins exactly
# which functions lowered, how optionals/elvis were spelled, and -- crucially --
# whether a non-scalar function silently fell back (KIRN_HAS_NATIVE 0).
check_codegen() {
  local name="$1" src="$2"
  local dump="$TMP/codegen_${name}.dump"
  local log="$TMP/codegen_${name}.log"
  local file="$GOLD/codegen_${name}.txt"
  local body rc
  KIRN_DUMP_NATIVE="$dump" "$BIN/kirn" build --native "$src" \
      -o "$TMP/codegen_${name}.out" >"$log" 2>&1
  rc=$?
  body=$({
    printf '$ KIRN_DUMP_NATIVE kirn build --native %s\n' "${src#"$ROOT"/}"
    if [ -f "$dump" ]; then cat "$dump"; else cat "$log"; fi
    printf '[exit %s]\n' "$rc"
  } | normalize)
  if [ "$RECORD" = 1 ]; then
    mkdir -p "$GOLD"
    printf '%s\n' "$body" > "$file"
    return
  fi
  if [ ! -f "$file" ]; then
    printf 'MISSING %s (no golden file; run with --record)\n' "$name"
    fail=$((fail + 1))
    return
  fi
  if printf '%s\n' "$body" | diff -u "$file" - > /tmp/golden.diff 2>&1; then
    printf 'PASS    %s\n' "$name"
    pass=$((pass + 1))
  else
    printf 'FAIL    %s\n' "$name"
    sed 's/^/        /' /tmp/golden.diff | head -40
    fail=$((fail + 1))
  fi
}

for f in "$ROOT"/tests/codegen/*.kn; do
  [ -e "$f" ] || continue
  check_codegen "$(basename "$f" .kn)" "$f"
done

printf 'import greet;\n\nfn main() -> none {\n  print(greet.hello());\n}\n' > "$TMP/main.kn"
printf 'pub fn hello() -> Str {\n  return "SIBLING-GREET";\n}\n'          > "$TMP/greet.kn"
check "kirnrun_sibling_module" "$BIN/kirnrun" "$TMP/main.kn"

printf 'fn main() -> none {\n  print(y + 1);\n}\n' > "$TMP/undef.kn"
check "kirnrun_type_error" "$BIN/kirnrun" "$TMP/undef.kn"

printf 'fn main() -> none {\n  panic("boom");\n}\n' > "$TMP/panic.kn"
check "kirnrun_panic" "$BIN/kirnrun" "$TMP/panic.kn"

# ----------------------------------------------------------------- kirn CLI
# The driver's own surface: usage text, the verb list, and the exit code for a
# missing entry point. The resolver order that Phase 0 unifies shows up here.
check_targets "kirn_targets" "$BIN/kirn" targets
check "kirn_help"    "$BIN/kirn" help
check "kirn_noargs"  "$BIN/kirn"
check "kirn_run_sibling_module" "$BIN/kirn" run "$TMP/main.kn"
check "kirn_run_type_error"     "$BIN/kirn" run "$TMP/undef.kn"
check "kirn_run_no_entry"       "$BIN/kirn" run "$TMP"

# `kirn test` with no args discovers *_test.kn under the CWD, which means it runs
# the real stdlib suite -- and stdlib/pet/io_test.kn writes .io_tmp_test.txt
# into the CWD. Left behind, that file makes the next run's discovery differ, so
# the snapshot would flap. Run it from a scratch directory and clean up, the same
# way the gate scripts do.
rm -f "$ROOT/.io_tmp_test.txt"
GOLD_BODY=$(cd "$ROOT" && snapshot "$BIN/kirn" test)
rm -f "$ROOT/.io_tmp_test.txt"
if [ "$RECORD" = 1 ]; then
  mkdir -p "$GOLD"
  printf '%s\n' "$GOLD_BODY" > "$GOLD/kirn_test_no_args.txt"
else
  if [ ! -f "$GOLD/kirn_test_no_args.txt" ]; then
    printf 'MISSING kirn_test_no_args (run with --record)\n'
    fail=$((fail + 1))
  elif printf '%s\n' "$GOLD_BODY" | diff -u "$GOLD/kirn_test_no_args.txt" - \
         > /tmp/golden.diff 2>&1; then
    printf 'PASS    kirn_test_no_args\n'
    pass=$((pass + 1))
  else
    printf 'FAIL    kirn_test_no_args\n'
    sed 's/^/        /' /tmp/golden.diff | head -40
    fail=$((fail + 1))
  fi
fi

echo
if [ "$RECORD" = 1 ]; then
  echo "GOLDEN RECORDED: $GOLD ($(find "$GOLD" -name '*.txt' | wc -l) files)"
  exit 0
fi
echo "GOLDEN RESULT: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
