# Coco — Advanced & User-Friendly Evolution Plan

**Status:** Living strategic document · Version 2.0
**Companion docs:** [`docs/COCO_PLAN.md`](docs/COCO_PLAN.md) (original 0–8 phase charter),
[`docs/FEATURE_GAP_ANALYSIS.md`](docs/FEATURE_GAP_ANALYSIS.md) (Go/Rust parity grid).

This plan advances Coco from a working C/Python-hybrid language (brace blocks, `;`
terminators, tree-walking interpreter, single-file GNU native AOT) into a language that is
simultaneously:

- **more powerful** — real compiler pipeline, rich type system, bytecode VM + native AOT;
- **more dynamic & user/English-friendly** — Python-like ergonomics, TypeScript-like gradual
  typing, Go/Rust/Java/C++-inspired modern features;
- **faster** — bytecode VM, method-cache/quickening, then native codegen and a JIT;
- **better at teaching** — deep, colourful, line-numbered diagnostics with caret, notes,
  help + fix-it suggestions, and a documented static-analysis lint suite (`unused`, `dead`,
  `shadow`, `import` hygiene, …);
- **more cross-platform** — a robust cross-build matrix, no-dependency bundling, and
  multi-archive release tooling.

Every phase has concrete **exit criteria** tied to verifiable artifacts (tests, corpus,
benchmarks). Phases are ordered for value-per-effort; later phases depend on earlier ones.

---

## 0. Orientation — Where Coco Is Today

Facts established by deep source review (see Appendix A for file:line evidence):

