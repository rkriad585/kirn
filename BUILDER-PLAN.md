# BUILDER-PLAN: A staged AOT toolchain with `kirn` + `kirnc`

## Goal

Drive the Kirn toolchain to a **two-executable** (`kirn` + `kirnc`), staged **AOT** build path that is industry-standard, **AI-first**, and measurably **faster than Rust and Go** for build+run — by adopting the smallest proven patterns from the reference languages (Rust, Go, CPython, Ruby, Nim, Odin, V, Zig, LLVM) rather than over-engineering new machinery.

## Must Follow

- The build produces **only** the `kirn` and `kirnc` executables — no extra shipped binaries. (`kirnrt` is a data artifact; `kirnlex/parse/check/run` become `kirnc` verbs.)
- **No over-engineering:** prefer the smallest proven design lifted from Rust/Go/CPython/Ruby over novel abstractions.
- **AI-first:** every stage must expose machine-consumable output — structured diagnostics, `--emit=` IR dumps, stage hooks, `--stop-after`, dry-run/`-###` — so agents and tooling can drive and inspect the pipeline.
- **Kirn-style spellings:** lowercase width micros (`i4..i256`, `u4..u256`, `f4..f256`, `uf4..uf256`, `isize`/`usize`) plus capitalized primitives (`Int`, `Float`, `Bool`, `Str`, `Char`, `BigInt`, `Rational`, `UInt`, `UFloat`, `Int4..UInt256`, `Float4..UFloat256`); methods are lowercase (`.to_int()`, `.checked_mul()`), free builtins/types are capitalized.
- **Performance:** each AOT stage must be engineered to beat the Rust/Go equivalent, verified with the §7 bench harness.

## Important Note

Key invariants this plan already fixes and must not lose: the two-tool split modeled on Go's `cmd/go` ↔ `cmd/compile`; the phase list modeled on `gc.Main` / rustc's query spine; stage hooks modeled on rustc `Callbacks`; `--emit=` output types and `-###` dry-run; the SSA pipeline modeled on Go's `ssacompile`; the IR shape from CPython ASDL (AST→bytecode flatten-late from Ruby); the six cross-language rules in §3.4.14; and the phased plan in §6, testing strategy in §7, and open questions in §9.

Date: 2026-09-30
Scope: how the Kirn build path becomes a real ahead-of-time (AOT) compilation
pipeline described by `prepro -> compile -> assemb -> executable`, exposed as
**two binaries** (`kirn` = user-facing operations, `kirnc` = the compiler
driver that also drives C/C++).

---

## 1. The ask, restated

**Goal:** Restate the staged AOT pipeline and the exactly-two-tool ask (`kirn` + `kirnc`) so every later section has one fixed target.
**Must Follow:** Keep the `prepro → compile → assemb → executable` stages and the `kirn`/`kirnc` split; ship no third tool.
**Important Note:** This restatement is the contract later sections refine — do not widen the tool count.

1. The build must be a staged AOT pipeline:

   ```
   source -> prepro (preprocessor)
           -> compile (compiler: front end + optimizer + codegen)
           -> assemb (assembler)
           -> executable (linker)
   ```

   with the intermediate forms from the current literature made explicit:

   ```
   scanning/lexing -> parsing -> macro expansion -> type checking
   -> HIR -> MIR -> optimization -> SSA -> LLVM codegen -> assembling -> linking
   ```

2. Exactly **two** tools:

   - `kirn` — "compile, run, and more kirn operations" (the user-facing driver,
     today's `kirn` extended: `run`, `build`, `test`, `new`, `install`, `doc`, …).
   - `kirnc` — the low-level compiler driver: can _run or compile_ Kirn _and_
     C/C++; exposes stage flags `check`, `lex`, `parse`, `rt`, `run`, plus more.

---

## 2. Current state (verified by code reading, 2026-09-30)

**Goal:** Record what actually exists today, verified by code reading, so the plan builds on reality rather than assumptions.
**Must Follow:** Cite `file:line` evidence; mark every "does not exist yet" item as a gap and reuse the existing front end/VM/runtime wherever possible.
**Important Note:** The gap table is load-bearing — several plan assumptions (LLVM, macros, HIR/MIR/SSA) were false; keep the corrections visible.

Important reality check: several things the ask assumes **do not exist yet**.

| The ask assumes…                        | Reality today                                                                                                                                                                                                                                  | Evidence                                                                                                             |
| --------------------------------------- | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | -------------------------------------------------------------------------------------------------------------------- |
| LLVM backend ("Code Generation & LLVM") | **No LLVM anywhere in the repo.** The backends are (a) bundle source text into a copy of the interpreted `kirnrt`, or (b) emit a C++ launcher and shell out to `g++`/`clang++`/`cl.exe`.                                                       | `kirn_driver::buildProgram` (bundle branch) (bundle), `:2787-2811` (C++ shell-out); grep for `LLVM` = zero in `src/` |
| A preprocessor with macros              | **None.** No `#define`, no macro table, no `__FILE__`. _The `prepro` half of this row's own grep was inaccurate: it has 3 hits, all a directory name in the driver's runtime sweep plus two error strings. The `macro` half was right — zero._ | grep `macro` = 0 hits in `src/`+`tools/`                                                                             |
| HIR / MIR / SSA / optimizer             | **None.** Closest IR is the bytecode VM (`src/vm/`), which is a _running_ VM, not a transformable IR. `ast.h:3` and `interp/runtime.h:3` say the "MIR/LLVM backend" is a _future_ goal.                                                        | `src/vm/bytecode.h`, `src/vm/compiler.cpp`                                                                           |
| An assembler + linker as named stages   | No assembler stage; linking is folded into one `g++`/`cl.exe` call. Only MSVC `lib.exe` is invoked explicitly.                                                                                                                                 | `kirn_driver::buildProgram`                                                                                          |
| A `kirnc` binary                        | Stage-oracle tools already exist and are a proto-`kirnc`: `kirnlex` (tokens), `kirnparse` (AST), `kirncheck` (type-check only), `kirnrun` (type-check + interpret).                                                                            | `CMakeLists.txt:72-82`                                                                                               |

What **does** exist and is solid:

- Full front end, already factored as static libs: `kirn_lex` → `kirn_ast` →
  `kirn_parser` → `kirn_sema` → `kirn_vm` → `kirn_interp`, plus `kirn_backend`
  (C++ emission, scalar subset). `CMakeLists.txt:33-91`.
- Two runtime execution engines: tree-walking `Interpreter` and a bytecode VM,
  dispatch preference `native -> bytecode VM -> tree-walker`
  (`src/interp/runtime.cpp:2675-2679`).
- One shared `frontEnd()` = lex + parse + `sema::Checker::checkModule`
  (`kirn_driver::readManifest`).
- `kirn build` modes today: `-S` sasm listing, default host bundle, GNU C++
  cross/native (used on Linux), MSVC `-O`/link, pet pack
  (`kirn_driver::buildProgram`).
- FFI to C via `extern fn` + `unsafe` (`examples/24_ffi_unsafe.kn`); symbols
  resolve through the process's linked C runtime
  (`src/interp/runtime.cpp:823`).
- `LintConfig` exists but is dead code — a free hook for a lint surface
  (`src/sema/checker.h:22-25`).

### Conventions from real-world drivers (research summary)

| Pattern                                   | Source of truth                                 | Spelling                                                                     |
| ----------------------------------------- | ----------------------------------------------- | ---------------------------------------------------------------------------- |
| Stage-selection is a **verb**, not a flag | Nim, cargo, Zig                                 | `nim check`, `cargo check`, `zig run`                                        |
| `check` = front end only, no codegen      | `cargo check`, `nim check`, `gcc -fsyntax-only` | bare word                                                                    |
| `run` implies build-then-execute          | `zig run`, `go run`, `nim r`                    | verb, or `-r` flag                                                           |
| Stop-at-stage flags                       | GCC/Clang driver                                | `-E`, `-S`, `-c`, `-###` (print pipeline)                                    |
| C driver in the same tool                 | Zig                                             | `zig cc`/`zig c++` accepts clang flags verbatim, forwards unknowns           |
| Emit-at-stage flags                       | Zig                                             | `-femit-llvm-ir`, `-femit-asm`, `-femit-bin`                                 |
| LLVM stage mapping                        | LLVM tooling                                    | `opt` = passes/O levels, `llc` = IR→asm/obj, `lld` = link; SSA via `mem2reg` |

---

## 3. Target architecture

**Goal:** Define the target pipeline and the strict two-tool responsibility split before any per-stage design.
**Must Follow:** One ordered stage list modeled on real drivers; exactly two shipped executables.
**Important Note:** Every later section must stay consistent with this architecture.

### 3.1 The pipeline

**Goal:** Fix the canonical stage list and each stage's IR/form and stop-at verb/flag.
**Must Follow:** Keep the ordered `prepro → lex → parse → check → hir → mir → ssa → llvm → asm → link` spine; do not hand-write SSA — `mem2reg` produces it.
**Important Note:** This table is the spine `-###`, `check`, and `--emit-*` all reason about; keep it in one place.

```
 source .kn/.c/.cpp
   │
   │  prepro ──────────── macro expansion (-D defs, #include-like imports)
   ▼
 compile
   ├─ scan/lex ────────── token stream              (kirn_lex today)
   ├─ parse ───────────── AST                       (kirn_parser today)
   ├─ type check ──────── resolved symbols, types   (kirn_sema today)
   ├─ HIR ─────────────── lowered, named, typed AST (NEW src/hir/)
   ├─ MIR ─────────────── CFG + values, still high-level (NEW src/mir/)
   ├─ optimize/SSA ────── SSA IR + optimization      (NEW, or LLVM passes)
   └─ LLVM codegen ────── LLVM IR                    (NEW src/backends/llvm/)
   │
   assemb ─────────────── machine code / object      (LLVM MC or host as)
   ▼
 executable ───────────── linked binary              (LLD or host linker)
```

Spelling per stage, matching the ask:

| Stage                                            | Kirn IR/form                      | Stop-at flag or verb           |
| ------------------------------------------------ | --------------------------------- | ------------------------------ |
| prepro                                           | token-level, macros expanded      | `kirnc -E` / `kirnc prepro`    |
| scanning (lexing)                                | `std::vector<Token>`              | `kirnc lex`                    |
| parsing (+ macro expansion is baked into prepro) | `kirn::ast::Program`              | `kirnc parse`                  |
| type checking                                    | `sema::Checker` results           | `kirnc check`                  |
| HIR                                              | `src/hir/` lowered AST (no sugar) | `kirnc -emit-hir`              |
| MIR                                              | `src/mir/` CFG, no-destructors    | `kirnc -emit-mir` / `kirnc rt` |
| optimization + SSA                               | LLVM `opt` new pass manager       | `-O0..3` / `-emit-llvm`        |
| LLVM codegen                                     | LLVM IR module                    | `-emit-llvm` / `-S`            |
| assembling                                       | object file                       | `-c`                           |
| linking                                          | executable                        | default / `-o`                 |

### 3.2 The two tools

**Goal:** Define the strict responsibilities of `kirn` (orchestrator) and `kirnc` (compiler proper).
**Must Follow:** `kirn` execs `kirnc` and never links the compiler; exactly two shipped executables.
**Important Note:** Modeled on Go's `cmd/go` ↔ `cmd/compile`; the oracle tools consolidate into `kirnc` verbs as temporary aliases.

```
kirn  = cargo/zig-style orchestrator (user operations)
kirnc = rustc/gcc-style driver (the compiler proper)
```

- `kirn build` / `kirn run` / `kirn check` become thin: resolve the project,
  pick target/profile, then invoke `kirnc` with the right flags and hand off
  artifacts/exit codes. (`kirn` keeps everything that is not pure compilation:
  `test`, `new`, `install`, `add`, `doc`, `targets`, `fmt`, `doctor`, …)
- `kirnc` owns: source → IR → asm → obj → exe, the `-E/-S/-c/-o/-###` grammar,
  C/C++ driving, and every stage dump.
- Exactly two shipped executables: `kirn` + `kirnc`. The stage-oracle binaries
  (`kirnlex`, `kirnparse`, `kirncheck`, `kirnrun`) are consolidated into
  `kirnc` verbs (kept working as hidden aliases for scripts first, removed
  later). `kirnrt` (the bundled interpreter) is treated as a _data artifact_,
  not a user tool, and is dropped when real AOT is the default.

### 3.3 `kirnc` CLI surface (initial proposal)

**Goal:** Specify the `kirnc` CLI — verbs, stop-at-stage flags, `--emit-*`, and backend selectors.
**Must Follow:** Adopt GCC's `-E/-S/-c/-o/-###` grammar and Zig's `--emit-*`/`zig cc` forwarding; reject unknown args rather than treating them as filenames.
**Important Note:** Verified against real drivers (Ruby `--dump=`, rustc `OutputType`, Go `Shell`); `run` semantics remain an open decision (§9).

```
kirnc <verb> [inputs] [flags]

verbs:
  check            lex + parse + type-check only (cargo check); exit 1 on error
  lex              lex only; print tokens            (was kirnlex --dump)
  parse            lex + parse; print AST            (was kirnparse --ast)
  rt               type-check + lower to MIR/bytecode and print IR / run via runtime
  run              full AOT pipeline then execute with [args...]
  build            full AOT pipeline to executable/object (default verb)

language selection:
  -x kirn|c|c++    force language (default from extension: .kn/.c/.cpp/.h)
  cc c++           alias: drive the C/C++ front end (wraps clang)

stop-at-stage (GCC grammar):
  -E               preprocess only (macro dumps)
  -S               assemble -> assembly? no: to LLVM-asm/object listing
  -c               compile+assemble to .o, no link
  -o <path>        output path
  -###             print the composed stage commands without running (gcc -###)
  -v / --verbose   print each stage's actual command

front-end / analysis flags:
  --emit-tokens    print token stream                   (lex)
  --emit-ast       print AST                            (parse)
  --emit-hir       print HIR after lowering
  --emit-mir       print MIR (CFG/dumps)  = `rt` dump   (see rt)
  --emit-llvm      write LLVM IR (.ll/.bc)              (zig -femit-llvm-ir)
  --emit-sasm      keep today's KIRN-SASM v1 listing (-S compat)
  -D name[=val]    define macro at prepro
  -U name          undefine macro
  -W flags, -A, -D (lint)  first-class surface for the dormant LintConfig

codegen/backend flags:
  -O0/-O1/-O2/-O3 / --debug / --release
  -target <os>-<arch>        KIRN_TARGET / host default (GOOS-GOARCH style)
  -fbackend=llvm|cpp         LLVM IR backend (future) vs C++-text backend (today)
  -fno-vectorize, -ffast-math etc. once LLVM passes land
  --asan

runtime/execution:
  rt file.kn [args...]       build via bundled runtime then execute  (today's default)
  run file.kn [args...]      full AOT then execute
  -- args                    split program args (zig/go idiom)
```

Notes:

- `rt` is interpreted here as **"run through the runtime"** — the current
  bundle-and-interpret path — versus `run` = true AOT. This is an assumption;
  see Open Questions. (The codebase already uses "rt" = runtime:
  `kirnrt`, `KIRN_RT`.)
- `cc`/`c++` verbs make `kirnc` a drop-in for C/C++ (Zig model): unknown flags
  forward to the wrapped compiler; `-E`, `-S`, `-c`, `-fsyntax-only`,
  `-emit-llvm`, `-o` all pass through.
- Empty args → `usage()` like `kirn` today (`kirn_driver::usage`).
- Stage-verb/flag grammar grounded in real drivers: verbs ↔ Ruby's
  `--dump=` bitset (`yydebug`/`parsetree`/`syntax`/`insns`, §3.4.12);
  `--emit-*` ↔ rustc's `OutputType` + `should_codegen()/should_link()`
  predicates (§3.4.5); dry-run ↔ Go's `Shell` + `-n`/`-x` (§3.4.6).

### 3.4 Reference code: how Go, Rust, Ruby and CPython implement each stage

**Goal:** Quote the actual reference-language sources each Kirn design choice borrows from, so no stage is invented.
**Must Follow:** Prefer one small proven pattern per stage from Rust/Go/CPython/Ruby/Nim/Odin/V/Zig/LLVM, and cite `file:line`.
**Important Note:** The six rules distilled in §3.4.14 are the inherited contract; the rest of the plan must stay consistent with them.

This section quotes the _actual_ compiler sources (local trees in
`/home/rkriad585/Projects/go-rust-ruby-cpython-source-code`) that the Kirn
design below borrows from. Every snippet is `file:line`-verified. Paths are
relative to each tree: `go-master`, `rust-main`, `ruby-master`, `cpython-main`.

#### 3.4.1 Two-tool split (`kirn` vs `kirnc`) — modeled on Go's `cmd/go` ↔ `cmd/compile`

Go is the cleanest example of a user-facing tool that _never links_ the
compiler; it finds the tool by name in a tool dir and execs it:

`go-master/src/cmd/go/internal/base/tool.go:30-42`

```go
// ToolPath returns the path at which we expect to find the named tool
// (for example, "vet"), and the error (if any) from statting that path.
func ToolPath(toolName string) (string, error) {
	if !ValidToolName(toolName) {
		return "", fmt.Errorf("bad tool name: %q", toolName)
	}
	toolPath := filepath.Join(build.ToolDir, toolName) + cfg.ToolExeSuffix()
	err := toolStatCache.Do(toolPath, func() error {
		_, err := os.Stat(toolPath)
		return err
	})
	return toolPath, err
}
```

`compile` is behind an interface, so the build graph never names the binary:

`go-master/src/cmd/go/internal/work/gc.go:35-43` and `build.go:276-286`

```go
type gcToolchain struct{}

func (gcToolchain) compiler() string { return base.Tool("compile") }
func (gcToolchain) linker()  string  { return base.Tool("link") }

func (c buildCompiler) Set(value string) error {
	switch value {
	case "gc":
		BuildToolchain = gcToolchain{}
	case "gccgo":
		BuildToolchain = gccgoToolchain{}
	default:
		return fmt.Errorf("unknown compiler %q", value)
	}
	cfg.BuildToolchainName = value
}
```

The `compile` argv is built in one place and handed to a single Shell choke
point:

`go-master/src/cmd/go/internal/work/gc.go:136`

```go
	args := []any{cfg.BuildToolexec, base.Tool("compile"), "-o", ofile, "-trimpath", a.trimpath(), defaultGcFlags, gcflags}
```

> **Adopt in Kirn:** `kirn` locates `kirnc` (env override `KIRN_CC`, then
> `<toolroot>/kirnc`, then exe-relative) and execs it; a `Toolchain`
> interface keeps a second backend (e.g. a JIT) swappable. `kirn build -x`
> / `-n` semantics come for free (see 3.4.6).

#### 3.4.2 Driver phase list — modeled on `gc.Main` (Go) and rustc's query spine

Go's driver is a straight-line function with an explicit ordered phase list —
parse+typecheck → walk → escape → SSA → obj — and the README states the phases:

`go-master/src/cmd/compile/README.md:9-11` and `98-119`

```markdown
`cmd/compile` contains the main packages that form the Go compiler. The compiler
may be logically split in four phases ...

### 1. Parsing (... src/cmd/compile/internal/syntax)

### 2. Type checking (... typecheck)

### 3. IR construction ("noding") (... noder)

### 4. Middle end (... walk, escape, ...)

### 6. Generic SSA (... ssa)

### 7. Generating machine code (... ssa, obj)
```

rustc's driver spine is the same shape in code (parse → expand → analysis →
codegen → link):

