# Coco "What did we do so far?" - shared understanding of the project's health &

# safety netting tasks, kept up to date as the work progresses.

#

# Things the repo is currently set up for (Phase-1 baseline snapshot done):

# - LF line endings EVERYWHERE (see .gitattributes "* text=auto eol=lf").

# core.autocrlf is DISABLED for this repo (`git config core.autocrlf false`)

# so git never auto-converts and never warns about LF/CRLF replacement.

# - 5-gate verification harness: scripts/runall.ps1, scripts/types.ps1

# (-Check/-Run), scripts/negative.ps1 (-Runner), scripts/vm_diff.ps1,

# tests/conventions/run.ps1. Phase-1 snapshot captured in scripts/

# rename_baseline.ps1 (transcripts under _rename/baseline/).

# - Node-based dev tooling (optional, husky v9 + lint-staged + commitlint +

# prettier): activate with `npm install`. Hooks live in .husky/ and NOP

# out when npx isn't available, so the C++ tree stays buildable with zero

# JS dependency.

#

# Active work streams (check these before starting anything):

# COCO_PLANS/COCO_TO_KIRN_PLAN.md - the Coco->Kirn migration plan (Phases

# 1..13 + decision log D1..D8). Phase 1

# (baseline snapshot) is complete.

# docs/COCO_PLAN.md / docs/README.md / docs/FEATURE_GAP_ANALYSIS.md

#

# Line-ending rules of thumb (kept in sync between .gitattributes,

# .editorconfig, .prettierrc/.prettierignore, .husky/):

# * Every text file: LF, UTF-8, final newline, no trailing whitespace.

# * NEVER introduce CRLF; git never re-adds it on checkout now that

# autocrlf is off.

# * New files created by any tool (Write/Edit) already come out LF.
