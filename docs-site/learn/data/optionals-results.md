---
title: Optionals and results
description: Represent missing values and recoverable errors in Kirn.
---

# Optionals and results

Optional types use `?` to express that a value may be absent:

```kirn
fn find_name(id: Int) -> Str? {
    if id == 1 { "Ada" } else { none }
}
```

The nil-safe access operator `.?.` helps inspect an optional value without pretending it is always present.

Results model success and failure explicitly. The `try` form propagates a failure to the caller:

```kirn
fn load_config(path: Str) -> Result<Config, Error> {
    text = try read_file(path);
    parse_config(text)
}
```

The exact result and optional APIs are evolving; use the compiler and standard-library reference as the current authority.

