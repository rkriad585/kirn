# Coco — Why Use Coco? (WHY_USE_COCO_PLAN)

**Status:** Strategic positioning + implementation roadmap · Living document · Version 1.0
**Date authored:** 2026-09-03 (landscape researched 2026; source audited 2026-09-03)
**Relationship to other plans:** `WHY_PLAN.md` is the *demand → capability* internal roadmap that
sequences features by adoption driver against Python/Go/Rust/C. This document is its **public,
strategic sibling**: it answers the outward-facing question *"Why should a developer choose Coco
instead of Python, Rust, Go, Ruby, C/C++, JavaScript/TypeScript, or Java?"* — grounding every answer
in **real developer pain points (2026, web-verified)** mapped onto **what Coco has implemented
today** (source-verified). It widens the field to Ruby and JS/TS (added), confronts the **AI-assisted
adoption barrier** that other plans do not address, and gives every phase the full
Goal / Problem / Why-it-matters / Design / Implementation / Files / Code / Tests / Outcome /
Risks structure.

> **How to read this.** §1 = the honest one-paragraph thesis. §2 = the landscape (why each language
> wins, its pain). §3 = a clean capabilities matrix (VERIFIED against source). §4 = the core
> "why Coco" argument, split into *real strengths today*, *honest gaps*, and *what Coco must promise
> but not break*. §5 = the central answer. §6+ = the multi-phase roadmap (Phases 1–18). §7 = risks &
> the AI-adoption reality. Appendices = evidence.

> **Scope rule.** This plan **does not re-specify** features already owned by `PLAN.md`,
> `SYNTAX_PLAN.md`, `WHY_PLAN.md`, `STD_LIBS_PLAN.md`, `DATA_TYPE_PLAN.md`, `EXP_PLAN.md`,
> `DO_FIRST_PLAN.md`, `MISSING_PLAN.md`, `NEED_REMOVE_PLAN.md`. When a phase here touches work owned
> elsewhere, I say "(owner: X) →" and keep only the *why* and the *positioning*, so the roadmap
> ecosystem stays non-contradictory.

---

## 1. The one-paragraph answer

**Coco is the batteries-included systems language that collapses the "two-language problem."**
Every serious stack today forces a choice between a *safe-and-fast* language (Rust/C++/Go — hard to
write, slow to iterate, thinnish batteries) and an *easy-to-write* language (Python/JS/Ruby — fast
to prototype, slow to run, unsafe at scale). Coco is engineered so a single language covers both
ends of that spectrum: it already has Python-grade builtins and comprehensions, Go-style
`spawn`/`chan`/`select` concurrency *built into the language*, Rust-style `match`/traits/generics/
exhaustiveness and a conservative borrow check, and a native AOT backend measured ~290× faster than
its own interpreter on recursion — all behind an English-first syntax that reads like prose and a
batteries-included stdlib + package manager + toolchain that ship with the language. The pitch is
not "another Rust" or "another Python"; it is: **write the whole product in one readable language,
prototype at Python speed, ship at C speed, and be memory-safe by default — without gluing three
stacks together.** That claim is only as strong as Coco's honesty about what it still lacks, which
§3–§5 state plainly alongside what is already true today.

---

## 2. The 2026 landscape: why each language wins, and where each one hurts

Web research (Appendices C–I) gives a consistent, cross-source picture. For each language family:
what developers actively choose it for (the "why"), and the recurring, counted pain points (the
"wedge" Coco could fill).

