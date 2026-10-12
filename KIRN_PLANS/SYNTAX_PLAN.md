# Kirn — Syntax Expansion & Expression-Power-Up Plan (SYNTAX_PLAN)

## Goal

This document is the syntax contract that keeps Kirn's grammar small, regular, industry-standard,
and AI-first — every proposed construct is either borrowed from a proven language under
`langs-source-code/` (Rust, Go, Python, Ruby, Nim, Odin, V, Zig, LLVM) or explicitly rejected as
over-engineering, so that both humans and code agents can write, read, and auto-complete it with
one obvious way to do each thing.

## Must Follow

- The toolchain build ships **exactly two** executables: `kirn` (driver) and `kirnc`
  (checker/compiler). No third binary; every new syntax must fit this two-tool world.
- **No over-engineering.** Add a syntax construct only when it removes more complexity than it
  adds; cite the reference language under `langs-source-code/` that already proved the pattern
  (e.g. Rust `match`, Go `defer`, Ruby `unless`).
- **AI-first.** The grammar must degrade gracefully under partial or agent-generated code and
  support unambiguous auto-completion: explicit terminators, regular productions, no
  indentation-sensitive constructs, and redundant-keyword-free design.
- **Kirn-style spellings.** Lowercase width micro-types (`i4..i256`, `u4..u256`, `f4..f256`,
  `uf4..uf256`, `isize`/`usize`) and capitalized primitives (`Int`, `Float`, `Bool`, `Str`,
  `String`, `Char`, `BigInt`, `Rational`, `UInt`/`UFloat`, `UInt4..UInt256`, `Int4..Int256`,
  `Float4..Float256`, `UFloat4..UFloat256`, `Bool8..`, `Char8..`, `ISize`/`USize`); methods are
  lowercase (`.to_int()`); free builtins/types are capitalized (`Str(...)`, `BigInt(...)`, `Min`,
  `Max`, `__Sqrt`).
- **One obvious way.** Any new syntax keeps a single canonical spelling; aliases are allowed only
  as soft, contextual, backward-compatible forms — never two reserved spellings for one thing.

## Important Note

This file is a **proposal/adoption document**, not a frozen spec: it preserves its existing
per-phase proposal structure and records conflicts between proposed syntax and the current
parser/mandate instead of silently resolving them. Where the doc's working name "Kirn" and its
lowercase builtin spellings (`int`, `str`, `sqrt`) differ from ratified Kirn style (`Int`, `Str`,
`__Sqrt`), the Kirn spellings win; the legacy names below are proposals to be reconciled, not
targets.

**Status:** Proposed roadmap · Living document · Version 1.0
**Scope:** The "built-in Kirn syntax feels too limited" ask — expand the **language surface**
(statement forms, expression forms, built-in functions, ergonomics) so Kirn is simultaneously:

- **more dynamic & Python-like** — `any`, dict/set/list ergonomics, `with`, generators, dynamic
  attribute access, assignment expressions;
- **more Go-like** — `for`-over-anything, `defer` (have), goroutines/`spawn`/`chan`/`select`
  (have), interface/method-set duck typing, structural immutability;
- **more Rust- & TS-like** — `#[…]`-style compile-time attributes, pattern-power-up, `fn`/
  `any`/union types, gradual typing, `yield` match expressions;
- **more Java-like** — `switch`-expressions (`->`), records, sealed-enum exhaustiveness, labeled
  `break`/`continue` (have), `instanceof`-style pattern type-test;
- **more English-friendly** — words over punctuation, readable control forms, expressive builtin
  functions;
- **faster like C/C++** — every syntax feature is defined so it lowers to the bytecode VM
  (Phase 4 of `PLAN.md`) and native AOT (Phase 8) without new runtime cost.

It also deliberately lands a **first-class, uniquely-Kirn decorator/attribute system** using the
`@name` / `@name(...)` spelling the user asked for, while **disambiguating it cleanly** from the
existing Rust-style pattern-alias `@` operator.

This document is _choice-bearing_ (like `PLAN.md`): it lists concrete options, their trade-offs,
and a recommended decision for each ratification point. **Not all of the below is future work —
several SP phases (or their core) have already shipped** in the Kirn seed: the `match`-as-expression
core (**SP-3**), the `any`/`dynamic` type (**SP-5**), and the `fn`-alias / multi-statement block
closures / `yield`-generator functions (**SP-11**) are implemented and verified (examples 37, 38,
40, 41, 44). Each shipped phase is marked `[IMPLEMENTED]` at its heading below. Where a feature is
also spec'd in another plan, a cross-reference (→) names the owning file so this doc stays the
_syntax_ owner, not a duplicate.

---

## 0. Orientation — What exists today (ground truth from the code)

Deep source review (evidence base in Appendix A) establishes the current surface:

| Area                              | Current built-in syntax (implemented)                                                                                                                                                                                                                                                                                                                                                                         |
| --------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| Blocks/terminators                | `{ }` brace blocks, `;` simple-statement terminator, free layout, `#` comments                                                                                                                                                                                                                                                                                                                                |
| Declarations                      | `fn f(a: int=0, *rest: int) -> int`, `struct`, `enum`, `trait`, `impl`, `const`, `var`                                                                                                                                                                                                                                                                                                                        |
| Statements                        | `if/elif/else`, `while`, `for p in expr`, `match … case …`, `select`, `unsafe`, `spawn`, `defer`, `return`, `raise`, `break`/`continue [label]`, `import`/`from…import`, `export`, `pass`                                                                                                                                                                                                                     |
| Expressions                       | binary/unary ops, `..`/`..=` ranges, slices `a[1:3]`, `and/or/not/is/in`, ternary-`if`-expr, lambdas `(x)=>e`, list/dict/set literals, f-strings `f"{x:>10}"`, tuple/comprehensions, `new`, `T?`, `.?.`, `?` propagation, `name: value` named args                                                                                                                                                            |
| Patterns                          | Wild `_`, Bind `x`, `x is T`, alias `x @ pat`, Or `a                                                                                                                                                                                                                                                                                                                                                          | b`, Tuple, Ctor `Point(x: 0)`, Range, Slice `[a,b,..r]`, Rest `..`, guards |
| Types                             | `int/i8..u64/f32/f64/bool/char/string/bytes`, `list[T]/dict[K,V]/set[T]/tuple/T?/fn/ptr/ref`, generics `[T is Bound]`, `result[T,E]`                                                                                                                                                                                                                                                                          |
| Builtin fns (runtime.cpp:485-613) | `print, len, sqrt, ord, chr, assert, assert_eq, range, panic, catch_panic, printf, strlen` **+** the Phase-WHY-1/SP-8 battery `str, int, float, bool, type, repr, sum, min, max, any, all, sorted, reversed, enumerate, map, filter, reduce, upper, lower, trim, contains, starts_with, ends_with, replace, split, join` (see `EXP_PLAN.md` §9 for the full inventory; `WHY_PLAN.md` WHY-1 for the rationale) |
| Builtin modules                   | `math, time, io, mem, json, text, os` (stubs) + real `.kn` stdlib                                                                                                                                                                                                                                                                                                                                             |
| Concurrency                       | `spawn`, `chan`, `select`, real OS threads, channels (mutex/condvar)                                                                                                                                                                                                                                                                                                                                          |
| `@` today                         | **Only** Rust-style pattern alias `name @ sub-pattern` (`parser.cpp`)                                                                                                                                                                                                                                                                                                                                         |

**Key constraints discovered**

- `@` is currently consumed _only inside patterns_ — never at statement/declaration start. That
  gives us a **free, unambiguous slot** for `@name` decorators.
- Keywords are **frozen** (`grammar/kirn.ebnf §1`, listed in `lexer.cpp:39-46`). Adding reserved
  words needs an RFC + edition bump, so most new syntax should be _contextual_ (soft keywords) or
  reuse existing tokens.
- Assignment is a **statement, not an expression** (deliberate, per `grammar §4.3`). A walrus
  `:=` operator must be introduced as a _new_ expression form, not by turning `=` into one.
- The type checker already has `any`-style poison markers (`Error`/`Unknown` in
  `src/sema/type.h`) — a real `any`/`dynamic` type slots in naturally.
- No decorator/attribute/meta-programming machinery exists anywhere (`rg` over `src|tools|examples`
  for `decorator|attribute|@inline|@allow` → 0 hits).

**Goal:** establish the ground-truth surface (what the parser/checker/runtime already accept) so every proposal below is additive, not a rewrite.
**Must Follow:** base each proposal on a cited `file:line` in this repo; never assume a token or keyword is free without checking `lexer.cpp`/`parser.cpp`.
**Important Note:** "Kirn" here is the plan's working name for Kirn; the table's lowercase builtins (`int`, `str`) are the legacy surface the Kirn spellings must supersede.