`rust-main/compiler/rustc_driver_impl/src/lib.rs:268-282`

```rust
        // Parse the crate root source code (doesn't parse submodules yet)
        let mut krate = passes::parse(sess);
        ...
        let linker = create_and_enter_global_ctxt(compiler, krate, |tcx| {
            // Make sure name resolution and macro expansion is run.
            let _ = tcx.resolver_for_lowering();
            if callbacks.after_expansion(compiler, tcx) == Compilation::Stop {
                return None;
            }
            ...
            tcx.ensure_ok().analysis(());
            ...
            let linker = Linker::codegen_and_build_linker(tcx, codegen_backend);
```

> **Adopt in Kirn:** `kirnc` runs one ordered sequence of stage functions
> (`prepro → lex → parse → check → hir → mir → ssa → llvm → asm → link`),
> each of which may be cut short by a verb/flag. Keep the list in one table so
> `-###` and `check` can reason about it.

#### 3.4.3 Stage hooks for tools — modeled on rustc `Callbacks`

rustc lets an embedding tool stop the pipeline at four named points:

`rust-main/compiler/rustc_driver_impl/src/lib.rs:121-152` (excerpt)

```rust
pub trait Callbacks {
    fn config(&mut self, _config: &mut interface::Config) {}
    fn after_crate_root_parsing(...) -> Compilation { Compilation::Continue }
    fn after_expansion<'tcx>(...) -> Compilation { Compilation::Continue }
    fn after_analysis<'tcx>(...) -> Compilation { Compilation::Continue }
}
```

> **Adopt in Kirn:** `kirnc` accepts `--stop-after=<stage>` (mirrored in the
> reserved `Compilation::Stop` value above) checked after each phase, so an
> IDE/editor integration and `kirn check` share one driver.

#### 3.4.4 Macro expansion — modeled on rustc `rustc_expand` (fixpoint worklist)

rustc's expander is a _collect-invocations → expand → re-collect_ worklist to
fixpoint, with the Resolver borrowed live so `use` and `macro` interleave:

`rust-main/compiler/rustc_expand/src/expand.rs:469-486`

```rust
    /// Recursively expand all macro invocations in this AST fragment.
    pub fn fully_expand_fragment(&mut self, input_fragment: AstFragment) -> AstFragment {
        // Collect all macro invocations and replace them with placeholders.
        let (mut fragment_with_placeholders, mut invocations) =
            self.collect_invocations(input_fragment, &[]);
        // Optimization: if we resolve all imports now,
        // we'll be able to immediately resolve most of imported macros.
        self.resolve_imports();
        // Resolve paths in all invocations and produce output expanded fragments ...
        invocations.reverse();
        ...
```

> **Adopt in Kirn:** `src/prepro/` (see §4.0) follows the same two rules:
> expansion is a to-fixpoint worklist, and `#fn` bodies are stored _before_
> parsing, expanded into the token stream before `parse` begins.

#### 3.4.5 `--emit=`-style stage outputs — modeled on rustc `OutputType`

rustc declares one enum of output kinds with shorthands, and two aggregate
predicates that short-circuit whole stages:

`rust-main/compiler/rustc_session/src/config.rs:914-936`

```rust
    pub fn should_codegen(&self) -> bool {
        self.0.keys().any(|k| match *k {
            OutputType::Bitcode
            | OutputType::Assembly
            | OutputType::LlvmAssembly
            | OutputType::Mir
            | OutputType::Object
            | OutputType::Exe => true,
            OutputType::Metadata | OutputType::DepInfo => false,
        })
    }
    pub fn should_link(&self) -> bool {
        self.0.keys().any(|k| match *k {
            OutputType::Exe => true,
            _ => false,
        })
    }
```

...with shorthands (`llvm-ir`, `asm`, `obj`, `link`, `mir`) parsed from
`--emit=` and stored as a `BTreeMap` (`config.rs:2403-2432`).

> **Adopt in Kirn:** `kirnc --emit-tokens/--emit-ast/--emit-hir/--emit-mir/
--emit-llvm [-c|-o]` mirror this: one output-type enum, `should_codegen()`
> / `should_link()` predicates. `rt`/`check`/`lex`/`parse` are the "stop
> before that predicate" verbs.

#### 3.4.6 Dry-run printing — modeled on Go's `-n`/`-x`

Go makes dry-run a 3-line idiom because every subprocess goes through one
`Shell`:

`go-master/src/cmd/go/internal/work/shell.go:602-626`

```go
	if cfg.BuildN || cfg.BuildX {
		var envcmdline string
		...
		sh.ShowCmd(dir, "%s", envcmdline)
		if cfg.BuildN {
			return nil, nil
		}
	}
	var buf bytes.Buffer
	path, err := pathcache.LookPath(cmdline[0])
	...
	cmd := exec.Command(path, cmdline[1:]...)
```

> **Adopt in Kirn:** `kirnc -###` (print, never run) and `-v` (print as
> executed) funnel through one "run a compiled stage" helper that prints the
> composed command first — and `-n/-x` land free as aliases.

#### 3.4.7 SSA + pass pipeline — modeled on Go's `ssacompile` (data-declared)

Go declares the pass list as _data_ with `Required`/`Disabled`/`Name`, then
machine-checks ordering at `init()`:

`go-master/src/cmd/compile/internal/ssacompile/compile.go:413-420` and `:479-492`

```go
var passes = [...]ssa.Pass{
	{Name: "number lines", Fn: numberLines, Required: true},
	{Name: "early phielim and copyelim", Fn: copyelim},
	{Name: "early deadcode", Fn: deadcode},
	{Name: "short circuit", Fn: shortcircuit},
	{Name: "opt", Fn: opt, Required: true},
	...
}
type constraint struct { a, b string } // a must come before b
var passOrder = [...]constraint{{"dse", "insert resched checks"}, ...}
```

and the runner skips non-`Required` passes under `-N`:

`go-master/src/cmd/compile/internal/ssacompile/compile.go:82-85`

```go
	for _, p = range passes {
		if !f.Config.Optimize && !p.Required || p.Disabled {
			continue
		}
```

> **Adopt in Kirn:** once a real MIR/SSA pass suite exists, declare it as a
> pass table (name + fn + required/disabled), verify ordering in an `init()`
> assert, and give `-O0` (debug) the same "skip non-required" shortcut. The
> plan's phase 4/5 use LLVM's pass manager instead of hand-writing this; the
> table pattern still applies to the _driver-level_ stage list (§3.4.2).

#### 3.4.8 IR node shape — modeled on CPython's ASDL tagged union & Go's `miniNode`

