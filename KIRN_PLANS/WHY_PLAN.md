# Kirn — "Why Adopt Kirn?" Roadmap (WHY_PLAN)

## Goal

This is the rationale ("why") plan for **Kirn**: it makes the language's "why" explicit for every
design decision, framing each as an **industry-standard, AI-first** choice — **faster than Rust and
Go** — that follows, or consciously rejects, a named reference language (`rust`, `go`, `cpython`,
`ruby`, `Nim`, `Odin`, `v`, `zig`, `llvm`) rather than inventing new machinery. The goal is a living
decision ledger in which every capability earns its place from a real adoption driver plus a proven
pattern, so no feature ships on novelty alone.

## Must Follow

- **Two executables, exactly.** The toolchain build ships only `kirn` (driver) and `kirnc`
  (checker/compiler) — no auxiliary binaries, no extra install step.
- **No over-engineering.** Every design decision's rationale must cite the reference language it
  follows — or, where Kirn diverges, explicitly reject the reference it declines. Novelty is not a
  justification.
- **AI-first is structural, not cosmetic.** The rationale must keep the grammar small and regular
  (few duplicate keywords, uniform spellings) so agents generate correct Kirn the first time, and
  machine-readable docs/grammar feed model context.
- **Kirn style.** Types are lowercase width micros + capitalized primitives (`Int`, `Float`, `Bool`,
  `Str`, `Char`, `BigInt`, `Rational`, `UInt`/`UFloat`, `Int4..UInt256`, `Float4..UFloat256`,
  `u4..u256`, `i4..i256`); free builtins/types are Capitalized (`Str(...)`, `BigInt(...)`,
  `__Sqrt`); methods stay lowercase.
- **Measurable or reference-citable.** Every "why" claim carries a `kirn bench` measurement or a
  verified reference-language citation; otherwise it is flagged aspirational and tied to the phase
  that proves it.

## Important Note