---

## 1. Guiding principles

1. **Additive & backward-compatible.** Every feature is a superset: existing corpus must keep
   compiling and running unchanged. New spelling never changes the meaning of old code.
2. **English-first, punctuation-second.** Where Python uses words and C uses symbols, prefer
   words (`and`/`or`/`not`/`in`/`is` already do this). Add **statement keywords as soft
   (contextual)**, never frozen, so they can still be used as identifiers in older code.
3. **One meaning per glyph, disambiguated by position.** `@` = decorator at declaration start;
   `@` = pattern alias inside a pattern. Like Python disambiguating `@` as decorator vs
   matrix-multiply, position resolves intent.
4. **Lower-safe to the VM.** Every syntactic sugar must have a deterministic desugaring to
   existing AST/VM ops so it costs nothing at runtime (see `PLAN.md` Phase 4 VM + Phase 8 AOT).
5. **Verifiable per phase.** Each phase ends with concrete examples (`examples/*.kn`), negative
   tests, and a `tests/syntax/` suite. Corpus stays ≥ 32/32.

**Goal:** fix the adoption rules (additive, English-first, one-meaning-per-glyph, lower-safe, verifiable) that keep Kirn's grammar regular and agent-completable.
**Must Follow:** every feature stays backward-compatible and soft-keyword/glyph-preserving; no principle may be dropped just to land a construct.
**Important Note:** these principles already match the parser's stance (frozen keywords, statement-only `=`), so proposals must route around them rather than re-open them.
**AI-first grammar rule:** prefer explicit terminators (`;`, `{ }`) and redundant-keyword-free productions; no indentation-sensitive constructs, so a partial or agent-generated fragment always parses or fails locally and unambiguously.

---

## 2. The Kirn Decorator / Attribute System (rated first — user's headline ask)

**Goal:** land a first-class decorator/attribute system as Kirn's headline syntax addition, position-disambiguated from the pattern-alias `@`.
**Must Follow:** Tier-1 attributes ship first; the `@`-at-declaration-start rule is parser-only and adds zero keywords.
**Important Note:** SP-1/SP-6 depend on this section's spelling decision; do not implement decorators before ratifying D1/D2.

### 2.1 Spelling (proposal + rationale)

Use the **`@name` / `@name(args)` / `@name(key: value)`** form the user requested, on declarations:

```kirn
@cache                                      # bare, no args
@deprecated("use serve_v2")                 # call form
@bind(port: 8080, host: "0.0.0.0")          # named args (reuses call_arg syntax)
@route("/api/users")
@author("riad") @safe                       # stacked top-to-bottom

fn handle_get(ctx: HttpCtx) -> Json {
    ...
}
```

Attach to: `fn` (functions & methods), `struct`, `enum`, `variant`, `const`, and (later, Phase 7)
statement-level loop bodies.

**Goal:** give decorators the user-requested `@name` / `@name(...)` / `@name(k: v)` spelling, stacked top-to-bottom on declarations.
**Must Follow:** grammar is `"@" IDENTIFIER [ "(" call_arg { "," call_arg } ")" ]`, reusing existing call-arg syntax; no new keyword.
**Important Note:** Kirn keeps Python/TS spelling but parser-disambiguates it from the existing pattern-alias `@` (§2.2).

> Reference: cpython — `@decorator` declaration syntax (`cpython/Lib`, `cpython/Grammar`).

### 2.2 Disambiguation with pattern-alias `@`

| Context                                                                                                       | Token meaning                      |
| ------------------------------------------------------------------------------------------------------------- | ---------------------------------- |
| **Start of a statement/declaration line** (before `fn`/`struct`/`enum`/`const`, or a soft-keyword annotation) | **Decorator**                      |
| **Inside `match`/`case`/`for` pattern**, i.e. after `case`/`in` and within a `pattern` production             | **Pattern alias** `name @ sub-pat` |

The parser decides by **lookahead position**, mirroring Python's own rule that a `@` at the start
of a logical line is a decorator while `@` between expressions is an operator. Concretely: in
`parseTopOrStmt()`/`parseSimpleStmt()`, if the current token is `Op "@"` (and we are _not_ inside a
pattern context), parse a decorator. The pattern parser's `parseCtorOrBind()` keeps its existing
`atOp("@")` branch. No lexer change needed — `@` is already a token. This is a **parser-only
disambiguation**, zero new keywords.

**Goal:** resolve `@` by position — decorator at declaration start, pattern alias inside a pattern — with no lexer change.
**Must Follow:** implement in `parseTopOrStmt()`/`parseSimpleStmt()` with a pattern-context guard; leave `parseCtorOrBind()`'s `atOp("@")` branch untouched.
**Important Note:** mirrors Python's positional `@` rule; an `@` in an invalid position must error, never silently reparse.

> Reference: cpython — position-disambiguated `@` (decorator vs operator), same rule in `cpython/Grammar`.

### 2.3 Semantics: compile-time attributes first, runtime decorators second

Two tiers, mirroring the Rust(attributes)-vs-Python(decorators) split in the research:

- **Tier 1 — Built-in attributes (compile-time).** Compiler-defined, no runtime cost. Given the
  rich LintConfig added in `PLAN.md` Phase 2, `@warn`, `@allow`, `@deny` become the on-item
  reflection of the lint framework. Also `@inline`, `@deprecated`, `@test`, `@pure`, `@unsafe`.
- **Tier 2 — User decorators (runtime, later).** A decorator is a function taking the declared
  value and returning a replacement (Python/TS semantics; import-time, once). Requires the
  reflection machinery in `PLAN.md` Phase 5 so the interpreter can apply arbitrary functions to
  `Value`s. Default-optional (opt-in) like Python.

**Goal:** two tiers — built-in compile-time attributes first (Tier 1, zero runtime cost), user runtime decorators second (Tier 2, reflection-gated).
**Must Follow:** Tier-1 names are compiler-registered and validated; Tier-2 ships only after the reflection machinery exists.
**Important Note:** Tier ordering matches Rust attributes vs Python decorators; do not ship Tier-2 before reflection or it becomes undefined behavior.

> Reference: rust — compile-time attributes (`rust/compiler/rustc_attr_parsing`); cpython — runtime decorators.

### 2.4 Rationale choices table

| Option                                          | Pros                                             | Cons                                                           | Recommended |
| ----------------------------------------------- | ------------------------------------------------ | -------------------------------------------------------------- | ----------- |
| `@name` (Python/TS), **declaration-start only** | user-requested, familiar, position-disambiguated | collides _conceptually_ with pattern `@` (parser handles it)   | **✅ yes**  |
| `#[name]` (Rust)                                | clear "meta" look                                | conflicts with list-opener `[` in a brace language; two glyphs | no          |
| `[[name]]` (C23)                                | unambiguous                                      | noisy, unfamiliar to Python/TS users                           | no          |
| `name:` prefix (soft keyword `attr name:…`)     | zero new symbols                                 | verbose, not requested                                         | no          |

**Decision (recommended):** ratify `@name`, `@name(...)`, `@name(a: 1)`, stacked, at
declaration-start, parser-disambiguated from pattern-alias `@`. Ship Tier-1 built-in attributes in
Phase 2; Tier-2 runtime decorators in Phase 6 (needs reflection).

**Goal:** record the rejected decorator spellings (`#[name]`, `[[name]]`, `name:`) with their trade-offs so ratification is evidence-based.
**Must Follow:** keep one recommended option per row; rejected options stay documented, not deleted.
**Important Note:** `#[…]` is rejected because `[` already opens lists in a brace language — the same reason Kirn keeps `@` position-disambiguated.

---

## PHASES

> Each phase: **goal**, **concrete feature list with before/after or target syntax**, **design
> notes / rationale**, **desugaring target**, and **exit criteria**. Phases are ordered for value
> and dependency (decorators and builtin breadth early; macro/metaprogramming and full dynamic
> typing later, after reflection exists).

---

## Phase SP-1 — Dekorator Syntax & Builtin Attributes (`@`)

**Goal:** land the decorated/annotated surface with the `@name` / `@name(...)` spelling and the
compiler-defined attribute set. Highest-value, most-visible, and the user's explicit ask.

**Feature list**

- Parser: accept `@`-prefixed decorators at declaration start (functions, `self` methods,
  `struct`, `enum`, `const`); the disambiguation in §2.2.
- `@deprecated("msg")` → emit `W0110 deprecated` warning on use (ties to Phase-2 lint infra).
- `@inline` → advisory for the native AOT backend (Phase 8); `@pure` (C23 `[[unsequenced]]`
  analogue) → enables constant folding in Phase 13 optimizer.
- `@warn(W0102)` / `@allow(W0105)` / `@deny(W0107)` → per-item lint toggles, the on-item form of
  the Phase-2 `LintConfig`.
