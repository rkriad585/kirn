# Kirn — MISSING_PLAN: The 19 "Unimplemented Features" Implementation Roadmap

## Goal

This plan is the additions ledger that fills the real, concrete gaps Kirn has versus the proven
reference languages — rust, go, cpython, ruby, Nim, Odin, v, and zig — by porting only features those
languages have already shipped and battle-tested, so Kirn becomes a genuinely industry-standard,
AI-first language that is faster than Rust and Go on the hot paths that matter while remaining
strictly free of over-engineering: every entry below must remove real friction, carry a recorded
reference-language precedent, lower to machinery Kirn already has (or to one small, shared
`VK`/`TyK`/operator-table extension), and never add speculative surface.

## Must Follow

- **Ship exactly two executables.** The toolchain builds only `kirn` (driver) and `kirnc`
  (checker/compiler); no third binary, no plugin host, no daemon.
- **Precedent + gap, recorded together.** Add a feature only when a reference-language precedent and
  the real gap it closes are written in the same entry; neither alone justifies new surface.
- **AI-first by construction.** Every new feature must be machine-checkable and agent-completable:
  deterministic diagnostics with codes/fix-its, typed signatures, and no ambiguity an agent cannot
  resolve from the spec.
- **Kirn-style spellings.** New builtins/types stay CapCase (`Str(...)`, `BigInt(...)`, `__Sqrt`),
  methods lowercase; width micros lowercase and primitives capitalized (`Int`, `Float`, `Bool`,
  `BigInt`, `Int4..UInt256`, `u4..u256`, `i4..i256`).
- **Test + bench, always.** Every added feature lands with a `kirn test` suite and a `kirn bench`
  measurement in the same change; no feature is "done" without both.

## Important Note

This is a **gap ledger**, not a redesign: it preserves the existing priorities and the M1–M12 queue
exactly as ratified, and keeps the add-only ordering intact — append missing entries, never reorder,
re-scope, or delete the prior roadmap.

**Status:** Active roadmap · Living document · Version 1.0
**Date authored:** 2026-09-03 (canonical semantics verified against the September 2026 state of
Python, PHP, JavaScript/TC39, Go, Rust, Kotlin, Java, Ruby via web research in `Appendix R`).
**Role in the plan ecosystem:** `PLAN.md` (infra), `SYNTAX_PLAN.md` (syntax/operators),
`EXP_PLAN.md` (expression surface), `DATA_TYPE_PLAN.md` (core types), `STD_LIBS_PLAN.md`
(module APIs), and `WHY_PLAN.md` (demand drivers) already _mention_ every feature in this list —
but spread across six documents at inconsistent depths, and **none are implemented in source**
(verified, §2). `MISSING_PLAN.md` is the **single executable roadmap** that:

1. **Consolidates the canonical cross-language semantics** for all 19 features in one place (so an
   implementer never has to re-derive "what should `??` do?" from six documents).
2. **Assigns each feature a single owning plan + a concrete phase**, cross-referencing the existing
   owners to _avoid re-specifying_ the parts they already got right.
3. **Ratifies the open design decisions** the structured reference surfaced (pedise §4), notably the
   **pipe-operator model**, the **walrus precedence**, and the **deliberate rejections** (`++/--`,
   `?:`) that are already decided elsewhere.
4. **Sequences everything into phases an agent can execute**, each with goals, files, steps,
   examples, and exit criteria.

> **Dedup contract:** wherever an existing plan already specifies a feature at the required depth,
> this document **points** to it and only adds the _canonical-semantics delta_ + the ratification +
> the sequencing. It does not duplicate the full API inventory. Where the existing spec is **stale,
> partial, or silent**, this document **owns** the correction (marked `[AMEND]`). Where a feature is
> **already decided as rejected**, this document records the rejection and the rationale so it is not
> revisited.

---

## 1. Ground truth — what the source actually has (audited 2026-09-03)

The structured reference calls these 19 items _"unimplemented features."_ Verified against source:

**Goal:** record the audited ground truth (planning status × source state) so no gap is claimed that
source already covers and none is lost.
**Must Follow:** the "In source today" column is the sole authority; re-audit `src/` paths before
touching a phase.
**Important Note:** a "Partial"/"Rejected" row is not new work — it only pins the delta to close.

| Feature                     | In plan docs?               | In source today | Source evidence                                                                                    |
| --------------------------- | --------------------------- | --------------- | -------------------------------------------------------------------------------------------------- |
| 1 Walrus `:=`               | SYNTAX SP-2, EXP P5         | **No**          | `:=` absent from `kOps2` (`src/lex/lexer.cpp:59-62`); no `ExKind::AssignExpr`                      |
| 2 Pipe `\|>`                | EXP P5                      | **No**          | `\|>` absent from lexer op tables                                                                  |
| 3 Elvis `??`/`??=`          | SYNTAX SP-4, EXP P6         | **No**          | `??` absent from `kOps2`; only `.?.` exists (`kOps3`, lexer:58)                                    |
| 4 `++`/`--`                 | PLAN §5.4, EXP §2           | **Rejected**    | grammar §4.9 keeps them out; use `+= 1`                                                            |
| 5 Ternary `?:`              | EXP §2 (§A4.9)              | **Rejected**    | `if c { a } else { b }` conditional-expression exists instead                                      |
| 6 `@` matrix mult           | EXP P13                     | **No**          | `@` is only a pattern-alias token today; no MatMul trait                                           |
| 7 Dict/set comps            | SYNTAX SP-9, EXP P12        | **Partial**     | `ListComp`+`CompClause` exist (`src/ast/ast.h:84,95`); no dict/set builders, no `*`/`**` unpacking |
| 8 Optional chain `?.`       | SYNTAX SP-4, EXP P6         | **Partial**     | `.?.` member form exists; no chaining, no `a?.[i]`, no `a?.()`                                     |
| 9 Smart casts               | EXP P6/P8, SYNTAX SP-5      | **No**          | `is` tests exist (`operatorIs`), but no branch-narrowing of static type                            |
| 10 `with`/CM                | EXP P3, SYNTAX SP-4/7       | **Partial**     | `defer` exists (parser:1126); no `with expr as x {}` enter/exit protocol                           |
| 11 Drop/RAII                | EXP P3 (trait), DATA traits | **Partial**     | `defer` exists; no `Drop` trait scope-exit hook                                                    |
| 12 `const fn`/comptime      | EXP P4, SYNTAX SP-14/15     | **Partial**     | `const` keyword exists; no `const fn`, no full const-eval, no `comptime`                           |
| 13 big_int                  | DATA P3, STD P8             | **No**          | `VK::BigInt` not in `value.h`                                                                      |
| 14 complex/rational/decimal | DATA P2/4/5                 | **No**          | `VK::Complex/Rational/Decimal` not in `value.h`                                                    |
| 15 bytes/bytearray          | DATA P6, STD P4a            | **Partial**     | `VK::Bytes` tag exists but **no `bytes` type spelling** (`checker.cpp` gap, DATA:81)               |
| 16 deque/Counter/heap       | DATA P10, STD P2c           | **No**          | not in `value.h` / `collections.kn`                                                                |
| 17 mutex/waitgroup/atomic   | DATA P12, STD §7, PLAN P14  | **Partial**     | `chan`+`spawn` exist; no `sync` mutex/rwlock/waitgroup/atomic                                      |
| 18 reflect                  | DATA P14, EXP P7, PLAN P5   | **No**          | no `reflect` module / `type_name` in `runtime.cpp`                                                 |
| 19 typeof                   | SYNTAX SP-5, EXP P7, WHY-2  | **No**          | no `typeof` builtin                                                                                |

**Net verdict:** all 19 are surfaced as _planned_ somewhere, but **zero are fully implemented** in
`src/`. The **genuinely new** work is: walrus, pipe, `??`/`??=`, `@` matmul, dict/set comps +
unpacking, `?.` chaining, smart casts, `with`, `Drop`, `const fn`/comptime, big_int, complex/
rational/decimal, bytes spelling+methods, deque/Counter/heap, `sync` primitives, `reflect`, and
`typeof`. The **explicitly rejected** work is `++/--` and `?:` ternary (ratified below so they stay
out).