The existing structure of this file is preserved: the phase roadmap (WHY-1 … WHY-15), the per-phase
Why / Goal / Capability / Design-notes / Exit-criteria blocks, the ratification ledger (§"Design
decisions that need ratification", decisions E1–E8), and both appendices all remain intact and
authoritative. The triple below each section (`**Goal:**` / `**Must Follow:**` / `**Important
Note:**`) is additive — it wraps and cross-references the existing prose without deleting,
reordering, or re-scoping any decision; ratified decisions (E1–E8) and the `[MOSTLY IMPLEMENTED]` /
`[PARTIALLY IMPLEMENTED]` statuses stay as written.

**Status:** Proposed roadmap · Living document · Version 1.0
**Scope:** Answer _why anyone would choose Kirn_ by mapping the adoption drivers of the four
dominant language families — Python (DX/productivity), Go (simplicity/concurrency/toolchain),
Rust (safety/zero-cost), C/C++ (raw performance/control/FFI) — onto **concrete Kirn features**,
then sequencing those features into phases so each "why" becomes a deliverable capability.

This is a companion to `PLAN.md` (strategic, infra-centric) and `SYNTAX_PLAN.md` (syntax-centric).
`WHY_PLAN.md` is **demand-driven**: each phase is justified by a real-world adoption driver first,
then implemented. The three documents cross-reference and share evidence tables.

Like the sibling plans, this is _choice-bearing_: every ratification point lists options,
trade-offs, and a recommendation. **Unless marked otherwise, phases are future work — but several
"whys" are already substantially DELIVERED** and are annotated `[MOSTLY IMPLEMENTED]` /
`[PARTIALLY IMPLEMENTED]` at their headings below: **WHY-1** (batteries-included builtins is largely
live — see `EXP_PLAN.md` §9 for the shipped list), **WHY-7** (the 10 stdlib modules now exist in
`stdlib/lib/`), **WHY-8** (the `spawn/chan/select` engine is present), and the WHY-4 toolchain core.
Those phases are re-scoped to "close residual gaps", not build from scratch.

---

## 0. Orientation — The four "why"s, and what each demands of Kirn

**Goal:** Ground every Kirn capability in the adoption driver it answers, so the four family "whys" (Python/Go/Rust/C-C++) map to concrete, reference-cited features instead of taste.
**Must Follow:** Keep the table the single source of truth — each row names a driver, the capability it implies, and the anchor phase(s) that deliver it.
**Important Note:** Python→WHY-1/2/5/6/7, Go→WHY-3/4/8/11, Rust→WHY-9/10/12, C/C++→WHY-11/13/14; a capability with no reference row is deferred.

Web research (Appendix B) plus `SYNTAX_PLAN.md`/`PLAN.md`/`docs/FEATURE_GAP_ANALYSIS.md` give a
clear, evidence-backed picture of **why each major language family wins developers**, and the
Capability each one implies:

| Family     | Why they win (2026 research)                                                                                                                            | Capability Kirn must deliver                                                                             | Anchor phases |
| ---------- | ------------------------------------------------------------------------------------------------------------------------------------------------------- | -------------------------------------------------------------------------------------------------------- | ------------- |
| **Python** | Productivity & DX; batteries-included stdlib; dynamic typing; comprehensions; huge ecosystem; ~38% usage vs JS 36% (SO 2026); default for ML/AI         | Rich builtins, `any`/dynamic typing, comprehensions, fast-to-write syntax, `str()`/collection ergonomics | WHY-1,2,5,6,7 |
| **Go**     | Simplicity, readability, fast builds, built-in concurrency (goroutines), batteries toolchain (fmt/gofmt/doc/test/build/clean module fmt/dist), `go vet` | `for`-over-anything, `defer`, goroutines (have), `_ =` blank, uniform toolchain, gofmt-style vigor       | WHY-3,4,8,11  |
| **Rust**   | Memory safety _without_ GC, zero-cost abstractions, fearless concurrency, strong types, government policy now pushing memory-safe languages             | Borrow checker (have roadmapped), pattern power, trait system, exhaustive match, zero-cost (AOT)         | WHY-9,10,12   |
| **C/C++**  | Raw performance & control, low-level/embedded/kernels, FFI to everything, de facto stable ABI                                                           | AOT/JIT (fast), `sizeof/alignof`, `@repr(C)`, FFI/interop, pointer/raw access                            | WHY-11,13,14  |

**Cross-cutting "why":** every family's dominance points at **developer experience** — docs,
linting, formatting, quick iteration, great errors. Kirn already has Phases 1–3 (diagnostics,
lints, convention files) and a Go-style CLI. WHY_PLAN protects and completes that ergonomic core,
then layers language power.

---

## 1. Guiding principles

**Goal:** Keep these six principles as the rubric every phase's rationale is scored against before any syntax or API is fixed.
**Must Follow:** Feature-first, piggyback existing plans, batteries-included stdlib, simplicity over symbols, zero-cost lowering, and per-phase verifiability (corpus ≥ 32/32).
**Important Note:** Each principle traces to a reference strength (Python productivity, Go readability, Rust/C zero-cost); a feature serving no "why" is deferred.

1. **Feature-first, reason-first.** Every phase is justified by a _why_ (an adoption driver) before
   any syntax/API is specified. If a feature doesn't serve a "why", it's deferred.
2. **Piggyback existing plans.** Prefer adopting capability from `SYNTAX_PLAN.md` /
   `PLAN.md` where the work is already specified — WHY_PLAN lives "above" those two, tagging and
   sequencing their phases by _why_ rather than re-specifying them.
3. **Batteries-included stdlib is a moat.** Python's largest single driver is stdlib + ecosystem.
   Kirn's `pin.kn` + `kirn_libs` + `kirn add/install` already form the packaging spine; WHY_PLAN
   widens the builtin/stdlib surface and the registry story.
4. **Simplicity is a feature, not a bug.** Go wins on _being able to read any codebase_. New Kirn
   features must preserve the "read it in one sitting" property; prefer words over symbols
   (matches SP-4).
5. **Zero-cost is a promise.** Features that touch hot paths must lower to the bytecode VM
   (`PLAN.md` Phase 4) and native AOT (Phase 8) with no new runtime cost (matches SYNTAX_PLAN
   principle 4).
6. **Verifiable per phase.** Each phase ends with `examples/*.kn`, positive/negative tests, and a
   corpus that stays ≥ 32/32 (and grows).

---

## 2. Why Kirn — one-paragraph thesis

**Goal:** State the thesis in one breath — Rust/C speed, control and safety, Python productivity and stdlib, Go simplicity and toolchain — with English-first syntax readable as prose.
**Must Follow:** Order deliverables cheapest-first (Python-productivity + Go-toolchain, largely present) before the hardest moats (Rust-grade safety, C-grade AOT/FFI).
**Important Note:** The thesis stays a hypothesis until a `kirn bench` measurement or a verified reference citation confirms it.

Kirn is the **readable, batteries-included systems language**: it wants the _speed, control, and
safety of Rust/C_ _and_ the _productivity, dynamic typing, and stdlib of Python_ _and_ the
_simplicity, concurrency, and toolchain of Go_ — with an English-first syntax that a solo
developer (or a team) can read like prose. WHY_PLAN turns that thesis into an ordered set of
deliverables, starting with the cheapest wins (Python-productivity + Go-toolchain, largely
present) and ending with the hardest moats (Rust-grade safety, C-grade AOT/FFI).

---

## 3. Roadmap summary (why → capability → phase)

**Goal:** Present the why→capability→phase map as the ordering spine the whole plan is executed from.
**Must Follow:** Address an adoption driver before its syntax, and treat the ordering charts as sequencing aids rather than separate scope.
**Important Note:** The second chart (below) deliberately duplicates this one; §3 remains canonical.

```
Python  productivity  ── WHY-1 batteries-included builtins
                        ── WHY-2 any/dynamic typing + comprehensions
                        ── WHY-6 collection ergonomics
                        ── WHY-7 stdlib breadth + registry
Go      simplicity     ── WHY-3 for-over-anything / iteration
                        ── WHY-4 toolchain completion (fmt/repl/check/vet)
                        ── WHY-8 concurrency ergonomics (already have)
Rust    safety         ── WHY-9 pattern & trait power-up
                        ── WHY-10 borrow-checker staging (PLAN Phase 12)
C/C++   perf/control   ── WHY-11 AOT/JIT "fast like C" benchmarking
                        ── WHY-12 low-level surface (sizeof/alignof/@repr(C))
                        ── WHY-13 FFI/interop
                        ── WHY-14 safety-tooling / `kirn vet`, audit
cross  DX/ecosystem    ── WHY-15 consolidation: docs, format, editions, registry
```

**Suggested solo-dev order (value-first):** WHY-1 → WHY-4 → WHY-3 → WHY-6 → WHY-2 → WHY-7 →
WHY-8 → WHY-5 → WHY-9 → WHY-14 → WHY-11 → WHY-12 → WHY-13 → WHY-10 → WHY-15.

For **maximum user-visible benefit quickly** (the "wow" order): WHY-1 (builtins) → WHY-4
(toolchain) → WHY-3 (for-everything) → WHY-2 (any/dynamic) → WHY-7 (stdlib/registry).

---

## PHASES

**Goal:** Standardize every phase to one reviewable shape so rationale, reference, and proof are compared uniformly.
**Must Follow:** Each phase carries why / goal / capability / design-notes / adoption-source / exit-criteria, plus a reference citation and one `.kn` illustration.
**Important Note:** Exit criteria are executable (`examples/*.kn`, tests, corpus) — not prose claims.

> Each phase: **why**, **goal**, **concrete capability**, **design notes / rationale**, **what it
> adopts from SYNTAX_PLAN/PLAN** (identity the parent work), and **exit criteria**.

---

## Phase WHY-1 — Batteries-Included Builtins (Python productivity) — **[MOSTLY IMPLEMENTED]**

> **Status/dedup:** the bulk of this phase **ships** — `map filter reduce any all sum max min
enumerate sorted reversed upper lower trim contains starts_with ends_with replace split join
str int float bool type repr` are implemented free builtins (canonical inventory in
> `EXP_PLAN.md` §9; syntax ownership `SYNTAX_PLAN.md` SP-8). **Residual gaps here:** `zip`,
> `flatten`, `take/skip/first/last/count`, `round/floor/ceil/clamp/pow`, `typeof`, `input`, and
> prelude `pi`/`e`. Re-scope this phase to "close the residual builtin gaps."

**Why:** Python's #1 adoption driver is productivity from its stdlib + builtins — you solve a
task in a line that other languages need a library for. Kirn's builtin surface today is thin
(`print,len,sqrt,ord,chr,assert,assert_eq,range,panic,catch_panic,printf,strlen`).

**Goal:** give Kirn a Python-grade day-one bag of builtins so most scripts run with zero imports.

**Must Follow:** Each helper is a free runtime builtin registered in `installBuiltins()` and lowered as an ordinary call (zero-cost), spelled Kirn-style (`Str`, `Int`, `Min`, `Max`).
**Important Note:** Prefer names over symbols (principle 4); `Str(x)` is the CapCase spelling and remaining gaps are `zip`/`flatten`/`round`/`typeof`/`input`.

> Reference: cpython — batteries-included free builtins (`len`/`sorted`/`enumerate`) over `cpython/Objects` and `cpython/Lib`.

```kn
fn main() -> none {
    var xs = [3, 1, 4, 1, 5];
    print(sum(xs), Min(xs), Max(xs));
    var odd = filter((x) => x % 2 == 1, xs);
    print(join("-", map((x) => Str(x), odd)));
}
```

**Capability (adopts `SYNTAX_PLAN.md` SP-8 + adds stdlib-first builtins)**

- `Str(x)` — add the deliberately-absent `Str()` (D10 in SYNTAX_PLAN).
- Collection helpers: `map, filter, reduce, sorted, reversed, min, max, sum, any, all, zip,
enumerate`.
- Text: `split, join, trim, upper, lower, replace, starts_with, ends_with, contains, format`.
- Numeric/any: `Int(x), Float(x), Bool(x), type(x), isinstance(x, T), len`.
- Conversion helpers that lower cleanly to existing ops.

**Design notes / rationale**

- Nothing new to the AST — each helper is a `fn`-shaped builtin registered in
  `installBuiltins()` (`runtime.cpp:485-613`); bytecode and AOT lower them as calls → zero-cost.
- `Str()` restores the missing primitive→string coercion users expect (currently only via
  f-strings).
- Prefer _names over symbols_ (Python-style) to keep code readable (principle 4).

**Exit criteria:** `examples/34_batteries.kn` runs using only builtins (no imports); corpus stays
green; `Str()`/`map`/`filter`/etc. covered by `tests/syntax/builtins_*.kn`.

---

## Phase WHY-2 — Dynamic & Gradual Typing + Comprehensions (Python/TS)

**Why:** Python's dynamic typing is a productivity engine (no type ceremony to iterate fast);
TypeScript proved gradual typing wins teams (opt-in). Kirn is statically typed (a safety asset)
but has no escape hatch.

**Goal:** adopt `SYNTAX_PLAN.md` SP-5 (`any`/`dynamic`, `typeof`, `is`), SP-9 (comprehension
power-up), and add gradual-typing toggles — letting a user start dynamic and harden later, the
TypeScript path.

**Must Follow:** `any` is a real tagged type subsuming the `Error`/`Unknown` poison markers, `dynamic` is an alias, and the doctrine stays additive so strict code is untouched.
**Important Note:** Gradual typing is opt-in via `--strict`/`@checked`; cost lands only on `any`-typed values (principle 5).

> Reference: cpython — dynamic typing with no static gate (`cpython/Python`); ruby — duck dispatch via `respond_to?` (`ruby/object.c`).

```kn
fn describe(v: any) -> Str {
    if type(v) == "Int" { return "Int:" + Str(v); }
    return repr(v);
}

fn main() -> none {
    var a: any = 5;
    a = "now a string";
    print(describe(a));
}
```

**Capability**

- `any` type (real, not the poison markers in `type.h:15-27`); `dynamic` as alias; duck-typed
  attribute access; `typeof(x)`, `isinstance`.
- Comprehension power-up: nested `for`/`if` clauses (the `CompClause` in `ast.h:92-97` already
  generalizes), dict/set builders.
- A `--typed`/`--strict` flag or `@checked` decorator to ratchet rigor after prototyping (the Go
  "vet-to-hard" analogue).

**Design notes / rationale**

- The `Error`/`Unknown` markers in `src/sema/type.h:15-27` give the poison-union precedent; real
  `any` subsumes them with a runtime tag — cost only on `any`-typed values, per principle 5.
- Gradual typing lets Kirn win both "fast to write" (Python/JS crowd) and "safe to ship".

**Exit criteria:** `examples/35_dynamic.kn` declares `any` values, coerces, duck-calls; corpus +
negative tests green; strict mode refuses `any` in a checked scope.

---

## Phase WHY-3 — `for`-Over-Anything & Iteration Protocol (Go simplicity)

**Why:** Go's `for range` over slices/maps/strings/channels is one reason Go code is short and
readable. Kirn's `for` is list/range-centric today.

**Goal:** adopt `SYNTAX_PLAN.md` SP-10: `for` over any iterable (dicts with key/value tuple
patterns, strings, channels, custom types with an iteration protocol), plus `break`/`continue`
with labels (have) and `_ =` blank binding (Go).

**Must Follow:** Route every iterable through the single `iterateSeq` entry point; add `_` blank binding and one iteration protocol a `struct` implements.
**Important Note:** Highest readability-per-line Go-parity win — one `for` grammar rule, not per-container variants.

> Reference: go — `for range` over slice/map/string/channel (`go/src/runtime`).

```kn
fn main() -> none {
    var counts = {"a": 1, "b": 2};
    for k, v in counts { print(k, Str(v)); }
    for _ in 0..3 { print("tick"); }
}
```

**Capability**

- `for k, v in d`, `for c in s` (chars), `for v in chan`, `for p in struct` (needs iteration
  protocol — SP-10).
- Blank binding `_ =` (Go `_`), `for _ in x`.
- Iteration protocol a custom `struct` can implement to be iterable in `for`.

**Design notes / rationale**

- Single entry point to extend: `iterateSeq` (`runtime.cpp:2847`); method-set lookup already at
  `runtime.cpp:1895-1913`.
- This is the single most "Go-like" readability win and pairs with WHY-6 (dict ergonomics).

**Exit criteria:** `examples/36_iteration.kn` iterates dict/set/string/chan/custom struct;
corpus green.

---

## Phase WHY-4 — Toolchain Completion (Go batteries / DX)

**Why:** Go's toolchain (gofmt, go doc, go test, go build, go vet, go mod) is a key reason teams
adopt it — tooling _is_ the DX moat. Kirn already has `run/build/test/new/install/add/update/
remove/clone/list/targets/doc` and Phases 1–3 (diagnostics/lints/convention files).

**Goal:** close the remaining Go-toolchain analogues (adopts `PLAN.md` Phase 9 plus `kirn bench`
from Phase 8): `kirn fmt`, `kirn repl`, `kirn check`/`lint`, `kirn get` (module fetch), `kirn
vet`(alias), LSP.

**Must Follow:** Extend the existing `kirn` CLI dispatcher only; `kirn` and `kirnc` remain the only two build artifacts.
**Important Note:** Tooling compounds every other phase; reuse `runProgramSrc`/`buildProgram` for `check`/`fmt` instead of new pipelines.

> Reference: go — uniform one-way toolchain (`gofmt`/`vet`/`test`) in `go/src/cmd/gofmt` and `go/src/cmd/vet`.

```kn
fn add(a: Int, b: Int) -> Int { return a + b; }
fn main() -> none { print(Str(add(2, 3))); }
```

**Capability**

- `kirn fmt` (gofmt-style canonical formatting — pairs with WHY-15).
- `kirn repl` (REPL for the dynamic/`any` surface in WHY-2).
- `kirn check` / `kirn lint` (type-check + lint pass; W0101–W0106 already exist).
- `kirn get` (module resolution, Go-mod-like); `kirn bench` (Phase 8).
- LSP server for editor integration.

**Design notes / rationale**

- Highest-leverage "why" per hour: tooling compounds every other phase's usefulness.
- CLI today (`tools/kirn.cpp:2850-2886`) is the insertion point; the build pipeline
  (`runProgramSrc` at 489, `buildProgram` at 2319) is ready to be reused by `check`/`fmt`.

**Exit criteria:** `kirn fmt` on a messy example yields canonical bytes; `kirn lint` reports
W-numbered findings; `kirn repl` evaluates an `any` expression; corpus unaffected.

---

## Phase WHY-5 — Ecoysystem Story (why install Kirn / why publish) — _name: Ecosystem/Registry_

**Why:** Python's 38% share and Go/Rust growth all trace back to **ecosystem** — packages,
docs (readthedocs/crates.io/pkg.go.dev), and community. A language without a package story dies.

**Goal:** make `kirn_libs` + `kirn add/install` + `kirn doc` + `pin.kn` into a first-class publish/
browse loop with a browsable registry (has `kirn list online`).

**Must Follow:** Publish/browse hangs off the existing `pin.kn` + `kirn_libs` + `kirn doc`; introduce no new package format and no third binary.
**Important Note:** This is the PyPI + Go module-proxy analogue compressed to solo-dev scope — local mirror first, public later.

> Reference: go — module proxy semantics (`go/src/cmd/go`); cpython — PyPI as the adoption flywheel (`cpython/Lib`).

```kn
# pin.kn — the manifest already serves as the registry contract
pub fn root() -> Int { return 42; }
```

**Capability**

- `kirn publish` (pack → register in the kirn-libs registry).
- `kirn doc` already serves markdown + API ref (`tools/kirn.cpp:1469-1721`); add package README
  rendering and a registry homepage.
- Versioning/semver discipline; `kirn update` (have); dependency audit (`kirn vet`-adjacent).

**Design notes / rationale**

- Builds directly on the Phase-3 convention files (`pin.kn` aggregator, `main.kn` entry) and the
  `.kirnlib` bundle + cross-build infra (Phase 10).
- This is the "python/pip" and "Go module proxy" analogue compressed to one solo-dev scope.

**Exit criteria:** publish a sample lib to a local registry mirror, `kirn add` it from a fresh
project, `kirn doc` renders it; `kirn list online` lists it.

---

## Phase WHY-6 — Collection Ergonomics (Python/Go readability)

**Why:** dict/set/list ergonomics are where Python (comprehensions, `.items()`, unpacking) and Go
(struct/literal simplicity) win daily readability.

**Goal:** adopt `SYNTAX_PLAN.md` SP-9 + SP-10 dict ergonomics: `for k, v in d`, comprehensions
over collections, splat/unpacking, `.items()/.keys()/.values()`.

**Must Follow:** All sugar desugars to existing AST/VM nodes (`CompClause`, splat `CallArg`) — no new runtime construct.
**Important Note:** Readability only: `sum(x for x in xs if x > 0)` must lower byte-for-byte to the equivalent loop.

> Reference: cpython — comprehensions + `.items()`/unpacking (`cpython/Lib`); go — composite literals (`go/src`).

```kn
fn main() -> none {
    var d = {"a": 1, "b": 2};
    for k, v in d { print(k, Str(v + 1)); }
    print(Str(sum(x for x in [1, 2, 3] if x > 1)));
}
```

**Capability**

- Dict iteration with tuple-pattern `for k, v in d` (SP-10 D9).
- Splatted args/call `*args, **kwargs` where `CallArg` already supports splat (`ast.h:87-90`).
- Collection comprehension over any iterable (pairs with WHY-2).

**Design notes / rationale**

- Pure sugar → desuggars to existing AST/VM (principle 5). Reuses `iterateSeq` and `CompClause`.
- Readability win (principle 4): `sum(x for x in xs if x > 0)` beats a loop.

**Exit criteria:** `examples/39_collections.kn` uses dict iteration, splice, and a comprehension;
tests green.

---

## Phase WHY-7 — Stdlib Breadth & `pin.kn`-driven Modules (Python batteries) — **[PARTIALLY IMPLEMENTED]**

> **Status/dedup:** the stub→real migration is **largely done** — 10 modules now ship in
> `stdlib/lib/` (`core, collections, io, json, math, os, path, regexp, strings, time`), each with
> a `*_test.kn`. Re-scope this phase to **breadth**: the still-pending deque/Counter/ordered-dict
> types and wider module function sets, spec'd in `STD_LIBS_PLAN.md` (→) — which is the owner for
> module APIs; this phase keeps only the "why" pointer.

**Why:** beyond builtins, Python's _stdlib modules_ (json, re, os, math, time, itertools,
functools) are why people reach for it. Kirn's modules are stubs (`math,time,io,mem,json,text,os`).

