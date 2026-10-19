#!/usr/bin/env bash
# Examples-only gate: run every examples/*.kn through kirnrun with the
# per-file watchdog, honoring each file's `# expect-exit: N` annotation.
#
# This is the fast, focused check for the examples/ corpus - the same script
# as gates/1_runall.sh, exposed here next to golden_linux.sh /
# crosscheck_linux.sh so "just check the examples" needs no knowledge of the
# gates directory. For the other checks run their own scripts; each file under
# scripts/gates/ stands alone, e.g.
#     bash scripts/gates/2_types.sh
#     bash scripts/gates/6_test.sh
exec "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/gates/1_runall.sh" "$@"