- `@test` on a `fn` → `kirn test` auto-discovers it (convention over `tests/*_test.kn`).
- `@unsafe` on a `fn` → requires `unsafe` context to call (Rust `unsafe fn` sugar).
- `@field(name: "x")`-style struct field annotations (Java-annotation-like metadata, read by
  `reflect` in Phase 6).
- `@bind(...)` example placeholder for framework-facing decorators (defined by libraries, Tier 2).

**AST/AST changes** (`src/ast/ast.h`)

- Add `std::vector<Decorator> decorators;` to `Stmt` (and `FieldDecl`). `struct Decorator {
std::string name; std::vector<CallArg> args; Span span; }`.

**Parser changes** (`src/parser/parser.cpp`)

- In `parseTopOrStmt()`/declaration entry: `while (atOp("@")) { parseDecorator(); }` then parse the
  declaration. Decorator grammar: `"@" IDENTIFIER [ "(" [ call_arg { "," call_arg } ] ")" ]`.
- Guard: reject decorators after non-`@` tokens and inside pattern contexts (keep existing
  `parseCtorOrBind` alias branch untouched).

**Checker changes** (`src/sema/checker.cpp`)

- Resolve built-in attribute names against a small registry; unknown names → error `E0120 unknown
decorator 'x'` with a `help:` listing known names (uses the Phase-1 diagnostics).
- Enforce `@warn/@allow/@deny` argument shape.

**Examples/tests**

- `examples/33_attributes.kn` (functions + struct + lint toggles).
- `tests/syntax/attrs_*.kn` positive/negative; `tests/negative/` for unknown/ill-formed decorators.
- Corpus stays green.

**Exit criteria:** decorated programs parse, check, and run; `@deprecated` warns; `@allow(W0102)`
suppresses an unused-var warning; pattern-alias `@` still works (example 31).

```kn
@deprecated("use ServeV2")
@allow(W0102)
@route("/api/users")
fn handle_get(ctx: HttpCtx) -> Json {
    return Json(*ctx.body);
}
```

**Goal:** ship `@name` decorators and the compiler-defined attribute set (`@deprecated`, `@inline`, `@pure`, `@warn/@allow/@deny`, `@test`, `@unsafe`).
**Must Follow:** accept stacked decorators only at declaration start; unknown names are checker errors with a `help:` list, never silent.
**Important Note:** highest-value, lowest-risk phase (parser-only + registry); it must not alter pattern-alias `@`.

> Reference: rust — `#[inline]`/`#[deprecated]`/`#[test]` attributes (`rust/compiler/rustc_builtin_macros`); cpython — `@decorator` call form.

---

## Phase SP-2 — Assignment Expressions & the Walrus-ish `:=`

> **Dedup/cross-ref:** this phase is the **syntax/grammar owner** for walrus `:=`. The broader
> expression-sugar bundle that also uses walrus is spec'd in `EXP_PLAN.md` Phase 5 (which should
> cross-ref here rather than re-specify the grammar). Ratify the token before either is coded.

**Goal:** let a value be computed, bound, and used in one expression (Python 3.8 `:=`; C/Go
already allow assignment-in-condition — this is the demand side of "more expressions").

**Feature**

- New operator `:=` (token: add to `kOps2` in `lexer.cpp`). `name := expr` evaluates `expr`,
  binds `name` in the enclosing scope, and **yields the value**.
- Legal in `if`/`while` conditions, `and`/`or`/`not` subexprs, `match` scrutinee, `for` iterable,
  `return`, call args, and inside `{ }` blocks:
  ```kirn
  while (line := reader.read()) != none {
      process(line);
  }
  if (n := len(xs)) > 10 { print(n); }
  ```
- **Important:** keep `=` a statement-only assignment (grammar §4.3 principle is preserved); `:=`
  is a _distinct expression form_, so no ambiguity: `x = y` is a statement, `x := y` is an expr.

**Design notes**

- Precedence: lowest, just above `or` (like Python `:=` NAS), so `(line := r.read()) != none`
  parses as a grouped walrus compared to `none`. Requires the RHS-use parenthesized except at the
  loosest positions — mirror Python's requirement.
- Requires scope/lvalue support in `lvaluePtr`/`assignTo` (runtime.cpp) and binding `used`-flag
  marking in the checker (so `:=`-bound vars are not wrongly warned unused).

**Desugar:** `(x := E)` → evaluate `E`, store into `x`, push `x`'s value; implemented as a new
`ExKind::AssignExpr` AST node evaluated directly.

**Exit criteria:** walrus works in `if`/`while`/`match`/comprehension/`return`/call-arg positions;
tests/negative for `x := 5` at statement start (must still be a statement, not an expr).

**Goal:** let a value be computed, bound, and used in one expression via `:=`, without turning `=` into an expression.
**Must Follow:** `:=` is a new expression form with lowest precedence; keep `=` statement-only and require grouping at ambiguous positions.
**Important Note:** the token must be added to `kOps2`; the conflict risk is with the existing statement-only `=` rule (grammar §4.3), not with the parser.

> Reference: cpython — walrus `:=` (PEP 572, `cpython/Grammar`).

---

## Phase SP-3 — Match & Switch Expressions (Java/Rust modern forms) — **[core IMPLEMENTED]**

> **Status:** the **`match`-as-expression** core ships — a `match` yielding a value in
> assignment/`return` positions (`s = match n { … };`, `return match t { … };`) is implemented and
> verified in `examples/38_patterns_power.kn` (VM ≡ tree-walker); see `DO_FIRST_PLAN.md` Phase 3.
> **Still new below:** the Java-style `->` arrow-arm form and the `yield expr`-inside-an-arm form,
> plus the `else` catch-all arm. Those are the remaining greenfield parts of this phase.

**Goal:** make `match` yield a value (Java 13 `switch`-expression + `yield`; Rust `match`
already yields) so it can be assigned, returned, and chained — a big "more expressions" win.

**Feature**

- `match` as an **expression**: `kind = match code { case 200 -> "ok"; case 404 -> "nope";
else -> "other" };`
- Arrow-arm form `case pattern -> expr` (Java/Kotlin) alongside the existing block-arm form;
  both may appear in one match.
- A `yield expr` statement inside any match arm block that transfers the arm value out
  (Java `yield`, distinct from `return` which exits the whole function):
  ```kirn
  port = match name {
      case "web"   -> 8080
      case "db"    -> { var p = lookup("db"); yield p }
      else         -> -1
  };
  ```
- `else` arm = catch-all wildcard (English-friendly replacement for `_` in match arms).

**Design notes**

- Reuses the existing `match_stmt`; add expression-position `match` (an `ExKind::MatchExpr`),
  `yield` becomes a soft statement keyword usable only inside a match-arm block (else error
  E0121 `yield outside match expression`).
- Exhaustiveness (Phase-6 of PLAN.md) applies; a non-exhaustive value-match without `else` errors.

**Exit criteria:** value-yielding match in assignments/returns/args; `yield`+`->`+`else` all
covered; negative test for `yield` outside a match; examples `34_match_expr.kn`.

**Goal:** make `match` yield a value and add Java/Kotlin `case p -> expr` arms, `yield` in arm blocks, and the `else` catch-all.
**Must Follow:** add `ExKind::MatchExpr` and a soft `yield` valid only inside match-arm blocks; keep existing block arms working.
**Important Note:** the `match`-as-expression core already ships; the open syntax is `->`, `yield`, and `else` — do not regress example 38.

> Reference: rust — value-yielding `match` (`rust/compiler/rustc_ast`); cpython — `match`/`case` soft keywords (`cpython/Grammar`).

---

## Phase SP-4 — English-Friendly Control Conventions & Alternatives

**Goal:** widen the approachable, word-based surface — the "more user- & English-friendly" ask —
_without_ breaking or renaming the frozen keywords.

**Feature list (all soft/contextual; none reserved)**

- `unless cond { }` — `if !cond { }` sugar (Ruby). Soft keyword, parse-ahead.
- `until cond { }` — `while !cond { }` sugar.
- `each x in xs { }` — alias for `for x in xs { }`; `for i in 0..=n { }` stays.
- `when` as alias for `match` (English-friendly; `switch` free too): `when code { case 1 -> ... }`.
- `otherwise { }` as the trailing wildcard arm in `when`/`match`.
- `is none` / `is some` predicates already exist; add **`?.` chaining** promotion: `a?.b?.c`
  (already: `.?.`) and **null-coalescing `??`**: `x ?? fallback` (Go/Rust-friendly; desugars to
  `if x is none { fallback } else { x }`).
- `true`/`false`/`none` already exist; add `nullptr`-style alias? **Recommendation: keep `none`
  only** — one word for bottom is friendlier; document it.