**Goal:** convert stub modules into real implementations and treat stdlib as versioned, shipped
`.kn` modules (the `pin.kn` pattern), adopted in `PLAN.md` Phase 7.

**Must Follow:** Ship modules as versioned `.kn` `pin.kn` aggregators over runtime primitives, with `STD_LIBS_PLAN.md` owning the module APIs.
**Important Note:** Prefer pure-Kirn `.kn` (faster to ship, user-extensible, dogfoods packaging); go native only where required.

> Reference: cpython — stdlib modules `json`/`re`/`os` (`cpython/Lib/json`, `cpython/Lib/re`); go — flat `os`/`io` split (`go/src`).

```kn
import json

fn main() -> none {
    var doc = json.loads("{\"n\": 3}");
    print(Str(doc["n"] + 1));
}
```

**Capability**

- Real `json` (encode/decode), `text` (regex/string), `os` (env/files), `io` (read/write),
  `math`/`time` (complete common surface), `mem`, plus `collections` (deque/counter/ordered-dict).
- Each module = a `pin.kn` aggregator exposing a stable public API (Phase-3 convention).

**Design notes / rationale**

- This is Python's #1 "why"; a broad builtin+stdlib surface directly attacks ecosystem demand.
- Deliberately implemented as `.kn` on the existing runtime primitives where possible (faster to
  ship, user-extensible, dogfoods `pin.kn`).