| Language | Why developers choose it (2026) | Recurring pain points developers report (2026) |
|---|---|---|
| **Python** | Productivity & DX; batteries-included stdlib; dynamic typing; comprehensions; dominance of ML/AI; largest ecosystem (~38% SO usage); AI tools generate the best Python | 10–100× slower than compiled on CPU-bound work; GIL serializes threads (free-threaded 3.14 is optional, ~10% slower, lib-incompatible); memory-hungry (a worker that was 180MB in Go → 1.1GB in Python); dynamic types surface errors at runtime (needs mypy); dependency hell / packaging friction; poor fit for mobile/games/real-time |
| **Rust** | Memory safety *without* GC (now mandated — CISA/NSA/EU CRA); zero-cost abstractions; fearless concurrency; best-in-class tooling; 45%+ enterprise production usage; WASM default | Steep learning curve — "2–3 months to productivity" vs "1–2 weeks" for Go; borrow-checker friction early; slow/complex compile times (a top cited problem, Rust's own 2025 survey); verbose ownership ceremony; no stable ABI for binary distribution; thinner ecosystem outside systems/networking/CLI; async is a separate, hard model |
| **Go** | Radical simplicity (25 keywords, spec readable in an afternoon); near-instant builds; goroutines/channels as *language* primitives; one-toolchain batteries (`go fmt/test/doc/vet/build`); tiny fast-starting static binaries; "read any codebase" | Verbose `if err != nil` error handling (28% want better); GC latency is unpredictable (bad for HFT/real-time); nil-interface gotcha; no `++`, no sum types/`Option`/`Result` (state-machine bugs easy); limited metaprogramming; large binaries; 37% still stuck on vendoring |
| **Ruby** | Readability + developer happiness; Rails productivity (scaffold a full CRUD app <1h); convention-over-configuration; strong test culture; small-team shipping speed; Shopify/GitHub track record | Slimmer talent pool (~6.4% SO); slower on CPU-bound work; heavy memory; concurrency weak (Puma threading) vs Go/Rust; "feels fast until it doesn't" — perf issues hidden until scale; Rails-only relevance |
| **C/C++** | Raw performance & control; embedded/kernels/engines still need it; universal C ABI for interop; 72% of embedded teams | The **memory-safety crisis**: ~70% of critical CVEs in C/C++ are memory-safety bugs; Microsoft "remove C/C++ by 2030", Google mandate, Linux migration; cryptic template errors; high complexity after decades of features; no safety without 3rd-party sanitizers/profiles |
| **JavaScript/TypeScript** | Universal runtime (every browser/Node/Deno/Bun); JS still ~66%, TS the github #1 with ~44% usage; huge npm ecosystem; TS adds compile-time types + zero runtime overhead; native TS in Bun/Deno | TS types are **erased** — no runtime enforcement (validation needs Zod); npm supply-chain attacks (796-pack compromise, Oct 2025); dependency bloat / node_modules; toolchain fragmentation; `tsconfig` complexity; event-loop concurrency limits CPU-bound work; "type theater" with `any` everywhere |
| **Java** | JVM portability (one artifact, three OSes); JIT peak throughput rivaling C++; backward-compat guarantee; Maven Central (10M+ artifacts); virtual threads (Project Loom) fixed the thread pain; LTS cadence | Verbosity (though records/sealed/patterns helped); JVM cold-start 3–6s + pile of RAM (350MB RSS disqualifies serverless); JIT warm-up 30–60s; memory heap vs Go/Rust; heavy operational overhead |

**Cross-cutting 2026 shifts (from the research):**
1. **Government/market memory-safety pressure** (US CISA/ONCD, EU Cyber Resilience Act, NSA) is
   pushing new *systems* code to memory-safe languages. Coco's conservative borrow check + rust-like
   exhaustiveness is directly on-trend.
2. **The AI coding-assistant advantage** now decides language adoption (JetBrains migration data:
   JS for jobs, Kotlin for DX, Go for performance, Python for ecosystem). A language with a small
   corpus gets *hallucinated APIs and wrong idioms* from AI tools — an "AI gap" that freezes many
   new languages (§7 addresses this head-on).
3. **Nobody wants the two-language stack.** The production evidence (Python-rewrites-to-Rust/Go,
   "polyglot strategy", package-vs-perf bifurcation) shows teams spend enormous real cost gluing a
   fast language to an easy language. This is Coco's opening.

---

## 3. Capability matrix: what Coco has TODAY (source-verified)

From the audit of `src/` (~14,800 LOC C++20), `stdlib/`, `tools/`, `examples/` (48 files), `tests/`
(31 files), `grammar/coco.ebnf` (557 lines), and the CI (`scripts/*.ps1`). ✅ = implemented.

| Capability | Coco | Notes (evidence) |
|---|---|---|
| Python-grade builtins | ✅ (mostly) | `map/filter/reduce/any/all/sum/min/max/sorted/enumerate/str/int/float/bool/type/upper/lower/...` live in `installBuiltins()` (`src/interp/runtime.cpp:485-613`); residual `zip/flatten/round/floor/ceil/input` gaps |
| Comprehensions | ✅ | list comps + multi-`for`/`if` clauses (`CompClause`, `src/ast/ast.h:92-97`) |
| Dynamic/gradual typing | ✅ `any`/`dynamic` | both → `unkTy()` (`src/sema/checker.cpp:868`) |
| Go-style concurrency | ✅ | `spawn`/`chan`/`select`, real OS threads (`src/interp/runtime.cpp`), examples 22/23/30 |
| `defer`, `panic`, recover | ✅ | examples 20 |
| Rust-style `match` + patterns | ✅ strong | or/alias/slice/rest/range/guard patterns; enum exhaustiveness enforced (examples 12, 31, 38; `tests/types/n0*.co`) |
| Traits + static & dynamic dispatch | ✅ | `impl Trait for X`, trait objects, default bodies (example 13; `src/sema/checker.cpp`) |
| Generics with bounds | ✅ | `[T is Bound]` (example 14) |
| `Option`/`Result` + `?` propagation | ✅ | `T?`/`none`, `result[T,E]`, `try`/`?` (examples 18, 19) |
| Error handling — exceptions | ✅ | `try`/`raise`/`catch`, `catch_panic` (examples 35, 20) |
| OOP facade | ✅ | `class`/`interface`/`record` desugar to `struct`/`trait` (example 36) |
| Memory management | ✅ variant | value semantics + ARC `new`/`box`, `weak` refs, arena `mem.Arena()`, `unsafe`, `ptr`/`ref` (examples 27–29) |
| Conservative borrow check | ✅ (staged) | `src/sema/borrow.cpp` escape analysis; negative tests `n06`, `types/n01` |
| Native AOT backend | ✅ (scalar) | `src/backend/native.cpp`; `coco build -O`; fib(30) ≈ **290×** faster than tree-walker (≈59ms vs ≈17.3s); cross-compile arm64/linux |
| Bytecode VM | ✅ | default runner, ~2.7–4× faster than tree-walker (`scripts/bench.ps1`) |
| Batteries stdlib (written in Coco) | ✅ (10 modules) | `core, collections, io, json, math, os, path, regexp, strings, time` in `stdlib/lib/`, each with `*_test.co` |
| Package manager / registry / lockfile | ✅ | `coco new/run/test/install/add/update/remove/clone/list/doc/build/targets`; `coco.toml`, `coco.lock` (`tools/coco.cpp`) |
| Doc generator / conventions | ✅ | `coco doc`; `main.co`/`pin.co` run-once conventions |
| Diagnostics with spans/caret/fix-its/lints | ✅ | `src/support/diag.h` (245 lines), W0101–W0107 lints |
| Differential correctness harness | ✅ | `vm_diff.ps1` (tree-walker ≡ VM byte-identical) |

**Honest gaps (do NOT overclaim in front of a developer):**
- Native AOT is **scalar-only** today; collections/structs/concurrency still run through VM/interp
  (internal fast-path tiering: native → VM → tree-walker).
- **No JIT yet** — the README claims JIT and "outperforms C/C++"; those are **aspirational, not
  measured** (§6 Phases 6–8 must close this honesty gap with real, comparable benchmarks).
- No `coco fmt`, no `coco repl`, no LSP server, no debugger yet.
- stdlib lacks networking, crypto, and a full regex engine (`regexp` is a glob-subset).
- No established ecosystem/registry population; no third-party packages.
- No WASM/embedded targets; no `@repr(C)`/`sizeof`/`alignof` yet.

---

## 4. The core "why Coco" argument (three honest claims)

### Claim A — "One readable language, both ends of the speed/safety spectrum"
*BEHIND:* the two-language-stack cost is real and measured (Python→Rust/Go rewrites, polyglot
strategies). Coco intends the Python-write speed *and* the C-ship speed *and* Rust-style safety in
one syntax you can read in one sitting. What is true today: builtins+comprehensions (`examples/34`),
`spawn/chan/select` as language primitives (no async-framework split like Rust/JS), `match`/
traits/generics (examples 12–15), a conservative borrow check (`src/sema/borrow.cpp`), and a native
backend (≈290×). What must still be true to keep the claim: Phases 5–8 (real benchmarks, broader
native coverage, honest docs).

### Claim B — "Batteries by default, not by agonizing over tooling"
*BEHIND:* Python wins on stdlib, Go on one-toolchain batteries, Rust on cargo. Each wins partly on
tooling. Coco ships the toolchain with the language: package manager with manifest/lockfile/registry,
doc generator, test runner (`coco test`), native + `.cob` bundles + cross-build, all from one binary.
What is true today: `tools/coco.cpp` implements all of it; `stdlib/lib/*_test.co` dogfoods the test
runner. What must be true to keep it: Phases 1–4 (fmt/repl/check/LSP) and Phases 12–13 (registry +
networking/crypto stdlib).

### Claim C — "Safety and sanity without the steep cliff"
*BEHIND:* Rust's #1 objection is the learning curve (2–3 months). Coco's pitch is *gradual*
safety: you can start with `any`/dynamic code and ratchet to checked/borrow-checked regimes. What is
true today: `any`/`dynamic` escape hatch, `unsafe` blocks, a conservative (not pedantic) borrow pass.
What must be true to keep it: Phases 9–11 (opt-in `@checked`/`--strict` ratchet, incremental
borrow-check hardening, safety lints + `coco vet`).

---

## 5. The central answer (direct)

> **Why choose Coco instead of Python, Rust, Go, Ruby, C/C++, JS/TS, or Java?**
> Because the mainstream answer to every one of those languages is a *compromise you don't want to
> make*, and Coco is designed to remove it:
> - instead of Python's *speed*, Coco compiles (AOT/VM) and has native concurrency;
> - instead of Rust's *learning cliff and compile times*, Coco is readable, batteries-included, and
>   makes safety **optional-to-strict** rather than all-or-nothing;
> - instead of Go's *verbose errors and GC latency*, Coco has `result[T,E]`, `?`, `match`, and
>   reference-counted/arena memory you can tune;
> - instead of C/C++'s *memory-safety crisis*, Coco is safe by default with an `unsafe` escape;
> - instead of Ruby's *performance and concurrency ceiling*, Coco ships green threads built-in;
> - instead of JS/TS's *erased types and npm supply chain* risk, Coco has real runtime types, safe
>   checked indexing, a lockfile, and fewer attack-surface dependencies;
> - instead of Java's *cold-start/memory/verbosity*, Coco starts instantly, is lightweight, and is
>   concise.
> **Choose Coco when your project needs several of these at once** (a product, a service, a tool,
> a systems component) **and you'd rather write it once, in one readable language, than glue a fast
> language to an easy language.** Choose it *now* for its strengths (readability, concurrency,
> batteries, checking, native scalar perf) and help it converge on the rest.

---

## 6. The multi-phase roadmap

Universal per-phase discipline (applies to every phase below):
- **Never break the corpus.** Every commit keeps `examples/`, `tests/`, `stdlib/`, `tools/` green on
  all three backends (`tree-walker ≡ VM ≡ native` via `scripts/vm_diff.ps1`).
- **Honesty gate.** No marketing claim ships before a measured benchmark or a working feature.
- **Owner handoff.** Where work is already owned by another plan, this plan links to it and only adds
  the *positioning/why* and any gap that plan misses.

---

### Phase 1 — `coco fmt`: canonical, idempotent formatter
- **Goal:** a `gofmt`-class formatter; `coco fmt` normalizes any source, `coco fmt --check` for CI.
- **Problem being solved:** Go's #1 readability moat is "one way to format a codebase". Without it,
  Coco codebases (and AI-generated Coco) diverge, and `readability` (Coco's core pitch) erodes.
- **Why it matters to developers:** "Can I read any Coco codebase in one sitting?" is a yes only if
  formatting is canonical. It compounds every other phase and makes AI codegen reproduce idiomatic Coco.
- **Coco design/feature proposal:** a lossless, idempotent pretty-printer driven by the existing AST
  (`src/ast/`, `ast_dump.cpp:653`), with stable rules mirroring `grammar/coco.ebnf` §1 style. Preserve
  comments (the AST currently may drop some — fix to round-trip `#` comments).
- **Implementation approach:** new subcommand in `tools/coco.cpp` dispatch (`runProgramSrc` at :489,
  `buildProgram` at :2319 are ready to be reused). Reuse `cocoparse`'s AST (`tools/cocoparse.cpp`)
  as the input; emit canonical text; verify idempotency (fmt(fmt(x)) == fmt(x)).
- **Relevant source files/components:** `tools/coco.cpp`, `tools/cocoparse.cpp`, `src/ast/`,
  `src/parser/parser.cpp`, `grammar/coco.ebnf`.
- **Code/syntax examples:**
  ```coco
  # before:  fn   add(a:int,b:int)->int{return a+b;}
  # after:   fn add(a: int, b: int) -> int { return a + b; }
  ```
- **Testing requirements:** run `coco fmt` over all 48 `examples/*.co` + `stdlib/`;
  assert byte-identical on second pass; `--check` fails on any messy input; corpus still runs after
  formatting (spacing-only changes).
- **Expected outcome:** canonical bytes everywhere; CI enforces formatting; AI tools get a
  reproducible output shape.
- **Potential risks / trade-offs:** touching formatting of all files risks large diffs; must be
  spacing-only so behavior is provably unchanged (verify via `vm_diff`). Owner: `WHY_PLAN.md` WHY-4 →
  implement here as the positioning-critical first tool.

---

### Phase 2 — `coco check` / `coco lint`: fast feedback + safety lints
- **Goal:** a `go vet`/`clippy`-class `coco check` (type + lint) and `coco lint` (W-numbered
  findings).
- **Problem being solved:** safety and correctness tooling is a leading adoption trust-builder (Go
  shipped it; Rust ships clippy; C needs sanitizers). Coco already has W0101–W0107 lints and
  diagnostics (`src/support/diag.h`) but no first-class `check`/`lint` entry point.
- **Why it matters:** trust is currency in 2026; organizations adopting a new language need
  "`coco check` passes" as a gatable gate before considering it.
- **Coco design:** expose the completed `checker.cpp` analysis as `coco check <file>` (type errors →
  exit 1) and `coco lint` (lint findings grouped by W-code, `@allow`/`@deny` toggles). Extend the
  lint set later (Phases 9–11).
- **Implementation approach:** add subcommands to `tools/coco.cpp`; reuse `LintConfig`
  checker path. `coco vet` can be an alias of `check --safety` (decision E8 in WHY_PLAN).
- **Relevant files:** `tools/coco.cpp`, `src/sema/checker.cpp`, `src/sema/borrow.cpp`,
  `src/support/diag.h`.
- **Code/syntax examples:**
  ```coco
  $ coco check app.co          # type errors reported with spans; exit 1 on failure
  $ coco lint app.co           # W0101 (unused), W0103 (immutable reassign hint), ...
  $ coco vet app.co            # safety subset: nil-unwrap, index-range, borrow-escape heuristics
  ```
- **Testing:** extend `tests/negative/` (n01–n18) + `types/` to be driven through `coco check`;
  add `tests/lint/` with one file per W-code.
- **Expected outcome:** a gatable, scriptable static-analysis command — the trust floor.
- **Risks:** lint false-positives annoy users; keep W-codes conservative and `@allow`-able.
  Owner: `WHY_PLAN.md` WHY-4/WHY-14.

---

### Phase 3 — `coco repl`: interactive loop for the dynamic surface
- **Goal:** a REPL that evaluates expressions/statements, aligned with `any`/dynamic (Phase) and
  `fmt` round-tripping.
- **Problem being solved:** Python/JS/Ruby win beginners and explorers with a live prompt; a new
  language without a REPL raises the "try it" barrier.
- **Why it matters:** lowest-friction on-ramp; also useful for teaching and debugging the dynamic
  surface.
- **Coco design:** read lines → parse via existing parser → check → eval via tree-walker
  (`src/interp/runtime.cpp`). Support multi-line continuation for blocks/`fn` closures; print result
  unless `none`; keep history.
- **Implementation approach:** new `tools/cocorepl.cpp` (or a `coco repl` mode); reuse `cocorun.cpp`
  pipeline (`tools/cocorun.cpp`).
- **Relevant files:** `tools/cocorun.cpp`, `src/interp/runtime.cpp`, `src/parser/parser.cpp`.
- **Code/syntax examples:**
  ```
  coco> 1 + 2
  3
  coco> xs = [1,2,3]; sum(x for x in xs if x > 1)
  5
  coco> spawn (() => print("hello"))  # dynamic snippet works live
  ```
- **Testing:** scripted REPL sessions with expected output in `tests/repl/`.
- **Expected outcome:** an interactive on-ramp and teaching tool.
- **Risks:** infinite loop / hang in REPL; add `Ctrl-C` handling. Owner: `WHY_PLAN.md` WHY-4.

---

### Phase 4 — LSP server (`coco lsp`) for editor integration
- **Goal:** a Language Server Protocol implementation so VSCode/Neovim/etc. get diagnostics,
  hover/doc, go-to-definition, rename.
- **Problem being solved:** "No IDE support" is a classic new-language blocker; TS's editorial
  advantage (IntelliSense, `$25 per type error caught` at compile time) is huge. Rust and every
  serious 2026 language ship an LSP.
- **Why it matters:** DX compounds; an LSP turns every lint/type feature into continuous, in-editor
  feedback and makes AI assistants more accurate (they read diagnostics).
- **Coco design:** a stdio JSON-RPC server opening a file → `cococheck` diagnostics as LSP
  `textDocument/publishDiagnostics`; hover via `coco doc`-ready symbol info; go-to-def/rename via a
  symbol table added to the checker.
- **Implementation approach:** new `tools/cocolsp.cpp`; minimal JSON-RPC over stdin/stdout (reuse
  `util/tomlmini.h`-style lightweight parsing or a tiny JSON writer); hook `cococheck` (existing) for
  diagnostics first.
- **Relevant files:** `tools/cococheck.cpp`, `src/sema/checker.cpp` (add a symbol/index pass),
  `tools/coco.cpp` (doc), `src/ast/`.
- **Code/syntax examples:** (protocol, no Coco syntax) — `initialize` → `didOpen` → diagnostic
  notifications; hover returns a doc comment.
- **Testing:** LSP integration tests using a headless LSP client harness in `tests/lsp/`.
- **Expected outcome:** a working diagnostics-first LSP; foundation for hover/goto/rename.
- **Risks:** LSP is broad; scope v1 to diagnostics + hover only. Owner: `WHY_PLAN.md` WHY-4/WHY-15.

---

### Phase 5 — `coco bench`: honest, reproducible benchmarks (close the README honesty gap)
- **Goal:** a canonical in-repo benchmark suite that reports Coco (tree-walker / VM / native AOT) vs
  a comparable baseline (C++, Rust, Go, Python where sensible), with tracked history.
- **Problem being solved:** the README's "JIT / outperforms C/C++" is unmeasured; a serious adopter
  will benchmark us and find a mismatch. Perf is a moat *only if proven*.
- **Why it matters:** this is the direct answer to "why not just C/Rust/Go for the fast part?" — real
  numbers, not marketing.
- **Coco design:** `coco bench` micro+macro harness; cases: fib, loops, matrix, string processing,
  dict/set ops, sort, JSON round-trip, a small concurrent workload. Nail the honest tiering:
  scalar-native ≈ C-speed, VM ~3× interp, interp slower but runs everything.
- **Implementation approach:** populate the empty `benchmarks/` dir; wire `scripts/bench.ps1` into
  `coco bench`; add `--release` + `--native` flags; output JSON + markdown table.
- **Relevant files:** `benchmarks/`, `scripts/bench.ps1`, `tools/coco.cpp` (build/run), `src/backend/
  native.cpp`.
- **Code/syntax examples:**
  ```
  $ coco bench --native fib
  coco tree-walker  fib(30): 17.31 s
  coco VM           fib(30):  ~4.4 s
  coco AOT(msvc)    fib(30):  0.059 s   (≈290x vs interp)
  ```
- **Testing:** benchmark outputs are deterministic within a tolerance; a CI job runs a smoke subset.
- **Expected outcome:** published, honest numbers replacing aspirational claims; a tracked
  performance budget.
- **Risks:** benchmarks invite unfair comparison; pick apples-to-apples cases and publish harnesses.
  Owner: `WHY_PLAN.md` WHY-11.

---

### Phase 6 — Broaden the native AOT backend (beyond scalars)
- **Goal:** mode-migrate the most-used non-scalar constructs onto native: simple structs, list/dict
  operations on hot paths, and selected builtin calls.
- **Problem being solved:** today AOT only lowers scalars; the "fast part" is too narrow for real
  apps, so the honest performance claim is limited.
- **Why it matters:** the C-speed claim must cover the code users actually write hot, not just fib.
- **Coco design:** extend `src/backend/native.cpp` with struct-layout/allocation lowering and simple
  list/dict loops; keep the fallback-to-VM tier so correctness never depends on the boundary.
- **Implementation approach:** start with stack/struct value lowering and straight-line list indexing
  loops; measure with Phase 5; expand incrementally. Guard with differential tests (native ≡ VM).
- **Relevant files:** `src/backend/native.cpp`, `src/vm/compiler.cpp`, `src/interp/runtime.cpp`.
- **Code/syntax examples:** a `sum`-over-list and a struct-field loop compiled to native while still
  matching the VM's output byte-for-byte.
- **Testing:** extend `vm_diff`/`native_diff` to cover struct/list workloads; `scripts/runall.ps1` +
  native build on all examples.
- **Expected outcome:** native covers common hot patterns; the "fast" claim is credible.
- **Risks:** struct/GC interop with native is the hardest part; keep native conservative and always
  fall back safely. Owner: `PLAN.md` P8, `FEATURE_GAP_ANALYSIS.md` §3.4.

---

### Phase 7 — A real JIT tier (research-gated)
- **Goal:** a JIT for the VM's hottest functions (not just static AOT of scalars), delivering native
  speed without a separate build/`--native` step.
- **Problem being solved:** the README promises JIT. Deliver a measured, scoped one or publicly
  re-scope the claim.
- **Why it matters:** "it just runs fast" (Python 3.13/3.14 added copy-and-patch JIT; Java JIT) — the
  no-friction path to C-class speed.
- **Coco design:** start from the existing native lowering (`native.cpp`) reused via a tiny JIT:
  compile a function body to native at first hot threshold (straight-line, then loops). This is the
  highest-risk phase.
- **Implementation approach:** prototype producing machine code from the existing native codegen for
  hot scalar functions, called by the VM; measure against AOT. Gate behind `-Ojit`; keep AOT path
  canonical for now.
- **Relevant files:** `src/vm/compiler.cpp`, `src/backend/native.cpp`, `src/interp/runtime.cpp`,
  `src/vm/bytecode.h`.
- **Code/syntax examples:** `coco run -Ojit fib.co` gets within ~2× of AOT without a separate build.
- **Testing:** differential (JIT ≡ VM ≡ tree-walker) + Phase 5 bench.
- **Expected outcome:** a measured JIT tier or an explicit, documented decision to push users to AOT
  instead (removing the overstated claim).
- **Risks:** JIT is a large, ongoing effort (GC/stack maps, deopt); budget accordingly and be
  transparent. Owner: `PLAN.md` P13 (JIT/optimizer).

---

### Phase 8 — `sizeof`/`alignof`/`@repr(C)` low-level surface (C control when needed)
- **Goal:** compile-time layout constants and layout annotations for direct FFI/ABI mapping.
- **Problem being solved:** C's control surface is why systems teams don't leave C. Providing
  `sizeof`/`alignof`/`@repr(C)` wins the "I need to shave bytes/latency" developer.
- **Why it matters:** paired with FFI (Phase 10), it makes "write real systems software in Coco"
  credible.
- **Coco design:** `sizeof(T)`, `alignof(T)` as compile-time constants; `@repr(C)`/`@packed`/
  `@align(N)` struct layout annotations feeding the native backend.
- **Implementation approach:** extend the checker to compute layout (`src/sema/type.h` TyKind) and
  the native backend to honor annotations.
- **Relevant files:** `src/sema/checker.cpp`, `src/backend/native.cpp`, `grammar/coco.ebnf`.
- **Code/syntax examples:**
  ```coco
  @repr(C) struct Packet { var id: i32 = 0; var len: u16 = 0; }
  print(sizeof(Packet));   # 8
  print(alignof(Packet));  # 4
  ```
- **Testing:** `tests/lowlevel/` asserting exact sizes/alignments; native build passes the struct to
  a C function.
- **Expected outcome:** a credible low-level/FFI surface.
- **Risks:** layout rules are additive but must match the platform ABI precisely; keep hidden from
  GC semantics (value types only first). Owner: `WHY_PLAN.md` WHY-12, `SYNTAX_PLAN.md` SP-14.

---

### Phase 9 — Gradual safety ratchet: `@checked` / `--strict`
- **Goal:** opt-in strictness so a project can start dynamic and harden to checked/borrow-checked
  without rewriting.
- **Problem being solved:** Coco's pitch is "safety without Rust's cliff." The mechanism is a
  **ratchet**: `any`/dynamic now, `@checked` stricter, `--strict` strictest — TypeScript's path.
- **Why it matters:** lets Coco win "fast to write" (Python/JS crowd) *and* "safe to ship" — the
  gradual path Rust and TS proved.
- **Coco design:** `@checked` module/crate attribute → forbids `any`, requires exhaustive match,
  enables conservative borrow checks at stricter levels; `--strict` = global. Matches decisions E2/E5
  in WHY_PLAN.
- **Implementation approach:** thread a "strictness" level through the checker (`src/sema/checker.cpp`)
  and borrow pass (`src/sema/borrow.cpp`); add diagnostics per level.
- **Relevant files:** `src/sema/checker.cpp`, `src/sema/borrow.cpp`, `src/lex/lexer.cpp` (attribute),
  `docs/FEATURE_GAP_ANALYSIS.md` §3.4.
- **Code/syntax examples:**
  ```coco
  any x = get_anything();       # ok anywhere
  @checked def f(y: any) -> int { return y; }   # ERROR: 'any' not allowed in checked scope
  ```
- **Testing:** positive/negative sets for each level; existing corpus must pass unchanged in the
  default (non-strict) mode.
- **Expected outcome:** a graded safety story with zero breakage of existing code.
- **Risks:** over-strict defaults annoy; keep strictness **opt-in and incremental**. Owner:
  `WHY_PLAN.md` WHY-2/WHY-10.

---

### Phase 10 — FFI / C interop refinement (`extern "C"`, linking, marshaling)
- **Goal:** clean `extern "C"` declarations that bind `.lib`/`.so`/`.dll`, with primitive/string/
  buffer marshaling and a manifest link line.
- **Problem being solved:** C's FFI reach (everything exposes a C ABI) is why it endures; Rust/Go/C#
  win by interoperating with it. This is "why you can ship real software in Coco today" — reuse the
  entire C ecosystem without rewriting.
- **Why it matters:** the single most practical adoption unlock; a Coco program that can call
  `libcurl`, `sqlite3`, or a game engine's C API is immediately useful.
- **Coco design:** `extern "C"` block (already seeded — `examples/24_ffi_unsafe.co`) + `@repr(C)`
  (Phase 8) + manifest `[link]` libs; automatic marshaling for primitives/strings/buffers.
- **Implementation approach:** extend the existing `extern def`/`c""` FFI seed; wire the linker
  command into `coco build` (`buildProgram`, `tools/coco.cpp:2319`; cross-build `:2451-2560`).
- **Relevant files:** `tools/coco.cpp`, `src/parser/parser.cpp` (`extern`), `src/backend/native.cpp`,
  `src/interp/runtime.cpp` (`c_ptr`), `grammar/coco.ebnf`.
- **Code/syntax examples:**
  ```coco
  extern "C" {
      def puts(s: c"str") -> i32;
      def strlen(s: c"str") -> usize;
  }
  def main() { puts("hello from C!".as_cstr()); }   # linked via coco.toml [link] msvcrt
  ```
- **Testing:** `examples/46_ffi.co`-style native build calling a small C lib; CI build links and runs.
- **Expected outcome:** real interop → access to the whole C ecosystem.
- **Risks:** ABI/pointer-safety marshaling; keep `unsafe` boundary explicit. Owner: `WHY_PLAN.md`
  WHY-13.

---

### Phase 11 — Safety tooling & `coco vet` (lints matured)
- **Goal:** fold safety findings (uninitialized reads, nil-`T?` unwraps, index-range heuristics,
  panic paths, deadlock in `spawn`, integer-overflow config) into `coco check`/`coco lint`; `coco
  vet` = `check --safety`.
- **Problem being solved:** beyond correctness, mature languages ship trust-tooling (go vet, clippy,
  -fsanitize). This is Coco's cheapest-per-hour trust builder — reuses the completed lint infra.
- **Why it matters:** organizations gate adoptions on these gates.
- **Coco design:** lint groups + `@warn/@allow/@deny`; wire the existing W0101–W0107 and add
  safety-specific W-codes using the checker/borrow info already computed.
- **Implementation approach:** extend Phase 2's `coco lint`/`check`; add the conservative safety
  subset to `src/sema/borrow.cpp`/`checker.cpp`.
- **Relevant files:** `src/sema/checker.cpp`, `src/sema/borrow.cpp`, `src/support/diag.h`,
  `tools/coco.cpp`.
- **Code/syntax examples:**
  ```coco
  @allow(W-some) def risky() { ... }   # suppress a known finding locally
  ```
- **Testing:** a deliberately buggy `tests/negative/` file yields grouped safety findings; `@allow`
  suppresses one; corpus green.
- **Expected outcome:** a vet-style safety gate. Owner: `WHY_PLAN.md` WHY-14.

---

### Phase 12 — Stdlib breadth: networking, crypto, full regex (batteries moat)
- **Goal:** add `net` (TCP/UDP/HTTP client), `crypto` (common digests/hash), and a real `regexp`
  NFA engine; keep each a Coco `pin.co` module with a `*_test.co`.
- **Problem being solved:** Python's largest #1 driver is stdlib + ecosystem. Coco's stdlib covers
  10 modules but not networking/crypto/full-regex — the gap a "batteries" language can't ignore.
- **Why it matters:** "can I build a real product without third-party libs?" must be yes.
- **Coco design:** implement modules in Coco on the runtime primitives where possible (`os` env,
  file IO already in `runtime.cpp`; `ws2_32` already linked in `tools/coco.cpp` for the networking
  entry). Full regex needs a small native NFA engine (owner: STD_LIBS_PLAN).
- **Implementation approach:** expose runtime networking builtins (socket/connect/send/recv) then
  wrap in `stdlib/lib/net.co`; regex NFA in native + `.co` wrapper; crypto via `@repr(C)` + FFI or
  pure-Coco where tractable.
- **Relevant files:** `stdlib/lib/*.co`, `src/interp/runtime.cpp` (builtins), `tools/coco.cpp`
  (ws2_32), `STD_LIBS_PLAN.md`.
- **Code/syntax examples:**
  ```coco
  import lib.net;
  import lib.regexp;
  conn = net.connect("example.com", 80);
  net.send(conn, "GET / HTTP/1.0\r\nHost: example.com\r\n\r\n");
  body = net.recv(conn);
  m = regexp.find(r"HTTP/\d\.\d", body);
  ```
- **Testing:** `stdlib/lib/net_test.co`, `regexp_test.co`, `crypto_test.co`; CI runs each module.
- **Expected outcome:** a genuinely batteries-included stdlib → "build a product without deps".
- **Risks:** networking/security surface; keep modules opt-in via explicit import, document
  threading/throughput limits. Owner: `STD_LIBS_PLAN.md` (→ this plan owns the *why* and module
  priority).

---

### Phase 13 — Registry + publishing loop (`coco publish` / ecosystem)
- **Goal:** a first-class publish/browse/install loop: `coco publish`, a browsable registry mirror
  (`coco list online` exists), versioning/semver discipline, dependency audit (`coco audit`).
- **Problem being solved:** "a language without a package story dies" (Python/PyPI, Go proxy, crates).
  Coco has the plumbing (`coco add/install/update/remove`, `coco_libs`, `pin.co`) but no populated
  registry.
- **Why it matters:** ecosystem is the ultimate flywheel; but an empty registry is worse than none —
  sequence AFTER stdlib breadth (Phase 12) so there's something to publish.
- **Coco design:** `coco publish` packs a published library (mirroring `coco build` bundles), signs
  metadata, registers in the local mirror; `coco add` pulls it; `coco audit` checks dependency
  versions/security (small scope).
- **Implementation approach:** extend `tools/coco.cpp` (`install`/`add` at), define a registry
  schema, seed the mirror with the 10 stdlib modules + a couple of demo libs.
- **Relevant files:** `tools/coco.cpp`, `coco.toml`/`coco.lock` schema, `coco_libs/`, `pin.co`.
- **Code/syntax examples:**
  ```
  $ coco publish lib/math.co        # packs + registers (semver from coco.toml)
  $ coco add github/foo@1.2.0       # pulls into a fresh project
  $ coco audit                      # reports out-of-date / flagged deps
  ```
- **Testing:** local-mirror round trip (publish → fresh `coco new` → `coco add` → run); `coco list
  online` lists it.
- **Expected outcome:** a credible (if small) package story that can grow.
- **Risks:** supply-chain trust — require `coco.toml` integrity, lockfile pins, publish-time checks
  from day one. Owner: `WHY_PLAN.md` WHY-5, `EXP_PLAN.md`.

---

### Phase 15 — Concurrency ergonomics & safety lints (`for in chan`, deadlock/race warnings)
- **Goal:** polish existing `spawn/chan/select` into Go-quality ergonomics + lint-safety.
- **Problem being solved:** Go wins cloud adoption on goroutines being the cleanest concurrency
  model; Coco has the machinery but not the polish (no `for v in chan`, no deadlock/race lints).
- **Why it matters:** concurrency "just working" is a concrete, demonstrable differentiator over
  Python (GIL) and Ruby.
- **Coco design:** `for v in chan` (WHY-3), `select` clarity, spawn-with-closures already work; add
  deadlock-heuristic + channel-close-usage lints to `coco lint`.
- **Implementation approach:** extend the parser/iterateSeq for channels (`runtime.cpp:2847`); add
  lint rules in the checker.
- **Relevant files:** `src/interp/runtime.cpp` (`iterateSeq`, `spawn`/`chan`), `src/parser/parser.cpp`,
  `src/sema/checker.cpp`.
- **Code/syntax examples:**
  ```coco
  ch = chan[int](2);
  spawn (() => { for i in 1..=5 { ch.send(i); }; ch.close(); });
  for v in ch { print(v); }         # range over a channel
  ```
- **Testing:** `tests/concurrency/` pipeline demo + a deadlocked sample that fires a warning; corpus
  green on VM/native.
- **Expected outcome:** Go-quality concurrency ergonomics + safety net. Owner: `WHY_PLAN.md` WHY-8.

---

### Phase 16 — `for`-over-anything + collection ergonomics (readability layer)
- **Goal:** `for k,v in d`, `for c in s`, custom iterable protocol, splat `*args/**kwargs`,
  dict/set comprehensions (readability wins below the type system).
- **Problem being solved:** dict/set ergonomics are where Python's daily readability wins; `for
  over-anything` is Go's shortest-code driver.
- **Why it matters:** readable code is the "why Coco over Rust-for-that-part" story.
- **Coco design:** extend `iterateSeq` + a public iteration protocol (`impl Iterator`, example 26);
  comprehension over any iterable; splat where `CallArg` supports it (`ast.h:87-90`).
- **Implementation approach:** parser/`iterateSeq` extensions; all sugar desugars to existing AST/VM.
- **Relevant files:** `src/interp/runtime.cpp`, `src/ast/ast.h`, `src/parser/parser.cpp`.
- **Code/syntax examples:**
  ```coco
  d = {"a": 1, "b": 2};
  for k, v in d { print(k, "=", v); }
  evens = [x for x in range(10) if x % 2 == 0];
  ```
- **Testing:** `examples/39_collections.co`-style; negative for mis-typed iteration; corpus green.
- **Expected outcome:** prose-like collection code. Owner: `WHY_PLAN.md` WHY-3/WHY-6.

---

### Phase 17 — Pattern & trait power-up to Rust `PatKind` parity (correctness layer)
- **Goal:** struct-pattern `{..}`, nested or/alias/ref patterns, exhaustive match on sealed enums,
  blanket impls — closing the remaining `PatKind` gaps (FEATURE_GAP `§3.2`).
- **Problem being solved:** Rust's pattern matching + traits are why people trust it with complex
  systems; they make intent explicit and exhaustive.
- **Why it matters:** the "safe to ship" half of the two-ended pitch, and the Rust-adopter bridge.
- **Coco design:** structural patterns on structs/tuples/lists with `..` rest, `&`-ref patterns;
  resilient exhaustive-match; generic defaults/blanket impls where feasible.
- **Implementation approach:** extend `parser.cpp` pattern parsing + `checker.cpp` exhaustiveness;
  recheck with `tests/types/*`. Owner: `WHY_PLAN.md` WHY-9, `FEATURE_GAP_ANALYSIS.md` §3.2.
- **Relevant files:** `src/parser/parser.cpp` (patterns), `src/sema/checker.cpp`, `src/ast/ast.h`
  (PatKind), `src/vm/compiler.cpp`, `grammar/coco.ebnf`.
- **Code/syntax examples:**
  ```coco
  match p {
    case Point { x: 0, .. } => print("on y-axis")
    case Point { x, y }     => print(x + y)
    case (a, .., b)         => print(a, "…", b)   # rest in tuples
  }
  ```
- **Testing:** `tests/pattern/` positive + non-exhaustive negative; `examples/42_patterns.co`;
  differential VM/native.
- **Expected outcome:** Rust-grade pattern power → explicit, exhaustive, readable code. Owner:
  `WHY_PLAN.md` WHY-9.

---

### Phase 18 — Consolidation: docs site, editions, AI-corpora handling, marketing integrity (the lasting why)
- **Goal:** canonical docs (`coco doc` site aggregating plans/API/tutorials), edition mechanics
  (2026/2027 soft-keyword gating), and an **AI-readiness** plan so AI assistants stop hallucinating
  Coco.
- **Problem being solved:** every surviving language has docs + a formatter + an edition/compat story
  + a registry — the institutional moat. Plus (2026-specific) **the AI gap**: AI tools produce poor
  output for languages with a thin corpus.
- **Why it matters:** a language you can read, keep compatible, find packages in, *and* that AI tools
  understand is the language that lasts.
- **Coco design:**
  - Docs: serve all plans + API ref + examples from `coco doc` (exists: `tools/coco.cpp:1469-1721`)
    into a browsable site.
  - Editions: gate soft keywords/features behind an edition (frozen keyword set in `coco.ebnf §1`);
    add `--edition 2026`.
  - **AI strategy:** publish an exhaustive, compact reference (grammar + stdlib signatures) as
    machine-readable context; curate the example corpus so it is *representative* for LLM training;
    document "Coco-aware" prompting; seed a `Coco.tmLanguage`/`tree-sitter-coco` grammar so code
    syntax-highlights and parses correctly in editors and LLM pipelines.
- **Implementation approach:** extend `coco doc` for a multi-page site; add edition gating in the
  lexer (`grammar/coco.ebnf` freezes keywords); author + publish the AI corpus/reference; add
  tree-sitter/tmLanguage.
- **Relevant files:** `tools/coco.cpp` (doc), `grammar/coco.ebnf`, `src/lex/lexer.cpp`,
  `docs/`, `.github/workflows/` (jekyll site already exists).
- **Code/syntax examples:**
  ```
  # coco.toml
  [project]
  edition = "2026"      # pins the keyword/semantics surface
  ```
- **Testing:** a 2026-edition project and a legacy project both build; `coco fmt --check` green on
  all examples; docs site renders; grammar lints clean; AI-reference is byte-stable.
- **Expected outcome:** the institutional + AI moats that make Coco "readable, compatible,
  findable, and AI-understood."
- **Risks:** editions introduce compatibility policy; keep the frozen keyword list stable. Owner:
  `WHY_PLAN.md` WHY-15, and this plan adds the AI-readiness track no other plan owns.

---

## 7. Risks, and the AI-adoption reality (must-read honesty)

1. **The AI gap is the 2026 killer (research: edgl.dev, JetBrains).** AI assistants amplify whatever
   corpus exists. For a small language they hallucinate APIs and idioms, making them *actively
   misleading* — a productivity tax that freezes adoption. **This plan's Phase 18 is the explicit
   mitigation**: a curated, machine-readable reference + representative corpus + tree-sitter grammar
   so Coco sits in model training/eval as early and cleanly as possible. Coco's *consistency* (few
   duplicate keywords — see `NEED_REMOVE_PLAN.md`) also directly helps AI: a cleaner, more
   rule-regular grammar is far easier for models to reproduce correctly than a large, irregular one.
2. **Overclaiming destroys trust.** The README currently says "JIT" and "outperforms C/C++" without
   measured support. A serious adopter will benchmark and find the gap. Phases 5–7 exist to either
   *match the claim* or *publicly re-scope it*. Never let a marketing line outrun a benchmark.
3. **Bundling too many "why"s risks doing none well.** Coco aims at Python+Go+Rust+C. The discipline
   is prioritizing completeness of the *already-honest* strengths (readability, concurrency,
   batteries, checking, scalar-native speed) before chasing the hardest (full borrowck/NLL, real JIT,
   WASM). Phase ordering above does exactly that.
4. **Ecosystem chicken-and-egg.** The registry (Phase 13) is sequenced *after* stdlib breadth
   (Phase 12) so an empty registry isn't exposed prematurely.
5. **Tooling is the compounder.** fmt/repl/check/LSP (Phases 1–4) multiply every other feature's
   usefulness; they are intentionally first.
6. **Two-language collapse is hard but is the whole point.** The market evidence (Python→Go/Rust
   rewrites, polyglot strategies) shows real demand. Coco must be able to honestly say: "prototype
   fast here, ship fast here, stay memory-safe here — in one file." Phases 6–7 (native/JIT breadth)
   plus 9–11 (gradual safety) are what turn that from aspiration to reality.

---

## 8. Suggested execution order (value-first for a solo/team dev)

```
Phases 1-4  tooling (fmt/check/repl/LSP)      → compounds everything       [cheap, high trust]
Phases 5    honest bench                      → fixes the README gap       [must-do early]
Phases 15,16 collection/concurrency ergonomics→ readability wins
Phases 9,11 gradual safety + vet              → trust floor
Phase 10    FFI                               → practical usefulness
Phases 6,7  native breadth / JIT              → the "fast" proof
Phase 8     low-level surface (sizeof/@repr)  → systems credibility
Phase 12,13 stdlib breadth + registry         → batteries/ecosystem
Phase 17    pattern/trait parity              → correctness layer
Phase 18    consolidation + AI-readiness      → the lasting moat
```

---

## Appendix A — Prioritized capability-vs-pain mapping (which pain each phase attacks)

| Phase | Developer pain it attacks (from §2) |
|---|---|
| 1 fmt | Go "readable any codebase"; "one way to format" (Go) |
| 2 check/lint | TS compile-time-caught bugs; "validation tests" pain; C needs-sanitizers |
| 3 repl | Python/JS/Ruby "zero-config try it" on-ramp |
| 4 LSP | "no IDE support" new-language blocker; TS `$25/type-error` DX |
| 5–7 bench/AOT/JIT | Python 10–100× slowness; "pure vs practical perf"; README honesty |
| 8 sizeof/alignof/@repr | C control; "shave bytes/latency" |
| 9 gradual safety | Rust 2–3-month learning cliff; "safe AND fast to write" |
| 10 FFI | C ecosystem reach; "ship real software today" |
| 11 vet/lints | go vet/clippy/sanitizers trust; CVE memory-safety (70%) |
| 12 stdlib breadth | Python batteries; "build a product without deps" |
| 13 registry | PyPI/cargo/go-proxy ecosystem flywheel |
| 15 concurrency | Python GIL; Ruby concurrency ceiling; "goroutines cleanest" |
| 16 for-everything | Python dict ergonomics; Go shortest code |
| 17 pattern/trait | Rust correctness; exhaustive intent |
| 18 docs/editions/AI | "readable & lasting"; AI-adoption gap |

## Appendix B — "Will not do" (deliberately, to avoid copying a flawed incumbent)

- No `++`/ternary (rejected in grammar; kept out on purpose).
- No `if err != nil` verbosity (Go's #1 complaint) → we have `result[T,E]`, `?`, `raise/catch`.
- No unchecked dynamic-only default → we ratchet to checked (Phase 9).
- No C-style raw pointers as the default → `unsafe` is opt-in.
- No JVM-style GC as the only model → value semantics + ARC + arena + `unsafe`.
- No `anywhere` type erasure → runtime-typed values with checked indexing (unlike TS's erased types).
- No `node_modules`-scale dependency sprawl → lockfile + audited registry (Phase 13).

## Appendix C–I — Research references (web, 2026)

- **C. Python:** SO 2026 (~38%, ML/AI default; AI tools best at Python) — langpop, universopython,
  tech-act, sourcetrail; pain: 10–100× CPU gap, GIL (free-threaded optional ~10% slower), memory
  (180MB→1.1GB vs Go), dynamic-type runtime risk, dependency hell — PlainEnglish/Medium 2026, DEV.
- **D. Rust:** memory safety without GC; CISA/ONCD/NSA/EU CRA mandates; 45% enterprise prod;
  WASM default; pain: 2–3 month learn curve; borrow-checker; slow compiles (Rust's own 2025 survey,
  3700 responses); no stable ABI; ecosystem gaps in GUI/sci — javacodegeeks, devache, byteiota,
  rust-lang blog.
- **E. Go:** 25 keywords, spec-in-afternoon; near-instant builds; goroutines; one-toolchain
  batteries; GC latency bad for HFT/real-time; `if err != nil` verbosity (28% want better); nil
  interface gotcha; no sum types; limited metaprogramming; 37% vendoring — devache, nazarboyko,
  precisionaiacademy, danilchenko.
- **F. Ruby:** readability + Rails productivity (CRUD <1h); Shopify/GitHub; pain: ~6.4% pool, slow
  CPU, heavy memory, weak concurrency, "feels fast until it doesn't" — codecurious, dev.to, coderio.
- **G. C/C++:** performance/control; embedded 72%; **memory-safety crisis** — ~70% CVEs, Microsoft
  2030, Google, Linux; cryptic errors; complexity — byteiota, blogverdict, wrocpp, bjpr, techbytes.
- **H. JavaScript/TypeScript:** JS ~66%, TS #1 github ~44%; universal runtime; zero runtime type
  cost; pain: **types erased** (no runtime enforcement, Zod), npm supply-chain (Oct-2025 796-pkg
  attack), dependency bloat, toolchain fragmentation, `any`-theater — tech-insider, precisionaiacademy,
  state-of-js.
- **I. Java:** JVM portability, JIT peak, back-compat, LTS, Maven Central; pain: verbosity, 3–6s
  cold start, 350MB RSS, JIT warmup 30–60s, heap — netguru, 42works, scalo.
- **J. New-language/AI adoption:** AI coding assistants are the new TIOBE (edgl.dev); JetBrains
  migration report 2026; network-effects/supply chain; "five factors: adoption, jobs, LLM codegen
  quality, concurrency, deployment" (herlein).

---

*This is the strategic sibling of `WHY_PLAN.md`. It answers "why choose Coco" honestly — with the
strengths that are real today, the gaps stated plainly, the 2026 AI-adoption reality confronted, and
an 18-phase roadmap that never breaks the corpus. Phases 1–4 and 5 are the recomomended first cut;
Phases 6–7 and 14–18 are the long-horizon differentiators.*
