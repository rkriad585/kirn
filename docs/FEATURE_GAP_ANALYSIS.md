# Kirn — Feature Gap Analysis vs Go & Rust

**Status:** Living document · last updated with `kirn build` single-file mode, brace-syntax
migration (v1), pattern aliases, or-patterns, labeled loops, the CapCase type surface cut,
`sizeof`/`alignof` builtins, broader `@comptime`/`const` evaluation, coded `W90xx`
driver/tool warnings, and Rust-style binding modifiers (`mut`/`immut`/`signed`/
`unsigned`).

This document compares Kirn against the reference language implementations in
`C:\Users\rkriad585\Projects\go-rust-source-code` (`go-master`, `rust-main`) and records
what has been adopted, what is deliberately deferred, and a prioritized roadmap.

---

## 1. Methodology

Both reference trees were surveyed at the compiler level:

|              | Go (`go-master`)                                               | Rust (`rust-main`)                    |
| ------------ | -------------------------------------------------------------- | ------------------------------------- |
| Lexer        | `src/cmd/compile/internal/syntax/scanner.go`, `src/go/scanner` | `compiler/rustc_lexer/src/lib.rs`     |
| Parser       | `src/go/parser/parser.go`                                      | `compiler/rustc_parse/src/parser/*`   |
| AST          | `src/cmd/compile/internal/syntax/nodes.go`                     | `compiler/rustc_ast/src/ast.rs`       |
| Types        | `src/go/types/*` (113 files)                                   | `compiler/rustc_hir_typeck/src/*`     |
| Bounds/moves | escape analysis `cmd/compile/internal/escape`                  | `compiler/rustc_borrowck/src/*` (NLL) |
| Backend      | SSA `cmd/compile/internal/ssa`                                 | LLVM/Cranelift/GCC                    |

Kirn's pipeline is a single-pass recursive-descent parser + AST + type checker
(`src/parser`, `src/sema`) feeding a tree-walking interpreter (`src/interp`) that is
also compiled ahead-of-time to a self-contained native binary (GNU toolchain,
`kirn build file.kn`).

---

## 2. What Kirn Already Has (mirrors Go/Rust)

### Syntax / structure

- `{ }` blocks, `;` statement terminators — C/Go/Rust style, layout-free.
- `fn f(a: int, b: int) -> int { ... }` — typed function declarations.
  - diverging from Rust's `fn`, Python's `fn`; keyword `fn` retained for
    familiarity; `fn` is accepted as an alias for `fn` (resolved).