**Exit criteria:** `kirn run` a sample using `json`, `os`, `text`, `collections` without imports
of user code; `kirn doc` shows each module's API; tests per module green.

---

## Phase WHY-8 — Concurrency Ergonomics (Go goroutines, present + honed)

**Why:** Go's goroutines/channels are the cleanest mental model for concurrency and a major reason
for its cloud adoption. Kirn **already has** `spawn`, `chan`, `select`, real OS threads
(`PLAN.md` Phase 14).

**Goal:** polish the existing concurrency into Go-quality _ergonomics_ and safety, not new
machinery.

**Must Follow:** Reuse the shipped `spawn`/`chan`/`select` engine; this phase adds ergonomics and lint only, never a new scheduler.
**Important Note:** Do not re-architect done engineering — surface deadlock/race as `kirn check` warnings (WHY-4).

> Reference: go — goroutines + channels (`go/src/runtime`, `go/src/sync`).

```kn
fn worker(ch: chan Str) -> none { ch <- "done"; }

fn main() -> none {
    var ch = chan Str(1);
    spawn worker(ch);
    for msg in ch { print(msg); }
}
```

**Capability**

- Reader-friendly sugar: `for v in chan` (WHY-3), `select` clarity, `spawn` with closures.
- Deadlock/race awareness surfaced in `kirn check`/lint (WHY-4) as warnings.
- Channel close/range protocol and select-default ergonomics.

