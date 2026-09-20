# Kirn Grammar (`grammar/`)

The normative, machine-checkable grammar of the Kirn language.

| File | Description |
|------|-------------|
| `grammar/coco.ebnf` | The authoritative EBNF grammar (ISO EBNF: `=` definition, `;` terminator, `[ ]` optional, `{ }` repetition, `(* *)` comments) |

## Usage

`grammar/coco.ebnf` is the single source of truth for the language syntax. The
C++ parser in [`src/parser/`](../src/parser/) implements it, and the example
corpus ([`examples/*.co`](../examples/)) is the **ground truth** the grammar must
accept.

**Rule:** any language change must update the grammar first, then be validated
against the entire example corpus before merge.

See [`docs/COCO_PLAN.md`](../docs/COCO_PLAN.md) for the grammar phase history
and change record.
