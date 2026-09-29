# Kirn Compiler Source (`src/`)

This is the C++20 implementation of the Kirn compiler and runtime. The pipeline
is a classic **lexer → parser → semantic analysis → VM/interpreter → native
backend**, with each stage in its own subdirectory and linked into the CLI
tools in [`tools/`](../tools/).

## Pipeline layout

| Directory | Stage | Key files |
|-----------|-------|-----------|
| `src/lex/` | Lexer | `lexer.{h,cpp}`, `token.h` — token kinds, keyword/operator tables, all string forms (normal/raw/byte/C/f-string) |
| `src/ast/` | AST | `ast.h`, `ast.cpp`, `ast_dump.cpp` — node kinds, `Span{line,col,endLine,endCol}` extents, AST printer |
| `src/parser/` | Parser | `parser.{h,cpp}` — recursive descent over `grammar/kirn.ebnf`, `syncToStatementEnd` error recovery |
| `src/sema/` | Semantic analysis | `checker.*` (two-pass type check, `typeOf`), `borrow.*` (borrow checker), `symbols.h` (`Symbol`, `Scope`, `FuncSig`), `type.h` (`Ty`/`TyK`) |
| `src/interp/` | Interpreter | `runtime.{h,cpp}`, `value.h` — tree-walking interpreter, `loadModuleFile` module resolution |
| `src/vm/` | Bytecode VM | `compiler.*`, `bytecode.h` — bytecode compilation |
| `src/backend/` | Native backend | `native.{h,cpp}` — AOT/native code generation (in progress) |
| `src/support/` | Diagnostics | `diag.h` — `Diag`/`SpanRange`/`Sev`/`FixIt`/`Note` |

## Utilities

- `src/util/` — small shared helpers (`tomlmini.h`, escape tables).

## Conventions

- New files must fit the existing include/style habits (headers + `.cpp`
  pairs, `kirn::` namespaces matching directory names).
- Every change to parsing/semantics should be validated against the example
  corpus (`examples/*.co`) and the negative test suite (`tests/negative/*.co`)
  — see [`../scripts/`](../scripts/) harnesses.