**Design notes / rationale**

- Don't re-architect; the hard engineering (threads, mutex/condvar channels) is done. This phase
  is _DX_: name + ergonomics + lint.

**Exit criteria:** `examples/41_concurrency.kn` demonstrates pipelined goroutines with `for in
chan` + `select`; concurrency lint warnings fire on a deadlocked sample; corpus green.

---

## Phase WHY-9 — Pattern & Trait Power-Up (Rust parity)

**Why:** Rust's pattern matching + traits are why people trust it with complex systems; they make
intent explicit and exhaustive.

**Goal:** adopt `SYNTAX_PLAN.md` SP-13 (pattern power-up toward Rust `PatKind` parity) and the
trait/method-set hardening in `PLAN.md` Phase 6.

**Must Follow:** Patterns extend the existing `PatKind` set and trait/method-set machinery; exhaustive checks on sealed enums remain mandatory.
**Important Note:** This is the "safe AND fast to write" moat — Python ergonomics (WHY-2) plus Rust correctness.

> Reference: rust — patterns + traits (`rust/compiler/rustc_ast`, `rust/library/core`).

```kn
struct Point { var x: Int; var y: Int; }

fn main() -> none {
    var p = Point { x: 0, y: 1 };
    match p {
        Point { x: 0, .. } => print("on y-axis"),
        Point { x: n, .. } => print("x = " + Str(n)),
    }
}
```

**Capability**

- Pattern additions: struct-pattern brace form `Point { x: 0, .. }`, nested or-patterns,
  range/slice/rest already present, guards, exhaustive matching checks on sealed enums.
- Trait/impl: blanket impls, trait bounds on generics (have `[T is Bound]`), method-set duck
  typing parity with Go interfaces.

**Design notes / rationale**

- Pairs the Python-productivity surface (WHY-2) with Rust's correctness guardrail — the
  "safe AND fast to write" moat.
- FEATURE_GAP_ANALYSIS §3.2 documents the current pattern gaps precisely.

**Exit criteria:** `examples/42_patterns.kn` uses struct-pattern, or-pattern, sealed-enum
exhaustive match; a non-exhaustive match errors; corpus green.

---

## Phase WHY-10 — Borrow-Checker Staging (Rust memory safety)

**Why:** Rust's #1 adoption driver (2026) is memory safety without GC — now reinforced by
government policy steering new systems work to memory-safe languages. This is Kirn's _longest-horizon_
differentiator, already a `PLAN.md` Phase 12 roadmap.

**Goal:** stage the borrow checker so memory-safety becomes opt-in and incrementally achievable
(adopts `PLAN.md` Phase 12; SYNTAX SP-12 type surface).

**Must Follow:** Stage the borrow checker behind `@checked` so existing GC code (32/32 corpus) never breaks — a superset safety mode.
**Important Note:** Highest risk, do LAST; ownership/borrow is opt-in sugar inverting Rust's usual "unsafe opt-in".

> Rejected: rust — borrow checking is mandatory in all code; Kirn stages it behind `@checked` for gradual migration.
> Reference: rust — ownership/move/borrow model (`rust/compiler/rustc_borrowck`).