- `with expr as name { }` resource scope → desugars to `defer` (Phase 11 of PLAN.md; spec here).
- `for`-`else` / `while`-`else` (Python): else runs if loop completed without `break`.

**Design notes**

- Soft keywords are matched by the parser via `atIdent("unless")` **in statement position**
  (like contextual `Self`). They remain legal identifiers elsewhere (back-compat).
- `??`/`?.`/`?.()` need lexer token additions (`kOps2`: `??`). `.??` not needed.

**Exit criteria:** all English aliases parse and execute identically to their base forms;
`unless/until/each/when/otherwise` work; `??`+`?.` compose; corpus + `tests/syntax/english_*.kn`;
examples `35_english_flow.kn`.

```kn
unless cache.Hit(key) {
    cache.Put(key, Load(key));
}
until done {
    Tick();
}
each item in items {
    Process(item);
}
result = value ?? Fallback();
```

**Goal:** widen the word-based surface (`unless`, `until`, `each`, `when`, `otherwise`, `??`, `?.`, `with`) without reserving or renaming keywords.
**Must Follow:** every alias is a soft/contextual keyword matched only in statement position and remains usable as an identifier elsewhere.
**Important Note:** the frozen-keyword constraint (`lexer.cpp`) is exactly why these are contextual — promoting any of them to reserved is a breaking edition change.

> Reference: ruby — `unless`/`until` and `||=` defaulting (`ruby/parse.y`); cpython — `for`/`while`-`else`.

---

## Phase SP-5 — Dynamic Typing: the `any` / `dynamic` Type & Duck Typing — **[any/dynamic IMPLEMENTED]**

> **Status:** the `any` (alias `dynamic`) type ships — `var x: any = 5; x = "hi"` works with the
> checker deferring and the runtime dispatching on the value tag; verified in
> `examples/37_dynamic_any.kn` (VM ≡ tree-walker). See `DO_FIRST_PLAN.md` Phase 2.
> **Still new below:** `typeof(x)`, `is`-type-test patterns, union `int | string`, and
> interface-as-method-set duck typing. (→ `EXP_PLAN.md` Phase 7 for the runtime-dispatch/reflection
> side; `WHY_PLAN.md` WHY-2 for the adoption rationale; `DATA_TYPE_PLAN.md` Phase 14 for reflect.)

**Goal:** Python/TS-grade dynamic ergonomics _where the static system isn't required_, never
weakening existing checks (extends PLAN.md Phase 5.1).

**Feature list**

- Predeclare `any` (and alias `dynamic`) as a real type: `var x: any = 5; x = "hi";
print(x.upper());`. Static checker defers; the runtime `Value::k` tag already dispatches
  dynamically (runtime.cpp value model). This is largely a **type-checker allowance**, not a new
  runtime.
- **`typeof(x)` operator** (C23/TS): produce a runtime type-name string →
  `typeof(5) == "int"`. Backed by `reflect.type` (PLAN Phase 5.2).
- **`is` type-test in patterns**: `case x is Point { … }` and `if x is string { … }` (Java
  `instanceof` pattern, Rust `is`-typed binding). Add a `type_pat` to the pattern grammar.
- **Union types (TS)** `int | string` as an `any`-like surface for `is`-narrowing; checker keeps
  the union as an opt-in type until narrowing.
- **Duck typing / interface-as-method-set** (Go): a value satisfies an `interface{ name() }`
  bound if it provides the method set (structural typing) — spec here, implement where PLAN.md
  Phase 5.3 lands.

**Design notes**

- `any` is **not** `Unknown` poison: it's a first-class, usable dynamic type. The checker routes
  any-typed operands straight through (defer), preserving soundness elsewhere.
- `typeof`/`is`-narrowing must be **conservative in `--strict`** and dynamic in default mode.

**Exit criteria:** `any` assignments/ops run; `typeof` returns correct names; `is string`/`is
Point` pattern matches narrow; union `|` types parse; examples `36_dynamic_typing.kn`.

```kn
var x: any = 5;
x = "hi";
if x is Str {
    print(x.upper());
}

var v: int | string = 3;
print(typeof(v));
```

**Goal:** add a first-class `any`/`dynamic` type plus `typeof`, `is`-narrowing, union types, and interface duck typing — opt-in and soundness-preserving.
**Must Follow:** `any` is not `Unknown` poison; the checker defers on `any` operands and `--strict` stays conservative.
**Important Note:** `any`/`dynamic` already ship; the open syntax is `typeof`, union `|`, and `is`-type patterns, which must not weaken existing static checks.

> Reference: go — structural method-set interfaces (`go/src/go/types`); cpython — dynamic dispatch/runtime tags; rust — `is`-typed patterns.

---

## Phase SP-6 — Reflection-Enabled User Decorators & Metaprogramming Ready-Path

**Goal:** complete the decorator story — allow _user-defined_ decorators (Python/TS runtime
semantics) on top of the reflection machinery, making `@name(...)` fully library-extensible.

**Feature list**

- `reflect` builtin (PLAN Phase 5.2) exposed: `reflect.type(x)`, `reflect.fields(x)`,
  `reflect.methods(t)`, `reflect.apply(decorator, value, args...)`.
- A decorator is just a function: `@logged` means `logged(fn)` at declaration time (run once).
  Support stacking (bottom-up application, Python order).
- Built-in framework-facing decorators become stdlib functions: `@route`, `@bind`, `@inject`
  (dependency injection), `@json`, `@deprecated` (already Tier-1) — all defined in Kirn, dogfooding
  the feature.
- `@record`-style sugar: derive `repr`/value-equality/`clone` for a struct (the `record` analog;
  spec with PLAN.md Phase 6.2 records) via a builtin decorator instead of macro rules.

**Design notes**

- Opt-in via `@enable(decorators)` or auto when a `@` has a user-defined callee. Runtime
  decorators pay a one-time application cost at module load (cached), matching Python's
  import-time semantics.

**Exit criteria:** a user writes and applies their own `@cached`/`@timed` decorator; stacking
order verified; `@inject`/`@json` stdlib decorators work; corpus green; examples `37_decorators.kn`.

```kn
fn logged(fn) {
    return fn (x) { print("call"); return fn(x); };
}

@logged
fn Compute(n: int) -> int {
    return n * n;
}
```

**Goal:** complete decorators with user-defined runtime decorators over the reflection API, so `@name` is fully library-extensible.
**Must Follow:** a decorator is a function applied once at declaration time, stacked bottom-up, and cached at module load.
**Important Note:** gated on the reflection machinery (PLAN Phase 5) — shipping earlier would execute arbitrary functions with no `Value` bridge.

> Reference: cpython — runtime decorators applied at import time; cpython — `@` stacking order.

---

## Phase SP-7 — Statement-Level `@` Annotations & `with`/Guarded Sugar

**Goal:** extend annotations to loops/a-declaration statements + land `with` and guarded loops.

**Feature list**

- `@warn`, `@allow`, `@deny` on a loop / block (file-item style) to scope lints.
- `with expr as name { }` → RAII/defer-sugar (spec in SP-4, concretized here).
- **`for`-`else` / `while`-`else`** (from SP-4) fully wired with lint `W0108 for-else with break`?
  (keep friendly: only fire when `break` present in the loop, Python `PEP 340`-adjacent).
- `loop { }` alias for `while true { }` (Rust); `loop name: { … break name; }`.
- **repeat `{ } while cond`** do-while form (Go `do`); `once { }` for single-shot blocks.
- `goto` still rejected (grammar §4.9) — keep out.

**Design notes:** all soft keywords; `with` desugars to `defer`; `loop`/`repeat`/`once` desugar to
`while` forms. No new reserved words.

**Exit criteria:** all sugar executes identically; lint toggles scoped to blocks; examples
`38_with_guards.kn`; negatives for misuse.

```kn
with File.Open("data.txt") as f {
    print(f.ReadAll());
}

loop {
    if Stop() { break; }
    Step();
}
```

**Goal:** extend `@` annotations to loops/blocks and land `with`, `loop`, `repeat`/`once`, and `for`/`while`-`else`.
**Must Follow:** all are soft keywords desugaring to `defer`/`while`; `goto` stays rejected.
**Important Note:** loop-scoped lint toggles must not leak into nested scopes, and `with` must lower to `defer` exactly.

> Reference: cpython — `with`-statement; go — `defer` (`go/src`); rust — `loop {}`.

---

## Phase SP-8 — Builtin Function Breadth (the "more built-in functions" ask) — **[largely IMPLEMENTED]**

> **Status/dedup:** a large slice of this list already ships (see §0 updated row): `map filter
reduce any all sum max min enumerate reversed sorted upper lower trim contains starts_with
ends_with replace split join str int float bool type repr` are implemented free builtins
> (`EXP_PLAN.md` §9 is the canonical inventory). **Still missing from this list:** `zip`, `flatten`,
> `take/skip/first/last/count`, `round/floor/ceil/clamp/pow/is_even/is_odd/is_prime/rand`, `input`,
> `typeof`, and prelude `pi`/`e` consts. The "why" is `WHY_PLAN.md` WHY-1; the function
> names/operators are owned by `EXP_PLAN.md`.

