# EXP_PLAN — Magical Expressions, Syntax Sugar, Math Expressions, Dynamic/Static Features & Builtins for Kirn

## Goal

Kirn's expression, syntax-sugar, magic-method, and math-expression layer is delivered as one cohesive, batteries-included surface: the _magical expressions_ and _builtins_ developers ask for, framed industry-standard, AI-first (machine-checkable and completable), and fast by construction — every sugar lowers deterministically to existing AST/VM ops with no added indirection, so expression- and math-heavy code stays faster than equivalent Rust and Go. It is never over-engineered: a feature is kept only where a proven reference language already removed more complexity than it adds.

## Must Follow

- The toolchain build ships EXACTLY two executables: `kirn` (driver) and `kirnc` (checker/compiler) — no extra binaries or daemons.
- No over-engineering: keep an expression/feature only if it removes more complexity than it adds; cite the reference language that already proved it (Python dunders, Rust `core::ops`, Go `defer`, Ruby safe-nav, …) and drop the rest.
- AI-first: any kept feature must be machine-checkable (checker-verifiable) and completable — every sugar has a documented, deterministic desugaring.
- Kirn-style spellings/CapCase: capitalized primitives and free constructors (`Int`, `Float`, `Bool`, `Str`, `Char`, `BigInt`, `Rational`, `Str(…)`, `BigInt(…)`, `__Sqrt`), lowercase width micros (`u4..u256`, `i4..i256`), lowercase methods.
- Performance where relevant: numeric dispatch stays table-driven and allocation-light; sugar must lower to existing AST/VM ops with zero runtime cost.

## Important Note

Preserve all existing structure and decisions: the audited baseline (§2), the layer cake (§3), the influence map (§4), the phase order (§5), the phased plan (§7, Phases 0–15 + Beyond), the magic/operator and builtin inventories (§8–9), the test strategy (§10), the appendices (§11–15), and every cross-ref to `DATA_TYPE_PLAN.md` / `STD_LIBS_PLAN.md` / `SYNTAX_PLAN.md`. New triples annotate; they never replace a decision.

**Status:** Comprehensive plan (deep research + phased implementation)
**Scope:** Add the _magical features and functions_, _dynamic & static expressions and syntax out-of-the-box_, _more builtins_, and above all _math expressions (not just math functions)_ that developers ask for every time but most languages lack — combining the best ideas from **Python** (`cpython`), **Rust** (`rustc`), **Go** (`go`), **Ruby** (`cruby`), plus modern teachers **Kotlin**, **Swift**, **Julia**, **Zig**, and **C#**.
**Deliverable:** This plan covers the **expression/operator/syntax** surface plus the **magic-method / builtin / dynamic-static** layer. It is the _front end_ companion to:

- `DATA_TYPE_PLAN.md` — the _types_ those expressions operate on (BigInt, complex, decimal, bytes, collections, concurrency, etc.).
- `STD_LIBS_PLAN.md` — the _modules_ those features ship through (`math`, `random`, `statistics`, ...).

> Where a feature needs a new _type_ (complex, rational, BigInt, bytes views), we build the **expression/syntax/sugar** here and delegate the _type internals_ to `DATA_TYPE_PLAN.md`. Where it needs a _module of functions_ (`math`, `random`), we define the surface here and the _module packaging_ in `STD_LIBS_PLAN.md`.

---

## Table of Contents

