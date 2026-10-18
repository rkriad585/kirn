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
#   6. test        - `kirn test .` runs the stdlib/pet *_test.kn suite (12
#                    tests, must report '12 passed, 0 failed').
#   7. pets        - .pet bundle round-trip: pack a lib, install the bundle
#                    into a consuming project's pets/ dir, consume it at run.
# Exit 0 iff every gate passes, else 1.

# Thin driver: shared setup is gates/_common.sh; each gate is one file in
# gates/. With no args every gate runs (identical to the old single-file
# harness); pass gate keys (e.g. `gates_linux.sh 6 2b`) to run a subset.
# Gates are sourced, not executed, so pass/fail counters and $TMP stay shared.
GD="$(cd "$(dirname "${BASH_SOURCE[0]}")/gates" && pwd)"
source "$GD/_common.sh"

if want "1"; then source "$GD/1_runall.sh"; fi
if want "2"; then source "$GD/2_types.sh"; fi
if want "2b"; then source "$GD/2b_syntax.sh"; fi
if want "3"; then source "$GD/3_negative.sh"; fi
if want "4"; then source "$GD/4_vm_diff.sh"; fi
if want "5"; then source "$GD/5_conventions.sh"; fi
if want "6"; then source "$GD/6_test.sh"; fi
if want "7"; then source "$GD/7_pets.sh"; fi
if want "8"; then source "$GD/8_bundle.sh"; fi
if want "8b"; then source "$GD/8b_HIR.sh"; fi
if want "9"; then source "$GD/9_MIR.sh"; fi
if want "10"; then source "$GD/10_LLVM.sh"; fi
if want "11"; then source "$GD/11_native.sh"; fi

echo "GATE RESULT: $pass passed, $fail failed, $hung hung"
[ "$fail" -eq 0 ] || exit 1
exit 0