CPython generates both the C `struct` and the Python-facing `ast` module from
one `asdl` schema:

`cpython-main/Grammar/python.asdl`

```asdl
mod = Module(body* stmt, type_ignores* type_ignore)
stmt = FunctionDef(identifier name, arguments args, stmt* body, ...)
```

`cpython-main/Include/internal/pycore_ast.h:196-207` (the generated C)

```c
struct _stmt {
    enum _stmt_kind kind;
    union {
        struct {
            identifier name;
            arguments_ty args;
            asdl_stmt_seq *body;
            ...
        } FunctionDef;
        ...
    } v;
};
```

Go shares a compact common header and keeps the rest in concrete nodes
(`miniNode` = `pos, op, bits, esc`, 12 bytes):

`go-master/src/cmd/compile/internal/ir/mini.go:16-26`

```go
type miniNode struct {
	pos  src.XPos
	op   Op
	bits bitset8
	esc  uint16
}
```

> **Adopt in Kirn:** keep the existing `unique_ptr`-based `src/ast/` (§4.4
> keeps house style); note the two _end_ shapes — a tagged union for compact
> memory (CPython/Go) vs. virtual dispatch, and an optional `ast.dump`-style
> "public IR" later, which the `PyCF_ONLY_AST` boundary (below) models.

#### 3.4.9 Types/name-checking stage — modeled on CPython `symtable` & Ruby `ruby -c`

CPython's _symbol table_ is the scope/analysis pass, and it is declared the
recursion-limit gate before codegen:

`cpython-main/Python/symtable.c:415-418` and `Include/internal/pycore_symtable.h:71-86`

```c
struct symtable *
_PySymtable_Build(mod_ty mod, PyObject *filename, _PyFutureFeatures *future)
{
    struct symtable *st = symtable_new();
    ...
}
```

```c
struct symtable {
    PyObject *st_filename;
    struct _symtable_entry *st_cur;  /* current scope */
    struct _symtable_entry *st_top;  /* module scope */
    PyObject *st_blocks;             /* AST node addr -> scope entry */
    PyObject *st_stack;              /* scope stack */
    ...
};
```

Ruby's `--dump=syntax` is the classic "parse-only, then exit non-zero on
errors" verb (`ruby -c`):

`ruby-master/ruby.c:2651`

```c
    if (dump & DUMP_BIT(syntax)) {
        printf("Syntax OK\n");
        ...
        return Qtrue;
    }
```

