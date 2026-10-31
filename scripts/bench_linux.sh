#!/usr/bin/env bash
# Reproducible build/run benchmark for the Builder Plan §7 requirement.
#
# Usage: scripts/bench_linux.sh [--runs N] [--json PATH]
#
# Compilation and execution are reported separately: Kirn's default AOT path
# compiles runtime support into the artifact, while Go and Rust use their own
# toolchain/runtime model. This is a measurement harness, not a performance
# claim; compare runs on a quiet, fixed machine.

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
KIRN="${KIRN_BIN:-$ROOT/build/kirn}"
RUSTC="${RUSTC_BIN:-rustc}"
GOCMD="${GO_BIN:-go}"
RUNS=3
JSON_OUT=""

usage() {
    cat <<'EOF'
usage: scripts/bench_linux.sh [--runs N] [--json PATH]

Environment overrides: KIRN_BIN, RUSTC_BIN, GO_BIN
EOF
}

while (($#)); do
    case "$1" in
        --runs)
            (($# >= 2)) || { echo "--runs needs a value" >&2; exit 64; }
            RUNS="$2"; shift 2
            ;;
        --json)
            (($# >= 2)) || { echo "--json needs a path" >&2; exit 64; }
            JSON_OUT="$2"; shift 2
            ;;
        -h|--help) usage; exit 0 ;;
        *) echo "unknown option: $1" >&2; usage >&2; exit 64 ;;
    esac
done

[[ "$RUNS" =~ ^[1-9][0-9]*$ ]] || { echo "--runs must be a positive integer" >&2; exit 64; }
[[ -x "$KIRN" ]] || { echo "Kirn binary not found: $KIRN" >&2; exit 2; }
command -v "$RUSTC" >/dev/null || { echo "rustc not found: $RUSTC" >&2; exit 2; }
command -v "$GOCMD" >/dev/null || { echo "go not found: $GOCMD" >&2; exit 2; }

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
mkdir -p "$TMP/src" "$TMP/bin" "$TMP/times"
mkdir -p "$TMP/go-cache"

cat >"$TMP/src/fib.kn" <<'EOF'
fn fib(n: Int) -> Int {
    if n < 2 { return n; }
    return fib(n - 1) + fib(n - 2);
}
fn main() -> none { print(fib(30)); }
EOF
cat >"$TMP/src/fib.rs" <<'EOF'
fn fib(n: i64) -> i64 {
    if n < 2 { return n; }
    fib(n - 1) + fib(n - 2)
}
fn main() { println!("{}", fib(30)); }
EOF
cat >"$TMP/src/fib.go" <<'EOF'
package main
import "fmt"
func fib(n int64) int64 {
    if n < 2 { return n }
    return fib(n-1) + fib(n-2)
}
func main() { fmt.Println(fib(30)) }
EOF

if [[ -n "$JSON_OUT" ]]; then : >"$JSON_OUT"; fi

median() {
    sort -n "$1" | awk -v n="$RUNS" 'NR == (n + 1) / 2 { print; exit }'
}

run_timed() {
    local output="$1"; shift
    local start end
    start="$(date +%s%N)"
    "$@" >/dev/null
    end="$(date +%s%N)"
    awk -v elapsed="$((end - start))" 'BEGIN { printf "%.6f\n", elapsed / 1000000000 }' >"$output"
}

record() {
    local lang="$1" phase="$2" values="$3" median_s="$4"
    printf '%-6s %-5s median=%8ss samples=%s\n' "$lang" "$phase" "$median_s" "$values"
    if [[ -n "$JSON_OUT" ]]; then
        printf '{"language":"%s","phase":"%s","median_seconds":%s,"samples":[%s]}\n' \
            "$lang" "$phase" "$median_s" "$values" >>"$JSON_OUT"
    fi
}

build_one() {
    local lang="$1"; shift
    local samples="$TMP/times/${lang}.build"
    : >"$samples"
    for ((i = 0; i < RUNS; ++i)); do
        run_timed "$TMP/time" "$@"
        tr -d '\n' <"$TMP/time" >>"$samples"
        printf '\n' >>"$samples"
    done
    local med; med="$(median "$samples")"
    record "$lang" build "$(paste -sd, "$samples")" "$med"
}

run_one() {
    local lang="$1" exe="$2"
    local samples="$TMP/times/${lang}.run"
    : >"$samples"
    local expected='832040'
    for ((i = 0; i < RUNS; ++i)); do
        "$exe" >"$TMP/out"
        [[ "$(tr -d '\r\n' <"$TMP/out")" == "$expected" ]] || {
            echo "$lang benchmark produced unexpected output" >&2; exit 1;
        }
        run_timed "$TMP/time" "$exe"
        tr -d '\n' <"$TMP/time" >>"$samples"
        printf '\n' >>"$samples"
    done
    local med; med="$(median "$samples")"
    record "$lang" run "$(paste -sd, "$samples")" "$med"
}

echo 'language phase median_seconds samples'
build_one kirn "$KIRN" build "$TMP/src/fib.kn" -o "$TMP/bin/kirn"
build_one rust "$RUSTC" -O "$TMP/src/fib.rs" -o "$TMP/bin/rust"
build_one go env GO111MODULE=off GOTOOLCHAIN=local GOCACHE="$TMP/go-cache" "$GOCMD" build \
    -trimpath -o "$TMP/bin/go" "$TMP/src/fib.go"

run_one kirn "$TMP/bin/kirn"
run_one rust "$TMP/bin/rust"
run_one go "$TMP/bin/go"

echo "benchmark artifacts: $TMP (removed on exit)"
