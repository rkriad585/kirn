# Kirn — Complete the Language (C++ Roadmap)

## Goal

The do-first plan orders work by **fastest achievable value** and unblocks the gates, keeping the build to exactly `kirn` + `kirnc`, industry-standard, AI-first (front-end checkability before glamour), minimal. It front-loads the items that make Kirn usable and checkable today, prioritizing honest diagnostics before backend glamour, so `kirn test` and `scripts/gates_linux.sh` stay green. Every item is the smallest change that unblocks the next; adopt the best proven patterns from the reference sources (Rust, Go, CPython, Ruby, Nim, Odin, V, Zig, LLVM) rather than over-engineering.

## Must Follow

- **Build ships ONLY `kirn` + `kirnc`.** No other executable is a product surface.
- **No over-engineering.** Each do-first item must be the _smallest_ change that unblocks the gate it targets; breadth belongs to later plans.
- **AI-first.** Prioritize correctness and diagnostics the agents need: stable `.kn` spellings, deterministic dumps, and actionable fix-it errors.
- **Kirn-style spellings.** Use `fn` for functions and `var` for variables; base types capitalized: Str/Int/Float/Bool/Char/BigInt/Rational; lowercase width micros (`i4..i256`, `u4..u256`, `f4..f256`); capitalized widths (UInt, UFloat, Int4..UInt256, Float4..UFloat256, ISize/USize); free builtins/types CapCase (Str(...), BigInt(...), Min, Max, __Sqrt); methods lowercase.
- **Every do-first item ends with its gate/verification** — explicitly run `kirn test` and/or `scripts/gates_linux.sh` to prove it.

## Important Note

The existing **ordering and priority are preserved**: the phase sequence (§0 → Phase 12), the
M0–M3 milestone grouping, and every gate/exit reference stay intact. Before acting, **re-verify
the ordering against the current repo state** (`scripts/gates_linux.sh` gates 1–10, `build/kirn`,
`build/kirnc`, `examples/*.kn`): most status notes on Phases 1–6 already mark those as DONE, so
treat those as gates already green and re-check that the still-open items (Phase 5 type-system
hardening, Phase 7 runtime correctness, Phase 9 native breadth, Phase 10 borrowck, Phase 11
concurrency/GC, Phase 12 CLI) are the ones the gates actually still fail.