- `struct`/`enum`/`trait`/`impl` — full nominal type system (Rust-like).
- Type parameters `[T is Bound]` on functions/structs/enums/traits.
- `match` with guards, exhaustiveness-aware arm checking.
- `import`/`from ... import` package system + `kirn install` registry/ecosystem.
- Visibility `pub`; modules (`code/`, `text/slug`, `json`, `math`, `time`, ...).
- Ownership-lite: `&` borrow, `*` deref, `weak` references, value semantics.
- Concurrency: `spawn`, `chan`, `select` (Go's goroutine/channel/select trio).
- Safety valve: `unsafe` blocks, `extern fn` FFI, `new`, `box`.
- `defer` (Go), `raise`/`?` (Rust error propagation), `panic`/recover analogue
  via `raise`/`catch`.
- `var` mutable vs immutable bindings; `const`; Rust-style `:` type annotations.
- **NEW (this pass):** binding modifiers `mut` (mutable), `immut` (read-only),
  `signed` (→`Int`/i64) and `unsigned` (→`UInt`/u64); `var` remains an alias for
  `mut`. Bindings default to immutable (reassignment is a compile error), and a
  negative literal assigned to an `unsigned` binding is rejected statically.
- f-strings with printf-style format specs (`{x:>10}`, `{x:.2f}`).
- Ranges `..` / `..=`; slices `a[1:3]`; tuple deconstruction; comprehensions.
- **NEW (this pass):** pattern aliases `name @ sub-pattern`, or-patterns `p1 | p2`,
  labeled loops + `break label:` / `continue label:`.

### Stdlib modules available to import

`math`, `time`, `io`, `mem`, `json`, `os`, `text` (e.g. `text.slug.slugify`).

---

## 3. Feature-by-Feature Gap Analysis

### 3.1 Expression & statement level

| Feature                            | Go                 | Rust                    | Kirn                                 | Priority    |
| ---------------------------------- | ------------------ | ----------------------- | ------------------------------------ | ----------- |
| `?:`-style conditional expressions | ✗ (if is stmt)     | `if/else` expr          | ✅ Rust-form `if c { a } else { b }` | done        |
| Ternary `a ? b : c`                | ✗                  | macro                   | ✅ (sugar §4.1, `W-nosugar` none)    | done        |
| `++`/`--` inc/dec                  | ✅                 | ✗ (via `+=`)            | ✅ (sugar §4.1, live `W-nosugar`)    | done        |
| Assignment as expression           | ✗                  | ✗ (`=` returns `()`)    | ✗ (stmt only)                        | done/intent |
| Multiple/swap assignment `a,b=b,a` | ✅                 | ✅ tuple                | ✅                                   | done        |
| `defer`                            | ✅                 | ✗                       | ✅                                   | done        |
| `panic`/`recover`                  | ✅                 | `panic!`/`catch_unwind` | ✅ `raise`/`catch_panic`             | done        |
| `go`/goroutines                    | ✅                 | ✗ (threads)             | ✅ `spawn`                           | done        |
| channels                           | ✅                 | `mpsc`/`sync`           | ✅ `chan`                            | done        |
| `select`                           | ✅                 | ✗                       | ✅ `select`                          | done        |
| `?` propagation                    | ✗                  | ✅                      | ✅                                   | done        |
| `for range`                        | ✅                 | `for x in`              | ✅ `for pat in expr`                 | done        |
| C-style `for(;;)`                  | ✅                 | `loop`/`while`          | ✗                                    | low         |
| labeled `break`/`continue`         | ✅                 | `'label: loop`          | **NEW** ✅                           | done        |
| pattern bindings `@`               | ✗                  | ✅                      | **NEW** ✅                           | done        |
| or-patterns `a                     | b`                 | ✗                       | ✅                                   | **NEW** ✅  | done |
| slice patterns `[a,b,..]`          | ✗                  | ✅                      | ✗                                    | medium      |
| `..` rest in patterns              | ✗                  | ✅                      | ✗                                    | medium      |
| range/`..=` patterns               | ✗                  | ✅                      | ✅                                   | done        |
| `@`-combined ranges `n @ 1..=5`    | ✗                  | ✅                      | **NEW** ✅                           | done        |
| closures / lambdas                 | ✅                 | ✅                      | ✅ `(x) => e`                        | done        |
| multi-statement closures           | ✅                 | ✅                      | ✗ (single-expr only)                 | medium      |
| generics                           | ✅                 | ✅                      | ✅                                   | done        |
| const generics                     | ✗                  | ✅                      | ✗                                    | low         |
| associated types / GATs            | ✗                  | ✅                      | ✗                                    | low         |
| trait bounds resolution            | ✅ iface           | ✅                      | partial                              | medium      |
| `impl Trait for Type`              | ✗                  | ✅                      | ✅                                   | done        |
| auto-derive (`#[derive]`)          | ✗                  | ✅                      | ✗                                    | low         |
| macros `macro_rules!`              | (via generics)     | ✅                      | ✗                                    | low         |
| build tags / cfg                   | ✅                 | `#[cfg]`                | `unsafe` only                        | low         |
| error type `result[T,E]`           | ✗                  | ✅                      | ✅                                   | done        |
| optional `T?`                      | ✗                  | ✅ `Option<T>`          | ✅                                   | done        |
| nil-able `T?` vs `none`            | ✗                  | ✗                       | ✅ `is none`                         | done        |
| `sizeof` / `alignof`               | ✅ `unsafe.Sizeof` | `std::mem::size_of`     | ✅ builtins (`size_of`/`align_of`)   | done        |
| compile-time eval (`@comptime`)    | const-expr         | `const fn`              | ✅ (dict/set/struct/methods)         | done        |

### 3.2 Pattern-power roadmap (Rust `PatKind` parity)

Rust `PatKind`: `Wild, Ident, Struct, TupleStruct, Or, Path, Tuple, Box, Deref,
Ref, Expr, Range, Slice, Rest, Never, Guard, Paren`.

Kirn now covers: Wild, Ident(bind), Ctor(struct/enum), Or, Tuple, Range,
Literal(expr-path), Guard(always via `if`), Paren(group). Remaining gaps:

- **Slice patterns** `[a, b, ..rest]` — for `case [x, y]` destructuring lists.
- **Rest `..`** inside tuples/lists.
- **`Ref` / `&` patterns** — borrow a matched value.
- Nested `@` on any sub-pattern (only binding-first supported today).

### 3.3 Type-system roadmap

Rust's richness the checker lacks today: deref coercion, subtyping/unsizing,
associated const/type items, generic defaults, inference from context (bidirectional),
lifetime elision. Go's uniqueness: method sets as first-class interfaces, `iota`,
struct embedding/promotion.

Kirn priorities (checked checker): default type-param values, `interface`-as-set
for trait bounds (Go constrains via method sets), `iota`-like enum discriminants.

### 3.4 Ownership / memory model

Go: GC, no borrow checks. Rust: moves + NLL borrowck. Kirn today: value semantics
with `&`/`*` sugar in the interpreter; compile-time borrow/alias tracking is the
largest missing piece and the biggest engineering effort (parallel to
`rustc_borrowck`). Recommended: implement a conservative move/borrow pass over
the AST before native AOT emission.

### 3.5 Standard library roadmap

Go `src/` stdlib groups and Kirn status:

| Group       | Examples                                | Kirn                             | Priority    |
| ----------- | --------------------------------------- | -------------------------------- | ----------- |
| text/format | `fmt`, `strings`, `strconv`             | f-strings, `str` methods         | done        |
| collections | `container/*`, `sort`, `slices`, `maps` | builtin list/dict/set, `.sort()` | high        |
| os/fs       | `os`, `os/exec`, `path/filepath`        | `os` stub, `io`                  | medium      |
| net/http    | `net/http`                              | ✗                                | low         |
| encoding    | `encoding/json`, `base64`, `hex`        | `json.dumps` stub                | medium      |
| regexp      | `regexp`                                | ✗                                | medium      |
| time        | `time`                                  | `time` stub                      | done/medium |
| math        | `math`, `math/rand`                     | `math` stub                      | medium      |
| crypto      | `crypto/*`                              | ✗                                | low         |
| sync        | `sync`, `sync/atomic`                   | `chan`/`spawn`                   | medium      |

---

## 4. Deliberately Deferred (with rationale)

- **Full borrow checker / NLL** — requires a MIR-like IR + region inference; a
  conservative AST pass is the staged first step (§3.4).
- **Proc/macro system** — the module system + generics cover most needs.
- **Trait solving engine** — current impl uses structural method-set checks; a
  full solver is a separate project.
- **`for(;;)` C-loop & `goto`** — rejected in §4.9 of the grammar; `while`/`for`
  suffice.
- **Async/await, CGO-style FFI upgrade, WASM targets** — out of interpreter scope.

### 4.1 Sugar — `++`/`--` and C-style ternary `?:` (accepted, nudged)

Both forms are **accepted as sugar** for v1. This supersedes the earlier permanent-rejection
stance: `MISSING_PLAN.md` ownership rows 4/5 were reopened, and the parser now accepts the
syntax and desugars it to machinery Kirn already has.

- **`++`/`--`** — `x++` / `++x` desugar to `x := x + 1` and `x--` / `--x` to `x := x - 1`
  (`ast::ExKind::AssignExpr`), yielding the **updated** value (the classic pre/post distinction
  is deliberately collapsed — both spellings return the new value). `kirn check` still emits
  `warning[W-nosugar]` pointing at the operator, with a `replace with` fix-it, to steer agents
  and humans to the explicit `x += 1` / `x -= 1` forms.
- **C-style ternary `a ? b : c`** — right-associative, lower precedence than every binary
  operator and the pipe, and it shares `ast::ExKind::Cond` with the Rust-form
  `if c { a } else { b }`. The postfix `?` propagation operator keeps precedence: `parsePostfix`
  declines to consume a `?` that opens a ternary (a depth-0 `:` follows before the expression
  terminates). The `.?[` / `.?(` / `?[` safe-chain forms are unaffected.

Canonical forms (both spellings work; the left column is preferred):

| Sugar form    | Canonical form          |
| ------------- | ----------------------- |
| `x++` / `++x` | `x += 1`                |
| `x--` / `--x` | `x -= 1`                |
| `a ? b : c`   | `if a { b } else { c }` |

`kirn check` reports `warning[W-nosugar]` on every `++`/`--` use (a nudge with a
`replace with += 1` fix-it); it never fires on valid `a + +b` or `a - -b`. The ternary emits no
warning. A nested-ternary chain lowers on the tree-walker and VM; the MIR `rt` dumper still
rejects nested conditional expressions (shared with nested `if` expressions).

### 4.2 Type surface cut, `sizeof`/`alignof`, and const-eval breadth (this pass)

- **CapCase is canonical.** The lowercase constructor aliases `complex(...)`,
  `rational(...)`, and `decimal(...)` were **removed**; only `Complex` / `Rational` /
  `Decimal` resolve and the lowercase spellings now error (`unknown type` /
  `undefined function`). `big_int` keeps its lowercase spelling alongside `BigInt`
  (the deliberate exception; `bigint` still resolves as a _type_ spelling but not as a
  constructor). The numeric-type examples/tests (`examples/56_numeric_types.kn`,
  `tests/syntax/numbers_ok.kn`, `tests/vm_parity/numeric_types.kn`) were rewritten to
  CapCase.
- **`sizeof(T)` / `alignof(T)`** (aliases `size_of` / `align_of`) resolve as builtins.
  For scalar `T` the parser bakes an integer literal; for struct/enum `T` it lowers to a
  normal builtin call evaluated at run time. Layout is C-style — fields in declaration
  order with natural alignment, padded to the struct's max alignment, and every
  non-scalar field is an 8-byte handle. E.g. `sizeof(Point) == 16`,
  `alignof(Point) == 8`, and `@comptime(sizeof(Point))` folds.
- **Const-eval breadth.** `@comptime` (and `const`) now evaluates set/dict literals,
  struct construction `Point(x: 1, y: 2)`, struct field access, indexing into sets/dicts,
  iteration over sets/dicts (a dict yields its keys), and the pure receiver methods
  `len`/`length`/`size`, `upper`/`lower`/`trim`, `starts_with`/`ends_with`/`contains`/`has`,
  and `keys`/`values`. Struct values fold back to `Name(field: v, ...)`.
- **Coded driver/tool warnings.** The non-source `warning:` strings in
  `src/driver/driver.cpp`, `tools/kirn.cpp`, and `tools/kirnrun.cpp` now carry a stable
  `warning[W90xx]: ...` code — W9001 dep path, W9002 no entry, W9003 unreadable entry,
  W9004 no runner, W9005 bundle failed, W9006 PATH update, W9007 toolchain, W9008 legacy
  env. Source lints remain in the `W01xx` band; HIR uses `W9000`.

---

## 5. Roadmap (prioritized)

### v1.x — expression/pattern parity (this pass)

- [x] Pattern aliases `name @ sub-pattern`
- [x] Or-patterns `p1 | p2 | ...`
- [x] Labeled loops + `break label` / `continue label`
- [ ] Slice patterns `[a, ..rest]`; rest `..` in tuples
- [ ] Multi-statement closures (introduce `fn` block bodies)

### v2 — standard library breadth

- [ ] `collections` module: `HashMap`, `HashSet`, `Vec`-style ergonomics
- [ ] `strings` module surface: `split/join/trim/replace/index/lower/upper`
- [ ] `regexp` module (small NFA engine)
- [ ] `path` / `filepath` module: `join/basename/ext/clean`
- [ ] `os` runtime stubs: `env`, `args`, `exit`, `cd`
- [ ] `io`: `read_file/write_file/read_lines` (some exist)

### v3 — type-system hardening

- [ ] Default type-parameter values
- [ ] Interface-as-method-set bounds (Go style)
- [ ] `iota` enum discriminants
- [ ] Conservative borrow/move checker over AST (pre-AOT)

### v4 — concurrency & memory

- [ ] `sync` module: mutex/rwlock/once/barrier (Go `sync`)
- [ ] `atomic` operations
- [ ] GC tuning for the native heap (`new`/`box`)

---

## 6. Reference

- Go grammar & pipeline: `go-master/doc/go_spec.html`, `go-master/src/go/ast/ast.go`
- Rust AST enums: `rust-main/compiler/rustc_ast/src/ast.rs` (ItemKind, ExprKind,
  StmtKind, PatKind, TyKind, BinOpKind)
- Kirn normative grammar: `grammar/kirn.ebnf`