**Goal:** grow the tiny builtin set (`print,len,sqrt,ord,chr,assert,assert_eq,range,panic,
catch_panic,printf,strlen`) into a rich, English-friendly standard surface — still **built-in /
always-in-scope**, cheap to call.

**Feature list (all become interpreter builtins, no import needed)**

- **Sequence/collection:** `map(fn, xs)`, `filter(fn, xs)`, `reduce(fn, init, xs)`,
  `any(fn, xs)`, `all(fn, xs)`, `sum(xs)`, `max(xs)`, `min(xs)`, `enumerate(xs)`, `zip(a, b)`,
  `reversed(xs)`, `sorted(xs, key: …)`, `flatten(xss)`, `take(n)`, `skip(n)`, `first(xs)`,
  `last(xs)`, `count(xs, v)`.
- **Numbers:** `abs`, `round`, `floor`, `ceil`, `min`, `max`, `clamp`, `pow`, `is_even`,
  `is_odd`, `is_prime(? bench)`, `rand` (xoshiro, Phase 8 of PLAN.md), `int(x)`, `float(x)`.
- **Text:** `upper`, `lower`, `strip`, `split(sep)`, `join(sep, xs)`, `replace`, `contains`,
  `startswith`, `endswith`, `repeat`, `pad`, `format` (already f-strings), `repr`.
- **Type/convert:** `str(x)` — **note:** there is deliberately **no `str()` today** (documented
  constraint); this phase _adds_ `str(x)` as the canonical string-conversion builtin (English name,
  TS `String(x)`), plus `int/float/bool/char/list/dict/set/tuple/bytes` casts.
- **Reflect/inspect:** `typeof`, `is_none`, `is_list`, `is_dict`, `type_name`.
- **IO/time:** `print` (have), `input(prompt)`, `read_file(path)`, `write_file(path, data)`,
  `sleep(ms)`.
- **Math consts:** `pi`, `e` (prelude `const`s).

**Design notes**

- Each is a `biFn` in `installBuiltins()` (runtime.cpp:485) — additive, no parser change except
  `str`/conversion-cast names which are just identifiers.
- `map/filter/reduce` take a `fn` value (lambdas are first-class, example 07) — natureal fit.
- New builtins are **not keywords**, so no reserved-word churn; they compete in normal scope so
  users can shadow them.

**Exit criteria:** every builtin listed runs in a demo example `39_builtins.kn`; `map/filter/
reduce/any/all` with lambdas verified; negative tests for arity; full-call dispatch stays O(1)-ish
(thin `biFn` table lookup, no linear name scan).

```kn
var xs = [3, 1, 2];
print(Sorted(xs), Sum(xs), Max(xs), Min(xs));
var doubled = Map(fn (x) { return x * 2; }, xs);
var big = Filter(fn (x) { return x > 1; }, xs);
print(Str(doubled), BigInt(big[0]));
```

**Goal:** grow the builtin surface into a rich, always-in-scope, English-friendly standard library with no import.
**Must Follow:** each builtin is a `biFn` lookup (additive, no parser change) and stays shadowable, never a keyword.
**Important Note:** many listed builtins already ship under lowercase names (`map`, `sorted`); Kirn-style free builtins are CapCase (`Map`, `Sorted`, `Str`), so the feature list must be re-spelled at ratification.

> Reference: cpython — built-in functions (`cpython/Lib`); go — `builtin` package (`go/src/builtin`); v — `vlib/builtin`.

---

## Phase SP-9 — Collection & Comprehension Power-Up (Python/Go ergonomics)

> **Dedup/cross-ref:** this phase is the **syntax** owner for splat/spread, dict/set comprehensions,
> method-views, and ranged iteration notation. The _builtin & iteration-protocol_ side is spec'd in
> `EXP_PLAN.md` Phase 12 (→), and the adoption rationale in `WHY_PLAN.md` WHY-6 (→). `DATA_TYPE_PLAN.md`
> Phases 9–10 own the collection _type_ internals.

**Goal:** richer literal/expression forms for collections — the "more expressions" heart.

**Feature list**

- **Splat/unpacking:** `f(*xs)` -> positional spread; `Point(**d)` -> named spread (Python).
  Reuses `CallArg`/VarArgs (PLAN.md Phase 5.4); AST: `CallArg{ spread: bool }`.
- **Set/dict/sequence built-ins:** `len/x`-free membership via `in` (have); **dict comprehension**
  `{k: v for k in ks}`, **set comprehension** `{x for x in xs}` (currently only list/generator).
- **Generator expressions** fully lazy: `(f(x) for x in xs if c)` (have `for x yield e`, add
  paren form).
- **`any`/`all` comprehensions:** `all(x > 0 for x in xs)` (Python), `any(...)`.
- **Chained/streaming pipelining** (TS/Go-friendly): `xs.filter(f).map(g).take(3)` — give
  builtin collections method-style views (`.map/.filter/.reduce/.take/.skip/.to_list`) while
  keeping the free-function forms of SP-8 (both spellings).
- **Negative/advanced indexing:** `xs[-1]` (Python) — needs runtime `Value` index handling.
- **Slice assignment free:** `xs[1:3] = [...]` (currently index-target only) — allow slice targets.
- **`..`-stepping ranges:** `0..10 step 2` or `range(0,10,step:2)` — pick one; **recommendation:
  `0..10 step 2`** (English word, distinct).

**Design notes**

- Splat touches `callValue`/`runFunc` argument assembly (runtime.cpp) — low risk, high ergonomics.
- Dict/set comprehensions extend the `Expr` comprehension AST (`ExKind::ListComp` +
  `CompClause` already generalize).
- Negative indexing & slice-assign touch index/`assignTo` code paths — additive.

**Exit criteria:** splat/kwargs, dict/set comprehensions, all/any-comps, method-views, negative
index, slice-assign, stepped ranges all run in `examples/40_collections.kn`; corpus green;
negatives for missing splat keys / bad slice targets.

```kn
var a = [1, 2, 3];
var b = [*a, 4];
var d = {k: k * k for k in 0..=3};
var s = {x for x in a if x > 1};
var last = a[-1];
var piped = a.filter(fn (x) { return x > 0; }).map(fn (x) { return x; }).to_list();
print(Str(b), Str(d), Str(s), Str(last), Str(piped));
```

**Goal:** add splat/kwargs spread, dict/set comprehensions, lazy generator expressions, method-views, negative indexing, slice assignment, and stepped ranges.
**Must Follow:** every form desugars to existing `CallArg`/comprehension/index AST nodes; free-function and method spellings coexist.
**Important Note:** stepping is spelled one way only — `0..10 step 2` (D8); do not also add `range(...step)` as a second reserved form.

> Reference: cpython — dict/set comprehensions, `*`/`**` splat, negative indexing (`cpython/Grammar`); rust — method adapters.

---

## Phase SP-10 — `for`-Over-Anything & Iteration Protocol (Go `for range` breadth)

> **Dedup/cross-ref:** `for` over dict/set/string/bytes/generator already works via `iterateSeq`
> (runtime); the open item is the **user-defined iteration protocol** (`next()`/`iter()`), whose
> "why" is `WHY_PLAN.md` WHY-3 (→). The free-function builtins `enumerate/zip/map/filter` that this
> phase leans on are owned by `EXP_PLAN.md` §9 / Phase 12.

**Goal:** make iteration uniform and English-friendly across all collection-like values.

**Feature list**

- `for` over `dict` yields `(key, value)` tuples (Python); over `set` yields elements; over
  `range`, channels, strings (chars), bytes, generators, `none` (zero iterations) — all currently
  largely working via `iterateSeq`; **formalize + test**.
- `for k, v in dict { }` — the current `for pattern in expr` already supports tuple patterns.
- `for i, v in enumerate(xs) { }` (SP-8 builtin) for indexed iteration.
- **`for` with `else`** (SP-4/7) unified here.
- **Iteration protocol:** allow user types to define a `next()`/`iter()`-style method (Go/Java
  `Iterable`); `for x in custom { }` calls `.iter()` then `.next()` under the hood. This is the
  _biggest_ correctness lift (new runtime dispatch) — spec carefully, gate behind trait-method
  convention.
- `chan` receive in `for`: `for v in ch { }` drains until closed (Go `range` over channel).

**Design notes**

- Dict/`enumerate` destructuring needs the tuple pattern (exists); `for k, v` is `for (k, v) in`.
- The protocol method dispatch reuses `invokeMethod` + a `DuckIterable` trait check — small,
  additive, keeps `iterateSeq` as the default fallback.