> **Audit note:** this table is the **pre-Phase-1 snapshot** and is partially stale. Phases 1–3 of
> this document are marked **COMPLETE** (diagnostics 2.0, warnings/lints/call-stack, special-file
> convention), so the corresponding rows here (e.g. "Diagnostics", "Tooling gap" for
> diagnostics/lint) are historical. For cross-plan ownership, see `DO_FIRST_PLAN.md` §Milestones →
> the [ownership map](#ownership-map-across-this-repo-s-plans).

| Area | Current state |
|---|---|
| Pipeline | `lexer → recursive-descent parser → AST → sema/type-checker → tree-walking interpreter` |
| Interpreter | `src/interp/runtime.cpp` (~3600 lines), naive AST walk, `PanicSignal`/`panicHere()` at 80 sites, **exit code 70** |
| Diagnostics | `DiagEngine` stores `{line,col,message}` only. Output = `file:line:col: error: msg`. **No caret, no color, no notes/help, no fix-it, no stack trace, no warning/lint channel** |
| Runtime errors | bare strings (`"index out of range"`, `"division by zero"`); line/col params exist but are `(void)`-cast away |
| Value repr | `repr`/`toStr` with **no depth/cycle/truncation limit** → can stack-overflow on cycles/self-referential data |
| Memory | refcount (`shared_ptr`) + `weak` refs; **no GC**, no tracing |
| Threads | real OS threads; channels via mutex/condvar; `select` busy-polls at 1 ms; **globals shared unsynchronized** (data races) |
| Types | full nominal systems: `struct/enum/trait/impl`, generics, `result[T,E]`, `T?`, `fn`, `tuple`, `var/let/const` |
| Patterns | Wild, Bind, BindAlias, Or, Slice, Rest, Tuple, Ctor, Range, Literal, guards |
| CLI | `run, new, test, install/add/update/remove/list, clone, build, targets, doc` — long if/else chain in `tools/coco.cpp:2845` |
| Tooling gap | **no `fmt`, `repl`, `check`, `lint`, `bench`, `lsp`, `bindgen`, `get`** (planned but unimplemented) |
| Testing | `examples/` corpus (31) run by `scripts/runall.ps1`; `tests/negative/` (8) NOT wired to a harness; no CMake `enable_testing` |
| Build | CMake (C++20) builds `coco,cocorun,cococheck,cocolex,cocoparse`; cross-build matrix of 6 triples via GNU toolchain; CI = Windows build-test + cross-arm64 |
| Perf | no constant folding, no method-cache, `for` copies each iterable, integer overflow unchecked |

The plan below directly closes every row.

---

## Phase 1 — Diagnostics Engine 2.0 (spans, caret, color, notes, fix-its)

> **Status: COMPLETE** (2026-08-31). Rich diagnostics model landed in `src/support/diag.h`
> (`Severity`, `SpanRange` w/ token extents, `FixIt`, `Note`, fluent `DiagEngine`, stable codes).
> `SourceMap` + GCC/Clang-style caret/color `renderDiags()` wired into the tools. Lexer
> `stampExtent()`; `ast::Span` + `Token` gained `endLine`/`endCol`. Runtime call-stack:
> `thread_local` frame stack + `CallFrameGuard` at every user-function entry, with
> `PanicSignal.frames` backtraces printed at all catch sites.

**Goal:** the single highest-leverage UX improvement, and prerequisite for *every* later tool
(`lint`, `fmt`, `lsp`, `repl`, better runtime errors).

### 1.1 Rich `Span` + `Diag` model
- Extend `src/support/diag.h`. Add:
  - `enum class Severity { Note, Warning, Error, InternalError }`.
  - `struct SpanRange { uint32_t line, col; uint32_t endLine, endCol; }` — capture token
    **ranges** (start+length), not just a point. The lexer already knows token extents.
  - `struct FixIt { SpanRange range; std::string replacement; }` — machine-applicable edits.
  - `struct Note { SpanRange span; std::string message; }` — secondary underlines/labels.
  - `struct Diag { Severity sev; std::string code; std::string message; SpanRange span;
     std::vector<Note> notes; std::vector<FixIt> fixIts; }` — carries a **stable code** like
     `E0027` / `W0134` (Kestrel/rustc style), so message wording can evolve without breaking
     tooling.
- Replace `DiagEngine::report(line,col,msg)` with a fluent builder:
  `engine.error(span).code("E0027").msg("...").note(span2, "...").fixit(span3, "...")`.

### 1.2 Source-line renderer (`SourceMap`)
- New `src/support/source.h` holding the raw source text + a line-start index.
- Render, GCC/Clang style:
  ```text
  src/main.co:12:7: error[E0027]: undefined variable 'y'
    12 |     let x = y + 1;
       |           ^ help: did you mean `x`?  → use `x`
  ```
- Colorize only when stderr is a TTY (`isatty`), controlled by `COCO_COLOR=auto|always|never`
  (mirrors GCC `-fdiagnostics-color`). Bold red `error`, bold magenta `warning`, bold cyan
  `note`, green/blue for range highlight.
- `-fmessage-length`-style wrapping and `file:line:col:` plain output behind
  `--error-format=human|json|plain` (JSON for editors/CI — SARIF-ready in Phase 9).

### 1.3 Wire the checker + parser onto spans
- Thread real spans through `src/sema/checker.cpp` (currently many `error(line,col)` calls).
  The AST already carries `Span` on nearly every node — surface `endLine/endCol` from the
  parser (tasks in 1.4).
- Upgrade the ~dozen bespoke error string sites to structured diagnostics with notes.

### 1.4 Lexer/parser extent tracking
- `src/lex/lexer.h`: make tokens carry `{line,col,length}` (or a `[lo,hi)` offset into a
  single source buffer). Parser `Span`s become offset ranges; this also enables multi-line
  spans for blocks, fn declarations, match arms.

### 1.5 Runtime diagnostics with locations
- Thread line/col into `panicHere(...)` (start with the entries that already take `line,col`
  but `(void)`-cast them — `callValue`, `makeEnumV`, `memberWrite`, `binop`).
- Add a **call-stack** (see Phase 2.4) and print `panic: msg` followed by an indented frame
  trace **with line numbers**.

### Exit criteria
- `coco build bad.co` renders caret + colored + code + `help:` suggestion.
- `COCO_COLOR=never` and `--error-format=json` round-trip.
- A golden test directory `tests/diag/*.co` snapshots expected outputs; negative corpus now
  asserts on **rendered** output, not just a substring.
- No regression: 31/31 corpus, 8/8 negatives still pass.

---

## Phase 2 — The Missing Stack: warnings, lints & runtime call-stack

> **Status: COMPLETE** (2026-08-31). `LintConfig` (`allow`/`deny`) passed to `Checker`; lints
> W0101 (unused import), W0102 (unused var), W0103 (unused fn), W0104 (dead code), W0105
> (shadowing), W0106 (unmuted `var`) implemented with usage/mutation tracking on `Symbol`
> (`used`/`mutated`/`declLine`/`declCol`/`importStmt`). `main`, consts, builtins, enum-variant
> discriminants, `self`, and `_`/`_`-prefixed bindings exempt from false positives. All build/run
> gates switched from `diags.count()` to `errorCount()` so warnings print but never block. Corpus
> runs warning-free.

**Goal:** `unused import/variable/function/branch` + `dead code` + `shadowing` become first-class
**warnings**, with a documented, configurable lint framework; runtime errors get real call
stacks.

### 2.1 Unused-import tracking
- In `src/sema/checker.cpp` `Import` handling, record each imported binding's `used` flag;
  at module end, if unused, emit `warning[W0101]: unused import 'foo'` with a `fixIt` deleting
  the line. `from x import *` (star) is exempt; per-name imports are checked.
- Respect `pub`/re-export (an import re-exported via `export` is *used*).

### 2.2 Unused-variable / unused-function / dead-code lints
- Add a **use-information pass**: after name resolution, collect def-sites vs use-sites.
  - `W0102 unused variable 'x'` (non-`pub`, non-`_`-prefixed, non-`debug` locals).
  - `W0103 unused function 'f'` (top-level non-`pub`, and `pub` inside libraries under
    `--warn-all`).
  - `W0104 dead code` for statements after unconditional `return`/`raise`/`break`.
- Keep AST stable; this is a read-only analysis pass reusing the resolved symbol table
  (`src/sema/symbols.h`).

### 2.3 Shadowing & mutability lints
- `W0105 variable 'x' shadows outer binding` (track lexical scopes; allow opt-out
  `@allow(shadow)`).
- `W0106 'var' declared but never mutated`.
- `W0107 comparison is always true/false` (constant-folded literals).

### 2.4 Runtime call-stack
- Add `std::vector<Frame>` to `Interpreter` (`src/interp/runtime.h`) as an explicit stack;
  `execCall` pushes `{fnName, line}` and pops on return; exceptions (`PanicSignal`) walk it.
- Prints `at frame N: f() at src/main.co:12:7` under the panic. Prevent double-instrumentation
  cost: only maintain the stack; do not deep-copy env.

### 2.5 Lint framework & config
- Introduce `@warn[group]`, `@allow[...]`, `#![allow(...)]`-style file-level toggles and a
  `[lint]` section in `coco.toml` (severity per lint: `allow|warn|deny`).
- `coco check` subcommand (Phase 9) surfaces these uniformly.

### Exit criteria
- `unused import`, `unused variable`, `unused function`, `dead code`, `shadowing`,
  `unused var` all fire as line-numbered warnings with correct spans on crafted corpus.
- `@allow(...)`/`coco.toml [lint]` suppress/deny correctly.
- Runtime panic prints a multi-frame stack trace. Corpus still green.

---

## Phase 3 — The Special-File Convention (Python-`__main__`/`__init__` inspiration)

> **Status: COMPLETE** (2026-08-31). `pin.co` ratified as the package-initializer /
> public-API-aggregator name. Entry resolution implemented in `tools/coco.cpp`
> (`resolveEntry`), package init in `src/interp/runtime.cpp` (`resolvePackageEntry`
> prefers `pin.co`; module-load now executes top-level `Assign`/`AugAssign`/`ExprStmt`
> so pin.co init runs **once**). `coco new`/`coco new lib` scaffold `main.co`/`pin.co`
> with doc templates. Bonus fixes surfaced by this phase: `coco run <dir>` module-path
> resolution (was resolving relative to `.` instead of the target dir), UTF-8 BOM
> tolerance in `readFile`, and `coco test .` resolving imports from the project root.
> Verified by `examples/32_conventions.co` + `tests/conventions/run.ps1` (7 checks).

### 3.1 Design the conventions (ratify in `grammar/coco.ebnf` notes + docs)
Proposed names (each namespace-clean inside a package root):
| File | Role | Python analogue | Coco semantics |
|---|---|---|---|
| `main.co` | executable entry point | `__main__.py` + `if __name__=="__main__"` | the file whose `main()` (or top-level `main()`) is invoked by `coco run .` / app build |
| `pin.co` (or `pkg.co`) | package initializer / public-API aggregator | `__init__.py` | run once when the package's tree is imported; declares the package's `pub` surface |
| `help.co` | per-package doc/help text + `--help` template | (PyPI long_description) | used by `coco doc`; human-facing |
| `bench.co` | benchmark suite conventions | `benchmarks/` in many langs | read by `coco bench` (Phase 8) |
| `tool.co` | internal dev-tooling script (not shipped to users) | `tools/` dir | ignored by `coco install` packing |

Open naming alternatives to decide by corpus vote (all listed so the plan is *choice-bearing*):
`__main__`→`main.co`; `__init__`→`pin.co` / `pkg.co` / `core.co` / `api.co`; docs→`help.co`;
tests→`<name>_test.co` (already established).

### 3.2 App vs package resolution (`tools/coco.cpp`)
- `coco run .`: if manifest `main` is set use it; else look for `main.co`; then `pin.co`+`main.co`;
  else error with a helpful fix-it (`coco new` scaffolds the right file).
- `import "pkg"` of a package tree runs `pin.co` first (module-init cache, run-once), then
  exposes its `pub` aggregate, matching Python's package init but with Coco's `pub`/module
  visibility rules.

### 3.3 Scaffolding
- `coco new`/`coco new lib` generate the right convention files with doc-comment templates.

### Exit criteria
- `coco run .` and `coco build .` correctly resolve all convention-file combinations.
- Importing a package executes `pin.co` exactly once and exposes its `pub` API.
- A new example `examples/32_conventions.co` + a `tests/conventions/` suite exercises the matrix.

---

## Phase 4 — Bytecode VM (faster interpreter) + expression compiler

**Goal:** get a **~1.9–2.2×+** interpreter speedup from a bytecode VM (well documented in the
shadowed-literature: tree-walk → unoptimized VM is the big win), while keeping the AST as the
authoritative correctness model.

### 4.1 Bytecode compiler (`src/vm/`)
- New `src/vm/bytecode.h`, `src/vm/compiler.cpp`, `src/vm/vm.cpp` (stack VM).
- Lower the duck-typed `src/interp/value.h` `Value` model to ops:
  `OP_PUSH_CONST, OP_LOAD_GLOBAL/FAST, OP_STORE, OP_CALL, OP_INVOKE_METHOD, OP_JUMP/IF,
  OP_MAKE_LIST/DICT/SET/TUPLE, OP_BINOP, OP_RETURN, OP_NEW, OP_MATCH`, … Reuse `Value` as the
  operand type (no rewrite of the value model).
- Keep `VM` semantics byte-for-byte identical to the tree-walker (differential fuzz harness).

### 4.2 Perf levers (in priority order, per the literature)
1. Reduce dispatch (fewer/hot instructions first).
2. **Method-call cache / inline cache** — `invokeMethod` currently linearly rescans the struct
   body + `impls_` per call (`runtime.cpp:1895`); cache `{type,method}→slot`.
3. **Register-machine operand forms** for hot instructions (or superinstructions).
4. **Constant folding** — stop re-parsing `Int/Float` literals on every eval
   (`runtime.cpp:1176`); precompute at compile time.
5. **`for`-iteration** — avoid the full `iterateSeq` copy (`runtime.cpp:2847`); iterate the
   underlying `shared_ptr` vector in place (immutable during iteration post-check).
6. Checked vs wrapping arithmetic behind `--release` (already the documented intent,
   COCO_PLAN §5 note 5).

### 4.3 Architecture
- The interpreter becomes "compile AST → bytecode → run VM"; the AST remains the sema/front-end
  target so `coco check`, `coco lint`, tooling all operate on one tree.
- The portable bytecode bundle (`.cob`, emitted by `coco build` for toolchain-less
  cross targets) already exists — the VM is its runtime.

### 4.4 Verified status (VM default-on; specialized ops + flat-SP stack landed)
- The core-slice VM is **correct** (32/32 deterministic differential match; 33/33 corpus; 8/8
  negatives; 7/7 conventions, in both Debug and Release; ASan-clean under the VM) and is now the
  **default runner** — `cocorun` (no flag), `coco run`, `coco test`, `coco build` and the
  executables `coco build` produces all run on the VM. Pass `--no-vm` / a `--no-vm`-style flag to
  force the tree-walker for comparison.
- **Slot-based locals:** frame-level and loop-var names are assigned compile-time integer slots and
  accessed via `OP_LOAD_LOCAL`/`OP_STORE_LOCAL`/`OP_ITER_VALUE_LOCAL` (flat `Value[]` per call)
  instead of `env->find` string hashing. A depth-aware shadow guard falls back to the exact env
  path whenever a slot name would be shadowed.
- **Compact `VmVal`:** the operand stack and frame locals in `vmRunBody` use a ~16-byte tagged
  `VmVal` (`VK` + inline int/float/bool/char + heap `Value*` box) instead of moving the 472-byte
  shared `Value` per op.
- **Research-backed speedups (new, 2026):** per CPython PEP 659 / Ruby YARV opt_* opcodes and the
  stack-pointer model (Ruby `cfp->sp`, CPython `stack_pointer`):
  - **Specialized numeric opcodes** (`OP_BINARY_ADD/SUB/MUL/DIV/MOD/POW`, `OP_LT/LE/GT/GE/EQ/NE`,
    `OP_RANGE`, `OP_NEG`, `OP_NOT`) — the operator is an immediate, so the hot arithmetic/comparison
    loop does **no per-op string compare** (before, `OP_BINARY` did `if (op=="+") … else if` chains).
  - **Flat pre-sized operand stack with an explicit stack-pointer (index-SP)** — push/pop reuse
    slots via `st[sp]/st[sp-1]` instead of `std::vector::push_back/pop_back` per op; heap-backed so
    it stays safe under deep recursion.
- **Honest perf result (Release, measured, best-of-3, idle machine):** VM is ~2.7–4× faster than the
  tree-walker:
  - `for i in 0..n` arithmetic loop: ratio ≈ 0.37 ⇒ ~2.7× faster.
  - `while` arithmetic loop: ratio ≈ 0.35 ⇒ ~2.9× faster.
  - fib(25) call bench: ratio ≈ 0.25 ⇒ ~4.0× faster.

### Exit criteria
- `svm` differential test: thousands of randomized programs produce identical output in
  tree-walker vs VM.
- Benchmark suite (fib, nbody, string ops, JSON) shows VM ≥ 1.5× faster than today.
- Full 31-example corpus passes under the VM; ASan/UBSan clean build in `build-asan`.

---

## Phase 5 — Dynamic & Gradual-Typing Surface (Python/TypeScript friendliness)

**Goal:** Python-grade ergonomics where the static type system is not required, delivered as
**gradual typing** (TS-style), never weakening existing checks.

### 5.1 The `any` type + dynamic bindings
- Predeclare `any` (currently only `Error/Unknown` poison markers exist in `src/sema/type.h`):
  an explicit `any`/`dynamic` type a variable may opt into: `var x: any = 5; x = "hi"`.
- `any` values: static checker defers; runtime dispatches on the `Value::k` tag (dynamic
  typing at runtime — the value model literally already does this).

### 5.2 Runtime reflection & introspection (`reflect` module)
Research-backed (Kotlin/Ruby/Java reflection; Python introspection). Provide as a **stdlib
module** `reflect`, implemented by exposing `Value::k`/fields:
- `reflect.type(x) -> string` (e.g. `"int"`, `"list[float]"`, `"Point"`).
- `reflect.fields(x)`, `reflect.methods(trait/struct)`, `reflect.new(name, args...)`, dynamic
  member set/get.
- Enables generic serialization (JSON/CBOR in Phase 7) *without* compile-time derive.

### 5.3 Duck typing via `interface`-as-method-set
- Add Go-style `interface` constraints as a **trait-bound sugar**: a value satisfies an
  interface if it provides the method set (structural typing / duck typing), regardless of a
  declared `impl`. This makes library code accept "anything that has `.len()`".

### 5.4 Python-like ergonomics (ergonomic sugar, purely additive)
- `++`/`--`? Keep **out** (rejected in grammar §4.9). Use `+= 1`.
- Multi-statement **closures** with `fn`-style block bodies:
  `map(xs, fn (v) { return v * 2; })` — Phase 11, but spec it here.
- **Splat/kwargs sugar**: `f(*xs)` / `Point(**d)` for positional/named-arg spread (low-cost in
  `CallArg`/VarArgs).
- `with`/`defer`-style RAII stays via `defer` (exists).

### 5.5 Eval / dynamic code execution (optional, guarded)
- A `core.dyn.eval(string)` under an explicit `--enable-eval` flag (Python's `eval` analogue).
  Default-off for safety; powers REPL (Phase 9) and scripting.

### Exit criteria
- `any` + runtime reflection examples run correctly; `reflect.type/fields/methods` verified.
- Interface-method-set bound accepts a duck-typed value; static and dynamic paths both tested.
- New examples `33_dynamic.co`, `34_reflect.co`. Grammar notes updated; no existing corpus regressions.

---

## Phase 6 — Type-System Hardening (Rust/Go/Java-grade)

**Goal:** close the checker gaps in `docs/FEATURE_GAP_ANALYSIS.md` §3.3, landed as opt-in
strictness so existing code keeps compiling.

### 6.1 Exhaustiveness & reachability
- Real exhaustiveness checking for `match` over enums (Java sealed-classes "switch expression",
  Rust), reporting missing arms as errors with a fix-it listing them, and un-reachable arms as
  `dead code` warnings.

### 6.2 Records (immutable data carriers)
- Add `record Point(x: int, y: int)` — like Java records: value-equality,
  copy-on-write-safe, auto-`repr`, structural immutability. Cheap on the by-value struct model.

### 6.3 Generics maturity
- Default type parameters `[T = int]`; associated constants/type items on traits (advanced);
  `Interface` bounds (Phase 5.3); bidirectional context-driven inference for lambdas.
- `iota`-style enum discriminants (described in FEATURE_GAP §3.3; add `enum E { A, B }` →
  implicit ints with `E.A.to_int()`).

### 6.4 Options & results ergonomics
- `result[T,E]` `?`/`try` propagation already works; add `match` default-arm guard and
  `.unwrap_or`, `Option`/`result` combinator presets (Go `errors`-like `wrap`, Rust
  `map/and_then`).

### 6.5 Conservative borrow/move checker (pre-AOT)
- Per FEATURE_GAP §3.4: a **conservative AST pass** (before native AOT) that flags escape of
  `&`/`*` borrows out of scope and moves-after-use in `var`/`let`. Not full NLL — a safe
  over-approximation that prevents the most common aliasing bugs. Full region-inference borrowck
  is Phase 12.

### Exit criteria
- Exhaustiveness/record/iota/generic-default examples; negative tests for non-exhaustive match
  and for borrow-escape.
- `coco check` accepts the hardened grammar; entire corpus still type-checks.

---

## Phase 7 — Standard Library Breadth & Collections (Go stdlib roadmap)

> **Dedup/ownership note:** the detailed spec for every module below lives in
> `STD_LIBS_PLAN.md` (→) — treat this Phase 7 as the strategic **anchor/pointer**, not a parallel
> spec. Also: **7.1 collections types** (`deque/heap/bitset/…`) are modeled in `DATA_TYPE_PLAN.md`
> Phase 10 (→); **7.2 regexp** has an **implementation-conflict to resolve** — this line says a
> C++ RE2-style NFA module, but `STD_LIBS_PLAN.md` Phase 0+5a proposes a **pure-Coco** regexp
> engine. **Recommendation: adopt the pure-Coco approach**; amend
> this line accordingly before coding.

**Goal:** deliver the high-priority groups from FEATURE_GAP §3.5, plus the reflection-driven
serialization enabled in Phase 5.

### 7.1 Collections (`collections` module)
- `HashMap`, `HashSet` backed by existing `Dict`/`Set` resources; `Vec`-style ergonomic wrappers;
  `deque`, `ringbuf`, `heap`, `bitset` (COCO_PLAN §13 lists these). Iterators + generator views
  stay lazy where possible.

### 7.2 Text & encoding (`text`, `encoding`, `regexp`)
- `text`: Unicode segmentation, `split/join/trim/replace/index/lower/upper/search`.
- `encoding`: JSON encode/decode via reflection (Phase 5.2), Base64, hex, TOML (dogfood
  `tomlmini.h`).
- `regexp`: small linear-time NFA engine (RE2-style) — a self-contained C++ module.

### 7.3 OS / I/O / time / math polish
- `os`: `env`, `args`, `exit`, `cd`, `getenv`; process spawn/pipes (Go `os/exec`).
- `io`: buffered reads, `read_file/write_file/read_lines`, paths/filepath (`join/basename/ext`).
- `time`: monotonic+wall clocks, durations, formatting, `sleep`.
- `math`: `math/rand` (xoshiro PRNG), statistical helpers, SIMD intrinsics wrappers.

### Exit criteria
- Each module has `examples/*.co` + a `*_test.co`; `coco test` green.
- JSON round-trip via reflection for nested structs/lists; regexp suite (match/submatch/replace)
  passes against a fixture corpus copied from the reference regexp tests.
- Zero new runtime deps beyond libc/libcoco.

---

## Phase 8 — Benchmarking, `coco bench`, and Fast-AOT (faster like C/C++)

**Goal:** verifiable speed, plus a proper optimized native path (the "faster like C/C++" ask).

### 8.1 `coco bench`
- New subcommand reading `bench.co`-style benchmarks; runs N iterations, reports mean/p50/p99,
  wall + CPU time; a `bench` table preserved across runs for regression detection in CI.
- Benchmarks wired into `.github/workflows/ci.yml` so perf regressions fail CI.

### 8.2 Optimized AOT (native codegen; `coco build --native`) **~DONE**
- **Implemented slice 1 (Phase 8.2):** a real scalar backend at `src/backend/native.cpp` lowers
  statically-typed *scalar* user functions (int/float/bool params & locals, arithmetic, comparison,
  logical ops, if/elif/else, while, int-range `for`, return, calls to other lowerable fns) to plain
  C++. `emitNative()` runs during `coco build` (after the checker's type pass), the C++ is embedded
  in the standalone launcher, and the registration helper wires each lowered function into the
  interpreter. The runtime dispatch in `runFunc` is **native → VM → tree-walker**, so anything not
  provably scalar keeps exact interpreter behaviour (the hybrid "VM/native hot paths" architecture).
- `coco build file.co --native` currently works single-file; `main` stays interpreted (it uses
  `print`/strings) so programs "just work" while hot scalar functions run as real compiled C++.
- Measured (fib(0..=30), manual): VM `~17.3 s` → native `~59 ms` ≈ **290× faster**, output identical
  (`2178308`); corpus green (34/34), native/VM differential green.
- **Remaining (later slices):** `--release`/`-O2` on the emitted C++, constant folding, lowering
  `main` itself, strings/lists via native runtime calls, and ASan/UBSan in CI for `--native` builds.
  §8.2.3's "keep the VM for dynamic/`any`/reflection; hybrid" stance is the plan going forward.
- Keep the VM for the dynamic/`any`/reflection paths; hybrid "VM + native hot paths" is the
  pragmatic, proven architecture (cf. JIT tiering in the literature).

### 8.3 Bench targets
- fib(30), nbody, JSON parse (10k records), string concat (100×), list map/sort. Document a
  "Coco vs Python vs C" table in `docs/BENCHMARKS.md` and set explicit targets (e.g. VM ≥ 2×
  Python's CPython for scalar loops; native ≥ within 5–10× of C for the same hot function).

### Exit criteria
- `coco bench` runs and CI enforces a regression budget.
- `--release` native path is measurably faster than the interpreter-embedded AOT for the hot
  benchmarks, with ASan/UBSan clean and corpus green.

---

## Phase 9 — `coco fmt`, `coco repl`, `coco check`/`lint`, `coco get`, LSP

**Goal:** the "one executable, like `go`" toolchain from COCO_PLAN §14, all with the new
diagnostics engine.

### 9.1 `coco fmt` (gofmt spirit, zero-config)
- Parse → canonical re-print via the AST pretty-printer (`src/ast/ast_dump.cpp` is a head start).
  Deterministic: brace indentation, spacing, `;` normalization, remove empty `;`, align nothing
  by default. `-d` diff, `--check` fail-in-CI, `-w` write. `coco fmt --migrate` for syntax
  migrations.

### 9.2 `coco repl`
- Read-eval-print on the bytecode VM (Phase 4) or interpreter; multiline continuation via braces,
  `:help`, tab-completion, history. A `--enable-eval`-style opt-in not required for REPL.

### 9.3 `coco check` / `coco lint`
- `check`: front-end + sema only, no build; surfaces all errors+warnings with the new renderer.
- `lint`: runs the Phase-2 warnings + Phase-6 exhaustiveness as a batch, with `--fix` applying
  `FixIt`s (rustfix-style) and `--explain CODE` giving prose (rustc/E-`explain` model).

### 9.4 `coco get` + registry
- Replace/augment `coco add` with `coco get <pkg>` (alias) and `coco get --scan` to audit a
  project (unused deps — ties into unused-import lint).

### 9.5 LSP server (`tools/lsp`, JSON-RPC)
- Highest-leverage adoption tool (COCO_PLAN says this). Serve `textDocument/diagnostic`
  (push from `DiagEngine`), `completion` (symbol table), `hover` (doc comments), `definition`
  (DottedName resolution), `formatting` (calls `fmt` engine). Wire to `compile_commands.json`
  (already generated by CMake).

### Exit criteria
- `coco fmt --check` is idempotent on the whole corpus (formatted corpus == corpus).
- REPL executes sample snippets and has completion.
- CLI application: `coco new demo && cd demo && coco add greet && coco fmt -w && coco check &&
  coco test` all green. LSP passes a JSON-RPC smoke suite (completion + diagnostics).

---

## Phase 10 — Cross-Platform Build & Cross-Compile Power-Up

**Goal:** the "build more cross-platform, more advanced" ask — robust matrix, no-dep bundles,
and multi-archive releases.

### 10.1 Build-backend abstraction (`src/build/`)
- Extract `cmdBuild`-specific logic (`tools/coco.cpp:2629`, `buildProgram`, target matrix
  `targetMatrix() :1814`) into a `src/build/` module so `coco build`, `coco cross`, CI, and a
  future IDE backend share one code path.
- Centralize `hostTarget()`/`resolveCrossCxx()` into a `Toolchain` table with per-triple
  candidate compilers, as today, but made data-driven.

### 10.2 Cross-platform improvements
- **Zig as a cross-linker — DECISION: abandoned after verification.** Tested zig 0.16.0 linking
  `aarch64-linux` ELF from a Windows host: it hangs reproducibly (even a tiny `printf` hello for
  `aarch64-linux-musl`; compile-only works, `x86_64-linux` works, any `-fuse-ld` linkage hangs).
  Known upstream (codeberg ziglang #31752/#31189/#31210, kaappi #1613). Not Coco's fault; keep
  the GNU/PATH-probe fallback (current default) and invest in the Phase-8.2 native codegen
  (`src/backend/native.cpp`) as the primary acceleration path instead.
- **Static linking** option (`--static`) bundling libc/libcoco → portable single binaries.
- **`.cob` portable bytecode** already exists as the no-toolchain fallback; keep it as a
  target equal to native.
- **Installers**: `--install` producing per-OS artifacts — Windows `.zip`/`.exe`, macOS
  `universal2` (build both arch slices), Linux `tar.gz` — with `SHA256SUMS` (cargo-forge model).

### 10.3 CMake hardening
- `enable_testing()` + `add_test` registering corpus + negative + unit tests under CTest.
- Presets for `ninja` Debug/Release and MSVC/GNU; `export compile_commands`.
- Optionally introduce a thin `xmake.lua`/Meson as an alternative generator (advanced,
  optional) so the toolchain isn't CMake-locked.

### Exit criteria
- `coco build --target=windows-arm64|linux-amd64|darwin-amd64` succeeds from the Windows host
  using the GNU fallback path (zig was verified and abandoned — see above).
- `--static`, universal2, and `--install --result build/release` produce the documented
  artifacts + `SHA256SUMS`.
- CTest runs the full suite; GitHub Actions matrix adds a Linux + macOS job — currently only
  Windows itself is built in CI.

---

## Phase 11 — Ergonomics & Syntax Polish (English-friendly)

**Goal:** the "more user- & English-friendly" ask — smaller helpers, clearer grammar, and a
more approachable surface, without breaking the frozen keyword set.

### 11.1 Syntax harmony
- Optional `fn` alias for `def`? **Recommendation: add `fn` as an accepted alias** for `def`
  (settling FEATURE_GAP §2 open question) so Go/Rust programmers feel at home; `def` stays.
- Multi-statement closures: `map(xs, fn (v) { return v * 2; })` (superset of the current
  single-expression lambda), accepted opt-in.
- `with`-style resource scope as sugar over `defer` inside a block (`with f { … }`) — optional.
- Nicer identifier rules: allow trailing `?` for predicates (Rust) or keep `is` predicates;
  decide via corpus, keep backward-compatible.

### 11.2 Errors in the language
- `error` module with `message()`/`source()` chain (COCO_PLAN §8) and `@discardable` on
  ignored `result`s as a *warning* (ties to Phase 2).

### 11.3 Docs & teaching
- `coco doc` already renders `##` docs; extend examples/ + a "Tour of Coco" so newcomers learn
  conventions (Phase 3), patterns, and the lint/diagnostics UX. There's already
  `docs/index.md` tooling — publish `features` and `functions` reference sections.

### Exit criteria
- `fn` alias + multi-statement closures added without altering existing-file behavior (corpus
  green); brand-new examples demonstrate the friendlier surface; docs index regenerated via
  `coco doc`.

---

## Phase 12 — Full Borrow Checker & Memory Safety v1 (Rust-grade, staged)

**Goal:** graduate the conservative Phase-6.5 pass into a real ownership/move checker with
region inference — the "advanced like Rust" differentiator.

### 12.1 MIR (mid-level IR)
- Introduce a small SSA MIR (`src/mir/`) — the architecture the original plan always intended
  (`docs/COCO_PLAN.md` §10.2, "arena arrive with MIR"). The AST stays the front-end surface;
  MIR is where borrowck + optimizations live.
- Move-liveness, domination, and region inference over MIR.

### 12.2 Borrowck rules
- `&`-borrow must not outlive the borrowed place (escape detection); `var` move-after-use
  errors; mutable aliasing `&mut` rules; `weak`/heap interplay documented.
- Errors land on the Phase-1 renderer with multi-span notes (borrow created here, used there).

### 12.3 Opt-in strictness
- Because Coco is gradual (Phase 5), full borrowck is enforced under `--strict`/`@strict`
  (opt-in), while the conservative pass (6.5) stays default-on for safety baseline.

### Exit criteria
- A `tests/borrow/*.co` corpus with positive (compiles) and negative (rejected with notes)
  cases; ASan-clean; runtime unaffected when not `--strict`.

---

## Phase 13 — Native JIT (optional, stretch) & compiler optimization passes

**Goal:** the last big performance lever — a JIT, plus a modern optimizer — both optional
because the AOT+VM hybrid already covers most needs.

### 13.1 JIT
- If targets demand it, add a lightweight **baseline JIT** over the VM's bytecode (method-JIT,
  tiered: interpret → profile → compile hot functions), following the
  interpreter→profile→compile→deopt flow described in the JIT literature. Use `MIR`-based
  lowering to machine code (or via emitted C++ → native, which we already have), avoiding a
  hard LLVM dependency now.
- Hot-path detection thresholds (call counts), inline caches (reuse Phase 4.2).

### 13.2 Optimizer passes
Constant folding/propagation, dead-code elimination, loop-invariant code motion, inlining of
`pub`/static small functions, and escape analysis to stack-allocate heap objects — all on MIR,
validated only under `--release`.

### Exit criteria
- Benchmarks show the JIT/`-O3` path ≥ 2× over the interpreter-embedded AOT for the hot suite;
  correctness differential against the unoptimized VM stays green.

---

## Phase 14 — Concurrency, Memory & GC (Go-grade goroutines/recovery)

**Goal:** make the Go-inspired concurrency actually safe and scalable, plus a real GC story.

### 14.1 Data-race safety
- Fix the documented **unsynchronized shared globals** (`runtime.h` note; `spawn` shared
  `EnvS`). Introduce a Go-like scheduler for `spawn`/`chan`/`select` (COCO_PLAN §7):
  - Replace the 1 ms busy-poll `select` (`runtime.cpp:3582`) with condvar-driven readiness.
  - **Sendability checker** (`isSendable`, exists in `checker.h`) enforced at compile time for
    `spawn` payloads → no shared mutable globals across threads unless via `chan`/`sync`.
  - Worker pool / M:N scheduler; cap threads to cores; structured concurrency groups (Go
    `errgroup`-style).
- `sync` module: `mutex`, `rwlock`, `once`, `barrier`, `atomic` (FEATURE_GAP §3.5).

### 14.2 GC / memory
- Continue refcount + `weak` (cycles already breakable) but add:
  - `repr`/`toStr` **depth + cycle guards** (fixes the current stack-overflow latent bug on
    self-referential data — Phase 1-adjacent but firmed up here).
  - Periodic/ephemeron-aware cycle collection for `new`/`box` heap, or a precise-enough tracing
    GC under `--gc` — decide by benchmark, keep `weak` semantics stable (examples 27/28).

### Exit criteria
- A 10k-task web-crawler demo (COCO_PLAN Phase-4 goal) runs ASan-clean with no data-race reports
  (add ThreadSanitizer build in CI `build-tsan`).
- `repr` of self-referential data terminates; cycle collection frees objects under `--gc`.
- `sync`/`atomic` tests green; select is condvar-driven (no busy-poll).

---

## Phase 15 — Ecosystem & Release (v1.0 path)

**Goal:** the "more and more important and useful thing" — real adoption infrastructure.

### 15.1 Package ecosystem hardening
- `coco publish` (or extend `build lib`) → push `.cocolib` + registry metadata to `coco-lib`
  (`coco-libs` registry). Registry schema evolution (semver ranges, features,
  `entry_points`-style advertised commands, `pin.co` init).
- Signature/checksum verification on install; private registries.

### 15.2 Docs, tests, CI, releases
- Coverage: golden diagnostics, lint, VM-vs-treewalk differential, borrow, conventions,
  concurrency/TSan, cross-build matrix.
- Release automation: GitHub Actions producing the Phase-10 installers + `SHA256SUMS` +
  SLSA provenance (already scaffolded in `.github/workflows/generator-generic-ossf-slsa3-*`).
- Website + book + `--version`/changelog governance; 25+ real external projects → v1.0.

### Exit criteria
- Full CJE: `coco new → add deps → build → test → bench → publish` end-to-end, documented and
  exercised in CI on win/linux/mac.

---

## Roadmap Summary (dependencies)

```
Phase 1 (diagnostics) ──► Phase 2 (lints/warnings/stack) ──► Phase 9 (fmt/repl/check/lsp)
        │                              │                              ▲
Phase 3 (conventions) ────────────────┘                              │
Phase 4 (bytecode VM) ──► Phase 5 (dynamic/any/reflect) ──► Phase 6 (types) ──► Phase 7 (stdlib)
        │                        │                                │
Phase 9/10 (toolchain/cross) ◄───┘                                Phase 8 (bench/AOT)
        │
Phase 12 (MIR + borrowck) ──► Phase 13 (JIT/optimizer)
Phase 14 (concurrency/GC)
Phase 15 (ecosystem/release)
```

**Suggested order of execution for a solo dev** (value first): 1 → 2 → 3 → 4 → 9 (fmt,
repl, check) → 10 → 5 → 6 → 7 → 8 → 14 → 11 → 12 → 13 → 15.

**Suggested order for maximum user-visible benefit quickly:** 1 (diagnostics) → 3 (conventions)
→ 2 (warnings/lints) → 9 (fmt/repl/check) → 4 (VM) → 7 (stdlib) → 10 (cross-build).

---

## Appendix A — Evidence base (file:line)

- Diagnostics minimalism: `src/support/diag.h:9-26` (`Diag{line,col,message}` only).
- Compile-error format `printDiags`: `tools/coco.cpp:409-413`.
- Runtime panic channel `panicHere`/`PanicSignal`: `src/interp/runtime.h:44-47`,
  `src/interp/runtime.cpp:31` (80 sites); `(void)`-cast locations:
  `runtime.cpp:1759, 2791, 3244`.
- No stack trace / call stack: `src/interp/runtime.h:54-158` (class has none).
- `repr`/`toStr` no limits & infinite-recursion bug: `src/interp/runtime.cpp:204-309`,
  `value.h:149-159`.
- Threading / select busy-poll / unsynchronized globals: `runtime.cpp:3520-3584, 3444-3518`,
  `runtime.h:22-38, 77-81`.
- Method-lookup linear rescan: `runtime.cpp:1895-1913`.
- `for` copies iterables: `runtime.cpp:2847`.
- CLI dispatch chain: `tools/coco.cpp:2845-2939`; missing `fmt/repl/check/lsp` (only planned
  in `docs/COCO_PLAN.md:732-740`).
- Cross-build matrix + toolchain resolution: `tools/coco.cpp:1785, 1814, 1842`.
- CMake targets + warnings: `CMakeLists.txt`; CI: `.github/workflows/ci.yml`.
- Test harness: `scripts/runall.ps1` only; negatives unwired; no CTest.
- Reference language sources surveyed: `C:\Users\rkriad585\Projects\go-rust-source-code`
  (`go-master`, `rust-main`).

## Appendix B — Research references (web)
- Clippy lint categories (correctness/suspicious/style/complexity/perf/pedantic) —
  `doc.rust-lang.org/stable/clippy`, `rust-lang.github.io/rust-clippy`.
- rustc diagnostics (Span, caret, help:, fix-its, `--error-format json`) —
  `rustc-dev-guide.rust-lang.org/diagnostics.html`.
- GCC diagnostics formatting (color, caret, labels, `-fdiagnostics-*`) —
  `gcc.gnu.org/onlinedocs/GCC`.
- Python `__main__.py` / `__init__.py` conventions —
  `docs.python.org/3/library/__main__.html`, `realpython.com/python-init-py`.
- Dynamic languages / reflection / duck typing — Wikipedia "Dynamic programming language",
  "Reflective programming".
- Bytecode-VM-vs-AST-interpreter performance — "AST vs. Bytecode: Interpreters in the Age of
  Meta-Compilation" (OOPSLA 2023), "Benchmarking a Bytecode VM".
- Cross-compilation with zig — `rust-cross/cargo-zigbuild`, `waveletsolutions/cargo-forge`,
  `xmake-io/xmake` (cross-platform build + package + cache).
- Modern Java — records, sealed classes, pattern matching, `switch` exhaustiveness
  (`docs.oracle.com`, javapro.io).

Note: `PLAN.md` intentionally leaves a few naming choices open (Phase 3 files, `fn` alias,
`--gc`) as *decision points* to be ratified against the reference source and corpus before
implementation — the plan is choice-bearing, not rigid.
