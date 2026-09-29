# Coco → Kirn rename: before/after report

Generated at the close of the rename migration. This is the capstone
measurement for the project: what the old name looked like before the work, what
it looks like now, and exactly which references to the old brand survive and why.

## Method

- **Before** is the frozen Phase-1 census at
  `_rename/baseline/08-dryrun-inventory.txt` (commit `c988c0d`, 2026-09-20). It
  counted occurrences of the three case-forms of the brand as whole words, over
  79 files.
- **After** is the same measurement re-run over git-tracked files today
  (`git grep -I -o -w`, one pass per case-form), so the two are directly
  comparable: both count occurrences, not matching lines.
- Two exclusions are applied to the "after" column, because neither is a
  regression:
  - `COCO_PLANS/` — the pre-rename design-plan archive, kept verbatim on purpose.
  - `_rename/baseline/` — the Phase-1 transcripts, which *are* the "before" half
    of this report. Rewriting them would destroy the measurement.

## Headline

| Metric                             | Before | After (all tracked) | After (excl. archives) |
| ---------------------------------- | -----: | ------------------: | ---------------------: |
| Occurrences, all three case-forms  |  2,370 |               1,719 |                     105 |
| Files containing at least one      |     79 |                  40 |                     22 |

**Live occurrences fell by 95.6% (2,370 → 105).** Every one of the 105 that
remain outside the frozen archives is intentional and individually justified in
`scripts/rename_allowlist.txt`.

Comparing 2,370 against the un-excluded 1,719 understates the work: 1,614 of
those are inside the two archives that were never meant to change. The
excluded-archives column is the honest one.

## Per case-form

| Case-form | Before | After (all tracked) | After (excl. archives) |
| --------- | -----: | ------------------: | ---------------------: |
| `Coco` (prose)          |   434 |                 423 |                      20 |
| `coco` (identifiers)    | 1,649 |               1,171 |                      81 |
| `COCO` (env / build)    |   287 |                 125 |                       4 |

The uppercase form is the cleanest signal, because the environment and CMake
surface had no legitimate reason to keep the old prefix. It fell furthest.

## What the remaining 105 references are

Every surviving reference falls into one of five buckets. The first two are the
bulk and are permanent by design.

| Bucket                                | Refs | Why it remains |
| ------------------------------------- | ---: | -------------- |
| `COCO_PLANS/` archive (incl. path)    |    — | The pre-rename design record, kept verbatim. |
| `_rename/baseline/` transcripts        |    — | The Phase-1 "before" evidence this report is built on. |
| Compatibility shims (code)            |    — | User-facing migration help. See below. |
| "Formerly known as" discoverability    |    — | One visible mention per surface, on purpose. |
| Inert test data                        |    — | Arbitrary string values that are not brand claims. |

The compatibility shims are the only ones that are *code*, and they are
load-bearing: the old name is the payload of the message.

- A stale `coco.toml` is detected and the tool prints `mv coco.toml kirn.toml`.
  Delete the old name from that string and the helpful message disappears.
- A stale `coco.lock` is likewise detected and the tool prints the `rm` command.
- `COCO_*` environment variables are detected and reported as renamed, so a
  stale `COCO_ASAN` in someone's build command gets a message instead of silence.
- `scripts/gates_linux.sh` creates a stale pre-rename manifest as a *fixture* and
  asserts the rename hint fires. That test is the shim's regression guard;
  renaming the fixture would silently delete the test.

The "formerly known as" mentions exist for a concrete reason: someone who
searches the old name should be able to land on this project. They are listed in
`docs/MIGRATION.md`, which maps every renamed surface in one table.

## What changed, by surface

| Surface                     | Before        | After                          |
| --------------------------- | ------------- | ------------------------------ |
| Source extension            | `.co`         | `.kn` (0 `.co` files tracked)   |
| CLI driver                  | `coco`        | `kirn`                         |
| Companion tools             | `cocorun` `cococheck` `cocolex` `cocoparse` | `kirnrun` `kirncheck` `kirnlex` `kirnparse` |
| Manifest / lockfile         | `coco.toml` `coco.lock` | `kirn.toml` `kirn.lock` |
| Library unit                | `lib` / `libs/` | `pet` / `pets/`               |
| Environment / build flags   | `COCO_*`      | `KIRN_*`                       |
| Native runtime              | `libcoco_rt`  | `libkirn_rt`                   |
| Grammar file                | `grammar/coco.ebnf` | `grammar/kirn.ebnf`      |
| Master design doc           | `docs/COCO_PLAN.md` | `docs/KIRN_PLAN.md`      |
| Registry                    | `coco-lib/coco-libs` | `pets-registry/pets`     |
| Repository / website        | `rkriad585/coco`, `coco-lib.github.io` | `rkriad585/kirn`, `kirn-lang.github.io` |

The `.cob` bundle extension and its `COCOB` magic were not renamed; they were
removed outright, along with the old portable cross-build fallback they served.

## Enforcing it

The measurement above is a snapshot, and a snapshot decays. The rename is now
guarded by `scripts/rename_verify.ps1`, wired into CI as the
`rename-regression` job, which runs on both `ubuntu-latest` and
`windows-latest` on every push and pull request.

The gate fails when a pre-rename token appears in a tracked file's contents *or
its path*, unless the file is exempted in `scripts/rename_allowlist.txt`. Two
properties keep that allowlist honest:

- Every entry must carry a reason, so an exemption is never granted silently.
- An entry that no longer matches any violation is itself reported as a
  failure, so the list cannot rot into a blanket suppression. If a exempted file
  is deleted or cleaned up, the gate tells you to remove its entry.

The gate also caught real drift during the rollout: several README files still
documented the old source extension after the extension had already been
renamed, and the CI workflow contained a duplicated block that made one job
unparseable. Both were fixed rather than allowlisted.
