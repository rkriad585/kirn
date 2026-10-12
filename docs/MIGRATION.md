# Migrating from Kirn to Kirn

> **Kirn was formerly named Kirn.** The rename is a hard cut: every old name below
> is gone, and there is no compatibility layer except the two deprecation shims
> noted at the bottom.

## Renamed surfaces

| Surface                     | Kirn (old)                                  | Kirn (new)                                  |
| --------------------------- | ------------------------------------------- | ------------------------------------------- |
| Language                    | Kirn                                        | Kirn                                        |
| Source extension            | `.kn`                                       | `.kn`                                       |
| CLI driver                  | `kirn`                                      | `kirn`                                      |
| Companion tools             | `kirnrun` `kirncheck` `kirnlex` `kirnparse` | `kirnrun` `kirncheck` `kirnlex` `kirnparse` |
| Project manifest / lockfile | `kirn.toml` / `kirn.lock`                   | `kirn.toml` / `kirn.lock`                   |
| Library unit                | `lib` / `libs/`                             | `pet` / `pets/`                             |
| Import statement            | `import lib.foo`                            | `import pet.foo`                            |
| Packaged library bundle     | `.kirnlib`                                  | `.pet`                                      |
| Environment variables       | `KIRN_*` (e.g. `KIRN_ASAN`, `KIRN_TARGET`)  | `KIRN_*` (e.g. `KIRN_ASAN`, `KIRN_TARGET`)  |
| Generated-program macros    | `KIRN_APP_*` / `KIRN_HAS_NATIVE`            | `KIRN_APP_*` / `KIRN_HAS_NATIVE`            |
| Native runtime library      | `libkirn_rt`                                | `libkirn_rt`                                |
| Global user data directory  | `~/.kirn/kirn-pkg`                          | `~/.kirn/pets`                              |
| Registry                    | `github.com/kirn-lib/kirn-libs`             | `github.com/pets-registry/pets`             |
| Website                     | `kirn-lib.github.io`                        | `kirn-lang.github.io`                       |
| Repository                  | `github.com/rkriad585/kirn`                 | `github.com/rkriad585/kirn`                 |

## What to do in an existing project

1. **Rename source files** from `*.kn` to `*.kn` (including `mod.kn`, `pin.kn`,
   `__init__.kn`, and `*_test.kn`).
2. **Rewrite imports**: `import lib.foo` → `import pet.foo`, and move any local
   `libs/` directory to `pets/`.
3. **Rename the manifest**: `kirn.toml` → `kirn.toml`, and `kirn.lock` →
   `kirn.lock`. The compiler detects a stale manifest and tells you to `mv` it.
4. **Update environment variables** in your build scripts: any `KIRN_*` you set
   is now ignored. The compiler prints a warning listing the new names.
5. **Update tooling invocations** in CI, Makefiles, and scripts: `kirn` → `kirn`
   and the four companion tools.
6. **Rebuild from scratch.** A CMake build tree is not relocatable, so delete
   `build/` and re-run `cmake -S . -B build` rather than reusing it.

## Deliberately unchanged

- **`KIRN_PLANS/`** holds the pre-rename design plans as a historical archive and
  keeps its directory name; see [REBRAND_PLAN.md](../KIRN_PLANS/REBRAND_PLAN.md)
  for the full migration record.
- **The `Kirn` name survives in exactly one visible place** — the "formerly
  known as" note in the [README](../README.md) — so that searches for the old
  name still land on the right project.

## Removed outright

- **`.cob` bundles and the `KIRNB` magic** were a dead-record format. The old
  portable `.cob` cross-build fallback has been removed: cross-compiling now
  requires a real toolchain for the target rather than silently falling back.

## Deprecation shims that still read old names

Two paths are still read purely so an existing project gets a clear message
instead of a confusing failure:

| Shim                                  | Behavior                                                               |
| ------------------------------------- | ---------------------------------------------------------------------- |
| A stray `kirn.toml` in a project root | `kirn` reports it and tells you to `mv kirn.toml kirn.toml`            |
| `-DKIRN_ASAN=ON` passed to CMake      | Migrated to `KIRN_ASAN` once, then CMake emits a `DEPRECATION` message |

Both are covered by the gate suite, so they cannot silently rot.