> **Adopt in Kirn:** `kirnc check` == the existing `sema::Checker` +
> `BorrowChecker` (exactly today's `kirncheck`), plus wiring the dormant
> `LintConfig`; CPython teaches keeping scope data (symbols) addressable from
> AST node pointers, which simplifies HIR/MIR later.

#### 3.4.10 AST→bytecode with a linked list, flatten late — modeled on Ruby

Ruby compiles into a `LINK_ANCHOR` linked list (patchable branches/labels),
runs peepholes, then flattens + threaded-code-translates at the very end:

`ruby-master/compile.c:898-910` (top of `rb_iseq_compile_node`)

```c
VALUE
rb_iseq_compile_node(rb_iseq_t *iseq, const NODE *node)
{
    DECL_ANCHOR(ret);
    INIT_ANCHOR(ret);
    ...
    case NODE_SCOPE: {
        iseq_set_local_table(iseq, RNODE_SCOPE(node)->nd_tbl, (NODE *)RNODE_SCOPE(node)->nd_args);
        ...
        CHECK(iseq_setup_insn(iseq, ret));   // step 3: peephole
        return iseq_setup(iseq, ret);        // step 4: flatten + tables
    }
}
```

...and the bytecode is a flat `VALUE` array later interpreted with a raw
`pc` pointer:

`ruby-master/vm_exec.c:97` and `vm_core.h:428`

```c
    reg_pc = reg_cfp->pc;            /* pc is a const VALUE* into iseq_encoded */
```

```c
    unsigned int iseq_size;
    VALUE *iseq_encoded;
```

> **Adopt in Kirn:** today's bytecode VM (`src/vm/compiler.cpp`) already emits
> instructions; Ruby's "linked list → flatten late" plus "keep the _original_
> (untranslated) opcode array for disassembly" are the two ideas to add for
> `rt`/`--emit-mir` dumps.

#### 3.4.11 Stop-at-AST, public IR — modeled on CPython `PyCF_ONLY_AST`

CPython stops the entire pipeline at the AST when asked, returning an AST
object instead of a code object:

`cpython-main/Python/bltinmodule.c:923-937`

```c
        if (flags & PyCF_ONLY_AST) {
            mod_ty mod = PyAST_obj2mod(source, arena, compile_mode);
            if (mod == NULL || !_PyAST_Validate(mod)) {
                _PyArena_Free(arena);
                goto error;
            }
            ...
            result = PyAST_mod2obj(mod);
        }
```

> **Adopt in Kirn:** `kirnc parse` (+ `kirnc -E` for prepro) is the same
> public-IR boundary. `gcc -fsyntax-only` / `nim check` / `cargo check` agree:
> a "no codegen" verb is the observable boundary between front end and backend.

#### 3.4.12 Stage introspection flags — modeled on Ruby `--dump=` bitset

Ruby models pipeline stages as a bitset and exits after printing one:

`ruby-master/ruby.c:139-158`

```c
#define EACH_DUMPS(X, SEP) \
    X(version) SEP X(copyright) SEP X(usage) SEP X(help) \
    SEP X(yydebug) SEP X(syntax) SEP X(parsetree) SEP X(insns)
enum dump_flag_bits {
    dump_version_v,
    ...
    dump_exit_bits = (DUMP_BIT(yydebug) | DUMP_BIT(parsetree) |
                      DUMP_BIT(insns)),
};
```

`ruby-master/ruby.c:2717` (the "print and exit" tail)

```c
    if (dump & DUMP_BIT(insns)) {
        rb_io_write(rb_stdout, rb_iseq_disasm((const rb_iseq_t *)iseq));
        rb_io_flush(rb_stdout);
        dump &= ~DUMP_BIT(insns);
        if (!dump) return Qtrue;
    }
```

> **Adopt in Kirn:** `check`/`lex`/`parse`/`rt` map onto Ruby's
> `yydebug`/`parsetree`/`syntax`/`insns` exactly: a `StageBits` field in the
> driver's options struct; the corresponding `--emit-*` writes a file,
> the verb prints to stdout, both `return` before codegen/link.

#### 3.4.13 Optimizer passes on a CFG — modeled on CPython's `flowgraph.c`

CPython's optimizer works on a CFG built from a flat instruction sequence,
split into Preprocessing / Optimization / Cleanup:

`cpython-main/Python/flowgraph.c:3788-3835`

```c
int
_PyCfg_OptimizeCodeUnit(cfg_builder *g, ...)
{
    /** Preprocessing **/
    RETURN_IF_ERROR(translate_jump_labels_to_targets(g->g_entryblock));
    RETURN_IF_ERROR(mark_except_handlers(g->g_entryblock));
    RETURN_IF_ERROR(label_exception_targets(g->g_entryblock));
    ...
    int ret = optimize_cfg(g, consts, const_cache, ...);
    ...
    RETURN_IF_ERROR(remove_unused_consts(g->g_entryblock, consts));
    RETURN_IF_ERROR(insert_superinstructions(g));
    RETURN_IF_ERROR(resolve_line_numbers(g, firstlineno));
}
```

And the whole 5-pass story is documented at the top of compile.c:

`cpython-main/Python/compile.c:1-15`

```c
/* This file compiles an abstract syntax tree (AST) into Python bytecode.
 * The primary entry point is _PyAST_Compile(), which returns a
 * PyCodeObject.  The compiler makes several passes to build the code object:
 *   1. Checks for future statements.  See future.c
 *   2. Builds a symbol table.  See symtable.c.
 *   3. Generate an instruction sequence. See compiler_mod() in this file ...
 *   4. Generate a control flow graph and run optimizations on it. See flowgraph.c.
 *   5. Assemble the basic blocks into final code. ...
 */
```

> **Adopt in Kirn:** the phase 3 `src/mir/` CFG and its optimizer are CPython
> `flowgraph.c` shaped: flat IR → CFG → optimize → assemble; and the
> `compile.c:1-15` header comment is the template for kirn's own stage
> documentation.

#### 3.4.14 Across the trees: the six rules Kirn inherits

1. **Orchestrator never links the compiler** (Go): `kirn` execs `kirnc`.
2. **Driver = one ordered stage list** (Go, rustc, CPython's compile.c:1-15).
3. **No-codegen is a verb, not a flag** (Go `-c`, `cargo check`, `-fsyntax-only`, `--dump=syntax`).
4. **The IR is a first-class public boundary** (CPython `PyCF_ONLY_AST`).
5. **Compile into a list, flatten late** (Ruby `LINK_ANCHOR`); keep original opcodes for `disasm`.
6. **Dry-run is one choke point per side-effecting operation class** (Go
   `Shell`). _Refined 2026-09-30:_ the original note said "3 lines", which
   overstates it. Go's `cfg.BuildN || cfg.BuildX` test really does appear 9
   times in `shell.go` — once for subprocesses (`runOut`) and once each for the
   non-subprocess side effects (`moveOrCopyFile`, `mkdir`, `remove`). The real
   rule is _one guard per class of side effect_, so budget ~6–10 for `kirnc`
   (spawn, mkdir, rm, mv, write). The output format is worth copying too: Go's
   `ShowCmd` emits a re-runnable script (it tracks the working dir and prints
   `cd <dir>` when it changes), and GCC quotes arguments so the printed line is
   paste-able — `-###` should print shell-quoted, one command per stage, not
   a raw argv dump. clang's alternative is a single `bool LogOnly` threaded
   through one exec function, which is the cheapest version of this idea.

---

## 4. Design by stage

**Goal:** Turn the pipeline of §3 into per-stage designs, from `prepro` to `executable`.
**Must Follow:** Reuse existing `kirn_lex/sema/vm` code; add new code only where a stage does not exist; keep house `unique_ptr` style.
**Important Note:** Each `### 4.x` carries its own Goal/Must Follow/Important Note and the §3.4 grounding that justifies it.

### 4.0 Preprocessor / macros (`prepro`) — module `src/prepro/`

**Goal:** Add a token-level preprocessor in `src/prepro/` with `-D/-U`, conditionals, and `__FILE__`-style builtins.
**Must Follow:** Directives are Nim-style `{. … .}` (never `#`, Kirn's comment sigil); hygiene is capture-free; `kirnc -E` prints the expanded token stream.
**Important Note:** A `#`-based design silently deleted directives; the `{.` choice needs no lexer change. See the measured demonstration below.

**The `#` premise in this section was wrong, and the error mattered.** The
original text proposed C-style `#fn` / `#if` / `#undef` and justified the sigil
with "`#` is unused today except in comments — verify in lexer". Verifying it
(`src/lex/lexer.cpp:186-189`, `grammar/kirn.ebnf:43`) came back the other way:
`#` is the _active, normative_ line-comment sigil, the lexer erases from `#` to
end-of-line before producing any token, and the plan's own suggested verify step
("verify in lexer") is exactly what catches it.

The consequence is worse than a syntax clash, and it is the reason this is
recorded rather than quietly fixed. A file written in the proposed syntax is not
an error today — it is a **silent no-op**:

```
$ cat probe.kn
#fn SQR(x) = x * x
#ifdef DEBUG
print("debug");
#endif
fn main() { print("hi"); }
$ kirnlex --dump probe.kn      # every directive silently gone, exit 0
$ kirncheck probe.kn           # probe.kn: OK     exit 0
```

A preprocessor whose directives vanish without complaint is the worst failure
mode available to one, and it would have shipped.

Design as built:

- **Unit**: the token stream _before_ parsing, so a macro can synthesize syntax.
- **Syntax**: Nim's `{. … .}`, which is what Nim itself uses in a language whose
  comment sigil is also `#`.
  - `{.fn NAME. body.}` and `{.fn NAME = body.}` — object-like, two spellings
  - `{.fn NAME(a, b) = body.}` — function-like
  - `{.undef NAME.}`, `{.ifdef NAME.}`, `{.ifndef NAME.}`, `{.if EXPR.}`,
    `{.elif EXPR.}`, `{.else.}`, `{.endif.}`
  - built-ins: `__FILE__`, `__LINE__`, `__KIRN__`, `__VERSION__`
    (`__TARGET__` deliberately **not** seeded: the target is a host property, and
    inventing a value here would let a macro observe a lie. The driver passes it
    via `-D`.)
- **The decisive engineering consequence of the sigil choice: no lexer change.**
  `{.` already lexes as two ordinary `Punct` tokens (`{` and `.` are both in
  `kPunct1`), so the whole directive layer is token pattern-matching over the
  existing `Tok` vocabulary. That means no new `Tok` enumerator, **no edit to the
  normative `COMMENT` production in `grammar/kirn.ebnf`**, and — measured — zero
  of the 199 pre-existing goldens changed when the preprocessor was switched on
  (`golden_linux.sh --record` rewrote all of them byte-identically). A `#`-based
  design would have re-recorded 53 of the 63 lex goldens, every one of which has
  a source beginning with a `#` comment.
- **Line-scoped directives.** Necessary, not stylistic: the lexer emits no
  newline token, so "end of line" is observable only as a change in
  `Token::line`. The terminator is a `.` _immediately_ followed by `}`, which is
  what lets a body contain both `.` and `}` — `1.5` and `{ return 1; }` both
  round-trip.
- **Hygiene — capture-free, which is this phase's verify criterion.**
  Parameters bind at the **call site**; every other identifier written in a
  body is alpha-renamed at **definition time** into a private per-macro
  namespace (`__kirnpre<N>_`). Two kinds of identifier are exempt: one followed
  by `(` (a call, resolved at the call site, so a macro body can call a
  function the caller defines) and one that already names a defined macro (so
  macro bodies compose). This is Rust's mixed-site hygiene restricted to what
  tokens alone can decide.
  Two consequences worth stating because they are not obvious:
  - Macro-to-macro composition is **backward-only** — the table is consulted at
    definition time, so `INNER` must be defined before `OUTER` names it.
  - Mutual recursion is therefore **structurally impossible**, not merely
    bounded: a body can only name a macro that already existed, so the reference
    graph only ever points backwards. `{.fn A. A.}` is caught at definition time
    with a message that names the cause. `kMaxSteps` remains as defence in depth.
- **No `#include`.** Module imports stay semantic (`import pet.math`), as
  originally decided.
- **Deliverable**: `kirnc -E` prints the expanded token stream.

> On the `-E` output shape: every real driver prints source text under `-E`, but
> `cargo expand`'s own disclaimer is the deciding evidence against it here —
> expansion to text is lossy in a way that _silently changes behaviour_ (expanded
> code can parse fine and compute a different answer). This project's oracle is
> byte-exact goldens, which reconstructed text spacing cannot support. So `-E`
> reuses the `KIND\tline:col\ttext` format `cmdLex` already emits
> (`src/driver/driver.cpp`), making `-E` and `lex` goldens directly comparable.
> Text output remains available as a later `--emit-source`.

Placement: `src/prepro/` library (`kirn_prepro` in CMake), consuming a `Token`
stream and returning an expanded `Token` stream.

```kn
{.fn MAX(a, b) = {.if a > b.} a {.else.} b {.endif.}.}

fn main() -> none {
    print(MAX(3, 9));
}
```

> Reference: Nim — `{. … .}` pragma/directive syntax, chosen because Nim also
> uses `#` for comments.

### 4.1 Scanning (lexing) — exists

**Goal:** Reuse the existing lexer (`kirn::Lexer`) behind `kirnc lex` and `-E`.
**Must Follow:** Keep the `kind\tline:col\ttext` output; add no new `Tok` enumerators.
**Important Note:** The token stream carries no filename, no newline/indent token, and no multi-line strings — every later stage is constrained by this.

`kirn::Lexer` (`src/lex/lexer.cpp`), `kirnlex --dump`. No changes needed beyond
exposing through `kirnc lex`. Output format: `kind\tline:col\ttext`, printed in
`kirn_driver::cmdLex` (`src/driver/driver.cpp`) — the earlier citation of
`tools/kirnlex.cpp:68-72` was wrong: that file is 19 lines and holds no printing
code at all, phase 1 having reduced it to a front end over the library.
`kirnc -E` reuses this exact format; see §4.0.

Three facts about the token stream that constrain every later stage, all
verified rather than assumed:

- **`Token` carries no filename or file id** (`src/lex/token.h`). The lexer
  _takes_ one and discards it — `Lexer::filename_` is written and never read — so
  `__FILE__` has to be threaded in explicitly, which is why the `prepro`
  constructor takes a filename. `ast::Span` has no file field either.
- **There is no newline, indent or dedent token.** The `Tok` enumerators exist
  but the lexer never emits them; the INDENT/DEDENT engine was deleted when the
  grammar moved to braces-plus-semicolons, and `src/lex/lexer.h` still advertises
  "INDENT/DEDENT synthesis (CPython algorithm)" in its header comment — stale.
  Line structure is observable only as a change in `Token::line`.
- **No raw or multi-line strings exist** (`scanString` breaks at EOL
  unconditionally), so a macro body cannot contain a multi-line literal. There
  are no block comments either; `#` is the only comment form.

```kn
fn main() -> none {
    var x = 1;
    print(x);
}
```

### 4.2 Parsing — exists

**Goal:** Reuse `kirn::Parser` behind `kirnc parse`.
**Must Follow:** Parse the source exactly once — fix the `-S` double parse.
**Important Note:** `frontEnd()` should hand the AST back so no later stage re-lexes or re-parses.

`kirn::Parser` (`src/parser/parser.cpp:1-2192`), `kirnparse --ast`. `kirnc
parse` reuses it. Fix: `frontEnd()` runs lex+parse; the `-S` path currently
parses twice (the `-S` branch of `kirn_driver::buildProgram`) — centralize to one parse
and keep the AST.

```kn
fn add(a: Int, b: Int) -> Int {
    return a + b;
}
```

### 4.3 Type checking — exists

**Goal:** Reuse `sema::Checker::checkModule` behind `kirnc check`, and wire the dormant `LintConfig` into `-W`.
**Must Follow:** `check` is the front end only — no codegen; exit 1 on user errors, 64 on CLI misuse.
**Important Note:** Grounded in CPython `symtable` and Ruby `ruby -c` (§3.4.9).

`sema::Checker::checkModule` (`src/sema/checker.cpp:318`). `kirnc check` =
exactly this (like `kirncheck`). Wire the dormant `LintConfig`
(`src/sema/checker.h:22-25`) into `-W` flags.

```kn
fn main() -> none {
    var x: Int = "not an int";
}
```

> Grounding: §3.4.9 — CPython's `symtable` (scope tree reachable from AST
> node pointers) and Ruby's `--dump=syntax` / `ruby -c` "Syntax OK" verb.

### 4.4 HIR — NEW `src/hir/`

**Goal:** Introduce a sugar-free, name-resolved HIR in `src/hir/`.
**Must Follow:** Per-kind `std::variant` payload (no fat node), resolved `DeclId`s, explicit scopes, `unique_ptr` ownership.
**Important Note:** Do not mix ownership strategies across stages; an unmapped construct must be `Unhandled`/`Nop` (never a silent drop). Grounding: §3.4.8.

Purpose: a sugar-free AST. Today the AST _is_ the parse tree plus a
desugar buffer (`src/parser/parser.h:104`). HIR lowers/match-evaluates:

- desugared `for`-over-range, `with`, f-strings → explicit ops (flavor tags
  already tracked by `ast_dump`'s `flavorSuffix`, `src/ast/ast_dump.cpp:28`);
- resolved names → symbol IDs; spans retained for diagnostics.
- Data layout fact: arenas were deferred until "MIR" (`src/ast/ast.h:3`);
  for HIR keep `unique_ptr` ownership to match house style, revisit at MIR.

Contains: lowered statements/expressions with explicit scopes, resolved
`FuncSig`s (`src/sema/symbols.h:29-37`), and the module graph.

> Grounding: §3.4.8 — CPython's generated ASDL tagged union vs Go's shared
> `miniNode` header; keep house `unique_ptr` style, note the shapes for a
> future `ast.dump`-style public IR.

> Reference: CPython — ASDL tagged-union node shape; Nim — variant objects; LLVM/MLIR — per-kind payload unions.

```kn
fn main() -> none {
    for i in 0..3 {
        print(f"i={i}");
    }
}
```

### 4.5 MIR — NEW `src/mir/`

**Goal:** Introduce a CFG-based, value-oriented MIR in `src/mir/` — the plane `rt`/`--emit-mir` dump and AOT consume.
**Must Follow:** Successor edges derived not stored; entry is block 0; alloca/load/store shaped with no hand-built SSA (`mem2reg` later).
**Important Note:** Keep `src/vm/compiler.cpp` a separate specializer; the MIR ownership decision must be explicit. Grounding: §3.4.10.

Purpose: CFG-based, value-oriented, destructor-free IR — the plane on which
`rt` dumps and future optimization live.

- Build from HIR as a per-function CFG with basic blocks and indexed operands
  (arena). This is the moment arenas arrive (matching `ast.h:3`).
- Keep the existing `src/vm/compiler.cpp` (AST→bytecode) as a _separate_
  specializer: it stays the fast path for the runtime, while MIR feeds AOT.
  The two converge later (bytecode becomes a codegen target of MIR).
- `kirnc rt` / `--emit-mir` dump here.

> Grounding: §3.4.10 — Ruby compiles via `LINK_ANCHOR` (linked list, patched
> labels) and flattens at the end (`iseq_setup`), keeping the original
> opcode array for `rb_iseq_disasm`; MIR is kirn's equivalent flat
> list → flatten-late plane.

> Reference: LLVM — derived successor edges; Ruby — flatten-late instruction list.

```kn
fn total(n: Int) -> Int {
    var sum = 0;
    var i = 0;
    while i < n {
        sum += i;
        i += 1;
    }
    return sum;
}
```

_Target: the bytecode VM path (`kirn run`) must beat the Rust/Go equivalent for the phase's programs, measured by the §7 bench harness._

### 4.6 Optimization + SSA — LLVM `opt`

**Goal:** Map optimization to the LLVM new pass manager; keep no in-tree hand-written SSA/optimizer.
**Must Follow:** Front end emits allocas + stores; `mem2reg` produces SSA; `-O0..3` maps to passes.
**Important Note:** If Kirn ever hand-writes passes, copy Go's data-declared `passes` table + `init()` order check (§3.4.7).

Two honest options; the plan picks a sequence:

- **Near term**: no in-tree optimizer. Optimization level maps to host flags
  (`-O2`/`-O1`, today at `kirn_driver::buildProgram` (`-O2`/`-O1`)) — unchanged for the C++
  backend.
- **With LLVM**: emit LLVM IR and run the new pass manager at the requested
  `-O` level. The front end never builds SSA by hand; it emits
  allocas + stores, and the `mem2reg` pass produces SSA (the standard
  lowering; confirmed against LLVM docs). "SSA generation" is therefore an
  LLVM-passes concern, not a hand-written phase.

> Grounding: §3.4.7 — Go declares `passes` as data (name/fn/required/disabled)
> and skips non-required ones under `-N`; if Kirn ever hand-writes SSA passes,
> copy that table + `init()` order-check.

> Reference: LLVM — `mem2reg` + new pass manager; Go — data-declared pass table.

```kn
fn area(w: Int, h: Int) -> Int {
    return w * h + 0;
}
```

_Target: `-O`/`--release` builds of the phase's programs must beat the Rust/Go equivalent, measured by the §7 bench harness._

### 4.7 LLVM codegen — NEW `src/backends/llvm/`

**Goal:** Add an opt-in LLVM backend (`src/backends/llvm/`, `KIRN_WITH_LLVM`) lowering MIR → `llvm::Module`.
**Must Follow:** In-process libs first (no PATH deps); scalar subset (int64/double/bool) first; `--emit-llvm` writes `.ll`/`.bc`.
**Important Note:** Keep `kirn`/driver libraries LLVM-free; non-scalar features fall back to the runtime or are rejected with a clear diagnostic.

- New lib `kirn_llvm` guarded by an opt-in CMake `KIRN_WITH_LLVM` (LLVM is
  heavy; keep it optional so dev machines and release pipelines without LLVM
  dev packages still build).
- Lower MIR → `llvm::Module`: scalars & vectors (int64/double/bool) first,
  mirroring the C++ backend's `isScalarTy` subset (`src/backend/native.cpp:17-41`);
  dynamic/object features fall back to the runtime (or are rejected with a
  clear diagnostic, `-fbackend=llvm`).
- Emission choices: (a) link the LLVM _libraries_ for in-process
  `PassBuilder` + `TargetMachine::addPassesToEmitFile`; or (b) shell out to
  `opt`/`llc` a la today's `g++` shell-out. Plan: **in-process first** (no
  PATH deps, matches the `g++`-free goal), keep `opt`/`llc` as `-###`-visible
  alternative.
- `--emit-llvm` writes `.ll`/`.bc` for inspection/tests.

> Reference: LLVM — `llc` / in-process `TargetMachine` codegen.

```kn
fn addmul(a: Int, b: Int) -> Int {
    return a + b * 2;
}

fn main() -> none {
    print(addmul(3, 4));
}
```

_Target: end-to-end `kirn build` and run of the phase's scalar feature must beat the Rust/Go equivalent, measured by the §7 bench harness._

### 4.8 Assembler — `assemb`

**Goal:** Make assembling a real stage: `-c` → `.o`, `-S` → asm listing, via LLVM MC or the host assembler.
**Must Follow:** One emission path so `-c`, `-S`, and `-###` work uniformly across targets.
**Important Note:** Cross targets reuse the release-workflow machine-header checks (ELF/PE/Mach-O).

- LLVM path: `llc`/`TargetMachine::addPassesToEmitFile(MCStreamer)` → `.s`/`.o`.
- C++ path: today's implicit assembler inside `g++`/`cl.exe` stays.
- `-c` = stop with a `.o` (`kirnc -c`), `-S` = `.s` listing. Cross targets
  reuse the header/machine-verify pattern from the release workflow
  (ELF e_machine / PE 0xAA64 / Mach-O).

> Reference: LLVM — MC layer / `llc` object emission; Zig — `-femit-asm` / `-femit-bin`.

```kn
fn main() -> none {
    print(6 * 7);
}
```

_Target: object emission for the phase's program must beat the Rust/Go equivalent, measured by the §7 bench harness._

### 4.9 Linker — `executable`

**Goal:** Make linking a real stage: in-process LLD (or host-linker fallback) producing the executable.
**Must Follow:** Select `lld::elf/coff/macho` by target; split today's fused `g++` call into explicit compile → assemble → link.
**Important Note:** The C++ and MSVC `lib.exe` paths must emit the same stage commands that `-###` prints.

- LLVM path: **LLD** (in-process `lld::elf::link`, `lld::coff::link`,
  `lld::macho` selection by target) → single driver, no PATH dependency.
  Fallback to host linker (`ld.lld`/`clang`/`link.exe`) when LLD libs are not
  linked in.
- C++ path: today's fused `g++ <launcher.cpp src/**> -o out`
  (`kirn_driver::buildProgram` (C++ shell-out)) — for kirnc, split this into explicit
  `compile -> assemble? -> link` commands so `-c` and `-###` work uniformly.
- MSVC `-O` path (`kirn_driver::buildProgram` (MSVC `-O`)) becomes: emit obj via cl, ar via
  `lib.exe`, honoring `KIRN_LIB_TOOL`.

> Reference: LLVM — LLD in-process linkers; Go — a single `cmd/link`; Zig — self-contained linking.

```kn
import pet.math;

fn main() -> none {
    print(pet.math.abs(-4));
}
```

_Target: link time for the phase's program must beat the Rust/Go equivalent, measured by the §7 bench harness._

---

## 5. Codebase refactors required (prereqs, in rough order)

**Goal:** List the prerequisite refactors (mostly done) that unblock the pipeline.
**Must Follow:** Extract the driver core, unify resolvers, single `frontEnd`, centralize path discovery, share payload formats, and type-check on every build path.
**Important Note:** Items 1–3 are done; 4–6 remain and are prerequisites for the installer/AOT phases.

1. ~~**Extract the driver core out of the `tools/kirn.cpp` monolith.**~~
   _Done — see the Phase 0 status note in §6._ The original wording said
   "everything below `frontEnd` is in an anonymous namespace", which was
   slightly wrong and understated the job: the `namespace {` actually opens at
   line 90 and closes at 3201, so **94% of the file** (3112 of 3303 lines) was
   unreachable from any other translation unit — and `readFile`, `readManifest`,
   and `struct Manifest` all sit _above_ `frontEnd` (`:493`) and were equally
   locked. The moved set was `resolveEntry :396`, `libDirsFor :421`,
   `resolveSource :1819`, `collectImports :1867`, `gatherEmbedded :2174`,
   `ensureRunner :2121`, `bundleTail :2078`, `buildProgram :2545`, and their
   dependency clusters, now in `src/driver/`.
2. **Unify the module resolvers.** _Done._ Three divergent copies existed, not
   two: the runtime path (`libDirsFor`), `kirnrun`'s own list (`addModuleDirs`),
   and the embedding resolver used by `kirn build` (`gatherEmbedded`, which is
   CWD-relative rather than entry-relative). `kirnrun` now calls `libDirsFor`
   and appends the entry's own directory, matching `kirn run`; the observable
   effect is that a module next to the entry file resolves under both tools
   instead of panicking under one. `gatherEmbedded` still has its own list —
   see the note in item 5.
3. **One `frontEnd()` call**, not two (`-S` double parse at `:2567`/`:2572`).
   _Done._ `buildProgram` called `frontEnd` for its diagnostics, discarded the
   result, then lexed and parsed the same source again to get an AST to print.
   `frontEnd` already returns `Parser(...).parseProgram()`, so its result is the
   AST wanted, and the re-parse's error branch was unreachable (the first pass
   had already returned). Type errors still fail the `-S` build with exit 65.
4. **Centralize the path assumptions**: `src/` at `<exedir>/../src`
   (`:2766`), `tools/runner.cpp` (`:2133`), prebuilt `.lib`s at `<exedir>`
   (`:2892`), `CMakeCache.txt` (`:1888`). _Partly done:_ `buildProgram` called
   `GetModuleFileNameA` twice and derived `binRoot2` inline when it was provably
   the same directory as `binRoot`; both now go through `selfExeDir()`. The
   remaining piece is the real requirement: one root-discovery with an explicit
   precedence (env override, then exe-relative, then install prefix) so an
   installed `kirn`+`kirnc` pair works without the source tree. Today
   `ensureRunner` still hardcodes `<exedir>/../src` and `<exedir>/../tools`.
5. **Share the payload formats.** `KIRNBUNDLE/1` writer/reader
   (`kirn_driver::bundleTail` / `tools/runner.cpp:61-90`) and `PETLIB/1`
   (`:2937-2951` / `:3068-3120`) are duplicated three ways; promote to one
   header. _Not started — now a cross-library concern_, since the writer is in
   `kirn_driver` and the reader is compiled into `kirnrt`. Two details make a
   single naive parse loop wrong: the two families differ in the `@meta`
   convention, and the bundle reader uses `rfind` for the `\nKIRNBUNDLE/1\n`
   marker (so the payload is the _last_ occurrence, which is what makes
   appending safe) while re-adding a trailing `\n` per line — meaning a bundle
   round-trip is byte-exact only for sources that end in a newline.
6. **Make `kirn build` type-check on the default bundle path.**
   Today branch B bundles without checking (`:2620-2637`); `kirnc check`
   semantics must be wired into `kirn build` (the user should never ship an
   unchecked bundle).

CMake impact: `add_executable(kirnc tools/kirnc.cpp)` +
`target_link_libraries(kirnc PRIVATE kirn_driver)` are ~2 lines now that the
library exists (§5.1 is done); `kirnc` gets the whole pipeline for free and
never re-links the compiler code, which is the point of extracting it.
`ws2_32` deliberately stays on the `kirn` executable alone: only the `kirn doc`
HTTP server uses WinSock, so the library does not inherit that dependency and
cannot force it onto future consumers.

---

## 6. Phased implementation plan

**Goal:** Sequence the work as independently shippable, CI-gated phases.
**Must Follow:** Each phase ships behind the corpus gates + golden snapshots and stays behavior-preserving.
**Important Note:** Phases 0–4 have landed; each carries a status note recording what reality corrected.

Each phase is independently shippable and CI-gated (corpus + gates_linux).

**Phase 0 — Driver-core extraction.** Split `tools/kirn.cpp` build plumbing;
unify module resolvers; single `frontEnd`; move path assumptions behind one
resolver. Merge `kirnrun`'s resolver back into the shared one.
**Goal:** Extract the driver core out of the `tools/kirn.cpp` monolith into the `kirn_driver` library.
**Must Follow:** Behavior-preserving move — one translation unit, forwarders not `using`, keep goldens byte-identical.
**Important Note:** Substantially done; still open are §5.5 (payload sharing) and §5.4's root discovery. Modeled on Go's `cmd/go` ↔ `cmd/compile` split.

```kn
# behavior-preserving fixture: stdout/exit must not change
fn main() -> none {
    print("hello, kirn");
}
```

_Verify:_ corpus gates green unchanged, byte-identical `kirn run`/`build`
behavior, CI green.

_Status 2026-09-30: substantially done; it had three real defects when
re-verified during phase 1, all now fixed._ The driver core is the
`kirn_driver` static
library (`src/driver/driver.{h,cpp}`); `tools/kirn.cpp` went 3303 → ~1540 lines
and keeps only the CLI layer (packages, `new`, `test`, the docs server,
`main()`). Three decisions worth recording, because each was a real fork in the
road:

1. **One translation unit, not a per-cluster split.** The code came out of an
   anonymous namespace, which has internal linkage _per translation unit_, so
   splitting 3100 lines across many `.cpp` files would have forced every
   cross-file symbol to acquire external linkage, and with it ODR, `inline`,
   and static-init-order risk. Keeping `driver.cpp` whole preserves linkage
   exactly, which is what makes the move behavior-preserving by construction
   rather than by testing. The cost is one big recompile on internal changes;
   the benefit is that the refactor needed no correctness argument at all.
2. **Forwarders, not `using` declarations, for the public surface.** A
   using-declaration cannot name an anonymous-namespace member, and declaring
   the same signature in both the header and the namespace makes every internal
   call site ambiguous. 45 one-line forwarders keep one definition and one
   public entry point. Sixteen internal call sites needed an explicit `::`
   because their arguments are driver-owned types (`Manifest`, `BuildOpts`,
   `TargetInfo`), so argument-dependent lookup pulls `kirn::driver` into the
   candidate set; those sites are listed in `driver.cpp`.
3. **Resolver unification goes toward `kirn run`, not toward `kirnrun`.** The two
   disagreed in a user-visible way: a module sitting next to the entry file
   resolved under `kirn run` and panicked with `unknown module` under `kirnrun`,
   because `libDirsFor` appends the project directory and `addModuleDirs` did
   not. `libDirsFor` is the superset and is what the real driver uses, so
   `kirnrun` now calls it. This is the one intentional behavior change in the
   phase, and `scripts/golden/kirnrun_sibling_module.txt` is the recorded
   before/after.

_Verification added with this phase:_ `scripts/golden_linux.sh` records every
tool's full observable behavior — argv, stdout+stderr interleaved, and exit
code — for all 199 cases (lex/parse/check over the example and negative
corpora, plus the `kirnrun` and `kirn` exit paths). `gates_linux.sh` was
insufficient on its own: it asserts exit codes and substring greps, so it stays
green while whitespace, ordering, and stderr wording change freely. The golden
suite is what makes "byte-identical" a checked claim instead of an aspiration.
It is also self-verifying — tampering with one recorded file makes it fail.
_Re-verification during phase 1 found three defects, which is the argument for
having oracles at all — none was visible in "199 goldens pass, 153 gates pass":_

1. **`libDirsFor` broke bare filenames** (a regression this phase introduced).
   `fs::path("main.kn").parent_path()` is empty, so the derived search dirs
   became `/pets`, `/../stdlib`, `/../../stdlib` — absolute paths off the
   filesystem root. `kirnrun main.kn` panicked `unknown module 'greet'` (exit 70) where `kirnrun ./main.kn` worked. The recorded golden could not see it,
   because the fixture passes an _absolute_ path, which always has a directory
   component. Fixed by mapping an empty parent to `.`.
2. **`collectRuntimeSources` swept the driver into the interpreter runtime.**
   It globs `src/**/*.cpp` with no exclusions, and moving the driver from
   `tools/` to `src/driver/` silently added 3400 lines of build plumbing to
   every `kirnrt` warmup and every cross build. Harmless today; from phase 4 it
   would drag LLVM headers into every launcher build, because
   `src/backends/llvm/` would be swept in the same way. Now an explicit
   skip-list, and the runtime is back to its original 9 translation units.
3. **`kirn build` shipped unchecked code** (§5.6). It returned 0 and produced an
   executable for a program `kirn run` rejects with 65; the failure only
   appeared when the binary was run. The bundled branch now type-checks first
   and returns 65. `gatherEmbedded` was also reading neither `$KIRN_PETS` nor
   `$KIRN_STDLIB`, so a program using them built into a binary that panicked
   `unknown module`; it now shares `libDirsFor`.

Still open in this phase: §5.5 (share the `KIRNBUNDLE/1` and `PETLIB/1` payload
formats between writer and reader — now a cross-library concern, since the
writer is in `kirn_driver` and the reader is compiled into `kirnrt`), and
§5.4's root-discovery (env override → exe-relative → install prefix), which
Phase 6 needs for the installer and which phase 1 does not block.

**Phase 1 — `kirnc` skeleton.** New binary with verbs `check|lex|parse|rt|run|
build` and `-E/-S/-c/-o/-###/-v`. `lex` and `parse` reuse the oracle code from
`kirnlex`/`kirnparse`; `check` reuses `kirncheck`; `rt` reuses the current
bundle path; `run` executes the bundled runtime. Keep old binaries as aliases.
**Goal:** Stand up the `kirnc` skeleton with `check|lex|parse|rt|run|build` and the `-E/-S/-c/-o/-###/-v` flags.
**Must Follow:** Reuse the extracted stage functions (no forked copies); reject unrecognized args explicitly instead of treating them as filenames.
**Important Note:** `run`/`rt` are deliberately unimplemented pending §9's oracle decision; the stop-at flags land in later phases.

```kn
fn add(a: Int, b: Int) -> Int {
    return a + b;
}

fn main() -> none {
    print(add(2, 3));
}
```

_Verify:_ `kirnc X` output == old tool output on the corpus; new CI job runs
the cross-check matrix.

_Status 2026-09-30: skeleton landed; `check`/`lex`/`parse`/`build` are real, `run`
is deliberately not implemented yet._ `tools/kirnc.cpp` is ~200 lines and links
`kirn_driver` in the 2 lines phase 0 made possible. The three stage verbs are
not reimplementations: `cmdLex`/`cmdParse`/`cmdCheck` were extracted from the
`kirnlex`/`kirnparse`/`kirncheck` main()s into the library, and those three
binaries are now thin front ends over the same functions, so the two paths
_cannot_ drift. `build` forwards to the existing `cmdBuild`, so the pipeline has
one implementation. `run` is rejected with an explanation instead of guessed —
see below.

Three things the research changed about this phase:

1. **"Reuse the oracle code" meant an extraction the plan did not schedule.**
   The four old tools shared _no_ code: each had its own private `readFile`,
   two incompatible `printDiags`, and `kirnrun` re-implemented `warnLegacyEnv`
   that the library already exported. So the phase required a third extraction,
   which is why the stage verbs live in `kirn_driver` rather than in
   `kirnc.cpp`.
2. **`readFile` is unusable for the stage verbs — a BOM trap.** The driver's
   `readFile` strips a UTF-8 BOM; the tools' copies do not. 7 files in
   `tests/negative/` start with a BOM, and `renderDiags` echoes the offending
   source line verbatim, so 7 recorded `check_*` goldens _contain the BOM
   bytes_. Calling the driver's `readFile` from `kirnc check` would render
   `" 1 | ;"` where the golden says `" 1 | <BOM>;"`. Hence a separate
   `readSourceVerbatim`. (The lexer strips a BOM itself, so token positions are
   unaffected — only the echoed line, which is exactly what the golden pins.)
3. **Seven cases can never be byte-identical, and must not be.** The usage and
   "cannot open" paths print the invoked tool's name, so `usage: kirncheck
<file.kn>` can never equal `usage: kirnc <file.kn>`. The cross-check
   normalises the program name for exactly those cases and requires everything
   else — including the exit code — to match byte for byte.

_Verification added:_ `scripts/crosscheck_linux.sh` is a **differential** oracle
(519 cases: 84 corpus files × 6 invocations, plus directory walks, missing
files, no-args and help paths). It is the only thing that can prove the
extraction did not fork the implementations. `scripts/golden_linux.sh` is the
**absolute** oracle (199 snapshots) and is what catches a change to the shared
front end. Both are now in `ci.yml`, which previously ran neither — `kirnlex`,
`kirnparse` and `kirnrun` had no CI coverage at all, and the plan's own verify
criterion depended on a harness that had never executed.

_Not done in this phase:_ `run` (see the open decision below), the stop-at-stage
flags `-E/-S/-c/-###/-v` (they are recognised and rejected with a pointer to the
phase that lands them, rather than silently treated as filenames, which is what
the old tools did with any unrecognised argument), and `rt`.

