# Kirn — Why Use Kirn? (WHY_USE_KIRN_PLAN)

## Goal

This is the adoption/positioning plan: the honest, outward-facing answer to "why would a developer
choose Kirn instead of Python, Rust, Go, Ruby, C/C++, JS/TS, or Java?" It frames Kirn as an
**industry-standard, AI-first** language on two load-bearing promises: **(1) AI-first by
construction** — Kirn is _built_, not retrofitted, for AI tooling, for models _and_ developers
(rule-regular grammar → reproducible codegen, machine-readable docs → accurate model context, clean
training corpus → no API hallucination); and **(2) faster than Rust and Go on the real workloads** —
an honest, `kirn bench`-backable claim of measured tiering (native AOT / VM / tree-walker) with its
gaps stated plainly, never flattened into marketing. Every claim below either has a measurement or is
explicitly flagged aspirational and tied to the phase that will prove it.

## Must Follow

- **Two executables, exactly.** The toolchain build ships only `kirn` (driver) and `kirnc`
  (checker/compiler) — no auxiliary binaries, no "extra install step".
- **No over-engineering; no overclaiming.** A claim about performance or model-effectiveness ships
  only if `kirn bench` or a reference measurement backs it; otherwise it is stated as aspirational
  and tagged to the phase that will prove it (§6 Phases 5–7).
- **AI-first is the headline differentiator.** The pitch centers (a) consistency-of-grammar (few
  duplicate keywords → easier for models to reproduce), (b) machine-readable docs (grammar + stdlib
  signatures as model context), and (c) a clean, representative training corpus.
- **Kirn style.** Spell types as lowercase-width-micro + capitalized primitive (`Int`, `Float`,
  `Bool`, `Str`, `Char`, `BigInt`, `Rational`, `UInt`, `UFloat`, `Int4..UInt256`,
  `Float4..UFloat256`, `u4..u256`, `i4..i256`); capitalize free builtins/types (`Str(...)`,
  `BigInt(...)`, `__Sqrt`); keep methods lowercase.
- **Ground adoption claims.** Every "industry-standard" framing cites a verified reference language
  (`rust/`, `go/`, `cpython/`) so it rests on documented practice, not vibes.

## Important Note

Everything below the title is pre-existing, in-scope content. This file carries a **scope rule** (it
never re-specifies features owned by `PLAN.md`, `SYNTAX_PLAN.md`, `WHY_PLAN.md`, `STD_LIBS_PLAN.md`,
`DATA_TYPE_PLAN.md`, `EXP_PLAN.md`, `DO_FIRST_PLAN.md`, `MISSING_PLAN.md`), an **Owner handoff**
convention, and an **existing 18-phase roadmap (§6)** — all of which are preserved as-is. A prior
edit pass removed the obsolete-plan references this file used to carry; those removals stay,
and every other owner-plan reference is retained. The triples below (`**Goal:**` / **Must Follow:** /
**Important Note:**) wrap each section without deleting its prose.

**Status:** Strategic positioning + implementation roadmap · Living document · Version 1.0
**Date authored:** 2026-09-03 (landscape researched 2026; source audited 2026-09-03)
**Relationship to other plans:** `WHY_PLAN.md` is the _demand → capability_ internal roadmap that
sequences features by adoption driver against Python/Go/Rust/C. This document is its **public,
strategic sibling**: it answers the outward-facing question _"Why should a developer choose Kirn
instead of Python, Rust, Go, Ruby, C/C++, JavaScript/TypeScript, or Java?"_ — grounding every answer
in **real developer pain points (2026, web-verified)** mapped onto **what Kirn has implemented
today** (source-verified). It widens the field to Ruby and JS/TS (added), confronts the **AI-assisted
adoption barrier** that other plans do not address, and gives every phase the full
Goal / Problem / Why-it-matters / Design / Implementation / Files / Code / Tests / Outcome /
Risks structure.

> **How to read this.** §1 = the honest one-paragraph thesis. §2 = the landscape (why each language
> wins, its pain). §3 = a clean capabilities matrix (VERIFIED against source). §4 = the core
> "why Kirn" argument, split into _real strengths today_, _honest gaps_, and _what Kirn must promise
> but not break_. §5 = the central answer. §6+ = the multi-phase roadmap (Phases 1–18). §7 = risks &
> the AI-adoption reality. Appendices = evidence.

> **Scope rule.** This plan **does not re-specify** features already owned by `PLAN.md`,
> `SYNTAX_PLAN.md`, `WHY_PLAN.md`, `STD_LIBS_PLAN.md`, `DATA_TYPE_PLAN.md`, `EXP_PLAN.md`,
> `DO_FIRST_PLAN.md`, `MISSING_PLAN.md`. When a phase here touches work owned
> elsewhere, I say "(owner: X) →" and keep only the _why_ and the _positioning_, so the roadmap
> ecosystem stays non-contradictory.

---

## 1. The one-paragraph answer

**Kirn is the batteries-included systems language that collapses the "two-language problem."**
Every serious stack today forces a choice between a _safe-and-fast_ language (Rust/C++/Go — hard to
write, slow to iterate, thinnish batteries) and an _easy-to-write_ language (Python/JS/Ruby — fast
to prototype, slow to run, unsafe at scale). Kirn is engineered so a single language covers both
ends of that spectrum: it already has Python-grade builtins and comprehensions, Go-style
`spawn`/`chan`/`select` concurrency _built into the language_, Rust-style `match`/traits/generics/
exhaustiveness and a conservative borrow check, and a native AOT backend measured ~290× faster than
its own interpreter on recursion — all behind an English-first syntax that reads like prose and a
batteries-included stdlib + package manager + toolchain that ship with the language. The pitch is
not "another Rust" or "another Python"; it is: **write the whole product in one readable language,
prototype at Python speed, ship at C speed, and be memory-safe by default — without gluing three
stacks together.** That claim is only as strong as Kirn's honesty about what it still lacks, which
§3–§5 state plainly alongside what is already true today.

**Goal:** one honest paragraph that makes the two-language collapse feel _removable_ — readable,
batteries, native speed, and safety-by-default in a single file. **Must Follow:** every speed claim
stays in its tiered, `kirn bench`-measured form (scalar-native ≈ C, VM ~3× interp, interp runs
everything) — never a bare "faster than Rust/Go"; name the honest gaps in the same breath.
**Important Note:** this is the load-bearing summary of Claims A–C; Phases 5–11 exist to keep it
true, and it is publicly re-scoped if a phase fails its gate.

One file, both ends of the spectrum — `tools/crawl.kn`:

```kirn
import lib.core, lib.net, lib.json;
fn fetch(url: Str) -> Result[Str, Str] {
    body = net.get(url);                       # stdlib batteries, no deps
    return json.valid(body) ? ok(body) : err("bad json");
}
fn worker(q: Chan[Str]) {
    for u in q {
        match fetch(u) { case Ok(b) => index.digest(b); case Err(e) => log.warn(e); }
    }
}
fn main() {
    jobs = chan[Str](8);
    for _ in 1..=4 { spawn(() => worker(jobs)); }   # concurrency as a language primitive
    for u in input_urls() { jobs.send(u); }
}
```

~15 readable lines replace a Python+gevent prototype _and_ a Rust service for the hot path; the same
crawler in Rust needs crates, an async runtime, and `map_err` plumbing before line one.

> Reference: Rust — performance + memory safety is Rust's own documented "Why Rust?" pitch
> (`rust/README.md`): the high bar Kirn's fast-and-safe half claims.

---

## 2. The 2026 landscape: why each language wins, and where each one hurts

Web research (Appendices C–I) gives a consistent, cross-source picture. For each language family:
what developers actively choose it for (the "why"), and the recurring, counted pain points (the
"wedge" Kirn could fill).

