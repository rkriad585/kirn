---
title: Concurrency
description: Explore Kirn's spawn, channel, select, and join primitives.
---

# Concurrency

Kirn includes language-level building blocks for concurrent programs. The verified example corpus covers green-thread-style `spawn`, channels, `select`, and `join`.

```kirn
channel = chan<Int>();

spawn {
    channel <- 42;
}

value = <-channel;
print(value);
```

Concurrency behavior and runtime guarantees are still developing. Start with `examples/22_spawn_channels_join.kn` and `examples/23_select_multiplex.kn`, then test changes against the current toolchain.