**Exit criteria:** examples `41_iteration.kn` covering dict/set/chan/string/custom; for-else;
corpus green; negatives for non-iterable custom `for`.

```kn
for k, v in {"a": 1, "b": 2} {
    print(k, v);
}

for i, x in Enumerate(["x", "y"]) {
    print(i, x);
}

for x in 1..=3 {
    print(x);
}
```

**Goal:** formalize `for`-over-anything (dict/set/string/bytes/channel/generator) and add a user-defined `iter()`/`next()` protocol.
**Must Follow:** the protocol is a trait-method convention dispatched via `invokeMethod`, with `iterateSeq` as the default fallback.
**Important Note:** dict iteration yields `(key, value)` tuples (D9) — the `.items()` alternative is rejected to keep one obvious way.

> Reference: go — `range` over maps/channels (`go/src`); cpython — iteration protocol (`cpython/Lib`); rust — `Iterator`.

---

## Phase SP-11 — `fn` Alias, Multi-Statement Closures & `yield`-Lambdas (TS/Rust sugar) — **[core IMPLEMENTED]**

> **Status:** the **`fn` alias for `fn`** (`parser.cpp:252` `fn == fn`), **multi-statement block
> closures** `map(xs, fn (v) { stmts })` (example 44), and **`yield`-based generator functions**
> (example 41) are implemented end-to-end (VM ≡ tree-walker). See `DO_FIRST_PLAN.md` Phases 2 &
> 3.5 & 4. **Still new:** only the _resumable-frame_ generator design (the current generators
> materialize eagerly) and any further closure ergonomics.

**Goal:** friendlier function forms for programmers coming from Go/Rust/TS (extends PLAN.md Phase
11.1).

**Feature list**

- **`fn` alias for `fn`:** `fn add(a: int, b: int) -> int { … }` accepted; `fn` stays. (Frozen
  keyword not required — `fn` is a soft contextual keyword in declaration position.)
- **Named function-type annotation:** `var op: fn(int) -> int = add;` (have `fn(...)` type; add
  lead `fn` declaration).
- **Multi-statement closures** (biggest ergonomics ilar): `map(xs, fn (v) { return v*2; })` —
  a lambda with a `{ }` body (Rust `|x| {}`, TS arrow-with-block). Currently only single-expression
  `(x) => e` (grammar §4.10). Add `fn (params) { body }` block-lambdas as a first-class value.
- **`yield`-based generator functions:** `fn gen() { yield 1; yield 2; }` — a function containing
  `yield` becomes a lazy generator (Python). Reuses interplay with `for x in gen()`.
- **Default/optional/rest params polish:** already present; add **key-only params**? (Python):
  `fn f(a, *, b) { }` — defer (rarely needed); not in scope.

**Design notes**

- `fn` block-lambdas need a `Closure` value that captures env (exists for nested `fn`s) but with
  an inline `{ }` body — new `ExKind::FnExpr` or reuse `Lambda` with a body.
- Generator functions need a resumable frame or desugar to a state machine; **desugar approach**:
  rewrite generator `fn` into a `struct`/iteration protocol (SP-10) holding the current PC —
  composes with the VM cleanly.

**Exit criteria:** `fn` alias, block-lambdas, generator `yield`-functions in `examples/
42_functions_co`; corpus green; negatives for `yield` outside generator.

```kn
fn add(a: int, b: int) -> int {
    return a + b;
}

var op: fn(int, int) -> int = add;
var doubled = Map(xs, fn (v) { return v * 2; });

fn gen() {
    yield 1;
    yield 2;
}
```

**Goal:** accept `fn` as `fn` sugar, add multi-statement `{ }` block-lambdas, and `yield`-based generator functions.
**Must Follow:** `fn` is a soft contextual keyword in declaration position (no reserve); block-lambdas are first-class values reusing closure capture.
**Important Note:** the core (`fn` alias, block closures, generators) already ships; the only open item is a resumable generator frame — do not regress examples 41/44.

> Reference: rust — block closures `|x| { }`; cpython — generator `yield`; go — functions.

---

## Phase SP-12 — Type-System Surface Expansion (TS/Rust/Java modern)

**Goal:** widen _type_ syntax (not just value syntax), all gradual/opt-in, keeping existing checks.

**Feature list**

- **Union / intersection types** (TS): `int | string`, `A & B` — supported for `any`-narrowing
  and `is` type-tests (SP-5). `any` = `int | string | …`.
- **`as` casts are sugar:** `x as Type` already exists (`Cast`); add **`is` narrowing** promotion
  and `typeof` (SP-5) integrated.
- **Record types (Java records / Rust structural):** `record Point(x: int, y: int)` — value
  equality, auto-`repr`, immutable (PLAN.md Phase 6.2); here as syntax, trimmed.
- **`option[T]` / `result[T,E]` combinators** as chaining sugar: `x.map(f).unwrap_or(d)`
  (Rust), `?` (have), `try` (have) — surface the already-lowerable `result` ops:
  - `.map(f)`, `.and_then(f)`, `.unwrap_or(d)`, `.ok_or(e)`, `result.ok()`/`.err()`.
- **Named & default type params** (Go/Java style): `fn pair[K, V = string]()`, `struct G[T = int]`
  (PLAN.md Phase 6.3). Soft extensions to `type_params`.
- **`type` aliases:** `type Id = int; type Handler = fn(HttpCtx) -> Json;` (Go `type`, TS type
  alias) — big readability win, cheap alias resolution in checker.
- **`impl` blocks for free (extension) functions** already exist; add **`interface` sugar** =
  structural trait bound (SP-5).

**Design notes**

- Union/`any` handled in checker; `type` alias is a name-resolution table entry; records reuse
  `struct` lowering with an `@record`/`record` marker.
- All additive via new (contextual) tokens/types; no change to existing type rules.

**Exit criteria:** union/record/type-alias/named-default-params/result-combinators in
`examples/43_types.kn`; corpus green; negatives for bad alias/union uses.

```kn
type Id = int;
type Handler = fn(HttpCtx) -> Json;

record Point(x: int, y: int);

fn pair[K, V = Str]() -> Point {
    return Point(0, 0);
}

var r: result[int, Str] = Ok(3);
print(r.unwrap_or(0));
```

**Goal:** widen type syntax — union/intersection, records, `option`/`result` combinators, named/default type params, and `type` aliases — all gradual and opt-in.
**Must Follow:** union/`any` live in the checker, `type` aliases are name-resolution entries, and all additions preserve existing type rules.
**Important Note:** `record` is syntax sugar over `struct` + `@record`/`@derive`, not a new runtime kind; layout conflicts must surface at check time.

> Reference: cpython — `type` alias statement (`cpython/Grammar`); rust — `Result`/enums; go — `type` declarations.

---

## Phase SP-13 — Pattern-Power-Up to near-Rust `PatKind` parity

**Goal:** close the pattern gaps from `docs/FEATURE_GAP_ANALYSIS.md §3.2`, making `match`/`when`
comprehensive.

**Feature list**

- **`Ref`/deref patterns:** `case &x { }`, `case *p { }` — borrow/deref a matched value.
- **Nested `@` anywhere** (not just binding-first): `case (x @ 1..=9) @ even()`-style (guards cover
  even; @ nests).
- **Constant / struct-pattern with rest:** `case Point { x: 0, .. }` (Rust struct patterns —
  matches fields by name, `..` ignores the rest). Currently only positional `Point(x: 0)` + tuple
  patterns; add `{ field: pat, .. }` form.
- **`Guard` as `is`-predicate arms** already via `case p if g`; add **English `where` arm guard**:
  `case p where p > 0 { }` (soft keyword, plain-word flavor).
- **Never pattern `!` / irreconcilable `_!`** for exhaustive `match` returning `none`.
- **Slice with typed rest / sub-slices:** `case [a, b, ..]` (have), add **`case [x, .., y]`**
  head+tail (Python starred) — one pattern, both ends.
- **String/char range patterns** (have) extended to support `case c in 'a'..='z'` style.

**Design notes**

- Struct `{ field: pat, .. }` patterns need a new `Pat` sub-kind (`Obj`) — checker maps fields by
  name; runtime `matchPat` builds a lookup. Contained, additive.
- All lexer tokens exist (`{`,`}`,`..`,`..=`,`.`).

**Exit criteria:** the full pattern matrix in `examples/44_patterns.kn`; negatives for bad struct/
never patterns; corpus + `tests/pattern/` green.

```kn
match p {
    case Point { x: 0, y: y } -> print(y)
    case [first, .., last] -> print(first, last)
    case n where n > 10 -> print(n)
    else -> print("other")
}
```

