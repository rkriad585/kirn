# CI and Release Action Errors

Captured from the latest remote runs for commit `0a4bbe9bfe88e8089e50b9235834c63e9c6edd0d`.
This file records only failed actions and the deleted Termux target.

## CI

Run: [CI #41385867798](https://github.com/rkriad585/kirn/actions/runs/41385867798)

### `corpus` — `Corpus gates`

```text
FAIL  69_parallel_sort.kn
      HANG (watchdog 20 s) stderr:
GATE RESULT: 89 passed, 1 failed, 0 hung, 333.33ms par file, 0m:30s took
##[error]Process completed with exit code 1.
```

### `build-test` — `Build`

```text
src/interp/runtime.cpp(1,1): error C1128: number of sections exceeded object file format limit: compile with /bigobj
##[error]Process completed with exit code 1.
```

## Release

Run: [release #41385881934](https://github.com/rkriad585/kirn/actions/runs/41385881934)

### `windows-amd64` — `Smoke test`

```text
smoke testing ./dist/kirn-windows-amd64/bin/kirn.exe
The system cannot find the path specified.
##[error]Process completed with exit code 1.
```

### `windows-arm64` — `Configure and build`

```text
CMake Error at CMakeLists.txt:2 (project):
  Generator
    Visual Studio 17 2022
  could not find any instance of Visual Studio.
##[error]Process completed with exit code 1.
```

### Deleted target: `termux-i686` — `Build Kirn and kirnc in Termux Docker`

This failed target has been removed from the release matrix and will not run in
future release workflows.

```text
E: Failed to fetch https://mirrors.cernet.edu.cn/termux/apt/termux-main/dists/stable/main/binary-i686/Packages
  File has unexpected size (570315 != 567403). Mirror sync in progress?
E: Some index files failed to download. They have been ignored, or old ones used instead.
##[error]Process completed with exit code 100.
```