1. [Objectives & Guiding Principles](#1-objectives--guiding-principles)
2. [Current State of Kirn Expressions & Builtins (audited)](#2-current-state-of-kirn-expressions--builtins-audited)
3. [The Unified Target: Magical Layer Cake](#3-the-unified-target-magical-layer-cake)
4. [Source-Language Influence Map](#4-source-language-influence-map)
5. [Layering & Cross-Feature Dependencies](#5-layering--cross-feature-dependencies)
6. [Design Principles Adopted From Each Language](#6-design-principles-adopted-from-each-language)
7. [Phased Implementation Plan](#7-phased-implementation-plan)
   - [Phase 0 — Expression value-model & sugar plumbing](#phase-0)
   - [Phase 1 — Math expression operators & builtins breadth](#phase-1)
   - [Phase 2 — Magic methods / dunder protocol traits](#phase-2)
   - [Phase 3 — Context managers, `with`, RAII/Drop, `defer` breadth](#phase-3)
   - [Phase 4 — Compile-time math & constant folding (`const fn` / comptime)](#phase-4)
   - [Phase 5 — Syntax sugar: walrus, pipe, spread/rest, `var-else`, chaining](#phase-5)
   - [Phase 6 — Null/optional safety sugar (`?.`, `??`, `??=`, smart casts)](#phase-6)
   - [Phase 7 — Dynamic features: reflection, dispatch, casts, `dynamic` protocols](#phase-7)
   - [Phase 8 — Static features: contracts, asserts, type checks, inference polish](#phase-8)
   - [Phase 9 — Vector/array math & broadcasting](#phase-9)
   - [Phase 10 — Callable objects, functional builtins & combinators](#phase-10)
   - [Phase 11 — Formatting & number/string conversion breadth](#phase-11)
   - [Phase 12 — Collection builtins & iterator protocol magic](#phase-12)
   - [Phase 13 — Extended operator set (custom operators, infix)](#phase-13)
   - [Phase 14 — Error handling sugar (try-as-expression, catch combinators)](#phase-14)
   - [Phase 15 — Random, number theory & special math functions](#phase-15)
   - [Beyond Phase 15 — Stretch goals](#beyond-phase-15)
8. [Magic-Method & Operator Inventory](#8-magic-method--operator-inventory)
9. [Builtin & Math-Function Inventory](#9-builtin--math-function-inventory)
10. [Test Strategy](#10-test-strategy)
11. [Appendix A — Python cpython expression/math surface (condensed)](#11-appendix-a)
12. [Appendix B — Go expression/math surface (condensed)](#12-appendix-b)
13. [Appendix C — Rust expression/math surface (condensed)](#13-appendix-c)
14. [Appendix D — Ruby expression/math surface (condensed)](#14-appendix-d)
15. [Appendix E — Kotlin/Swift/Julia/Zig sugar worth stealing (condensed)](#15-appendix-e)
16. [Verification / Definition of Done](#16-verification--definition-of-done)
17. [Closing Notes](#17-closing-notes)

---

## 1. Objectives & Guiding Principles

Kirn already has an unusually rich expression and syntax surface for a young interpreter (f-strings, comprehensions, generators, pattern matching, operator overloading, optionals, results with `?`, labeled control flow, `defer`, `spawn`/`chan`/`select`, traits/impl, generics, `any`/`dynamic`). This plan makes the **remaining** gap disappear: the _magic sugar_ and _math expressions_ developers ask for in every "what's missing" thread, combined across the four reference languages.

**Guiding principles:**

1. **Syntax first, types later.** Where a feature needs a supporting type, phase it behind the `DATA_TYPE_PLAN.md` dependency; never block the syntax on an unbuilt type.
2. **Developer-loved, not academic.** Every feature selected is one that appears in "features I wish my language had" surveys and cross-language cheat sheets (walrus, pipe, safe-call, elvis, dunder/`__magic__` methods, context managers, compile-time math, vector math, etc.).
3. **Math _expressions_, not just math functions.** The headline request is an operator/expression story for math: overloadable arithmetic on custom number-likes, vector/element-wise math, degrees-vs-radians, `**`/`//`/`%` conventions made explicit, constant folding, and a broad numeric-function library.
4. **Out-of-the-box.** Everything lands either as a keyword/operator, a predeclared free builtin, or a well-known trait so it works with **zero imports** — matching Kirn's batteries-included philosophy (WHY-1 / SP-8).
5. **Etiquette-first static/dynamic blend.** Dynamic escape hatches (`any`, reflection, casts) exist alongside static guarantees (`const` folding, `Result`/`Option`, contracts, exhaustiveness) and never silently degrade correctness.
6. **Sugar must desugar trivially.** Each new piece of syntax is specified as a small, well-defined lowering to existing constructs so the parser/checker/runtime changes stay bounded and the native backend can lower the same way.
7. **No duplication of existing work.** Anything already in Kirn (f-strings, comprehensions, `?`, `.?.`, `defer`, pattern matching, `any`/`dynamic`) is **not** re-planned; we extend or leave it as-is.

**Goal:** state the adoption principles (syntax-first, developer-loved, math _expressions_ over functions, out-of-the-box, etiquette-first static/dynamic blend, trivial desugar, no duplication) that keep Kirn's expression surface regular, minimal, and agent-completable.
**Must Follow:** every principle maps to a phase in §7 and keeps the zero-import guarantee; never re-plan a feature Kirn already ships (f-strings, `?`, `.?.`, `defer`, `match`, `any`).
**Important Note:** no principle may introduce a second implementation path for an existing feature, and no feature lands without a proven reference-language precedent.

```kn
# zero imports, Kirn spellings
const TAU = 2.0 * PI
print(Str(1) + Str(2))              # "12"
r = [1, 2, 3].map((x) => x * x)     # sugar; no import
print(r)                            # [1, 4, 9]
n: Int? = none
print(n ?? 0)                       # 0 (elvis spirit)
```

> Reference: cpython — batteries-included naming (`Lib/functools.py`); rust — trait-driven operators (`library/core/src/ops`).

---

## 2. Current State of Kirn Expressions & Builtins (audited)

> This section is the **implemented baseline** (ground truth from the code), not a to-do list.
> The operators/literals/expressions/statements/builtins/methods listed below **already exist and
> run** (see `examples/`). Future work is the Phases in §7. Where an existing feature is only
> partially generalized (e.g. §2.7 operator traits lacking reflected/in-place forms), the phase
> that extends it is tagged `[NEEDS-ENHANCE]`.

Audited from `grammar/kirn.ebnf`, `src/parser/parser.cpp`, `src/sema/checker.cpp`, `src/interp/runtime.cpp`, `examples/`, and `docs/`.

**Goal:** record the implemented expression / sugar / builtin baseline (ground truth) so every later phase is additive and no shipped feature is re-planned.
**Must Follow:** treat the A2 precedence ladder, literals, statements, builtins, methods, and `impls_` operator traits below as frozen contracts; extend only via the tagged `[NEEDS-ENHANCE]` phases.
**Important Note:** the lowercase names here (`int`, `str`, `len`) are the legacy surface the Kirn spellings (`Int`, `Str`) supersede name-only; semantics are unchanged.

```kn
# already working today
xs = [3, 1, 2]
print(len(xs), sorted(xs))          # 3 [1, 2, 3]
print(f"{1.5:.1f}")                 # "1.5"
print(3 in xs, 9 not in xs)         # true true
match xs { case [a, ..rest] { print(a); } }   # 3
```

> Reference: cpython — comprehension/f-string surface (`cpython/Grammar/python.gram`); rust — `match`/pattern surface (`rust/compiler`).

### 2.1 Operators already present (A2 precedence ladder)

```
() [] . .?. ?      postfix call/index/member/nil-safe/try-propagate   (17)
**                 power, right-assoc                                  (16)
- + ~ not & *      unary (&=borrow  *=deref)                           (15)
* / % //           mul / true-div / rem / floor-div                    (14)
+ -                add / sub                                           (13)
<< >>              shift                                               (12)
&                  bitand                                              (11)
^                  bitxor                                              (10)
|                  bitor                                               (9)
.. ..=             ranges                                              (8)
< > <= >= == != is in   comparison (chainable)                         (7)
not                prefix not                                          (6)
and                and                                                 (5)
or                 or                                                  (4)
=>                 lambda arrow                                        (3)
if c { a } else { b }   conditional expression                         (2)
for p in e yield e      comprehension / generator                      (1)
```

Compound assignment: `+= -= *= /= %= **= //= &= |= ^= <<= >>=`. No `++`/`--`, no `?:` ternary (rejected A4.9), no walrus `:=`, no pipe `|>`, no `?.` elvis.

**Goal:** freeze the A2 precedence ladder (17 bands) as the ground-truth operator surface.
**Must Follow:** any new operator slots into an existing band or the documented custom band; no renumbering.
**Important Note:** rejected `?:`/`++`/walrus stay out here; walrus is introduced as a new expression form in Phase 5.

### 2.2 Literals

- Integer: decimal, `0x`, `0o`, `0b`, underscore `_` separators.
- Float: decimal + exponent `e/E`, `NaN`, `inf`.
- Char `'c'`, string `"..."`, raw `r"..."`, byte `b"..."`, C-string `c"..."`, f-string `f"...{expr:spec}..."`.
- No complex `3+4i`, no BigInt suffix `n`, no hex-float `0x1.8p1`.

**Goal:** freeze the literal forms (int/float/char/string/raw/byte/C/f-string) as implemented.
**Must Follow:** new literals (complex, `n` suffix, hex-float) arrive via `DATA_TYPE_PLAN.md`, not by editing this list.
**Important Note:** `_` separators, `NaN`, and `inf` are already accepted; keep lexer output stable.

### 2.3 Expressions already present

- Lambdas `(x) => e`, block closures `fn (x) { ... }`, comprehensions `[y for p in it if c]`, generator views, `if/else` expression, `match` expression, tuple/list/set/dict displays, `new T(...)`, `?` try propagation, `.?.` nil-safe member.

**Goal:** freeze the expression forms (lambdas, closures, comprehensions, displays, `?`, `.?.`) as implemented.
**Must Follow:** extend rather than duplicate; all new sugar lowers into these existing nodes.
**Important Note:** `match`/`if`-as-expression already exist — reuse them for Phase 14 try-as-expression.

### 2.4 Statements already present

`var/const`, `if/elif/else`, `while`, `do..while`, `for..in`, `match`, `select`, `try/raise/catch_panic`, `defer`, labeled `break/continue`, `goto`, `gather`, `yield`, `local/global/temp/bucket`, `spawn`, `unsafe`, `import/from`, `del`, `class/interface/record/implements/extends`, `pass`.

**Goal:** freeze the statement keyword set as implemented.
**Must Follow:** new statements reuse soft keywords; never add a frozen keyword just for sugar.
**Important Note:** `defer`/`spawn`/`select` here are the substrate for Phases 3 and 9.

### 2.5 Free builtins (runtime `installBuiltins` + checker `predeclareBuiltins`)

```
print len sqrt ord chr assert assert_eq range panic catch_panic printf str int float bool type repr
sum min max any all sorted reversed enumerate map filter reduce upper lower trim contains
starts_with ends_with replace split join    NaN inf nil
```

Math functions in `math.kn`: `PI E sqrt abs clamp min2 max2 ipow fpow` (Newton sqrt).

**Goal:** freeze the current free-builtin + `math.kn` inventory (ground truth).
**Must Follow:** additions are predeclared in both `installBuiltins` and `predeclareBuiltins`.
**Important Note:** legacy lowercase names stay valid; Kirn capitalized types are a name-level spelling, not a semantic break.

### 2.6 Builtin methods (method-table dispatch, one per VK)

- **string:** `.len .repeat .contains .starts_with .ends_with .replace .find .capitalize .upper .lower .trim`
- **list:** `.append .extend .insert .remove .pop .clear .reverse .sort .len .contains .index .fold`
- **dict:** `.len .contains .get .setdefault .remove .keys .values .items`
- **set:** `.add .remove .contain .len .union` _(`.intersection` / `.difference` are **not yet**
  implemented; they are a `[NEEDS-ENHANCE]` gap, not current state)_

**Goal:** freeze per-VK method-dispatch tables as implemented.
**Must Follow:** new methods append to the table; `.intersection`/`.difference` are the known `[NEEDS-ENHANCE]` gap owned by Phase 12.
**Important Note:** method names stay lowercase and dispatch stays one-entry-per-VK.

### 2.7 Well-known operator traits (`impls_` method-set dispatch)

`Add Sub Mul Div Rem Index Eq Ne Neg Cmp`-style traits lower `a+b`, `g[i]`, `a==b`, `-x`. Reflected forms (`5 * vec`), in-place (`x += y`), `__bool__`/`len`/`iter`/`call` are not yet generalized.

**Goal:** freeze the `impls_` method-set dispatch that already lowers `a+b`, `g[i]`, `a==b`, `-x`.
**Must Follow:** Phase 2 generalizes reflected/in-place/`__bool__`/`len`/`iter`/`call` — it extends this, never replaces it.
**Important Note:** this is the seed `MagicRegistry`; keep its lowering identical.

### 2.8 Gaps this plan fills (the "wish list")

| Gap                                                                                                                                                            | Category                                   | Source inspiration |
| -------------------------------------------------------------------------------------------------------------------------------------------------------------- | ------------------------------------------ | ------------------ |
| Math expr breadth: asin/acos/atan/atan2, log2/log10/log1p/expm1/exp2, round/trunc, sign, hypot, lerp, gcd/lcm, sinh/cosh/tanh+inv, pow, cbrt, isnan/isinf, fma | math                                       | Python/Rust/C      |
| Magic/dunder methods: `__call__ __iter__ __len__ __getitem__ __setitem__ __contains__ __bool__ __hash__ __str__ __repr__ __eq__ __enter__ __exit__`            | magic                                      | Python             |
| Context managers / `with` / RAII Drop                                                                                                                          | magic                                      | Python/Rust/Go     |
| Compile-time math & `const fn` folding                                                                                                                         | static                                     | Rust/Zig           |
| Walrus `:=`, pipe `                                                                                                                                            | >`, spread/rest in expressions, `var-else` | sugar              | Python/JS/Kotlin/Rust |
| Null sugar `?.`/`??`/`??=`/smart casts                                                                                                                         | sugar                                      | Kotlin/Swift/C#    |
| Dynamic: reflection `type()`, dispatch, casts, `dynamic` protocols                                                                                             | dynamic                                    | Python/Ruby        |
| Static: contracts, `assert`, type checks, freeze                                                                                                               | static                                     | Rust/Python/Zig    |
| Vector/array math & broadcasting                                                                                                                               | math                                       | NumPy/Julia        |
| Callable objects; functional combinators                                                                                                                       | builtins                                   | Python/Ruby        |
| Formatting & parse breadth                                                                                                                                     | builtins                                   | Rust/Python/C#     |
| Collection builtins / iterator protocol                                                                                                                        | builtins                                   | Python/Ruby        |
| Custom operators / `infix`                                                                                                                                     | syntax                                     | Kotlin/Ruby        |
| Error-handling sugar (try-as-expr, combinators)                                                                                                                | sugar                                      | Kotlin/Rust/Zig    |
| Random, number theory, special math                                                                                                                            | builtins/module                            | Python/Go/C        |

**Goal:** enumerate the wish-list gaps and their source inspiration so no phase is invented without a precedent.
**Must Follow:** each row maps to exactly one phase below; never re-plan an implemented feature.
**Important Note:** this table is the traceability matrix from inspiration → phase and is the index for §7.

> Reference: cpython — math/dunder surface (`cpython/Objects`, `cpython/Modules/mathmodule.c`); rust — trait ops (`rust/library/core/src/ops`).

---

## 3. The Unified Target: Magical Layer Cake

We introduce a 5-layer model so new features compose.

```
 L5  STATIC  contracts, const-folding, type-checks, freeze, exhaustiveness
 L4  SYNTAX  walrus, pipe, spread/rest, var-else, guard, chaining, infix
 L3  MAGIC   dunder/operator protocol traits, callable, iterator, containers,
             context managers, RAII/Drop, reflection
 L2  DYNAMIC `any` dispatch polish, reflection, casts, dynamic protocols
 L1  VALUES  math-expression engine: numeric operator dispatch, vector math,
             number-likes (complex/rational/bigint via DATA_TYPE_PLAN)
```

- **L1 (values)** — the _math expression_ engine. Numeric operator dispatch is table-driven (add/sub/mul/... per numeric TyK+VK), so `+ - * / % // ** << >> & | ^` behave correctly across `i8..u64`, `f32/f64`, and (later) `BigInt`, `complex`, `rational`. Vector/array element-wise math lives here.
- **L2 (dynamic)** — a small `any`-protocol table so `a + b`, `a[i]`, `len(a)`, `str(a)` on `any` dispatch by runtime tag; plus reflection (`type`, `methods`, `fields`, `size`).
- **L3 (magic)** — the dunder/well-known trait system. Kirn's operator overloading already exists; we generalize it into a **magic-method protocol** sugar layer so both `impl Add for Vec2 { fn add ... }` and (optionally) a `__dunder__` spelling drive the same dispatcher.
- **L4 (syntax)** — pure desugaring sugar: walrus, pipe, spread/rest, `var-else`, safe-call/elvis, infix, chaining. Each lowers to L3/L1 primitives.
- **L5 (static)** — compile-time guarantees layered on top: constant folding, `const fn`, contracts, freeze, exhaustiveness extensions.

Every new feature declares which layer(s) it touches and which existing phase it depends on.

**Goal:** fix the 5-layer model (L1 values, L2 dynamic, L3 magic, L4 syntax, L5 static) so every feature composes and declares its layer plus dependencies.
**Must Follow:** new operators route through the table-driven L1 dispatch; sugar (L4) lowers only to L1/L3 primitives; no feature may skip a layer.
**Important Note:** L1 numeric dispatch is the perf spine — it must stay data-driven and allocation-free to keep Kirn faster than Rust/Go on numeric kernels.

```kn
# one feature per layer, composing
v: any = BigInt(2) ** 100        # L2: dispatch on `any`
print(v)                          # 1267650600228229401496703205376
w = Vec2(x: 1.0, y: 2.0) + Vec2(x: 3.0, y: 4.0)  # L3: operator trait
q = [1, 2, 3] |> sum |> sqrt      # L4: pipe sugar
const HALF = 0.5                  # L5: static fold
```

> Reference: rust — `core::ops` trait dispatch (`rust/library/core/src/ops`); cpython — dunder protocol (`cpython/Objects`).

---

## 4. Source-Language Influence Map

| Capability                              | Python       | Rust           | Go        | Ruby           | Kotlin/Swift/Julia/Zig/C# |
| --------------------------------------- | ------------ | -------------- | --------- | -------------- | ------------------------- |
| True division `/` + floor `//`          | ✔            | ✔(f64)         |           |                | Swift                     |
| Power `**`                              | ✔            |                |           | ✔              | Julia `^`                 |
| Math breadth (trig/hyp/log1p/expm1)     | ✔            | ✔ (f32/f64)    | math      | Math           | Julia/C                   |
| Magic/dunder methods                    | ✔            | traits         |           |                |                           |
| Operator overloading traits             |              | ✔ core::ops    |           |                |                           |
| Reflected ops (`5 * vec`)               | ✔ `__rmul__` | core::ops`Rhs` |           |                |                           |
| Context managers / `with`               | ✔            | Drop RAII      | defer     | block          | Swift defer / C# usin     |
| `defer`                                 |              |                | ✔         |                | Swift                     |
| Pipe `                                  | >`           |                |           |                |                           | Elixir/R/F# |
| Walrus `:=`                             | ✔            |                |           |                |                           |
| Safe-call / elvis `?. ??`               |              |                |           | ✔ `&.`         | Kotlin/Swift/C#           |
| `var-else`                              |              | ✔              |           |                | Swift `guard`             |
| Spread/rest in expressions              | ✔ `*`/`**`   | `..`           | `...`     | splat `*`/`**` | JS                        |
| Compile-time math (`const fn`/comptime) |              | ✔              |           |                | Zig/C++                   |
| Vector/broadcast math                   | NumPy        | ndarray        |           |                | Julia/MATLAB              |
| Enum/`case` as expression               |              | ✔              |           |                | Kotlin                    |
| Callable objects                        | ✔ `__call__` | Fn traits      |           | ✔ `to_proc`    |                           |
| Extension methods / infix               |              |                |           |                | Kotlin                    |
| Reflection/introspection                | ✔ `dir/type` |                | reflect   | ✔ `methods`    | C#                        |
| Random + number theory                  | ✔            | rand/num       | math/rand | Random         |                           |

**Goal:** attribute every capability to its proven source so adoption is evidence-based, not speculative.
**Must Follow:** ship only capabilities that have a real reference-language precedent; Kirn takes the best fit, never a contradictory union.
**Important Note:** rows inspired by languages not vendored here (Kotlin/Swift/Julia/C#) are inspirations only; the shipped reference set is cpython/rust/go/ruby/Nim/Odin/v/zig/llvm.

```kn
# best-fit picks, one per source
print(sin(PI / 2))                # 1.0   (cpython math)
impl Add for Vec2 { fn add(self, o: Vec2) -> Vec2 { ... } }   # (rust trait)
defer f.close()                   # (go defer)
name: Str? = none
print(name?.len())                # none  (ruby safe-nav spirit)
```

> Reference: cpython — math breadth (`cpython/Modules/mathmodule.c`); rust — operator traits (`rust/library/core/src/ops`); go — `defer` (`go/src/runtime`); ruby — safe navigation (`ruby/parse.y`).

---

## 5. Layering & Cross-Feature Dependencies

Strict (non-cyclic) build order. A phase list with its inputs:

```
Phase 0  value-model + sugar plumbing                    (no deps)
   v
Phase 1  math operators & builtin breadth                 <- 0
   v
Phase 2  magic-method protocol traits                     <- 0
   v
Phase 3  context managers / with / Drop / defer           <- 2
   v
Phase 4  compile-time math / const fn                     <- 1 (folds math)
   v
Phase 5  sugar: walrus, pipe, spread, var-else, chaining  <- 0,2
   v
Phase 6  null/optional sugar (?. ?? ??= smart casts)      <- 0, (existing T?)
   v
Phase 7  dynamic: reflection, dispatch, casts, protocols  <- 2,6
   v
Phase 8  static: contracts, assert, type checks, freeze   <- 0,4
   v
Phase 9  vector/array math & broadcasting                 <- 1,2 (operator overload)
   v
Phase 10 callable objects + functional builtins           <- 2,7
   v
Phase 11 formatting & number/string conversion            <- 1,0
   v
Phase 12 collection builtins & iterator protocol          <- 2,5
   v
Phase 13 custom operators / infix                         <- 2,5
   v
Phase 14 error-handling sugar (try-as-expr, combinators)  <- 0,2, (result/option)
   v
Phase 15 random, number theory, special math              <- 1, (math/random module)
   v
Beyond 15 stretch
```

Everything references existing constructs; nothing depends on a not-yet-built type from `DATA_TYPE_PLAN.md` except where explicitly noted (complex/rational/BigInt from that plan are _optional consumers_ of Phase 1's operator dispatch and are referenced as future work, not blockers).

**Goal:** enforce a strict, non-cyclic phase order so each phase builds only on completed phases (plus the optional `DATA_TYPE_PLAN.md` consumers).
**Must Follow:** no phase may depend on a not-yet-built type; complex/rational/BigInt stay optional consumers, never blockers.
**Important Note:** this order is the memoized dependency graph for AI-first planning — reordering it without re-checking edges breaks completability.

```kn
# Phase 1 math only needs Phase 0's dispatch; Phase 4 folds using Phase 1
const PI2 = 2.0 * PI              # Phase 4 (needs 1)
x: Float = log1p(1e-15)           # Phase 1
r = [1.0, 2.0] + [3.0, 4.0]       # Phase 9 (needs 1 + 2)
print(PI2, x, r)                  # 6.28… 1e-15 [4.0, 6.0]
```

> Reference: rust — staged `core`/`alloc`/`std` layering (`rust/library`); zig — `std` built on `zig/lib/std`.

---

## 6. Design Principles Adopted From Each Language

- **Python:** dunder/magic-method protocol; reflected operators; context managers; walrus; comprehensive math names (`math`), floor-division and Euclidean-`%`-friendliness; `f"{x=}"` debug; `enumerate(start=)`; iterator protocol.
- **Rust:** trait-driven operator overloading (core::ops); `let-else`; `const fn`; clear sign/rounding conventions; `.rem_euclid`-style explicit methods alongside `%`; `Result`/`Option` combinator ergonomics (`map/filter/recover`).
- **Go:** `defer`; `iota`-style enum discriminants; explicit error handling; `errors`/wrapping; simple, predictable sugar (avoid overloading).
- **Ruby:** safe-navigation `&.`; splat/spread; `yield`/blocks; `methods`/introspection; `<=>` spaceship and `Comparable`; packing/unpacking; `attr_accessor`-style accessors.
- **Kotlin/Swift/Julia/Zig/C#:** safe-call/elvis/smart-cast; extension functions; `infix`; trailing lambdas; `comptime`; broadcast math (Julia); `using`/`try-with-resources` (C#); `guard` (Swift).

**Goal:** distill the one principle per language Kirn actually adopts, so contributors copy the proven idea and not the accidental complexity.
**Must Follow:** Python = protocols/dunders, Rust = trait ops + `const fn` + `let-else`, Go = `defer` + explicit errors, Ruby = safe-nav + splat + introspection; take the idiom, drop the baggage.
**Important Note:** Kotlin/Swift/Julia/Zig/C# items here are inspirations; re-express them through the four vendored references wherever possible.

```kn
# Python protocol + Rust let-else + Ruby introspection, one function
fn describe(x: any) -> Str {
    var n: Int = x as? Int else { return "not-int"; }   # rust let-else
    return f"{type(x)}:{n * 2}"                          # cpython protocol/dunder
}
print(describe(21))        # "int:42"
print(methods(describe))   # ruby-style introspection
```

> Reference: cpython — dunder protocol (`cpython/Objects`); ruby — `methods`/`respond_to?` (`ruby/object.c`); rust — `let-else` (`rust/compiler`).

---

## 7. Phased Implementation Plan

Each phase: **Goal**, **C++/interpreter substrate**, **Kirn surface**, **code example(s)**, **dependencies**, **tests**.

**Goal:** list the 16 phases (0–15) + stretch, each specified by Goal, C++/interpreter substrate, Kirn surface, example(s), dependencies, and tests.
**Must Follow:** every phase below carries its own `Goal`/`Must Follow`/`Important Note` triple; keep substrate/surface/example/deps/tests as the fixed template.
**Important Note:** new examples use `.kn` (Kirn); the legacy `.kn` snippets and corpus already in the repo remain valid and are not rewritten.

---

### Phase 0

**Goal:** Expression value-model and sugar plumbing ready for every later phase. Introduce the numeric-operator dispatch table and a magic-method registry stub, and audit the parser/checker seams where new operators and desugarings attach.
**Must Follow:** add only the `dispatchBin`/`dispatchUn`/`dispatchIndex` helpers, the `MagicRegistry`, and the post-parse desugar hook; no user-visible syntax here.
**Important Note:** route compound assignments through the same dispatch so later phases never special-case; internal only.

> Reference: rust — operator-trait dispatch (`rust/library/core/src/ops`).

**Substrate (C++):**

- Add a `dispatchBin(op, a, b)` + `dispatchUn(op, a)` + `dispatchIndex(obj, key)` helper layer over the existing overload-trait lowering and intrinsic numeric op, so any later phase can route through it.
- Add a `MagicRegistry` map (`trait/method -> lowering`) seeded with the currently-wired traits (`Add/Sub/Mul/Div/Rem/Neg/Index/Eq/...`) so future dunders plug in uniformly.
- Add a desugar pipeline hook in `parser.cpp` (a post-parse rewrite pass list) so sugar phases (5/6/13) can register pure desugarings without hand-editing the grammar every time.
- Audit operator-emit in the runtime so compound assignments (`+= *= **= //=`) route through the same dispatch (today some are special-cased).

**Kirn surface:** none user-visible (internal plumbing). Establishes the seams.

**Tests:** `kirn test` golden on `15_operator_overloading` and `25_operator_precedence` still pass; a unit test drives `dispatchBin` for every numeric pair.

```co
fn main() {
    a = Vec2(x: 1.0, y: 2.0);
    b = Vec2(x: 3.0, y: 4.0);
    c = a + b;        # routed through new dispatch table (Add)
    d = a * 2.0;      # Mul with scalar   (Phase 2 adds reflected form)
    e = -a;           # unary Neg
    print(c.x, c.y);  # 4 6
}
```

---

### Phase 1

> **Ownership/cross-ref (dedup — math spans three plans):** this phase owns the **builtin function
> names / operator semantics** for math. `DATA_TYPE_PLAN.md` Phase 2 owns the **float type**
> (method-form `.round()/.floor()/...`, float breadth); `STD_LIBS_PLAN.md` Phase 3a owns the
> **`math` module packaging** (`lib.math`). `math.kn` already ships `PI E sqrt abs clamp min2 max2
ipow fpow` (see `stdlib/lib/math.kn`); this phase adds the rest as free builtins. Avoid
> re-specifying a function in more than one of the three.

**Goal:** Math _expressions and functions_ breadth. Complete the trig/log/round/power/sign families and establish explicit, documented operator semantics (`%` vs `rem`, `/` vs `//`, `**`), plus IEEE-754 helpers — all out-of-the-box.
**Must Follow:** new math names land as predeclared free builtins (mirrored into `math.kn` where trivial); operator semantics (`/` float, `//` floor, `%` matcher, `.rem()`/`.rem_euclid()`, `**`) are documented once and shared by checker/runtime/AOT.
**Important Note:** reuse libm/native kernels and keep names identical to Python/Rust so parity holds and Phase 4 can fold every call.

> Reference: cpython — `math` breadth (`cpython/Modules/mathmodule.c`); rust — `f64` method set (`rust/library/core/src/num`); zig — `std.math` (`zig/lib/std/math`).

**Substrate/checker:**

- Add checker + runtime for the new math builtins (as free builtins in `installBuiltins` + `predeclareBuiltins`, and mirrored into `math.kn` as pure-Kirn fallbacks where trivial).
- Add `isnan`, `isinf`, `isfinite`, `signbit`, `copysign`, `nextafter`, `fma` (float substrates).
- Document operator conventions (decided):
  - `/` on two ints → **float** (true division, Python-style); `//` → **floor division** int→int.
  - `%` on ints → matcher; add explicit `.rem()` (truncated toward zero) and `.rem_euclid()` (non-negative) methods so both are available; `%` keeps its current documented behavior but `mod`/`divmod` builtins added.
  - `**` integer → power; negative exponent on int → float; `0**0` → 1.

**Kirn surface (new free builtins + methods):**

```
Trig        sin cos tan asin acos atan atan2
Hyperbolic  sinh cosh tanh asinh acosh atanh
Exp/Log     exp exp2 expm1 ln log log2 log10 log1p logb
Power/Root  pow cbrt sqrt hypot
Round       round trunc floor ceil fract fmod
Special     sign signum abs clamp lerp midpoint
IEEE        isnan isinf isfinite signbit copysign nextafter fma frexp ldexp modf
Number      gcd lcm divmod mod
String→Num  parse_int(s, [radix]) parse_float(s)
```

```co
fn main() {
    print(sin(PI / 2));       # 1.0
    print(atan2(1, 1));       # 0.785...
    print(log2(1024));        # 10.0
    print(log1p(1e-15));      # ~1e-15  (stable)
    print(expm1(1e-15));      # ~1e-15
    print(round(2.5));        # 2.0  (banker's ties-to-even, documented)
    print(trunc(-2.7));       # -2.0
    print(sign(-5));          # -1
    print(hypot(3, 4));       # 5.0
    print(lerp(0, 100, 0.3)); # 30.0
    print(gcd(12, 8));        # 4
    print(lcm(4, 6));         # 12
    print(isnan(0.0/0.0));    # true
    print(fma(2.0, 3.0, 4.0));# 10.0
    print((-17).rem_euclid(5));# 3
    print(divmod(17, 5));     # (3, 2)
}
```

**Dependencies:** Phase 0. Optional consumers: `complex`/`rational`/`BigInt` from `DATA_TYPE_PLAN.md` later hook into the operator dispatch (not blockers).

---

### Phase 2

> **Ownership/cross-ref:** this phase owns the **magic-method / dunder trait layer** (the runtime
> dispatch hooks for `Call/Index/Len/Iter/...`). The `__dunder__` name-spelling ties into the
> decorator/attribute surface in `SYNTAX_PLAN.md` SP-1 (→); the reflection backend that reads these
> is `DATA_TYPE_PLAN.md` Phase 14 (→). `Add/Sub/Mul/Div/Rem/Index/Eq/Neg` dispatch already exists
> (§2.7); this phase generalizes it (reflected + in-place + the missing traits) — a
> **[NEEDS-ENHANCE]** extension of current behavior, not greenfield.

**Goal:** Magic-method / dunder protocol traits. Generalize Kirn's operator overloading into a complete, Python-inspired magic protocol so user types behave like builtins: callable, indexable, iterable, len-able, bool-able, hashable, comparable, stringable, containable.
**Must Follow:** default to Rust-style capitalized traits; wire each trait to exactly one dispatch hook and treat `__dunder__` as an alias; add reflected + in-place forms only.
**Important Note:** this is a `[NEEDS-ENHANCE]` extension of the existing `impls_` dispatch — never a second dispatcher, and the `any` path must reuse it.

> Reference: cpython — dunder protocol (`cpython/Objects`); rust — `core::ops` (`rust/library/core/src/ops`).

**Substrate/checker/runtime:**

- Define the well-known trait set and wire each to a dispatch hook:
  - `Call` → `f(x)`; `Index`/`IndexAssign` → `a[i]`/`a[i]=v`; `Len` → `len(a)`; `Iter`/`Next` → `for x in a` and generator protocol; `Contains` → `x in a`; `Bool` → truthiness in `if`/`and`/`or`; `Hash` → hash for dict/set membership; `ToString`/`ToRepr` → `str(a)`/`repr(a)`; `Format` → f-string `{a:spec}`; `Neg`/`Pos`/`Abs`/`Invert` unary.
- Add **reflected** operator support (`5 * vec` → `Vec2.mul(scalar_left, 5)` when left operand lacks the trait) and **in-place** variants (`x += y` → `AddAssign`).
- Add an optional `__dunder__` method-name spelling that maps onto the same traits (so both `impl Add` and `__add__` target one dispatcher) — decided: **default to Rust-style traits** (already Kirn's idiom) and treat `__x__` as aliases for discoverability/teaching.

**Kirn surface / examples:**

```co
struct Fraction { num: int; den: int; }
impl Add for Fraction { fn add(self, o: Fraction) -> Fraction {...} }
impl Mul for Fraction { fn mul(self, k: int) -> Fraction { ... } }
impl Call for Greeter { fn call(self, name) { print("hi " + name); } }
impl Len for Bag { fn len(self) -> int { return self.items.len(); } }
impl Iter for Bag { fn iter(self) -> gen[int] { for x in self.items { yield x; } } }
impl Contains for Bag { fn contains(self, x) -> bool { return self.items.contains(x); } }
impl Bool for Bag { fn bool(self) -> bool { return self.items.len() > 0; } }
impl Hash for Point { fn hash(self) -> int { return self.x * 31 + self.y; } }
impl ToString for Vec2 { fn to_string(self) -> string { return f"({self.x}, {self.y})"; } }

fn main() {
    g = Greeter();
    g("world");              # Call.__call__  → "hi world"
    b = Bag(items: [1, 2, 3]);
    print(len(b));           # Len
    print(3 in b);           # Contains
    for x in b { print(x); } # Iter
    if b { print("non-empty"); } # Bool
    print(f"{Vec2(x:1.0,y:2.0)}"); # ToString/Format
}
```

**Dependencies:** Phase 0. Campus: `DATA_TYPE_PLAN.md` new types implement these traits.

---

### Phase 3

**Goal:** Context managers, `with`, RAII/`Drop`, and `defer` breadth — guaranteed cleanup out-of-the-box.
**Must Follow:** `with` desugars to `try/finally` over `Enter`/`Exit`; `Drop` runs at deterministic scope exit; `defer` gains expression/block form with Go-style LIFO + argument snapshot.
**Important Note:** cleanup must run even on `raise`; `with` is canonical and `using` is an accepted alias, not a second path.

> Reference: cpython — context managers (`cpython/Lib/contextlib.py`); rust — `Drop` RAII (`rust/library/core/src/ops`); go — `defer` (`go/src/runtime`).

**Substrate:**

- Add `with <expr> as <name> { block }` desugaring to `try/finally` using an `Enter`/`Exit` (or `__enter__`/`__exit__`) magic pair (Phase 2 traits).
- Add a `Drop` trait whose `drop` runs deterministically at end-of-scope (interpreter: scope-exit hook), enabling Rust-style RAII.
- Extend `defer` to accept arbitrary expressions (today it's Go-style call-only) and add LIFO + `defer`-with-args snapshot semantics already Go-like; add `defer { block }` form.
- Provide `using` alias for C# familiarity (decision: `with` is canonical, `using` accepted as alias).

```co
fn main() {
    with File.open("log.txt") as f {
        f.write("hello");
    }   # f.close() runs automatically (Exit), even on raise

    lock = Mutex();
    with lock {             # Enter acquires, Exit releases
        critical_section();
    }

    defer f.close();        # extended to run at function exit

    with conn as c {        # transactional: rollback unless committed
        c.execute("INSERT ...");
        c.commit();
    }
}
```

**Dependencies:** Phase 2 (Enter/Exit/Drop traits). `Mutex`/`File` come from `STD_LIBS_PLAN.md` real modules or `DATA_TYPE_PLAN.md` concurrency types.

---

### Phase 4

**Goal:** Compile-time math and constant folding. `const` expressions evaluate at compile time; introduce `const fn` so reusable compile-time math is possible (Rust `const fn` / Zig `comptime` flavour) — without a full macro system.
**Must Follow:** fold via a small const-evaluator over Phase 1 builtins + literals; `const fn` calls with non-constant args are a static error (Rust v1 rule).
**Important Note:** `comptime` stays reserved for Beyond-15; keep const-ness tracked through the tree so no non-const call slips through.

> Reference: rust — `const fn` (`rust/compiler`); zig — `comptime` (`zig/lib/std`).

**Substrate/checker:**

- Extend the checker to **fold** constant expressions (`const x = 2 + 3 * 4;` → `14`, `const PI2 = PI/2;`, enum discriminants, array sizes) via a small const-evaluator over the new math builtins (Phase 1) and literals.
- Add `const fn name(...) -> T { ... }` declaring a function that may be called at compile time with constant args; calls with non-constant args are a static error (or fall back to runtime — decide: error, like Rust v1; `comptime` keyword reserved for Zig-style in stretch).
- Track _const-ness_ through the expression tree and reject non-const calls inside `const`/`const fn`.

```co
const fn fact(n: int) -> int {
    if n <= 1 { return 1; }
    return n * fact(n - 1);
}
const FACT_10 = fact(10);      # 3,628,800 — computed at compile time
const TWO_PI = 2.0 * PI;       # folded
const TABLE = [i * i for i in 0..10];  # const comprehension → compile-time list

fn main() {
    print(FACT_10);            # no runtime cost
    print(TABLE[3]);           # 9
}
```

**Dependencies:** Phase 1 (math builtins to fold). Static layer (L5) foundation for Phase 8.

---

### Phase 5

> **Ownership/cross-ref (dedup):** this sugar bundle overlaps `SYNTAX_PLAN.md`:
> **walrus `:=` grammar is owned by `SYNTAX_PLAN.md` SP-2** (→) — this phase should cross-ref SP-2
> for the token/grammar and only keep the expression-surface aspects here. The collection
> spread/rest and call-splat belong with `SYNTAX_PLAN.md` SP-9 (→); the trailing closure belongs
> with SP-11.

**Goal:** High-impact expression syntax sugar: walrus `:=`, pipe `|>`, spread/rest in expressions (not just patterns), `var-else`, and method-chain/trailing-lambda ergonomics. All pure desugarings.
**Must Follow:** each sugar is a pure desugaring registered in Phase 0's rewrite pass; walrus is the only assignment-expression, and pipe/placeholder/spread/var-else/trailing-lambda lower to existing AST nodes.
**Important Note:** cross-ref `SYNTAX_PLAN.md` SP-2/SP-9/SP-11 for grammar ownership; keep only the expression-surface aspects here.

> Reference: cpython — walrus (`cpython/Grammar/python.gram`); rust — `let-else` (`rust/compiler`); ruby — splat (`ruby/array.c`).

**Substrate/parser:**

- **Walrus `:=`** — assignment-as-expression within `if`/`while`/list comps/closures: `if (n := len(xs)) > 10 { ... }`. Lower: introduce a hidden temp binding evaluated once in the condition, visible in the body (covers the classic Python pattern). Add an assignment **expression** node distinct from the statement form (keep "assignment is a statement" rule intact by allowing _only_ the walrus as an expression assignment).
- **Pipe `|>`** — `x |> f |> g` lowers to `g(f(x))`; supports placeholders `x |> f(_)` where `_` marks the argument position. Left-associative, low precedence (just above `or`).
- **Spread/rest in expressions** — list `[1, 2, ...xs]`, dict `{**d1, k: v}`, set `{...s}`, call f-args `f(...args)`, and rest-capture `fn (head, ...tail)`. (Pattern rest `..rest` already exists; extend to expression displays and call sites.)
- **`var-else`** — `var x = expr else { handle; }` binds `x` from a pattern-match or non-optional, else runs the else block and returns (Rust `let-else` / Swift `guard let`).
- **Trailing lambda / trailing closure** — `xs.map { x => x * 2 }` as sugar for `xs.map((x) => x * 2)`; last-arg omission with a block.

```co
fn main() {
    if (n := len("hello world")) > 5 {
        print("long: " + str(n));   # n bound in scope
    }

    text = "  hello  ";
    cleaned = text |> trim |> upper |> split(" ");
    print(cleaned);                 # ["HELLO"]

    a = [1, 2];
    b = [...a, 3, 4];               # [1,2,3,4]
    d = {**{"x": 1}, "y": 2};       # {"x":1,"y":2}

    var value = maybe() else { print("no value"); return; };
    print(value);                   # value is non-optional here

    doubled = [1, 2, 3].map { x => x * 2 };   # trailing lambda
}
```

**Dependencies:** Phase 0 (dispatch), Phase 2 (for `map` on iterables). `trim/upper/split` already exist.

---

### Phase 6

> **Ownership/cross-ref (dedup):** the `?.`/`??`/`??=` sugar spelling is shared with
> `SYNTAX_PLAN.md` SP-4 (English-friendly flow / null sugar) — keep the grammar there and the
> checker/type-narrowing semantics here. The `Option`/`result` combinators overlap
> `DATA_TYPE_PLAN.md` Phase 8 (→) and `EXP_PLAN.md` Phase 14 (→); consolidate combinator-method
> specs into Phase 14.

**Goal:** Null/optional safety sugar: safe-call `?.`, elvis `??`, null-coalescing assignment `??=`, `guard let`, and smart casts. Kirn already has `.?.` (nil-safe member) and `T?`; bring the remainder for a Kotlin/Swift/C#-grade story.
**Must Follow:** `?.` extends the existing `.?.` chain, `??`/`??=` desugar to `if`, `x!` unwraps or panics, and smart casts are a bounded checker narrowing.
**Important Note:** reuse the existing `T?`/`is none` machinery; keep `SYNTAX_PLAN.md` SP-4 as grammar owner and consolidate combinator specs into Phase 14.

> Reference: ruby — safe navigation `&.` (`ruby/parse.y`).

**Substrate/parser/checker:**

- **Safe call `?.`** — extend existing `.?.` member form to cover chains `a?.b?.c` (short-circuits at first `none`) and safe-index `a?. [i]`; already partially present.
- **Elvis `??`** — `expr ?? default` : yields `expr` if non-`none`, else `default`. Right-associative, low precedence. Desugar to `if …`.
- **Null-coalescing assignment `??=`** — `x ??= default` : assign only if `x is none`.
- **Non-null assertion `!`** — postfix `x!` asserts non-`none` and unwraps, panicking if `none` (Swift `!`).
- **Smart casts** — in `if x != none { …x… }` narrow the static type of `x` to the non-optional inside the branch (checker improvement; requires the checker to track narrowed types — a bounded feature).

```co
fn main() {
    name: string? = get_name();
    print(name?.len());        # none if name none, else length
    print(name?.upper());
    safe = name ?? "guest";    # "guest" if none
    name ??= "anon";           # only assigns if none
    print(name!.len());        # panic if none

    if name != none {
        # inside: name treated as non-optional (smart cast)
        print(name.len());
    }
}
```

**Dependencies:** Phase 0; existing `T?`/`.?.`/`is none` machinery. Complements `DATA_TYPE_PLAN.md` Option type.

---

### Phase 7

> **Ownership/cross-ref (dedup):** the `any`/`dynamic` type keyword is owned by `SYNTAX_PLAN.md`
> SP-5 (→); the `reflect` module/type metadata is owned by `DATA_TYPE_PLAN.md` Phase 14 (→). This
> phase owns the runtime **dispatch/casts/reflection builtins** surface. Note: `any`/`dynamic`
> themselves already ship (example 37); `type(x)`/`repr(x)` exist but richer `reflect` machinery
> (`methods/fields/size/type_name`, `as?`) is new.

**Goal:** Dynamic features: reflection, runtime dispatch polish, casts, and `dynamic` protocols. Give `any`/`dynamic` real power (Python/Ruby flavour) while keeping the static path clean.
**Must Follow:** reflection builtins (`type/methods/fields/size/type_name`), `any`-tag dispatch reusing Phase 0/2, checked casts `as?`/`as!`, and `dynamic` attribute/index access.
**Important Note:** `any`/`dynamic` already ship; only richer `reflect` + `as?` are new — keep the static path unaffected.

> Reference: cpython — reflection/introspection (`cpython/Objects`); ruby — `methods`/`respond_to?` (`ruby/object.c`).

**Substrate/runtime:**

- **Reflection builtins** (out-of-the-box): `type(x)` (exists, returns `string`), add `methods(x)`, `fields(x)`, `size(x)`, `type_name(x)` typed as reflection metadata; `typeof`/`traitof` for asking "does x satisfy trait T".
- **Dynamic dispatch polish** — when the receiver is `any`, route method calls through a runtime method-table by the value's tag (partially present via `dynamic` duck typing); complete for operator dispatch (Phase 0 helper) and magic methods (Phase 2).
- **Casts** — `x as T` (exists) plus a _checked_ `dynamic_casts`: `x as? T` returns `T?`, panicking form `as!`.
- **`dynamic` object** — a `Dynamic` record/protocol type (Python-ish dict-with-attributes) for scripting: `d.name = 5; d["key"] = 2;` attribute/index read-write on an `any` bag. Expose via a `Object`/`Dynamic` type from `DATA_TYPE_PLAN.md`.
- **`hasattr`/`getattr`/`setattr`** builtins (Ruby/Python) for scripts.

```co
fn main() {
    v: any = Complex(3, 4);
    print(type(v));             # "complex" (or nominal name)
    print(methods(v));          # ["real","imag","abs", ...]
    print(fields(v));           # [("re", 3), ("im", 4)]

    d: dynamic = {};
    d.name = "Kirn";
    d["version"] = "1.0";
    print(d.name, d["version"]);

    x: any = "42";
    n: int? = x as? int;        # safe checked cast → none if not an int
    print(n);
    # p: Point = v as! Point;   # panicking cast

    if hasattr(d, "name") {
        print(getattr(d, "name"));
    }
}
```

**Dependencies:** Phase 2 (magic dispatch on `any`), Phase 6 (`as?` reuses `T?`). Reflection metadata hooks into `DATA_TYPE_PLAN.md` reflection/meta phase.

---

### Phase 8

**Goal:** Static features: contracts/preconditions, richer `assert`, type checks, `freeze`, and compile-time guarantees — the "static" half of dynamic & static.
**Must Follow:** contracts (`require`/`ensure`), message-form `assert`, `freeze`, `is`-driven smart casts, and simple `static`/`inline` hints; enforced by default, relaxable in release.
**Important Note:** build on Phase 4 const-folding and Phase 6 narrowing; this is lightweight design-by-contract, not a full verifier.

> Reference: rust — `debug_assert`/contracts (`rust/library/core`); cpython — `assert` (`cpython/Python`).

**Substrate/checker:**

- **Contracts** — `require(cond), ensure(cond)` or `invariant`-style annotations on functions that the checker (and interp) enforces; simplest form: builtin `assert` already exists — add `assert x == y, "msg"` (message) and `precondition`/`postcondition` builtins in debug/permissive mode (decision: enforced by default, `-O`/`release` relaxes or keeps).
- **`freeze`** — `freeze x;` makes a binding read-only after first assignment (compile error on later writes); complements `const`.
- **Static type checks** — `is` operator for type tests (exists), plus `x is T` in `if` giving smart-cast; `trycast`.
- **Exhaustiveness & const improvements** — carry Phase 4 const-folding into more contexts (array literals, default params), and extend match exhaustiveness (partially done).
- **`static`/`inline`** hints — `static` local that persists across calls (like C), `inline` function body substitution (simple, when provably small).

```co
fn divide(a: int, b: int) -> int {
    require b != 0;            # precondition
    return a // b;
}

fn main() {
    freeze config_size = 32;   # compile-error on config_size = 64 later
    print(divide(10, 2));

    x: any = 5;
    if x is int {              # type test + smart cast
        y = x + 1;             # x narrowed to int
        print(y);
    }
    assert config_size == 32, "config frozen";
}
```

**Dependencies:** Phase 4 (const folding), Phase 6 (smart casts). This is the Kirn "design-by-contract, lightweight" story.

---

### Phase 9

**Goal:** Vector / array math & broadcasting — element-wise expressions so math reads naturally (NumPy/Julia/MATLAB flavour) without full array-first redesign.
**Must Follow:** element-wise `+ - * /` for `list[...]`/`Vec*` via Phase 2 traits; start with same-shape + scalar broadcast; add `dot`/`cross`/`matmul`/`@`.
**Important Note:** element-wise-on-`list` is a documented special case gated by an explicit rule so user code stays predictable.

> Reference: llvm — vector types / element-wise lowering (`llvm/llvm/lib`); zig — vector ops (`zig/lib/std`).

**Substrate/runtime:**

- Define a **vector** display + numeric dispatch: when either operand of `+ - * /` is a `list[float]`/`list[int]` (or a dedicated `Vec`/`Array` from `DATA_TYPE_PLAN.md`) and the other is a scalar or same-length list, do element-wise operation. Gated behind an explicit rule so user code stays predictable.
- Add **broadcasting** for scalar↔array and same-shape arrays (start same-shape + scalar; extend to trailing-dim broadcast later).
- Add math builtins `dot(a,b)`, `cross(a,b)`, `matmul(A,B)`/`@` operator (Phase 13), `sum/min/max/mean/var/std` on arrays (mean/var/std to `math`/`statistics` in `STD_LIBS_PLAN.md`, but element-wise ops are the expression story here).
- Provide `Vec2/Vec3/Vec4` convenience types + operator overload (via Phase 2 traits) for games/graphics.

```co
fn main() {
    a = [1.0, 2.0, 3.0];
    b = [4.0, 5.0, 6.0];
    print(a + b);       # [5.0, 7.0, 9.0]   element-wise
    print(a * 2.0);     # [2.0, 4.0, 6.0]   scalar broadcast
    print(a - b);       # [-3.0, -3.0, -3.0]
    print(dot(a, b));   # 32.0

    v = Vec3(x: 1.0, y: 2.0, z: 3.0);
    w = Vec3(x: 4.0, y: 5.0, z: 6.0);
    print(v + w);       # Vec3(5,7,9) via operator traits
    print(v.length());  # sqrt(14)
}
```

**Dependencies:** Phase 1 (math builtins), Phase 2 (operator overload on `Vec*`). Mesh with `STD_LIBS_PLAN.md` `statistics` module. Element-wise-on-`list` is a decided, documented special case.

---

### Phase 10

> **Dedup/cross-ref:** the Result/Option **combinator methods** listed here (`map, map_err,
and_then, or_else, unwrap_or, unwrap_or_else`) are ALSO specified in **Phase 14** (error-handling
> sugar) below and overlap `DATA_TYPE_PLAN.md` Phase 8. **Consolidate:** keep the combinator-method
> spec in **Phase 14** (the error-handling home) and treat this phase's bullet 3 as a cross-ref,
> not a second spec. The functional builtins (`zip/chain/chunk/partition/...`) belong HERE and are
> the unique part.

**Goal:** Callable objects & functional builtins & combinators — the batteries that make pipelines and callbacks ergonomic.
**Must Follow:** callable via the `Call` trait; functional builtins (`zip/chain/chunk/partition/…`) are free builtins; defer the combinator-method spec to Phase 14.
**Important Note:** `lazy { }` defers evaluation to first use; keep the combinator spec in one place (Phase 14) to avoid drift.

> Reference: cpython — `__call__`/`functools` (`cpython/Lib/functools.py`); ruby — blocks/`to_proc` (`ruby/proc.c`).

**Substrate/runtime:**

- **Callable objects** via `__call__`/`Call` trait (Phase 2) — `f(x)` on any object with `Call`.
- **Functional builtins breadth** (out-of-the-box): `zip(*iters)`, `enumerate(xs, start)`, `chain(*iters)`, `chunk(xs, n)`, `partition(pred, xs)`, `flatten`, `take/drop`, `unique`, `group_by`, `pairwise`, `cycle`, `repeat(x, n)`, `pipe(x, f, g, ...)` (function-composition form of `|>`).
- **Combinators as methods** on `result[T,E]`/`option[T]`: `map`, `map_err`, `and_then`, `or_else`, `unwrap_or`, `unwrap_or_else`, `flat_map` (Rust flavour, complements existing `?`) — these slot on the `Result`/`Option` types from `DATA_TYPE_PLAN.md`, but the _method surface_ is defined here.
- **Lazy/`lazy` thunk** — `lazy { expr }` defers evaluation until first use.

```co
fn main() {
    sum = (a, b) => a + b;        # lambda is already callable
    print(sum(2, 3));

    xs = [1, 2, 3, 4, 5];
    print(partition((x) => x % 2 == 0, xs));   # ([2,4],[1,3,5])
    print(chunk(xs, 2));                     # [[1,2],[3,4],[5]]
    print(zip([1,2], ["a","b"]));            # [(1,"a"),(2,"b")]
    print(enumerate(xs, start: 1));          # [(1,1),(2,2),...]

    r: result[int, string] = parse_num("42");
    print(r.map((x) => x * 2).unwrap_or(0));   # 84

    v = lazy { expensive(); };   # not computed until v is used
}
```

**Dependencies:** Phase 2 (`Call`/`Iter`), Phase 5 (trailing lambda, pipe), Phase 7 (dynamic for untyped). Result/Option types from `DATA_TYPE_PLAN.md`.

---

### Phase 11

**Goal:** Formatting & number/string conversion breadth — the question every language gets asked: "how do I format a number / parse a number safely / control precision".
**Must Follow:** full printf-style f-string grammar over the existing `{expr:spec}` seam; safe `parse_int`/`parse_float` return optionals while `int`/`float` keep throwing.
**Important Note:** keep the spec grammar identical across f-strings and `format(x, spec)`; include debug `f"{x=}"`.

> Reference: cpython — format spec (`cpython/Objects`); go — `strconv` parse/format (`go/src/strconv`).

**Substrate/runtime:**

- **Format builtins**: `format(x, spec)` and rich f-string format specs (Kirn f-strings already take `{expr:spec}`; extend the spec grammar to the full printf-style set): `width.precision type` + alignment `< > ^`, `+`/`-`/space signs, `#` alt (0x), `0` zero-pad, `,` thousands, `%` percent, `e/E/f/g/`, integer `b/o/x/d`).
- **Debug f-strings** `f"{x=}"` → `x=42` (Python).
- **Robust parsing**: `parse_int(s, radix?)` → `int?` (none on failure, no panic), `parse_float(s)` → `float?`, `parse_bool`, radix support; handle whitespace/sign. Replace/augment the bare `int(s)`/`float(s)` (which currently throw on bad input) with safe variants while keeping the throw-forms for convenience.
- **Number formatting**: `to_hex(to_fixed, to_sci, to_percent, with_separators)` methods on int/float; f32/f64 precision control.

```co
fn main() {
    print(format(3.14159, ".2f"));    # "3.14"
    print(format(255, "#08x"));       # "0x0000ff"
    print(format(1234567, ","));      # "1,234,567"
    print(format(0.5, ".0%"));        # "50%"

    x = 42;
    print(f"{x=}");                   # "x=42"
    print(f"{1234567:,}");            # "1,234,567"

    n: int? = parse_int("ff", 16);    # 255 (safe, none on failure)
    print(n);
    print(int("0xff"));               # 255 (existing convenience)
}
```

**Dependencies:** Phase 1 (number builtins), Phase 0 (format spec plumbing). Extends existing f-string machinery in `lexer.cpp`/`runtime.cpp`.

---

### Phase 12

**Goal:** Collection builtins & iterator protocol magic — make builtin collections and the `for`/comprehension machinery complete and composable.
**Must Follow:** the `Iter`/`Next` protocol generalizes `for`/comprehensions/`map`/`filter`/`reduce`/spread; add dict/set comprehensions and set ops.
**Important Note:** existing containers/generators keep working; the new protocol is the default fallback in `iterateSeq`/`invokeMethod`.

> Reference: cpython — iterator protocol (`cpython/Objects`); ruby — `Enumerable` (`ruby/enum.c`).

**Substrate/runtime:**

- **Iterator protocol** — an object implements `Iter`/`Next` (Phase 2) to work in `for p in e`, comprehensions (`[y for v in obj]`), `map/filter/reduce`, and spread (`[...obj]`). Currently `for` works on the builtin containers and generators; generalize to any `Iter` object.
- **Collection builtins** (free/global): `len`, `sum`, `min`, `max` (exist), add `sorted(xs, key, reverse)`, `reversed` (exist), `zip`, plus `dict`/`set` operations: `keys`, `values`, `items` (exist as methods — add as builtins too), `merge`, `update`, `union`, `intersection`, `difference` (set ops partial).
- **Comprehension generosity**: allow dict comprehension `{k: v for p in it}`, set comprehension `{x for p in it if c}` (currently list + generator only).
- **`range` sugar** — `range(n)`, `range(a,b,step)` (exists); add `0..10` iterable directly usable in comprehensions (partially), `x.times { }` Ruby-like block enlargement for readability (toggle: provide as method, not grammar).

```co
fn main() {
    squares = {k: k * k for k in 0..5};   # dict comprehension
    evens   = {x for x in 1..10 if x % 2 == 0};  # set comprehension
    print(squares, evens);

    d1 = {"a": 1};
    d2 = {"b": 2};
    merged = {**d1, **d2};                # spread-merge
    print(merged);

    3.times { i => print("hi " + str(i)); };   # Ruby-style block
    print(sorted([3, 1, 2], reverse: true));   # [3, 2, 1]
}
```

**Dependencies:** Phase 2 (Iter protocol), Phase 5 (spread, trailing lambda). Complements `DATA_TYPE_PLAN.md` collections & `STD_LIBS_PLAN.md` `collections` module.

---

### Phase 13

**Goal:** Extended operator set: custom operators, `infix` functions, `@` matrix multiply/`:` special operators — for DSLs and math readability (Kotlin `infix`, Ruby custom ops, Julia `^`).
**Must Follow:** `infix fn` gets a single documented custom-operator precedence band (just above `or`, parens when ambiguous); complete the operator-trait table; `@` is binary `MatMul`.
**Important Note:** a fixed precedence keeps the grammar regular; no per-operator precedence invention.

> Reference: ruby — operator overloading (`ruby/numeric.c`); Nim — infix operators (`Nim/lib/pure`).

**Substrate/parser/checker:**

- **`infix` functions** — `infix fn poww(a: int, b: int) -> int {...}` lets you call `2 poww 10`. Introduces a user-defined operator name; precedence is fixed (a documented "custom-operator" precedence band, lowest, or named like the `+` level — decision: a single band just above `or`, requiring parens when ambiguous).
- **Custom unary/operator overloading breadth** — complete the trait table (Phase 2) for all operators: `Add/Sub/Mul/Div/Rem/Pow/Shl/Shr/BitAnd/BitOr/BitXor/Neg/Pos/Not/Index/IndexAssign/...` + reflected + in-place.
- **`@` operator** — currently token exists (`@` is pattern-only); add binary `@` → `MatMul` trait for `A @ B` matrix multiply (Julia/Python), plus element-wise via `Vector` ops (Phase 9).
- **`in`/`not in`** — is already membership; keep. Add `is`/`is not` refinement.

```co
infix fn poww(a: int, b: int) -> int { return ipow(a, b); }

struct Matrix { rows: list[list[float]]; }
impl MatMul for Matrix {
    fn matmul(self, o: Matrix) -> Matrix { ... }
}

fn main() {
    print(2 poww 10);            # 1024   (infix custom op)
    A = Matrix(rows: [[1.0,2.0],[3.0,4.0]]);
    B = Matrix(rows: [[5.0,6.0],[7.0,8.0]]);
    C = A @ B;                   # MatMul
    print(C.rows);
}
```

**Dependencies:** Phase 2 (full operator traits), Phase 5 (sugar parser hook), Phase 9 (Matrix element-wise support). `ipow` from `math.kn`.

---

### Phase 14

> **Ownership/cross-ref (dedup):** this is the designated home for the **Result/Option combinator
> methods** (`map/map_err/and_then/...`) — `EXP_PLAN.md` Phase 10 now defers to here for them. The
> `result[T,E]`/`option[T]` **types** and the error taxonomy live in `DATA_TYPE_PLAN.md` Phase 8
> (→); the `errors` module packaging in `STD_LIBS_PLAN.md` Phase 1b (→). Note: `try/catch/raise`
> statement + expression forms already ship (DO_FIRST_PLAN Phase 1, `examples/35_try_catch.kn`);
> the _new_ parts here are typed multi-catch, error wrapping, and the combinator surface.

**Goal:** Error-handling sugar: try-as-expression, catch combinators, error chaining/wrapping — the ergonomics developers constantly request (Kotlin `runCatching`, Rust `?`, Zig `catch`, Go `%w`).
**Must Follow:** `try`/`catch` as expression; Option/Result combinators defined here (Phase 10 defers to this); `catch`-inline, Go `%w` wrapping, and typed multi-catch.
**Important Note:** this is the single home for combinator methods; reuse the existing `?`/`try`/`raise`/`catch_panic` machinery.

> Reference: rust — `?`/combinators (`rust/library/core`); zig — error unions `try`/`catch` (`zig/lib/std`); go — `%w` wrapping (`go/src`).

**Substrate:**

- **`try`/`catch` as expression** — `try { expr } catch e { fallback }` yields a value; complements the existing `?` propagation and `catch_panic`/`match on result`.
- **Result/Option combinators** (methods defined here, types in `DATA_TYPE_PLAN.md`): `map`, `map_err`, `and_then`, `or_else`, `unwrap_or`, `unwrap_or_else`, `ok_or`, `ok_or_else`, `try_into`.
- **`catch` with error unions** — Zig-style: `var x = try risky();` (propagate) and `var x = risky() catch fallback;` (handle inline). `catch`/`try` keywords already exist for statements; extend to expression position.
- **Error wrapping** — Go `%w`-style context chaining: `raise "read failed: " + err` or `.wrap(msg)` on a result produce a wrapped error whose `root(err)`/`cause(err)` unwinds.
- **Multi-catch** — `try { } catch (TypeA e) {} catch (TypeB e) {}` typed catches (currently single generic catch); exception groups later.

```co
fn main() {
    # try-as-expression with fallback
    v = try { parseInt("42") } catch e { 0 };
    print(v);                       # 42

    # Zig catch-style
    data = escape_risk() catch "fallback";

    # combinator chain on result
    r: result[int, string] = parse("7");
    doubled = r.map((x) => x * 2).or_else((err) => err).unwrap_or(0);
    print(doubled);                 # 14

    # typed multi-catch
    try {
        might_throw();
    } catch (ValueError e) {
        print("value: " + err_msg(e));
    } catch (IOError e) {
        print("io: " + err_msg(e));
    }
}
```

**Dependencies:** Phase 2 (Call/magic), existing `result`/`?`/`try`/`raise`/`catch_panic`. Result/Option types from `DATA_TYPE_PLAN.md`.

---

### Phase 15

> **Ownership/cross-ref (dedup):** this phase owns the **builtin names** for random/number-theory/
> special math. The `random`/`statistics` **module packaging** is `STD_LIBS_PLAN.md` Phase 3b/3e
> (→) — define names here, package there, don't double-spec.

**Goal:** Random, number theory & special math functions out-of-the-box — rounding out the math story so no "why doesn't Kirn have X" remains.
**Must Follow:** random/`is_prime`/`pow_mod`/`factorial` land as builtins (module packaging in `STD_LIBS_PLAN.md`); special functions (`gamma`/`erf`/…) stay module-only.
**Important Note:** define names here, package there — don't double-spec; reuse the Phase 1 math foundation.

> Reference: go — `math/rand` (`go/src/math/rand`); cpython — `random` (`cpython/Lib/random.py`).

**Substrate/runtime + `math`/`random`/`statistics` modules:**

- **Random** (builtin + `random` module): `rand()`, `rand_int(lo, hi)`, `rand_float(lo, hi)`, `shuffle(xs)`, `choice(xs)`, `sample(xs, k)`, `seed(n)`; distributions: normal/exponential/uniform via `random` module.
- **Number theory** (builtins): `is_prime`, `next_prime`, `prime_factors`, `phi` (totient), `pow_mod(base, exp, mod)` (modular exponentiation), `inv_mod`, `binomial(n,k)`, `factorial`, `isqrt`. Land in `math.kn` / `number_theory` module, exposed as builtins for the out-of-the-box feel of the rare ones.
- **Special math** (module): `gamma`, `lgamma`, `erf`, `erfc`, `beta`, `zeta` (via `STD_LIBS_PLAN.md` `math` module; defined here for naming).

```co
fn main() {
    print(rand_int(1, 7));        # die roll 1-6
    xs = [1,2,3,4];
    shuffle(xs);
    print(choice(xs));
    print(is_prime(97));          # true
    print(pow_mod(2, 10, 1000));  # 24
    print(factorial(5));          # 120
    print(binomial(10, 3));       # 120
    print(isqrt(144));            # 12
}
```

**Dependencies:** Phase 1 (math foundation), Phase 9 (array reductions). Random/statistics modules in `STD_LIBS_PLAN.md`.

---

### Beyond Phase 15

Stretch goals (referenced but staged by future plans):

- **Proc/macro system** (`macro_rules!`-style, `#[derive]` auto-impl) — currently deferred (FEATURE_GAP). A `derive(Eq, Hash, ToString)` built-in derive for magic traits would auto-generate Phase 2 impls and is the highest-value stretch.
- **Full Zig `comptime`** — types as values, metaprogramming via normal code (vs Phase 4's pragmatic `const fn`).
- **Async/await** on top of `spawn`/`chan`.
- **Multiple dispatch** (Julia style) for generic numeric kernels.
- **Exception groups** (`except*`) and full error type hierarchy.
- **`fold`/`scan`/`window`** lazy-stream operators (deferred; reduce exists).
- **GPU/native SIMD vector math** via `native.cpp` backend.
- **Polymorphic inline caches** for the `any` dispatch table (perf).

**Goal:** stage stretch items (derive/macros, full `comptime`, async/await, multiple dispatch, exception groups, lazy streams, SIMD, PICs) without committing to them.
**Must Follow:** each stays out of v1 until it demonstrably removes more complexity than it adds and has a proven precedent.
**Important Note:** `derive(Eq, Hash, ToString)` is the highest-value stretch because it auto-generates the Phase 2 impls.

```kn
# future: derive auto-generates the magic impls
@derive(Eq, Hash, ToString)
struct Point { x: Int; y: Int; }
# generates -> impl Eq, impl Hash, impl ToString
```

> Reference: rust — `#[derive]` (`rust/compiler`); zig — `comptime` (`zig/lib/std`).

---

## 8. Magic-Method & Operator Inventory

**Goal:** provide one canonical trait↔dunder↔trigger table so the magic layer is machine-checkable and every trigger resolves to exactly one dispatch hook.
**Must Follow:** default to Rust-style capitalized traits; treat `__x__` spellings as aliases for teaching/discovery only.
**Important Note:** reflected and in-place forms are the `[NEEDS-ENHANCE]` delta over §2.7; do not add a second dispatcher.

```kn
struct Bag { items: list[Int]; }
impl Len for Bag { fn len(self) -> Int { return self.items.len(); } }
impl Iter for Bag { fn iter(self) -> gen[Int] { for x in self.items { yield x; } } }
impl Contains for Bag { fn contains(self, x: Int) -> Bool { return self.items.contains(x); } }

fn main() {
    b = Bag(items: [1, 2, 3]);
    print(len(b), 2 in b);      # 3 true
    for x in b { print(x); }    # 1 2 3
}
```

> Reference: cpython — numeric/container dunders (`cpython/Objects`); rust — `core::ops` traits (`rust/library/core/src/ops`).

### 8.1 The well-known trait set (Kirn spelling ↔ Python dunder ↔ trigger)

| Kirn trait                            | Python `__dunder__`                          | Trigger                       | Phase |
| ------------------------------------- | -------------------------------------------- | ----------------------------- | ----- |
| `Add`/`Sub`/`Mul`/`Div`/`Rem`/`Pow`   | `__add__` … `__pow__`                        | `a + b` … `a ** b`            | 2/13  |
| `Neg`/`Pos`/`Abs`/`Not`/`Invert`      | `__neg__` `__pos__` `__abs__` `__bool__`/`~` | unary `- + abs ~`             | 2     |
| `MatMul`                              | `__matmul__`                                 | `a @ b`                       | 13    |
| `Shl`/`Shr`/`BitAnd`/`BitOr`/`BitXor` | `__lshift__`…                                | `<< >> & \| ^`                | 13    |
| `Index`/`IndexAssign`                 | `__getitem__`/`__setitem__`                  | `a[i]` / `a[i]=v`             | 2     |
| `Len`                                 | `__len__`                                    | `len(a)`                      | 2     |
| `Iter`/`Next`                         | `__iter__`/`__next__`                        | `for … in a`, comprehensions  | 2/12  |
| `Contains`                            | `__contains__`                               | `x in a`                      | 2     |
| `Bool`                                | `__bool__`                                   | `if a`, `and/or/not`          | 2     |
| `Hash`                                | `__hash__`                                   | dict/set key                  | 2     |
| `Eq`/`Ne`/`Cmp`                       | `__eq__`/`__lt__`…                           | `== != < <= > >=`             | 2/13  |
| `ToString`/`ToRepr`/`Format`          | `__str__`/`__repr__`/`__format__`            | `str(a)`/`repr(a)`/`f"{a:…}"` | 2/11  |
| `Call`                                | `__call__`                                   | `f(x)`                        | 2/10  |
| `Enter`/`Exit`                        | `__enter__`/`__exit__`                       | `with`                        | 3     |
| `Drop`                                | `__del__` (approx)                           | end-of-scope RAII             | 3     |

Reflected ops: if left operand has no trait, try the right operand's reflected trait (`__radd__` → `Add` with args swapped), for `5 * vec`. In-place: `x op= y` → `AddAssign`… if present, else `x = x op y`.

**Goal:** enumerate the well-known trait set and its Phase home so the dispatch table is complete before coding.
**Must Follow:** every trigger maps to one trait and one phase; add nothing that has no trigger.
**Important Note:** reflected (`__radd__`) falls back to the right operand's trait; in-place (`+=`) prefers `*Assign` then falls back to `x = x op y`.

> Reference: cpython — dunder naming (`cpython/Objects`); rust — reflected ops via `Rhs` (`rust/library/core/src/ops`).

### 8.2 Operator conventions (ratified for EXP_PLAN)

| Op               | Int↔Int   | Conventions documented                                                    |
| ---------------- | --------- | ------------------------------------------------------------------------- |
| `/`              | float     | true division                                                             |
| `//`             | int       | floor division                                                            |
| `%`              | matcher   | `.rem()` truncated, `.rem_euclid()` non-negative (both methods available) |
| `**`             | int power | `0**0=1`; negative int exponent → float                                   |
| `-`              | int       | two's-complement, checked/wrapping per build config                       |
| `<< >> & \| ^ ~` | int       | bitwise                                                                   |

**Goal:** ratify the operator conventions (`/` true-div, `//` floor, `%` matcher + `.rem()`/`.rem_euclid()`, `**`, shifts, bitwise) once.
**Must Follow:** keep the table authoritative for checker, runtime, and AOT; `-` overflow behavior follows the build config.
**Important Note:** conventions must match the folded Phase 4 results so `const` and runtime agree exactly.

> Reference: rust — `rem_euclid`/`div_euclid` (`rust/library/core/src/num`); cpython — floor-division/`divmod` (`cpython/Objects`).

---

## 9. Builtin & Math-Function Inventory

**Goal:** give a single machine-consumable name inventory (existing, new math, functional/collection, sugar) so docs, checker, and LSP agree.
**Must Follow:** every name lands as a predeclared free builtin or a documented module function; never a keyword for a builtin.
**Important Note:** names not yet implemented are staged by phase; the inventory is the contract `predeclareBuiltins` must satisfy.

```kn
# out-of-the-box builtins, zero imports
print(sqrt(2.0), gcd(12, 8))              # 1.414… 4
print(sorted([3, 1, 2], reverse: true))   # [3, 2, 1]
n: Int? = parse_int("ff", 16)
print(n)                                  # 255
```

> Reference: cpython — `math`/builtin naming (`cpython/Modules/mathmodule.c`, `cpython/Lib/functools.py`); go — `math` (`go/src/math`).

### 9.1 Existing → keep / extend

`print len sqrt ord chr assert assert_eq range panic catch_panic printf str int float bool type repr sum min max any all sorted reversed enumerate map filter reduce upper lower trim contains starts_with ends_with replace split join`

**Goal:** freeze the existing builtin list as back-compat surface.
**Must Follow:** never remove or rename an existing builtin; add new ones alongside.
**Important Note:** legacy lowercase names remain valid forever even as Kirn spellings arrive.

### 9.2 New math builtins (Phase 1 + 15)

```
sin cos tan asin acos atan atan2
sinh cosh tanh asinh acosh atanh
exp exp2 expm1 ln log log2 log10 log1p logb
pow cbrt hypot
round trunc floor ceil fract fmod
sign signum abs clamp lerp midpoint
isnan isinf isfinite signbit copysign nextafter fma frexp ldexp modf
gcd lcm divmod mod
parse_int parse_float
is_prime next_prime prime_factors phi pow_mod inv_mod binomial factorial isqrt
```

**Goal:** define the complete math name set for Phases 1 + 15.
**Must Follow:** each name is a predeclared free builtin; module-only extras (`gamma`/`erf`) live in `math`.
**Important Note:** every name must fold under Phase 4 (`const`) and agree with the Phase 1 operator semantics.

> Reference: cpython — math/C `libm` names (`cpython/Modules/mathmodule.c`); zig — `std.math` breadth (`zig/lib/std/math`).

### 9.3 New functional/collection builtins (Phase 10 + 12)

```
zip chain chunk partition flatten take drop unique group_by pairwise cycle repeat
enumerate(start:) sorted(key, reverse) keys values items merge update union intersection difference
dict/set comprehension
```

**Goal:** define the functional/collection builtin set for Phases 10 + 12.
**Must Follow:** these materialize eagerly (lists/containers); lazy stream adapters stay deferred to Beyond-15.
**Important Note:** `enumerate(start:)` and `sorted(key, reverse)` reuse existing call-arg syntax — no new grammar.

> Reference: cpython — `itertools`/`functools` (`cpython/Lib/functools.py`); ruby — `Enumerable` (`ruby/enum.c`).

### 9.4 New sugar builtins / features (Phases 5–8)

```
walrus :=  pipe |>  spread ...  var-else  safe-call ?.  elvis ??  ??=  x!  x as? T  x as! T
type/methods/fields/size  hasattr/getattr/setattr  require/ensure  freeze
f"{x=}"  format(x, spec)  lazy { }
```

**Goal:** list the sugar features (walrus, pipe, spread, safe-call, casting, contracts, freeze, f-string debug) introduced across Phases 5–8.
**Must Follow:** each has a documented desugaring and a desugar-equivalence test.
**Important Note:** most of these are surface features, not builtins; only `format`, `parse_*`, `type`, and `*attr` are actual builtins.

---

## 10. Test Strategy

1. **Golden examples** — one new `.kn` file per phase under `examples/` (or `stdlib/lib/*_test.kn` for module-tied features), run via `kirn test`. Each demonstrates the feature and asserts expected output with `assert_eq`.
2. **Operator dispatch table test** — a dedicated test drives `dispatchBin`/`dispatchUn`/`dispatchIndex` across all numeric TyK/VK pairs and confirms `/` float, `//` floor, `%`, `**`, rounding/tie conventions.
3. **Reflection tests** — `type/methods/fields/size`, `hasattr/getattr/setattr`, `as?/as!`, `dynamic` attribute access round-trips.
4. **Desugar-equivalence tests** — for walrus, pipe, spread, `var-else`, `with`, elvis: assert the sugar and its manual expansion produce identical results (guards against desugar drift).
5. **Error-path tests** — panics (`x!` on none, checked cast failure), `?` propagation, catch-as-expression, `require`/`assert` failures, `parse_int` returning `none` on garbage.
6. **Compile-time tests** — `const fn`/const-folding correctness; verifying a constant is folded and that non-const args to `const fn` are rejected (static error).
7. **Backward-compat gate** — the full existing `examples/` corpus + stdlib tests must stay green after each phase; new sugar must not break the frozen grammar examples.

**Goal:** define the verification bar (golden examples, dispatch tables, reflection, desugar-equivalence, error paths, compile-time, back-compat) for every phase.
**Must Follow:** each phase ships an example + negative tests and keeps the existing corpus green.
**Important Note:** AI-first means tests are machine-runnable and the desugar-equivalence suite is mandatory for every new sugar.

```kn
# tests/exp_phase1_test.kn
fn main() {
    assert_eq(sin(PI / 2), 1.0);
    assert_eq(round(2.5), 2.0);
    assert_eq((-17).rem_euclid(5), 3);
    assert_eq(divmod(17, 5), (3, 2));
    assert_eq(isnan(0.0 / 0.0), true);
}
```

> Reference: cpython — `test_math`/`test_operator` golden style (`cpython/Lib/test`); rust — unit/doc tests (`rust/library/core`).

---

## 11. Appendix A — Python cpython expression/math surface (condensed)

- **Operators:** `+ - * / // % **`, `@` (matmul), bitwise `& | ^ ~ << >>`, comparisons (`== != < <= > >= is is not in not in`), unary `+ - ~`, augmented `+= … **=`, walrus `:=`, ternary `x if c else y`.
- **Floor/Euclid:** `/` true div; `//` floor; `%` Euclidean-sign; `divmod(a,b)`, `pow(a,b,mod)`, `math.fmod`.
- **Math module:** `sin cos tan asin acos atan atan2`, `sinh cosh tanh asinh acosh atanh`, `exp expm1 exp2 log log2 log10 log1p`, `pow sqrt cbrt hypot`, `floor ceil trunc round`, `isnan isinf isfinite`, `copysign fabs fmod modf frexp ldexp`, `gcd lcm factorial comb perm isqrt`, `pi e tau inf nan`.
- **Dunders:** complete numeric/reflected/inplace set; `__len__ __getitem__ __setitem__ __contains__ __iter__ __next__ __reversed__ __call__ __str__ __repr__ __format__ __bool__ __hash__ __eq__/__lt__… __enter__ __exit__ __del__ __copy__/__deepcopy__ __matmul__`.
- **Sugar:** f-strings (`f"{x:.2f}"`, `f"{x=}"`), comprehensions incl. dict/set, generators, `with`/contextlib, `enumerate(start)`, `zip`/`zip_longest`, `map/filter/reduce`, star-spread `*`/`**kwargs`, `@dataclass`/`__slots__`.

**Goal:** record Python's expression/math surface as the primary reference for dunders, floor-division, and math breadth.
**Must Follow:** borrow Python semantics (floor `//`, Euclidean-ish `%`, reflected ops) but not its GIL/boxing cost.
**Important Note:** cpython is a naming/semantics reference; Kirn's implementation is table-driven C++, not interpreted dispatch.

```kn
# what Kirn takes from cpython, re-spelled
print(7 // 2, 7 % 2, divmod(7, 2))   # 3 1 (3, 1)
print(2 ** 10)                        # 1024
print(f"{3.14159:.2f}")               # "3.14"
```

> Reference: cpython — `math`/`operator`/`functools` (`cpython/Modules/mathmodule.c`, `cpython/Lib/operator.py`).

## 12. Appendix B — Go expression/math surface (condensed)

- **Operators:** `+ - * / %`, bitwise `& | ^ &^ << >>`, comparisons, unary `+ - ^ !`, `&` address/`*` deref, augmented all, no `**`/`//`/`%`-euclid (truncated `%`), no ternary/int-only `/`.
- **Math:** `math.Sin/Cos/Tan/Asin/Acos/Atan/Atan2`, `Sinh/Cosh/Tanh`, `Exp/Exp2/Log/Log10/Log2`, `Pow/Pow10/Sqrt/Cbrt/Hypot`, `Floor/Ceil/Trunc/Round/Mod/Modf`, `IsNaN/IsInf/Wrap/Signbit/Copysign`, `Max/Min/Dim`.
- **Match/Rand:** `math/rand` (Seed, Intn, Float64, Shuffle, Perm, NormalFloat64), `math/big` (Int/Float/Rat — arbitrary precision).
- **Sugar:** deferred `defer` (LIFO), multiple return `(T, error)`, struct/embedding, `iota` enum, `for range`, `select`, goroutines/channels, `errors.Is/As/Unwrap` (`%w`), `encoding/*`, `strconv` (`ParseInt/ParseFloat/Atoi/Itoa/FormatInt`).

**Goal:** record Go's expression/math surface as the reference for `defer`, explicit errors, and `math`/`strconv` naming.
**Must Follow:** borrow `defer` and `%w` error wrapping, but not Go's lack of true division or its truncated `%` as the default.
**Important Note:** Go's truncated `%` is exposed as the `.rem()` method, not the default `%` (see §8.2).

```kn
# what Kirn takes from go
defer print("cleanup")     # LIFO at scope exit (go defer)
print(int("ff", 16))       # strconv.ParseInt parity
print(isnan(0.0 / 0.0))    # math.IsNaN parity
```

> Reference: go — `defer` (`go/src/runtime`), `math` (`go/src/math`), `strconv` (`go/src/strconv`).

## 13. Appendix C — Rust expression/math surface (condensed)

- **Operators:** `+ - * / %`, `**`? (no; `pow`), `& | ^ << >>`, `.. ..=`, comparisons, unary `- !`, `&`/`&mut` borrow/`*` deref, augmented, no `++/--`.
- **Traits (core::ops):** `Add/Sub/Mul/Div/Rem/Neg/Not/BitAnd/BitOr/BitXor/Shl/Shr` + `Assign` variants; `Index`/`IndexMut`; `Deref`/`DerefMut`; `Fn/FnMut/FnOnce`; `PartialEq/Eq/PartialOrd/Ord`; `Drop`; `From/Into/TryFrom`.
- **Numeric methods:** `abs signum pow powi sqrt cbrt exp ln log2 log10 exp_m1 ln_1p hypot sin/cos/tan…` per f64, `rem_euclid`, `div_euclid`, `clamp`, `round` (half-away-from-zero), `floor/ceil/trunc/fract`, `to_radians/to_degrees`, `is_nan/is_infinite/is_finite/signum/signbit/copysign/total_cmp/next_up/next_down`.
- **Sugar:** `match`/`if`/`loop`/`while var` as expression, `var-else`, `?`, `Result`/`Option` combinators (map/and_then/or_else/unwrap_or), `const fn`, ranges in `for`, tuples/destructuring, `..` rest, `@` bindings.
- **Rand:** `rand` crate (thread_rng, gen_range, shuffle, etc.), `num` (BigInt, Rational, traits).

**Goal:** record Rust's expression/math surface as the reference for trait operators, `const fn`, `let-else`, and float methods.
**Must Follow:** borrow `core::ops` + `const fn` + `?`, but not the borrow-checker surface (Kirn is value-semantics by default).
**Important Note:** Rust's `round` is half-away-from-zero while Kirn's is documented ties-to-even — a deliberate, documented divergence.

```kn
# what Kirn takes from rust
const FACT: Int = fact(10)                 # const fn, folded
var v: Int = maybe() else { return; }      # var-else

fn main() {
    r: Result[Int, Str] = parse("7");
    print(r.map((x) => x * 2).unwrap_or(0));   # ?/combinators
}
```

> Reference: rust — `core::ops` (`rust/library/core/src/ops`), `num` (`rust/library/core/src/num`), `const fn` (`rust/compiler`).

## 14. Appendix D — Ruby expression/math surface (condensed)

- **Operators:** `+ - * / % **`, `<=>` spaceship (Comparable auto-derives ordering), `=~`/`!~` regex, bitwise `& | ^ ~ << >>`, augmented, ternary `a ? b : c`, `&&`/`||`/`!`.
- **Math:** `Math.sin/cos/tan/atan2/sqrt/exp/log/log10/log2`, `Integer#gcd/lcm/abs/floor/ceil/round/even?/odd?`, `Float#nan?/infinite?/finite?`, `Rational`/`Complex` native, `bigdecimal` for decimal.
- **Sugar:** string interpolation `"#{x}"`, safe-nav `&.`, splat `*`/`**double-splat`, blocks/`do…end`/`yield`, `Enumerable#map/each/select/reduce`, `attr_accessor`, `method_missing`/`respond_to?`, `methods`, exceptions `begin/rescue/ensure`, `Comparable` include, `Random.rand`.
- **Number literals:** hex `0x`, octal `0o`, binary `0b`, underscores, `1.5e3`, `Complex(3,4)`/`3+4i`, `Rational(1,2)`.

**Goal:** record Ruby's expression/math surface as the reference for safe-navigation, splat, blocks, and introspection.
**Must Follow:** borrow `&.`, splat `*`/`**`, `Comparable`, and `methods`, but not Ruby's `method_missing` dispatch cost.
**Important Note:** Ruby's unbounded `Integer` motivates `BigInt`; Kirn keeps `Int` as the fast fixed-width default.

```kn
# what Kirn takes from ruby
x: Str? = none
print(x&.len())                  # safe nav -> none
args = [1, 2, 3]
print([0, ...args])              # splat -> [0, 1, 2, 3]
print(methods(1))                # introspection
```

> Reference: ruby — `math`/`numeric` (`ruby/math.c`, `ruby/numeric.c`), `Enumerable` (`ruby/enum.c`).

## 15. Appendix E — Kotlin/Swift/Julia/Zig sugar worth stealing (condensed)

- **Kotlin:** safe-call `?.` / elvis `?:` / `!!`, smart-cast in `if x != null`, `when`/`if` as expression, `data class` (auto `equals/hashCode/toString/copy/componentN`), extension functions, `infix`, trailing lambda, `with/apply/let/run/also`, named+default args, `runCatching`, `by` delegation.
- **Swift:** `guard let … else`, optional chaining `?.`/`??`/`!`, `defer`, `switch` as expression, extension methods, trailing closures, `try?`/`try!`.
- **Julia:** broadcast `.` (`a .+ b`), matrix `*` vs element-wise `.*`, `^` pow, `comptime`-like generated functions, macros `@time`/`@eval`, multiple dispatch by type, `1//2` Rational literal, `3+4im` complex literal, `dims`/reductions (`sum(A, dims=1)`).
- **Zig:** `comptime` everywhere, error unions `!T` + `try`/`catch`, arbitrary-precision `comptime_int`/`comptime_float`, `inline`/`@-builtins`, `std.math` breadth plus `clamp/lerp/midpoint/trans` (overflow-safe).
- **C#:** `using`/`IDisposable` (try-with-resources), `??`/`??=`/`?.`/`?[]`, string interpolation `$"{x:F2}"` + raw strings, `Math.Clamp/Lerp/DivRem/Midpoint`, `BigInteger`, `NumberFormatInfo` locale formatting, source generators.

**Goal:** record the modern-sugar wishlist (Kotlin/Swift/Julia/Zig/C#) worth stealing, mapped to Kirn spellings.
**Must Follow:** re-express each idea through the four vendored references where possible; ship only the ones with a proven precedent.
**Important Note:** these are inspirations, not vendored source trees; `> Reference:` lines cite only cpython/rust/go/ruby/Nim/Odin/v/zig/llvm.

```kn
# stolen sugar, Kirn-spelled
name: Str? = get_name()
print(name?.upper() ?? "GUEST")   # safe-call + elvis spirit
a = [1.0, 2.0, 3.0]
print(a + [4.0, 5.0, 6.0])        # element-wise broadcast spirit
const N = 2 + 3                   # folded (compile-time spirit)
```

> Reference: zig — `std.math`/`comptime` (`zig/lib/std/math`, `zig/lib/std`); Nim — `math` (`Nim/lib/pure/math.nim`); v — `vlib/math`; Odin — `core/math`.

---

## 16. Verification / Definition of Done

A phase is **done** when:

1. All its code examples run unmodified and produce the documented output (`assert_eq`-verified).
2. The feature is usable with **zero imports** (builtin) or documented as a `math`/`random` module function.
3. The full existing example corpus + all stdlib tests still pass (no regressions).
4. New syntax has a documented desugaring (for sugar phases) and the equivalence test passes.
5. The checker assigns correct static types where the feature is static, and defers to `any`/runtime where dynamic — no silent correctness loss.
6. Error/edge paths (panic, none, cast failure, nan/inf, overflow, tie-breaking) are covered by dedicated tests.

**Goal:** define the machine-checkable definition-of-done so an agent can self-verify a phase without human judgment.
**Must Follow:** all examples run (`assert_eq`-verified), zero-import or documented-module, no regressions, desugaring documented, static types sound, edge paths tested.
**Important Note:** a phase is not done until the existing `examples/` corpus and the stdlib tests stay green.

```kn
# DoD in one test
assert_eq(sin(PI / 2), 1.0)            # example runs
assert_eq((-17).rem_euclid(5), 3)      # edge path
assert_eq([1, 2] |> sum, sum([1, 2]))  # sugar ≡ expansion
```

> Reference: rust — `library/core` doc-tests (`rust/library/core`); cpython — `Lib/test` golden tests (`cpython/Lib/test`).

---

## 17. Closing Notes

- **Ownership:** this plan is the _expression/syntax/builtin_ companion to `DATA_TYPE_PLAN.md` (types) and `STD_LIBS_PLAN.md` (modules). Where a feature touches two docs, the rule is: _syntax & builtin surface here; type internals & module packaging there._
- **Big first-wins:** Phases 1 (math), 2 (magic methods), and 5 (sugar) deliver the most visible "magic out-of-the-box" value; 4 (compile-time math), 9 (vector math), and 15 (random/number theory) are the highest-requested math headline items.
- **Restraint honored:** ternary `?:`, `++/--`, and a full macro system remain deliberately out of `v1` (consistent with Kirn's A4.9 decisions); `var-else`, `with`, `??`, `|>`, `:=`, and dunder/`const fn` give the loved ergonomics without those.
- Everything above is staged to land incrementally while keeping the frozen grammar and existing corpus green.

**Goal:** restate ownership (expression surface here; types/modules elsewhere), the high-value first phases, and the restraint decisions.
**Must Follow:** keep the ownership rule (syntax/builtin surface here, type internals/module packaging there) and the frozen-grammar guarantee.
**Important Note:** ternary `?:`, `++/--`, and a full macro system stay deliberately out; everything here lands incrementally with the corpus green.

```kn
# the "first wins" in one line
print(sin(PI / 2), Vec2(x: 1.0, y: 2.0) * 2.0, [1, 2, 3] |> sum)   # 1.0 Vec2(2,4) 6
```

> Reference: rust — trait ops (`rust/library/core/src/ops`); cpython — dunders (`cpython/Objects`).