**Phase 2 — Preprocessor.** `src/prepro/` with a macro layer, `-D/-U`, and
`__FILE__`-style builtins. `kirnc -E` dumps the expanded token stream.
**Goal:** Land the preprocessor (`src/prepro/`) with macros, `-D/-U`, and builtins; `kirnc -E` dumps the expanded token stream.
**Must Follow:** Nim-style `{. … .}` syntax, capture-free hygiene, no lexer change, byte-exact goldens.
**Important Note:** First increment done; open are `-D/-U` beyond `-E`, per-file macro scope, and the normative grammar file is not yet updated.

```kn
{.fn DEBUG = 1.}

fn main() -> none {
    {.ifdef DEBUG.}
    print("debug build");
    {.else.}
    print("release build");
    {.endif.}
}
```

> Reference: Nim — `{. … .}` token-level directives.

_Verify:_ golden-file macro tests; macros cannot capture caller locals.

_Status 2026-09-30: first increment landed; the phase's headline syntax decision
had to be reversed before any of it could be written._ The plan proposed
C-style `#fn` / `#if` / `#undef`. That is not a clash to be negotiated but a
silent data-loss bug: `#` is Kirn's line-comment sigil
(`src/lex/lexer.cpp:186-189`), so the plan's directives were being discarded as
comments and the file still type-checked at exit 0. See §4.0 for the measured
demonstration and for why `{. … .}` was chosen instead.

