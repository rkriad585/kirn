---
title: Installation
description: Build and run the Kirn toolchain from source.
---

# Installation

Kirn is currently developed from source. The repository is the authoritative place for supported build instructions and platform changes.

## Get the source

```bash
git clone https://github.com/rkriad585/kirn.git
cd kirn
```

Kirn uses CMake and a C++20 toolchain. Build instructions may vary by platform; check the repository README and CI workflow for the current dependency list.

## Verify the checkout

After building, confirm that the driver is available:

```bash
kirn
kirn run examples/01_hello.kn
```

The example corpus is intentionally useful: it gives you small programs to run while learning the language and a concrete way to check whether a feature is implemented.

## Next step

Continue with the [Quick start](/learn/quick-start), or read the [CLI guide](/guide/cli) for the full workflow.

