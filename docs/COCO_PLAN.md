# Kirn Language — Complete Design & Implementation Plan

> **One-line vision:** Python's syntax, Go's compilation model, Rust/C++-level performance.
>
> **Implementation language:** C and modern C++ (the compiler is written in C++; the runtime is mostly C).

---

## Table of Contents

1. [Vision & Goals](#1-vision--goals)
2. [Non-Goals](#2-non-goals)
3. [Design Principles](#3-design-principles)
4. [Why the Old README Plan Was Rejected](#4-why-the-old-readme-plan-was-rejected)
5. [Language Specification](#5-language-specification)
6. [Memory Management — The Core Decision](#6-memory-management--the-core-decision)
7. [Concurrency Model](#7-concurrency-model)
8. [Error Handling Model](#8-error-handling-model)
9. [Foreign Function Interface (FFI) & Unsafe Code](#9-foreign-function-interface-ffi--unsafe-code)
10. [Compiler Architecture](#10-compiler-architecture)
11. [Implementing the Compiler in C/C++](#11-implementing-the-compiler-in-cc)
12. [Runtime Library Design (C)](#12-runtime-library-design-c)
13. [Standard Library Plan](#13-standard-library-plan)
14. [Tooling](#14-tooling)
15. [Testing Strategy](#15-testing-strategy)
16. [Benchmarking & Performance Targets](#16-benchmarking--performance-targets)
17. [Roadmap: Phases 0–8](#17-roadmap-phases-08)
18. [Versioning & Compatibility Policy](#18-versioning--compatibility-policy)
19. [Governance, Docs & Community](#19-governance-docs--community)
20. [Risks & Mitigations](#20-risks--mitigations)
21. [Comparison With Other Languages](#21-comparison-with-other-languages)
22. [Open Questions](#22-open-questions)

---

## 1. Vision & Goals

Kirn is a statically typed, ahead-of-time compiled, general-purpose systems-and-applications
language that *feels like Python* to write but produces fast native binaries.

| Goal | Meaning in practice |
|---|---|
| **Python-like syntax** | Indentation-based blocks, no braces/semicolons, `def` functions, readable one-liners, REPL-friendly |
| **Go-like compilation** | Single static binary output, near-instant startup, fast compile times (`kirn build` < 1s for medium projects), trivial cross-compilation, no VM/JIT required at runtime |
| **Rust/C++ performance** | Value semantics + stack allocation by default, zero-cost abstractions, monomorphized generics, LLVM optimization pipeline, no mandatory garbage-collection pauses on the hot path |
| **Safe by default** | Memory-safe subset by default; raw pointers only inside explicit `unsafe:` blocks; array bounds always checked unless proven/elided |
| **Simple toolchain** | One executable `kirn`: build, run, test, fmt, package manager — like `go`, not a zoo of tools |

**Elevator pitch:** *"Write it like Python. Ship it like C."*

---

## 2. Non-Goals

Being explicit prevents scope creep:

- **Not** dynamically typed; **not** adding runtime `eval`.
- **No** full Rust-style borrow checker in v1 (see §6 for the phased alternative).
- **No** interpreter-only mode as the primary story — AOT compilation is the product; an interpreter exists only as a dev/bootstrap tool.
- **No** C++-style template metaprogramming, ADL, or implicit conversions.
- **No** macros-as-syntax (no Lisp/Rust macro system in v1).
- **No** manual memory management as default (`malloc`/`free` are unsafe-only escape hatches).
- **Not** aiming to be a JVM-language competitor; we target native code.

---

## 3. Design Principles

1. **Readable > clever.** If a feature needs a paragraph to explain, cut it.
2. **No hidden costs.** Every allocation, copy, or indirection must be visible in the source (or provably elided). This is what makes C-level performance honest.
3. **Immutable by default.** Reassignment requires `var`. Enables safe concurrency and better optimization.
4. **Compile errors over runtime surprises.** Statically catch nil, wrong types, unused results of fallible calls.
5. **Fast compiles are a feature.** Grammar kept LALR-friendly, minimal header/metadata machinery, parallel compilation from day one.
6. **Escape hatches exist, but they're loud.** `unsafe:` blocks are greppable and reviewed.
7. **Python familiarity where free.** Use Python spelling (`def`, `elif`, `None`-like `none`, f-strings, slicing) whenever it doesn't cost safety or speed.

---

## 4. Why the Old README Plan Was Rejected

| Requirement | Old plan said | Problem | This plan |
|---|---|---|---|
| Python-like syntax | `fn main() {}`, `let mut`, braces | Rust-flavored, contradicts goal | Indentation-based grammar (§5) |
| Go-like compiled | "JIT compilation at runtime" | JIT = VM dependency, slow startup | Pure AOT → static binary (§10) |
| Rust-level perf + simplicity | "Ownership model like Rust" | Borrow checker ≠ simple | ARC + value semantics, phased ownership inference (§6) |
| Buildable roadmap | Generic compiler textbook | No decisions, no milestones | Concrete phases 0–8 with exit criteria (§17) |
| Toolchain | Vague | No CLI spec, no host language | `kirn` CLI + C++ implementation plan (§11, §14) |

---

## 5. Language Specification

### 5.0 Hello World

```python
# main.co
def main():
    name = "World"
    print(f"Hello, {name}!")
```

```bash
$ kirn run main.co     # compile + execute
$ kirn build main.co   # emit ./main native binary
```

### 5.1 Lexical Structure

- **Encoding:** UTF-8 source files, extension `.co`.
- **Blocks:** indentation-based, exactly like Python. The lexer emits virtual
  `INDENT` / `DEDENT` / `NEWLINE` tokens (same algorithm as CPython:
  stack of indent widths; tabs forbidden — 4 spaces enforced by compiler & formatter).
- **Comments:** `#` line comments. Docstrings are plain string literals first-in-body.
- **Identifiers:** `[A-Za-z_][A-Za-z0-9_]*`. Reserved words below.
- **Literals:** ints (`123`, `0xFF`, `0b1010`, `1_000_000`), floats (`3.14`, `1e-9`),
  strings `"..."` with `\n \t \\ \" \u{...}` escapes, raw strings `r"..."`,
  byte strings `b"..."`, chars `'A'`, bools `true`/`false`, unit `none`.
- **Keywords (v1):**

```
def    var    let    if     elif   else   while  for    in
return break  continue match  case   struct enum   trait
impl   import export pub    defer  spawn  chan   select
try    raise  unsafe extern  new    box    self   Self
and    or     not    is     as     true   false  none
```

> **Frozen (Phase 0):** this list is final for v1 — `go` was dropped
> (`spawn` covers it). Additions require an RFC + edition bump. See
> `grammar/coco.ebnf` §1 and §4 for the full freeze record.

(`let x = expr` = immutable alias/binding sugar; plain `x = expr` also binds immutably;
`var x = expr` declares mutable.)

- **Operators:** `+ - * / % ** // & | ^ << >> ~ < > <= >= == != = += -= *= /= etc.` plus
  `?` (error propagate), `.?.`(nil-safe access), ranges `..` (exclusive) and `..=` (inclusive).

### 5.2 Grammar (EBNF sketch)

Kept deliberately small; LL(1)-with-lookahead friendly so both the hand-written
recursive-descent parser and future tooling stay simple:

```ebnf
program        = { statement } ;
statement      = func_def | struct_def | enum_def | trait_def | impl_block
               | import_decl | var_stmt | expr_stmt | if_stmt | while_stmt
               | for_stmt | match_stmt | return_stmt | block_stmt ;
func_def       = "def" IDENT [ type_params ] "(" param_list ")" [ "->" type ] ":" block ;
block          = INDENT { statement } DEDENT ;
if_stmt        = "if" expr ":" block { "elif" expr ":" block } [ "else" ":" block ] ;
for_stmt       = "for" pattern "in" expr ":" block ;
match_stmt     = "match" expr ":" INDENT { "case" pattern [ "if" guard ] ":" block } DEDENT ;
pattern        = literal | IDENT | "_" | tuple_pat | enum_pat | binding_pat ["is" type] ;
type           = prim_type | IDENT [ "[" type_list "]" ] | "*" type | "&" type | "?" type
               | "(" type "," type ")" /* tuple */ | fn_type ;
expr           = assignment ; /* precedence climbing, Python-like levels */
```

Full machine-readable grammar ships as `grammar/coco.ebnf` and is tested against
the parser via golden files (§15).

### 5.3 Variables & Mutability

```python
x = 42              # immutable int (inferred), reassignment = compile error
var y = 10          # mutable
y += 1              # ok
z: f64 = 3.14       # optional explicit type annotation
const MAX = 1000    # compile-time constant
```

Rules:

- All bindings are **immutable by default**; mutation requires `var`.
  *(This is the single biggest departure from pure-Python feel — justified because
  it enables thread-safety guarantees and aggressive optimization. Formatter/LSP
  will suggest `var` automatically when you mutate.)*
- Types are **statically inferred** (bidirectional Hindley–Milner-flavored local inference).
- Shadowing in inner scopes allowed; unused variables are warnings, `_` discards.

### 5.4 Types

**Primitives:**

| Type | Meaning | Default ops |
|---|---|---|
| `int` | 64-bit signed (i64) | full arithmetic, checked overflow in debug, wrapping in release unless `@checked` |
| `i8 i16 i32 i64 u8 u16 u32 u64 usize isize` | fixed-width ints | explicit `as` casts only between widths |
| `f32 f64` | IEEE floats | NaN/inf per IEEE-754 |
| `bool` | `true`/`false` | no implicit int conversion |
| `char` | Unicode scalar value | |
| `string` | UTF-8 owned string | slicing, iteration by `char`/byte |
| `bytes` | owned byte buffer | |

**Compound types:**

```python
list[int]          # growable array
dict[string, int]  # hash map
set[string]        # hash set
(int, string)      # tuple
int?               # option: value or none  (sugar for option[int])
result[int, string]# success value or error (§8)
*int               # raw pointer — unsafe contexts only
&T                 # reference (borrowed, non-owning, read)
&mut T             # mutable borrow
box[T]             # heap-allocated owning pointer (single owner)
fn(int, int) -> int# function type
```

**No implicit narrowing conversions ever.** `as` performs checked casts
(traps on overflow in debug builds); `unsafe_as` does bit reinterpretation.

### 5.5 Functions & Closures

```python
def add(a: int, b: int) -> int:
    return a + b

def greet(name: string = "world"):            # default args
    print(f"hello {name}")

def sum_all(*nums: int) -> int:               # variadic
    total = 0
    for n in nums:
        total += n
    return total

square = def(x: int) -> int: return x * x     # anonymous fn expression
double = (x) => x * 2                          # short lambda sugar

def make_counter():
    count = 0
    return () => {                              # closure capturing `count` by ref-box
        count += 1
        return count
    }
```

- Functions are first-class values; closures capture by reference (heap-promoted only if escaped — capture analysis in §10).
- Every function has exactly one exit contract: normal `return` or `raise` (typed, §8).
- Tail calls optimized where possible; recursion fine but no TCO guarantee in v1 docs.

### 5.6 Control Flow

```python
if x > 0 and y < 10:
    ...
elif x == 0:
    ...
else:
    ...

for i in 0..10:            # 0..9
for i in 0..=10:           # 0..10 inclusive
for item in my_list:       # any iterable
    if item is none:
        continue
    break

while cond():
    ...

# expressions, not just statements:
label = if score >= 50: "pass" else: "fail"
values = for x in nums yield x * 2        # comprehension-style (phase 4)
```

### 5.7 Pattern Matching

```python
match value:
    case 0:
        print("zero")
    case n if n < 0:
        print("negative")
    case (a, b):                     # tuple destructuring
        print(a, b)
    case Point(x: 0, y):
        print("on axis", y)
    case Some(v) if v > 100:
        print("big", v)
    case _:
        print("other")
```

Patterns cover literals, bindings, tuples, enums, structs, guards, `is` type tests,
ranges (`case 1..=9:`). Exhaustiveness is **checked at compile time** for enums/options.

### 5.8 Structs, Enums, Traits

```python
struct Point:
    x: f64
    y: f64 = 0.0                    # field default

    def magnitude(self) -> f64:     # methods live inside the struct
        return sqrt(self.x * self.x + self.y * self.y)

p = Point(x: 3.0)                   # named-argument construction
print(p.magnitude())

enum Shape:
    Circle(radius: f64)
    Rect(w: f64, h: f64)
    Point                            # unit variant

def area(s: Shape) -> f64:
    match s:
        case Circle(r): return 3.14159 * r * r
        case Rect(w, h): return w * h
        case Point:   return 0.0

trait Drawable:
    def draw(self)

impl Drawable for Point:
    def draw(self):
        print(f"({self.x}, {self.y})")

def render(d: Drawable):            # dynamic dispatch (trait object)
    d.draw()
```

Semantics:

- **Structs have value semantics**: assignment copies (like C/Go), stored inline
  in parents/arrays → cache-friendly, zero hidden heap traffic.
- **Enums are tagged unions**, laid out compactly (tag + largest payload), matched exhaustively.
- **Traits** = interfaces. Static dispatch via generics (monomorphization) is default;
  trait objects give vtable dispatch only when you ask (`Drawable` as parameter type).
- No inheritance of structs (composition only) — this avoids C++'s worst complexity
  while keeping polymorphism through traits.
- Operator overloading only via well-known traits (`Add`, `Index`, `Iterator`, …),
  never arbitrary methods.

### 5.9 Generics

Subscript syntax matches modern Python typing (`list[int]`) — familiar and unambiguous:

```python
def first[T](items: list[T]) -> T:
    return items[0]

struct Pair[A, B]:
    first: A
    second: B

trait Hashable:
    def hash(self) -> u64

def bucket[T is Hashable](item: T) -> u64:   # trait bounds
    return item.hash() % 256
```

- **Monomorphization** (like C++ templates / Rust generics): each concrete instantiation
  gets specialized machine code → zero runtime overhead.
- Trait bounds are checked **at definition site** (unlike C++ templates which check at
  instantiation) → error messages point at your code, not 40 levels deep.
- Compile-time dedup of identical instantiations keeps binaries lean.

### 5.10 Modules & Visibility

```python
import math                       # stdlib module
import json                       # stdlib
from utils.string import slugify  # named import

# utils/string.co
def _internal_helper(): ...       # leading underscore = module-private
pub def slugify(s: string) -> string: ...   # pub = exported
```

- File = module; directory = package with `__init__.co` (or `coco.toml` manifest).
- No headers, no forward declarations: compiler does two-pass resolution like Go.
- Visibility: `pub` (exported), `_`-prefixed (private), default = package-visible.

---

## 6. Memory Management — The Core Decision

This is where "simple as Python + fast as C" lives or dies. Full Rust borrow checking was
rejected (it's the opposite of Python-simple). Full stop-the-world GC was rejected
(it caps determinism and hurts the C-perf claim). The chosen model:

### 6.1 The Chosen Model: Value Semantics + Escape Analysis + ARC

1. **Everything is a value by default.**
   Structs/enums/tuples/arrays-of-values live on the **stack** or inline inside their parent.
   Assignment copies bytes (memcpy) — exactly like C. No hidden heap, no hidden pointers.

2. **Heap allocation is explicit and visible.**
   You touch the heap only via `box[T]`, closures that escape, growing collections,
   or strings beyond SSO (small-string optimization ≤ 23 bytes inline).

3. **Escape analysis (compile time).**
   Values that don't outlive their frame never hit the heap — even boxed-looking ones.
   Most real code becomes pure stack code.

4. **Atomic Reference Counting (ARC) for shared heap objects** — Swift's model:
   - Deterministic destruction (no pauses, predictable latency — critical claim vs GC languages).
   - Retain/release pairs are elided aggressively by the optimizer (borrow-scoped analysis);
     measured cost on typical workloads ≈ 2–8% vs fully-manual C.
   - Reference cycles leak → shipped with a concurrent **cycle collector** (background,
     incremental, sub-millisecond slices; opt-out per type with `weak` refs).

5. **`weak` references** break cycles explicitly:
   ```python
   struct Node:
       var parent: Node? 
       weak var child: Node?
   ```

6. **Arena/region allocators** as first-class library (`mem.Arena`) for bulk patterns
   (per-frame game allocs, per-request server allocs) — freed in O(1), zero per-object cost.

7. **Phased upgrade path (the honesty clause):**
   - v0.x–v1.0: model above ships and is enough for the perf targets (§16).
   - v1.x: add **ownership inference** — the compiler proves single-owner graphs and strips
     retain/release entirely for those paths (this is research-grade but incremental:
     it starts as a warning-free optimization, never a compile error, so it can't fragment users).
   - Always available: `unsafe:` + raw pointers + custom allocators for the last 5%.

### 6.2 Alternatives Considered (and why rejected)

| Option | Pros | Cons | Verdict |
|---|---|---|---|
| Tracing GC (Go-style) | Simplest user model, battle-tested | Pauses hurt p99 latency; memory bloat 2–5×; undermines "C-class perf" claim | Rejected as default |
| Full borrow checker (Rust-style) | Zero runtime cost, maximal safety | Learning cliff contradicts core goal; slows ecosystem bootstrapping | Deferred as opt-in lint far-future |
| Manual malloc/free (C-style) | Absolute control | Unsafe by default; contradicts safety goal | unsafe-only |
| **Value + EA + ARC (chosen)** | Simple, deterministic, near-C, incremental path to zero-cost | RC cycles need collector; atomic RMW cost until ownership inference lands | **Chosen** |

### 6.3 Safety Rules (even before/without borrow checking)

These are cheap static checks shipping in v1 — they prevent ~90% of real bugs:

- No returning references/pointers to locals (escape analysis makes this a hard error).
- Borrows (`&T`) are non-owning and region-tracked within a function body;
  mutating a collection while iterating it = compile error (iterator invalidation check).
- Data races impossible across threads without explicit sharing annotations (§7):
  values crossing `spawn` must be `sendable` (deeply immutable or `var`-free +
  no raw pointers) — checked at compile time, Go/Hacker's-Delight style, no runtime cost.
- Bounds checks on all indexing; optimizer eliminates them where provable
  (range analysis + induction variables); `unsafe_get(list, i)` is the loud bypass.

---

## 7. Concurrency Model

Go's ergonomics, minus the GC tax:

```python
def worker(id: int, jobs: chan[int], results: chan[string]):
    for j in jobs:
        results.send(f"worker {id} done {j}")

def main():
    jobs = chan[int](cap: 100)
    results = chan[string]()

    for i in 1..=3:
        spawn worker(i, jobs, results)     # lightweight green thread

    for j in 1..=5:
        jobs.send(j)
    jobs.close()

    for _ in 1..=5:
        print(results.recv())              # blocking receive

    # select for multiplexing:
    select:
        case msg = results.recv(): print(msg)
        case <-time.after(1s):      print("timeout")
```

Design points:

- **M:N scheduler** in the C runtime: goroutine-style stacks (2 KB initial, grows),
  work-stealing across cores, async syscalls integrated (epoll/kqueue/IOCP).
- Millions of spawns feasible; `spawn` returns a join handle (`h.join()` / `h.wait()`).
- **Channels**: typed, buffered or unbuffered; `select` for fan-in; close semantics like Go.
- **Shared memory is opt-in and checked**: `sendable` bound (§6.3) makes cross-thread
  data race-free at compile time for the common cases; deliberate sharing uses
  `sync.Mutex[T]` (data attached to its lock) or atomics (`atomic[int]`).
- **`defer`** (Go-style) for cleanup — runs at scope exit, LIFO:
  ```python
  f = open("data.txt")
  defer f.close()
  ```
- Structured concurrency helpers in stdlib: `concur.group()` (wait-all, cancel-on-error),
  context cancellation tokens.
- `async/await`: **deferred to v2** — M:N channels cover 95% of use cases with simpler
  semantics; colored-functions problem avoided.

---

## 8. Error Handling Model

Exceptions were rejected (invisible control flow, terrible for optimization, unpredictable
perf — the anti-C++ lesson). Result types win, with Python-grade ergonomics:

```python
def parse_port(text: string) -> result[int, ParseError]:
    port = text.to_int()
    if port is none or port < 0 or port > 65535:
        raise ParseError(bad: text)      # constructs Err(...) early-return
    return port                           # auto-wraps Ok(port)
```

Rules:

- Fallible functions return `result[T, E]`; the compiler **warns/errors on ignored results**
  (`@discardable` opts out for things like `write` where ignoring is common).
- `try expr` (or trailing `?`) propagates the error upward — sugar for the match-and-return:
  ```python
  def run() -> result[none, io.Error]:
      data = try read_file("config.toml")
      cfg  = try toml.parse(data)
      return none
  ```
- **Panics** exist for programmer bugs only (assert failures, index-out-of-bounds in
  unrecoverable spots): unwind + backtrace + abort. `panic(msg)` is explicit;
  `catch_panic(fn)` boundary exists for FFI/server harnesses.
- Errors are ordinary values: wrap with context (`err.context("loading config")`),
  compose, `match` on them. Standard error trait gives `message()` + `source()` chain.
- Checked exceptions interop (Java-style) is unnecessary: FFI errors surface as results.

Why this beats exceptions for our goals: every fallible call is visible at the call site
(readability), control flow stays straight-line (optimizer-friendly, precise costs),
and error paths are data flow the compiler can schedule like any other branch.

---

## 9. Foreign Function Interface (FFI) & Unsafe Code

Calling C/C++ is a first-class requirement ("leverage 50 years of libraries"):

```python
# Declare external symbols — linked at build time
extern def printf(fmt: *char, ...) -> i32

# Wrap once, safely:
def c_printf_safely(msg: string):
    unsafe:
        printf(c"%s\n", msg.c_ptr())    # c"" literal is NUL-terminated
```

- `extern def` supports variadics, custom calling conventions (`extern "system"`), structs
  with explicit layout (`@packed`, alignment attributes).
- **Bindgen companion tool** (`kirn bindgen header.h`) generates declarations from C headers —
  same role cargo-bindgen plays for Rust. C++ headers: name-mangled extern support in phase 6.
- `unsafe:` blocks unlock: raw pointers (`*T`), pointer arithmetic, `unsafe_as`,
  `malloc`/`free`/custom allocators, `unsafe_get` unchecked indexing.
- Safety invariant documented in one sentence: *"Inside `unsafe:`, YOU uphold memory rules
  the compiler normally enforces; everything outside is guaranteed safe."*

---

## 10. Compiler Architecture

### 10.1 Pipeline

```
 .co sources
     │
     ▼
┌─────────┐  tokens (+INDENT/DEDENT)   ┌──────────┐   AST    ┌─────────────┐
│  Lexer   │ ─────────────────────────▶ │  Parser  │ ───────▶ │ Name resolve │
│ (C++)    │                            │ (C++,    │          │ + Type check │
└─────────┘                             │ recurs.  │          │ + Inference  │
                                        │ descent) │          │ + Capture/EA │
                                        └──────────┘          └──────┬──────┘
                                                                     │ typed AST
                                                                     ▼
                     ┌──────────────┐   MIR (SSA)   ┌──────────────┐
                     │  LLVM IR gen │ ◀──────────── │  Lowering to  │
                     │  (C++ API)   │               │  MIR + opt    │
                     └──────┬───────┘               │  passes       │
                            │                       └──────────────┘
                            ▼
                  LLVM optimization pipeline (O1/O2/O3, PGO/LTO-ready)
                            │
                            ▼
                native objects ──link──▶ single static executable
```

### 10.2 Stage Details

| Stage | Responsibility | Key algorithms |
|---|---|---|
| **Lexer** | Bytes → tokens; INDENT/DEDENT synthesis; string interpolation desugars here | CPython indent-stack algorithm; O(n) single pass |
| **Parser** | Tokens → AST; best error messages of the whole pipeline | Hand-written recursive descent (LL(k)), error-recovery sync sets, expected-vs-found diagnostics with fix-it hints |
| **Resolve/Check** | Two-pass name resolution (collect then bind, Go-style — kills header problem); type inference; trait bounds; exhaustive match; sendability; iterator invalidation; capture analysis feeding escape analysis | Bidirectional HM subsets, constraint solving, union-find for type vars |
| **Lowering** | Typed AST → MIR (mid IR, SSA form): explicit drops/retains inserted here, closures → structs+vtables, generics monomorphized, bounds-check insertion | Dominator-tree SSA construction (Braun et al.), insert-before-optimize strategy for ARC elision |
| **MIR passes** | Cheap, language-aware opts before LLVM: ARC pair elision, bounds-check elimination, devirtualization of trait calls, closure-to-context flattening, common-subexpr, const-prop | Classic dataflow + side-effect analysis |
| **Codegen** | MIR → LLVM IR; drives LLVM passes; emits DWARF/PDB debug info; LTO/PGO wiring | LLVM C++ API (we're in-process — no binding layer needed since compiler is C++) |
| **Driver** | Parallel compilation units, incremental caches (AST+MIR keyed by content hash), cross-module inlining via serialized MIR, linker invocation (lld for static/fat binaries) | Work-stealing thread pool, sccache-style artifact store |

### 10.3 Why LLVM (and alternatives)

| Backend | Verdict | Reason |
|---|---|---|
| **LLVM (chosen)** | ✔ | C-level codegen quality today; we get Clang-grade optimizer for free; C++ API is native to our host language; proven at Rust/Swift/Zig scale |
| Cranelift/MIR-lite | Later, optional | Fast-debug-build backend if compile-time becomes pain point (Rust does this) |
| Custom backend | Never (v1–v3) | Multi-year distraction; revisit only if licensing/targets demand |
| GCC backend | No | C-coupled, hostile embedding API |

Cross-compilation: LLVM target triples + lld + sysroot packaging →
`kirn build --target aarch64-linux-gnu` works day one on any host (same trick Zig popularized).

### 10.4 Interpreter (bootstrap & REPL)

Phase 1 ships a **tree-walking interpreter over the same AST** (~10× simpler than a VM,
fine for correctness work and REPL). Phase 3 optionally upgrades to bytecode+C loop VM
for a faster REPL — decision deferred until profiled. The interpreter is also our
differential-testing oracle (§15) forever.

---

## 11. Implementing the Compiler in C/C++

### 11.1 Host-Language Decision

| Candidate | Pros | Cons | Verdict |
|---|---|---|---|
| **Modern C++ (chosen)** | LLVM is a C++ API — direct, zero-binding friction; total perf control; huge talent pool; dogfoods the perf promise | Footgun-prone; needs discipline | **Chosen, with guardrails below** |
| C | Runtime-grade simplicity, ABI king | No RAII → compiler dev velocity tanks (string/vector mgmt everywhere); LLVM embed still needs C++ | Runtime + hot utilities only |
| Rust | Safety, great LLVM bindings | Team/toolchain learning curve; slower iteration early; ironic dependency | Rejected for v1 pragmatism |
| Go | Fast to write | GC pauses in compiler itself; LLVM via cgo = pain | Rejected |
| Zig | Great cross story | Pre-1.0 churn risk | Watch-list |

**Split:** `frontend + driver + codegen` in **C++20/23**; `libcoco_rt` runtime in **C11**
(plus tiny asm bits) so the runtime links cleanly into every produced binary regardless
of host-toolchain politics.

### 11.2 C++ Guardrails (coding standard deltas for the compiler codebase)

- C++20 core; **no exceptions** (`-fno-exceptions`) → `Result<T,Diag>` internally (dogfooding!);
  no RTTI; no iostreams (custom diag renderer instead).
- Ownership spelled out: `std::unique_ptr` default, arenas (`llvm::BumpPtrAllocator` style)
  for AST/MIR nodes (freed wholesale per stage — compilers love arenas).
- Banned: raw `new/delete`, shared_ptr outside driver caches, implicit conversions, macros-for-logic.
- clang-tidy + `-Wall -Wextra -Werror -fsanitize=address,undefined` in CI debug builds.
- Formatting: clang-format config committed; 100-col; snake_case types PascalCase.

### 11.3 Repository Layout (compiler repo)

```
coco/
├── grammar/coco.ebnf           # normative grammar
├── src/
│   ├── lex/                    # lexer, token defs, indent engine
│   ├── ast/                    # node defs (arena-allocated), printer
│   ├── parser/                 # recursive descent + diagnostics
│   ├── sema/                   # resolve, infer, checks, capture/EA
│   ├── mir/                    # MIR def, SSA builder, passes
│   ├── codegen/                # LLVM IR emission
│   ├── driver/                 # kirn CLI entry, job graph, caching
│   ├── interp/                 # tree-walk interpreter
│   └── support/                # diag engine, arena, hashing, paths
├── rt/                         # libcoco_rt (C11): sched, chan, arc, panics
├── stdlib/                     # std modules written IN KIRN (§13)
├── tests/                      # §15 layout
├── tools/                      # fmt, lsp, bindgen, bench-runner
├── third_party/{llvm,fmt,utf8proc,...}
├── CMakeLists.txt              # + presets; Ninja recommended
└── docs/
```

### 11.4 Build System & Dependencies

- **CMake ≥ 3.25** + Ninja; CI matrices: Linux (gcc/clang), macOS, Windows (MSVC + clang-cl).
- LLVM: link against **prebuilt releases first** (v18+); vendored-source build only in
  release-pipeline CI. Keep `LLVM_*` behind `src/codegen` walls so backend swap remains possible.
- Vendored deps minimized: fmt, utf8proc, xxHash, (optional) mimalloc for compiler itself.

---

## 12. Runtime Library Design (C)

Every Kirn binary embeds `libcoco_rt` (~50–150 KB):

| Component | Notes |
|---|---|
| Scheduler | M:N green threads; 2 KB growable stacks (guard pages); work-stealing deque; IO integration epoll/kqueue/IOCP |
| Channels | Lock-free MPSC fast path, blocking slow path; select via registered waiters |
| ARC support | `rt_retain/rt_release` (pause-thread variant for young objects), cycle collector: incremental mark-sweep over candidate subgraphs, budgeted slices |
| Panics | Unwinder (table-based, DWARF .eh_frame), backtrace symbolizer, abort hook |
| Allocators | malloc wrapper default; pluggable vtable → arenas, pool, mimalloc/jemalloc drop-ins |
| Strings/interning | SSO helper, UTF-8 validation fast path (SIMD later) |
| Time/env/fs shims | Thin portable layer over OS APIs used by prelude |

Prelude (auto-imported basics: `print`, `list`, `chan`, `spawn`…) maps onto this runtime.

---

## 13. Standard Library Plan

Written **in Kirn itself** wherever possible (dogfooding):

| Module | Contents | Phase |
|---|---|---|
| `core` (prelude) | Option/result, iterators/lazy views, ranges, formatting (`f`-strings engine) | 2–3 |
| `collections` | list, dict, set, deque, ringbuf, btree, heap, bitset | 3 |
| `io` | Files (buffered, mmap), paths, tempdirs | 3 |
| `os` | env, args, process spawn/pipes, signals | 3 |
| `net` | TCP/UDP sockets, HTTP/1.1 client+server, URL parsing | 5 |
| `concur` | sync primitives, atomics, worker pools, structured groups | 4–5 |
| `text` | Unicode segmentation, regex (RE2-style linear engine), JSON encode/decode | 5 |
| `math` | numeric tower helpers, statistics, PRNG (xoshiro), SIMD intrinsics wrappers | 4 |
| `time` | monotonic/wall clocks, durations, timers, tz-lite | 4 |
| `serialize` | derive-based JSON/TOML/CBOR via reflection-at-compile-time (trait derivation, not runtime reflection) | 6 |
| `test` | built-in framework powering `kirn test` (assert_eq, fixtures, benches) | 3 |

Non-negotiable rule: stdlib adds **zero runtime deps** beyond libcoco_rt/libc; networking
may shell to nothing — pure socket APIs.

---

### 13.1 Kirn ships **no built-in pets** — all std pets live on GitHub, `kirn setup` installs them globally

- **Author decision (2026):** Kirn does **not** contain any built-in pets. There is no
  bundled/embedded std library in the binary, and no std pets are hard-coded into the
  compiler output. The "dogs" analogy is dropped entirely — no pre-installed pets ship
  with the toolchain.
- **All std pets are pushed to GitHub** (the pets registry — see §14.2 "Package manager"
  and §7/§10.5 identities; registry host is the `pets-registry` GitHub org, website
  `https://pets-registry.github.io`). Each std pet (`core`, `collections`, `io`, `os`,
  `net`, `concur`, `text`, `math`, `time`, `serialize`, `test`) is published as its own
  repo in that org so it can be fetched/installed by URL, org+name, or org+name@version —
  exactly like any third-party pet. There is **no special stdlib fetch path** distinct
  from the normal package manager: "std" is just the set of pets the Kirn org curates.
- **`kirn setup` auto-installs the std pets globally.** When a developer runs `kirn setup`
  (first-time init or after install), Kirn:
  1. reads the pinned std-pet set from the registry lock metadata (`kirn.lock` / the
     `[pets]` manifest section),
  2. fetches each std pet **into the system-wide pet store** (global, under
     `~/.kirn/pets` — available on any path, any project, any directory on the machine),
  3. records a global `<std-pet>@<version>` snapshot so all std pets become resolvable
     from **anywhere** (`import pet.io*;` works in `/tmp/x.kn` as well as in a project).
  This is the "usable anywhere, globally, std-only" guarantee: only the **std** pets are
  auto-installed machine-wide; third-party pets stay scoped to their project's `pets/` as
  in §14.2. Re-running `kirn setup` is idempotent — it re-fetches/updates the std set to
  the pinned registry tags without touching project-local pets.
- **Rationale:** keeps the language binary tiny and registry-first (Go philosophy), makes
  the ecosystem the single source of truth for std, and gives new machines a one-command
  `kirn setup` bootstrap that lands the whole standard library in the global pet store.

---

## 14. Tooling

All subcommands ship inside the one `kirn` binary (Go philosophy):

| Command | Function |
|---|---|
| `kirn run x.co [-- args]` | compile (cached) + exec |
| `kirn build [-O0..O3] [--target t] [-o out]` | produce binary/static-lib |
| `kirn check` | full front-end only — IDE-speed diagnostics |
| `kirn test [path]` | discovers `test "name" { }` blocks, parallel runner |
| `kirn bench` | criterion-style statistical benches (§16) |
| `kirn fmt` | opinionated formatter (gofmt spirit: zero config) |
| `kirn doc` | doc comment extraction → static HTML |
| `kirn i` / `kirn install <pets>@<tag\|commit>` | package manager fetch: registry name, `user/repo`, or third-party full URL (see §14.2) |
| `kirn update [pets name]` / `.` | update pets — `.` = default **all** (§14.2) |
| `kirn sync` | reconciles manifest ↔ lock ↔ installed (replaces `kirn add`; npm-install / go-mod-tidy spirit). |
| `kirn info <pets>\|<user>/<repo>\|full-url` | print a pet's config info to terminal / website (§14.2) |
| `kirn doctor` | syntax + type check (`kirn check` + negative/types harnesses) + registry/install sanity |
| `kirn new <project>` / `kirn new pets <name>` | init project/pets template + auto Auto Build & Release workflow (§14.2) |
| `kirn setup` | one-time machine init: global defaults + auto-install std pets (§13.1) |
| `kirn self-update` | fetch & install the latest `kirn` release (§14.2) |
| `kirn repl` | interpreter-backed REPL w/ multiline, completion |
| `kirn bindgen header.h` | C header → Kirn extern decls |

### 14.2 Package manager, pets registry & the ecosystem

- **GitHub orgs (rkriad585 has full access to both):** the language org
  `https://github.com/kirn-lang` (the compiler repo `kirn-lang/kirn` + the `kirn-lang` GitHub
  Pages landing) and the pets registry org `https://github.com/pets-registry` (the registry
  repo + every std/third-party pet repo). A dedicated **registry repo is created on the
  `pets-registry` org** (the pets index, same role the `coco-lib` registry repo played for
  the old COCO ecosystem) — it hosts the machine-readable pet index (`registry.toml`) that
  powers name→repo resolution.
- **Websites:** full-identity landing `https://kirn-lang.github.io`; **docs / learn-Kirn-
  syntax** site `https://rkriad585.github.io/kirn`; **pets landing** `https://pets-registry.github.io`
  where developers search pets or learn how to author/`kirn new pets <name>`. (The old
  `coco_pets` branding is dropped — the library term is `pet`/`pets`, not `coco_pets`.)
- **`kirn setup` — global defaults, asked once, remembered.** First-time `kirn setup`
  prompts for global project defaults: **package name, version, license, author, website
  URL (optional), GitHub URL (optional)** — the GitHub URL accepts any of
  `[https://]github|gitlab|codeberg.com/some_user` (repo name auto-inferred from the project
  or pets name: `[https://]github|gitlab|codeberg.com/<user>/<project|pets>`) plus an
  optional **branch** (default `main`). These are stored globally (`~/.kirn/kirn.toml`,
  `[global]` section). Kirn **does not re-ask** once global info is set; the user must pass
  `kirn new --setup` to re-run/re-edit the global defaults. `kirn setup` also auto-installs
  the std pets globally (idempotent — §13.1).
- **`kirn i` / `kirn install <pets>@<tag\|commit>`** fetches a pet by:
  1. **registry name** (`kirn i json`) — resolved via the `pets-registry` index;
  2. **user/repo** (`kirn i someuser/some_kirn_pets_repo`) — no URL prefix needed;
  3. **third-party git services by full URL** (`kirn i [https://]gitlab.com/someuser/some_kirn_pets`)
     — auto-{fetch,install} from any git host. Version is optional (`@<semver tag>` or
     `@<commit id>`); default = default branch (`main`).
- **`kirn i`, `kirn install <pets>@<tag\|commit>`** — see above; tag/commit form always
  allowed. `kirn i` is the short form.
- **`kirn update [pets name]`** updates a single pet; `kirn update .` (or bare `kirn update`)
  updates **all** registered pets to the latest within their range.
- **`kirn sync`** is the new consolidate command (replaces `kirn add`): reads the manifest
  (`kirn.toml`) + lockfile, installs/removes what changed, rewrites `kirn.lock` — the
  npm-install / go-mod-tidy reconciliation step.
- **`kirn info <pets>|<user>/<repo>|full-url`** fetches the pet's config (name, version,
  license, author, website, GitHub URL) and prints it to the terminal or renders it on the
  registry website.
- **`kirn doctor`** runs syntax + type checking (reuses the negative/types harnesses) and
  health-checks the registry/installer/global config.
- **`kirn new <project>`** and **`kirn new pets <petsname>`** scaffold a template. A pets
  template auto-generates a GitHub **Auto Build & Release workflow** (tag-triggered): on a
  pushed `v*` tag the workflow runs `kirn build pets` and publishes a release artifact so
  anyone can `kirn i <petsname>` (registry), `kirn i someuser/some_kirn_pets_name`, or
  `kirn i [https://]gitlab.com/someuser/some_kirn_pets` to install it.
- **Cross-platform, rust-style one-shot installers** (tag-triggered from the same Auto Build
  & Release repo, no extra setup / no config prompts):
  Windows PowerShell `irm https://raw.githubusercontent.com/rkriad585/kirn/main/installer.ps1 | iex`,
  POSIX `curl -fsSL https://raw.githubusercontent.com/rkriad585/kirn/main/installer.sh | sh`.
  Every installer is built by the Auto Build & Release workflow and published as a tagged
  GitHub release; `kirn self-update` fetches and applies the newest release on any platform.

**LSP server** (`tools/lsp`, speak JSON-RPC): completions, hover types (inference reuse!),
go-def/find-refs, rename, semantic highlight, inline errors — this is the single highest-
leverage adoption tool after the compiler (scheduled in Phase 6).

**k-docs — inline docs for Kirn (pydoc/godoc spirit, new syntax):** a dedicated
`COCO_KIRN_DOCS_PLAN.md`-style plan tracks the k-doc comment syntax (`///` block + Kirn-
specific directive markers), extraction into the `kirn doc` pipeline, and rendering to
static HTML on the docs site (`https://rkriad585.github.io/kirn`) — see `COCO_PLANS/`. This
is analogous to pydoc/godoc but with Kirn-native comment syntax; scheduled alongside
Phase 6 `kirn doc`.

**LSP server** (`tools/lsp`, speak JSON-RPC): completions, hover types (inference reuse!),
go-def/find-refs, rename, semantic highlight, inline errors — this is the single highest-
leverage adoption tool after the compiler (scheduled in Phase 6).

Debugger story v1: emit DWARF/PDB (codegen stage) → gdb/lldb/VS work for stepping;
custom debug console deferred.

---

## 15. Testing Strategy

| Layer | Technique |
|---|---|
| Unit (C++) | GoogleTest-style per-stage tests; lexer/parser golden files |
| Grammar conformance | `grammar/*.ebnf` ↔ parser round-trip corpus |
| **Differential** | Interpreter vs compiled output on the entire test suite + fuzz corpora — outputs must match bit-for-bit |
| End-to-end | `tests/cases/**/*.co` each with expected stdout/stderr/exit-code; runner compares |
| Error-message tests | Diagnostic snapshot tests (message text, span, fix-it) |
| Fuzzing | libFuzzer on lexer/parser/sema (AST-shape-aware mutators later); OSS-Fuzz integration in Phase 6 |
| Sanitizers | ASan/UBSan builds run full suite nightly; MSan for frontend |
| Performance regression | Bench suite gated in CI with ±5% alert threshold |
| Stdlib | Property-based tests (rapidcheck-style) for containers/string algorithms |

Coverage gates: frontend ≥ 85% line coverage enforced from Phase 3 onward.

---

## 16. Benchmarking & Performance Targets

Methodology: hyperfine for wall-clock, perf/VTune profiles for hotspots, fixed hardware
matrix in CI runners (bench on bare metal, never shared CI VMs). Suites:

1. **Micro:** fib(35), nbody, mandelbrot, matmul, sha256, json-parse, string-concat loops.
2. **Macro:** HTTP server req/s (net stdlib), compiler-bootstrapping speed, raytracer frame.
3. **Latency:** p99/p999 under sustained load (this is where no-GC-pauses pays off visibly).

Targets at v1.0 (relative to same-machine baselines):

| Baseline | Target |
|---|---|
| C (clang -O2) | within 1.2× on compute micro-benchmarks |
| Rust | parity ±10% typical |
| Go | faster than Go on ≥70% of suites; better p99 tail latency |
| C++ | parity ±10% (ARC overhead offset by EA/value semantics) |
| Python (CPython 3.12+) | 30–100× on compute-bound code |

Honesty policy: publish every number including regressions; benchmark scripts versioned
next to compiler (`tools/bench`).

---

## 17. Roadmap: Phases 0–8

Each phase has **exit criteria**; no phase starts before the previous exits.

| Phase | Deliverables | Exit criteria | Est. effort |
|---|---|---|---|
| **0. Spec freeze (v0.1)** | This doc ratified; `grammar/coco.ebnf` complete; 30 example programs that must parse someday | 3 independent people can hand-execute examples unambiguously | 2–4 wks |
| **1. Frontend + interpreter** | Lexer, parser, resolver/inference, tree-walk interpreter, `kirn repl`, diag engine | All Phase-0 examples run correctly in interpreter; error messages have spans+fix-its | 2–3 mo |
| **2. MIR + codegen skeleton** | SSA MIR, lowering for core subset, LLVM emission, `kirn build` produces hello-world binary linking libcoco_rt (scheduler stubbed) | fib/nbody run compiled & match interpreter bit-for-bit | 2 mo |
| **3. Core language complete** | Generics/monomorphization, traits static+dynamic, match exhaustiveness, ARC runtime + cycle collector, bounds checks + elision, `test` framework | Suite §15 green incl. sanitizers; differential testing automated in CI | 3 mo |
| **4. Concurrency + collections** | Full scheduler, channels/select, sendability checker, defer, collections+iterators, io/os/time/math | 10k-goroutine web crawler demo stable; ASan-clean | 2–3 mo |
| **5. Stdlib + net + tooling** | net/text/serialize modules, `kirn fmt`, `kirn get` MVP + 20 published demo packages, bench infra online | Real project (JSON REST API server) builds & serves load with good p99 | 2–3 mo |
| **6. Ecosystem hardening** | LSP server, bindgen, fuzzing/OSS-Fuzz, incremental compilation caches, Windows polish, packages site | Third-party contributors land features unassisted; LSP in VS Code marketplace | 3 mo |
| **7. v1.0** | Spec v1.0 frozen, semver guarantee, installers (brew/scoop/apt/docker), website+book, governance seated (§19) | 90-day freeze with zero spec changes; 25+ real external projects | 2 mo |

Total realistic timeline for a small dedicated team: **~18–24 months to v1.0**
(aggressive solo-dev: 24–36 months). Cut scope, not quality bars.

---

## 18. Versioning & Compatibility Policy

- **Semver** for compiler AND language: breaking language changes require major bump.
- Edition system (like Rust/C++): `edition = "2027"` in `coco.toml` lets old code keep compiling
  while new editions clean up warts; migrations automated by `kirn fmt --migrate`.
- Deprecation ladder: warn (≥2 minor releases) → error in next edition only.
- Stdlib stability: anything exported from stdlib follows semver too; experimental modules
  namespace `x/` explicitly unstable.

---

## 19. Governance, Docs & Community

- **Governance v1:** BDFL (project founder) + RFC process: any language change needs a
  numbered RFC (template in `rfcs/`), 2-week comment window, recorded decision + rationale.
  At 50+ contributors: elect a 5-person language council; BDFL retains tie-break only.
- **Docs:** The Kirn Book (mdBook) generated in CI; stdlib docs via `kirn doc` published
  per release; playground (wasm-compiled interpreter) embedded in website.
- **Community:** GitHub Discussions for Q&A, RFCs for design, Discord for chat;
  issue templates already in repo — extend with `rfc`, `stdlib-proposal`.
- Fix repo inconsistencies found during review: unify canonical URL to `rkriad585/coco`,
  replace placeholder emails/Discord links in CONTRIBUTING.md, delete dead `index.md` links.

---

## 20. Risks & Mitigations

| Risk | Impact | Mitigation |
|---|---|---|
| Scope creep (macros, async, borrow-checker demands) | Timeline death | Non-goals list (§2) enforced via RFC gate; every addition must cite a principle (§3) |
| ARC cycles/perf edge cases embarrass benchmarks | Credibility | Cycle collector from day 1; publish honest numbers (§16); ownership inference roadmap (§6.1.7) |
| LLVM compile times hurt "fast builds" goal | Adoption | Incremental caches, parallel driver, thin-LTO only in release; Cranelift fallback studied in Phase 6 |
| Solo-founder bus factor | Project death | RFC record, public roadmap, welcome-first-contributor issues tagged from Phase 1 |
| Windows toolchain pain (paths, FS semantics, debug info) | Half-broken platform | MSVC CI from Phase 1, not bolted on later |
| Ecosystem cold start | Nobody comes | Bindgen + C-interop means users bring their favorite C libs on day one — market THIS |

---

## 21. Comparison With Other Languages

| Dimension | Kirn | Python | Go | Rust | C++ |
|---|---|---|---|---|---|
| Typing | static, inferred | dynamic | static, inferred | static, inferred | static |
| Syntax feel | indentation, Pythonic | ★★★★★ | braces | braces | braces |
| Compilation | AOT native | interpreted (+JIT opts) | AOT native | AOT native | AOT native |
| Startup | ~ms | ~50ms+ | ~ms | ~ms | ~ms |
| Memory model | value + EA + ARC | tracing GC | tracing GC | ownership | manual |
| Pause times | deterministic-ish | yes | sub-ms STW | none | none (you wish) |
| Concurrency | goroutines+channels | GIL threads/async | goroutines | async+fearless | threads+chaos |
| Generics | monomorphized, bounded | N/A | runtime-shaped | monomorphized | templates |
| Errors | result + try/? | exceptions | values (if err != nil) | Result + ? | exceptions/codes |
| Null safety | `?` types, no null derefs | None everywhere | nil panics | Option | UB |
| Package mgr | built-in `get` | pip (external) | built-in | cargo | none canon |
| Interop with C | extern def + bindgen | ctypes/cffi | cgo | ffi crate | native |

Positioning sentence: **"Go's workflow, Python's face, C's speed, minus each one's famous flaw."**

---

## 22. Open Questions

Tracked here until resolved by RFC (each blocked item names its decider phase):

1. Integer overflow semantics final call: checked-always vs checked-debug/release-wrapping? (Phase 0)
2. Comprehension syntax (`for..yield` vs Python `[x*2 for x in xs]`) — readability vote. (Phase 0)
3. String type split: one `string` (UTF-8) + `bytes`, or add `&str` borrowed view keyword? (Phase 1, needed for parser APIs)
4. Method-call uniformity: UFCS (`first(items)` vs `items.first()` auto-deref rules)? (Phase 1)
5. Trait-object layout: fat pointers vs vtable-in-instance? (Phase 2, affects ABI)
6. Reflection depth for serialization derives: compile-time-only vs limited runtime metadata? (Phase 5)
7. Edition cadence and minimum-supported-compiler window. (Phase 6)

---

*Ratify by opening an RFC referencing section numbers. Changes to §§2, 5.1, 6, 8, 17 require
super-majority consensus per §19 once governance activates.*