Landed:

- `src/prepro/prepro.{h,cpp}` as `kirn_prepro`, between `kirn_lex` and
  `kirn_parser`. Two stages: a directive scan that applies
  `fn`/`undef`/conditionals and emits the surviving tokens, then a fixpoint
  expansion worklist. **No lexer change and no new `Tok` enumerator** — `{.`
  already lexes as two `Punct` tokens, so the layer is pure pattern-matching.
- Hooked into all four front-end paths: `frontEnd` (`kirn run`/`test`/`build`),
  `cmdCheck`, `cmdParse`, and `tools/kirnrun.cpp`. The last one is its own copy
  of the pipeline, and missing it produced a real divergence — a macro file
  type-checked under `kirn run` and failed to _parse_ under `kirnrun` with
  "expected expression, found '.'". Two tools disagreeing about one file is
  exactly the failure the phase-1 differential oracle was built to catch, and it
  was found by a phase-2 fixture instead.
- `kirnc -E` (both as `kirnc -E f.kn` and `kirnc check -E f.kn`), `-D`, `-U`, in
  both the joined and separated spellings, applied left to right so a later `-U`
  undoes an earlier `-D`. `-E` left `kLaterFlags`; `-D`/`-U` on a non-`-E` verb
  are now rejected explicitly, because they were previously _silently_ treated as
  filenames by `cmdLex` and silently accepted-and-ignored by `cmdCheck`.
- Hygiene as specified in §4.0: call-site parameters, definition-time
  alpha-renaming for everything else, with the two documented exemptions.

_Verification:_ 16 new goldens (`tests/macros/*.kn` through `-E` and `check`,
plus the `-D`/`-U` cases) and **9 new gates** asserting the properties rather than
only snapshotting them — that a macro cannot capture a caller local, that a
macro's own binding does not leak to the caller, that parameters bind at the call
site, that bodies compose, that a dead branch is elided, that `-U` undoes a `-D`
and that order matters, that self-reference is rejected by name, and that mutual
recursion cannot hang. A golden would happily record a macro that _did_ capture
a caller local; the gate fails if one ever does.

_The no-op property is measured, not asserted:_ `golden_linux.sh --record`
rewrote all 199 pre-existing goldens **byte-identically** when the preprocessor
was switched on. Final state: 215 goldens, 162 gates (153 + 9), 519 cross-check,
all green, with zero new compiler warnings (37 pre-existing, unchanged, and none
in `src/prepro/` or `tools/kirnc.cpp`).

_Not done in this increment,_ recorded rather than left to be discovered:

1. **`-D`/`-U` only reach `-E`.** The preprocessor runs in front of `check`,
   `parse` and `run`, but the flags are only parsed by `cmdPreprocess`, so
   `kirnc check -DFOO=1 f.kn` is still rejected rather than accepted. Threading
   them into the other verbs is mechanical and unblocked.
2. **Macros are per-file.** Each source is preprocessed with its own fresh table,
   so a macro defined in an imported module is invisible to the entry file, and a
   macro cannot gate an `import`. This is forced by the architecture, not chosen:
   modules are lexed independently in `collectImports`
   (`kirn_driver::collectImports`), in `Interpreter::loadModuleFile`
   (`src/interp/runtime.cpp`), and in the generated `kirnrt` launcher, each with
   its own `DiagEngine`. Sharing a table across those boundaries is a real design
   question — it makes `kirn_interp` depend on `kirn_prepro` — and is deliberately
   not guessed at here.
3. **No forward references.** A body may only name a macro defined earlier
   (§4.0). Consequence: mutual recursion is impossible rather than diagnosed.
4. **`{.elif.}`, `.` bodies and other syntax corners are gated but lightly
   covered**; `__TARGET__` is a `-D` away, not seeded.
5. The `grammar/kirn.ebnf` `COMMENT` production is untouched and still correct,
   but the new directive syntax is **not yet in the grammar file**, which
   declares itself normative. That is the outstanding must-do for this phase.

**Phase 3 — HIR/MIR.** `src/hir/`, `src/mir/` (arena CFG). `--emit-hir`,
`--emit-mir`. `kirnc rt` dumps MIR.
**Goal:** Land HIR (`src/hir/`) and MIR (`src/mir/`) with `--emit-hir`/`--emit-mir` and a `kirnc rt` dump.
**Must Follow:** Structural verification, not execution; HIR per-kind `std::variant`; MIR derived CFG with no hand-built SSA.
**Important Note:** Done — 53/53 corpus lower/verify/dump; the old "lower-verify vs direct run" criterion was unsatisfiable and was replaced.

```kn
fn classify(n: Int) -> Str {
    if n < 0 { return "neg"; }
    elif n == 0 { return "zero"; }
    return "pos";
}

fn main() -> none {
    print(classify(-1));
    print(classify(0));
    print(classify(7));
}
```

> Reference: CPython — ASDL-tagged IR; Ruby — flatten-late instruction list.

_Verify:_ AST→HIR→MIR preserves results on a corpus subset (run after
lower-verify vs direct run).

_Status 2026-09-30: landed — HIR and MIR both verify on all 53 corpus files._ Two
things about this phase were wrong in the plan before any of it could be
written, and one of them is a defect in the verify criterion itself.

**1. The verify criterion is not satisfiable as written.** It says _"run after
lower-verify vs direct run"_, which requires MIR to be executable. But §4.5 says
MIR "stays a separate specializer [from the bytecode compiler] ... MIR feeds
AOT", and phases 4 and 5 are LLVM codegen and linking — **no phase in this plan
introduces a MIR interpreter**. So the criterion asks for a thing the plan never
builds. `lower-verify` is also a term defined nowhere: it appears exactly once in
this repository, in that sentence. The criterion has been **replaced with
structural verification**, which is what LLVM (`Verifier.cpp`), Swift
(`SILVerifier.cpp`) and rustc (`hir_id_validator.rs`) actually use at the
equivalent stage, and which is honestly available when nothing executes yet.
LLVM's LangRef states the intended scope and it is the right one to adopt: the
verifier's violations "indicate bugs in transformation passes or input to the
parser" — it finds compiler bugs, it does not validate user programs.

**2. `rt` means two incompatible things.** §3.3 line 184, the note at 191-194 and
phase 1 at 1084 all read `rt` as "run through the runtime" (build a bundle and
execute it). Lines 115, 169, 872, 879 and this phase's own line 1208 read it as
"dump MIR". §3.4.5 and §3.4.12 group `rt` with the stop-at-stage verbs, which is
the dumper reading. **Resolved for this phase as the dumper**, because the
phase-3 text says so explicitly; the runner meaning is deferred to the phase
that retires the bundle (§6), where `run` and `rt` have to be settled together.

**3. The ownership split in §4.4 has no precedent and inverts the real
consensus.** §4.4 says "for HIR keep `unique_ptr` ownership to match house
style, revisit at MIR", and §4.5 says "this is the moment arenas arrive". The
surveyed evidence is the opposite: **rustc arena-allocates HIR too** (there is
no "box tree for HIR, arena for body IR" precedent), while Swift, MLIR and
Kotlin use pointer-stable trees for _every_ stage. The transferable rule is
**do not mix ownership strategies across stages** — which is the opposite of what
§4.4/§4.5 prescribe. HIR is therefore `unique_ptr`, matching the house style,
and **the MIR ownership decision must be made explicitly rather than inherited
from this paragraph.**

Two further plan terms did not survive checking and should not be cited:
"destructor-free" is **not a documented IR term** from any major compiler (it is
local coinage here), and LLVM is a counter-example — `llvm::Value` is non-copyable
_by design_. "Every instruction has at least one use" is **not an LLVM
invariant**; the LLVM verifier has no such check, so building it here would be
inventing a rule rather than copying one. And "LLVM SSA is in infinite use form"
could not be verified from any primary source; the citable subject is Braun et
al.'s sealed-block / pruned-SSA construction.

Landed — HIR (`src/hir/`, `kirn_hir`, between `kirn_sema` and `kirn_vm`):

- **A per-kind payload union, replacing the fat node.** `ast::Expr` is one struct
  with a kind tag and 20 members (5 owning pointers, 9 vectors) for any one of 24
  kinds; `ast::Stmt` has 43 members for 37. HIR uses `std::variant`, which is
  what Nim, Swift, Kotlin, LLVM and MLIR all do, and which makes the
  "field invalid for this kind" state unrepresentable rather than merely
  discouraged. The whole language has a mapping: all 24 `ExKind` and every
  body-level `StKind`; anything without one becomes `Item::Unhandled` carrying a
  note, never a silent drop.
- **Names resolved to `DeclId`s** in a `Context` that owns the table, mirroring
  Swift's `ASTContext` and rustc's interner, with a `NO_DECL` sentinel. Builtins
  are read from `sema::Checker::builtinNames()`, which this phase added, because
  two hand-maintained builtin name lists already exist (in `vm/compiler.cpp` and
  `interp/runtime.cpp`) and a third would drift.
