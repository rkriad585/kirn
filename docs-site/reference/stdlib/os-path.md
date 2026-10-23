---
title: os and path
description: Work with process arguments, environment, and filesystem paths.
---

# `os` and `path`

The `os` module exposes process-oriented helpers such as arguments, environment lookup, and exit behavior. The `path` module provides path operations.

```kirn
args = args();
home = getenv("HOME");
```

Platform-specific behavior should be verified on the target you intend to ship.