```kn
@checked
fn take(p: &Point) -> &Point { return p; }

fn main() -> none {
    var p = Point { x: 1, y: 2 };
    var q = take(&p);
    print(Str(q.x));
}
```

**Capability (staged)**

- Ownership/borrow annotations on references (`&`, `&mut`) as opt-in sugar, checked by a staged
  borrow checker.
- Mutable-borrow exclusion, moves vs copies, lifetime inference for common cases.
- Graduate from "annotation-free, GC-managed" (today) to "borrow-checked fast paths" (AOT).

**Design notes / rationale**

- Stage it behind a `@checked`/unsafe boundary so existing GC code (32/32 corpus) never breaks —
  this is a _superset_ safety mode, matching Rust's "safe by default, unsafe opt-in" inverted for
  gradual migration.
- Highest risk, highest reward; do LAST, after AOT/FFI prove performance demand.

**Exit criteria:** a `@checked` module that copies/moves/borrows with enforced exclusivity
compiles and runs; a double-borrow error fires; non-`@checked` code is untouched.

---

## Phase WHY-11 — Fast-AOT Benchmarking ("fast like C/C++")

**Why:** C/C++ dominate wherever performance is the deciding adoption factor (embedded, kernels,
engines). Go built `go bench`; Rust built criterion. Kirn already has `kirn build -O` (native
obj+exe via the prebuilt MSVC runtime, `tools/kirn.cpp:2517-2560`) and `.cob`/cross-build.

**Goal:** adopt `PLAN.md` Phase 8 (`kirn bench` + fast-AOT) so "how fast is Kirn" is _measurable
and improvable_, and publish benchmark numbers as the adoption proof.

**Must Follow:** Prove speed with `kirn bench` (Kirn-AOT vs interpreted vs C/Rust baseline) before any "faster than" claim ships.
**Important Note:** Claims stay measurable or reference-citable (principle 6); `@inline`/`@pure` must show a real AOT gain.

> Reference: go — `go bench`/testing benchmarks (`go/src/cmd/go`); rust — criterion (`rust/library`).

```kn
fn fib(n: Int) -> Int {
    if n < 2 { return n; }
    return fib(n - 1) + fib(n - 2);
}

fn main() -> none { print(Str(fib(30))); }
```

**Capability**

- `kirn bench` micro/macro harness; compare Kirn-AOT vs interpreted vs a C/Rust baseline.
- `-O` AOT optimization (constant folding, inlining `@inline`, `@pure` folding from SP-1).
- Establish a `benchmarks/` suite in-repo, tracked over time.

**Design notes / rationale**

- Perf is a _moat only if proven_; benchmarks are the "why C/C++ shops would try Kirn" evidence.
- Ties `@inline`/`@pure` (SP-1) to real AOT gains; zero-cost promise (principle 5) is demonstrated.

**Exit criteria:** `kirn bench` runs and prints Kirn-AOT vs baseline; AOT beats interpretation;
corpus green.

---

## Phase WHY-12 — Low-Level Surface (`sizeof/alignof/@repr(C)`)

**Why:** C/C++ win on _control_: precise layout, alignment, pointers, direct memory. Kirn today has
`ptr`/`ref`, `mem`, `unsafe` — the cornerstones.

**Goal:** round out the low-level surface (adopts `SYNTAX_PLAN.md` SP-14) so C-like control is
available when needed.

**Must Follow:** `sizeof`/`alignof`/`offsetof` are compile-time constants and `@repr(C)`/`@packed`/`@align(N)` are layout annotations — no runtime cost.
**Important Note:** Purely additive control surface for the "shave bytes/latency" developer; GC semantics stay unchanged.

> Reference: zig — `@sizeOf`/`@alignOf`/`extern struct` (`zig/src`); llvm — C ABI/layout (`llvm/clang`).

```kn
@repr(C)
struct Header { var tag: UInt; var len: UInt; }

fn main() -> none { print(Str(sizeof(Header)), Str(alignof(Header))); }
```

**Capability**

- `sizeof(T)`, `alignof(T)`, `offsetof(Field)` — compile-time constants.
- `@repr(C)` / `@packed` / `@align(N)` struct layout annotations → direct FFI/ABI mapping.
- Explicit pointer arithmetic in `unsafe`; raw memory ops via `mem`.

**Design notes / rationale**

- These are the C-compilers' defining ergonomics; even if Kirn's GC choices differ, providing the
  _control_ surface wins the "I need to shave bytes/latency" developer.
- Purely additive compile-time constants + layout annotations → no runtime cost.

**Exit criteria:** `examples/45_lowlevel.kn` prints `sizeof`/`alignof`, uses `@repr(C)` struct
passed to a native call; corpus green.

---

## Phase WHY-13 — FFI / Interop with C (and the C ecosystem)

**Why:** C's longevity = its FFI reach (everything exposes a C ABI). Rust Go/C# all win adopters by
interoperating with the C ecosystem.

**Goal:** adopt `SYNTAX_PLAN.md` SP-14 FFI ergonomics: a clean `extern "C"` declaration surface +
`kirn build` link against native libs.

**Must Follow:** `extern "C" fn` bindings plus `@repr(C)` layout plus a manifest link line; reuse the existing AOT linker harness.
**Important Note:** FFI is "why you can ship real software in Kirn today" — reuse the entire C ecosystem rather than rewriting it.

> Reference: go — cgo bindings (`go/src/cmd/cgo`); rust — `extern "C"` FFI (`rust/library/std/src/ffi`).

```kn
extern "C" fn puts(s: Str) -> Int;

fn main() -> none { _ = puts("hello from C"); }
```

**Capability**

- `extern "C" fn` declarations binding `.lib`/`.so`/`.dll`; struct layout via `@repr(C)` (WHY-12).
- Call native functions with automatic marshaling for primitives/strings/buffers.
- Link line in manifest/kirn.toml; `kirn build` emits the link command.

**Design notes / rationale**

- FFI is _"why you can ship real software in Kirn today"_: reuse the entire C ecosystem without
  rewriting. The AOT backend (`buildProgram`, cross-build `tools/kirn.cpp:2451-2515`) sets up the
  linker harness already.
- Uses `ptr`/`ref` and `mem` (existing) as the marshaling substrate.