**Implementation progress (2026-10-13).** M1, M2 and M3 are implemented in `src/` and green on the
full gate (`scripts/gates_linux.sh`, 217), golden (`scripts/golden_linux.sh`, 224) and stdlib
(`kirn test stdlib/pet`, 9) suites, so rows 1, 2, 3, 6, 7, 8, 9 and 19 above are now done. The
audited table is kept as the historical baseline. Recorded deviations from the M3 text: safe
navigation is spelled `.?[` / `.?(` (not `?.[` / `?.()`), and `TypeGuard` is spelled as a
generic return type `fn f(x: any) -> TypeGuard[T]` (the plan only said "TypeGuard-like
annotation"). `typeof` is now on the VM builtin allowlist (`src/vm/compiler.cpp`), so the value
form runs on the bytecode VM instead of falling back to the interpreter. The M1 walrus operator,
`?.` safe navigation, and set/dict comprehensions now lower through HIR/MIR via an eval-once
`Let` expression (`payload::Let`, `ExprKind::Let`) plus an extended comprehension payload
(`payload::Comp::value`, `ExprKind::SetComp`/`DictComp`); the scalar-native backend also lowers
walrus and pipe. The bytecode VM now fully desugars the sugar — walrus, pipe (with `%`), elvis
`??`/`??=`, and `.?[`/`.?(` — using hidden frame locals for eval-once, and lowers all
comprehensions (`list`/`set`/`dict`/generator, with `if` filters and nested `for`) to a loop that
builds the collection with a new `OP_APPEND` opcode (a=0 list / 1 set / 2 dict; sets dedupe and
dicts are last-wins to match the tree-walker). A function containing a comprehension runs on the
env path rather than frame slots, since clause bindings are comprehension-scoped. Still unlowered
in HIR/MIR: elvis `??`/`??=` operands are optionals, which the scalar-native subset cannot
represent, and `*`/`**` display unpacking (`ExKind::Spread`) needs collection-unpack runtime ops
the MIR/LLVM lowering does not have. Native keeps these on the interpreter/VM, so behaviour is
unchanged. See the per-phase notes below.

**Implementation progress (2026-10-15).** M4 (`with`/context managers, `using`/scoped `Drop`,
`Enter`/`Exit` protocol) is implemented in `src/` (parser → AST → checker → tree-walker, with HIR
lowering and a bytecode-VM interpreted fallback) and green on the full gate (`scripts/gates_linux.sh`,
225), golden (`scripts/golden_linux.sh`, 227), crosscheck (`scripts/crosscheck_linux.sh`, 543) and
stdlib (`kirn test stdlib/pet`, 9) suites. See the M4 phase note for the recorded scope deviations
(no implicit declaration-based RAII, no `drop(value)` builtin, no `contextlib.kn`, no `Copy`/`Drop`
conflict check).

**Implementation progress (2026-10-16).** M5 (`const fn`, compile-time constant evaluation,
`@comptime(expr)` / `comptime { }`) is implemented in `src/sema/checker.{h,cpp}` as a pure AST
interpreter (`ConstVal`) that folds arithmetic, comparisons, `and`/`or`, conditionals, calls to other
`const fn`s (with recursion and a step budget), list/tuple displays, indexing, bounded comprehensions
over ranges, and a small set of pure builtins (`abs`/`min`/`max`/`len`/`Int`/`Float`/`Bool`/`Ord`/
`Chr`/`Pow`/`sum`). A `const NAME = <evaluable>` initializer is folded to a literal (so native/codegen
sees a constant and the `--release` call is eliminated); `const fn` bodies are structurally checked and
a call to a runtime function or a runtime-only statement reports `E-CONST-IMPURE`; `@comptime(expr)`
and `comptime { }` force compile-time evaluation and report `E-CONST-IMPURE` when the operand is not
constant. Executable paths treat `@comptime(expr)` as its operand and `comptime { }` as an ordinary
scoped block (HIR lowers both), so behaviour is unchanged on the tree-walker and the VM. Green on the
full gate (`scripts/gates_linux.sh`, 230), golden (`scripts/golden_linux.sh`, 230), crosscheck
(`scripts/crosscheck_linux.sh`, 545+) and stdlib (`kirn test stdlib/pet`, 9) suites. Recorded scope
deviations: const-eval supports scalars, lists/tuples, and bounded comprehensions but not
dicts/sets/structs, method calls, string methods, or I/O; `sizeof`/`alignof` are not builtins, so the
plan's `@comptime(alignof(Point))` form is out of scope for this stage; the constant table is not
emitted as a separate data section (folding rewrites the AST literal instead).

**Implementation progress (2026-10-16).** M6 (extended numeric types) is implemented in `src/`
(runtime values + checker); `big_int`/`rational` already existed, so the new work is `complex` and
`decimal` plus the promotion lattice `Integral < Rational < Real < Complex`. Green on the full gate
(`scripts/gates_linux.sh`, 236), golden (`scripts/golden_linux.sh`, 233), crosscheck
(`scripts/crosscheck_linux.sh`, 555) and stdlib (`kirn test stdlib/pet`, 9) suites. See the M6 phase
note for the recorded scope deviations (different transcendental path for `decimal`, no `stdlib/lib`
modules, property vs method accessors).

**Implementation progress (2026-10-16).** M7 (bytes/bytearray + buffer protocol) is implemented in
`src/` (runtime values + checker). `bytes`/`b"..."` already existed; the new work is the mutable
`bytearray`, the zero-copy `memoryview` view, the buffer `VK::ByteArray`/`VK::View` values, Int
indexing, `+`/`*`/`in`, and the buffer method surface. Green on the full gate
(`scripts/gates_linux.sh`, 242), golden (`scripts/golden_linux.sh`, 236), crosscheck
(`scripts/crosscheck_linux.sh`, 561) and stdlib (`kirn test stdlib/pet`, 9) suites. See the M7 phase
note for the recorded scope deviations (no `stdlib/lib` module).

**Implementation progress (2026-10-16).** M8 (Deque / Counter / heap + collection free-functions) is
implemented in `src/` (runtime values + checker). The new work is `VK::Deque`/`VK::Counter`, their
method surfaces, the min/max binary-heap helpers, and the free functions
`group_by`/`chunk`/`flatten`/`count`/`find`/`binary_search`/`partition`. Green on the full gate
(`scripts/gates_linux.sh`, 242), golden (`scripts/golden_linux.sh`, 239), crosscheck
(`scripts/crosscheck_linux.sh`, 561) and stdlib (`kirn test stdlib/pet`, 9) suites. See the M8 phase
note for the recorded scope deviations (no `stdlib/lib` module; builtins are shadowable).

**Implementation progress (2026-10-16).** M9 (`sync` module: mutex/waitgroup/atomic) is implemented
in `src/` (runtime values + checker). The new work is `VK::Mutex`/`RwMutex`/`WaitGroup`/`Once`/
`Cond`/`Atomic` sharing one `SyncImpl` allocation, their method surfaces, `sync.Atomic[T](v)` /
`Atomic[T](v)` construction, `WaitGroup.go` = spawn + add, and the `sync` pseudo-module. Green on
the full gate (`scripts/gates_linux.sh`, 251), golden (`scripts/golden_linux.sh`, 242), crosscheck
(`scripts/crosscheck_linux.sh`, 573) and stdlib (`kirn test stdlib/pet`, 9) suites. See the M9 phase
note for the recorded scope deviations.

**Implementation progress (2026-10-16).** M10 (`reflect` module: runtime introspection) is implemented
in `src/interp/runtime.cpp` (runtime metadata over `VK`/the struct/impl tables). The new work is the
`reflect` pseudo-module — `type`/`kind_of`/`fields`/`field_names`/`methods`/`is_of_type`/`get_field`/
`set_field`/`has_field`/`deep_equal`/`new`/`sizeof` — backed by two new helpers (`kindName`,
`typeRefName`) and the existing `valEq` deep-equality (`typeof` is the same name query). Verified:
gates 255, golden 245, crosscheck 579, stdlib 9, all 0 fail. See the M10 phase note for recorded
scope deviations (no `stdlib/lib` module; `reflect.new` builds structs with type-based zero defaults).

**Implementation progress (2026-10-16).** M11 (native AOT & polish for the new surface) is implemented
for the tractable scalar core. `src/backend/native.cpp` now lowers scalar optionals (`T?` →
`std::optional<scalar>`) and elvis `??` alongside the existing scalar/walrus/pipe surface; a codegen
dump hook in `src/driver/driver.cpp` (`KIRN_DUMP_NATIVE=<path>`) writes the emitted C++ and returns
before the toolchain, and a new `check_codegen` stage in `scripts/golden_linux.sh` (plus gate 11 in
`scripts/gates_linux.sh`) snapshots and differentially tests it. Tests added:
`tests/codegen/{scalar_core,new_surface,reject_non_scalar}.kn` and `examples/61_native_new_surface.kn`.
Verified: gates 261, golden 251, crosscheck 579, stdlib 9, all 0 fail. See the M11 phase note for the
recorded deviations (optional params, `@` matmul, safe-chain, comprehensions, and `const fn` folding
remain deferred).

---

## 2. Ownership map — the 19 features × the six owner plans

> Add this to the `DO_FIRST_PLAN.md` ownership table (→ `ownership map`, DO_FIRST:537-555) as the
> "missing features" row so future edits land in one place.

**Goal:** assign each gap one owning spec + one executable phase so ownership never drifts.
**Must Follow:** `[AMEND]` means this plan only corrects the owner doc — it never forks its surface.
**Important Note:** rejected rows carry no owner; they stay closed.

| #   | Feature                  | **Owner** (spec)             | Syntax/grammar                | Types/VK                       | Module API                      | Execution |
| --- | ------------------------ | ---------------------------- | ----------------------------- | ------------------------------ | ------------------------------- | --------- |
| 1   | Walrus `:=`              | **MISSING_PLAN** P1          | `SYNTAX SP-2` [AMEND §prec]   | `EXP P5`                       | —                               | this plan |
| 2   | Pipe `\|>`               | **MISSING_PLAN** P1          | `SYNTAX SP-*` _(new)_         | `EXP P5` [AMEND §model]        | —                               | this plan |
| 3   | Elvis `??`/`??=`         | **MISSING_PLAN** P1          | `SYNTAX SP-4`                 | `EXP P6`                       | —                               | this plan |
| 4   | `++`/`--`                | rejected (PLAN §5.4, EXP §2) | —                             | —                              | —                               | —         |
| 5   | Ternary `?:`             | rejected (EXP §A4.9)         | —                             | —                              | —                               | —         |
| 6   | `@` matmul               | **MISSING_PLAN** P2          | `EXP P13`                     | `DATA P9` (Matrix)             | —                               | this plan |
| 7   | Dict/set comps           | **MISSING_PLAN** P3          | `SYNTAX SP-9` [AMEND unpack]  | `ast.h CompClause`             | —                               | this plan |
| 8   | `?.` chain               | **MISSING_PLAN** P3          | `SYNTAX SP-4` [AMEND chain]   | `EXP P6`                       | —                               | this plan |
| 9   | Smart casts              | **MISSING_PLAN** P3          | `SYNTAX SP-5` [AMEND lattice] | `EXP P6/P8`                    | —                               | this plan |
| 10  | `with`/CM                | **MISSING_PLAN** P4          | `EXP P3`                      | protocol `Enter/Exit`          | `contextlib` `STD`              | this plan |
| 11  | Drop/RAII                | **MISSING_PLAN** P4          | `EXP P3` (`Drop` trait)       | trait table                    | —                               | this plan |
| 12  | `const fn`/comptime      | **MISSING_PLAN** P5          | `EXP P4`, `SYNTAX SP-14/15`   | `@pure`/const-eval             | —                               | this plan |
| 13  | big_int                  | `DATA P3`                    | `EXP P8`                      | `VK::BigInt`                   | `lib.bigint` `STD P8`           | this plan |
| 14  | complex/rational/decimal | `DATA P2/4/5`                | `EXP P8`                      | `VK::Complex/Rational/Decimal` | `STD` math                      | this plan |
| 15  | bytes/bytearray          | `DATA P6`                    | `SYNTAX SP-* b"..."`          | `VK::Bytes`+spelling           | `lib.bytes` `STD P4a`           | this plan |
| 16  | deque/Counter/heap       | `DATA P10`                   | —                             | `VK::Deque/Counter/Heap`       | `lib.collections` `STD P2c`     | this plan |
| 17  | mutex/waitgroup/atomic   | `DATA P12`                   | —                             | `VK::Mutex/Atomic/…`           | `lib.sync` `SYNTAX §7`+`STD §7` | this plan |
| 18  | reflect                  | `DATA P14`                   | `SYNTAX SP-5/6`               | type metadata                  | `lib.reflect`                   | this plan |
| 19  | typeof                   | **MISSING_PLAN** P3          | `SYNTAX SP-5`                 | `EXP P7`                       | —                               | this plan |

`[AMEND]` marks the sections this document **corrects or completes** in the owner docs (see §4).

---

## 3. Canonical semantics digest (the "single source of truth" this plan consolidates)

> One-line canonical semantics per feature, distilled from the structured reference + verified
> research. These are the _requirements an implementation must satisfy_; the owner plan provides the
> AST/VM/type detail.

**Goal:** pin one canonical semantic per feature so an implementer never re-derives it from six docs.
**Must Follow:** owner plans keep the AST/VM/type detail; this digest fixes only observable behavior.
**Important Note:** these are requirements, not API inventories — cite the owner for surface.

**Operators (P1–P2):**

1. **Walrus `:=`** — `NAME := expr` evaluates `expr`, binds `NAME` in the enclosing scope, yields the
   value (Python PEP 572). Precedence: **between `or` and `,`** — binds looser than `or`, tighter than
   comma; unusable as a bare statement (assignment `=` stays statement-only). Legal in `if`/`while`
   conds, `match` scrutinee, comprehensions (binds in _containing_ scope), and as a parenthesized
   sub-expression. No augmented `:=+=`. _(Precedence per PEP 572, not the raw `SP-2` note — see §4._)
2. **Pipe `\|>`** — **model TBD (ratify §4):** (a) _inverted-invocation_ `x |> f` == `f(x)` with a
   unary callable (PHP 8.5 single-arg model, shipped Nov 2025 — always passes as **first/only**
   arg, no placeholder), or (b) _Hack/TC39 placeholder_ `x |> f(%) |> g(%, 0)` where `%` names the
   topic and must appear ≥ once. Left-associative, very low precedence (above assignment). PHP's
   shipped form has zero call overhead for direct calls; TC39 Hack remains Stage 2.
3. **Elvis `??` / `??=`** — `a ?? b` ⇒ if `a` is non-`nil` return `a` else evaluate+return `b`
   (short-circuits, right-associative, chains `a ?? b ?? c`). `a ??= b` assigns `b` only if `a` is
   `nil`. Precedence: lower than `||`, and per JS **cannot mix with `&&`/`||` without parens**
   (adopt the no-mixing rule). Binds tightly enough that `a?.b ?? default` composes.
4. **`++`/`--`** — **rejected.** Use `+= 1` / `-= 1`. (Rust/Go/Python all omit; source of
   precedence/evaluation-order bugs in C.)
5. **Ternary `?:`** — **rejected as a new token.** Kirn already has the conditional-expression form
   `if c { a } else { b }` (see §2 "rejected"). Do **not** add `?:`; document the existing form. _(If
   a future edition wants it, it is C-class precedence, just above assignment — noted but not built._)
6. **`@` matrix multiply** — `a @ b` ⇒ matrix product, **same precedence/associativity as `*`**,
   via operator-trait `MatMul` / `__matmul__` (+ `@=` `__imatmul__`). Scalars raise; 1-d vectors
   auto-promote (`mat @ vec` → 1-d, `vec @ vec` → scalar); higher-d broadcast across leading dims
   (NumPy semantics; NumPy 2.0 deprecated `numpy.matrix`).

**Collections & access (P3):** 7. **Dict/set comprehensions** — `{k: v for x in seq if c}` (dict), `{expr for x in seq if c}`
(set). **Key evaluated before value** (PEP 572 counter). Nested `for`/`if`; walrus allowed;
**`*/**` unpacking inside comps** (`[*it for it in its]`, `{**d for d in dicts}`) per **PEP 798
(Final, Python 3.15)** — extend `CompClause` (`ast.h:95`). 8. **Optional chain `?.`** — `a?.b` (attr), `a?.[i]` (index), `a?.()` (call), and chaining
`a?.b?.c`; if any receiver is `nil` the whole chain short-circuits to `nil`. Extend the existing
`.?.` (`kOps3`) to chaining forms. Composes with `??`: `a?.b?.c ?? default`. 9. **Smart casts / narrowing** — after `if (x is T)` narrow `x` to `T` in the branch (Kotlin-style);
user-defined **type guards** via a `TypeGuard`-like annotation; narrowing via `is`, equality,
`in`, pattern `if var`; require **stable bindings** (immutable locals / no getters). Track a
positive/negative type **lattice** (`P`,`N`) like Kotlin KEEP-0442 so exhaustiveness uses
negative info on sealed hierarchies. Conservative default; `--strict` requires soundness. 10. **`with` / context managers** — `with EXPR as VAR: BLOCK` → `Enter() -> T` (bind `as`),
`Exit(exc_type, exc_value, traceback) -> bool` (True suppresses), desugar to `try/finally`;
multiple items `with A, B:` (enter A then B, exit reversed); `using name = expr` declaration form
auto-closes at scope end; async variants optional. Provide `contextlib`-style utilities
(`ExitStack`, `suppress`, `closing`, `nullcontext`). 11. **Drop / RAII** — `Drop` trait with `fn drop(self)` **called implicitly at scope exit**, cannot be
called directly (provide `drop(value)` builtin for early drop); `Copy` and `Drop` mutually
exclusive. This is the deterministic RAII; `defer` (Go-style) and `with` (Python-style) coexist.

**Compile-time (P5):** 12. **`const fn` / const eval / comptime** — functions evaluable at compile time in const contexts;
`const { … }` inline blocks; **const generics** (`struct Vec[const N]`); progressive relaxation
(arithmetic first, then control flow, then limited allocation); optional `comptime`/`@comptime`
for compile-time-only code and (later) compile-time reflection (Rust 2026 roadmap goals:
const traits, struct/enum const params, comptime reflection).

**Numeric types (P6):** 13. **big_int** — arbitrary-precision integers, `VK::BigInt` sign+limbs (`DATA P3`). Kirn keeps
fixed-width `Int` default + `big_int` opt-in (see DATA §3.7 pragmatism, line 54). Methods:
`bit_length`, `bit_count`, `is_prime` (Miller-Rabin), `powmod`, `gcd`, `to_bytes/from_bytes`,
`Str(x, base 2..36)`. 14. **complex / rational / decimal** — `complex(re, im)`, `.real/.imag/.conjugate()`, full
arithmetic (ANSI C Annex G semantics, `DATA P4`); `rational(num, den)` auto-reduces,
`.numerator/.denominator` (`DATA P2`); `decimal("0.1")` context precision, money-safe, does
**not** auto-promote to float (`DATA P5`). Promote along `Integral < Rational < Real < Complex`.

**Byte & collections types (P6–P7):** 15. **bytes / bytearray** — `bytes` immutable+hashable, `bytearray` mutable+unhashable; `b"..."`
literal; indexing returns **int**; buffer protocol + `memoryview` zero-copy view; fix the missing
`bytes` type spelling (`DATA P6`; `checker.cpp` / `VK::Bytes`). 16. **deque / Counter / heap** — `deque` (O(1) both ends, `maxlen`), `Counter` (missing→0,
`most_common`, `total()`, `+/-/|/&`), `heap` (min + max-heap variants, Python 3.14+ `heappush_max`).

**Concurrency (P7):** 17. **mutex / waitgroup / atomic** — `sync` module: `Mutex` (lock/unlock/try_lock), `RwMutex`
(RLock/RUnlock/Lock/Unlock), `WaitGroup` (add/done/**go()**/wait — Go 1.25 `Go()`), `Once`
(run exactly once), `Cond`, `Atomic<T>` (load/store/swap/compare_and_swap/add, sequential
consistency); `spawn`/`chan` already exist.

**Introspection (P3/P7):** 18. **reflect** — runtime `type(x)`, `fields(x)`, `methods(t)`, `kind_of`, `variants`, `get/set_field`,
`deep_equal`, `new(name, args)`; compile-time reflection later (Rust-2026-style). Backs
serialization & decorators (`DATA P14`, `PLAN P5.2`). 19. **typeof** — `typeof(expr)` returns a runtime type descriptor usable both as a value and in type
position (`var x: typeof(get_value()) = …`); sugar over `reflect.type`. (`SYNTAX SP-5`.)

---

## 4. Ratification — the decisions this plan must settle before coding

> Confirmed against the **canonical semantics** (§3) and the **shipping facts** (Appendix R).
> Existing decisions are reaffirmed; genuinely open ones are settled here.

**Goal:** settle the genuinely open decisions (R1–R10) before any code is written.
**Must Follow:** reaffirm existing choices unless a shipping reference contradicts them.
**Important Note:** R1's placeholder model overrides EXP P5's `_`; record every ruling as an `[AMEND]`.

| #   | Decision                    | Existing plan                                  | **Recommended (this plan)**                                                                                                                     | Rationale                                                                                                                                                                                                                                                              |
| --- | --------------------------- | ---------------------------------------------- | ----------------------------------------------------------------------------------------------------------------------------------------------- | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| R1  | **Pipe model**              | EXP P5: `x\|>f\|>g`→`g(f(x))`, `_` placeholder | **Adopt Hack/TC39 placeholder `%` model** (`x \|> f(%) \|> g(%, 0)`), **plus** unary sugar `x \|> f` == `x \|> f(%)`.                           | Highest flexibility (any arg position); C++ P2672R0 recommended placeholder; matches the structured reference. PHP's shipped form is single-arg-only and cannot reorder args — too restrictive as the _primary_ model. **Override EXP P5's `_` placeholder with `%`.** |
| R2  | Walrus precedence           | SYNTAX SP-2: "lowest, just above `or`"         | **PEP 572 exact:** binds **between `or` and comma** (looser than `or`, tighter than comma).                                                     | SP-2's "just above or" is ambiguous; PEP 572 is precise. Ratify token `:=` in `kOps2`.                                                                                                                                                                                 |
| R3  | `??`/`&&`/`\|\|` mixing     | EXP P6 silent                                  | **No mixing without parens** (JS ES2020 rule).                                                                                                  | Prevents the classic precedence bug.                                                                                                                                                                                                                                   |
| R4  | `++`/`--`                   | Rejected (PLAN §5.4, EXP §2)                   | **Keep rejected.** Document `x += 1`.                                                                                                           | Rust/Go/Python omit; C bug source.                                                                                                                                                                                                                                     |
| R5  | `?:` ternary                | Rejected (EXP §A4.9)                           | **Keep rejected.** `if c { a } else { b }` is the conditional.                                                                                  | Already decided; avoid token/ambiguity churn.                                                                                                                                                                                                                          |
| R6  | `b"..."` literal            | SYNTAX SP-* silent                             | **Add `b"..."`** for `bytes` (immutable) alongside the `bytes()`/`bytearray()` ctors.                                                           | Canonical Python surface; pairs DATA P6.                                                                                                                                                                                                                               |
| R7  | `Drop` vs `with` vs `defer` | EXP P3 says `Drop` + `defer`                   | **Ship all three** with clearly separated roles: `Drop`=type-scoped RAII, `defer`=function-scope (exists), `with`=block-scoped context manager. | They serve different scopes; all are requested.                                                                                                                                                                                                                        |
| R8  | `with` keyword              | SYNTAX SP-4/7: "keyword?"                      | **Soft/contextual keyword** `with` (statement position), never reserved.                                                                        | Frozen-keyword constraint (lexer:39-46); backward-compat.                                                                                                                                                                                                              |
| R9  | `const fn` vs `comptime`    | EXP P4 + SYNTAX SP-14/15 both                  | **`const fn` first** (EXP P4 as-is), then `@comptime`/`comptime` blocks (SP-14/15) gated behind const-eval maturity.                            | Progressive relaxation per canonical §3.12.                                                                                                                                                                                                                            |
| R10 | `Int` default vs `big_int`  | DATA P3 (keeps both)                           | **Keep `Int` default; `big_int` opt-in.** No automatic silent promotion (perf + FFI safety).                                                    | DATA §3 rule #5.                                                                                                                                                                                                                                                       |

These ratifications should be applied **back** to the owner docs as `[AMEND]` markers (SP-2
precedence, EXP P5 pipe model, SP-4 chain + `??` no-mixing).

---

## 5. Phased implementation roadmap

> Phases are ordered for **value-per-effort + dependency**, matching the repo's style
> (`Goal / Files / Architectural changes / Steps / Syntax / Compiler-runtime / Toolchain / Error
handling / Security / Cross-platform / Examples / Testing / Expected result`). Each phase groups
> features with shared machinery (operators → one lexer/checker change; types → one `VK`/`TyK`
> extension). Phases build on `PLAN P1–P4` (diagnostics, lints, bytecode VM) and `DO_FIRST P1–P4`.

**Goal:** sequence the ledger into phases an agent can execute end-to-end.
**Must Follow:** one phase per shared-machinery cluster; each phase ships with `kirn test` + `kirn bench`.
**Important Note:** phase order is value-per-effort + dependency, not the feature-number order.

### Phase M1 — Expression operators: walrus `:=`, pipe `|>`, elvis `??`/`??=` _(features 1,2,3)_

**Goal:** land the three high-value expression operators as pure sugar that lowers to existing AST/VM,
zero new runtime cost.

**Must Follow:** pure desugaring to existing AST/VM — no new opcodes, no runtime cost.
**Important Note:** PHP 8.5's shipped `|>` is single-arg only, so the `%` placeholder (R1) stays primary.

> Reference: Python — assignment expressions `:=` (PEP 572, `cpython/Grammar/python.gram`); PHP 8.5 — pipe `|>`.

**Files:** `src/lex/lexer.cpp` (`kOps2` += `:=`, `|>`, `??`, `??=`), `src/parser/parser.cpp`
(pratt/precedence table + `parseWalrus`, `parsePipe`, `parseElvis`), `src/ast/ast.h`
(`ExKind::AssignExpr`, `ExKind::Pipe`, `ExKind::Coalesce`, `ExKind::CoalesceAssign`),
`src/sema/checker.cpp` (types: walrus binds+uses, pipe result type = callee return, elvis unification
with `nil`/`T?`), `src/interp/runtime.cpp` (eval for the three new kinds; `lvaluePtr`/`assignTo` for
`??=`), `grammar/kirn.ebnf` (operator table update).

**Architectural changes:**

- Walrus `(x := E)` lowers to evaluate `E` → store into `x` → push `x`'s value (a store+load pair),
  reusing `assignTo`. Mark the `:=`-bound symbol `used` in the checker so it isn't linted W0102.
- Pipe `a |> stmt` lowers per the ratified model (R1): with placeholder support, rewrite to a call
  substituting `%`; without placeholders, `x |> f(%)`. `%` is a contextually-scoped topic token legal
  only inside a pipe body (lexer/parser-recognized after `|>`).
- Elvis `a ?? b` lowers to `if a is none { b } else { a }`; `??=` lowers to
  `if a is none { a = b }` (only then assigns). Short-circuit via existing branch/guard.

**Syntax examples:**

```kirn
# walrus — precedence: looser than or, tighter than comma
while (line := reader.read()) != none { process(line); }
if (n := len(xs)) > 10 { print(n); }
squares = [y := x*x, y + 1 for x in 0..10];   # binds y in enclosing scope

# pipe — placeholder model + unary sugar
slug = name |> lower(%) |> replace(" ", "-");
out = xs |> filter(%, fn(v){ v > 0 }) |> map(%, fn(v){ v*2 });
# PHP-style unary chain also works:
n = 5 |> increment(%) |> square(%);      # == square(increment(5))

# elvis
name = user?.name ?? "anonymous";
cache ??= compute_expensive();           # assign only if cache is none
a = x ?? y ?? z;                          # right-assoc chain
```

**Error handling:** walrus at statement-start still a statement (mirror Python; parser lookahead);
`%` used outside a pipe → `E-PIPE-TOPIC`; `??` mixed with `&&`/`||` unparenthesized → `E-COALESCE-MIX`
(note with fix-it to parenthesize); `??=` on a non-assignable target → checker error.

**Security/cross-platform:** no new I/O or platform surface; all three are pure expression sugar that
lower to existing ops (perf-neutral; native AOT `native.cpp` lowers them as folded calls once the
scalar path covers them).

**Testing:** `tests/syntax/expr_ops_*.kn` positive/negative; `examples/51_expr_ops.kn`; differential
VM ≡ tree-walker; corpus green (≥ current count).

**Expected result:** all three operators parse/check/run on tree-walker + VM with identical output;
precedence and short-circuit verified; `native_main.kn` still green.

---

### Phase M2 — Matrix multiply `@` _(feature 6)_

**Goal:** `a @ b` matrix product via the operator-trait machinery.

**Must Follow:** route through the `MatMul`/`__matmul__` trait; `@` keeps `*` precedence.
**Important Note:** scalars raise `E-MATMUL-SCALAR`; 1-d↔2-d promotion follows NumPy semantics
(a numeric operand is rejected even if the other side defines `__rmatmul__`, so this scalar rule
takes precedence over PEP 465's reflected-of-scalar case).

> Reference: Python — `@` matmul / `__matmul__` (PEP 465).

**Files:** `src/lex/lexer.cpp` (`@=` token), `src/parser/parser.cpp` (`@` at `*`
precedence, `@` augment), `src/sema/checker.cpp` (type `@`/`@=`; route to the trait),
`src/interp/runtime.cpp` (`binop` `@` case, `@=` augmented), `grammar/kirn.ebnf`.
`Matrix` is an ordinary library struct (there is no core `Matrix` type: `DATA P9` /
`DATA_TYPE_PLAN.md` has no Matrix, so the op enum / core-type files originally listed
here do not apply — ops are strings on the AST and structs are user-defined).

**Architectural changes:** reuse the operator-trait dispatch (per `EXP P13`). `@` lowers like `*` with
the `matmul` dunder (alias `__matmul__`), plus PEP-465 reflected `__rmatmul__`; `@=` calls
`__imatmul__` when defined and otherwise falls back to `a = a @ b` (so `@=` functions are marked
interpreter-only in `src/vm/compiler.cpp`, like `??=`). Scalar operands raise a dedicated
`E-MATMUL-SCALAR`; 1-d↔2-d auto-promotion follows NumPy semantics and is handled inside the `MatMul`
implementation.

**Syntax/example:**

```kirn
impl MatMul for Matrix { fn matmul(self, o: Matrix) -> any { ... } }
c = a @ b;
c @= scale;   # __imatmul__ if present, else c = c @ scale
v = m @ vec;          # matrix @ vector -> 1-d
dot = v1 @ v2;        # vector @ vector -> scalar
```

**Testing:** scalar-error negative; promotion cases; `@=`; a `Matrix` example `examples/46_matmul.kn`
(the plan's `52` slot is stale).

**Expected result:** `@` runs for Matrix/vector types with PEP-465-consistent precedence/associativity.

---

### Phase M3 — Collections & access sugar: dict/set comps + `*/**` unpacking, `?.` chaining, smart casts, `typeof` _(features 7,8,9,19)_

**Goal:** the Python/TS ergonomics bundle — richer comprehension builders, safe-call chaining, and
type narrowing + `typeof`.

**Must Follow:** extend the existing `ListComp`/`CompClause` and `.?.` paths — no duplicate builders.
**Important Note:** smart casts only on stable bindings; `typeof` delegates to `reflect.type`.

> Reference: Python — comprehensions + PEP 798 unpacking; Kotlin — smart casts (KEEP-0442).

**Files:** `src/parser/parser.cpp` (dict/set comp grammar; `*.`/`*[`/`*()` chained; `typeof`),
`src/ast/ast.h` (`ExKind::DictComp`/`SetComp`, extend `CompClause` for `*`/`**`, `ExKind::SafeIndex`,
`ExKind::SafeCall`, `ExKind::TypeOf`), `src/sema/checker.cpp` (comp type inference; **smart-cast
lattice**: track `P`/`N` per branch after `is`; `typeof` returns `reflect.type`), `src/interp/
runtime.cpp` (comp builders; safe-chain eval; type-guard caching), `SYNTAX SP-9/SP-5`, `EXP P7`.

**Architectural changes:**

- **Dict/set comps** extend the existing `ListComp`/`CompClause` path (`ast.h:95,158`): `{k:v for …}`
  and `{e for …}` reuse `iterateSeq`. **Unpacking** `[*it for it in its]` / `{**d for d in dicts}` per
  PEP 798 replaces `append` with `extend`/`update` (exactly the PEP's lowering). Key-before-value.
- **`?.` chaining** extends the existing `.?.` member form (`kOps3`) to `a?.[i]` and `a?.()`; each
  link tests `nil` and, if so, yields `nil` (do not evaluate the rest). Lower to nested `if` guards.
- **Smart casts:** after `if (x is T)`/`case`, the checker narrows `x:T` in the branch via a
  `positive/negative` lattice (Kotlin KEEP-0442); only for **stable bindings** (immutable locals, no
  user getters). User type-guard functions annotated `TypeGuard` narrow call sites. Conservative in
  default mode; sound under `--strict`.
- **`typeof(expr)`**: return a type descriptor (string + structured descriptor for type-position use);
  implemented via `reflect.type` (faces `DATA P14`).

**Syntax/example:**

```kirn
squares = {x: x*x for x in 0..10};            # dict comp
evens   = {x for x in 0..20 if x % 2 == 0};   # set comp
merged  = {**cfg_default, **cfg_user};        # dict unpacking comp (PEP 798)
flat    = [*batch for batch in batches];      # list unpacking comp

name = user?.profile?.name ?? "anon";          # chained safe-call + elvis
first = xs?.[0];
res   = obj?.compute?();

# smart cast
if (v is Str) { print(v.upper()); }           # v narrowed to Str in branch
if var Some(n) = opt { print(n + 1); }        # if-var narrowing

var t: typeof(get_value()) = get_value();     # typeof in type position
print(typeof(5));                             # "int"
```

**Error handling:** smart-cast on a mutable/non-stable binding → W-narrow (or `--strict` error);
`unsafe` field access after narrowing to a finalized type; unpacking `*` in a dict value / `**` in a
list → dedicated syntax error (mirror PEP 798 messages).

**Testing:** `tests/syntax/comprehensions_*.kn`, `tests/syntax/safecall_*.kn`,
`tests/syntax/narrow_*.kn`, `tests/syntax/typeof_*.kn`; negatives; `examples/53_collection_ops.kn`;
differential VM ≡ tree-walker; corpus green.

**Expected result:** dict/set comps (with `*/**`), `?.`/`?.[i]`/`?.()`, branch narrowing, and `typeof`
all run; existing behavior unchanged.

**Implemented (2026-10-13).** Landed with the decided semantics: dict/set comps reuse the
`ListComp`/`CompClause` path with key-before-value for `{k: v for …}`; `*`/`**` unpacking follows
PEP 448/798 in list/set/dict literals and comprehensions; safe navigation is spelled `.?[` (index)
and `.?(` (call) alongside the existing `.?.` member form; narrowing is a full P/N lattice over
`is none`/`== none`/`is T` with `&&`/`||`/`and`/`or`/`not` and early-exit, emitting `W-narrow`
for `var` bindings (no TypeGuard yet); `typeof(expr)` works in value and type position. Sets are
true sets and dict duplicate keys are last-wins on every construction path. Tests:
`tests/syntax/comprehensions_ok`, `comprehension_spread_ok`, `safe_nav_ok`, `narrow_ok`,
`typeof_type_ok` and `*_err_*` negatives; `tests/vm_parity/collections.kn`;
`examples/47_collection_ops.kn`. HIR/MIR/LLVM do not lower the new expression kinds (they emit
well-formed `Error` nodes); that is deferred to M11, as the interpreter and bytecode VM are the
execution paths for this surface.

---

### Phase M4 — Resource management: `with`/context managers, `Drop`/RAII _(features 10,11)_

**Goal:** deterministic, composable resource cleanup via three complementary mechanisms: type-scoped
`Drop` (RAII), block-scoped `with` (context-manager protocol), and the existing function-scoped
`defer`.

**Must Follow:** `Drop` = type-scoped RAII, `with` = block-scoped CM, `defer` = function-scoped.
**Important Note:** `Exit` suppression and reversed exit order match PEP 343; cleanup runs on exception.

**Implemented (2026-10-15).** `with`/`using` plus the `Drop`/`Enter`/`Exit` protocol landed with the
decided semantics. `with e1 as a, e2 as b { }` evaluates items left-to-right, calls
`enter`/`__enter__` in order (the `as` name binds to the result), runs the body, then calls
`exit`/`__exit__` in reverse order on normal completion _and_ on a raised error; an `exit` returning
truthy suppresses the error (PEP 343 — and only `SignalRaise` is suppressed, not
`return`/`break`/`continue`). `using n = e;` declares `n` in the enclosing block and registers its
value for release at block exit (normal or unwind, LIFO) via `drop`, falling back to
`__del__`/`exit`/`__exit__` when `drop` is absent. The checker rejects a `with` receiver without the
protocol (`E-CTXMGR-NO-PROTOCOL`), a `using` value without a drop/exit method
(`E-DROP-NO-PROTOCOL`), and an `exit` whose return type is not `Bool`. `with` and `using` are
contextual keywords, not reserved. Recorded deviations from the M4 text: implicit
declaration-based RAII (a bare `{ h = FileHandle(...) }` dropping at block end) is **not**
implemented — release is driven by `using` (and `with`); the early-drop `drop(value)` builtin and the
`stdlib/lib/contextlib.kn` helpers (`ExitStack`, `suppress`, `closing`, `nullcontext`) are not
included yet; `Copy`/`Drop` mutual exclusion is not enforced. Parser/AST/checker/tree-walker are done;
`with`/`using` are marked interpreted in the bytecode VM and HIR lowers the bindings (`With` → a
scoped `Block`, `Using` → a `VarDecl`) so gates 8/9 stay green. Tests
`tests/syntax/context_drop_ok.kn`, `tests/syntax/ctxmgr_err_no_protocol.kn`,
`tests/syntax/drop_err_no_protocol.kn`, `tests/vm_parity/context_drop.kn` and
`examples/48_context_drop.kn`.

> Reference: Python — context managers `with` (`cpython/Lib/contextlib.py`); Rust — `Drop` (`rust/library/core/src/ops/drop.rs`).

**Files:** `src/parser/parser.cpp` (soft keyword `with`; `using name = expr`), `src/ast/ast.h`
(`StKind::With`, protocol bindings), `src/sema/checker.cpp` (`Enter`/`Exit`/`Drop` method resolution),
`src/interp/runtime.cpp` (scope-exit hook executing `drop`; `with` → `try/finally`; `Drop` registry),
`stdlib/lib/contextlib.kn` (new: `ExitStack`, `suppress`, `closing`, `nullcontext`), `EXP P3`.

**Architectural changes:**

- `with E as v: body` desugars to: `v = E.Enter()`; `try { body } finally { if error → Exit(err…)
else Exit(nil,nil,nil) }`; `Exit` returning true suppresses the exception (PEP 343). Multiple items
  `with A, B:` enter in order, exit reversed.
- `Drop` trait: a value whose type implements `Drop` runs `drop(self)` at end of the scope where it
  becomes unreachable (interpreter scope-exit hook; conservative for the GC/refcount model — runs on
  `del`/scope-exit, not non-deterministic). Cannot be called directly; `drop(value)` builtin for early
  drop. `Copy`/`Drop` are mutually exclusive (enforced by checker).
- `using name = expr` is declaration sugar for a block-scoped `Drop`/`with` auto-close (C#).

**Syntax/example:**

```kirn
with File.open("x.txt") as f { print(f.read()); }          # context manager
with Lock as l { l.acquire(); ... }                        # enter/exit protocol
with A, B:  ...                                            # enter A, exit B (reversed)

using conn = db.connect();                                 # auto-close at scope end

struct FileHandle { ... }
impl Drop for FileHandle { fn drop(self) { close(self.fd); } }
{ h = FileHandle(open("y.txt")); ... }                     # drop runs at block end
drop(h);                                                    # explicit early drop (Rust mem::drop)
```

**Error handling:** `Exit` return type not bool → checker error; `Drop`/`Copy` conflict; `with` on a
type without `Enter`/`Exit` → `E-CTXMGR-NO-PROTOCOL` with help listing required methods.

**Security:** cleanup runs even on exception (finally); no resource leak on `raise`.

**Testing:** `tests/syntax/with_*.kn`, `tests/syntax/drop_*.kn`; exception-in-body → `Exit` still runs;
`Exit`-suppress case; `examples/54_resources.kn`; corpus green.

**Expected result:** `with`, `using`, and `Drop` each run cleanup deterministically; order and
suppression semantics verified.

---

### Phase M5 — Compile-time: `const fn`, const-eval, `comptime`/`@comptime` _(feature 12)_

**Goal:** fold a real compile-time function-and-constant evaluator, then optional `comptime` blocks for
compile-time-only code (and later, compile-time reflection).

**Must Follow:** const eval is pure and progressive (arithmetic → control flow → measured allocation).
**Important Note:** `const fn` first, then `@comptime`/`comptime` gated on const-eval maturity (R9).

> Reference: Rust — `const fn` / const generics (`rust/library/core`); Zig — `comptime`.

**Files:** `src/sema/checker.cpp` (mark `const fn`; const-folding; constant-propagation over AST),
`src/ast/ast.h` (`ConstFn` flag, `StKind::ComptimeBlock`, `@comptime` expr), `src/backend/native.cpp`
(fold const into emitted C++), `EXP P4`, `SYNTAX SP-14/15`.

**Architectural changes:** introduce a **constant evaluator** (fold `2+3*4→14`; now over `const fn`
calls too). `const fn f(...) -> T { arithmetic/control-flow-limited body }` is checked into a pure
const boundary: no runtime allocation, no non-const method calls, no external I/O. `@comptime(expr)`
evaluates at compile time when `@pure`; `comptime { }` block forces compile-time-only evaluation.
Progressive relaxation per canonical §3.12: arithmetic → control flow → measured allocation (`--release`).

**Syntax/example:**

```kirn
const fn fact(n: Int) -> Int { return n <= 1 ? 1 : n * fact(n-1); }
const TABLE = [fact(i) for i in 0..=20];       # folded at compile time
const fn sum_of(n: Int) -> Int { ... }
aligned = @comptime(alignof(Point));            # comptime expression
comptime { validate(enum::variants); }          # compile-time-only block
```

**Expected result:** `const fn`/consts evaluate at compile time; `@comptime`/`comptime` blocks guard
compile-time-only code; native output folds constants.

**M5 phase note (implemented).** Delivered in `src/ast` (`Stmt::isConstFn`, `StKind::ComptimeBlock`,
`ExKind::Comptime`), `src/parser/parser.cpp` (`const fn`, `comptime { }`, `comptime(expr)` /
`@comptime(expr)`), and `src/sema/checker.{h,cpp}` (the const evaluator + purity scan). Executable
paths: `src/interp/runtime.cpp` (`Comptime`→operand, `ComptimeBlock`→block), `src/vm/compiler.cpp`
(interpreted fallback), `src/hir/hir.cpp` (both lowered), native `default:false` (interpreter
fallback). Tests: `tests/syntax/const_fn_ok.kn`, `tests/syntax/const_fn_err_impure.kn`,
`tests/syntax/comptime_err_impure.kn`, `tests/vm_parity/comptime.kn`, `examples/49_comptime.kn` (+3
recorded goldens). The plan's `tests/comptime/` path was not used because the gate discovers
`tests/syntax/*_ok*.kn` / `*_err*.kn`; the tests live there so they are actually run. Deviations
listed in the progress note above: scalar/list/tuple const-eval only (no dict/set/struct/method/IO),
no `sizeof`/`alignof`, and folding rewrites the AST literal rather than emitting a data section.

---

### Phase M6 — Numeric types: big_int, complex, rational, decimal _(features 13,14)_

**Goal:** land the four extended numeric `VK` types backed by `DATA_PLAN` Phases 2/3/4/5.

**Must Follow:** keep `Int` default, `big_int` opt-in (R10); no silent float promotion for decimal.
**Important Note:** promotion is `Integral < Rational < Real < Complex`; division by zero raises.

> Reference: Python — bignum `int`, `fractions`, `decimal` (`cpython/Lib/fractions.py`, `cpython/Lib/decimal.py`).

**Files:** `src/interp/value.h` (`VK::BigInt/Rational/Complex/Decimal` + fields), `src/interp/
runtime.cpp` (`toStr`/`repr`/`truthy`/`len`/`binop` cases; operators), `src/sema/type.h` +
`checker.cpp` (`big_int`, `complex`, `rational`, `decimal` type spellings + promotion
`Integral<Rational<Real<Complex`), `stdlib/lib/` `math`/`bigint` modules (`STD P3/P8`), `DATA P2-5`.

**Architectural changes:**

- **big_int**: sign + `std::vector<uint64_t>` limbs; schoolbook + Karatsuba; div/mod; `pow`, `gcd`,
  `is_prime` (Miller-Rabin), `powmod`, `bit_length`, `bit_count`, `to_bytes/from_bytes`, `Str(s,base)`.
- **complex**: two doubles; ANSI C Annex G arithmetic; `.real/.imag/.conjugate()/abs`, trig, `pow`,
  `exp`, `log`.
- **rational**: `Value::Int` num/den, GCD-normalized; `.numerator/.denominator`, auto-reduce.
- **decimal**: sign×digits×exponent arbitrary precision; context precision + rounding; `quantize`,
  `sqrt`, `ln`, `exp`; money-safe; **no auto-promote to float**.

_Cross-ref:_ `VK`/`TyK` additions mirror `DATA P0` (which expects these exact tags); the module-facing
API stays in `STD` (`lib.bigint`, `lib.math`).

**Syntax/example:**

```kirn
big_int("123456789012345678901234567890") ** 10      # arbitrary precision
complex(3,4) + complex(1,2)                          # 4+6i, .real/.imag/.conjugate()
rational(1,3) + rational(1,6)                        # 1/2, auto-reduced
Decimal("0.1") + Decimal("0.2")                      # 0.3 (exact), never 0.3000...004

x: big_int = big_int(2) ** 1000;                     # prints full 2^1000
```

**Error handling:** division by zero in rational/decimal → raise; losing-precision implicit downcast
(forbidden); `decimal`↔float mixing disallowed (explicit conversion only).

**Testing:** per-type `tests/numbers/*_test.kn` (arith identities, rounding, precision); promotion
order; `examples/56_numeric_types.kn`.

**Expected result:** all four numeric types work with correct promotion and precision; existing `Int`/
`Float` behavior unchanged.

**M6 phase note (implemented).** Delivered in `src/interp/value.h` (`VK::Complex` with `double re,im`;
`VK::Decimal` with sign + decimal-digit string + `int64_t` exponent; `Value::complexNum`/`decimalNum`)
and `src/interp/runtime.cpp` (complex/decimal helpers, `toStr`/`truthy`/`typeName`, global
registrations `Complex`/`complex`/`Decimal`/`decimal`/`rational`/`big_int` and `ComplexReal`/
`ComplexImag`/`ComplexConj`/`ComplexAbs`, `memberRead`, `binop`, `valEq`/`valLess`, `Cast`,
`typeTestMatches`). Types in `src/sema/type.h` (`TyK::Complex`/`TyK::Decimal`, `complexTy`/`decimalTy`)
and `src/sema/checker.cpp` (spellings, builtin registration, `unify`, binary-arith typing with
`complex` dominating and `decimal`+`Float` rejected, `memberAccess` property access, method surfaces,
`assignable` widening). Complex is backed by `std::complex<double>`; accessors are `z.real`/`z.imag`
(properties) and `.conjugate()`/`.abs()`/`.arg`/`exp`/`log`/`sqrt`/`sin`/`cos`/`tan`/`pow` (methods).
Decimal is exact for `+`/`-`/`*` via `bignum::Big`, with `/` rounded to 28 significant digits
(half-up) and `quantize(int)`. Tests: `tests/syntax/numbers_ok.kn`,
`tests/syntax/numbers_err_float_mix.kn`, `tests/vm_parity/numeric_types.kn`,
`examples/56_numeric_types.kn` (+3 recorded goldens). Deviations recorded: (1) `rational` exposes
`numerator()`/`denominator()` as methods, not properties, so existing call-sites are unchanged;
`complex` uses `.real`/`.imag` as properties (matching the plan example) with `absolute`/conjugate as
methods. (2) `decimal` `sqrt`/`ln`/`exp` are computed through `double` (documented precision limit),
not full arbitrary-precision transcendentals; `+`/`-`/`*` are exact and `/` is rounded to 28
significant digits. (3) There is no `stdlib/lib` directory in the tree, so the plan's `lib.bigint`/
`lib.math` module wrappers were not added; the builtins are globally registered as before. (The plan's
`tests/numbers/` path was not used because the gate discovers `tests/syntax/*_ok*.kn` / `*_err*.kn`
and `tests/vm_parity/*.kn`; the tests live there so they are actually run.)

---

### Phase M7 — Bytes/bytearray + buffer protocol _(feature 15)_

**Goal:** a first-class `bytes`/`bytearray` pair with `b"..."` literal and buffer views.

**Must Follow:** `bytes` immutable+hashable, `bytearray` mutable; index returns `Int`; zero-copy view.
**Important Note:** fix the missing `bytes` type spelling first; `b"..."` literal pairs with it (R6).

> Reference: Python — `bytes`/`bytearray`/`memoryview` (`cpython/Lib`).

**Files:** `src/lex/lexer.cpp` (`b"..."` string literal), `src/sema/type.h` + `checker.cpp`
(**add the missing `bytes` type spelling** — DATA:81 known gap; also `bytearray`), `src/interp/
runtime.cpp` (`VK::Bytes` is mutable vs immutable variants; `binop` `+`/`*`/`in`/`len`; `__getitem__`
returns int; `.decode/.encode/.hex/.fromhex`; buffer protocol `VK::View`), `stdlib/lib/bytes.kn`
(`STD P4a` API), `DATA P6`.

**Architectural changes:** split the existing `VK::Bytes` into immutable `bytes` (hashable → dict key)
and mutable `bytearray` (unhashable); add a zero-copy `VK::View`/`memoryview`. Index returns int; `+`
concat, `*` repeat; both expose the buffer protocol (bridge to Phase-FFI / `KIRN_CROSS_PLAN` byte
marshalling).

**Syntax/example:**

```kirn
b = b"\x00\x01\x0A";                 # bytes literal
n = b[1];                            # int 1
s = b.decode("utf-8");
h = b.hex();
ba = bytearray(b); ba.append(0xFF); ba[0] = 0x10;
mv = memoryview(ba);                 # zero-copy
```

**Error handling:** index out of range; mutating `bytes` (immutable) → error / returns new value.

**Testing:** `tests/bytes/*_test.kn` (immutable vs mutable, hashing, views); `examples/57_bytes.kn`.

**Expected result:** `bytes`/`bytearray`/`memoryview` fully usable; missing type spelling fixed.

**M7 phase note (implemented).** Delivered in `src/interp/value.h` (`VK::ByteArray` sharing a
`std::shared_ptr<std::string>` buffer, and `VK::View` carrying `bstr`/`voff`/`vlen`/`vreadonly` plus
`Value::byteArray`/`Value::view`) and `src/interp/runtime.cpp` (buffer helpers `isBuffer`/`bytesData`;
`repr`/`truthy`/`typeName`; globals `bytes`/`bytearray`/`memoryview`/`fromhex`; `memberRead` with
`decode`/`hex`/`to_string`/`len`/`contains`/`count`/`to_list`/`tolist`/`to_bytes` and the `bytearray`
mutators `append`/`push_back`/`extend`/`extend_from`/`clear`/`pop`/`pop_back`; `sliceOf`,
`OP_INDEX` and index-assignment for buffers with a readonly-view panic; `+` concat / `*` repeat /
`in`; `valEq`/`valLess` cross-buffer comparison; casts; `typeTestMatches`). Types in
`src/sema/type.h` (`TyK::ByteArray`/`TyK::MemoryView`, `byteArrayTy`/`memoryViewTy`) and
`src/sema/checker.cpp` (`toString`, spellings `bytearray`/`memoryview`, builtin registrations,
index→`UInt8`, `iterableElem`, `+`/`*`/`in`/`==` buffer rules, method surfaces, `assignable`
widening, and a static "cannot assign into immutable `bytes`"). Immutability is enforced at check
time and mirrored at run time. Tests: `tests/syntax/bytes_ok.kn`,
`tests/syntax/bytes_err_immutable.kn`, `tests/syntax/bytes_err_concat_str.kn`,
`tests/vm_parity/bytes.kn`, `examples/57_bytes.kn` (+3 recorded goldens). Deviations recorded:
(1) the plan lists `stdlib/lib/bytes.kn` (`STD P4a`), but the repository's stdlib is the
`stdlib/pet` package tree (there is no `stdlib/lib`), so — as with M6 — the buffer API ships as
builtins/methods and no module wrapper was added. (2) `memoryview` writes are permitted (and flow
back to the source) unless the view is marked read-only, matching Python's writable-buffer default;
the plan's "zero-copy view" requirement is met by sharing the underlying buffer. (3) Tests live under
`tests/syntax` (via the `*_ok`/`*_err` conventions) and `tests/vm_parity` rather than `tests/bytes`,
so the existing gates pick them up.

---

### Phase M8 — Deque / Counter / heap + collection free-functions _(feature 16)_

**Goal:** the high-value specialized collections and their free-function forms.

**Must Follow:** `deque` O(1) both ends; `Counter` multiset; `heap` min+max; free functions mirror methods.
**Important Note:** match canonical semantics (`Counter.most_common`, `heappush_max`).

> Reference: Python — `deque`/`Counter` (`cpython/Lib/collections/__init__.py`); Zig — `std/deque.zig`, `std/heap.zig`.

**Files:** `src/interp/value.h` (`VK::Deque`, `VK::Counter`, `VK::Heap`), `runtime.cpp` (method
surfaces), `stdlib/lib/collections.kn` (`STD P2c`: `Deque`, `Counter`, `SortedList`), `DATA P10`.

**Architectural changes:** `deque` = ring buffer (O(1) append/append_left/pop/pop_left/rotate); `Counter`
= multiset (`most_common`, `total()`, `+/-/max/min`); `heap` = binary heap (min + max variants,
`heappush`, `heappop`, `heapify`, `nlargest`/`nsmallest`). Mirror the missing free functions
`group_by`, `chunk`, `flatten`, `count`, `find`, `binary_search`, `partition`.

**Syntax/example:**

```kirn
import lib.collections;
d = collections.Deque(maxlen: 10); d.append(1); d.append_left(0);
c = collections.Counter(["a","b","a"]); print(c.most_common(1));
h = [3,1,2]; collections.heappush(h, 0); print(collections.heappop(h));   # 0
```

**Testing:** `tests/collections/deque_*,counter_*,heap_*`; `examples/58_collections.kn`.

**Expected result:** deque/Counter/heap and free-functions run with canonical semantics.

**M8 phase note (implemented).** Delivered in `src/interp/value.h` (`VK::Deque` — a
`std::shared_ptr<std::vector<Value>>` with an `Int` `maxlen` (`-1` = unbounded) and
`Value::deque`; `VK::Counter` — a `map` of key→Int count and `Value::counter`) and
`src/interp/runtime.cpp`: `repr`/`truthy`/`typeName`; the `Deque`/`deque` and `Counter`/`counter`
constructors; deque methods (`append`/`append_left`/`appendleft`/`push_back`/`push_front`/`pop`/
`pop_back`/`pop_right`/`pop_left`/`popleft`/`pop_front`/`extend`/`extend_left`/`extendleft`/`rotate`/
`clear`/`reverse`/`count`/`remove`/`index`/`copy`/`len`/`maxlen`); Counter methods (`total`/`len`/
`get`/`elements`/`keys`/`most_common`/`update`/`subtract`/`copy`/`clear`); static heap helpers
`heapLt`/`heapSiftUp`/`heapSiftDown` and the free functions `heappush`/`heappop`/`heapify`/
`heappush_max`/`heappop_max`/`nlargest`/`nsmallest` plus `group_by`/`chunk`/`flatten`/`count`/`find`/
`binary_search`/`partition`; `len`, `in`, Int/Counter indexing, slicing (deque slices return a
deque), iteration (`iterateSeq`, VM `OP_ITER_BEGIN`/`OP_INDEX`), index assignment, and `+`/`-`
Counter algebra (positive counts only). The checker gained `TyK::Deque`/`TyK::Counter` (+
`dequeTy`/`counterTy`), spellings, builtin registrations, method surfaces, index/SafeIndex typing,
`iterableElem`, and `+`/`-` Counter typing; `src/mir/mir.cpp` maps both to `Ty::Any`
(interpreter-only). Tests: `tests/syntax/collections_ok.kn`,
`tests/syntax/collections_err_deque_index.kn`, `tests/vm_parity/collections_m8.kn`,
`examples/58_collections.kn` (+3 recorded goldens). Deviations recorded: (1) the plan lists
`stdlib/lib/collections.kn` (`STD P2c`), but the repository's stdlib is the `stdlib/pet` package tree
(there is no `stdlib/lib`), so — as with M6/M7 — the collections API ships as builtins/methods and no
module wrapper (or `SortedList`) was added. (2) `VK::Heap` was not added as a distinct value: heaps
are ordinary lists manipulated by `heappush`/`heappop`/`heapify`, matching Python's `heapq` module.
(3) To keep the plan's global free-function names (`count`, `find`, …) from colliding with existing
user bindings (e.g. `var counter`, `fn find`, the `counter` generator in `examples/41`), prelude
builtins are now overridable by user `var`/`const`/`fn` declarations at module scope and shadowable by
local assignment — the interpreter erases the builtin binding when a user function takes its name.
(4) Tests live under `tests/syntax`/`tests/vm_parity` rather than `tests/collections`, so the existing
gates pick them up.

---

### Phase M9 — Concurrency: `sync` module (mutex/waitgroup/atomic) _(feature 17)_

**Goal:** safe, Go-quality concurrency primitives on top of the existing `spawn`/`chan`.

**Must Follow:** one `sync` module; `WaitGroup.go(f)` = spawn+add; `Atomic<T>` sequential consistency.
**Important Note:** integrate with `PLAN P14` sendability so only `chan`/`sync` cross threads.

> Reference: Go — `sync` (`go/src/sync/waitgroup.go`, `go/src/sync/mutex.go`, `go/src/sync/atomic/`).

**Files:** `src/interp/value.h` (`VK::Mutex/RwMutex/WaitGroup/Once/Cond/Atomic`), `runtime.cpp`
(implementations; `spawn` + WaitGroup integration), `stdlib/lib/sync.kn` (`STD §7`),
`DATA P12`, `PLAN P14` (sendability + scheduler), `DO_FIRST P11`.

**Architectural changes:** implement the `sync` module per `DATA P12`; `WaitGroup.go(f)` = spawn + add
(Go 1.25 semantics); `Atomic<T>` with sequential consistency (maps to C++ `std::atomic`); integrate
with the `sendability` checker (`PLAN P14`) so no shared mutable globals cross threads except via
`chan`/`sync`; switch `select` off the busy-poll (condvar) if not already.

**Syntax/example:**

```kirn
import lib.sync;
var mu = sync.Mutex(); var wg = sync.WaitGroup();
wg.go(fn(){ mu.lock(); count += 1 in mu; mu.unlock(); });
wg.go(fn(){ ... });
wg.wait();
var a = sync.Atomic[Int](0); a.add(1); print(a.load());
```

**Testing:** `tests/sync/*_test.kn`; worker-pool (10k tasks) ASan + TSan clean on CI; deadlock-negative
cases. **Exit criteria** reuse `PLAN P14` (crawler demo, condvar select, no data races).

**Expected result:** `sync` primitives run correctly and race-free.

**M9 phase note (implemented).** Delivered in `src/interp/value.h` (`VK::Mutex`/`RwMutex`/
`WaitGroup`/`Once`/`Cond`/`Atomic`, one `std::shared_ptr<SyncImpl>` slot) and `src/interp/runtime.cpp`
(`SyncImpl` + `makeSync` + constructors, the `sync` pseudo-module, and the full member surface):
`Mutex.lock/unlock/try_lock/locked`; `RwMutex.lock/unlock/rlock/runlock/try_lock` (readers share, one
writer); `WaitGroup.add/done/wait/go` where `go(f)` = increment + spawn and `wait()` blocks on a
condition variable until the count drains; `Once.do/done`; `Cond.wait(mu)/signal/broadcast` (its
condvar combined with the caller's `Mutex`, adopting the lock so the wait is atomic); and
`Atomic.load/store/swap/add/sub/inc/dec/compare_exchange` over `std::atomic<int64_t|bool|double>`
with sequential consistency. `sync.Atomic[T](v)` (and the bracketless `Atomic(v)`) type the element
by `T`/the initial value; `Atomic[T]` also runs the `isSendable(T)` gate. The checker registers the
primitives as dynamic handles (verified: gate 251, golden 242, crosscheck 573, stdlib 9, all 0 fail).
Tests: `tests/syntax/sync_ok.kn`, `tests/vm_parity/sync_m9.kn`, `examples/59_sync.kn` (+3 recorded
goldens). Deviations recorded: (1) the plan lists `stdlib/lib/sync.kn` and `import lib.sync`, but the
repository has no `stdlib/lib` tree, so — as with M6/M7/M8 — `sync` ships as a builtin pseudo-module
(`import sync; sync.Mutex()`) plus bare globals, and no module wrapper was added. (2) The plan's
example grammar `count += 1 in mu` is illustrative only; locking is explicit via `mu.lock()`/
`mu.unlock()` (no `in`-guarded-assignment form was added). (3) `select` keeps its 1 ms-backoff poll
rather than switching to a channel condvar: select spans channels, timers and boolean gates, so the
periodic poll is still required for the last two, and changing it was out of proportion to the
primitive work. (4) TSan/ASan CI runs are not wired into this repository's gate scripts; correctness
is shown by deterministic tree-walk/VM parity tests (`tests/vm_parity/sync_m9.kn`) and the
`examples/59_sync.kn` shared-counter totals, not by a sanitizer build. (5) Full `PLAN P14`
"no shared mutable across threads" is not statically enforced for `spawn`/`WaitGroup.go` closures;
the existing `chan.send` sendability gate is retained and `Atomic[T]` element types are checked for
sendability, but the dynamic closure model leaves a sound general analysis to a later phase.

---

### Phase M10 — Introspection: `reflect` module + `typeof` completion _(features 18,19 tail)_

**Goal:** the `reflect` layer that unlocks serialization, decorators, and tooling.

**Must Follow:** expose type info as a first-class descriptor; `typeof` delegates here.
**Important Note:** runtime reflection first; compile-time reflection is a later `--strict` goal.

> Reference: Odin — `core/reflect` (`Odin/core/reflect/reflect.odin`); Python — `types`.

**Files:** `stdlib/lib/reflect.kn` + `src/interp/runtime.cpp` (runtime metadata over `VK`/type table:
`type/fields/methods/kind_of/variants/get_field/set_field/deep_equal/new`), `src/sema/type.h`
(type-descriptor), `EXP P7`, `DATA P14`, `PLAN P5.2`.

**Architectural changes:** expose `Value::k`/type info as a first-class descriptor; `reflect.type(x)`,
`reflect.fields(x)`, `reflect.methods(t)`, `reflect.kind_of`, `reflect.new(name, args)`; `deep_equal`.
`typeof` (M3) delegates here. This backs the decorator tier-2 (`SYNTAX SP-6`) and JSON/CBOR
serialization (`PLAN P7`) later. Compile-time reflection (Rust-2026-style) is a `--strict`/later
goal.

**Syntax/example:**

```kirn
import lib.reflect;
print(reflect.type(x)); print(reflect.fields(p)); print(reflect.methods(Point));
if reflect.is_of_type(v, Point) { var p: Point = reflect.get_field(v, "x"); }
b = reflect.deep_equal(a, b);
```

**Testing:** `tests/reflect/*_test.kn`; generic JSON serialization exercise; `examples/60_reflect.kn`.

**Expected result:** full runtime `reflect` surface works and `typeof` is backed by it.

**M10 phase note (implemented).** Delivered in `src/interp/runtime.cpp`: two static helpers —
`kindName(v)` (coarse category: `int`/`float`/`string`/`list`/`dict`/`struct`/`enum`/`function`/…) and
`typeRefName(v)` (extracts the type name from a `__struct__:`/`__enum__:` module value, a string, or a
concrete value) — plus a `reflect` pseudo-module branch in `moduleMember` implementing
`reflect.type(x)`/`reflect.typeof(x)` (alias of the M3 `typeof` query), `reflect.kind_of(x)`,
`reflect.fields(x)` (struct/heap fields as an ordered dict), `reflect.field_names(x)`,
`reflect.methods(t)` (struct-body `FuncDef`s plus `impl` methods for a type reference),
`reflect.is_of_type(v, t)`, `reflect.get_field(x, name)`, `reflect.set_field(x, name, val)` (mutable
heap fields), `reflect.has_field(x, name)`, `reflect.deep_equal(a, b)` (reuses `valEq`),
`reflect.new(t)` (struct with default/zero field values), and `reflect.sizeof(x)`. The checker adds
`reflect` to `importRoots_` so `import reflect;` resolves and member calls type as dynamic handles
(same treatment as `sync`/`math`). Tests: `tests/syntax/reflect_ok.kn`,
`tests/vm_parity/reflect_m10.kn`, `examples/60_reflect.kn` (+3 recorded goldens); gate 255, golden
245, crosscheck 579, stdlib 9, all 0 fail; tree-walk and VM outputs are byte-identical. Deviations
recorded: (1) the plan lists `stdlib/lib/reflect.kn` + `import lib.reflect`, but the repository has no
`stdlib/lib` tree, so — as with M6–M9 — `reflect` ships as a builtin pseudo-module (`import reflect;`)
and no module wrapper was added. (2) `reflect.fields` returns an ordered `dict` (name→value) rather
than a list of descriptors; `field_names`/`methods` return `list(Str)`. (3) `reflect.new(t)` takes only
a type reference and fills fields from their declared defaults (or a type-appropriate zero: `0`/`0.0`/
`false`/`""`, containers from `containerDefaultFor`); a name+args overload was not added.
(4) `reflect.set_field` mutates only `VK::Heap` fields (struct values are copied by value, so
reflective mutation of an inline struct would not propagate). (5) Tests live under
`tests/syntax`/`tests/vm_parity` rather than `tests/reflect`, so the existing gates run them. (6)
Compile-time reflection and a first-class type _descriptor object_ remain a later `--strict` goal; the
descriptor here is the type-name string plus the `fields`/`methods` queries.

---

### Phase M11 — Native AOT & polish for the new surface _(cross-cutting)_

**Goal:** the new operators and types lower correctly through `kirn build --native` and stay
performant.

**Must Follow:** lower the new surface through `emitNative`; no VM fallback on scalar paths.
**Important Note:** fold const/pipe/walrus/elvis as calls; golden codegen tests guard regressions.

> Reference: LLVM — native codegen backend (`llvm`).

**Files:** `src/backend/native.cpp` (lower walrus/elvis/pipe as folded calls; const-fn folding from
M5; `@`/`??`/safe-chain on scalar paths), `tools/kirn.cpp` (no new surface needed), `grammar/
kirn.ebnf` (final operator table + soft-keyword registry), `SYNTAX SP-17` (fmt/doc/editions).

**Steps:** extend `emitNative` for the new expression kinds; add `---` golden codegen tests;
`kirn fmt`/`doc` cover new syntax; edition-gate any soft keywords that become reserved later.

**Testing:** native/VM/tree-walker differential green on the new examples; ASan clean; corpus green.

**Example (`examples/59_native_new_surface.kn`, `kirn build --native` acceptance):**

```kirn
const N: Int = 1 << 20;

fn sum(xs: [Int]) -> Int { ... }

fn main() {
  var xs = [i * i for i in 0..N];   // comps lower on the scalar path
  var total = xs |> sum(%);         // pipe lowers to a folded call
  print(total ?? 0);                // elvis lowers with no VM fallback
}
```

**Expected result:** every new feature builds natively with no VM fallback on scalar paths.

**M11 phase note (implemented).** Delivered in `src/backend/native.cpp`: optional-scalar (`T?`) and
elvis `??` lowering on the scalar native path. `cppTy` emits `std::optional<scalar>` for a scalar
optional (the checker stores the element in `args[0]` — see `sema::optTy`/`unwrapOpt` — so new
helpers `optInner`/`isOptScalarTy`/`isNativeTy`/`assignCompatible` read it there, not via `Ty::inner`);
`declTy` maps `TyKind::Optional` through `sema::optTy`; `tryLower` accepts optional returns;
`exprLowerable` accepts a bare optional local and a `Coalesce` (elvis) whose result is a native
scalar; `Return` and `VarDecl`/`ConstDecl` gate on `assignCompatible` (scalars widen into optionals,
`none` stores into any optional); `emitPreamble` adds `#include <optional>`; the emitter renders
`none` as `std::nullopt`, lowers `a ?? b` to `[&]{ auto __co = (a); return __co ? *__co : (b); }()`
(a non-optional lhs drops the fallback), and `out_registerAll` boxes `std::optional<...>` returns.
A codegen dump hook in `src/driver/driver.cpp` (`KIRN_DUMP_NATIVE=<path>`) writes the emitted
translation unit and returns before the toolchain; `scripts/golden_linux.sh` gained a `check_codegen`
stage that records/verifies `scripts/golden/codegen_*.txt`, and gate 11 in `scripts/gates_linux.sh`
asserts lowerability (`KIRN_HAS_NATIVE`) plus native-binary == interpreter. Tests added:
`tests/codegen/{scalar_core,new_surface,reject_non_scalar}.kn` (3 goldens) and
`examples/61_native_new_surface.kn` (+3 front-end goldens). Suites: gates 261, golden 251, crosscheck
579, stdlib 9, all 0 fail.
**Deviations/reality vs the plan's sketch:** (1) the backend stays **scalar-only**: optional
_parameters_, aggregate/container locals, and comprehension bodies do not lower — the plan's
`[i * i for i in 0..N]` example falls back to the interpreter. (2) `@` matmul (P2) and safe-chain
`?.`/`?[]`/`?!` are non-scalar / optional-aggregate and remain interpreter-only. (3) `const fn`
folding (R9) is not wired into the native backend; `const` bindings lower as ordinary locals. (4)
`kirn fmt`/`doc` and edition-gated soft keywords (SYNTAX SP-17) were not touched — no `fmt`/`doc`
surface exists to extend in this tree. (5) The plan's example path `examples/59_native_new_surface.kn`
is already taken by M9 (`sync`), so the example ships as `examples/61_native_new_surface.kn`. (6) The
codegen golden harness (`scripts/golden_linux.sh` `check_codegen` + gate 11) is newly added, since no
codegen-snapshot infrastructure existed before.

---

### Phase M12 — Rejected features closed (documentation + rationale) _(features 4,5)_

**Goal:** permanently settle `++/--` and `?:` so they are never re-opened without an edition RFC.

**Must Follow:** document the rejections; add only an optional `W-nosugar` nudge, no syntax.
**Important Note:** reopening needs an edition RFC; sanctioned forms are `+= 1` and `if c { a } else { b }`.

> Reference: Rust/Go/Python — omit `++`/`--` and C-style `?:`.

**Deliverables:** a `docs/FEATURE_GAP_ANALYSIS.md` note + grammar comment recording both rejections with
rationale (canonical §3.4/§3.5); a `kirn check` lint `W-nosugar` (optional, warn if a user writes
`x++`-like patterns suggesting `+= 1`). No syntax added.

**Expected result:** `++/--` and `?:` documented as deliberate exclusions; users get a helping nudge.

---

## 6. Maturity/ordering short-cut (for a solo dev)

**Goal:** give a solo dev a minimal, dependency-aware order.
**Must Follow:** never start M9 before `PLAN P14` sendability lands; M2 may slot in after M1.
**Important Note:** this is an ordering aid only — it changes no phase's scope.

```
M1  expr ops (:= |> ??)        — cheapest, high value, one lexer/checker change   ─┐
M3  comps / ?. / smart-cast / typeof   (Python/TS ergonomics)                      │  value first
M4  with / Drop                    (resource safety)                              │
M5  const fn / comptime            (compile-time)                                 ┘
M6  numeric types (big_int/complex/rational/decimal) — big, well-specified (DATA) ─┐
M7  bytes                          (I/O substrate)                                │
M8  deque/Counter/heap             (collections)                                  │  breadth
M9  sync primitives                (concurrency)                                  │
M10 reflect + typeof tail          (introspection)                                ┘
M11 native AOT + polish ; M12 close rejections
```

**Short version for a single dev:** M1 → M3 → M4 → M5 → M6 → M7 → M8 → M9 → M10 → M11 → M12.
(M2 `@` matmul can slot in anytime after M1's operator table; M9 needs `PLAN P14` sendability.)

---

## 7. Testing strategy (shared)

**Goal:** one shared test/bench strategy for every gap.
**Must Follow:** each feature runs on tree-walker + VM + `--native`, with negatives, plus a `kirn bench` figure.
**Important Note:** the existing corpus stays green every phase; the two known rejections stay tracked.

- **Unit:** `tests/syntax/…`, `tests/numbers/…`, `tests/bytes/…`, `tests/collections/…`,
  `tests/sync/…`, `tests/reflect/…`, `tests/comptime/…` per phase.
- **Integration:** `examples/51…60_*.kn` each run on tree-walker **and** bytecode VM (differential);
  native `--native` equivalent.
- **Negative:** precedence (walrus-at-statement, `??`-mix), protocol-missing (`with`/`Drop`), impure
  `const fn`, scalar `@`, immutable-bytes write, smart-cast on non-stable binding.
- **Golden/regression:** existing corpus (≥ current count) stays green every phase; the two known
  rejections (`native_main.kn` exit-code, `regexp` glob-vs-engine) are tracked separately and not
  regressed.

---

## 8. Dedup / cross-reference table (add to `DO_FIRST_PLAN.md` ownership map)

**Goal:** mirror the ledger into the `DO_FIRST_PLAN.md` ownership map.
**Must Follow:** one row per feature family, `[AMEND]` where the owner doc is corrected.
**Important Note:** this is a pointer table — authority stays in each phase above.

| Feature family                          | Owner (spec)     | Syntax                  | Types         | Module API       | Execution |
| --------------------------------------- | ---------------- | ----------------------- | ------------- | ---------------- | --------- |
| Wallet/pipe/elvis operators             | **MISSING_PLAN** | SYNTAX (AMEND)          | EXP           | —                | M1        |
| Matmul `@`                              | **MISSING_PLAN** | EXP P13                 | DATA P9       | —                | M2        |
| Comps / safe-call / smart-cast / typeof | **MISSING_PLAN** | SYNTAX SP-9/SP-4/SP-5   | EXP P7        | —                | M3        |
| `with`/Drop/RAII                        | **MISSING_PLAN** | EXP P3                  | trait table   | contextlib (STD) | M4        |
| `const fn`/comptime                     | **MISSING_PLAN** | EXP P4, SYNTAX SP-14/15 | —             | —                | M5        |
| big_int/complex/rational/decimal        | DATA P2-5        | EXP P8                  | VK types      | STD math/bigint  | M6        |
| bytes/bytearray                         | DATA P6          | SP-* b"..."             | VK::Bytes     | STD P4a          | M7        |
| deque/Counter/heap                      | DATA P10         | —                       | VK types      | STD P2c          | M8        |
| sync/mutex/atomic                       | DATA P12         | —                       | VK types      | STD §7           | M9        |
| reflect                                 | DATA P14         | SP-5/6                  | type metadata | STD              | M10       |

---

## Appendix R — Web research & verification (2026-09-03)

Every "recent change" claim in the canonical digest was verified via web search/fetch:

**Goal:** verify every "recent change" claim against shipping facts.
**Must Follow:** cite a URL or a verified reference-language path; no unverified claim enters §3.
**Important Note:** research is dated 2026-09-03 — re-verify before ratifying a later edition.

1. **PHP 8.5 pipe `|>`** — **SHIPPED Nov 20, 2025** (`php.net/manual/en/language.operators.functional.php`;
   `wiki.php.net/rfc/pipe-operator-v3`). Semantics: `mixed |> callable`, right side = single-parameter
   callable; left value always passed as the **first and only** parameter (no placeholder/position
   change); zero-call-overhead for direct function/method/static forms; arrow functions must be
   parenthesized. Confirmed the reference's claim and the _limitation_ (can't reorder args), which
   motivates Kirn's placeholder model (R1).
2. **PEP 798 (unpacking in comprehensions)** — **Status: Final**, Python 3.15 (Oct 2026)
   (`peps.python.org/pep-0798/`, `docs.python.org/3.15/whatsnew/3.15.html`). Adds `[*it for it in its]`,
   `{*s for s in sets}`, `{**d for d in dicts}`, generator + async variants.
3. **TC39 Hack pipe proposal** — still **Stage 2** (`%` topic placeholder, must appear ≥ once).
4. **Kotlin KEEP-0442 (negative smart casts)** — **stable** in Kotlin 2.3 (2025); DFA adds negative-type
   info for exhaustiveness on sealed hierarchies.
5. **Rust 2026 roadmap** — const traits (#106), full const generics (#100), compile-time reflection
   (#406); `pin_drop`/AsyncDrop nightly; auto async drop landed 2025-04.
6. **Python 3.14/3.15 stdlib** — PEP 734 `concurrent.interpreters` (3.14), PEP 782 `PyBytesWriter`
   (3.15), bytearray `.take()`/`.bytes`, heapq max-heap funcs (3.14), all confirmed.
7. **Go 1.24/1.25** — `WaitGroup.Go()` added (1.25); `math/big` perf work (1.24+).
8. **NumPy 2.0 (2024)** — `numpy.matrix` deprecated; `ndarray @` is the only matmul path.
9. **Python PEP 505 (None-aware ops, `??`/`?.`)** — status **Deferred** (no active champion); the
   reference's reliance on JS/C#/Kotlin for canonical `??`/`?.` is correct.

---

_This is the seventh living plan. It does not re-specify what the owner docs already cover; it
consolidates the canonical semantics, ratifies the open decisions (R1–R10), and sequences the 19
"unimplemented features" into M1–M12 so an implementer has a single executable roadmap. Ratify
R1–R10 before coding M1._