- **Scopes are explicit `ScopeId`s on `Block` nodes.** This is not cosmetic: both
  interpreter bugs fixed earlier in this phase were "which frame does this
  binding go in" questions the tree could not answer.
- **`frontEnd` no longer throws the `Checker` away.** It was a local that died
  with the function, so every consumer needing resolved types re-lexed and
  re-parsed. There is now a 4-argument overload that hands it back.
- `--emit-hir`, and a `hir` verb: `kirnc hir f.kn` and `kirnc check --emit-hir
f.kn`. Verification runs unconditionally and is not a separate mode — a dump
  that verifies is worth something.
- `src/support/literals.h`: the underscore/`0x`/`0b`/`0o` integer and float
  decoders, promoted out of two duplicate copies (`parseInt` in
  `vm/compiler.cpp`, `parseIntText` in `interp/runtime.cpp`) rather than adding a
  third.
- `grammar/kirn.ebnf` still has no HIR/MIR content, which is correct — it is the
  surface grammar — but the normative file now has to be aware of the IR.

_Verification:_ **gate 8** adds four properties. **53 of 53** corpus files lower,
verify and dump. HIR does not lower code that failed type checking. The dump is
byte-for-byte deterministic across runs, without which a golden comparison means
nothing. And the verifier _rejects_ broken IR, via
`tests/hir/verifier_selftest.cpp` — eight cases compiled and run against the real
library, in the test tree and linked into no shipped binary.

Two of those four took a second attempt, and the reason is worth recording.

_The first verifier was wrong on first contact._ It had a hand-written table of
`std::variant` indices; it was wrong in nine places the instant it ran against
real input, and would have needed editing on every payload insertion. It was
replaced by a check that cannot drift — "the active payload implies its kind",
derived inside the same `if constexpr` arm that has to check it.

_The first rejection test was worse than no test._ It used a `.kn` fixture with a
construct HIR had no mapping for, on the reasoning that a lowering gap on
_checked_ input is the only defect a structural verifier can catch. That was
right about the scope and wrong about the mechanism: the test passed by asserting
"HIR does not model this yet", and the moment HIR learned the construct it went
**quiet**. It would have stopped testing anything without ever failing, and a
passing test that cannot fail reads as coverage. No valid source can produce a
violation at all — `sema::Checker` rejects every source-level defect before the
lowering runs, and HIR now maps the whole language — so the malformed IR is built
in memory and handed to `verify()` directly. It immediately found a real hole: the
`Lit` arm was **missing from the payload dispatch entirely**, so a literal payload
on a non-literal node was never checked. No corpus file can find that, because no
corpus file has a kind/payload mismatch.

_A third check was added because a coverage count lied._ A statement the lowering
cannot represent was emitted as a `Nop` with only a W9000 warning, so HIR silently
discarded code while the file still "verified". `payload::Nop` now carries
`dropped`, and `verify()` fails on it. The claim that unmapped constructs are
surfaced "never a silent drop" was simply false for statements, and this is the
check that makes it true — it found three more drops (`gather`, `bucket release`)
in the same corpus the coverage count had been reporting as fully passing.

_Not done, and recorded rather than left to be discovered:_

1. ~~MIR does not exist.~~ **Closed.** `src/mir/` as `kirn_mir`, linking `kirn_hir`
   and deliberately _not_ `kirn_vm`/`kirn_interp`, since §4.5 keeps the bytecode
   compiler a separate specializer. `--emit-mir` and a `kirnc rt` verb; both lower
   the whole way (HIR is verified on the way through, because a broken HIR makes a
   broken CFG and diagnosing it a layer earlier is much cheaper).

   Nothing in this codebase had a CFG before: the bytecode is a flat stream whose
   only control flow is a relative offset, and `SasmEmitter` emits symbolic labels
   with no machine representation. So the blocks are genuinely new, and three
   design points follow the survey rather than the plan:

   - **Successor edges are derived, never stored**, as LLVM does — there is no
     `succs` field to disagree with a terminator. `predsOf()` recomputes them, and
     that recomputation is what the phi check compares against.
   - **Entry is `id() == 0`, not a flag.** LangRef: the entry block "is not allowed
     to have predecessor basic blocks ... it also cannot have any PHI nodes."
   - **No SSA construction.** §4.6's own guidance is that the front end "never
     builds SSA by hand; it emits allocas + stores, and the `mem2reg` pass
     produces SSA", so MIR is alloca/load/store shaped and Phase 4's LLVM backend
     gets SSA from mem2reg. Locals are therefore memory-resident and a read is a
     `load`; the one place a phi is genuinely required is a value that reaches a
     join block _differently_ along different predecessors, which is also the only
     case where a naive builder gets it wrong. A binding written the same value on
     every arm gets no phi, and that is checked rather than assumed.

   The builder emits into the entry block by **prepending**, and `emit()` returns
   `{block, index}` because the auto-seal can land an instruction in a block other
   than the caller expected — recording `size()-1` and then indexing the caller's
   own `cur` patched the wrong instruction, and aborted.

   **The verifier found six real defects on first contact**, none of which any
   corpus file could have: three terminators landing mid-block (a `return` followed
   by more statements in the same block; an `if` arm's tail branch sharing a block
   with the next condition; hoisted allocas appended after the entry block's
   terminator), every `elseSucc` in an `if`/`elif` chain left at `NO_BLOCK`, a
   `do`-`while` that reused the current block as its loop head so the back edge
   targeted block 0, a `continue` that would have gone to the loop _exit_, and a
   `load` that allocated a value without claiming it. The last is the instructive
   one: the operand index was in range, so every other check passed and the CFG
   looked entirely plausible. Hence the value-density invariant — every value
   defined exactly once — which is rustc's `ItemLocalId` density check applied to
   the value table.

   _Verification:_ gate 9 adds four properties, mirroring gate 8: **53 of 53**
   corpus files lower through HIR to a verified CFG; the verifier _rejects_ broken
   CFGs via `tests/mir/verifier_selftest.cpp` (14 in-process cases); the dump is
   byte-identical across runs and starts with `mir 1`, confirming `rt` is a dump
   rather than an execution; and — deliberately redundant with the library — every
   dumped block is re-checked for a terminator **from the text**, so a flag wired
   to the wrong function is caught even when the library is fine. A `.kn` fixture
   would repeat the trap gate 8 already fell into.

2. ~~9 of 53 corpus files did not lower.~~ **Closed: 53 of 53 lower, verify and
   dump.** The list `gates_linux.sh` carried by name is now empty, and closing it
   found seven _defects_ rather than seven missing cases:
   `from mod import a as b` binds each item, not the module; select-arm and
   `catch (e)` bindings are declarations; `t @ pat` and `..name` bind names that
   live outside a pattern's `elems`; `temp`/`bucket` bind usable names, and a
   bucket's name is in `tempVarName`, not `name`, because the parser reuses that
   field; the Checker keeps pseudo-modules in `importRoots_` and _not_ in its
   scope, so `io.open(...)` resolved against nothing; `gather` binds an implicit
   `item` for its body; and `bucket release` needed a real representation.
   Two of these were also latent bugs in the harness rather than the lowering:
   `cmdHir` gated on `diags.count()`, so a W0105 shadowing warning stopped the
   lowering before it ran (the scoping fixture exited 1 having lowered nothing),
   and an open-ended range `a..` is a Binary with a _null_ rhs and is legal
   (`OP_UNARANGE`), so flagging every null rhs rejected examples/30.
3. **Struct field types keep the spelling; everything else resolves.** Function
   signatures, parameters and `var`/`const` annotations now carry _resolved_ types,
   so MIR dumps a real `ret=int` rather than `ret=?`. `sema::Checker::resolveType()`
   and `resolveSig()` are public for this. **The obvious way to use them is
   wrong**, and the way it is wrong is worth recording: both are pure — they read
   `typeVars_` and return interned types — but `typeVars_` is pushed and popped by
   a `TvGuard` around each generic declaration, so calling them after
   `checkModule()` returns "unknown type 'T'" for every generic function. Purity
   was never the issue; the _environment_ was. The Checker therefore caches each
   function's signature where it is computed, inside the `TvGuard`, and HIR reads
   that back (`Checker::funcSig`). Struct fields now have one too: `Checker::fieldTypes()` returns the resolved
   types, cached inside the struct's own `TvGuard` scope in `registerTopLevel`,
   which previously did not resolve field types at all. A generic struct's fields
   therefore dump as `first : A` rather than as the spelling.

   Making the signature real also unmasked a latent null dereference in the MIR
   builder: `buildFunc` computed the return type one line _above_ `fn = &f`, and
   `tyOf` interns into `fn->types`. That was safe only because `sig.ret` was
   always null, so the line never ran. A field that is always empty hides the bug
   in the code that would otherwise have caught it.

4. **A lost FIELD is a different bug shape from a lost statement.** `verify()`'s
   drop check makes an unmappable _statement_ a hard error. It cannot catch a lost
   _field_: the node is present and well formed, the value is just wrong. HIR was
   dropping `ast::Pat::inclusive`, so `case 200..=299` became `case 200..298` --
   three corpus files use inclusive range patterns and nothing noticed. The
   per-kind payload union was supposed to make "nowhere to put it" impossible; it
   was possible, because the IR payload simply lacked the field. It is now carried,
   printed in the dump as `range 1..=9` / `range 10..99`, and pinned three ways:
   behaviourally (`describe(99)` is `large` because `10..99` is exclusive, while
   `describe(9)` is `small` because `1..=9` includes it), in the HIR text, and in
   the MIR comparison chosen (`<=` for inclusive, `<` for exclusive).
   `tests/mir/patterns.kn` exists because the corpus had **no** exclusive range
   pattern at all, so the bit was entirely untested.

5. **What MIR does not model, precisely.** These are fidelity limits, not
   failures: the IR verifies, and none of them drops a statement — the drop check
   in `verify()` guarantees that — but they are places where the CFG is coarser
   than the program.

   - ~~**`match` arm selection.**~~ **Closed for the patterns expressible as
     comparisons.** The subject is evaluated once into a value and each arm tests
     _that value_ against its own pattern, so arms are genuinely selected rather
     than all reachable. `wild`, `bind`, `literal`, `range`, `bindalias`, `or`,
     `ref` and fixed-arity `tuple` lower to real `binary`/`condbr`; the value form
     emits a join phi only when every arm produced a value, never an invented one.
     `ctor`, `slice` and tuple-with-rest still emit a named `pattern_test:<kind>`
     call, because they need a decision tree over a tag or a length that this IR
     has no operation for. That marker is deliberate: it makes the IR say "a
     backend must implement a decision tree here" in a form a reader cannot
     mistake for a comparison. Branching to the first arm instead would verify, and
     be wrong. Corpus-wide: 59 real pattern comparisons, 16 named markers.
   - ~~**`goto` and labels.**~~ **Closed.** A label registers the block it names;
     a `goto` is an unconditional `Br` to it, back-patched when the label appears
     later in the source, which is the normal shape. **No unwinding is needed**,
     and that is the alloca model paying for itself: bindings live in entry-block
     allocas, so unlike the interpreter -- which throws `SignalGoto` and unwinds to
     the owning block -- there is no frame for the jump to leave. Gate 9 asserts a
     forward goto out of a loop emits a branch and no `unreachable`.
   - **`bucket`.** HIR models `bucket x = v` as an ordinary binding and
     `bucket release x` as a self-assignment, which is consistent but loses the
     separate store the construct is for.
   - **Comprehensions and lambdas** become `call_indirect` with their operands
     flattened; the closure body is not built as its own MIR function. This one is
     _deliberately_ left, and the reason matters more than the gap. A closure in
     this language captures **by reference** (`examples/07_closures_captures.kn`), so
     a nested function would need the enclosing frame's allocas passed as pointers:
     a closure environment, and a memory model for it. Building one that captures
     **by value** would be easy, would verify, and would be wrong for any closure
     that mutates a captured variable. A subtly incorrect IR that passes every
     structural check is precisely the failure this phase's verifier work exists to
     prevent, so the flattened call stays: visibly coarser rather than quietly
     wrong. The right home for that model is Phase 4, alongside the LLVM backend's,
     where `mem2reg` decides what actually escapes.

6. ~~Multi-target assignment infers its arity.~~ **Closed.** `ast::Stmt` gained
   `assignTargets` and the parser now records `targets.size()`. The split is not
   recoverable from `exprs`: three entries could be two targets and one value
   (`x, y = point`) or one target and two values, and the `size/2` guess got
   `x, y = point` wrong.
7. **Nested named functions are lowered but reach `Module::funcs` via a parallel
   `items` entry**, which is inelegant; a real nested-function item form is
   cleaner.

**Phase 4 — LLVM codegen (opt-in).** `KIRN_WITH_LLVM=ON`; MIR→LLVM IR for the
scalar subset; `--emit-llvm`; `mem2reg` for SSA; `-fbackend=llvm`.
**Goal:** Add opt-in LLVM codegen (`KIRN_WITH_LLVM`) lowering verified MIR to `llvm::Module` for the scalar subset.
**Must Follow:** Keep `kirn`/driver libraries LLVM-free; `mem2reg` for SSA; emit clean IR that passes `opt -verify` and runs under `lli`.
**Important Note:** First increment done; the scalar subset is reachable only via annotations, and `-fbackend=llvm` answers with the phase-5 pointer.