**Exit criteria:** `examples/46_ffi.kn` declares and calls a small C function (e.g. `sqrt`/`puts`)
from a linked lib; `kirn build` produces a working exe; tests green.

---

## Phase WHY-14 — Safety Tooling & Audit (Go vet / Rust clippy / C sanitizers)

**Why:** beyond correctness, mature languages ship _safety tooling_: `go vet`, `clippy`, `-fsanitize`,
`cargo audit`. This gives adopters confidence and is a low-cost, high-trust feature.

**Goal:** fold safety findings into `kirn check`/lint (WHY-4) and `kirn bench`, adopting the
Phase-1/2 lint infra fully.

**Must Follow:** Fold safety findings into the existing Phase-2 `LintConfig`/diagnostics; `kirn vet` is an alias of `check --safety`.
**Important Note:** Trust is the cheapest adoption currency — safety lints are wiring, not new architecture.

> Reference: go — `go vet` (`go/src/cmd/vet`); rust — clippy (`rust/compiler`).

```kn
fn main() -> none {
    var x: Int? = none;
    print(Str(x));   # W-numbered optional-unwrap finding, suppressible with @allow
}
```

**Capability**

- Lint groups: uninitialized/undefined reads, potential nil/`T?` unwraps, index-out-of-range
  heuristics, `panic` paths, deadlock (WHY-8), integer-overflow config.
- `kirn vet` alias running the conservative safety subset; `@warn/@allow/@deny` toggles (SP-1).
- Audit command for installed deps (CVE-ish / version health; small scope, registry-driven).

**Design notes / rationale**

- Trust is currency in adoption; safety lints are the cheapest trust-builder per hour.
- Reuses the completed Phase-2 `LintConfig`/diagnostics — wiring, not new architecture.

**Exit criteria:** a deliberately buggy `examples/negative/` file yields grouped safety findings;
`@allow` suppresses a chosen one; corpus green.

---

## Phase WHY-15 — Consolidation: Docs, Format, Editions, Registry (the "readable & lasting" why)

**Why:** every surviving language (Python/Go/Rust/C++) has canonical docs, a formatter, an edition/
compat story, and a registry. These are the _institutional_ moat beyond any single feature.

**Goal:** cross-cut all phases with: canonical `kirn fmt` output, first-class docs
(`kirn doc` + `##` comments, have), edition/back-compat mechanics (SYNTAX_PLAN SP-17), and the
registry from WHY-5.

**Must Follow:** `kirn fmt` is idempotent/canonical, editions gate new soft keywords without breaking old code, and docs+registry aggregate the three plans.
**Important Note:** This is the institutional moat — a language you can read, keep formatted, keep compatible, and find packages in.

> Reference: go — gofmt canonical formatting (`go/src/cmd/gofmt`); rust — edition mechanics (`rust/compiler`).

```kn
# canonical form: one true layout, verified in CI with `kirn fmt --check`
fn main() -> none {
    print("stable");
}
```

**Capability**

- `kirn fmt` idempotent canonical form; CI-friendly `--check`.
- Edition mechanics so soft keywords/features gate on a "2026/2027…" edition without breaking old
  code (frozen keyword set already documented in `grammar/kirn.ebnf §1`).
- Consolidated docs site aggregating PLAN/SYNTAX_PLAN/WHY_PLAN + API ref + tutorial examples.
- Registry promotion (from WHY-5) as the "Python/PyPI" destination.

**Design notes / rationale**

- This is the unifying "why Kirn lasts": a language you can _read_, _keep formatted_, _keep
  compatible_, and _find packages in_.
- Pairs WHY-4 tooling with WHY-5 ecosystem; closes the loop.

**Exit criteria:** `kirn fmt --check` is green on all examples; a 2026-edition project and a
legacy project both build; docs site renders all three plans + API + examples; corpus + registry
green.

---

## Roadmap Summary (suggested solo-dev order, value-first)

**Goal:** Give a solo dev a value-first execution order — cheap wins before the hardest moats.
**Must Follow:** Treat this chart as a convenience duplicate of §3; §3 stays canonical and this copy is not edited independently.
**Important Note:** Order is sequencing, not scope — every entry still needs a reference citation and a `kirn bench` number.

> **Dedup note:** this ordering chart duplicates the "§3. Roadmap summary" chart near the top of
> the file (lines ~72-94). Keep the §3 chart as canonical; this second copy is retained for
> convenience but is **not** a separate plan — do not edit it independently.

```
WHY-1  batteries-included builtins        (Python productivity, cheap)   ─┐
WHY-4  toolchain completion fmt/repl/check (Go DX, compounds everything)  │  cheap wins first
WHY-3  for-over-anything                   (Go readability)               │
WHY-6  collection ergonomics               (readability)                  ┘
WHY-2  any/dynamic + comprehension         (Python/TS dynamic)            ─┐  power layer
WHY-7  stdlib breadth + registry           (Python batteries/ecosystem)   │
WHY-8  concurrency ergonomics              (Go, already have machinery)   │
WHY-9  pattern & trait power-up            (Rust parity)                  ┘
WHY-14 safety tooling / vet               (trust, cheap after lint)       ─┐
WHY-11 fast-AOT + bench                    (C/C++ perf proof)             │
WHY-12 low-level surface                   (C control)                    │
WHY-13 FFI/interop                         (C ecosystem reach)            │  hardest moats
WHY-10 borrow-checker staging              (Rust safety, PLAN Phase 12)   |  (after AOT/FFI)
WHY-15 consolidation (fmt/docs/editions/registry)                         ┴
```

**Short version for a single dev:** 1 → 4 → 3 → 6 → 2 → 7 → 8 → 5 → 9 → 14 → 11 → 12 → 13 →
10 → 15.

**Cross-links:** WHY-1→SYNTAX SP-8; WHY-2→SP-5/SP-9; WHY-3→SP-10; WHY-4→PLAN P9/P8; WHY-5→PLAN
P3/P15; WHY-7→PLAN P7; WHY-8→PLAN P14; WHY-9→SP-13/PLAN P6; WHY-10→PLAN P12; WHY-11→PLAN P8/P13;
WHY-12/13→SP-14; WHY-15→SP-17/PLAN P9.

---

## Design decisions that need ratification (before coding)

