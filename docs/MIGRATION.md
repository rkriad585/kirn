# Migrating from Coco to Kirn

> **Kirn was formerly named Coco.** The rename is a hard cut: every old name below
> is gone, and there is no compatibility layer except the two deprecation shims
> noted at the bottom.

## Renamed surfaces

| Surface                     | Coco (old)                                  | Kirn (new)                                  |
| --------------------------- | ------------------------------------------- | ------------------------------------------- |
| Language                    | Coco                                        | Kirn                                        |
| Source extension            | `.co`                                       | `.kn`                                       |
| CLI driver                  | `coco`                                      | `kirn`                                      |
| Companion tools             | `cocorun` `cococheck` `cocolex` `cocoparse` | `kirnrun` `kirncheck` `kirnlex` `kirnparse` |
| Project manifest / lockfile | `coco.toml` / `coco.lock`                   | `kirn.toml` / `kirn.lock`                   |
| Library unit                | `lib` / `libs/`                             | `pet` / `pets/`                             |
| Import statement            | `import lib.foo`                            | `import pet.foo`                            |
| Packaged library bundle     | `.cocolib`                                  | `.pet`                                      |
| Environment variables       | `COCO_*` (e.g. `COCO_ASAN`, `COCO_TARGET`)  | `KIRN_*` (e.g. `KIRN_ASAN`, `KIRN_TARGET`)  |
| Generated-program macros    | `COCO_APP_*` / `COCO_HAS_NATIVE`            | `KIRN_APP_*` / `KIRN_HAS_NATIVE`            |
| Native runtime library      | `libcoco_rt`                                | `libkirn_rt`                                |
| Global user data directory  | `~/.coco/coco-pkg`                          | `~/.kirn/pets`                              |
| Registry                    | `github.com/coco-lib/coco-libs`             | `github.com/pets-registry/pets`             |
| Website                     | `coco-lib.github.io`                        | `kirn-lang.github.io`                       |
| Repository                  | `github.com/rkriad585/coco`                 | `github.com/rkriad585/kirn`                 |

## What to do in an existing project

1. **Rename source files** from `*.co` to `*.kn` (including `mod.kn`, `pin.kn`,
   `__init__.kn`, and `*_test.kn`).
2. **Rewrite imports**: `import lib.foo` → `import pet.foo`, and move any local
   `libs/` directory to `pets/`.
3. **Rename the manifest**: `coco.toml` → `kirn.toml`, and `coco.lock` →
   `kirn.lock`. The compiler detects a stale manifest and tells you to `mv` it.
4. **Update environment variables** in your build scripts: any `COCO_*` you set
   is now ignored. The compiler prints a warning listing the new names.
5. **Update tooling invocations** in CI, Makefiles, and scripts: `coco` → `kirn`
   and the four companion tools.
6. **Rebuild from scratch.** A CMake build tree is not relocatable, so delete
   `build/` and re-run `cmake -S . -B build` rather than reusing it.

## Deliberately unchanged

- **`COCO_PLANS/`** holds the pre-rename design plans as a historical archive and
  keeps its directory name; see [COCO_TO_KIRN_PLAN.md](../COCO_PLANS/COCO_TO_KIRN_PLAN.md)
  for the full migration record.
- **The `Coco` name survives in exactly one visible place** — the "formerly
  known as" note in the [README](../README.md) — so that searches for the old
  name still land on the right project.

## Removed outright

- **`.cob` bundles and the `COCOB` magic** were a dead-record format. The old
  portable `.cob` cross-build fallback has been removed: cross-compiling now
  requires a real toolchain for the target rather than silently falling back.

## Deprecation shims that still read old names

Two paths are still read purely so an existing project gets a clear message
instead of a confusing failure:

| Shim                                  | Behavior                                                               |
| ------------------------------------- | ---------------------------------------------------------------------- |
| A stray `coco.toml` in a project root | `kirn` reports it and tells you to `mv coco.toml kirn.toml`            |
| `-DCOCO_ASAN=ON` passed to CMake      | Migrated to `KIRN_ASAN` once, then CMake emits a `DEPRECATION` message |

Both are covered by the gate suite, so they cannot silently rot.