```kn
fn square(x: Int) -> Int {
    return x * x;
}

fn main() -> none {
    print(square(6));
}
```

> Reference: LLVM — `mem2reg` + new pass manager.

_Verify:_ emitted `.ll` passes `opt -verify` and `lli`; scalar corpus programs
match interpreter output numerically.

_Status 2026-10-06: first increment landed._ `src/backends/llvm/emit_llvm.cpp`
lowers verified MIR to an `llvm::Module` for the scalar subset
(int64/double/bool/void), guarded by `KIRN_WITH_LLVM=ON` and linked only into
`kirnc` behind `KIRN_HAS_LLVM` — the `kirn`/driver libraries stay LLVM-free, so
the default build is unchanged. `kirnc llvm <f.kn> [-o f.ll]` (and the
`--emit-llvm`/`-emit-llvm` alias) emit a verified module; a no-LLVM build prints
a one-line explanation and exits 2. The driver gained `produceMir()`
(`rt` and `llvm` now share one front-end→verified-MIR pipeline; `cmdRt` was
refactored onto it with byte-identical output — 215/215 goldens hold). The
headline's fifth item, `-fbackend=llvm`, is a recognised selector: run/build
answer it with the phase-5 pointer (exit 2), every other verb accepts it as a
compatible no-op, and an unknown value is rejected — so `-fbackend=llvm` is
never mistaken for a filename.

Two design facts were not in the plan and had to be discovered on the ground.
(1) The scalar subset is reachable only through **annotations**: param types
(`x: int`) and value returns (`-> float`) must be declared, and void functions
must be written `-> none` — an unannotated function resolves its return type to
`any` and is rejected with a diagnostic, never guessed. Locals infer scalar
types from their initializer (`var x = 7;` → i64), but a call whose callee is
unannotated poisons the local to `any`. The plan's `print` formatting contract
comes from the interpreter's toStr, and the LLVM side implements it as an
internal `kirn.fmt_f64` (%.15g/%.16g/%.17g round-trip + a '.'/'e' scan, so
"6.0" prints as "6.0" not "6"). (2) `print` in the interpreter builds its line
with a NUL-terminated buffer and `puts`; the LLVM `print` reuses that shape, and
the float formatter must NUL-terminate its fixed texts or `puts` overruns.

Gate 10 added: `tests/llvm/p*.kn` must emit clean, pass `opt -passes=verify`
`-disable-output`, and produce `lli` stdout+exit byte-identical to the
tree-walker; `tests/llvm/n*.kn` must be rejected with a diagnostic (non-scalar
locals, `any` returns). A build without LLVM skips gate 10 rather than failing
it. Current suite: gates 185/185, goldens 215/215, exit-code parity 42/7/0 and
identical stdout on the gate-10 fixtures.

**Phase 5 — Assemble + link.** In-process TargetMachine emission (`-c`, `-S`)
and LLD linking (`-o`). `-###` prints every command; cross targets emit correct
machine headers.
**Goal:** Make assembling and linking real stages — in-process TargetMachine (`-c`/`-S`) and LLD (`-o`), with `-###` printing every command.
**Must Follow:** One emission path for all targets; cross targets emit correct ELF/PE/Mach-O machine headers.
**Important Note:** Reuse the release-workflow machine-header checks for all six release targets.

```kn
extern fn puts(s: *Char) -> i32;

fn main() -> none {
    unsafe {
        puts(c"hello, kirn");
    }
}
```

> Reference: LLVM — LLD; Zig — `-femit-asm` / `-femit-bin` stage flags.

_Verify:_ `kirnc -c` → object; linker output runs `hello` + `pet.math`; reuse
release-workflow machine-header checks for all 6 release targets.

**Phase 6 — `kirn` integration + shipping.** `kirn check`; `kirn build`/`kirn
run` delegate to `kirnc`; `kirn -v/--verbose`, `kirn version`, `kirn doctor`;
warn-and-remove for `kirnlex/parse/check/run`; retire the bundle as default on
AOT-capable hosts (keep `--fbackend=runtime`).
**Goal:** Integrate `kirnc` into `kirn` and ship a clean installed two-tool tree.
**Must Follow:** `kirn build/run/check` delegate to `kirnc`; remove the oracle tools; retire the bundle as the AOT default on capable hosts.
**Important Note:** Must work from an installed tree with no source checkout — needs §5.4 root discovery plus installer/CI/release updates.

```kn
fn main() -> none {
    print("hello from an installed kirn");
}
```

_Verify:_ `kirn new` + `kirn build` + run works from a _clean installed_ tree
(via installer) with no source checkout; update `install.sh`, `ci.yml`,
`release.yml` for `kirnc`; full release dry-run; end-to-end installer test.

---

## 7. Testing & verification strategy

**Goal:** Define the correctness floors and oracles (corpus gates, goldens, cross-checks) every phase must pass.
**Must Follow:** Add a bench harness that measures `kirn build`/run against the Rust/Go equivalent for each phase.
**Important Note:** Exit codes are load-bearing (1 = check errors, 64 CLI, 65 type errors, 66 no entry, 70 internal); goldens catch the whitespace/ordering changes the gates miss.

- **Corpus gates** (`scripts/gates_linux.sh`) stay the correctness floor at
  every phase; phase 3+ adds pipeline property tests.
- **Golden snapshots** (`scripts/golden_linux.sh`, added with phase 0) are the
  byte-level oracle the gates are not. The gates assert exit codes, substring
  greps, and exactly one stdout comparison, so they stay green while
  whitespace, ordering, and stderr wording change freely — and `kirnlex` and
  `kirnparse` have no CI coverage at all. The golden suite records argv,
  stdout+stderr interleaved, and the exit code for all 199 cases, so
  "behavior-preserving" becomes a checked claim. Re-record with `--record`
  only as a deliberate act: every diff is a behavior change that needs a
  reason. One gotcha it taught us: `kirn test` discovers tests from the CWD and
  `stdlib/pet/io_test.kn` writes a temp file, so a probe that leaves state
  behind makes the _next_ run's snapshot differ; the harness cleans up the way
  `gates_linux.sh` does.
- **Pipeline cross-check tools** (heavy reuse of the "two oracles must agree"
  - rename-gate CI trick): for each phase, a script that runs the same input
    through old and new paths and diffs output (token dumps, AST dumps, exit
    codes, program stdout).
- **Golden files** for prepro (`kirnc -E`), HIR, MIR, LLVM IR dumps under
  `tests/`.
- **Machine-header checks** for `-c`/link output on cross targets (the
  release-workflow pattern already exists: ELF/PE/Mach-O).
- **REPL-checks**: `kirnc run` vs `kirn run` identical stdout on the corpus.
- **Failure exit codes** are load-bearing for the gates: today exit 64 = usage
  (`kirn_driver::cmdBuild`), 66 = no entry point (`:3223`); `kirnc check`
  should use exit 1 and keep 64 for CLI errors.
  _Corrected 2026-09-30:_ this previously said exit 5 "cargo-check-like".
  Measured across the real drivers, 1 is the convention and 5 was never one:
  gcc/clang exit 1 for `-fsyntax-only`; GCC's documented non-1 code is 4 (an
  internal compiler error, and only under `-pass-exit-codes`); `nim check` exits
  1 (`msgQuit(int8(conf.errorCounter > 0))`, and `cmdCheck` is not special-cased);
  rustc exits 1 on compile errors and 101 on panic; and `cargo check` exits
  **101**, not 1 or 5 — cargo funnels every library-side error through
  `From<anyhow::Error> for CliError`, so 101 is a deliberate "this is a cargo
  bug, file a report" namespace rather than a per-verb code. So: 1 = "check found
  errors" (matches every driver and kirn's own existing `return 1` sites),
  70 stays `EX_SOFTWARE` for internal errors, 65/66/64 keep their sysexits
  meanings. A note on sysexits itself: the FreeBSD man page now marks the
  interface deprecated, but the values remain the clearest available names, and
  kirn already uses them.

---

## 8. Risks and decisions

**Goal:** Record the risks (LLVM weight, dynamic-vs-SSA, runner staleness, installer coupling) and the decisions that bound scope.
**Must Follow:** Keep LLVM opt-in (`KIRN_WITH_LLVM`); AOT covers the typed/scalar core first, dynamic features keep the runtime path.
**Important Note:** These decisions bound the phase plan; do not silently expand scope.

1. **LLVM weight.** Real LLVM (via `find_package(LLVM)`) is a large, slow,
   disk-heavy dependency; the repo deliberately avoids it today. Mitigation:
   opt-in `KIRN_WITH_LLVM`, in-process libs on dev hosts, and for _release
   builds_ the existing llvm-mingw already ships clang/lld, so cross-target
   release CI stays feasible. Decision: **near-term default remains the
   runtime/C++ backends; LLVM is the named destination, toggled by
   `KIRN_WITH_LLVM`.**
2. **Dynamic semantics vs. SSA.** The runtime value model is dynamically
   typed (`src/interp/value.h`). Real SSA only makes sense for the statically
   typed subset. Plan: AOT/LLVM covers the typed/scalar core first (same
   subset the C++ backend already proves), and expands; dynamic features keep
   the runtime path. Empirically verify coverage % on the corpus.
3. **Bundled-interpreter staleness.** `kirnrt` is built on demand by the
   _driver_ via `ensureRunner` (`:2121`); once `kirnc` exists, `rt` and `run`
   must agree on which runner is authoritative. Consolidate to one builder.
4. **Installer/release coupling.** `install.sh:169-170` asserts exactly
   `bin/kirn`; `ci.yml` and `release.yml` enumerate shipped binaries. Adding
   `kirnc` touches all four and ships `kirnc` + (temporarily) `kirnrt` per
   archive.
5. **PATH-driven `g++` in `-###`.** If LLVM is in-process, `-###` prints
   pseudo-commands; keep real shell-outs only where unavoidable (C++ backend).
6. **`-O` dead on POSIX.** Fix or remove (`kirn_driver::buildProgram` returns before
   `:2835`). Align `-O` semantics with object emission, not MSVC-only.

---

## 9. Open questions (need user confirmation)

**Goal:** Collect the questions that need the language owner's confirmation before their phases can be finalized.
**Must Follow:** Keep unresolved items visible (meaning of `rt`, macro scope, C/C++ driver backend, LLVM policy, AOT promise) rather than guessing.
**Important Note:** The `rt`/`run` contradiction is the sharpest — phase 1 deliberately implements neither until it is answered.

1. **Meaning of `rt`.** The plan reads it as "run through the runtime"
   (bundle-and-interpret), distinct from `run` (full AOT). Alternative
   readings: "run tests", or "register-transfer/IR dump". Please confirm.

   _The plan is self-contradictory here, and phase 1 found it the hard way._
   §3.4 and phase 1's own text say `rt` = "run through the runtime"; but §3.1
   (line 115), §4.5 (line 799) and phase 3 (line 1061) all assign the MIR dump
   to `kirnc rt`. Both cannot hold. Worse, the neighbouring verb is equally
   undefined: `run` has **three** incompatible existing behaviours, and the
   plan never says which one `kirnc run` must match —

   | tool           | type errors on             | exit | summary line | panic frames |
   | -------------- | -------------------------- | ---- | ------------ | ------------ |
   | `kirnrun`      | stdout                     | 1    | `N error(s)` | yes          |
   | `kirn run`     | stderr                     | 65   | none         | no           |
   | bundled binary | _(nothing printed at all)_ | 65   | none         | no           |

   So "kirnc X output == old tool output" is currently unanswerable for
   `run`/`rt`, and phase 1 therefore implements neither. `kirnc run` exits 2
   with an explanation instead of silently picking a winner — baking a guess
   into the driver would be worse than a visible gap. **Decision needed:**
   which column is the oracle for `run`? My recommendation is the `kirn run`
   column (stderr, 65, no summary), because `kirn run` is the documented user
   surface and 65 is already load-bearing for `gates_linux.sh`; and `rt` should
   mean "run through the runtime", with the MIR dump owned solely by
   `--emit-mir` from phase 3, which removes the contradiction with lines
   115/799/1061.

2. **Macro scope.** Token-level macros (Nim-style, can synthesize syntax) vs
   expression-level only. The plan assumes token-level `#fn`.
3. **C/C++ driver backend.** Wrap the _system_ compiler (whatever
   `clang`/`gcc`/`cl.exe` is found, Zig model) vs bundle its own clang. The
   plan assumes wrap-the-system-compiler for now, LLVM-clang later.
4. **LLVM dependency policy.** Confirm `KIRN_WITH_LLVM=ON` is acceptable for
   release builds (larger toolchain; cross toolchains already provide
   clang/lld).
5. **Version of the AOT promise.** v0.1.0 = LLVM for scalar subset; dynamic
   features stay on runtime until further notice — acceptable?
