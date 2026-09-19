# Coco — Complete the Language (C++ Roadmap)

**Status:** ACTIVE — this is the primary roadmap.
**Decision (user directive, grounded in source + web research):** finish the **Coco language**
itself in C++ only. **Status note (updated):** Phases 1–4 below have
**shipped** (catchable exceptions, OOP `class`/`interface`/`record`, `fn`/`dynamic`, slice/rest
patterns incl. `match`-as-expression, memory/scoping keywords, multi-statement closures) — so
§0 items 1–3 are now **historical / resolved**. The remaining roadmap (Phases 5–12: type-system
hardening, stdlib in Coco, correctness, tooling, native breadth, borrow checker, concurrency,
CLI) is the active work. See the **[ownership map](#ownership-map-across-this-repo-s-plans)**
below so this plan, `SYNTAX_PLAN.md`, `EXP_PLAN.md`, `DATA_TYPE_PLAN.md`, `STD_LIBS_PLAN.md`,
and `WHY_PLAN.md` don't duplicate each other.
**Implementation languages:** C++ (extend existing seed), **Go** (pure-tooling CLI accents), and
**Rust** (optional components that map to Coco's Rust-inspired model). Every phase below shows
real code in the relevant language so the plan is executable, not prose.

---

## 0. Evidence the language is not done (source-backed) — *historical; items 1–2 now RESOLVED*

> Revised: items 1–3 below were the original rationale. Items **1 (keywords + catchable
> exceptions) and 2 (stdlib)** have been **shipped** (see Phase 1/2/3.5 status notes and
> `stdlib/lib/`). Item **3** is partially resolved (patterns, closures, and generators landed in
> Phases 3/3.5/4); the remaining parts (borrow/move checker, default generic params, `iota`
> discriminants, crypto/extensive stdlib, `coco fmt/repl/check/lint`) are still open. Item **4**
> (scalar-only native backend) is still accurate. Kept below for provenance.

1. **Keyword table** `src/lex/lexer.cpp:38-50` has no `class`, `interface`, `record`, `fn`,
   `extends`, `implements`, `dynamic`, `try`.`try/catch` does not exist; `raise` throws
   `SignalRaise` which **nothing catches in-language** (`src/interp/runtime.cpp:1290`);
   `catch_panic` only catches `PanicSignal` (`:544`). **[RESOLVED]** — all of these keywords
   are now in `lexer.cpp:40-46`; `try/catch/raise` parse and run (see Phase 1 status below).
2. **Stdlib is almost empty as source** — only `stdlib/text/slug.co`; `json/math/time/os/io/mem`
   are C++ baked-in builtins (`src/interp/runtime.cpp` builtin tables). **[RESOLVED, Phase 6
   largely done]** — `stdlib/lib/` now holds `core, collections, io, json, math, os, path,
   regexp, strings, time` as importable Coco modules (see `STD_LIBS_PLAN.md`).
3. **Documented unfinishings** (`docs/FEATURE_GAP_ANALYSIS.md` §3-5, `PLAN.md`): slice/rest
   patterns, multi-statement closures, borrow/move checker, default generic params,
   `iota`-like discriminants, collections/strings/regexp/path stdlib, `coco fmt/repl/check/lint`.
   **[PARTIALLY RESOLVED]** — slice/rest patterns, multi-statement closures, and generators
   landed in Phases 3/3.5/4; collections/strings/regexp/path now exist in `stdlib/lib/`. Still
   open: borrow/move checker (Phase 5.5/10), default generic params + iota discriminants
   (Phase 5), `coco fmt/repl/check/lint` (Phase 8).
4. **Native backend** `src/backend/native.cpp` lowers only *scalar* functions; not strings/lists/`main`.

The plan below closes all of these, with code at every step.

---

## Design principle: OOP as a facade over the Rust/Go model

Coco already has the **composition** model (`struct` + `trait` + `impl`, examples 11/13). We add
OOP keywords as **desugaring** so existing code keeps working:

- `class Foo { … }` → `struct Foo` + inherent `impl` (methods inside the body).
- `class Dog extends Animal implements Barkable { … }` → single inheritance over a base, plus
  trait `interface`s — Java/C#-style.
- `record Point(x: int, y: int)` → immutable value type, structural `==`/`hash`, auto-`repr`.
- `interface Len { def len() -> int }` → **structural/duck-typed** trait bound (Go-style).

---

## Phase 1 — Catchable exceptions (`try { } catch e { }`)

> **Status: DONE** — `try`, `catch e { }`, and `raise` parse and run end-to-end (statement form
> *and* expression form returning a value), verified in `examples/35_try_catch.co` (VM ≡
> tree-walker; nested re-raise and non-string payloads supported). Exit criteria met.

**Goal:** unblock error recovery; a compiler/fmt/repl/repl-all need catchable errors.

**Syntax to add** (C<?>. The language itself, so written in Coco files and implemented in the C++
seed):
```coco
# examples/37_try_catch.co
def risky(n: int) -> int {
    if n < 0 { raise("n must be >= 0"); }
    return n * n;
}
def main() {
    # statement form
    try {
        print(risky(-1));
    } catch e {
        print(f"recovered: {e}");
    }
    # expression form (returns a value)
    r = try { risky(3) } catch e { -1 };
    print(r);           # 9
    r2 = try { risky(-2) } catch e { -1 };
    print(r2);          # -1
}
```

**In the C++ seed:**
- Lexer (`src/lex/lexer.cpp:38-50`): add `catch`, keep `try`.
- Parser (`src/parser/parser.cpp`): parse `try` as a **statement** and an **expression**
  (`parseTry`).
- Checker (`src/sema/checker.cpp`): type the `catch e { }` branch; each branch must unify.
- Runtime (`src/interp/runtime.cpp`): a `try` compiles to a protected region that already exists
  as `SignalRaise`; `catch` unwinds to it (reuse the same signal the `raise` stmt throws).
```cpp
// src/sema/checker.cpp (sketch)
case ast::StKind::Try: {
    checkExpr(*s->exprs[0], env);                 // body
    checkUnify(s->exprs[0]->ty(), s->exprs[1]);   // catch branch result type
}
```

**Exit:** `examples/35_try_catch.co` runs on tree-walker + VM; a *small* Coco parser reproduction
(`parse_try_top`-shaped) terminates instead of stack-overflowing; corpus green.

---

## Phase 2 — OOP: `class`, `interface`, `record`, `fn`, `dynamic`

> **Status: increment 2 DONE (extends + virtual dispatch)** — `extends Base` now
> implements single inheritance end-to-end across parser, checker (subtyping +
> field/method merge with override/shadowing), and the runtime (a one-time
> `mergeInheritance()` materializes the effective derived node so `makeStruct`,
> `invokeMethod`, `memberRead` need no changes; `invokeMethod`/overrides give
> virtual dispatch, so calling through a base-typed reference reaches the derived
> method). `class A extends B extends C` chains; a second base is rejected. A
> derived struct is assignable to any transitive base (subtype rule in
> `Checker::assignable`). Negative test `n09_undefined_base.co`. Example:
> `examples/36_oop.co` (VM ≡ tree-walker). Corpus green (39/40; only the
> pre-existing `native_main.co`, known native-backend exit-code discrepancy).
> **Also done (phase tail):** `record` structural `==` (records are value-typed
> structs; `==` compares fields structurally — verified in `examples/36_oop.co`);
> `any` / `dynamic` (**SP-5**) — `: any` / `: dynamic` now resolve as the dynamic
> type (`var x: any = 5; x = "hi"`), checker defers, runtime dispatches on the
> value tag with duck-typed method calls (verified `examples/37_dynamic_any.co`,
> VM ≡ tree-walker). **Also done:** builtin method forms extended — `len()` on
> string/list/dict/set; `repeat(n)`/`contains`/`starts_with`/`ends_with`/
> `replace(from,to)`/`find(sub)` (returns `-1` if absent)/`capitalize`/`strip`
> on string; list `extend(ys)`/`reverse()`/`clear()`; dict `remove(k)`/
> `contains(k)`/`setdefault(k,v)`; set `remove(x)`. Checker (`methodLookup`)
> typechecks each sig; runtime (`memberRead` `bind`s) implements them;
> verified `examples/39_builtin_methods.co` (VM ≡ tree-walker). **Still
> deferred:** `pub class/interface/record` in cross-module exports (needs the
> Phase-6 module-export fix).

**Goal:** the headline ask. Add keywords as a desugaring pass over the working model.

**Target Coco (what users write):**
```coco
# examples/38_oop_classes.co
interface Named { def name(self) -> string; }   # structural bound

class Animal {                                 # == struct + inherent impl
    var name: string;
    def speak(self) -> string { return "?"; }
}
class Dog extends Animal implements Named {    # single inheritance + trait
    def speak(self) -> string { return "woof"; }
    def name(self) -> string { return self.name; }
}
record Point(x: int, y: int) { }               # immutable, structural ==

def main() {
    d = Dog(name: "Rex");
    print(d.speak());                          # woof (virtual dispatch)
    g = render(d);                             # via interface Named
    p = Point(x: 1, y: 2);
    print(p == Point(x: 1, y: 2));             # true (structural)
    fn_plus = fn (a: int, b: int) -> int { return a + b; };  # fn == def alias
    print(fn_plus(2, 3));
}
```

**How it desugars (keep AST semantics-preserving):**
```coco
# desugared class Dog …
struct Dog extends Animal { var name: string; }
# Dog inherits Animal fields/methods (single inheritance, vtable)
impl Named for Dog { def name(self) -> string { return self.name; } }
def speak_dog(self) { return "woof"; }   # override replaces base method in vtable
```

**In the C++ seed — a `class` → `struct+impl` lowering:**
```cpp
// src/sema/checker.cpp (sketch) — class declaration lowers to struct + inherent impl
ast::Stmt lowerClass(ast::Stmt& s) {
    s.kind = ast::StKind::StructDef;         // reuse existing struct node
    /* s.typeParams, s.fields unchanged */
    /* methods moved to an implicit impl body kept on the node */
    return s;
}
```

**Go accent (tooling for codegen of record equality):** a tiny Go generator that is later dogfooded.
```go
// tools/recordgen/main.go — emits ==/hash/repr for a record descriptor
func EmitRecord(name string, fields []Field) string {
    var b strings.Builder
    b.WriteString("    def __eq__(self, other) -> bool {\n    ")
    for _, f := range fields {
        fmt.Fprintf(&b, "if self.%s != other.%s { return false; }\n", f.Name, f.Name)
    }
    b.WriteString("    return true;\n   }\n")
    return b.String()
}
```

**Exit:** examples 36 (`oop.co` — class/interface/record/fn) and the try/catch example run on
tree-walker + VM; negative tests for bad `extends` produce a real diagnostic (undefined base,
multiple bases). **Tail (pending):** structural `record ==`, `dynamic` semantics, `pub`
cross-module export.

---

## Phase 3 — Pattern power (reach Rust `PatKind` parity)

> **Status: DONE** — added the missing `PatKind` forms end-to-end (parser, AST,
> checker, runtime `matchPat`/`bindPat`, AST dump):
> **slice** `[a, b, ..rest]`, **pure rest** `..` (now tracked via a new
> `Pat::hasRest`, so `[a, ..]` and `(x, y, ..)` match any tail without binding),
> **tuple-rest** `(x, y, ..)`, **nested `@`** `v @ (1..=5)`, and **ref patterns**
> `&pat` (new `PatKind::Ref`, transparent deref in the v1 by-value model).
> The tuple-rest checker bug (rejected `(x, y, ..)` on a 3-subject as "tuple
> pattern has 2 elements but subject has 3") is fixed. Example:
> `examples/38_patterns_power.co` now uses **`match`-as-expression** (values flow
> from arms, including a trailing `return match t { … };` clause and
> `s = match n { … };` assignment) and still passes VM ≡ tree-walker. Corpus
> green (39/40; only pre-existing `native_main.co`).

**Goal:** close the biggest `match` expressiveness gap (`FEATURE_GAP §3.2`).

**Target Coco:**
```coco
def describe(val) {
    match val {
        case [] { print("empty"); }
        case [a, b, ..rest] { print(f"first={a} rest={rest}"); }   # slice pattern
        case (x, y, ..) { print(f"tuple {x},{y}"); }                # rest in tuple
        case n @ (1..=5) { print(f"small {n}"); }                   # nested @
        case &Point { print("borrowed point"); }
    }
}
```

**Rust reference (the model):**
```rust
pub enum Pat {
    Slice(Vec<Pat>),          // [a, b, ..rest]
    Rest,                     // ..
    Ref { pmut: Mutability },
    Ident { sub: Option<Box<Pat>> },  // n @ sub
}
```

**In the C++ seed** — extend `src/sema/` pattern representation & matcher to add the missing
variants (`PatKind::Slice/Rest/Ref`), and `src/interp/runtime.cpp` destructure.

**Exit:** `tests/patterns/*.co` positive + negative; existing `match` examples unchanged; AST dump
stable for new forms.

---

## Phase 3.5 — Memory & scoping keywords (`None`, `del`, `pr`, `local`/`global`, `temp`, `bucket`)

> **Status: DONE** — a batch of new keywords/features requested by the user:
> - **`None`** — type spelling for `noneTy()` (`var n: None = none`), like `int`/`string`.
> - **`del`** — delete a variable binding, dict key (`del d[k]`), list/tuple index
>   (`del xs[i]`), or struct instance field (`del obj.f`). Runtime walks the env chain to
>   unbind; `del` of an undefined name/const is a compile error.
> - **`pr`** — explicit private modifier (`pr def f`, `pr class C`, …), the mirror of `pub`.
> - **`local`/`global`** — strict scoping: `local x = v` forces a fresh binding in the
>   current block (same-scope re-decl = error); `global x [= v]` reads/writes the
>   module-top-level binding from inside a function (`global x` errors if `x` is not a
>   top-level binding). Both run on VM and tree-walker identically.
> - **`temp <name> <N>[: T] = <v>`** — compile-time use budget (1..10). The checker
>   decrements per Ident read; use #(N+1) is a compile error ("temp 't' exhausted (N uses)").
> - **`bucket`** — a separate namespace for parking values: `bucket x = v` offloads `x`
>   into the bucket store (invisible to normal lookup); `bucket release x` moves it back.
>
> **Design notes:** `bucket` is a **hard keyword** (user decision); `yield` stays a
> **contextual** statement-start word so `for x in xs yield x` generator views — and any
> pre-existing identifiers — keep working. Generator *functions* are implemented directly
> (this milestone, not the suspendable-frame design deferred earlier): a `def` whose body
> contains `yield` returns a `gen[T]`; the interpreter materializes the yielded values
> eagerly (allowed divergence per COCO_PLAN §runtime), so `for x in g()`, `.collect()`,
> `.filter()`, `.map()`, `.len()` all work, VM ≡ tree-walker.
> `pr` is parsed and stored; true cross-module export enforcement is deferred to
> the module-export fix (same item as `pub class` export).
>
> **Verified:** `examples/40_keywords.co` and `examples/41_generators.co` (both VM ≡
> tree-walker), negative tests `n10_temp_exhausted.co` / `n11_del_undefined.co`
> / `n12_yield_outside.co` / `n13_yield_return_value.co` / `n14_yield_wrong_ret.co`,
> corpus green (41/41), typecheck 41/41, vm_diff 39 matched / 0 failed.

---

## Phase 4 — Expression & statement completeness

> **Status: DONE** — multi-statement block closures `fn (v) { stmts }` work end-to-end (checker +
> runtime), verified in `examples/44_block_closures.co` (VM ≡ tree-walker); the `fn` alias for
> `def` also landed (Phase 2). See `SYNTAX_PLAN.md` SP-11 for the still-missing `fn`-as-first-class
> value breadth (resumable generator frames) — not part of this milestone.

**Goal:** the constructs the user said "are not working" — chiefly multi-statement closures.

**Target Coco (multi-statement closure):**
```coco
# examples/36_multistmt_closure.co
def map(xs, f: fn(int) -> int) -> list {
    out = [];
    for x in xs { out.append(f(x)); }
    return out;
}
def main() {
    ys = map([1, 2, 3], fn (v) {        # block-bodied closure
        doubled = v * 2;
        return doubled + 1;
    });
    print(ys);                           # [3, 5, 7]
}
```

**Rust reference (`|v| { let d = v*2; d+1 }` ⇔ Coco `fn (v) { … }`).** Extend the single-expression
lambda (`(x) => e`, `examples/07`) to a `fn (x) { stmts }` form in the checker + runtime.

**Exit:** closure examples with bodies run; `examples/25_operator_precedence.co` still the diff
oracle for the precedence table; corpus green.

---

## Phase 5 — Type-system hardening

**Goal:** opt-in strictness, default-on safety (`FEATURE_GAP §3.3`).

**Target Coco:**
```coco
# 5.1 default type params
def make_pair[T = int](a: T, b: T) -> (T, T) { return (a, b); }
# 5.3 iota-like enum discriminants
enum Status { Ok, Err, Running; }           # implicit 0,1,2 via .to_int()
# 5.4 exhaustiveness
def grade(c) -> string {
    match c {
        case 90..=100 { return "A"; }
        case 80..=89  { return "B"; }       # missing 0..=79 and other ints → ERROR/fix-it
    }
}
```
**5.5 conservative borrow/move pass** — over the AST before native AOT:
```cpp
// src/backend/borrow.cpp  — default-on, conservative
visit(Assign target t, Expr value):
  if value is '&' of local v AND t escapes (returned / stored) → error E-BORROW-ESCAPE
```
**Rust accent:** the pass is modeled on Rust's *simplified* rules; a full NLL solver comes in
Phase 10 here (below).

**Exit:** `tests/types/*.co` positive+negative; `coco check` accepts the hardened grammar; the
existing `tests/negative/n*.co` are still rejected with identical text.

---

## Phase 6 — Stdlib breadth as real Coco modules

> **Status: largely DONE (see `STD_LIBS_PLAN.md` for the full module spec).** The stdlib now
> ships as importable Coco source in `stdlib/lib/` — `core, collections, io, json, math, os,
> path, regexp, strings, time` — each with a `*_test.co` (except `core`). Remaining work here is
> *breadth/enhancement* (more functions; fix `json` recursion; real `regexp` engine), owned by
> `STD_LIBS_PLAN.md`. The `import lib.x` resolution and module-export constraints in
> `src/interp/runtime.cpp` are the implementation surface.

**Goal:** replace C++ builtins with importable, dogfoodable Coco source (real module
dependency in the stdlib).

**Target Coco (a real module):**
```coco
# stdlib/collections.co  — written in Coco, exported & importable
pub struct HashMap[K, V] {
    var buckets: dict;
    def put(self, k: K, v: V) { self.buckets[str(k)] = v; }
    def get(self, k: K) -> V? { return self.buckets[str(k)]; }
}
pub def split(s: string, sep: string) -> list { … }
pub def join(parts: list, sep: string) -> string { … }
```
```coco
# consumer
import lib.collections;
def main() {
    m = collections.HashMap[string, int]();
    m.put("a", 1);
    print(m.get("a"));
    print(collections.join(["x", "y"], "-"));   # "x-y"
}
```

**Fix the module-export constraint** (the "only `pub def`" limit that blocks
exporting types): allow `pub struct`/`pub class`/`pub enum`/`pub trait` in imported modules
(`src/interp/runtime.cpp` module resolver).

**Exit:** each module (`collections`, `strings`, `path`, `regexp`, `io`, `os`, `json`, `math`,
`time`) ships a `*_test.co`; `coco test` green; json round-trips nested types.

---

## Phase 7 — Correctness & runtime robustness

**Goal:** kill latent correctness and runtime-robustness bugs in the interpreter/VM.

- `repr`/`toStr` depth + cycle guard (stack-overflow on self-referential data today).
- Integer overflow: checked by default, wrapping under `--release`.
- `select` readiness via condvar (remove the 1 ms busy-poll).
- `for` iterate the underlying vector in place.
```cpp
// src/interp/runtime.cpp — repr cycle guard
Value reprDepth(const Value& v, int depth, std::set<const void*>& seen) {
    if (depth > 64) return Value::str("…");
    if (v.k == VK::List) { if (!seen.insert(v.vec.get()).second) return "…"; … }
}
```

**Exit:** `repr` of a self-referential list terminates; TSan-clean worker-pool demo; corpus unchanged.

---

## Phase 8 — Tooling: `coco fmt/repl/check/lint/get`

**Goal:** "one executable like `go`" (`PLAN.md` Phase 9). **Go** is the natural implementation for
the driver if we want speed + concurrency; C++ also works.

**Go accent — the `coco` driver:**
```go
// tools/coco/main.go
func main() {
    cmd := os.Args[1]
    switch cmd {
    case "fmt":     os.Exit(runFmt(os.Args[2:]))
    case "check":   os.Exit(runCheck(os.Args[2:]))
    case "lint":    os.Exit(runLint(os.Args[2:]))
    case "repl":    os.Exit(runRepl())
    case "get":     os.Exit(runGet(os.Args[2:]))
    default:        fmt.Fprintln(os.Stderr, "coco <build|run|test|fmt|check|lint|repl|get|env>")
    }
}
```

**Exit:** `coco fmt --check` idempotent on the corpus; `coco check`/`lint`/`repl` smoke green.

---

## Phase 9 — Native backend breadth

**Goal:** widen native lowering to strings/lists and `main` itself.

**C++ (extend `src/backend/native.cpp`):** lower strings/lists; hybrid calls to non-lowered fns;
finally lower `main` itself; resolve the lowered-`main -> int` exit-code discrepancy (350→0);
`--release`/`-O2` on emitted C++.
```cpp
// src/backend/native.cpp — escape for non-scalar to a runtime call
if (!isScalar(ty)) out += callRuntime("coco_rt_str_concat", args) + ";";
```

**Exit:** native/VM/tree-walker differential green on 100% of corpus; ASan/UBSan clean; fib/string/
JSON benchmarks within documented factor of C.

---

## Phase 10 — Full borrow checker & memory safety v1

**Goal:** "safe like Rust" (`PLAN.md` Phase 12) with staged delivery.

**Rust reference architecture (our model):** `MIR` + `borrowck` in `rustc_borrowck`.
**Staged:** (a) the conservative Phase-5.5 pass default-on; (b) a real **SSA MIR**
(`src/mir/`) with move-liveness + region inference enforced under `--strict`.

```rust
// tools/mir_borrowck (Rust, optional accelerator) — rule engine skeleton
pub fn check(expr: &Expr, ctx: &mut Ctx) -> Result<(), Error> {
    match expr {
        Expr::Ref(l) => if ctx.escaping(l) { return Err(escapes(l)) }
        Expr::Var(v) => if ctx.moved(v) { return Err(move_after_use(v)) }
        _ => ctx.walk(expr),
    }
}
```

**Exit:** `tests/borrow/*.co` positive+negative; ASan-clean; runtime unchanged when not `--strict`.

---

## Phase 11 — Concurrency & GC finalize

**Goal:** Go-grade `spawn/chan/select` safety and a GC story.
- Sendability checker for `spawn` payloads; worker pool; structured concurrency.
- `sync` module (mutex/rwlock/once/barrier/atomic).
- GC under `--gc` (precise tracing vs ephemeron cycles), keep `weak` semantics.
```go
// cmd/coco sync mutex (Go) if desired — or C++ (sync.Mutex)
type Mutex struct{ ch chan struct{} }
func NewMutex() *Mutex       { return &Mutex{ch: make(chan struct{}, 1)} }
func (m *Mutex) Lock()       { m.ch <- struct{}{} }
func (m *Mutex) Unlock()     { <-m.ch }
```

**Exit:** 10k-task crawler ASan + TSan clean; `sync` tests green; `repr` cycle-safe.

---

## Phase 12 — CLI/UX & ecosystem

**Goal:** standards-aligned CLI (`PLAN.md` Phase 13) + registry.
- `coco <build|run|test|vet|fmt|doc|env|check|lint>`; config via `COCOCACHE`, `coco.mod`,
  `coco env`.
- Package ecosystem: `coco publish/install/get`, registries, checksums, `pin.co`.

**Exit:** `coco help` < 40 lines; `coco env | grep COCOCACHE` works; install/publish CJE on win/linux/mac.

---

## Milestones

- **M0 (P1-4):** catchable errors; OOP `class/interface/record/fn/dynamic`; patterns at Rust
  parity; memory/scoping keywords; multi-statement closures. **[COMPLETE — see status notes on
  Phases 1/2/3/3.5 and examples 35–44.]**
- **M1 (P4-6):** closures/expressions; type system; stdlib in Coco. **[P4 closures + P6 stdlib
  largely DONE; P5 type-system hardening in progress.]**
- **M2 (P7-9):** runtime correctness; tooling; native breadth.
- **M3 (P10-12):** borrow checker, concurrency/GC, standard CLI.

---

## Ownership map (across this repo's plans) — deduplication guide

Feature families span several plans. To keep each feature in **exactly one** place and cross-ref
the rest, own by:

| Feature family | Owner file |
|---|---|
| Stdlib **module** packaging/APIs (`json, os, io, math, time, collections, path, regexp, strings`, all future modules) | `STD_LIBS_PLAN.md` |
| Core data **types** internals (big_int, complex, decimal, rational, bytes, collections types, sync types, time/Duration, reflect) | `DATA_TYPE_PLAN.md` |
| **Syntax**/sugar/operators (walrus, pipe, comprehensions, splat, `any` keyword, decorators, FFI, patterns) | `SYNTAX_PLAN.md` |
| Free **builtin** names & math-expression surface (defaults, operator traits, vector math) | `EXP_PLAN.md` |
| **Why**/demand drivers + sequencing | `WHY_PLAN.md` (stubs + cross-refs only) |
| Compiler **infra** (VM, AOT, diagnostics, toolchain, borrowck, GC) | `PLAN.md`, `DO_FIRST_PLAN.md` |

Consequences already applied in this repo: builtins `map/filter/reduce/sum/min/max/sorted/
enumerate/reduce/zip` are spec'd in `EXP_PLAN.md`/`SYNTAX_PLAN.md` (their "why" is `WHY_PLAN.md`
WHY-1), not re-spec'd here; math function breadth is partitioned as *builtins→EXP*, *float
type→DATA_TYPE*, *module→STD_LIBS*; the `result[T,E]`/`error` combinators live in
`DATA_TYPE_PLAN.md` Phase 8 + `EXP_PLAN.md` Phase 14, not here.

---

## Ordering rationale

1. **P1 (exceptions)** is a language feature *and* unblocks every self-referential tool. Fix the
   crash we actually hit first.
2. **Feature parity while the interpreter is the oracle** is cheap; the same additions later
   would mean touching multiple implementations at once (interpreter + VM + native).
3. **Stdlib in Coco now** gives exact parity before the Coco compiler must depend on it.
4. **Only a language that speaks its own errors can bootstrap honestly** (Go/Rust research).

*Use C++ to extend the seed; Go for the CLI/tooling driver; Rust for the borrow-check/SOLVER and
MIR components that map cleanly onto Coco's model. Do not force a full Rust rewrite.*

---

## Decision points to ratify before code (draft defaults noted)

1. `class` = Rust-facade over `struct+impl` (**default**) vs Java reference-type-first.
2. Inheritance: **single + `implements` traits** (Java/C#-style) — least surprising.
3. `try/catch` syntax: `catch e { }` binding the message (**default**); keep `try expr` `?`-form.
4. `fn` alias accepted for `def` (**default**, additive).
5. `const` promoted to a real compile-time keyword (**default**).
6. Modules must export types (`pub struct/class/enum`) — a real stdlib milestone.
7. GC default stays refcount+weak (no flag); keep `repr` cycle-safe; `--gc` later.
8. `??` nil-coalescing (**default** include).

## References

- Gaps: `docs/FEATURE_GAP_ANALYSIS.md`, `PLAN.md`, `docs/COCO_PLAN.md`.
- Keywords: `src/lex/lexer.cpp:38-50`; errors: `src/interp/runtime.cpp:1290,544`.
- Model sources (web): class-vs-struct semantics (C#/Swift/C++); Go vs Java/C# OOP; traits-vs-
  interfaces; C# `record`/`interface`; Kotlin `data class`. Takeaway: keep the Rust/Go
  **composition** model and add `class`/`interface`/`record` as ergonomic, semantics-preserving
  facades.