**Goal:** close the pattern gaps — ref/deref patterns, nested `@`, struct `{ field: pat, .. }` patterns, `where` guards, never patterns, and head+tail slices.
**Must Follow:** all tokens already exist; add only a struct-pattern `Pat` sub-kind (`Obj`) and name-based field lookup.
**Important Note:** nested `@` must not re-open the decorator/alias ambiguity — inside a pattern `@` stays alias-only (§2.2).

> Reference: rust — struct/ref patterns (`rust/compiler/rustc_pattern_analysis`); cpython — starred slice patterns.

---

## Phase SP-14 — Interop- & AOT-Ready Syntax (faster like C/C++, plus FFI ergonomics)

**Goal:** syntax that makes Kirn _fast_ and _low-level-approachable_ without leaving the friendly
surface.

**Feature list**

- **`asm`-style / raw pointer / `volatile` sugar:** extend `unsafe` with `cast(x, Type)` (have
  `as`), `&addr(x)` alias (have `&`), `alignof(Type)`, `sizeof(Type)` builtins (C).
- **`@inline`/`@noinline`/`@pure`** attributes (SP-1) consulted by AOT.
- **`bitfield` / packed struct sugar:** `struct Bit { a: u1; b: u3; }` (C bitfields, C23 `_BitInt`)
  — for FFI/networking; lower to packed bytes.
- **`comptime`/`constexpr` blocks** (Zig/C23): `const`-fold anything marked `@const` →
  enables SP-1 `@pure` folding in the optimizer.
- **`var`-value ergonomics + type elision:** `var x = 5;` (have), add **`auto`-style inference**
  (it already infers); add **`typeof` for casts** (have in SP-5).
- **`defer` in `for`/block scope** (Go `defer` in loops) — formalize loop-body defer unwind.
- **C-`struct`/union FFI `repr` decorators:** `@repr(C)` on `struct` for ABI-stable layout
  (Rust `#[repr(C)]`), read by the FFI pathway.

**Design notes**

- `@repr(C)`, `sizeof`, `alignof`, bitfields are compiler-consulted annotations; `constexpr`
  folding relies on `@pure` + a constant evaluator in the checker/optimizer.
- All surface additive; AOT lowering gated behind `--release`/native path (PLAN.md Phase 8/13).

**Exit criteria:** `sizeof/alignof/cast/&addr`, `@repr(C)` structs, `@inline/@pure@const`,
bitfield structs verified in `examples/45_lowlevel_aot.kn`; FFI example `24` still green.

```kn
@repr(C)
struct Header {
    magic: u4;
    flags: u4;
    size: u32;
}

var n = sizeof(Header);
var p = &addr(header);
var q = p as *Header;
```

**Goal:** add C/Zig-facing syntax (`sizeof`, `alignof`, `cast`, `&addr`, bitfields, `comptime`, `@repr(C)`) for fast, low-level, FFI-ready code.
**Must Follow:** these are compiler-consulted annotations/builtins; AOT lowering is gated behind `--release`, and surface syntax stays friendly.
**Important Note:** `@repr(C)` must reuse the SP-1 decorator grammar — no separate attribute channel; `comptime` is the same evaluator used by SP-15.

> Reference: rust — `#[repr(C)]`; zig — `comptime`/`@sizeOf` (`zig/src/Sema.zig`).

---

## Phase SP-15 — Macro-Lite: Expression Templates & Compile-Time Expansion

**Goal:** a pragmatic metaprogramming surface _without_ a full macro system, giving some of Rust
`macro_rules!`/C `#define` power with the decorator + reflection machinery.

**Feature list**

- **`@comptime` expression that expands:** `@comptime(f(x))` evaluates `f(x)` at compile time when
  `@pure`, replacing the node with a constant (ties SP-14).
- **`defer`d/`template` strings** for code building under `reflect` (build DSLs at runtime).
- **`gen`/`derive` via decorators:** `@derive(repr, eq, clone)` on a `struct` auto-generates
  methods (Rust `#[derive]` analog) — the highest-value macro goal, expressed as _builtin
  decorators_ (SP-6) rather than new syntax.
- **Module-level `@import` / re-export reuse** (have `export`).
- Keep **out**: full `macro_rules!`, `#embed` (add later if needed), build-tags `#[cfg]`
  (deferred to PLAN Phase 9-level cargo-style config).

**Design notes**

- `@derive` implemented as a compiler pass that, given the struct `Stmt`, appends synthesized
  method `Stmt`s — reuses the AST builder + `reflect`; no separate macro language.
- Compile-time eval requires the `@pure`/constant evaluator from SP-14.

**Exit criteria:** `@derive(repr, eq, clone)` produces working methods; `@comptime` constant-folds;
examples `46_meta.kn`; corpus green; negatives for deriving on unsupported kinds.

```kn
@derive(repr, eq, clone)
struct Point {
    x: int;
    y: int;
}

const K = @comptime(Factorial(5));
print(K);
```

**Goal:** a pragmatic metaprogramming surface — `@comptime` constant expansion and `@derive`-generated methods — via decorators/reflection, not a macro language.
**Must Follow:** `@derive` is a compiler pass appending synthesized method statements; compile-time eval requires the `@pure` evaluator (SP-14).
**Important Note:** full `macro_rules!`/`#define`/`#[cfg]` stay out of scope by design (one obvious way); re-enter only with a dedicated RFC.

> Reference: rust — `#[derive]` (`rust/compiler/rustc_builtin_macros`); zig — `comptime`.

---

## Phase SP-16 — Unified Iteration + Streaming / Lazy Pipelines (advanced)

**Goal:** full lazy, composable dataflow — a Go/Rust/TS-style "chain everything" surface backed by
`gen`/generators and method-views.

**Feature list**

- First-class **lazy views** composed through `fn`/block-lambdas (SP-11) and SP-9 method-views;
  materialize on `collect`/`to_list`/`for`.
- **Error-returning iteration** in pipelines: `xs.map(f?)` auto-`?`-propagates a `result` (Rust
  `?` in iterator adapters).
- **Parallel/batched iteration** hook: a `@par` / `par(xs, fn)` that maps over `spawn` (ties
  concurrency) — speculative, gated behind SP-10 protocol + Phase 14 scheduler.
- **`tap`/`peek`** debug adapters.

**Design notes**

- Composability requires the generator/`gen` (SP-11) + `iterateSeq` + method-views all returning
  the same lazy `View` value type — a new (opaque) `VK` or reuse `Gen`.
- Purely additive; performance target: chained adapters run without materializing in between
  (the VM loop stays tight; PLAN Bench Phase 8 validates).

**Exit criteria:** a multi-stage lazy pipeline (`read_lines(f).filter(…).map(…).take(10)`),
error-propagating `?` adapters, and (stretch) `par(...)` verified in `examples/47_pipelines.kn`;
noism materialization regression vs `for`-loops.

```kn
var lines = Read_Lines("access.log");
var top = lines
    .filter(fn (l) { return Contains(l, "GET"); })
    .map(fn (l) { return Len(l); })
    .take(10)
    .to_list();
print(top);
```

**Goal:** make lazy, composable, error-propagating pipelines first-class (`filter`/`map`/`take`/`collect`, `?` in adapters).
**Must Follow:** all adapters return the same lazy `View` value and materialize only on `to_list`/`collect`/`for`; composable with SP-11 generators.
**Important Note:** the performance contract is "no intermediate materialization" — a regression vs a plain `for`-loop must fail the phase.

> Reference: rust — lazy `Iterator` adapters; go — range/iterators (`go/src/iter`).

---

## Phase SP-17 — Consolidation: Back-Compat, Fmt, Docs, Edition Mechanics

**Goal:** make every new surface robust and teachable, with migration tooling.

**Feature list**

- **`kirn fmt`** adoption (PLAN.md Phase 9.1) extended to all new sugar (decorators, `->` match
  arms, `fn` block-lambdas, walrus, `@`-lines) — canonical print.
- **`kirn doc`** / `kirn --explain` for new constructs; update `grammar/kirn.ebnf` notes to
  ratify the soft keywords (`unless, until, each, when, otherwise, loop, repeat, once, yield,
where, fn, step`) and new tokens (`:=`, `??`, `..`-step, struct-pattern `{..}`).
- **Edition/bump mechanics** for future reserved-word additions (only if a soft keyword is later
  promoted) — a `#![kirn(edition="2026")]`-style attr (PLAN Phase 15-style governance).
- **Migration hints:** `kirn fmt --migrate` rewrites `for`->`each`-style suggestions? No — keep
  old forms forever; only _add_ idiomatic spelling docs.
- **Full corpus/negative/diag test refresh** across every phase; a single `tests/syntax/` suite.

**Exit criteria:** `kirn fmt` idempotent on all new examples; `kirn doc` renders the new syntax;
force-`kirn check` clean on corpus + syntax suite; a versioned language-feature table in
`docs/FEATURE_GAP_ANALYSIS.md` updated.

