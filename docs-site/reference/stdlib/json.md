---
title: json
description: Encode and decode JSON values in Kirn.
---

# `json`

The JSON module provides `dumps` and `loads` for converting between Kirn values and JSON text.

```kirn
payload = {"name": "Kirn", "active": true};
text = dumps(payload);
value = loads(text);
```

Use the source and tests for supported value shapes and error cases.

