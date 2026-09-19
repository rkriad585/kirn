# Coco Plans (`COCO_PLANS/`)

Phased implementation plans for the Coco language and tooling. Each plan opens
with a research/source-review note before offering step-by-step phases with
goals, problems solved, files affected, code, testing, and limitations.

| Plan | Topic |
|------|-------|
| `PLAN.md` / `DO_FIRST_PLAN.md` | Overall roadmap and immediate-first priorities |
| `COCO_TO_KIRN_PLAN.md` | Coco → Kirn rebrand/rename (`.co`→`.kn`, `lib`→`pet`) |
| `COCO_DOCS_PLAN.md` | Documentation website |
| `SYNTAX_PLAN.md` / `EXP_PLAN.md` / `DATA_TYPE_PLAN.md` | Syntax, expressions, data types |
| `MISSING_PLAN.md` / `NEED_REMOVE_PLAN.md` | Missing / to-be-removed features |
| `WHY_PLAN.md` / `WHY_USE_COCO_PLAN.md` | Rationale and adoption messaging |
| `STD_LIBS_PLAN.md` | Standard library |
| `COCO_HIGHLIGHT_PLAN.md` | Syntax highlighting: TextMate + tree-sitter |
| `COCO_LSP_PLAN.md` | Language Server Protocol server (`coco-lsp`) |
| `COCO_CROSS_PLAN.md` | Cross-compilation / targets |

These plans are living documents; keep them in sync with the code and the
normative grammar ([`grammar/coco.ebnf`](../grammar/coco.ebnf)) and the example
corpus ([`examples/`](../examples/)).