| Language                  | Why developers choose it (2026)                                                                                                                                                                                                                    | Recurring pain points developers report (2026)                                                                                                                                                                                                                                                                                           |
| ------------------------- | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| **Python**                | Productivity & DX; batteries-included stdlib; dynamic typing; comprehensions; dominance of ML/AI; largest ecosystem (~38% SO usage); AI tools generate the best Python                                                                             | 10–100× slower than compiled on CPU-bound work; GIL serializes threads (free-threaded 3.14 is optional, ~10% slower, lib-incompatible); memory-hungry (a worker that was 180MB in Go → 1.1GB in Python); dynamic types surface errors at runtime (needs mypy); dependency hell / packaging friction; poor fit for mobile/games/real-time |
| **Rust**                  | Memory safety _without_ GC (now mandated — CISA/NSA/EU CRA); zero-cost abstractions; fearless concurrency; best-in-class tooling; 45%+ enterprise production usage; WASM default                                                                   | Steep learning curve — "2–3 months to productivity" vs "1–2 weeks" for Go; borrow-checker friction early; slow/complex compile times (a top cited problem, Rust's own 2025 survey); verbose ownership ceremony; no stable ABI for binary distribution; thinner ecosystem outside systems/networking/CLI; async is a separate, hard model |
| **Go**                    | Radical simplicity (25 keywords, spec readable in an afternoon); near-instant builds; goroutines/channels as _language_ primitives; one-toolchain batteries (`go fmt/test/doc/vet/build`); tiny fast-starting static binaries; "read any codebase" | Verbose `if err != nil` error handling (28% want better); GC latency is unpredictable (bad for HFT/real-time); nil-interface gotcha; no `++`, no sum types/`Option`/`Result` (state-machine bugs easy); limited metaprogramming; large binaries; 37% still stuck on vendoring                                                            |
| **Ruby**                  | Readability + developer happiness; Rails productivity (scaffold a full CRUD app <1h); convention-over-configuration; strong test culture; small-team shipping speed; Shopify/GitHub track record                                                   | Slimmer talent pool (~6.4% SO); slower on CPU-bound work; heavy memory; concurrency weak (Puma threading) vs Go/Rust; "feels fast until it doesn't" — perf issues hidden until scale; Rails-only relevance                                                                                                                               |
| **C/C++**                 | Raw performance & control; embedded/kernels/engines still need it; universal C ABI for interop; 72% of embedded teams                                                                                                                              | The **memory-safety crisis**: ~70% of critical CVEs in C/C++ are memory-safety bugs; Microsoft "remove C/C++ by 2030", Google mandate, Linux migration; cryptic template errors; high complexity after decades of features; no safety without 3rd-party sanitizers/profiles                                                              |
| **JavaScript/TypeScript** | Universal runtime (every browser/Node/Deno/Bun); JS still ~66%, TS the github #1 with ~44% usage; huge npm ecosystem; TS adds compile-time types + zero runtime overhead; native TS in Bun/Deno                                                    | TS types are **erased** — no runtime enforcement (validation needs Zod); npm supply-chain attacks (796-pack compromise, Oct 2025); dependency bloat / node_modules; toolchain fragmentation; `tsconfig` complexity; event-loop concurrency limits CPU-bound work; "type theater" with `any` everywhere                                   |
| **Java**                  | JVM portability (one artifact, three OSes); JIT peak throughput rivaling C++; backward-compat guarantee; Maven Central (10M+ artifacts); virtual threads (Project Loom) fixed the thread pain; LTS cadence                                         | Verbosity (though records/sealed/patterns helped); JVM cold-start 3–6s + pile of RAM (350MB RSS disqualifies serverless); JIT warm-up 30–60s; memory heap vs Go/Rust; heavy operational overhead                                                                                                                                         |

**Cross-cutting 2026 shifts (from the research):**

1. **Government/market memory-safety pressure** (US CISA/ONCD, EU Cyber Resilience Act, NSA) is
   pushing new _systems_ code to memory-safe languages. Kirn's conservative borrow check + rust-like
   exhaustiveness is directly on-trend.
2. **The AI coding-assistant advantage** now decides language adoption (JetBrains migration data:
   JS for jobs, Kotlin for DX, Go for performance, Python for ecosystem). A language with a small
   corpus gets _hallucinated APIs and wrong idioms_ from AI tools — an "AI gap" that freezes many
   new languages (§7 addresses this head-on).
3. **Nobody wants the two-language stack.** The production evidence (Python-rewrites-to-Rust/Go,
   "polyglot strategy", package-vs-perf bifurcation) shows teams spend enormous real cost gluing a
   fast language to an easy language. This is Kirn's opening.

**Goal:** map each incumbent's dominant _strength_ and _documented pain_ so Kirn's wedge is
verifiable, not asserted. **Must Follow:** every row stays web-researched and dated (2026), and each
claimed pain is compared against a reference-language source before we call it a gap — no row claims
a pain Kirn does not yet address in §6. **Important Note:** keep this a _landscape_, not a roast —
each language wins its users for real reasons (Rust safety, Go tooling, Python batteries) that
Claims A–C must credibly inherit.

> Reference: Rust — "memory safety" as the core documented promise is the source of the §2 mandate
> trend (`rust/README.md`).

---

## 3. Capability matrix: what Kirn has TODAY (source-verified)

From the audit of `src/` (~14,800 LOC C++20), `stdlib/`, `tools/`, `examples/` (48 files), `tests/`
(31 files), `grammar/kirn.ebnf` (557 lines), and the CI (`scripts/*.ps1`). ✅ = implemented.

| Capability                                 | Kirn               | Notes (evidence)                                                                                                                                                                                                    |
| ------------------------------------------ | ------------------ | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| Python-grade builtins                      | ✅ (mostly)        | `map/filter/reduce/any/all/sum/min/max/sorted/enumerate/str/int/float/bool/type/upper/lower/...` live in `installBuiltins()` (`src/interp/runtime.cpp:485-613`); residual `zip/flatten/round/floor/ceil/input` gaps |
| Comprehensions                             | ✅                 | list comps + multi-`for`/`if` clauses (`CompClause`, `src/ast/ast.h:92-97`)                                                                                                                                         |
| Dynamic/gradual typing                     | ✅ `any`/`dynamic` | both → `unkTy()` (`src/sema/checker.cpp:868`)                                                                                                                                                                       |
| Go-style concurrency                       | ✅                 | `spawn`/`chan`/`select`, real OS threads (`src/interp/runtime.cpp`), examples 22/23/30                                                                                                                              |
| `defer`, `panic`, recover                  | ✅                 | examples 20                                                                                                                                                                                                         |
| Rust-style `match` + patterns              | ✅ strong          | or/alias/slice/rest/range/guard patterns; enum exhaustiveness enforced (examples 12, 31, 38; `tests/types/n0*.kn`)                                                                                                  |
| Traits + static & dynamic dispatch         | ✅                 | `impl Trait for X`, trait objects, default bodies (example 13; `src/sema/checker.cpp`)                                                                                                                              |
| Generics with bounds                       | ✅                 | `[T is Bound]` (example 14)                                                                                                                                                                                         |
| `Option`/`Result` + `?` propagation        | ✅                 | `T?`/`none`, `result[T,E]`, `try`/`?` (examples 18, 19)                                                                                                                                                             |
| Error handling — exceptions                | ✅                 | `try`/`raise`/`catch`, `catch_panic` (examples 35, 20)                                                                                                                                                              |
| OOP facade                                 | ✅                 | `class`/`interface`/`record` desugar to `struct`/`trait` (example 36)                                                                                                                                               |
| Memory management                          | ✅ variant         | value semantics + ARC `new`/`box`, `weak` refs, arena `mem.Arena()`, `unsafe`, `ptr`/`ref` (examples 27–29)                                                                                                         |
| Conservative borrow check                  | ✅ (staged)        | `src/sema/borrow.cpp` escape analysis; negative tests `n06`, `types/n01`                                                                                                                                            |
| Native AOT backend                         | ✅ (scalar)        | `src/backend/native.cpp`; `kirn build -O`; fib(30) ≈ **290×** faster than tree-walker (≈59ms vs ≈17.3s); cross-compile arm64/linux                                                                                  |
| Bytecode VM                                | ✅                 | default runner, ~2.7–4× faster than tree-walker (`scripts/bench.ps1`)                                                                                                                                               |
| Batteries stdlib (written in Kirn)         | ✅ (10 modules)    | `core, collections, io, json, math, os, path, regexp, strings, time` in `stdlib/lib/`, each with `*_test.kn`                                                                                                        |
| Package manager / registry / lockfile      | ✅                 | `kirn new/run/test/install/add/update/remove/clone/list/doc/build/targets`; `kirn.toml`, `kirn.lock` (`tools/kirn.cpp`)                                                                                             |
| Doc generator / conventions                | ✅                 | `kirn doc`; `main.kn`/`pin.kn` run-once conventions                                                                                                                                                                 |
| Diagnostics with spans/caret/fix-its/lints | ✅                 | `src/support/diag.h` (245 lines), W0101–W0107 lints                                                                                                                                                                 |
| Differential correctness harness           | ✅                 | `vm_diff.ps1` (tree-walker ≡ VM byte-identical)                                                                                                                                                                     |

**Honest gaps (do NOT overclaim in front of a developer):**

- Native AOT is **scalar-only** today; collections/structs/concurrency still run through VM/interp
  (internal fast-path tiering: native → VM → tree-walker).
- **No JIT yet** — the README claims JIT and "outperforms C/C++"; those are **aspirational, not
  measured** (§6 Phases 6–8 must close this honesty gap with real, comparable benchmarks).
- No `kirn fmt`, no `kirn repl`, no LSP server, no debugger yet.
- stdlib lacks networking, crypto, and a full regex engine (`regexp` is a glob-subset).
- No established ecosystem/registry population; no third-party packages.
- No WASM/embedded targets; no `@repr(C)`/`sizeof`/`alignof` yet.

**Goal:** a source-verified snapshot of what Kirn does today, so adoption claims in Claims A–C cite
implemented features (with file/line evidence), never roadmap. **Must Follow:** every ✅ row stays
traceable to `src/`, `stdlib/`, `tools/`, `tests/`, or `grammar/`; every "honest gap" stays listed
beside its closing phase (§6) so nothing drifts into promotion. **Important Note:** the `langs-source-code/`
audit is the changelog of the adoption pitch — update it when a phase lands, don't append cheerleading.

> Reference: Go — goroutines/channels as language-level primitives is the documented pattern Kirn's
> concurrency row mirrors (`go/doc/go_spec.html`).

---

## 4. The core "why Kirn" argument (three honest claims)

**Goal:** reduce the §1 thesis to three individually defensible claims — readability+speed (A),
batteries (B), gradual safety (C) — each with what is true _today_ and what a phase must still make
true. **Must Follow:** each claim's evidence is §3-verified today-truths plus the named closing
phases; a claim loses both labels the moment it overstates. **Important Note:** the three claims are
the load-bearing structure of §5's central answer — keep them in lockstep with the honesty gaps.

### Claim A — "One readable language, both ends of the speed/safety spectrum"

_BEHIND:_ the two-language-stack cost is real and measured (Python→Rust/Go rewrites, polyglot
strategies). Kirn intends the Python-write speed _and_ the C-ship speed _and_ Rust-style safety in
one syntax you can read in one sitting. What is true today: builtins+comprehensions (`examples/34`),
`spawn/chan/select` as language primitives (no async-framework split like Rust/JS), `match`/
traits/generics (examples 12–15), a conservative borrow check (`src/sema/borrow.cpp`), and a native
backend (≈290×). What must still be true to keep the claim: Phases 5–8 (real benchmarks, broader
native coverage, honest docs).

**Goal:** convince a doubting engineer that "readable _and_ fast" is not a trade-off. **Must Follow:**
keep each sub-claim in its measured form (builtins now, concurrency now, borrow-check staged, native
scalar-only) and let §3's honesty gap delimit the promise — no claim crosses into Phases 5–8.
**Important Note:** the two-language collapse is framed as cost-benefit, not sentiment; the example
below is the pitch in miniature.

Same algorithm, a few readable Kirn lines where Rust needs HashMap + named lifetimes:

```kirn
fn freq(text: Str) -> Dict[Str, Int] {
    d = { };
    for w in re.split(r"\W+", text.lower()) { d[w] = d.get(w, 0) + 1; }
    return d;
}
fn top(counts: Dict[Str, Int]) -> Str {
    items = sort_by(counts.items(), |kv| kv.1, desc=true)[0..3];
    return join(items.map(|kv| kv.0 + ":" + Str(kv.1)), ", ");
}
```

Python-grade ergonomics (dict `.get` default, lambdas, slicing, `map`/`join`) with static types —
that part would be `HashMap<_,_>`, `entry().or_insert()`, and lifetime-bound iterators in Rust.

### Claim B — "Batteries by default, not by agonizing over tooling"

_BEHIND:_ Python wins on stdlib, Go on one-toolchain batteries, Rust on cargo. Each wins partly on
tooling. Kirn ships the toolchain with the language: package manager with manifest/lockfile/registry,
doc generator, test runner (`kirn test`), native + `.cob` bundles + cross-build, all from one binary.
What is true today: `tools/kirn.cpp` implements all of it; `stdlib/lib/*_test.kn` dogfoods the test
runner. What must be true to keep it: Phases 1–4 (fmt/repl/check/LSP) and Phases 12–13 (registry +
networking/crypto stdlib).

**Goal:** make "install `kirn` and start shipping" literally true — everything the toolchain needs
lives under the two executables. **Must Follow:** every planned feature (fmt/check/repl/LSP in
Phases 1–4) dispatches from `kirn`/`kirnc`, never a third binary; stdlib modules stay Kirn source
(`pin.kn`) so the language dogfoods them. **Important Note:** shape after Go's one-toolchain moat but
judge on deliverable quality (byte-stable `fmt`, gatable `check`), not breadth.

> Reference: Go — the single toolchain (go doc / go test / go vet / gofmt all in `go/src/cmd/`) is the
> documented batteries pattern Claim B inherits.

### Claim C — "Safety and sanity without the steep cliff"

_BEHIND:_ Rust's #1 objection is the learning curve (2–3 months). Kirn's pitch is _gradual_
safety: you can start with `any`/dynamic code and ratchet to checked/borrow-checked regimes. What is
true today: `any`/`dynamic` escape hatch, `unsafe` blocks, a conservative (not pedantic) borrow pass.
What must be true to keep it: Phases 9–11 (opt-in `@checked`/`--strict` ratchet, incremental
borrow-check hardening, safety lints + `kirn vet`).

**Goal:** let a team start dynamically and harden without rewriting — a ratchet, not a binary.
**Must Follow:** strictness stays opt-in and incremental; the existing corpus must pass unchanged in
the default mode; `@checked`/`--strict` diagnostics are W-coded and `@allow`-able. **Important Note:**
this is the "safe to ship" half of the pitch and the Rust-adopter bridge — but it must never be sold
as "borrow-check parity" until Phases 9–11 land.

---

## 5. The central answer (direct)

> **Why choose Kirn instead of Python, Rust, Go, Ruby, C/C++, JS/TS, or Java?**
> Because the mainstream answer to every one of those languages is a _compromise you don't want to
> make_, and Kirn is designed to remove it:
>
> - instead of Python's _speed_, Kirn compiles (AOT/VM) and has native concurrency;
> - instead of Rust's _learning cliff and compile times_, Kirn is readable, batteries-included, and
>   makes safety **optional-to-strict** rather than all-or-nothing;
> - instead of Go's _verbose errors and GC latency_, Kirn has `result[T,E]`, `?`, `match`, and
>   reference-counted/arena memory you can tune;
> - instead of C/C++'s _memory-safety crisis_, Kirn is safe by default with an `unsafe` escape;
> - instead of Ruby's _performance and concurrency ceiling_, Kirn ships green threads built-in;
> - instead of JS/TS's _erased types and npm supply chain_ risk, Kirn has real runtime types, safe
>   checked indexing, a lockfile, and fewer attack-surface dependencies;
> - instead of Java's _cold-start/memory/verbosity_, Kirn starts instantly, is lightweight, and is
>   concise.
>   **Choose Kirn when your project needs several of these at once** (a product, a service, a tool,
>   a systems component) **and you'd rather write it once, in one readable language, than glue a fast
>   language to an easy language.** Choose it _now_ for its strengths (readability, concurrency,
>   batteries, checking, native scalar perf) and help it converge on the rest.

**Goal:** a direct, quotable answer — "choose Kirn when you need several of these at once" — so the
whole plan compresses to one paragraph. **Must Follow:** each "instead of X" line maps to a verified
capability from §3 and a phase that closes whatever remains (Phases 5–13); none is a free-standing
promise. **Important Note:** the hedge in the last line ("help it converge on the rest") is load-bearing —
remove it only when Phases 6–11 make the whole list measured.

---

## 6. The multi-phase roadmap

Universal per-phase discipline (applies to every phase below):

- **Never break the corpus.** Every commit keeps `examples/`, `tests/`, `stdlib/`, `tools/` green on
  all three backends (`tree-walker ≡ VM ≡ native` via `scripts/vm_diff.ps1`).
- **Honesty gate.** No marketing claim ships before a measured benchmark or a working feature.
- **Owner handoff.** Where work is already owned by another plan, this plan links to it and only adds
  the _positioning/why_ and any gap that plan misses.

**Goal:** give every phase a shared constitution so the roadmap never drifts into hype or scope creep.
**Must Follow:** the corpus-greens gate, the honesty gate (`kirn bench` before claims), and the
owner-handoff rule apply to **every** phase below — each phase's triple inherits them.
**Important Note:** this is the binding contract that keeps the §2 pains, §3 honesty gaps, and
Claims A–C honest across all 18 phases; a phase that can't meet its gate is re-scoped, not sold.

---

### Phase 1 — `kirn fmt`: canonical, idempotent formatter

**Goal:** canonical bytes are a dev _and_ AI win — one way to format makes generated Kirn idiomatic
by construction. **Must Follow:** spacing-only changes, byte-identical on rerun
(fmt(fmt(x)) == fmt(x)), `--check` for CI; no AI-boost claim without a measured codegen check on the
formatted corpus. **Important Note:** owner `WHY_PLAN.md` WHY-4; this phase supplies the _why_ and
the gofmt-derived design only.

> Reference: Go — gofmt is the documented origin of "one way to format a codebase"
> (`go/src/cmd/gofmt/`).

- **Goal:** a `gofmt`-class formatter; `kirn fmt` normalizes any source, `kirn fmt --check` for CI.
- **Problem being solved:** Go's #1 readability moat is "one way to format a codebase". Without it,
  Kirn codebases (and AI-generated Kirn) diverge, and `readability` (Kirn's core pitch) erodes.
- **Why it matters to developers:** "Can I read any Kirn codebase in one sitting?" is a yes only if
  formatting is canonical. It compounds every other phase and makes AI codegen reproduce idiomatic Kirn.
- **Kirn design/feature proposal:** a lossless, idempotent pretty-printer driven by the existing AST
  (`src/ast/`, `ast_dump.cpp:653`), with stable rules mirroring `grammar/kirn.ebnf` §1 style. Preserve
  comments (the AST currently may drop some — fix to round-trip `#` comments).
- **Implementation approach:** new subcommand in `tools/kirn.cpp` dispatch (`runProgramSrc` at :489,
  `buildProgram` at :2319 are ready to be reused). Reuse `kirnparse`'s AST (`tools/kirnparse.cpp`)
  as the input; emit canonical text; verify idempotency (fmt(fmt(x)) == fmt(x)).
- **Relevant source files/components:** `tools/kirn.cpp`, `tools/kirnparse.cpp`, `src/ast/`,
  `src/parser/parser.cpp`, `grammar/kirn.ebnf`.
- **Code/syntax examples:**
  ```kirn
  # before:  fn   add(a:int,b:int)->int{return a+b;}
  # after:   fn add(a: int, b: int) -> int { return a + b; }
  ```
- **Testing requirements:** run `kirn fmt` over all 48 `examples/*.kn` + `stdlib/`;
  assert byte-identical on second pass; `--check` fails on any messy input; corpus still runs after
  formatting (spacing-only changes).
- **Expected outcome:** canonical bytes everywhere; CI enforces formatting; AI tools get a
  reproducible output shape.
- **Potential risks / trade-offs:** touching formatting of all files risks large diffs; must be
  spacing-only so behavior is provably unchanged (verify via `vm_diff`). Owner: `WHY_PLAN.md` WHY-4 →
  implement here as the positioning-critical first tool.

---

### Phase 2 — `kirn check` / `kirn lint`: fast feedback + safety lints

**Goal:** a gatable `kirn check`/`kirn lint` trust floor — orgs adopt on gates, and LLM editors read
diagnostics as ground truth. **Must Follow:** reuse the W0101–W0107 infra and ship through `kirnc`
(no new binary); keep W-codes conservative and `@allow`-able so false positives stay an edge, not a
moat. **Important Note:** owner `WHY_PLAN.md` WHY-4/WHY-14; the lint set grows only in Phases 9–11.

> Reference: Go — `go vet` ships beside the compiler as the standard static gate
> (`go/src/cmd/vet/`).

- **Goal:** a `go vet`/`clippy`-class `kirn check` (type + lint) and `kirn lint` (W-numbered
  findings).
- **Problem being solved:** safety and correctness tooling is a leading adoption trust-builder (Go
  shipped it; Rust ships clippy; C needs sanitizers). Kirn already has W0101–W0107 lints and
  diagnostics (`src/support/diag.h`) but no first-class `check`/`lint` entry point.
- **Why it matters:** trust is currency in 2026; organizations adopting a new language need
  "`kirn check` passes" as a gatable gate before considering it.
- **Kirn design:** expose the completed `checker.cpp` analysis as `kirn check <file>` (type errors →
  exit 1) and `kirn lint` (lint findings grouped by W-code, `@allow`/`@deny` toggles). Extend the
  lint set later (Phases 9–11).
- **Implementation approach:** add subcommands to `tools/kirn.cpp`; reuse `LintConfig`
  checker path. `kirn vet` can be an alias of `check --safety` (decision E8 in WHY_PLAN).
- **Relevant files:** `tools/kirn.cpp`, `src/sema/checker.cpp`, `src/sema/borrow.cpp`,
  `src/support/diag.h`.
- **Code/syntax examples:**
  ```kirn
  $ kirn check app.kn          # type errors reported with spans; exit 1 on failure
  $ kirn lint app.kn           # W0101 (unused), W0103 (immutable reassign hint), ...
  $ kirn vet app.kn            # safety subset: nil-unwrap, index-range, borrow-escape heuristics
  ```
- **Testing:** extend `tests/negative/` (n01–n18) + `types/` to be driven through `kirn check`;
  add `tests/lint/` with one file per W-code.
- **Expected outcome:** a gatable, scriptable static-analysis command — the trust floor.
- **Risks:** lint false-positives annoy users; keep W-codes conservative and `@allow`-able.
  Owner: `WHY_PLAN.md` WHY-4/WHY-14.

---

### Phase 3 — `kirn repl`: interactive loop for the dynamic surface

**Goal:** the lowest-friction on-ramp and a live sandbox for the dynamic surface — the first place
AI-tooling users probe Kirn. **Must Follow:** reuse the existing parse→check→eval path (no new VM),
support multi-line blocks, `Ctrl-C` hang protection. **Important Note:** owner `WHY_PLAN.md` WHY-4;
this is a tooling _completer_, not a language feature.

- **Goal:** a REPL that evaluates expressions/statements, aligned with `any`/dynamic (Phase) and
  `fmt` round-tripping.
- **Problem being solved:** Python/JS/Ruby win beginners and explorers with a live prompt; a new
  language without a REPL raises the "try it" barrier.
- **Why it matters:** lowest-friction on-ramp; also useful for teaching and debugging the dynamic
  surface.
- **Kirn design:** read lines → parse via existing parser → check → eval via tree-walker
  (`src/interp/runtime.cpp`). Support multi-line continuation for blocks/`fn` closures; print result
  unless `none`; keep history.
- **Implementation approach:** new `tools/kirnrepl.cpp` (or a `kirn repl` mode); reuse `kirnrun.cpp`
  pipeline (`tools/kirnrun.cpp`).
- **Relevant files:** `tools/kirnrun.cpp`, `src/interp/runtime.cpp`, `src/parser/parser.cpp`.
- **Code/syntax examples:**
  ```
  kirn> 1 + 2
  3
  kirn> xs = [1,2,3]; sum(x for x in xs if x > 1)
  5
  kirn> spawn (() => print("hello"))  # dynamic snippet works live
  ```
- **Testing:** scripted REPL sessions with expected output in `tests/repl/`.
- **Expected outcome:** an interactive on-ramp and teaching tool.
- **Risks:** infinite loop / hang in REPL; add `Ctrl-C` handling. Owner: `WHY_PLAN.md` WHY-4.

---

### Phase 4 — LSP server (`kirn lsp`) for editor integration

**Goal:** in-editor, continuous feedback that compounds DX _and_ makes AI assistants accurate — tools
read diagnostics. **Must Follow:** stdio JSON-RPC only; v1 scope = diagnostics + hover; ship as part
of the `kirn` tool flow, not a third binary. **Important Note:** owner `WHY_PLAN.md` WHY-4/WHY-15;
the symbol table added here also feeds `kirn doc`.

- **Goal:** a Language Server Protocol implementation so VSCode/Neovim/etc. get diagnostics,
  hover/doc, go-to-definition, rename.
- **Problem being solved:** "No IDE support" is a classic new-language blocker; TS's editorial
  advantage (IntelliSense, `$25 per type error caught` at compile time) is huge. Rust and every
  serious 2026 language ship an LSP.
- **Why it matters:** DX compounds; an LSP turns every lint/type feature into continuous, in-editor
  feedback and makes AI assistants more accurate (they read diagnostics).
- **Kirn design:** a stdio JSON-RPC server opening a file → `kirncheck` diagnostics as LSP
  `textDocument/publishDiagnostics`; hover via `kirn doc`-ready symbol info; go-to-fn/rename via a
  symbol table added to the checker.
- **Implementation approach:** new `tools/kirnlsp.cpp`; minimal JSON-RPC over stdin/stdout (reuse
  `util/tomlmini.h`-style lightweight parsing or a tiny JSON writer); hook `kirncheck` (existing) for
  diagnostics first.
- **Relevant files:** `tools/kirncheck.cpp`, `src/sema/checker.cpp` (add a symbol/index pass),
  `tools/kirn.cpp` (doc), `src/ast/`.
- **Code/syntax examples:** (protocol, no Kirn syntax) — `initialize` → `didOpen` → diagnostic
  notifications; hover returns a doc comment.
- **Testing:** LSP integration tests using a headless LSP client harness in `tests/lsp/`.
- **Expected outcome:** a working diagnostics-first LSP; foundation for hover/goto/rename.
- **Risks:** LSP is broad; scope v1 to diagnostics + hover only. Owner: `WHY_PLAN.md` WHY-4/WHY-15.

---

### Phase 5 — `kirn bench`: honest, reproducible benchmarks (close the README honesty gap)

**Goal:** honest numbers that close the §6/README honesty gap — perf is a moat only if proven.
**Must Follow:** apples-to-apples cases vs published harnesses (C++, Rust, Go, Python where sensible)
with harnesses published; every claim in §1–§3 and Claims A–C traces to a `kirn bench` row; outputs
deterministic within tolerance. **Important Note:** owner `WHY_PLAN.md` WHY-11; this is the honesty
gate every other "fast" claim waits on.

> Reference: Go — in-repo, repeatable microbenchmarks are Go's documented practice
> (`go/src/testing/benchmark.go`).

- **Goal:** a canonical in-repo benchmark suite that reports Kirn (tree-walker / VM / native AOT) vs
  a comparable baseline (C++, Rust, Go, Python where sensible), with tracked history.
- **Problem being solved:** the README's "JIT / outperforms C/C++" is unmeasured; a serious adopter
  will benchmark us and find a mismatch. Perf is a moat _only if proven_.
- **Why it matters:** this is the direct answer to "why not just C/Rust/Go for the fast part?" — real
  numbers, not marketing.
- **Kirn design:** `kirn bench` micro+macro harness; cases: fib, loops, matrix, string processing,
  dict/set ops, sort, JSON round-trip, a small concurrent workload. Nail the honest tiering:
  scalar-native ≈ C-speed, VM ~3× interp, interp slower but runs everything.
- **Implementation approach:** populate the empty `benchmarks/` dir; wire `scripts/bench.ps1` into
  `kirn bench`; add `--release` + `--native` flags; output JSON + markdown table.
- **Relevant files:** `benchmarks/`, `scripts/bench.ps1`, `tools/kirn.cpp` (build/run), `src/backend/
native.cpp`.
- **Code/syntax examples:**
  ```
  $ kirn bench --native fib
  kirn tree-walker  fib(30): 17.31 s
  kirn VM           fib(30):  ~4.4 s
  kirn AOT(msvc)    fib(30):  0.059 s   (≈290x vs interp)
  ```
- **Testing:** benchmark outputs are deterministic within a tolerance; a CI job runs a smoke subset.
- **Expected outcome:** published, honest numbers replacing aspirational claims; a tracked
  performance budget.
- **Risks:** benchmarks invite unfair comparison; pick apples-to-apples cases and publish harnesses.
  Owner: `WHY_PLAN.md` WHY-11.

---

### Phase 6 — Broaden the native AOT backend (beyond scalars)

**Goal:** make "faster than Rust/Go" cover the code users actually write hot, not just `fib`.
**Must Follow:** keep the native→VM fallback tier so correctness never depends on the boundary; every
new lowering ships with a differential test (native ≡ VM). **Important Note:** owner `PLAN.md` P8,
`FEATURE_GAP_ANALYSIS.md` §3.4; expand only what Phase 5's budget shows pays off.

The "fast for real code" target, in Kirn:

```kirn
struct Point { var x: i32; var y: i32; }
fn grid_sum(pts: List[Point]) -> i32 {
    s = 0;
    for p in pts { s += p.x * p.y; }   # hot loop lowers to native; output ≡ VM
    return s;
}
```

- **Goal:** mode-migrate the most-used non-scalar constructs onto native: simple structs, list/dict
  operations on hot paths, and selected builtin calls.
- **Problem being solved:** today AOT only lowers scalars; the "fast part" is too narrow for real
  apps, so the honest performance claim is limited.
- **Why it matters:** the C-speed claim must cover the code users actually write hot, not just fib.
- **Kirn design:** extend `src/backend/native.cpp` with struct-layout/allocation lowering and simple
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

**Goal:** a measured JIT tier or an explicit, public re-scope of the README claim — both are
acceptable outcomes; silence is not. **Must Follow:** gate behind `-Ojit` and differential tests
(JIT ≡ VM ≡ tree-walker); never claim "outperforms C/C++" on an unmeasured JIT. **Important Note:**
owner `PLAN.md` P13 (JIT/optimizer); highest-risk phase — budget accordingly and be transparent.

The no-friction path ("it just runs fast"), in Kirn:

```kirn
# hot scalar tail: after the first warm-up call, the body lowers to machine code
fn fib(n: i32) -> i32 {
    return n <= 1 ? n : fib(n - 1) + fib(n - 2);
}
fn main() {
    print(fib(30));    # handled by the JIT tier — no separate --native build step
}
```

- **Goal:** a JIT for the VM's hottest functions (not just static AOT of scalars), delivering native
  speed without a separate build/`--native` step.
- **Problem being solved:** the README promises JIT. Deliver a measured, scoped one or publicly
  re-scope the claim.
- **Why it matters:** "it just runs fast" (Python 3.13/3.14 added copy-and-patch JIT; Java JIT) — the
  no-friction path to C-class speed.
- **Kirn design:** start from the existing native lowering (`native.cpp`) reused via a tiny JIT:
  compile a function body to native at first hot threshold (straight-line, then loops). This is the
  highest-risk phase.
- **Implementation approach:** prototype producing machine code from the existing native codegen for
  hot scalar functions, called by the VM; measure against AOT. Gate behind `-Ojit`; keep AOT path
  canonical for now.
- **Relevant files:** `src/vm/compiler.cpp`, `src/backend/native.cpp`, `src/interp/runtime.cpp`,
  `src/vm/bytecode.h`.
- **Code/syntax examples:** `kirn run -Ojit fib.kn` gets within ~2× of AOT without a separate build.
- **Testing:** differential (JIT ≡ VM ≡ tree-walker) + Phase 5 bench.
- **Expected outcome:** a measured JIT tier or an explicit, documented decision to push users to AOT
  instead (removing the overstated claim).
- **Risks:** JIT is a large, ongoing effort (GC/stack maps, deopt); budget accordingly and be
  transparent. Owner: `PLAN.md` P13 (JIT/optimizer).

---

### Phase 8 — `sizeof`/`alignof`/`@repr(C)` low-level surface (C control when needed)

**Goal:** C control when needed — the "shave bytes/latency" developer is won here, not in syntax
surface area. **Must Follow:** value-types only first, hidden from GC semantics; layout must match
the target platform ABI precisely and be pinned by `tests/lowlevel/`. **Important Note:** owner
`WHY_PLAN.md` WHY-12, `SYNTAX_PLAN.md` SP-14; this is the ABI root of Phase 10 FFI.

> Reference: Zig — `@sizeOf`/`@alignOf` as spec'd compile-time builtins (`zig/doc/langref.html.in`)
> is the documented practice Kirn models the low-level surface on.

- **Goal:** compile-time layout constants and layout annotations for direct FFI/ABI mapping.
- **Problem being solved:** C's control surface is why systems teams don't leave C. Providing
  `sizeof`/`alignof`/`@repr(C)` wins the "I need to shave bytes/latency" developer.
- **Why it matters:** paired with FFI (Phase 10), it makes "write real systems software in Kirn"
  credible.
- **Kirn design:** `sizeof(T)`, `alignof(T)` as compile-time constants; `@repr(C)`/`@packed`/
  `@align(N)` struct layout annotations feeding the native backend.
- **Implementation approach:** extend the checker to compute layout (`src/sema/type.h` TyKind) and
  the native backend to honor annotations.
- **Relevant files:** `src/sema/checker.cpp`, `src/backend/native.cpp`, `grammar/kirn.ebnf`.
- **Code/syntax examples:**
  ```kirn
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

**Goal:** safety without Rust's cliff — a ratchet, not a binary; start dynamic, harden in place.
**Must Follow:** opt-in and incremental; existing corpus passes unchanged in the default mode;
every stricter level's diagnostics are W-coded and `@allow`-able. **Important Note:** owners
`WHY_PLAN.md` WHY-2/WHY-10; this is the "safe to ship" half of Claim C and the Rust-adopter bridge.

- **Goal:** opt-in strictness so a project can start dynamic and harden to checked/borrow-checked
  without rewriting.
- **Problem being solved:** Kirn's pitch is "safety without Rust's cliff." The mechanism is a
  **ratchet**: `any`/dynamic now, `@checked` stricter, `--strict` strictest — TypeScript's path.
- **Why it matters:** lets Kirn win "fast to write" (Python/JS crowd) _and_ "safe to ship" — the
  gradual path Rust and TS proved.
- **Kirn design:** `@checked` module/crate attribute → forbids `any`, requires exhaustive match,
  enables conservative borrow checks at stricter levels; `--strict` = global. Matches decisions E2/E5
  in WHY_PLAN.
- **Implementation approach:** thread a "strictness" level through the checker (`src/sema/checker.cpp`)
  and borrow pass (`src/sema/borrow.cpp`); add diagnostics per level.
- **Relevant files:** `src/sema/checker.cpp`, `src/sema/borrow.cpp`, `src/lex/lexer.cpp` (attribute),
  `docs/FEATURE_GAP_ANALYSIS.md` §3.4.
- **Code/syntax examples:**
  ```kirn
  any x = get_anything();       # ok anywhere
  @checked fn f(y: any) -> int { return y; }   # ERROR: 'any' not allowed in checked scope
  ```
- **Testing:** positive/negative sets for each level; existing corpus must pass unchanged in the
  default (non-strict) mode.
- **Expected outcome:** a graded safety story with zero breakage of existing code.
- **Risks:** over-strict defaults annoy; keep strictness **opt-in and incremental**. Owner:
  `WHY_PLAN.md` WHY-2/WHY-10.

---

### Phase 10 — FFI / C interop refinement (`extern "C"`, linking, marshaling)

**Goal:** reuse the entire C ecosystem _today_ — the single most practical adoption unlock.
**Must Follow:** keep the `unsafe` boundary explicit; v1 marshaling is primitives/strings/buffers
only; link line comes from the manifest, not magic. **Important Note:** owner `WHY_PLAN.md` WHY-13;
pairs with Phase 8's ABI so "write real systems software in Kirn" is credible.

- **Goal:** clean `extern "C"` declarations that bind `.lib`/`.so`/`.dll`, with primitive/string/
  buffer marshaling and a manifest link line.
- **Problem being solved:** C's FFI reach (everything exposes a C ABI) is why it endures; Rust/Go/C#
  win by interoperating with it. This is "why you can ship real software in Kirn today" — reuse the
  entire C ecosystem without rewriting.
- **Why it matters:** the single most practical adoption unlock; a Kirn program that can call
  `libcurl`, `sqlite3`, or a game engine's C API is immediately useful.
- **Kirn design:** `extern "C"` block (already seeded — `examples/24_ffi_unsafe.kn`) + `@repr(C)`
  (Phase 8) + manifest `[link]` libs; automatic marshaling for primitives/strings/buffers.
- **Implementation approach:** extend the existing `extern fn`/`c""` FFI seed; wire the linker
  command into `kirn build` (`buildProgram`, `tools/kirn.cpp:2319`; cross-build `:2451-2560`).
- **Relevant files:** `tools/kirn.cpp`, `src/parser/parser.cpp` (`extern`), `src/backend/native.cpp`,
  `src/interp/runtime.cpp` (`c_ptr`), `grammar/kirn.ebnf`.
- **Code/syntax examples:**
  ```kirn
  extern "C" {
      fn puts(s: c"str") -> i32;
      fn strlen(s: c"str") -> usize;
  }
  fn main() { puts("hello from C!".as_cstr()); }   # linked via kirn.toml [link] msvcrt
  ```
- **Testing:** `examples/46_ffi.kn`-style native build calling a small C lib; CI build links and runs.
- **Expected outcome:** real interop → access to the whole C ecosystem.
- **Risks:** ABI/pointer-safety marshaling; keep `unsafe` boundary explicit. Owner: `WHY_PLAN.md`
  WHY-13.

---

### Phase 11 — Safety tooling & `kirn vet` (lints matured)

**Goal:** a vet-style safety gate (uninitialized reads, nil-`T?` unwraps, index-range heuristics,
panic paths, `spawn` deadlocks) that orgs can gate on. **Must Follow:** conservative findings,
`@warn/@allow/@deny` escapes, shipped through `kirnc check` — the cheapest-per-hour trust builder.
**Important Note:** owner `WHY_PLAN.md` WHY-14; reuses the completed W0101–W0107 lint infra, extends
it only with checker/borrow info already computed.

- **Goal:** fold safety findings (uninitialized reads, nil-`T?` unwraps, index-range heuristics,
  panic paths, deadlock in `spawn`, integer-overflow config) into `kirn check`/`kirn lint`; `kirn
vet` = `check --safety`.
- **Problem being solved:** beyond correctness, mature languages ship trust-tooling (go vet, clippy,
  -fsanitize). This is Kirn's cheapest-per-hour trust builder — reuses the completed lint infra.
- **Why it matters:** organizations gate adoptions on these gates.
- **Kirn design:** lint groups + `@warn/@allow/@deny`; wire the existing W0101–W0107 and add
  safety-specific W-codes using the checker/borrow info already computed.
- **Implementation approach:** extend Phase 2's `kirn lint`/`check`; add the conservative safety
  subset to `src/sema/borrow.cpp`/`checker.cpp`.
- **Relevant files:** `src/sema/checker.cpp`, `src/sema/borrow.cpp`, `src/support/diag.h`,
  `tools/kirn.cpp`.
- **Code/syntax examples:**
  ```kirn
  @allow(W-some) fn risky() { ... }   # suppress a known finding locally
  ```
- **Testing:** a deliberately buggy `tests/negative/` file yields grouped safety findings; `@allow`
  suppresses one; corpus green.
- **Expected outcome:** a vet-style safety gate. Owner: `WHY_PLAN.md` WHY-14.

---

### Phase 12 — Stdlib breadth: networking, crypto, full regex (batteries moat)

**Goal:** "can I build a real product without third-party libs?" must be yes — Python's biggest
adoption driver. **Must Follow:** each module is a Kirn `pin.kn` with a `*_test.kn` and an explicit
opt-in import; mirror the documented module surface of a reference stdlib, don't guess.
**Important Note:** owner `STD_LIBS_PLAN.md` (this plan owns only the _why_ and module priority).

> Reference: CPython — stdlib-as-adoption-driver, e.g. `net`/`regexp`/`crypto` mirror Python's
> `socket`/`re`/`hashlib` (`cpython/Lib/socket.py`, `cpython/Doc/library/socket.rst`).

- **Goal:** add `net` (TCP/UDP/HTTP client), `crypto` (common digests/hash), and a real `regexp`
  NFA engine; keep each a Kirn `pin.kn` module with a `*_test.kn`.
- **Problem being solved:** Python's largest #1 driver is stdlib + ecosystem. Kirn's stdlib covers
  10 modules but not networking/crypto/full-regex — the gap a "batteries" language can't ignore.
- **Why it matters:** "can I build a real product without third-party libs?" must be yes.
- **Kirn design:** implement modules in Kirn on the runtime primitives where possible (`os` env,
  file IO already in `runtime.cpp`; `ws2_32` already linked in `tools/kirn.cpp` for the networking
  entry). Full regex needs a small native NFA engine (owner: STD_LIBS_PLAN).
- **Implementation approach:** expose runtime networking builtins (socket/connect/send/recv) then
  wrap in `stdlib/lib/net.kn`; regex NFA in native + `.kn` wrapper; crypto via `@repr(C)` + FFI or
  pure-Kirn where tractable.
- **Relevant files:** `stdlib/lib/*.kn`, `src/interp/runtime.cpp` (builtins), `tools/kirn.cpp`
  (ws2_32), `STD_LIBS_PLAN.md`.
- **Code/syntax examples:**
  ```kirn
  import lib.net;
  import lib.regexp;
  conn = net.connect("example.com", 80);
  net.send(conn, "GET / HTTP/1.0\r\nHost: example.com\r\n\r\n");
  body = net.recv(conn);
  m = regexp.find(r"HTTP/\d\.\d", body);
  ```
- **Testing:** `stdlib/lib/net_test.kn`, `regexp_test.kn`, `crypto_test.kn`; CI runs each module.
- **Expected outcome:** a genuinely batteries-included stdlib → "build a product without deps".
- **Risks:** networking/security surface; keep modules opt-in via explicit import, document
  threading/throughput limits. Owner: `STD_LIBS_PLAN.md` (→ this plan owns the _why_ and module
  priority).

---

### Phase 13 — Registry + publishing loop (`kirn publish` / ecosystem)

**Goal:** a credible publish/browse/install loop — ecosystem is the flywheel, but an empty registry
is worse than none. **Must Follow:** sequence after Phase 12 so there is something worth publishing;
supply-chain trust (manifest integrity, lockfile pins, publish-time checks) from day one.
**Important Note:** owners `WHY_PLAN.md` WHY-5, `EXP_PLAN.md`; this plan owns the sequencing argument.

- **Goal:** a first-class publish/browse/install loop: `kirn publish`, a browsable registry mirror
  (`kirn list online` exists), versioning/semver discipline, dependency audit (`kirn audit`).
- **Problem being solved:** "a language without a package story dies" (Python/PyPI, Go proxy, crates).
  Kirn has the plumbing (`kirn add/install/update/remove`, `kirn_libs`, `pin.kn`) but no populated
  registry.
- **Why it matters:** ecosystem is the ultimate flywheel; but an empty registry is worse than none —
  sequence AFTER stdlib breadth (Phase 12) so there's something to publish.
- **Kirn design:** `kirn publish` packs a published library (mirroring `kirn build` bundles), signs
  metadata, registers in the local mirror; `kirn add` pulls it; `kirn audit` checks dependency
  versions/security (small scope).
- **Implementation approach:** extend `tools/kirn.cpp` (`install`/`add` at), define a registry
  schema, seed the mirror with the 10 stdlib modules + a couple of demo libs.
- **Relevant files:** `tools/kirn.cpp`, `kirn.toml`/`kirn.lock` schema, `kirn_libs/`, `pin.kn`.
- **Code/syntax examples:**
  ```
  $ kirn publish lib/math.kn        # packs + registers (semver from kirn.toml)
  $ kirn add github/foo@1.2.0       # pulls into a fresh project
  $ kirn audit                      # reports out-of-date / flagged deps
  ```
- **Testing:** local-mirror round trip (publish → fresh `kirn new` → `kirn add` → run); `kirn list
online` lists it.
- **Expected outcome:** a credible (if small) package story that can grow.
- **Risks:** supply-chain trust — require `kirn.toml` integrity, lockfile pins, publish-time checks
  from day one. Owner: `WHY_PLAN.md` WHY-5, `EXP_PLAN.md`.

---

### Phase 15 — Concurrency ergonomics & safety lints (`for in chan`, deadlock/race warnings)

**Goal:** Go-quality ergonomics on machinery Kirn already ships — a demonstrable differentiator over
Python (GIL) and Ruby. **Must Follow:** deadlock/race findings are `kirn lint` heuristics with
`@allow` escapes, never hard errors; corpus stays green on VM/native. **Important Note:** owner
`WHY_PLAN.md` WHY-8; polish, not new concurrency primitives.

> Reference: Go — channels, selects, and channel range are language-level in the spec
> (`go/doc/go_spec.html`), the ergonomics Kirn's `for v in chan` mirrors.

- **Goal:** polish existing `spawn/chan/select` into Go-quality ergonomics + lint-safety.
- **Problem being solved:** Go wins cloud adoption on goroutines being the cleanest concurrency
  model; Kirn has the machinery but not the polish (no `for v in chan`, no deadlock/race lints).
- **Why it matters:** concurrency "just working" is a concrete, demonstrable differentiator over
  Python (GIL) and Ruby.
- **Kirn design:** `for v in chan` (WHY-3), `select` clarity, spawn-with-closures already work; add
  deadlock-heuristic + channel-close-usage lints to `kirn lint`.
- **Implementation approach:** extend the parser/iterateSeq for channels (`runtime.cpp:2847`); add
  lint rules in the checker.
- **Relevant files:** `src/interp/runtime.cpp` (`iterateSeq`, `spawn`/`chan`), `src/parser/parser.cpp`,
  `src/sema/checker.cpp`.
- **Code/syntax examples:**
  ```kirn
  ch = chan[int](2);
  spawn (() => { for i in 1..=5 { ch.send(i); }; ch.close(); });
  for v in ch { print(v); }         # range over a channel
  ```
- **Testing:** `tests/concurrency/` pipeline demo + a deadlocked sample that fires a warning; corpus
  green on VM/native.
- **Expected outcome:** Go-quality concurrency ergonomics + safety net. Owner: `WHY_PLAN.md` WHY-8.

---

### Phase 16 — `for`-over-anything + collection ergonomics (readability layer)

**Goal:** prose-like collection code — the daily readability wins that make "Kirn over Rust for that
part" a happy choice. **Must Follow:** all sugar desugars to the existing AST/VM (no special-cased
semantics); negative tests cover mis-typed iteration. **Important Note:** owners `WHY_PLAN.md`
WHY-3/WHY-6; this is the cheapest readability moat in the whole roadmap.

> Reference: Go — `range` over the built-in containers is spec'd (`go/doc/go_spec.html`), the pattern
> `for k, v in d` extends to dicts/sets/iterables.

- **Goal:** `for k,v in d`, `for c in s`, custom iterable protocol, splat `*args/**kwargs`,
  dict/set comprehensions (readability wins below the type system).
- **Problem being solved:** dict/set ergonomics are where Python's daily readability wins; `for
over-anything` is Go's shortest-code driver.
- **Why it matters:** readable code is the "why Kirn over Rust-for-that-part" story.
- **Kirn design:** extend `iterateSeq` + a public iteration protocol (`impl Iterator`, example 26);
  comprehension over any iterable; splat where `CallArg` supports it (`ast.h:87-90`).
- **Implementation approach:** parser/`iterateSeq` extensions; all sugar desugars to existing AST/VM.
- **Relevant files:** `src/interp/runtime.cpp`, `src/ast/ast.h`, `src/parser/parser.cpp`.
- **Code/syntax examples:**
  ```kirn
  d = {"a": 1, "b": 2};
  for k, v in d { print(k, "=", v); }
  evens = [x for x in range(10) if x % 2 == 0];
  ```
- **Testing:** `examples/39_collections.kn`-style; negative for mis-typed iteration; corpus green.
- **Expected outcome:** prose-like collection code. Owner: `WHY_PLAN.md` WHY-3/WHY-6.

---

### Phase 17 — Pattern & trait power-up to Rust `PatKind` parity (correctness layer)

**Goal:** Rust-grade patterns → explicit, exhaustive, readable code; the correctness half of the
pitch and the Rust-adopter bridge. **Must Follow:** exhaustiveness stays resilient (no un-sound
escape hatch); every new pattern form gets `tests/pattern/` positive + negative coverage and
differential VM/native runs. **Important Note:** owners `WHY_PLAN.md` WHY-9, `FEATURE_GAP_ANALYSIS.md`
§3.2; this closes the remaining `PatKind` gaps, not "new syntax for novelty".

> Reference: Rust — `Option`/`Result` are pattern-matched core enums (`rust/library/core/src/option.rs`,
> `rust/library/core/src/result.rs`), the trust pattern Kirn's `match`/`T?` inherit.

- **Goal:** struct-pattern `{..}`, nested or/alias/ref patterns, exhaustive match on sealed enums,
  blanket impls — closing the remaining `PatKind` gaps (FEATURE_GAP `§3.2`).
- **Problem being solved:** Rust's pattern matching + traits are why people trust it with complex
  systems; they make intent explicit and exhaustive.
- **Why it matters:** the "safe to ship" half of the two-ended pitch, and the Rust-adopter bridge.
- **Kirn design:** structural patterns on structs/tuples/lists with `..` rest, `&`-ref patterns;
  resilient exhaustive-match; generic defaults/blanket impls where feasible.
- **Implementation approach:** extend `parser.cpp` pattern parsing + `checker.cpp` exhaustiveness;
  recheck with `tests/types/*`. Owner: `WHY_PLAN.md` WHY-9, `FEATURE_GAP_ANALYSIS.md` §3.2.
- **Relevant files:** `src/parser/parser.cpp` (patterns), `src/sema/checker.cpp`, `src/ast/ast.h`
  (PatKind), `src/vm/compiler.cpp`, `grammar/kirn.ebnf`.
- **Code/syntax examples:**
  ```kirn
  match p {
    case Point { x: 0, .. } => print("on y-axis")
    case Point { x, y }     => print(x + y)
    case (a, .., b)         => print(a, "…", b)   # rest in tuples
  }
  ```
- **Testing:** `tests/pattern/` positive + non-exhaustive negative; `examples/42_patterns.kn`;
  differential VM/native.
- **Expected outcome:** Rust-grade pattern power → explicit, exhaustive, readable code. Owner:
  `WHY_PLAN.md` WHY-9.

---

### Phase 18 — Consolidation: docs site, editions, AI-corpora handling, marketing integrity (the lasting why)

**Goal:** the institutional + AI moats — docs, editions, and a corpus AI tools can actually train on.
**Must Follow:** AI-readiness artifacts (reference, corpus, grammar) are machine-readable and
byte-stable; editions freeze the keyword set in `grammar/kirn.ebnf §1`; the AI corpus is curated to
be _representative_, not padded. **Important Note:** owner `WHY_PLAN.md` WHY-15 — the AI-readiness
track is owned here, by no other plan, and it is the headline "AI-first" differentiator from the Goal.

- **Goal:** canonical docs (`kirn doc` site aggregating plans/API/tutorials), edition mechanics
  (2026/2027 soft-keyword gating), and an **AI-readiness** plan so AI assistants stop hallucinating
  Kirn.
- **Problem being solved:** every surviving language has docs + a formatter + an edition/compat story
  - a registry — the institutional moat. Plus (2026-specific) **the AI gap**: AI tools produce poor
    output for languages with a thin corpus.
- **Why it matters:** a language you can read, keep compatible, find packages in, _and_ that AI tools
  understand is the language that lasts.
- **Kirn design:**
  - Docs: serve all plans + API ref + examples from `kirn doc` (exists: `tools/kirn.cpp:1469-1721`)
    into a browsable site.
  - Editions: gate soft keywords/features behind an edition (frozen keyword set in `kirn.ebnf §1`);
    add `--edition 2026`.
  - **AI strategy:** publish an exhaustive, compact reference (grammar + stdlib signatures) as
    machine-readable context; curate the example corpus so it is _representative_ for LLM training;
    document "Kirn-aware" prompting; seed a `Kirn.tmLanguage`/`tree-sitter-kirn` grammar so code
    syntax-highlights and parses correctly in editors and LLM pipelines.
- **Implementation approach:** extend `kirn doc` for a multi-page site; add edition gating in the
  lexer (`grammar/kirn.ebnf` freezes keywords); author + publish the AI corpus/reference; add
  tree-sitter/tmLanguage.
- **Relevant files:** `tools/kirn.cpp` (doc), `grammar/kirn.ebnf`, `src/lex/lexer.cpp`,
  `docs/`, `.github/workflows/` (jekyll site already exists).
- **Code/syntax examples:**
  ```
  # kirn.toml
  [project]
  edition = "2026"      # pins the keyword/semantics surface
  ```
- **Testing:** a 2026-edition project and a legacy project both build; `kirn fmt --check` green on
  all examples; docs site renders; grammar lints clean; AI-reference is byte-stable.
- **Expected outcome:** the institutional + AI moats that make Kirn "readable, compatible,
  findable, and AI-understood."
- **Risks:** editions introduce compatibility policy; keep the frozen keyword list stable. Owner:
  `WHY_PLAN.md` WHY-15, and this plan adds the AI-readiness track no other plan owns.

---

## 7. Risks, and the AI-adoption reality (must-read honesty)

1. **The AI gap is the 2026 killer (research: edgl.dev, JetBrains).** AI assistants amplify whatever
   corpus exists. For a small language they hallucinate APIs and idioms, making them _actively
   misleading_ — a productivity tax that freezes adoption. **This plan's Phase 18 is the explicit
   mitigation**: a curated, machine-readable reference + representative corpus + tree-sitter grammar
   so Kirn sits in model training/eval as early and cleanly as possible. Kirn's _consistency_ (few
   duplicate keywords) also directly helps AI: a cleaner, more
   rule-regular grammar is far easier for models to reproduce correctly than a large, irregular one.
2. **Overclaiming destroys trust.** The README currently says "JIT" and "outperforms C/C++" without
   measured support. A serious adopter will benchmark and find the gap. Phases 5–7 exist to either
   _match the claim_ or _publicly re-scope it_. Never let a marketing line outrun a benchmark.
3. **Bundling too many "why"s risks doing none well.** Kirn aims at Python+Go+Rust+C. The discipline
   is prioritizing completeness of the _already-honest_ strengths (readability, concurrency,
   batteries, checking, scalar-native speed) before chasing the hardest (full borrowck/NLL, real JIT,
   WASM). Phase ordering above does exactly that.
4. **Ecosystem chicken-and-egg.** The registry (Phase 13) is sequenced _after_ stdlib breadth
   (Phase 12) so an empty registry isn't exposed prematurely.
5. **Tooling is the compounder.** fmt/repl/check/LSP (Phases 1–4) multiply every other feature's
   usefulness; they are intentionally first.
6. **Two-language collapse is hard but is the whole point.** The market evidence (Python→Go/Rust
   rewrites, polyglot strategies) shows real demand. Kirn must be able to honestly say: "prototype
   fast here, ship fast here, stay memory-safe here — in one file." Phases 6–7 (native/JIT breadth)
   plus 9–11 (gradual safety) are what turn that from aspiration to reality.

**Goal:** a must-read honesty section that a real adopter can bring to their team unfiltered.
**Must Follow:** each risk names its closing phase(s) — no risk is parked indefinitely; item 2
(overclaiming) is the master rule of the whole plan. **Important Note:** the AI-gap item is the 2026
differentiator and Phase 18 is its only owner; the other risks discipline the roadmap so the AI-first
claim stays credible.

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

**Goal:** give a solo/team dev a value-first order to ship in — cheap trust before hard proof.
**Must Follow:** the order respects dependencies (bench before native claims; stdlib breadth before
registry; Phases 6–7 only after Phase 5); a phase listed earlier is not "more important", just more
leverage-per-effort. **Important Note:** this is _suggested_; the §6 discipline gates still apply to
whichever order is actually executed.

---

## Appendix A — Prioritized capability-vs-pain mapping (which pain each phase attacks)

| Phase                  | Developer pain it attacks (from §2)                                      |
| ---------------------- | ------------------------------------------------------------------------ |
| 1 fmt                  | Go "readable any codebase"; "one way to format" (Go)                     |
| 2 check/lint           | TS compile-time-caught bugs; "validation tests" pain; C needs-sanitizers |
| 3 repl                 | Python/JS/Ruby "zero-config try it" on-ramp                              |
| 4 LSP                  | "no IDE support" new-language blocker; TS `$25/type-error` DX            |
| 5–7 bench/AOT/JIT      | Python 10–100× slowness; "pure vs practical perf"; README honesty        |
| 8 sizeof/alignof/@repr | C control; "shave bytes/latency"                                         |
| 9 gradual safety       | Rust 2–3-month learning cliff; "safe AND fast to write"                  |
| 10 FFI                 | C ecosystem reach; "ship real software today"                            |
| 11 vet/lints           | go vet/clippy/sanitizers trust; CVE memory-safety (70%)                  |
| 12 stdlib breadth      | Python batteries; "build a product without deps"                         |
| 13 registry            | PyPI/cargo/go-proxy ecosystem flywheel                                   |
| 15 concurrency         | Python GIL; Ruby concurrency ceiling; "goroutines cleanest"              |
| 16 for-everything      | Python dict ergonomics; Go shortest code                                 |
| 17 pattern/trait       | Rust correctness; exhaustive intent                                      |
| 18 docs/editions/AI    | "readable & lasting"; AI-adoption gap                                    |

**Goal:** prove every phase attacks a §2 pain already _traced to a source_ — the roadmap is evidence
architecture, not enthusiasm. **Must Follow:** adding a phase requires adding a row here; mapping a
row to a pain requires the pain to exist in §2's research. **Important Note:** rows that reference a
reference-language pain (Go fmt, `go vet`, CPython batteries) are the ground for the Part-Must-Follow
citation rule.

## Appendix B — "Will not do" (deliberately, to avoid copying a flawed incumbent)

- No `++`/ternary (rejected in grammar; kept out on purpose).
- No `if err != nil` verbosity (Go's #1 complaint) → we have `result[T,E]`, `?`, `raise/catch`.
- No unchecked dynamic-only default → we ratchet to checked (Phase 9).
- No C-style raw pointers as the default → `unsafe` is opt-in.
- No JVM-style GC as the only model → value semantics + ARC + arena + `unsafe`.
- No `anywhere` type erasure → runtime-typed values with checked indexing (unlike TS's erased types).
- No `node_modules`-scale dependency sprawl → lockfile + audited registry (Phase 13).

**Goal:** make the negative choices explicit so the plan says "no" before someone says "why not" —
each rejection names both the incumbent flaw and the Kirn replacement. **Must Follow:** every
"no" is a design decision with a §3 capability or a §6 phase behind it, not an aesthetic rule;
revisit a "no" only when evidence shifts. **Important Note:** the `if err != nil` rejection is the
most quotable — the example below is its 8-line proof:

Same "open then parse then get" job, with `?` instead of Go's `if err != nil` chain:

```kirn
fn load(path: Str, key: Str) -> Str? {
    f   = io.read(path) ?;    # propagate Err as none — no err-tree
    cfg = lib.json.parse(f) ?;
    return cfg.get(key);
}
fn main() { print(load("app.toml", "port") ?? "unset"); }
```

~8 Kirn lines vs Go's `if err != nil` at every call — the concrete shape of Go's #1 complaint
(28% want better) is the wedge Appendix B names.

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

**Goal:** audit trail — every pain in §2 and every matrix row in §3 has a dated, citable source so
the pitch survives cross-examination. **Must Follow:** no row cites a source that does not exist;
dark-web research cited here lives alongside the `langs-source-code/` tree-audited practices in
References. **Important Note:** external references ground the _market_ reality; in-repo
`langs-source-code/` references ground the _"industry-standard"_ framing — both must be quoted
together in any published claim.

---

_This is the strategic sibling of `WHY_PLAN.md`. It answers "why choose Kirn" honestly — with the
strengths that are real today, the gaps stated plainly, the 2026 AI-adoption reality confronted, and
an 18-phase roadmap that never breaks the corpus. Phases 1–4 and 5 are the recomomended first cut;
Phases 6–7 and 14–18 are the long-horizon differentiators._