```kn
@deprecated("use ServeV2")
fn handle(ctx: HttpCtx) -> Json {
    return match ctx.method {
        case "GET" -> Json(ctx.query)
        else -> Json("")
    };
}
```

**Goal:** make every new surface robust and teachable via `kirn fmt`, `kirn doc`, edition mechanics, and a full syntax test suite.
**Must Follow:** old spellings stay valid forever; only new idiomatic spellings are documented; `fmt` is idempotent on all examples.
**Important Note:** any soft keyword later promoted to reserved requires an edition bump — this phase owns that migration path, not the feature phases.

> Reference: go — `gofmt` canonical printing (`go/src/go/format`); rust — editions/`rustfmt`.

---

## Roadmap Summary (suggested solo-dev order, value-first)

```
SP-1  @ decorators/attributes        (headline ask, high visibility)      ─┐
SP-2  := assignment expressions        (expression power)                  │
SP-3  match/switch-expresssions        (expression power, Java/Rust)      │  ~ contiguous, small,
SP-4  English-friendly flow            (unless/until/each/when/??)        │  orthogonal, high value
SP-5  any/dynamic + typeof/is          (dynamic surface)                  │
SP-8  builtin fn breadth               (more built-ins)                   │
SP-6, SP-7, SP-9, SP-10                (decorators-runtime, with,         │  build on SP-1..5,8
                                        collections, iteration)           │
SP-11 fn-alias/block-lambda/gen        (function ergonomics)              │
SP-12 type surface (union/record/alias)┘----------------------------------┘

SP-13 patterns  ──►  SP-14 lowlevel/AOT  ──►  SP-15 macro-lite  ──►  SP-16 pipelines
SP-17 consolidation (fmt/doc/editions) cross-cuts all
```

**Short version for a single dev:** 1 → 2 → 3 → 4 → 8 → 5 → 9 → 10 → 11 → 12 → 6 → 7 →
13 → 14 → 15 → 16 → 17.

> **Status overlay (dedup — which phases are effectively done):** the cores of **SP-3** (match
> expr), **SP-5** (any/dynamic), and **SP-11** (fn-alias / block closures / generators) are
> **implemented**; much of **SP-8** is implemented. The genuinely-open high-value work is SP-1
> (decorators), SP-2 (walrus), SP-9 (comprehensions/splat), SP-10 (iteration protocol), SP-12
> (type surface), and SP-13 (pattern parity).

For **maximum user-visible benefit quickly**: SP-1 (decorators) → SP-8 (builtins) → SP-2 (walrus)
→ SP-3 (match-expr) → SP-4 (English flow) → SP-9 (collections) → SP-5 (any/typeof).

---

## Design decisions that need ratification (before coding)

| #   | Decision                  | Options                                             | Recommend                                 |
| --- | ------------------------- | --------------------------------------------------- | ----------------------------------------- |
| D1  | Decorator "name" spelling | `@name`, `#[name]`, `[[name]]`, `name:`             | `@name`, `@name(...)`, `@name(a:1)` (§2)  |
| D2  | Decorator semantics       | Tier-1 compile attrs only / also Tier-2 runtime fns | Tier-1 now, Tier-2 in SP-6                |
| D3  | Walrus spelling           | `:=` (Python)                                       | `:=` (SP-2)                               |
| D4  | Match-arm value form      | `->` (Java) vs `yield` vs both                      | both (`->` sugar, `yield` for blocks)     |
| D5  | English aliases           | `unless/until/each/when/otherwise/loop/repeat/once` | adopt all as soft keywords (SP-4/7)       |
| D6  | Null-coalescing           | `??`, `?.`, `or_default`                            | `??` + existing `?.`                      |
| D7  | Dynamic type keyword      | `any`, `dynamic`, both                              | `any` + alias `dynamic`                   |
| D8  | Step-ranges spelling      | `0..10 step 2`, `range(s,e,step)`                   | `0..10 step 2`                            |
| D9  | Dict iteration            | `for k,v in d` vs `.items()`                        | `for k,v in d` (tuple-pattern, SP-10)     |
| D10 | `str()` builtin           | add `str(x)` (name) / keep none                     | add `str(x)` (currently absent by design) |
| D11 | Block-lambda keyword      | `fn (x) { }` / `fn (x) { }` / `\|x\| { }`           | `fn (x) { }` (SP-11)                      |
| D12 | `fn` alias promotion      | always-soft / edition-gated                         | always accepted (soft)                    |
| D13 | Struct-pattern brace form | `Point { x: 0, .. }`                                | add (SP-13)                               |
| D14 | Generator keyword         | `gen` marker / implicit via `yield`                 | implicit via `yield` (symbol-free)        |

---

## Appendix A — Evidence base (file:line)

- Current builtin set (the "too limited" surface): `src/interp/runtime.cpp:485-613`
  (`print,len,sqrt,ord,chr,assert,assert_eq,range,panic,catch_panic,printf,strlen`,
  pseudo-modules `math,time,io,mem,json,text,os`).
- `@` today = pattern alias only: `src/parser/parser.cpp:936-941` (`atOp("@")` in
  `parseCtorOrBind`); no decorator/attribute code anywhere (`rg` 0 hits).
- Frozen keyword set: `grammar/kirn.ebnf:49-55`; `src/lex/lexer.cpp:38-50`
  (soft keywords therefore needed).
- Assignment is a statement only (§4.3): `grammar/kirn.ebnf:229-239, 423-431`.
- `any`-style markers exist as poison only: `src/sema/type.h:15-27` (`Error`/`Unknown`);
  real `any` is additive.
- Lambdas single-expression only (§4.10): `grammar/kirn.ebnf:295-299`.
- Match is statement-only today; no yield: `grammar/kirn.ebnf:275-276`; `src/ast/ast.h:225-231`.
- Comprehension clauses already generalize (`CompClause`): `src/ast/ast.h:92-97`.
- Iteration entry point: `src/interp/runtime.cpp:2847` (`iterateSeq`), `runtime.h:137-139`.
- Named args / `CallArg` (splat hook): `src/ast/ast.h:87-90`.
- Method lookup (for later caching/method-views): `src/interp/runtime.cpp:1895-1913`.
- Lint infra to hang `@warn/@allow/@deny` on: `PLAN.md` Phase 2 (complete); `LintConfig` in checker.
- Reflection/dynamic plans to build SP-5/6 on: `PLAN.md` Phase 5 (§5.1–5.3); `docs/FEATURE_GAP_ANALYSIS.md` §3.3.
- Records / exhaustiveness / results: `PLAN.md` Phase 6.
- VM + native AOT target for "faster like C/C++": `PLAN.md` Phase 4 (bytecode VM), Phase 8 (AOT), Phase 13 (JIT).

## Appendix B — Research references (web, 2026)

- **Decorators/attributes across languages:** Python (`wiki.python.org/moin/PythonDecorators` —
  indicator-token trade-offs incl. `@`, keywords, `|`, `%`), TypeScript `@` experimental/Stage-3
  decorators (`typescriptlang.org/docs/handbook/decorators.html`, devblogs TS 5.0), JS TC39 Stage-3
  decorators semantics (JS + context API), Rust `#[…]` attributes (`doc.rust-lang.org/reference/
attributes.html` — modeled on ECMA-335/C#), C# `[Attr]`, Java annotations (reflection-based),
  C23 standardized `[[…]]` attributes (`cppreference.com/w/c/23` — `[[deprecated]]`, `[[fallthrough]]`,
  `[[noreturn]]`, `[[unsequenced]]`, `[[reproducible]]`).
- **Assignment expressions:** Python PEP 572 walrus `:=` (rationale: avoid subexpression
  duplication; distinct from statement assignment so no identical context makes both valid;
  parenthesization requirements; disallowed in some positions) — `peps.python.org/pep-0572/`,
  `realpython.com/python-walrus-operator`.
- **Match/switch-family expressions:** Java 13 `switch` expressions with `->` and `yield`
  (arrow-form as sugar; `yield` returns value from arm block; exhaustiveness without `default`)
  — JEP 354, `stackoverflow.com/questions/58049131`.
- **English-friendly / readable syntax:** Python "reads like pseudocode, uses English keywords
  where other languages use punctuation" (Wikipedia "Python"); language-design guidance that
  word-keywords (`and/or/not/in/is`) aid beginners and readability.
- **Structural/duck typing & method-set interfaces (Go), union types & `any` (TypeScript),**
  records/sealed/`instanceof`-pattern (Java), generics defaults — summary in
  `docs/FEATURE_GAP_ANALYSIS.md` §3.3 and the reference trees under
  `C:\Users\rkriad585\Projects\go-rust-source-code` (`go-master`, `rust-main`).