**Status:** ACTIVE — this is the primary roadmap.
**Decision (user directive, grounded in source + web research):** finish the **Kirn language**
itself in C++ only. **Status note (updated):** Phases 1–4 below have
**shipped** (catchable exceptions, OOP `class`/`interface`/`record`, `fn`/`dynamic`, slice/rest
patterns incl. `match`-as-expression, memory/scoping keywords, multi-statement closures) — so
§0 items 1–3 are now **historical / resolved**. The remaining roadmap (Phases 5–12: type-system
hardening, stdlib in Kirn, correctness, tooling, native breadth, borrow checker, concurrency,
CLI) is the active work. See the **[ownership map](#ownership-map-across-this-repo-s-plans)**
below so this plan, `SYNTAX_PLAN.md`, `EXP_PLAN.md`, `DATA_TYPE_PLAN.md`, `STD_LIBS_PLAN.md`,
and `WHY_PLAN.md` don't duplicate each other.
**Implementation languages:** C++ (extend existing seed), **Go** (pure-tooling CLI accents), and
**Rust** (optional components that map to Kirn's Rust-inspired model). Every phase below shows
real code in the relevant language so the plan is executable, not prose.

---

## 0. Evidence the language is not done (source-backed) — _historical; items 1–2 now RESOLVED_

> Revised: items 1–3 below were the original rationale. Items **1 (keywords + catchable exceptions) and 2 (stdlib)** have been **shipped** (see Phase 1/2/3.5 status notes and `stdlib/lib/`). Item **3** is partially resolved (patterns, closures, and generators landed in Phases 3/3.5/4); the remaining parts (borrow/move checker, default generic params, `iota` discriminants, crypto/extensive stdlib, `kirn fmt/repl/check/lint`) are still open. Item **4** (scalar-only native backend) is still accurate. Kept below for provenance.

**Goal:** Document provenance while moving do-first items forward; no changes required.
**Must Follow:** Treat as historical; use `fn`/`var` in any edits; verify with `kirn test`, `scripts/gates_linux.sh` only if changing behavior.
**Important Note:** Preserve original rationale. > Reference: See top-level dirs in `/home/rkriad585/Projects/langs-source-code/` for language comparison context.

1. **Keyword table** `src/lex/lexer.cpp:38-50` has no `class`, `interface`, `record`, `fn`,
   `extends`, `implements`, `dynamic`, `try`.`try/catch` does not exist; `raise` throws
   `SignalRaise` which **nothing catches in-language** (`src/interp/runtime.cpp:1290`);
   `catch_panic` only catches `PanicSignal` (`:544`). **[RESOLVED]** — all of these keywords
   are now in `lexer.cpp:40-46`; `try/catch/raise` parse and run (see Phase 1 status below).
2. **Stdlib is almost empty as source** — only `stdlib/text/slug.kn`; `json/math/time/os/io/mem`
   are C++ baked-in builtins (`src/interp/runtime.cpp` builtin tables). **[RESOLVED, Phase 6
   largely done]** — `stdlib/lib/` now holds `core, collections, io, json, math, os, path,
regexp, strings, time` as importable Kirn modules (see `STD_LIBS_PLAN.md`).
3. **Documented unfinishings** (`docs/FEATURE_GAP_ANALYSIS.md` §3-5, `PLAN.md`): slice/rest
   patterns, multi-statement closures, borrow/move checker, default generic params,
   `iota`-like discriminants, collections/strings/regexp/path stdlib, `kirn fmt/repl/check/lint`.
   **[PARTIALLY RESOLVED]** — slice/rest patterns, multi-statement closures, and generators
   landed in Phases 3/3.5/4; collections/strings/regexp/path now exist in `stdlib/lib/`. Still
   open: borrow/move checker (Phase 5.5/10), default generic params + iota discriminants
   (Phase 5), `kirn fmt/repl/check/lint` (Phase 8).
4. **Native backend** `src/backend/native.cpp` lowers only _scalar_ functions; not strings/lists/`main`.

The plan below closes all of these, with code at every step.

---

## Design principle: OOP as a facade over the Rust/Go model

**Goal:** Add OOP as a semantics-preserving facade so existing `struct` + `trait` + `impl` code keeps working unchanged; this keeps "Kirn style" minimal while matching industry-standard composition.
**Must Follow:** `class` → `struct` + inherent `impl`, `record` → value type, `interface` → structural bound; no new runtime semantics; use `fn`/`var` and CapCase types where shown.
**Important Note:** Compatibility contract behind Phase 2 — verify with `examples/36_oop.kn` on tree-walker + VM and via `kirn test`, `scripts/gates_linux.sh`.

> Reference: Go — struct + implicit-interface composition (`go/src/go/types`).

Kirn already has the **composition** model (`struct` + `trait` + `impl`, examples 11/13). We add
OOP keywords as **desugaring** so existing code keeps working:

- `class Foo { … }` → `struct Foo` + inherent `impl` (methods inside the body).
- `class Dog extends Animal implements Barkable { … }` → single inheritance over a base, plus
  trait `interface`s — Java/C#-style.
- `record Point(x: int, y: int)` → immutable value type, structural `==`/`hash`, auto-`repr`.
- `interface Len { fn len() -> int }` → **structural/duck-typed** trait bound (Go-style).

---

## Phase 1 — Catchable exceptions (`try { } catch e { }`)

> **Status: DONE** — `try`, `catch e { }`, and `raise` parse and run end-to-end (statement form
> _and_ expression form returning a value), verified in `examples/35_try_catch.kn` (VM ≡
> tree-walker; nested re-raise and non-string payloads supported). Exit criteria met.

**Goal:** Unblock error recovery with the smallest change; front-end catchability needed for AI-first diagnostics.
**Must Follow:** Keep build to `kirn` + `kirnc` only; use `fn`/`var`/CapCase in examples; every check ends with `kirn test` and/or `scripts/gates_linux.sh`.
**Important Note:** Reuse existing signal path; preserve parser/VM parity. Portrayal below is the exact minimal surface.

**Syntax to add** (C<?>. The language itself, so written in Kirn files and implemented in the C++
seed):

```kirn
# examples/37_try_catch.kn
fn risky(n: int) -> int {
    if n < 0 { raise("n must be >= 0"); }
    return n * n;
}
fn main() {
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

**Exit:** `examples/35_try_catch.kn` runs on tree-walker + VM; a _small_ Kirn parser reproduction
(`parse_try_top`-shaped) terminates instead of stack-overflowing; corpus green.

---

## Phase 2 — OOP: `class`, `interface`, `record`, `fn`, `dynamic`

> **Status: increment 2 DONE (extends + virtual dispatch)** — `extends Base` now implements single inheritance end-to-end across parser, checker (subtyping + field/method merge with override/shadowing), and runtime (`mergeInheritance()` materializes effective derived node so `makeStruct`, `invokeMethod`, `memberRead` need no changes; `invokeMethod`/overrides give virtual dispatch). `class A extends B extends C` chains; a second base is rejected. Derived struct assignable to any transitive base (subtype rule in `Checker::assignable`). Negative test `n09_undefined_base.kn`. Example `examples/36_oop.kn` (VM ≡ tree-walker). Corpus green (39/40; only pre-existing `native_main.kn`, known native-backend exit-code discrepancy).
> **Also done (phase tail):** `record` structural `==` (records are value-typed structs; `==` compares fields structurally — verified in `examples/36_oop.kn`); `any` / `dynamic` (**SP-5**) — `: any` / `: dynamic` now resolve as the dynamic type (`var x: any = 5; x = "hi"`), checker defers, runtime dispatches on value tag with duck-typed method calls (verified `examples/37_dynamic_any.kn`, VM ≡ tree-walker). **Also done:** builtin method forms extended — `len()` on string/list/dict/set; `repeat(n)`/`contains`/`starts_with`/`ends_with`/`replace(from,to)`/`find(sub)` (returns `-1` if absent)/`capitalize`/`strip` on string; list `extend(ys)`/`reverse()`/`clear()`; dict `remove(k)`/`contains(k)`/`setdefault(k,v)`; set `remove(x)`. Checker (`methodLookup`) typechecks each sig; runtime (`memberRead` `bind`s) implements them; verified `examples/39_builtin_methods.kn` (VM ≡ tree-walker). **Still deferred:** `pub class/interface/record` in cross-module exports (needs the Phase-6 module-export fix).

**Goal:** Headline ask. Add OOP keywords as a desugaring pass over the working model (smallest change) so `kirn` + `kirnc` surface remains minimal.
**Must Follow:** Desugar `class`→`struct+impl`, `record`→value type, `interface`→structural bound; use `fn`/`var`; CapCase types; no new runtime semantics. Verify with `kirn test`, `scripts/gates_linux.sh`.
**Important Note:** Single inheritance + `implements` traits (Java/C#-style), preserving semantics. > Reference: Go — struct + implicit-interface composition.

**Target Kirn (what users write):**

```kirn
# examples/38_oop_classes.kn
interface Named { fn name(self) -> string; }   # structural bound

class Animal {                                 # == struct + inherent impl
    var name: string;
    fn speak(self) -> string { return "?"; }
}
class Dog extends Animal implements Named {    # single inheritance + trait
    fn speak(self) -> string { return "woof"; }
    fn name(self) -> string { return self.name; }
}
record Point(x: int, y: int) { }               # immutable, structural ==

fn main() {
    d = Dog(name: "Rex");
    print(d.speak());                          # woof (virtual dispatch)
    g = render(d);                             # via interface Named
    p = Point(x: 1, y: 2);
    print(p == Point(x: 1, y: 2));             # true (structural)
    fn_plus = fn (a: int, b: int) -> int { return a + b; };  # fn == fn alias
    print(fn_plus(2, 3));
}
```

**How it desugars (keep AST semantics-preserving):**

```kirn
# desugared class Dog …
struct Dog extends Animal { var name: string; }
# Dog inherits Animal fields/methods (single inheritance, vtable)
impl Named for Dog { fn name(self) -> string { return self.name; } }
fn speak_dog(self) { return "woof"; }   # override replaces base method in vtable
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
    b.WriteString("    fn __eq__(self, other) -> bool {\n    ")
    for _, f := range fields {
        fmt.Fprintf(&b, "if self.%s != other.%s { return false; }\n", f.Name, f.Name)
    }
    b.WriteString("    return true;\n   }\n")
    return b.String()
}
```

**Exit:** examples 36 (`oop.kn` — class/interface/record/fn) and the try/catch example run on
tree-walker + VM; negative tests for bad `extends` produce a real diagnostic (undefined base,
multiple bases). **Tail (pending):** structural `record ==`, `dynamic` semantics, `pub`
cross-module export.

---

## Phase 3 — Pattern power (reach Rust `PatKind` parity)

> **Status: DONE** — added the missing `PatKind` forms end-to-end (parser, AST, checker, runtime `matchPat`/`bindPat`, AST dump): **slice** `[a, b, ..rest]`, **pure rest** `..` (tracked via `Pat::hasRest`, so `[a, ..]` and `(x, y, ..)` match any tail without binding), **tuple-rest** `(x, y, ..)`, **nested `@`** `v @ (1..=5)`, and **ref patterns** `&pat` (new `PatKind::Ref`, transparent deref in v1 by-value model). The tuple-rest checker bug fixed. Example: `examples/38_patterns_power.kn` uses **`match`-as-expression** (values flow from arms, including trailing `return match t { … };` and `s = match n { … };`) and passes VM ≡ tree-walker. Corpus green (39/40; only pre-existing `native_main.kn`).

**Goal:** Close the biggest `match` expressiveness gap with the smallest change, unblocking tooling/stdlib that need richer destructuring.
**Must Follow:** Extend AST/AST dump/checker/runtime with the missing variants; use `fn`/`var`/CapCase in examples; verify with `kirn test`, `scripts/gates_linux.sh`.
**Important Note:** Preserve backward compatibility with existing `match` examples. > Reference: Rust — `PatKind` forms (`rustc_middle/src/thir/pattern.rs`-verified in `/home/rkriad585/Projects/langs-source-code/rust`).

**Target Kirn:**

```kirn
fn describe(val) {
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

**Exit:** `tests/patterns/*.kn` positive + negative; existing `match` examples unchanged; AST dump
stable for new forms.

---

## Phase 3.5 — Memory & scoping keywords (`None`, `del`, `pr`, `local`/`global`, `temp`, `bucket`)

> **Status: DONE** — batch added end-to-end.
>
> - **`None`** — type spelling for `noneTy()` (`var n: None = none`).
> - **`del`** — delete a variable binding, dict key (`del d[k]`), list/tuple index (`del xs[i]`), or struct instance field (`del obj.f`). Runtime walks env chain to unbind; `del` of undefined name/const is a compile error.
> - **`pr`** — explicit private modifier (`pr fn f`, `pr class C`, …), mirror of `pub`.
> - **`local`/`global`** — strict scoping: `local x = v` forces fresh binding in current block (same-scope re-decl = error); `global x [= v]` reads/writes module-top-level binding from inside a function (`global x` errors if `x` is not a top-level binding). Both VM and tree-walker identical.
> - **`temp <name> <N>[: T] = <v>`** — compile-time use budget (1..10). Checker decrements per Ident read; use #(N+1) is a compile error ("temp 't' exhausted (N uses)").
> - **`bucket`** — separate namespace for parking values: `bucket x = v` offloads `x` into bucket store (invisible to normal lookup); `bucket release x` moves it back.
>
> **Design notes:** `bucket` is a **hard keyword** (user decision); `yield` stays **contextual** so `for x in xs yield x` generator views and pre-existing identifiers keep working. Generator _functions_ implemented directly: a `fn` whose body contains `yield` returns a `gen[T]`; interpreter materializes yielded values eagerly (allowed divergence per KIRN_PLAN §runtime), so `for x in g()`, `.collect()`, `.filter()`, `.map()`, `.len()` all work, VM ≡ tree-walker. `pr` parsed/stored; true cross-module export enforcement deferred to Phase-6 module-export fix.
>
> **Verified:** `examples/40_keywords.kn` and `examples/41_generators.kn` (VM ≡ tree-walker), negative tests `n10_temp_exhausted.kn` / `n11_del_undefined.kn` / `n12_yield_outside.kn` / `n13_yield_return_value.kn` / `n14_yield_wrong_ret.kn`, corpus green (41/41), typecheck 41/41, vm_diff 39 matched / 0 failed.

**Goal:** Add the requested memory/scoping control with minimal surface area to keep gates green.
**Must Follow:** Treat `bucket` as hard keyword, `yield` as contextual; use `fn`/`var`/CapCase in examples; every verification via `kirn test` and/or `scripts/gates_linux.sh`.
**Important Note:** Generator is eager in this milestone (documented divergence) — acceptable for "do-first". > Reference: CPython — `yield` semantics context (`cpython/Grammar/python.gram`, `cpython/Python/ceval.c` in `/home/rkriad585/Projects/langs-source-code/cpython`).

---

## Phase 4 — Expression & statement completeness

> **Status: DONE** — multi-statement block closures `fn (v) { stmts }` work end-to-end (checker + runtime), verified in `examples/44_block_closures.kn` (VM ≡ tree-walker); the `fn` alias for `fn` also landed (Phase 2). See `SYNTAX_PLAN.md` SP-11 for the still-missing `fn`-as-first-class value breadth (resumable generator frames) — not part of this milestone.

**Goal:** Complete the constructs requested with smallest change; multi-statement closures unblock many idioms while keeping `kirn` + `kirnc` minimal.
**Must Follow:** Extend lambda/closure to block body; use `fn`/`var`/CapCase types (e.g. `fn(int) -> Int` form); verify with `kirn test`, `scripts/gates_linux.sh`.
**Important Note:** Diff oracle remains `examples/25_operator_precedence.kn`; keep semantics identical. > Reference: Rust — closures `|v| { … }` (`rustc_hir/src/hir.rs`); Ruby — blocks/lambdas (`ruby/parse.y` in `/home/rkriad585/Projects/langs-source-code/ruby`).

**Target Kirn (multi-statement closure):**

```kirn
# examples/36_multistmt_closure.kn
fn map(xs, f: fn(int) -> int) -> list {
    out = [];
    for x in xs { out.append(f(x)); }
    return out;
}
fn main() {
    ys = map([1, 2, 3], fn (v) {        # block-bodied closure
        doubled = v * 2;
        return doubled + 1;
    });
    print(ys);                           # [3, 5, 7]
}
```

**Rust reference (`|v| { let d = v*2; d+1 }` ⇔ Kirn `fn (v) { … }`).** Extend the single-expression
lambda (`(x) => e`, `examples/07`) to a `fn (x) { stmts }` form in the checker + runtime.

**Exit:** closure examples with bodies run; `examples/25_operator_precedence.kn` still the diff
oracle for the precedence table; corpus green.

---

## Phase 5 — Type-system hardening

**Goal:** Add opt-in strictness and default-on safety with the smallest unblocking changes.
**Must Follow:** Keep Kirn-style spellings (`fn`/`var`, CapCase types); ship no extra executables; verify each subitem via `kirn test`, `scripts/gates_linux.sh`.
**Important Note:** Preserve existing `tests/negative/n*.kn` rejection texts. > Reference: Rust — type defaults/generics (`rustc_middle/src/ty/generics.rs`) and match exhaustiveness (`rustc_mir_build/src/thir/pattern/check_match.rs`) in `/home/rkriad585/Projects/langs-source-code/rust`.

**Target Kirn:**

```kirn
# 5.1 default type params
fn make_pair[T = int](a: T, b: T) -> (T, T) { return (a, b); }
# 5.3 iota-like enum discriminants
enum Status { Ok, Err, Running; }           # implicit 0,1,2 via .to_int()
# 5.4 exhaustiveness
fn grade(c) -> string {
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

**Rust accent:** the pass is modeled on Rust's _simplified_ rules; a full NLL solver comes in
Phase 10 here (below).

**Exit:** `tests/types/*.kn` positive+negative; `kirn check` accepts the hardened grammar; the
existing `tests/negative/n*.kn` are still rejected with identical text.

---

## Phase 6 — Stdlib breadth as real Kirn modules

> **Status: largely DONE (see `STD_LIBS_PLAN.md` for the full module spec).** The stdlib now ships as importable Kirn source in `stdlib/lib/` — `core, collections, io, json, math, os, path, regexp, strings, time` — each with a `*_test.kn` (except `core`). Remaining work here is _breadth/enhancement_ (more functions; fix `json` recursion; real `regexp` engine), owned by `STD_LIBS_PLAN.md`. The `import lib.x` resolution and module-export constraints in `src/interp/runtime.cpp` are the implementation surface.

**Goal:** Replace C++ builtins with importable, dogfoodable Kirn source as the smallest change to achieve parity while dogfooding the language.
**Must Follow:** Allow `pub struct`/`pub class`/`pub enum`/`pub trait` in imported modules; all examples use `fn`/`var`, CapCase types; verify with `kirn test`, `scripts/gates_linux.sh`.
**Important Note:** Keep modules in `stdlib/lib/`; tie enhancements to tests in `STD_LIBS_PLAN.md`. > Reference: Go — stdlib layout and module packages (`go/src/…`); Python — `Lib/` structure (`cpython/Lib/` in `/home/rkriad585/Projects/langs-source-code/cpython`).

**Target Kirn (a real module):**

```kirn
# stdlib/collections.kn  — written in Kirn, exported & importable
pub struct HashMap[K, V] {
    var buckets: dict;
    fn put(self, k: K, v: V) { self.buckets[str(k)] = v; }
    fn get(self, k: K) -> V? { return self.buckets[str(k)]; }
}
pub fn split(s: string, sep: string) -> list { … }
pub fn join(parts: list, sep: string) -> string { … }
```

```kirn
# consumer
import lib.collections;
fn main() {
    m = collections.HashMap[string, int]();
    m.put("a", 1);
    print(m.get("a"));
    print(collections.join(["x", "y"], "-"));   # "x-y"
}
```

**Fix the module-export constraint** (the "only `pub fn`" limit that blocks
exporting types): allow `pub struct`/`pub class`/`pub enum`/`pub trait` in imported modules
(`src/interp/runtime.cpp` module resolver).

**Exit:** each module (`collections`, `strings`, `path`, `regexp`, `io`, `os`, `json`, `math`,
`time`) ships a `*_test.kn`; `kirn test` green; json round-trips nested types.

---

## Phase 7 — Correctness & runtime robustness

**Goal:** Fix latent correctness/robustness bugs with the smallest surgical changes to keep `kirn test` and `scripts/gates_linux.sh` green.
**Must Follow:** No feature creep; apply fixes in C++ seed; use `fn`/`var`/CapCase in any new snippets; verify with `kirn test`, `scripts/gates_linux.sh`.
**Important Note:** Preserve corpus semantics; prefer cycle guards and safe defaults. > Reference: CPython — cycle-safe repr (`cpython/Objects/object.c`); Go — runtime correctness patterns (`go/src/runtime/` in `/home/rkriad585/Projects/langs-source-code/go`).

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

## Phase 8 — Tooling: `kirn fmt/repl/check/lint/get`

**Goal:** Provide "one executable like `go`" with minimal surface (`kirn fmt/repl/check/lint/get/build/run/test/vet/doc/env`) while keeping build to `kirn` + `kirnc` only as product surface.
**Must Follow:** Driver unifies verbs; idempotent `fmt --check`; use `fn`/`var`/CapCase in any examples; verify with `kirn test`, `scripts/gates_linux.sh`.
**Important Note:** Implement in Go for driver speed/concurrency (natural) or C++ if consistent; dogfood later. > Reference: Go — `go` CLI (`go/src/cmd/go/main.go`) in `/home/rkriad585/Projects/langs-source-code/go`; Zig — build tooling patterns (`zig/src/` in `/home/rkriad585/Projects/langs-source-code/zig`).

**Go accent — the `kirn` driver:**

```go
// tools/kirn/main.go
func main() {
    cmd := os.Args[1]
    switch cmd {
    case "fmt":     os.Exit(runFmt(os.Args[2:]))
    case "check":   os.Exit(runCheck(os.Args[2:]))
    case "lint":    os.Exit(runLint(os.Args[2:]))
    case "repl":    os.Exit(runRepl())
    case "get":     os.Exit(runGet(os.Args[2:]))
    default:        fmt.Fprintln(os.Stderr, "kirn <build|run|test|fmt|check|lint|repl|get|env>")
    }
}
```

**Exit:** `kirn fmt --check` idempotent on the corpus; `kirn check`/`lint`/`repl` smoke green.

---

## Phase 9 — Native backend breadth

**Goal:** Widen native lowering to strings/lists and `main` itself with smallest surface, to reduce interpretation while keeping `kirn` + `kirnc` minimal.
**Must Follow:** Hybrid calls for non-scalar; keep VM/tree-walker parity; use `fn`/`var`/CapCase in examples; verify with `kirn test`, `scripts/gates_linux.sh`.
**Important Note:** Fix lowered-`main` exit-code discrepancy; aim ASan/UBSan clean; measure against corpus. > Reference: LLVM — lowering patterns (`llvm/lib/CodeGen/` in `/home/rkriad585/Projects/langs-source-code/llvm`); Zig — backend lowering (`zig/src/codegen/` in `/home/rkriad585/Projects/langs-source-code/zig`).

**C++ (extend `src/backend/native.cpp`):** lower strings/lists; hybrid calls to non-lowered fns;
finally lower `main` itself; resolve the lowered-`main -> int` exit-code discrepancy (350→0);
`--release`/`-O2` on emitted C++.

```cpp
// src/backend/native.cpp — escape for non-scalar to a runtime call
if (!isScalar(ty)) out += callRuntime("kirn_rt_str_concat", args) + ";";
```

**Exit:** native/VM/tree-walker differential green on 100% of corpus; ASan/UBSan clean; fib/string/
JSON benchmarks within documented factor of C.

---

## Phase 10 — Full borrow checker & memory safety v1

**Goal:** Deliver staged memory safety ("safe like Rust") with smallest viable pass first (conservative) then MIR+borrowck under `--strict`.
**Must Follow:** Phase-5.5 conservative pass default-on; non-strict runtime unchanged; use `fn`/`var`/CapCase; verify with `kirn test`, `scripts/gates_linux.sh`.
**Important Note:** Keep staged delivery to avoid over-engineering. > Reference: Rust — `rustc_borrowck`/MIR (`rustc_borrowck/src/` in `/home/rkriad585/Projects/langs-source-code/rust`).

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

**Exit:** `tests/borrow/*.kn` positive+negative; ASan-clean; runtime unchanged when not `--strict`.

---

## Phase 11 — Concurrency & GC finalize

**Goal:** Achieve Go-grade `spawn/chan/select` safety and a GC story with smallest unblocking surface.
**Must Follow:** Sendability checks for `spawn`; `sync` module; cycle-safe repr; use `fn`/`var`/CapCase in examples; verify with `kirn test`, `scripts/gates_linux.sh`.
**Important Note:** ASan/TSan clean for demos; keep refcount+weak default, `--gc` later. > Reference: Go — runtime scheduler/channels (`go/src/runtime/`), sync (`go/src/sync/`) in `/home/rkriad585/Projects/langs-source-code/go`.

- Sendability checker for `spawn` payloads; worker pool; structured concurrency.
- `sync` module (mutex/rwlock/once/barrier/atomic).
- GC under `--gc` (precise tracing vs ephemeron cycles), keep `weak` semantics.

```go
// cmd/kirn sync mutex (Go) if desired — or C++ (sync.Mutex)
type Mutex struct{ ch chan struct{} }
func NewMutex() *Mutex       { return &Mutex{ch: make(chan struct{}, 1)} }
func (m *Mutex) Lock()       { m.ch <- struct{}{} }
func (m *Mutex) Unlock()     { <-m.ch }
```

**Exit:** 10k-task crawler ASan + TSan clean; `sync` tests green; `repr` cycle-safe.

---

## Phase 12 — CLI/UX & ecosystem

**Goal:** Deliver standards-aligned CLI + minimal registry while keeping toolchain to exactly `kirn` + `kirnc`.
**Must Follow:** Verbs `<build|run|test|vet|fmt|doc|env|check|lint|get|publish/install>` surface via `kirn`; config `KIRNCACHE`, `kirn.mod`, `kirn env`; use `fn`/`var`/CapCase in examples; verify with `kirn test`, `scripts/gates_linux.sh`.
**Important Note:** Help < 40 lines; cross-platform install/publish. > Reference: Go — CLI UX (`go/src/cmd/go/`); V — package manager (`v/cmd/v` in `/home/rkriad585/Projects/langs-source-code/v`).

- `kirn <build|run|test|vet|fmt|doc|env|check|lint>`; config via `KIRNCACHE`, `kirn.mod`,
  `kirn env`.
- Package ecosystem: `kirn publish/install/get`, registries, checksums, `pin.kn`.

**Exit:** `kirn help` < 40 lines; `kirn env | grep KIRNCACHE` works; install/publish CJE on win/linux/mac.

---

## Milestones

- **M0 (P1-4):** catchable errors; OOP `class/interface/record/fn/dynamic`; patterns at Rust parity; memory/scoping keywords; multi-statement closures. **[COMPLETE — see status notes on Phases 1/2/3/3.5 and examples 35–44.]**
- **M1 (P4-6):** closures/expressions; type system; stdlib in Kirn. **[P4 closures + P6 stdlib largely DONE; P5 type-system hardening in progress.]**
- **M2 (P7-9):** runtime correctness; tooling; native breadth.
- **M3 (P10-12):** borrow checker, concurrency/GC, standard CLI.

**Goal:** Keep gating aligned with these milestones and ensure `kirn test`, `scripts/gates_linux.sh` pass at each gate.
**Must Follow:** Verify milestone completion against gates; use `fn`/`var`, CapCase types.
**Important Note:** Ordering/priority preserved; focus on still-open items for fastest value. > Reference: Go — milestone-based release model (`go/RELEASE.md` in `/home/rkriad585/Projects/langs-source-code/go`).

---

## Ownership map (across this repo's plans) — deduplication guide

Feature families span several plans. To keep each feature in **exactly one** place and cross-ref the rest, own by:

| Feature family                                                                                                                    | Owner file                              |
| --------------------------------------------------------------------------------------------------------------------------------- | --------------------------------------- |
| Stdlib **module** packaging/APIs (`json, os, io, math, time, collections, path, regexp, strings`, all future modules)             | `STD_LIBS_PLAN.md`                      |
| Core data **types** internals (big_int, complex, decimal, rational, bytes, collections types, sync types, time/Duration, reflect) | `DATA_TYPE_PLAN.md`                     |
| **Syntax**/sugar/operators (walrus, pipe, comprehensions, splat, `any` keyword, decorators, FFI, patterns)                        | `SYNTAX_PLAN.md`                        |
| Free **builtin** names & math-expression surface (defaults, operator traits, vector math)                                         | `EXP_PLAN.md`                           |
| **Why**/demand drivers + sequencing                                                                                               | `WHY_PLAN.md` (stubs + cross-refs only) |
| Compiler **infra** (VM, AOT, diagnostics, toolchain, borrowck, GC)                                                                | `PLAN.md`, `DO_FIRST_PLAN.md`           |

Consequences already applied in this repo: builtins `map/filter/reduce/sum/min/max/sorted/enumerate/reduce/zip` are spec'd in `EXP_PLAN.md`/`SYNTAX_PLAN.md` (their "why" is `WHY_PLAN.md` WHY-1), not re-spec'd here; math function breadth is partitioned as _builtins→EXP_, _float type→DATA_TYPE_, _module→STD_LIBS_; the `result[T,E]`/`error` combinators live in `DATA_TYPE_PLAN.md` Phase 8 + `EXP_PLAN.md` Phase 14, not here.

**Goal:** Avoid duplication and preserve existing ordering/priority.
**Must Follow:** Respect single-owner rule; use `fn`/`var`, CapCase types in examples; cross-refs only to existing files.
**Important Note:** This plan remains the do-first execution order (DO_FIRST_PLAN.md); other plans describe scope. > Reference: Rust — modular plan docs (`rustc-dev-guide` style) as seen in `/home/rkriad585/Projects/langs-source-code/rust/README.md`.

---

## Ordering rationale

1. **P1 (exceptions)** is a language feature _and_ unblocks every self-referential tool. Fix the crash we actually hit first.
2. **Feature parity while the interpreter is the oracle** is cheap; the same additions later would mean touching multiple implementations at once (interpreter + VM + native).
3. **Stdlib in Kirn now** gives exact parity before the Kirn compiler must depend on it.
4. **Only a language that speaks its own errors can bootstrap honestly** (Go/Rust research).

_Use C++ to extend the seed; Go for the CLI/tooling driver; Rust for the borrow-check/SOLVER and MIR components that map cleanly onto Kirn's model. Do not force a full Rust rewrite._

**Goal:** Preserve this ordering to maximize fastest achievable value while keeping gates green.
**Must Follow:** Execute in phase order; each phase ends with `kirn test`, `scripts/gates_linux.sh`.
**Important Note:** Phases 1–6 are largely shipped; still-open items (Phase 5 hardening, Phase 7 correctness, Phase 9 native breadth, Phase 10 borrowck, Phase 11 concurrency/GC, Phase 12 CLI) are the remaining gate-critical work.

---

## Decision points to ratify before code (draft defaults noted)

1. `class` = Rust-facade over `struct+impl` (**default**) vs Java reference-type-first.
2. Inheritance: **single + `implements` traits** (Java/C#-style) — least surprising.
3. `try/catch` syntax: `catch e { }` binding the message (**default**); keep `try expr` `?`-form.
4. `fn` is the only function keyword (no aliasing); only `var` declares variables.
5. `const` promoted to a real compile-time keyword (**default**).
6. Modules must export types (`pub struct/class/enum`) — a real stdlib milestone.
7. GC default stays refcount+weak (no flag); keep `repr` cycle-safe; `--gc` later.
8. `??` nil-coalescing (**default** include).

**Goal:** Ratify minimal defaults that keep build to `kirn` + `kirnc` and gates green.
**Must Follow:** Enforce `fn`/`var`, CapCase types, width micros/capitalized widths; no legacy-name or extension remnants anywhere.
**Important Note:** Defaults align with "industry-standard, AI-first, minimal" Kirn style. > Reference: Go — explicit choices over overloading (`go/doc/design`); Rust — minimal surface in grammar (`rustc_lexer/src/lib.rs`).

## References

- Gaps: `docs/FEATURE_GAP_ANALYSIS.md`, `PLAN.md`, `docs/KIRN_PLAN.md`.
- Keywords: `src/lex/lexer.cpp:38-50`; errors: `src/interp/runtime.cpp:1290,544`.
- Reference dirs (verified): `/home/rkriad585/Projects/langs-source-code/rust`, `/home/rkriad585/Projects/langs-source-code/go`, `/home/rkriad585/Projects/langs-source-code/cpython`, `/home/rkriad585/Projects/langs-source-code/ruby`, `/home/rkriad585/Projects/langs-source-code/Nim`, `/home/rkriad585/Projects/langs-source-code/Odin`, `/home/rkriad585/Projects/langs-source-code/v`, `/home/rkriad585/Projects/langs-source-code/zig`, `/home/rkriad585/Projects/langs-source-code/llvm`.
- Model sources (web): class-vs-struct semantics (C#/Swift/C++); Go vs Java/C# OOP; traits-vs-interfaces; C# `record`/`interface`; Kotlin `data class`. Takeaway: keep the Rust/Go **composition** model and add `class`/`interface`/`record` as ergonomic, semantics-preserving facades.

**Goal:** Cite only verified paths; keep references minimal.
**Must Follow:** Use exact verified dirs; no speculative URLs beyond those required.
**Important Note:** All references above are top-level verified dirs under `langs-source-code/`.
