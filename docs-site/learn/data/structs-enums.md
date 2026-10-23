---
title: Structs and enums
description: Model structured data with structs and enums.
---

# Structs and enums

Use a `struct` to group named fields and behavior:

```kirn
struct User {
    name: Str;
    active: Bool;
}

fn main() {
    user = User(name: "Riad", active: true);
    print(user.name);
}
```

Enums represent a closed set of variants and work naturally with `match`:

```kirn
enum Status {
    Ready,
    Done,
}

fn label(status: Status) -> Str {
    match status {
        case Status.Ready => "ready";
        case Status.Done => "done";
    }
}
```

See [syntax and operators](/reference/language/syntax) for pattern details.