**Goal:** Ratify E1–E8 before coding so each decision's reference and evidence are explicit.
**Must Follow:** Every row names options, a recommendation, and the anchor phase that proves it; a ratified recommendation supersedes prose elsewhere.
**Important Note:** Record any change to a ratification here first — the ledger stays authoritative.

| #   | Decision             | Options                                             | Recommend                                    |
| --- | -------------------- | --------------------------------------------------- | -------------------------------------------- |
| E1  | `Str()` builtin      | add `Str(x)` / keep absent                          | add `Str(x)` (mirrors SYNTAX SP-8 D10)       |
| E2  | Dynamic type toggle  | always-on `any` / `--strict` flag / `@checked` attr | `any` keyword + `@checked` ratchet (WHY-2)   |
| E3  | Registry scope       | local mirror / public kirn-libs page / both         | local-first, then public (WHY-5)             |
| E4  | Stdlib impl language | `.kn` on runtime / native C++ / mixed               | `.kn` via `pin.kn` where possible (WHY-7)    |
| E5  | Borrow-checker entry | `@checked` module attr / separate edition           | `@checked` opt-in attr (WHY-10)              |
| E6  | FFI declaration      | `extern "C" fn` / `@link` attr / both               | both (WHY-13)                                |
| E7  | Edition mechanics    | 2026/2027 edition gate / always-soft                | edition-gated for new soft keywords (WHY-15) |
| E8  | `kirn vet`           | alias of `check --safety` / new subcommand          | alias of `check --safety` (WHY-14)           |

---

## Appendix A — Evidence base (file:line)

**Goal:** Anchor every "why" claim to a real `file:line` so rationale is auditable rather than asserted.
**Must Follow:** Keep anchors current; a claim missing its anchor is flagged aspirational and tied to the phase that proves it.
**Important Note:** This is the evidence ledger that `kirn bench` and `kirn check` ultimately confirm.

- Current builtin set (thin, the "batteries" gap): `src/interp/runtime.cpp:485-613`
  (`print,len,sqrt,ord,chr,assert,assert_eq,range,panic,catch_panic,printf,strlen`; stub modules
  `math,time,io,mem,json,text,os`).
- Builtin registration point (for WHY-1/7): `installBuiltins()`, `src/interp/runtime.cpp:485`.
- Frozen keyword set (for WHY-15 editions): `grammar/kirn.ebnf:49-55`; `src/lex/lexer.cpp:38-50`.
- `any`-style poison markers (real `any` is additive): `src/sema/type.h:15-27` (`Error`/`Unknown`).
- Comprehension clauses already generalize (`CompClause`): `src/ast/ast.h:92-97`.
- Iteration entry point (WHY-3/6): `src/interp/runtime.cpp:2847` (`iterateSeq`), `runtime.h:137-139`.
- Method lookup (protocol for WHY-3): `src/interp/runtime.cpp:1895-1913`.
- Named args / splat hook (`CallArg`): `src/ast/ast.h:87-90`.
- CLI surface (WHY-4/5/14/15): `tools/kirn.cpp:2850-2886` (usage), dispatcher 2902-2989.
- `kirn doc` markdown+API viewer (WHY-5/15): `tools/kirn.cpp:1469-1721`.
- Cross/AOT build + `-O` native objects (WHY-11/13): `tools/kirn.cpp:2451-2560`
  (`buildProgram` 2319; MSVC native objects 2517-2560).
- Convention files `pin.kn`/`main.kn` (WHY-5/7): `PLAN.md` Phase 3 (COMPLETE); resolution in
  `tools/kirn.cpp:335-355`; scaffold `cnNew` `tools/kirn.cpp:542-681`.
- Lint infra (WHY-14): `PLAN.md` Phase 2 (COMPLETE), `LintConfig` in checker; SP-1
  `@warn/@allow/@deny` map to it.
- Concurrency machinery (WHY-8): `PLAN.md` Phase 14; `spawn/chan/select` in grammar/parser.
- Borrow-checker roadmap (WHY-10): `PLAN.md` Phase 12.
- Pattern gaps / reflection (WHY-9): `doc: docs/FEATURE_GAP_ANALYSIS.md` §3.2, §3.3.
- AOT/JIT + optimizer (WHY-11/12): `PLAN.md` Phase 8 (AOT), Phase 13 (JIT/optimizer).

## Appendix B — Research references (web, 2026)

**Goal:** Record the 2026 web research behind the four family "whys" so adoption claims are citable.
**Must Follow:** Tie each citation to its phase(s) and prefer primary sources over commentary.
**Important Note:** A marketing line must never outrun either this evidence or a `kirn bench` number.

- **Python — productivity & batteries-included (WHY-1/2/7):** Stack Overflow Developer Survey 2026 —
  Python ~38% usage vs JavaScript ~36%, most "wanted/used" for ML/AI; Python's stdlib + dynamic
  typing cited as top productivity drivers; PyPI ecosystem as the adoption flywheel.
- **Go — simplicity/concurrency/toolchain (WHY-3/4/5/8):** Go's selling points — fast builds,
  uniform toolchain (`go fmt/test/doc/vet/build/mod`), goroutines for cloud/concurrency,
  "one way to format = readable any codebase"; Go module proxy (`proxy.golang.org`) as the
  ecosystem/registry analogue.
- **Rust — memory safety/zero-cost (WHY-9/10):** US White House ONCD (2024) + DARPA/NSA guidance
  steering new systems software to memory-safe languages (Rust/C#/Go/Java/Swift); Rust's
  borrow-checker without GC, zero-cost abstractions, pattern/trait system; friction noted:
  borrow-checker learning curve + compile times.
- **C/C++ — performance/control/FFI (WHY-11/12/13):** C's embedded/kernel/engine dominance and the
  C ABI as universal interop; C/C++ ~23% combined share; `sizeof/alignof/@repr(C)` control surface
  and linkage as the reason systems teams rarely leave C.

---

_This is the third of three living plans. `PLAN.md` = the strategic/infrastructure spine;
`SYNTAX_PLAN.md` = the language-surface expansion; `WHY_PLAN.md` = the demand/adoption roadmap that
sequences the other two by "why". Ratify decisions E1–E8 before coding the first feature._
