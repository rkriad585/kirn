# KIRN Cross-Language Integration Plan (`KIRN_CROSS_PLAN.md`)

## Goal

Kirn's cross-language surface must let one minimal, AI-first program import, call, and embed
**C, C++, Rust, Go, Ruby, and Python** through a single industry-standard boundary — a typed C-ABI
dynamic-library path for compiled languages and a co-resident-interpreter path for scripting ones —
sharing one marshalling layer so every language behaves identically. It must be faster than Rust and
Go across that boundary: no over-engineered bridging runtime, no per-call allocation storms, and
generated, machine-consumable binding tables instead of hand-maintained glue.

## Must Follow

- The toolchain build ships **exactly two executables**: `kirn` (driver) and `kirnc`
  (checker/compiler). Foreign compilation/embedding is a _mode_ of these two, never a third binary.
- **No over-engineering:** adopt the smallest proven ABI/interop pattern already shipped by the
  reference languages (C ABI + `dlopen`, cgo `//export`, ctypes, Nim `{.importc.}`) rather than
  inventing a new bridging runtime.
- **AI-first:** every foreign-declaration inventory (symbol/signature table) must be
  machine-consumable — generated bindings, never hand-written glue.
- **Kirn style:** minimal; lowercase methods; CapCase types and free builtins (`Str`, `Int`, `Float`,
  `Bool`, `Char`, `Ptr`); width micros `i64`/`u64` alongside primitives `Int64`/`UInt64`.
- **Performance:** FFI call overhead and cross-boundary copies must beat the reference
  implementations, measured with `kirn bench`.

## Important Note

The existing structure and ratified decisions below are preserved in force; `KIRN`/`kirn`/`.kn` are
the pre-rename names for Kirn/`kirn`/`.kn`, and every existing `extern fn`, `use import`,
marshalling, toolchain, and phase decision stands unchanged. This is the cross-language / FFI /
embedding / native-linking plan — not a source-to-source transpiler.

**Status:** Proposed design (not yet implemented). Living document.

**Audience:** a developer / coding agent who will implement cross-language interoperability in the
existing KIRN project, phase-by-phase.

---

## 0. Executive Summary

**Goal:** import, call, and embed **C, C++, Rust, Go, Ruby, and Python** from Kirn using two proven
architectures (C-ABI dynamic libraries; embedded interpreter) plus a subprocess fallback.

**Must Follow:** reuse the existing `extern fn`/FFI plumbing and `tools/kirn.cpp` C++ toolchain
integration; do not duplicate foundational pieces (Section 2) or add a third architecture.

**Important Note:** implement **Phase 1 (C)** first — it validates the whole runtime-FFI + syntax +
build + marshalling foundation and every other language is a delegate of that machinery.

KIRN needs to import, call, and embed code from **C, C++, Rust, Go, Ruby, and Python**. This plan
defines that capability from the ground up, reusing KIRN's existing `extern fn`/FFI plumbing and
its build-time C++ toolchain integration as much as possible, and clearly separating:

- **What already exists** (Section 2) — foundational pieces we build _on_, not _duplicate_.
- **What is new** (Section 3) — the FFI runtime, the foreign-import syntax, and per-language
  backends.

Because the six languages have very different interop models, the plan uses **two complementary
architectures**, with a clear rationale for each:

| Architecture                                  | When used                           | Mechanism                                                                                                                           |
| --------------------------------------------- | ----------------------------------- | ----------------------------------------------------------------------------------------------------------------------------------- |
| **A. C-ABI dynamic libraries** (compiled FFI) | C, C++, Rust, Go                    | Compile foreign code to a shared library exposing `extern "C"` symbols; KIRN `dlopen`s it at runtime and calls through typed shims. |
| **B. Embedded interpreter** (co-resident VM)  | Ruby, Python, Go (interpreter mode) | Link the language's embedding API into the KIRN binary; KIRN drives it via its C API (`PyObject_Call*`, `rb_funcall`, …).           |

Where a language is inconvenient to embed natively (Python ABI pinning, Ruby init lifecycle, Go
need for subprocess isolation), the plan additionally supports **C. subprocess/low-level pipe**
execution as a fallback. The build tool (`tools/kirn.cpp`) already shells out via `std::system()`
(Section 2.7), so this pattern reuses existing machinery.

> **Recommendation:** implement **Phase 1 (C)** first. It validates the entire runtime FFI +
> syntax + build + marshalling foundation on the simplest case, and every other language reuses
> that foundation.

---

## 1. Goals & Proposed Syntax

**Goal:** fix the user-facing `use import` / `use from` / `use <lang> { … }` surface Kirn developers
write against.

**Must Follow:** fold every foreign construct into the existing module/import resolver — no new import
pipeline, no new declaration AST.

**Important Note:** `main` is a **module stem** (file name without extension); the `:` is a
**language tag**, ratified in Section 2.4.

### 1.1 Import external source files

**Goal:** bind an external `<stem>.<ext>` source file into a forwards-typed module root.

**Must Follow:** reuse `importRoots_` (checker) and `loadedModules_` (runtime); only a new
`stdlib/lib/<lang>/` search space plus a foreign-loader fallback is new.

**Important Note:** `use from c:main import x;` binds `x` directly, mirroring `from text.slug import
slugify`; symbol signatures come from `extern fn` or discovery (Section 6.3).

```kirn
# Whole-module import: binds the module under its stem name
use import cpp:main;      # -> main.cpp
use import c:main;        # -> main.c
use import rust:main;     # -> main.rs
use import go:main;       # -> main.go
use import ruby:main;     # -> main.rb
use import python:main;   # -> main.py

# Named-symbol import (functions/classes/vars/consts)
use from cpp:main import something;
use from c:main import something;
use from rust:main import something;
use from go:main import something;
use from ruby:main import something;
use from python:main import something;

# Use the imported module namespace
main.some_function();
main.some_class();
value = main.some_variable;
```

Semantics (shared with KIRN's existing `import`/`from import`, Section 2.4):

- `use import c:+` / `cpp:+` etc. resolve `<stem>.<ext>` relative to the importing `.kn` file and
  the standard library search paths, then register **module root** `main` in the checker
  (`importRoots_`, `checker.cpp`) and interpreter (`loadedModules_`, `runtime.cpp`).
- `use from c:main import x;` binds the name `x` directly (mirrors `from text.slug import
slugify`).
- The `:` on the left of `main` is a **language tag**; the right-hand name is the **module stem**
  (file name without extension), exactly as the proposal states.

### 1.2 Embedded cross-language code

**Goal:** let `use <lang> { … }` embed foreign source directly in a `.kn` file and expose its tagged
symbols.

**Must Follow:** capture the brace-block as raw text without interpreting the foreign language's own
braces/comments (same mechanism for all six languages).

**Important Note:** ratified behavior — the block (a) verifies the toolchain, (b) compiles or
registers with the embedded interpreter, and (c) exposes `//export`/`extern "C"`/`@export` symbols
(or all top-level defs for scripts) as module members; the trailing `print_hello("kirn");` works
because `print_hello` becomes a callable module member.

```kirn
use cpp {
    # C++ code (e.g. `extern "C"` exported fn)
}
use c {
    /* C code */
}
use rust {
    // Rust code
}
use go {
    // Go code
}
use ruby {
    # Ruby code
}
use python {
    import something
    print("hello python from kirn")
    fn print_hello(name):
        print(f"hello {name}")
}

print_hello("kirn");
```

`use <lang> { … }` is a **declaration** that: (a) extracts the raw embedded text, (b) verifies the
needed toolchain/runtime is installed, (c) compiles it to a shared library (compiled languages) or
registers it with the embedded interpreter (script languages), and (d) exposes any
`//export`/`extern "C"`/`@export`-tagged symbol (or all top-level defs for scripts) in the module
namespace. The example's trailing `print_hello("kirn");` works because the Python `print_hello`
fn becomes a callable module member.

---

## 2. Existing KIRN functionality we reuse (ground truth from source)

**Goal:** catalogue the exact source-verified pieces Kirn already has so cross-language work _builds
on_ them.

**Must Follow:** reuse, never duplicate — each item below is confirmed by direct source inspection.

**Important Note:** anything cited here already exists and is _not_ new work; everything new is
Section 3+.

> Verified by direct source inspection. Anything relied on below already exists and is _not_ new
> work. New work is Section 3+.

### 2.1 `extern fn` / FFI declaration plumbing

`extern fn printf(fmt: *char, ...) -> i32;` is parsed as a `FuncDef` with `externDef=true`
(`src/parser/parser.cpp:287-293`), registered with full signature but body-checking skipped
(`src/sema/checker.cpp:329, 2240-2256`). Pointer types `*T` exist in the type system
(`src/sema/type.h:30,92`; `parser.cpp:1478-1497`). **Avoids inventing a new declaration AST.**

> Caveat: today only `printf`/`strlen` are actually bound (hardcoded `biFn` lambdas,
> `runtime.cpp:823-865`); there is **no dynamic-loading**. Making `extern fn` resolve to a
> dynamically loaded symbol is the core new FFI work (Phase 3).

**Goal:** reuse the existing typed `extern fn` declaration surface for foreign symbols.

**Must Follow:** keep `extern fn` as the single declaration AST — no new foreign-signature
mini-language.

**Important Note:** ratified — `printf`/`strlen` hardcoding must keep working while dynamic
resolution is added in front of it (Phase 3).

> Reference: Rust — cdecl `extern "C"` ABI and `c_char`/`c_void` naming precedent in
> `rust/library/std/src/ffi/mod.rs` (verified) for zero-cost C-compatible types.

A minimal Kirn `.kn` binding in this exact surface:

```kn
extern fn puts(s: *Char) -> Int;       # parsed as FuncDef, externDef=true
extern fn strlen(s: *Char) -> UInt;    # pointer types *T already in the type system
print(strlen(c"hello"));                # 5
```

### 2.2 Value representation & builtin calling convention

**Goal:** pin the one calling convention every FFI shim and embed driver must speak.

**Must Follow:** pass `std::vector<Value>&` and wrap results with `Value::integer/floating/…` via
`biFn`.

**Important Note:** no new marshalling vector is needed to talk to KIRN — the shim must use this
exact convention.

Runtime `Value` is a fat tagged union ($\approx$472 B) with `VK` tag and inline `i/d/b/ch/s`
fields (`src/interp/value.h:31-156`); strings are `std::string` in `.s`; containers are
`shared_ptr` (refcounted). Values are passed to builtins as `std::vector<Value>&`, results wrapped
with `Value::integer/floating/…` via `biFn` (`value.h:44`, `runtime.cpp:730-877`). Any FFI shim
**must use this exact calling convention** — no new marshalling vector is needed to talk to KIRN.

### 2.3 `c"..."` strings, `c_ptr()`, `VK::Ptr`

**Goal:** keep the current `c"..."` / `.c_ptr()` user-facing pointer surface.

**Must Follow:** extend it to expose a real address (Phase 3) while keeping back-compat with the
`std::string`-holding `VK::Ptr` today.

**Important Note:** this is the ratified handle for string/byte data crossing toward C ABI — do not
replace it with a new syntax.

`c"x"` yields a `VK::Ptr` value holding `std::string` (`runtime.cpp:1964-1968`); `str.c_ptr()`
returns `*char` typed as `VK::Ptr` (`runtime.cpp:3326-3332`, `checker.cpp:1345-1347`). This is the
user-facing handle for passing string/byte data toward C ABI. **We extend it** to expose a real
address (Phase 3) but keep the existing surface for back-compat.

### 2.4 Module import system

**Goal:** make `use import`/`use from` a thin fold over the existing import resolver.

**Must Follow:** add only a `stdlib/lib/<lang>/` search space and a foreign-loader fallback — change
no existing resolver logic.

**Important Note:** `:` is a language tag and the right-hand name is the module stem — this is
ratified; `importRoots_`/`loadedModules_` remain the registration points.

`import`/`from … import` with aliases (`parser.cpp:753-810`), dotted names, dynamic module roots
(`importRoots_`, `checker.cpp:655,774-790,2262`), module value caching (`loadedModules_`,
`runtime.cpp:1176-1241`), package-entry resolution (`pin.kn`/`mod.kn`, `resolvePackageEntry`
`runtime.cpp:1131-1173`), and search paths via `libDirsFor()` (`tools/kirn.cpp:356-379`,
`$KIRN_LIBS`/`$KIRN_STDLIB`). **The `use import` syntax folds into this resolver** — new code only
needs to add `stdlib/lib/<lang>/` as a search space and a foreign-loader fallback.

### 2.5 Native build: C++ launcher + host compiler

**Goal:** extend, not replace, `kirn build … --native` to carry foreign objects.

**Must Follow:** add foreign `.o`/`.lib`/`.a` artifacts and link flags to the existing GNU/MSVC
compile/link commands.

**Important Note:** no second build system; `tools/kirn.cpp:2342-2668` is the extension point for
C/C++ linking (Phase 5).

`kirn build … --native` emits a C++ launcher embedding source and re-parsing at runtime, then
compiles with MSVC `cl.exe` or a GNU/LLVM cross toolchain (`tools/kirn.cpp:2342-2668, 2524-2667`),
reusing probed compilers (`tools/kirn.cpp:1862-1923`). **This is our extension point for C/C++
linking** (Phase 5): we add foreign `.o`/`.lib`/`.a` artifacts and link flags to this existing
command, rather than inventing a second build system.

### 2.6 Native scalar lowering

**Goal:** reuse the generated-C++ scalar path as a host for linked foreign code.

**Must Follow:** keep dispatch order native > VM > tree-walker; never force foreign calls through the
tree-walker.

**Important Note:** the generated C++ already pulls in the whole runtime, so foreign compiled code
can be linked beside it with no new runtime container.

`src/backend/native.cpp` lowers scalar-only pure KIRN functions to real C++ and registers them via
`kirn_native_register()` (`native.cpp:102-131,334-356,612-655`; dispatch order native > VM >
tree-walker after lowering in `runtime.cpp:2667-2672`). **The generated-C++ path already pulls in
the whole runtime** — foreign compiled code can be linked beside it.

### 2.7 Subprocess/toolchain detection

**Goal:** reuse the existing `std::system()` probe pattern for every foreign toolchain.

**Must Follow:** `std::system(cmd)==0` probe, then PATH/env lookup — identical to `git`/`curl`/`cl`
probing today.

**Important Note:** no new process-spawning framework; this is the ratified detection mechanism for
all six languages (§7.1).

`tools/kirn.cpp` already runs external programs with `std::system()` for git/curl/`cl`/compiler
probing and for C/C++ compilation (`tools/kirn.cpp:413,1893,1922,2575,2612,2661`). **Toolchain
detection for each foreign language reuses this exact pattern** (`std::system(cmd)==0` probe, then
PATH/env lookup).

### 2.8 Libraries build (MSVC path)

**Goal:** hook foreign-object linking into the existing static-lib link step.

**Must Follow:** reuse the `kirn_lex/ast/parser/sema/vm/interp/backend` `.lib`s pipeline
(`CMakeLists.txt:22-57`, `tools/kirn.cpp:2585-2667`).

**Important Note:** foreign objects are appended to this link line; no new CMake target replaces it.

CMake builds static libs `kirn_lex/ast/parser/sema/vm/interp/backend` (`CMakeLists.txt:22-57`);
the MSVC build links these `.lib`s (`tools/kirn.cpp:2585-2667`). Foreign-object linking hooks here.

---

## 3. New functionality (must implement)

**Goal:** implement only the missing pieces N1–N10 — dynamic loading, typed dynamic binding, the
`use` surface, marshalling, per-language backends, discovery, cache, subprocess fallback, policy.

**Must Follow:** every item lands in an existing subsystem (`src/ffi/*`, `src/interp`,
`src/sema`, `src/parser`, `tools/kirn.cpp`) — nothing is a parallel runtime.

**Important Note:** N1/N2 are the foundation; all other items (N3–N10) are delegates of the same
`FfiModuleRegistry` + `FfiFuncShim` machinery, so ordering N1 → N2 first is non-negotiable.

| #   | Capability                                                                            | Where it lands                                      |
| --- | ------------------------------------------------------------------------------------- | --------------------------------------------------- |
| N1  | Real dynamic library loading (`dlopen`/`LoadLibrary` + `dlsym`/`GetProcAddress`)      | new `src/ffi/*`                                     |
| N2  | Typed `extern fn` → dynamic-symbol binding (replace hardcoded printf/strlen)         | `src/interp/runtime.cpp`, `src/sema/checker.cpp`    |
| N3  | `use import` / `use from` / `use <lang>{}` parsing + AST                              | `src/parser/parser.cpp`, `src/lex`, `src/ast/ast.h` |
| N4  | Value↔C marshalling (int/float/bool/char/string/array/bytes/error/pointer)            | `src/ffi/marshal.*`                                 |
| N5  | Per-language compilers (C/C++/Rust/Go → shared lib) and drivers (Python/Ruby embed)   | `tools/kirn.cpp` + `src/ffi/lang/*`                 |
| N6  | Symbol discovery/validation from generated headers or optional `@extern` declarations | `src/ffi/discover.*`                                |
| N7  | Toolchain discovery + clear diagnostics + missing-toolchain errors                    | `tools/kirn.cpp`                                    |
| N8  | Compilation cache + dependency tracking + parallel builds                             | `tools/kirn.cpp`                                    |
| N9  | Subprocess fallback (`os.exec`/pipe) for script-language isolation                    | `src/interp/runtime.cpp`, `stdlib/lib/os.kn`        |
| N10 | Security/sandbox policy + docs                                                        | `src/ffi/policy.*` + docs                           |

> Reference: Odin — `core/dynlib` (`lib_unix.odin`/`lib_windows.odin`) for the smallest portable
> dlopen/LoadLibrary wrapper; Zig — `zig/lib/std/dynamic_library.zig` for the same pattern in a
> stdlib; V — `v/vlib/dl` (`dl.v`) for the compact cross-platform loader; Go — `go/src/runtime/cgo`
> for the `//export` symbol boundary (all verified on disk).

```kn
use import c:math_util;                    # N3: parse + register the foreign module root
@extern("mymath", "kirn_pow2")             # N6/N2: pin library + exact C symbol
extern fn pow2(x: Int64) -> Int64;

print(pow2(6));                            # 64 — dlopen/dlsym through FfiFuncShim (N1)
```

---

## 4. Target architecture

**Goal:** one pipeline that turns `use import`/`use <lang> { … }` into either a `dlopen`ed C-ABI lib
or a co-resident foreign VM, all behind `FfiFuncShim`.

**Must Follow:** the two proven models (A dynamic library, B embedded interpreter) plus subprocess
fallback — no third bridging layer.

**Important Note:** both paths converge on the same `marshal.in/out` layer, so semantics must be
identical across languages; this property is ratified.

```
                 ┌──────────────────────────── KIRN program ───────────────────────────┐
                 │  use import c:main;  use cpp:main;  use rust:main;  go:main;        │
                 │  use ruby:main;      use python:main;   +  use <lang> { … }          │
                 └──────────────┬───────────────────────────────┬──────────────────────┘
                                │  (parser/checker, N3)         │  embedded text
                 ┌──────────────▼────────────┐      ┌───────────▼───────────┐
                 │  FfiModuleRegistry         │      │  embedded-source cache │
                 │  (resolver + cache)        │      │  (raw source per lang) │
                 └───┬───────────┬────────────┘      └───────────┬───────────┘
          compiled  │           │ scripting              compile at build (N5)
        ┌───────────▼──┐   ┌────▼────────┐
        │ compiled FFI │   │ embedded VM │   Python (Py_*), Ruby (rb_*), Go(pipes)
        │  dlopen/dlsym│   │  co-resident│
        └──────┬───────┘   └──────┬──────┘
   ┌───────────┼──────────────────┼───────────────────────────────┐
   │  C  ── cc -shared main.c            → libmain.{so,dll,dylib}  │
   │  C++└─ c++ -shared main.cpp         → libmain.so  (extern"C") │  A. compiled C ABI
   │  Rust└─ rustc -Ccdylib main.rs      → libmain.so  #[no_mangle]│
   │  Go  └─ go build -buildmode=c-shared→ libmain.{so,dll} + .h   │
   └───────────┴───────────────────────────────────────────────────┘
             each loaded via                           ┌──────────────┐
             dlopen/GetProcAddress (N1,N2) ──────────►│ FfiFuncShim  │ BiFn-wrapper
                                                      │ (BiFn,cache) │ → Value
                                                      └──────────────┘
                                                                │
                                          marhal.in/out (N4): KIRN Value ↔ C types
```

### 4.1 Two runtime call paths

**Goal:** make interpreter dispatch choose the compiled `dlopen`-shim path or the embed path with
identical arg/return semantics.

**Must Follow:** compiled path calls a typed C-function pointer inside an `FfiFuncShim` invoked like a
`VK::Builtin`; script path marshals through the embed API and maps errors to `raise`.

**Important Note:** both share one marshalling layer — do not fork semantics between `kirn run`
(interpreter) and `kirn build --native`.

**Performance:** scalar call overhead and marshal cost on the compiled path must beat cgo's
`//export` boundary, measured with `kirn bench` (subprocess mode is exempt — it is the isolation
fallback, not the fast path).

1. **Compiled path (C/C++/Rust/Go):** at _run_ time KIRN `dlopen`s the prebuilt shared lib and
   `dlsym`s each required symbol, calling through a typed C-function pointer contained in an
   `FfiFuncShim` that the interpreter invokes like a `VK::Builtin`.
2. **Script path (Python/Ruby):** KIRN statically (or lazily `dlopen`) links the foreign
   interpreter; each import calls the embed API to marshal args, invoke, and fetch the result /
   raise on error.

Both paths converge on the same in/out marshalling layer, so argument/return semantics are
identical across languages.

---

## 5. Data marshalling (shared by every language)

**Goal:** define exactly one KIRN-Value ↔ C-type mapping used by the compiled, embed, and subprocess
paths alike.

**Must Follow:** copy at the boundary, own memory explicitly, and reuse the Phase 1
`src/ffi/marshal.*` layer for all six languages.

**Important Note:** the scalar table is a contract — adding a language means adding a row, never a
new conversion path.

**Performance:** per-call marshalling (arg pack + return unwrap) must beat cgo's `C.xxx` conversion
cost and stay allocation-free for scalars and short strings, measured with `kirn bench`; string
copies are bounded by the payload, with zero copies for `c_ptr()` borrows.

### 5.1 Supported scalar mapping

**Goal:** ratify the byte-level mapping for scalars, strings, and bytes across all six languages.

**Must Follow:** `Int`/`Int64` → `int64_t`, `Float`/`Float64` → `double`, `Bool` → C `bool`/`GoBool`,
`Char` (rune) → `uint32`/`GoInt32`, `Str` → `const char*`+len / `GoString` /
`PyUnicode`/`rb_str_new`.

**Important Note:** Kirn primitives are CapCase (`Int`, `Float`, `Bool`, `Char`, `Str`) with width
micros `i64`/`u64`; the `int`/`i32`/`f64` spellings in the table are the pre-rename aliases and stay
valid.

| KIRN         | C/C++/Rust                | Go (cgo)                | Python (Py*)          | Ruby (VALUE)               |
| ------------ | ------------------------- | ----------------------- | --------------------- | -------------------------- |
| `int`/`i64`  | `int64_t`/`long long`     | `GoInt` (int64)         | `PyLong_FromLongLong` | `LL2NUM`/`rb_ll2inum`      |
| `i32`/`u64`… | `int32_t`/`uint64_t`…     | specific C int          | `PyLong_*`            | per-width macros           |
| `f64`        | `double`                  | `GoFloat64`             | `PyFloat_FromDouble`  | `DBL2NUM`                  |
| `f32`        | `float`                   | `GoFloat32`             | `PyFloat`             | `rb_float_new`             |
| `bool`       | `bool`/`int`              | `GoBool`(int32)         | `PyBool_*`            | `Qtrue`/`Qfalse`           |
| `char`       | `char32_t`→`uint32`       | `GoInt32` (rune)        | `PyLong`              | `INT2FIX`                  |
| `string`     | `const char*` (+len)      | `GoString{p,n}`         | `PyUnicode_*`/bytes   | `rb_str_new2`/`rb_str_new` |
| `bytes`      | `void*`+len → `VK::Bytes` | `GoSlice{data,len,cap}` | `PyBytes_*`           | `rb_str_new`               |

> Reference: Nim — `{.importc.}` C-import pragma (used throughout `Nim/lib/pure`; documented in
> `Nim/doc/manual.md`) for type-directed C binding; CPython — `ctypes` in `cpython/Lib/ctypes` and
> `cpython/Modules/_ctypes` for a table-driven foreign-type mapper; Go — `go/src/runtime/cgo` for the
> `GoString`/`GoInt` boundary types (all verified on disk).

### 5.2 Composite mapping & ownership rules

**Goal:** make arrays, handles, strings/bytes, and errors cross the boundary with zero leaks and zero
double-frees.

**Must Follow:** caller-owned KIRN→foreign copies freed after the call; foreign-owned returns copied
into `std::string`/`Value` immediately then freed by the registered owner; out-params copied back by
value.

**Important Note:** `VK::Ptr` handles are the only state allowed to outlive a call, and each carries
opaque handle + owned-flag + finalizer.

- **Arrays/lists:** passed as `(ptr, len)` C arrays for compiled path; `GoSlice` for Go; Python
  `PyList`/`PyTuple`; Ruby `rb_ary_new*`. Out-params are copied back by value (never hold foreign
  pointers past the call for scalar copies).
- **Pointers/handles:** foreign pointer is wrapped as `VK::Ptr` carrying an **opaque handle +
  owned-flag + optional finalizer**; KIRN must `.free()`/drop via a registered dtor, or the foreign
  side owns it (caller-declared).
- **Memory ownership (default):** _caller-owned for KIRN→foreign string/bytes_ — KIRN copies into
  C-malloc'd memory for the duration of the call and frees it after (or passes `c_ptr()`/`.data()`).
  _Foreign-owned returns_ are copied into KIRN `std::string`/`Value` immediately, then the foreign
  owner frees its buffer via the registered `free` (from libc for C, `free_string` for Go,
  `PyMem_Free`/ref-decref for Python, GC for Ruby).
- **Errors:** compiled languages return via an out-`char** err` (or `int` code + message) captured
  into a KIRN `result`/`raise`; scripts propagate the foreign exception into a KIRN `raise`
  (`PyErr_Occurred` / `rb_protect` error flag).

### 5.3 String lifetime nuance (ground truth)

**Goal:** state and enforce the lifetime rule that prevents use-after-free at the FFI edge.

**Must Follow:** never hand a `const char*` into long-lived foreign state without a copy;
short-lived call-borrowing is allowed.

**Important Note:** Kirn strings stay `std::string` (owned by Kirn) — the foreign side is licensed to
read, not to keep, unless an explicit copy/handle is made.

KIRN strings are `std::string`; `s.c_str()` is NUL-terminated but the memory stays owned by KIRN.
The FFI layer must therefore **never** hand a `const char*` into long-lived foreign state without a
copy; short-lived call-borrowing is fine. This is stated here to avoid a whole class of
use-after-free bugs.

---

## 6. Syntax & AST changes

**Goal:** add the minimum grammar/AST to express foreign import, foreign embed, and symbol pinning.

**Must Follow:** reuse the existing `import_decl`/`top_item` production shape and add exactly two AST
structs (`ForeignImport`, `ForeignEmbed`).

**Important Note:** `use` becomes a keyword and the brace block is captured raw — this is ratified and
must not alter any non-`use` parse.

### 6.1 Grammar additions (`grammar/kirn.ebnf`)

**Goal:** extend the grammar with `foreign_import`, `foreign_module`, `lang_tag`, and `foreign_embed`.

**Must Follow:** keep `lang_tag` a closed set (`c | cpp | rust | go | ruby | python`) and foreign
imports semicolon-terminated like every existing import.

**Important Note:** grammar deltas are additive only — no existing production is rewritten.

Add to `import_decl` and `top_item`/`simple_stmt`:

```
foreign_import  = "use" ( "import" foreign_module [ "as" IDENTIFIER ]
                        | "from" foreign_module "import" import_item )
                , ";" ;
foreign_module  = lang_tag , ":" , IDENTIFIER ;        (* c:main, python:main *)
lang_tag        = "c" | "cpp" | "rust" | "go" | "ruby" | "python" ;
foreign_embed   = "use" lang_tag block ;               (* embedded source *)
```

### 6.2 AST (`src/ast/ast.h`)

**Goal:** carry the parsed foreign module/embed information through the existing AST with two nodes.

**Must Follow:** `ForeignImport` holds lang + stem + alias + `use from` items + wholeModule flag;
`ForeignEmbed` holds lang + exact raw text.

**Important Note:** the embedded block preserves foreign comments/syntax byte-for-byte — the lexer
never reinterprets it.

```cpp
enum class ForeignLang { C, Cpp, Rust, Go, Ruby, Python };
struct ForeignImport {          // folded into Stmt or a sibling node
    ForeignLang lang;
    std::string stem;           // "main"
    std::string alias;
    std::vector<ImportBinding> items;   // for `use from`
    bool wholeModule = false;
};
struct ForeignEmbed {           // use <lang> { raw }
    ForeignLang lang;
    std::string text;           // raw embedded source (exact chars, decoded from the brace block)
};
```

- Lexer: `use` becomes a keyword; the embedded block is captured as raw text (like `r"…"` raw
  strings but block-scoped), preserving the foreign language's own comments/syntax untouched.
- The checker registers the module root (reusing `importRoots_`) and, for `use from`, binds each
  item with a type derived from a generated/declared signature (`@extern` or discovered symbol).

### 6.3 Symbol binding model

**Goal:** bind every foreign symbol to a Kirn type from the most authoritative source available.

**Must Follow:** ordered precedence — (1) explicit typed `extern fn`, (2) compiler-generated
header/metadata, (3) `@extern "foo"` name pinning; fall back to `dynamic`/`any` only when 1–3 fail.

**Important Note:** explicit `extern fn` is always the most reliable declaration (ratified in §10);
discovery is convenience/validation, never a requirement.

> Reference: Nim — `{.importc.}`/`{.header.}` pragma-style symbol binding
> (`Nim/lib/pure/*.nim`, manual in `Nim/doc/manual.md`); Go — cgo `//export` symbol boundary
> (`go/src/runtime/cgo`); Rust — `#[no_mangle] extern "C"`; CPython — `ctypes` in
> `cpython/Lib/ctypes` for the generated-declaration fallback (all paths verified on disk).

Two ways to declare the foreign signature, in decreasing precedence:

1. Explicit KIRN `extern fn` (typed) inside the module — preferred, mirrors existing FFI.
2. Auto-generated header/definitions from the compiler (`cgo -godefs`-style for Go; the generated
   `.h`; C/C++ actual header; Rust `cbindgen`-generated header; Python `inspect` signature;
   Ruby `arity`) — best-effort, may fall back to `dynamic`/`any` type (KIRN already has dynamic
   import roots).
3. `@extern "foo"` attribute on a KIRN `extern fn` to pin the exact foreign symbol name.

---

## 7. Toolchain discovery & diagnostics

**Goal:** detect each foreign toolchain up front and fail with an actionable error, never a
mysterious link/crash later.

**Must Follow:** reuse the `std::system(cmd)==0` probe pattern (§2.7) and the `KIRN_*` env-override
convention.

**Important Note:** a missing toolchain is a **compile-time** diagnostic listing the language, module,
and install/env remedy — no silent runtime fallback.

### 7.1 Per-language toolchain probe (reuses `std::system` pattern)

**Goal:** give every language one probe command, one env override, and one parsed result
(availability, version, lib-naming rule).

**Must Follow:** probe commands are the defaults in the table; env overrides (`KIRN_CC`, `KIRN_CXX`,
`KIRN_RUSTC`, `KIRN_GO`, `KIRN_RUBY`, `KIRN_PYTHON`) always win.

**Important Note:** the error format `KIRN-FFI-E0002` (toolchain + module + remedy) is ratified and
shared by all six languages.

| Lang   | Probe command (default)                                                         | Env override                    |
| ------ | ------------------------------------------------------------------------------- | ------------------------------- |
| C      | `cc --version`                                                                  | `KIRN_CC`                       |
| C++    | `c++ --version` (MSVC: `where cl`)                                              | `KIRN_CXX`, existing `cl` probe |
| Rust   | `rustc --version` (needs a `cdylib` target)                                     | `KIRN_RUSTC`                    |
| Go     | `go version` (needs cgo host toolchain)                                         | `KIRN_GO`                       |
| Ruby   | `ruby -e 'print RUBY_VERSION'` + dev headers                                    | `KIRN_RUBY`                     |
| Python | `python3 --version` + dev headers (`python3-config --embed --cflags --ldflags`) | `KIRN_PYTHON`                   |

Each probe yields: availability flag, version, and (for compiled-from-source C/Rust/Go) the
platform shared-lib naming rules. Missing toolchain → **compile-time error** listing, e.g.
`KIRN-FFI-E0002: 'rustc' not found; needed to build the 'rust:main' module. Set KIRN_RUSTC or
install the Rust toolchain.`

### 7.2 Cross-platform shared-library output names

**Goal:** one deterministic artifact name per platform so the loader and cache keys are stable.

**Must Follow:** Linux `lib<stem>.so`, macOS `lib<stem>.dylib`, Windows `<stem>.dll` (MSVC `/LD` or
mingw `-shared`).

**Important Note:** the target triple participates in the Phase 11 cache key, so these names are the
contract between build and load.

| Platform | suffix                                                |
| -------- | ----------------------------------------------------- |
| Linux    | `lib<stem>.so`                                        |
| macOS    | `lib<stem>.dylib`                                     |
| Windows  | `<stem>.dll` (built by `msvc /LD` or `mingw -shared`) |

---

## 8. Security & sandboxing

**Goal:** make the trust boundary explicit and give untrusted scenarios the subprocess fallback.

**Must Follow:** no implicit/arbitrary `dlopen` — require an explicit `use import`/`@extern` and an
allow-listed origin; pass argv (never shell metacharacters) to subprocesses.

**Important Note:** imported/embedded native code is full process trust by default — this is ratified
and must be documented, with `--ffi-sandbox`/`@allow ffi` as the opt-in gate.

- **Trust boundary:** importing foreign code executes arbitrary native code — treat it as full
  process trust by default. Provide `--ffi-sandbox` (or `@allow ffi`) gates.
- **No arbitrary `dlopen` from untrusted source** without an explicit `@extern`/`use` directive and
  (optionally) allow-listed library origin (path under `kirn_libs/` or `$KIRN_LIBS`).
- **Subprocess fallback isolation:** for `python`/`ruby`/`go` in subprocess mode, run in a child
  process with: no inherited dangerous env if requested, rlimit/pipe-limits, and a timeout; never
  pass arbitrary `TMPDIR`-redirected data unsafely.
- **Memory safety:** marshal layer copies rather than holding raw pointers across calls by default;
  foreign `handle` values are tracked and freed by registered destructors (no double-free).
- **Version pinning:** record toolchain versions in the build cache key so a rebuilt/runtime-
  mismatched ABI is detected.

---

## 9. Multi-phase roadmap

**Goal:** sequence the work so each phase is independently shippable and Phases 1–4 form the
foundation every later language delegates to.

**Must Follow:** implement Phase 1 (C) first; never start a language phase before the shared
marshalling/registry phases it depends on are green.

**Important Note:** every later phase is a delegate of the same machinery — no phase introduces a
parallel FFI path or a third executable.

The phases are ordered so each is independently shippable and the foundation (Phase 1–4) is reused
by every language. "Foundational" phases are required by all later ones.

---

### Phase 1 — C FFI foundation (C-ABI dynamic loading)

**Goal:** import a plain C source file via `use import c:main;`, compile it to a shared lib at
build/run time, and call its functions from KIRN with correct argument/return marshalling for
scalars and strings.

**Must Follow:** build exactly one foundation — `FfiModule` + `FfiFuncShim` + `marshal.*` — that every
later language delegates to; keep `printf`/`strlen` working through the same dispatch.

**Important Note:** the artifact is `lib<stem>.{so,dll,dylib}` loading via `dlopen`/`LoadLibrary`
(§7.2); missing compiler/symbol/file must fail with a `KIRN-FFI-E…` code, never a raw crash.

> Reference: Odin — `core/dynlib` (`lib_unix.odin`/`lib_windows.odin`) for the minimal
> dlopen/LoadLibrary + dlsym/GetProcAddress wrapper this phase mirrors.

**Performance:** a Phase 1 `i64`→`i64` call (pack → resolved invoke → return) must beat cgo's
`//export` call on the same symbol, measured with `kirn bench`.

**Files/components affected:** `src/ffi/*` (new: `ffi.h`, `dylib.cpp`, `marshal.*`),
`src/interp/runtime.cpp`, `src/sema/checker.cpp`, `src/ast/ast.h`, `src/parser/parser.cpp`,
`src/lex/lexer.cpp`, `grammar/kirn.ebnf`, `tools/kirn.cpp`, `CMakeLists.txt`,
`examples/x-ffi/`.

**Required architectural changes:** add a dynamic-loading layer (`dlopen`/`LoadLibrary` +
`dlsym`/`GetProcAddress`) and route `extern fn` symbols for C modules through it; introduce the
`FfiModule` + `FfiFuncShim` runtime objects; add per-call scalar/string marshalling.

**Implementation steps:**

1. `src/ffi/dylib.cpp`: portable `dlopen/dlsym/dlclose` (POSIX) vs `LoadLibrary/GetProcAddress`
   (Windows) wrappers returning typed function pointers.
2. `src/ffi/marshal.*`: `Value→C` (int/float/bool/char/string/bytes) and `C→Value` conversions
   plus the string copy/free policy in §5.3.
3. Parser: add `use import c:main;`/`use from c:main import x;` → `ForeignImport` AST; checker
   registers foreign module roots; `extern fn` inside an imported C module binds to a shim.
4. Interpreter: `FfiModuleRegistry` that (a) compiles the `.c` (Phase 5 hookthin here — see below)
   or loads a prebuilt lib, (b) resolves symbols lazily on first call, (c) returns `VK::Builtin`
   wrappers for calls.
5. `tools/kirn.cpp`: add `--ffi` acceptance and `cc -shared` invocation producing
   `build/.../lib<stem>.{so,dll,dylib}`; cache the artifact (Phase 11) with a `.d` dependency list.

**Syntax/AST changes:** §6 additions; `ForeignImport`, `extern fn` with `@extern` name pinning.

**Compiler/runtime changes:** `extern fn` _runtime_ resolution now consults the FFI registry
before falling back to builtins (so `printf`/`strlen` still work).

**Toolchain/build requirements:** a C compiler (`cc`, or MSVC `cl /LD` on Windows).

**Error handling:** `KIRN-FFI-E…` codes for: toolchain missing, file not found, compile failure,
symbol missing, marshalling type mismatch, ABI/version mismatch.

**Security and sandboxing considerations:** treat imported `.c` as trusted native code; require
explicit `use import` (no implicit dlopen); document that embedded/imported C is not sandboxed.

**Cross-platform considerations:** shared-lib naming table (§7.2); MSVC vs GCC calling convention
(no `__stdcall`; KIRN uses the platform default C ABI).

**Code example:**

```c
// math_util.c
#include <stdint.h>
int64_t kirn_add(int64_t a, int64_t b) { return a + b; }
const char* kirn_greet(const char* who) { return who; }  /* caller copies */
```

```kirn
use import c:math_util;          # compiles math_util.c => libmath_util
use from c:math_util import kirn_add;
result = kirn_add(20, 22);       # 42
```

(Internally `kirn_add` becomes `extern fn kirn_add(a: i64, b: i64) -> i64;` bound to
`dlsym(handle,"kirn_add")`.)

**Testing requirements:** a `test_ffi_c.kn` + `*_test.kn` asserting correct int/float/string
round-trips; a negative test for a missing symbol and a missing compiler.

**Expected result:** `kirn run examples/x-ffi/c_basic.kn` compiles the C file, dlopens it, and
prints `42`; `kirn build --native` (Phase 5) links the same.

---

### Phase 2 — FFI value-marshalling hardening

**Goal:** full marshalling correctness for all supported scalars plus `bytes`, arrays of scalars,
booleans, and pointer/handle round-tripping with safe ownership.

**Must Follow:** one `FfiHandle` registry (monotonic id → `void*` + dtor + owner) is the only way to
carry foreign memory across calls; scalars/strings stay copy-in/copy-out.

**Important Note:** finalizers run exactly once and raw `*Char` never outlives the call — this is the
leak/double-free guarantee every later language inherits.

**Performance:** scalar/bytes/array round-trip overhead must beat cgo's slice→`GoSlice` conversion on
the same matrix, measured with `kirn bench`.

**Files/components affected:** `src/ffi/marshal.*`, `src/ffi/handle.*` (new), `src/interp/value.h`,
`tests/ffi/`.

**Required architectural changes:** add an `FfiHandle` registry (monotonic id → `void*` + dtor +
owner) so foreign pointers can be passed between KIRN and foreign calls without leaking or
double-freeing; define out-parameter and array conventions.

**Implementation steps:** scalar test matrix; bytes via `VK::Bytes` ⇄ `void*+len`; array pass-by-
`(ptr,len)` with copy-back; handle alloc/release/finalize; error-on-overflow and type-punning
guards.

**Syntax/AST changes:** none (reuses Phase 1).

**Compiler/runtime changes:** handle finalization on KIRN GC/scope exit (add to `Value` a pending
`FfiHandle` finalizer list).

**Toolchain/build requirements:** none beyond Phase 1.

**Error handling:** clear `KIRN-FFI-E03xx` for overflow, alignment, mismatched length.

**Security:** handles are the only allowed way to carry foreign memory across calls; raw `*char`
never outlives the call; finalizers run exactly once.

**Cross-platform:** validate `sizeof`/`alignof` of each scalar per target in tests.

**Code example (C):** pass an array, get copied-back modified values; allocate a struct via a C
`make()`, hand back an opaque handle, call `free(handle)`.

The same flow in Kirn (`.kn`):

```kn
use import c:buffers;
extern fn make(n: Int64) -> *Char;        # opaque handle, owned by Kirn
extern fn fill(h: *Char, v: Int64);       # in-place mutate, no copy
extern fn drop(h: *Char);                 # registered dtor

h = make(4);
fill(h, 7);
drop(h);                                   # finalizer runs exactly once
```

**Testing:** exhaustive scalar/array/bytes/handle round-trip tests on all three OSes (CI matrix).

**Expected result:** FFI calls are type-safe and leak-free across a broad marshalling matrix.

---

### Phase 3 — Real `extern fn` dynamic binding (replace hardcoded printf/strlen)

**Goal:** make `extern fn f(...) -> T;` genuinely bind to an arbitrary dynamically-loaded C
symbol (not only the two hardcoded builtins), while keeping the existing `printf`/`strlen`
behavior as a fallback.

**Must Follow:** dispatch order is fixed — (1) FFI registry symbol, (2) existing builtin shim,
(3) error; `@extern` pins library + symbol name with no new syntax beyond the attribute.

**Important Note:** symbol-not-found should fail at bind time, not on first call; explicit
`use import` remains the only source of allowed symbols.

**Files/components affected:** `src/interp/runtime.cpp` (`installBuiltins`,
`collectProgram`/call dispatch, `bind`), `src/sema/checker.cpp`.

**Required architectural changes:** introduce a name-resolution order for `extern fn` calls —
(1) FFI registry symbol, (2) existing builtin shim, (3) error. Add an `externSpec` (library name +
symbol name, optionally via `@extern`) so users can `extern fn` any exported C function.

**Implementation steps:** extend `extern fn` parsing with `@extern("<lib>", "<sym>")`; add the
call-dispatch branch; keep `printf`/`strlen` working; add a test that binds a user C function.

**Syntax/AST changes:** optional `@extern` attribute on `extern fn`.

**Compiler/runtime changes:** call dispatch consults FFI before builtins.

**Toolchain/build:** a C compiler to build test libs.

**Error handling:** symbol-not-found at bind time (not first call, if possible).

**Security:** only symbols from modules explicitly imported via `use import`.

**Cross-platform:** pointer-width / calling-convention tested on all three OSes.

**Code example:**

```kirn
use import c:libc_like;
@extern("mymath", "kirn_pow2") extern fn pow2(x: i64) -> i64;
print(pow2(6));   # 64, dispatched through dlopen/dlsym
```

**Testing:** bind a variety of libc + custom functions; ensure `printf` example
(`examples/24_ffi_unsafe.kn`) still passes unchanged.

**Expected result:** any C-exported symbol is callable via `extern fn` with no hardcoding.

---

### Phase 4 — `use <lang> { … }` embedded source for C

**Goal:** support embedding C (and later other) source directly in a `.kn` file:
`use c { … }`, verified-compiled, symbols exposed.

**Must Follow:** the lexer captures a balanced brace block as raw bytes without interpreting nested
C braces/comments as Kirn; identical embedded sources compile once via the embed cache.

**Important Note:** brace balancing is language-agnostic, so the same raw-block mechanism must serve
all six languages unchanged.

> Reference: Nim — `{.emit.}` inline-C pragma (`Nim/doc/manual.md`) for embedding foreign source
> inside the host file; Go — cgo preamble in `go/src/runtime/cgo` for the cgo-comment variant.

**Files/components affected:** `src/lex/lexer.cpp` (raw-block capture), `src/parser/parser.cpp`,
`src/ast/ast.h` (`ForeignEmbed`), `tools/kirn.cpp` (compile from temp file), `src/ffi/`.

**Required architectural changes:** lexer must capture a balanced brace block as raw text without
interpreting nested C braces/comments as KIRN; compiler writes the text to a temp `<stem>.c`, runs
`cc -shared`, loads it, and registers exported symbols.

**Implementation steps:** raw-block tokenizer; balanced-brace scanner; temp-file + compile + dlopen;
symbol export detection (best-effort: all non-`static` global functions, or explicit `@export`).

**Syntax/AST changes:** `ForeignEmbed` node; `use c { … }`.

**Compiler/runtime changes:** embed cache so identical embedded sources compile once.
**Toolchain/build:** `cc`; temp build dir under `<project>/.kirn-ffi-build/`.

**Error handling:** C compile errors surfaced with the foreign diagnostics captured as
`KIRN-FFI-E02xx` including the foreign compiler stderr.

**Security:** embedded C runs as trusted native code; document this; optional `--ffi-no-exec`.

**Cross-platform:** brace balancing is language-agnostic so the same mechanism serves all six.

**Code example:**

```kirn
use c {
    int64_t kirn_triple(int64_t v) { return v * 3; }
}
print(kirn_triple(14));   # 42  (kirn_triple exposed as a module member)
```

**Testing:** embedded C with nested braces/comments; a syntax-error case reporting foreign stderr.

**Expected result:** KIRN files can embed and immediately call C code.

---

### Phase 5 — Link foreign objects into `kirn build --native`

**Goal:** when building a native executable, statically/dynamically link compiled foreign objects
(not just run them via dlopen).

**Must Follow:** extend `BuildOpts` with `foreignLibs`/`foreignLinkFlags`/`foreignIncludeDirs` and
append them to the existing GNU and MSVC link commands — no second build system.

**Important Note:** the dlopen path is erased for native builds (foreign code is linked in), while
`kirn run` keeps dlopen; both must behave identically by construction.

**Performance:** linked calls must show zero dlopen/dlsym overhead at run time (direct PLT call),
measured with `kirn bench` against the `kirn run` dlopen path.

**Files/components affected:** `tools/kirn.cpp` (`BuildOpts`, `buildProgram`, GNU/MSVC link steps
`2524-2667`), `CMakeLists.txt`.

**Required architectural changes:** extend `BuildOpts` with `foreignLibs`, `foreignLinkFlags`,
`foreignIncludeDirs`; append `.o`/`.lib`/`.a` and link flags (`-l`/`/link`) to the existing GNU and
MSVC compile/link commands; for the app-shim path, embed and compile foreign C/C++ sources
alongside the generated launcher.

**Implementation steps:** gather foreign artifacts during `gatherEmbedded`/`buildProgram`; pass
them through to both compiler paths; verify `--native` scalar functions still lower.

**Syntax/AST changes:** none (build-level).

**Compiler/runtime changes:** native binary now carries foreign code; dlopen path used only by
`kirn run` (interpreter).

**Toolchain/build:** C/C++ compilers; linkers available in CI for all three OSes.

**Error handling:** unresolved-symbol errors surfaced at link with foreign files listed.

**Security:** linked foreign code is in-process trust.

**Cross-platform:** distinct GNU vs MSVC flag plumbing.

**Code example:** `kirn build app.kn --native --link-lib mymath` where `mymath` was produced from
`use import c:math`.

The Kirn source that is linked (`.kn`):

```kn
use import c:math;                 # produces libmath
use from c:math import kirn_add;
print(kirn_add(20, 22));           # 42 in the native binary
```

**Testing:** build+run a native app that calls C; ensure `examples/native_main.kn` unaffected.

**Expected result:** native KIRN executables can statically link C/C++/Rust/Go artifacts.

---

### Phase 6 — C++ support

**Goal:** `use import cpp:main;` and `use cpp { … }`.

**Must Follow:** C++ exports must cross via `extern "C"` (dlsym cannot find mangled names);
everything else reuses the C FFI, marshalling, and Phase 5 linking unchanged.

**Important Note:** class objects never cross by value — only `extern "C"` free functions and opaque
`new/delete` handles; a missing `extern "C"` yields a symbol-not-found hint, not a mangling guess.

**Files/components affected:** `src/ffi/dylib.cpp` (C ABI only), `tools/kirn.cpp` (C++ compiler),
lexer/parser (add `cpp` tag), `examples/x-ffi/cpp_*`.

**Required architectural changes:** C++ must export a C ABI surface (`extern "C"` wrappers) because
`dlsym` cannot reliably find mangled names; the tool compiles `.cpp` with the C++ compiler and
loads the resulting lib.

**Implementation steps:** add `cpp` language tag; compile with `c++ -shared -fPIC` (or `cl /LD`);
require `extern "C"` on exported functions OR mangled-name resolution via a generated
mangling-aware stub; provide a helper macro for users.

**Syntax/AST changes:** new `cpp` tag (grammar + parser); everything else reuses C FFI.

**Compiler/runtime changes:** none beyond the compile step; reuses Phase 1–5 marshalling.

**Toolchain/build:** a C++ compiler (already required by KIRN itself).

**Error handling:** `extern "C"` missing → symbol-not-found with a helpful hint to add
`extern "C"` or `KIRN_EXPORT`.

**Security:** same as C (trusted native code).

**Cross-platform:** class objects cannot cross ABI by value; only `extern "C"` free functions and
opaque `new/delete` handles are callable.

**Code example:**

```cpp
// math.cpp
extern "C" __kirn_export int64_t kirn_factorial(int64_t n) {
    return n <= 1 ? 1 : n * kirn_factorial(n - 1);
}
```

```kirn
use import cpp:math;
print(kirn_factorial(5));   # 120
```

**Testing:** C++ with `extern "C"` free functions and an opaque class handle
(new/free round-trip).

**Expected result:** C++ libraries with C ABI surfaces are importable and callable.

---

### Phase 7 — Rust support

**Goal:** `use import rust:main;` and `use rust { … }`.

**Must Follow:** compile with `rustc --crate-type=cdylib` (or `cargo build`) and require
`#[no_mangle] pub extern "C" fn`; marshal strings via `CString`/`CStr`; opaque handles only for
structs.

**Important Note:** Rust panics must not unwind through `extern "C"` — wrap entry points in
`catch_unwind` and return an error handle (a `panic!` test is required).

**Files/components affected:** `tools/kirn.cpp` (rustc cdylib), `src/ffi/`, lexer/parser (`rust`
tag), `examples/x-ffi/rust_*`.

**Required architectural changes:** compile `.rs` with `rustc --crate-type=cdylib` (or `cargo
build`), producing a C-ABI shared lib; require `#[no_mangle] pub extern "C" fn` on exports; resolve
symbols via `dlsym` like C. Strings must be converted via `CStr`/`CString`; slicing/by-value structs
are not supported (opaque handles only).

**Implementation steps:** `rust` tag; `rustc -C cdylib -o lib<stem>.so <stem>.rs`; `--extern`
generated header via `cbindgen` optional; marshalling: `i64/f64/Bool` direct, `Str` via
`CString::into_raw`/`CStr`, callbacks via `extern "C" fn` pointer.

**Syntax/AST changes:** new `rust` tag only.

**Compiler/runtime changes:** reuse FFI registry/handles from Phases 1–2.

**Toolchain/build:** `rustc` with a matching `cdylib` target and the C ABI toolchain (KIRN already
needs a C++ toolchain).

**Error handling:** panic across FFI → abort (Rust panics can't unwind through `extern "C"`); wrap
entry points in `catch_unwind` and return an error handle.

**Security:** trusted native code; `unsafe` inside the Rust `use rust {}` block is Rust's own
responsibility, but we document it.

**Cross-platform:** cdylib naming `lib<stem>.so/.dylib/<stem>.dll`.

**Code example:**

```rs
#[unsafe(no_mangle)]
pub extern "C" fn kirn_rust_add(a: i64, b: i64) -> i64 { a + b }
```

```kirn
use import rust:main;
use from rust:main import kirn_rust_add;
print(kirn_rust_add(40, 2));   # 42
```

**Testing:** Rust add/string/handle; a `panic!` case that surfaces as a KIRN error via
`catch_unwind`.

**Expected result:** Rust cdylib functions are importable.

---

### Phase 8 — Go support (compiled C-ABI + subprocess modes)

**Goal:** `use import go:main;` and `use go { … }`. Go cannot be embedded the way C/Rust can, so we
offer two modes.

**Must Follow:** compiled mode uses `go build -buildmode=c-shared` with only `//export` functions and
cgo-safe types (`C.int`, `GoString`, `GoSlice`); subprocess mode reuses Phase 9 and is the
cross-platform default.

**Important Note:** on Windows only one Go runtime may live per process image (go.dev issue #50304),
so c-shared mode is Linux/macOS-only and multiple embedded Go guests are forbidden — ratified
limitation, not a bug to work around.

> Reference: Go — `go/src/runtime/cgo` for the `//export` + cgo-safe type boundary that compiled mode
> mirrors exactly.

**Files/components affected:** `tools/kirn.cpp` (`go build`), `src/ffi/`, lexer/parser (`go` tag),
`stdlib/lib/os.kn` (subprocess helpers), `examples/x-ffi/go_*`.

**Required architectural changes & approach:**

1. **Compiled mode:** `go build -buildmode=c-shared -o lib<stem>.{so,dll} <stem>.go` produces a
   C-ABI shared lib + generated `.h`. Only `//export`-annotated functions are callable, and they
   must use cgo-safe types (`C.int`, `GoString`, `GoSlice`). KIRN `dlopens` the `.so` like C.
   **Limitation:** on Windows there can be only one Go runtime per process image (go.dev issue
   #50304), and a Go `.so` dragged into a KIRN process that is not itself Go is generally not
   supported — so compiled mode is **Linux/macOS-first**, and even there mixing multiple Go
   runtimes is fragile.
2. **Subprocess mode (portable, recommended cross-platform):** `go build` a standalone
   executable that speaks a simple line/JSON protocol over stdio; KIRN talks to it via the new
   `os.exec`/pipe API (Phase 9). **Recommended default** for cross-platform Go interop.

**Implementation steps:** add `go` tag; `go build -buildmode=c-shared` (compiled mode) or
`go build -o <stem>.exe` (subprocess mode); marshal `GoString{ptr,len}`/`GoSlice{data,len,cap}`;
subprocess protocol framing (length-prefixed JSON lines with an explicit schema).

**Syntax/AST changes:** new `go` tag; a mode hint (`@subprocess`) optional.

**Compiler/runtime changes:** subprocess runner; Go handle lifetime (strings must be `C.free`d
after copy on the Go side).

**Toolchain/build:** `go` + host C toolchain (cgo) for compiled mode; `go` alone for subprocess.

**Error handling:** subprocess non-zero exit → captured stderr as `KIRN-FFI-E04xx`; protocol
misparse → structural error; compiled-mode runtime collision → documented error.

**Security:** subprocess mode is naturally sandboxed (separate process, rlimit/timeout);
compiled mode is in-process trust.

**Cross-platform:** subprocess mode recommended everywhere; c-shared mode documented as
Linux/macOS-only.

**Code example (subprocess mode):**

```go
package main
import ("fmt";"os")
func main() {
    // reads int,float,string from stdin; writes JSON result to stdout
    var a,b int64
    fmt.Scan(&a,&b)
    fmt.Printf(`{"result":%d}`, a+b)
    os.Exit(0)
}
```

```kirn
use import go:adder;   # subprocess mode by default
print(adder(20, 22));  # 42
```

**Testing:** subprocess add + string echo; compiled-mode on Linux CI; exit-code/error propagation.

**Expected result:** Go logic callable cross-platform via subprocess; on Linux/macOS optionally via
c-shared lib.

---

### Phase 9 — `os.exec` / subprocess + pipe API (foundation for script modes)

**Goal:** expose a first-class, safe subprocess API in KIRN so the Python/Ruby/Go-as-script paths
and the fallback story share a common mechanism.

**Must Follow:** `os.execv(argv, stdin, timeout)` and `os.start_process` use argv (never a shell
string) and abstract Windows `CreateProcess` vs POSIX `fork+exec`/popen behind one surface.

**Important Note:** nonzero exit, timeout, and spawn failure all return a structured `result`, not a
throw; this API is the single subprocess mechanism reused by Phases 8/10/13.

**Files/components affected:** `src/interp/runtime.cpp` (new `os.exec`, `os.popen2` builtins),
`stdlib/lib/os.kn`, `src/sema/checker.cpp` (signatures), `tools/kirn.cpp`.

**Required architectural changes:** add `os.execv(argv, stdin, timeout) -> {stdout, stderr, code}`
and a bidirectional pipe (`os.start_process` → handle with `.write/.read/.close/.wait`), using
portable `CreateProcess` (Windows) / `fork+exec`+`popen2` (POSIX). Wire into the `os` module the
same way `os.exit` is wired today (`runtime.cpp:1481-1498`).

**Implementation steps:** platform subprocess layer; stdio pipe plumbing; timeout + kill;
negative-path type-checking; cache handles.

**Syntax/AST changes:** none (new builtin members).

**Compiler/runtime changes:** new `VK::Process` value tag + `os.exec`/`os.start_process`.

**Toolchain/build:** none.

**Error handling:** spawn failure, timeout kill, non-zero exit all surfaced as structured `result`.

**Security:** subprocesses get limited capabilities; never pass shell metacharacters (use
`execv`-style argv, not `system`).

**Cross-platform:** Windows vs POSIX process creation abstracted.

**Code example:**

```kirn
r = os.execv(["python3", "-c", "print(6*7)"], timeout=10);
print(r.stdout);   # "42\n"
```

**Testing:** round-trip across all three OSes; timeout enforcement; argv safety (no shell).

**Expected result:** KIRN can launch and pipe to any external program generically.

---

### Phase 10 — Python support (embed + subprocess)

**Goal:** `use import python:main;`, `use from python:main import …;`, `use python { … }`, calling
Python functions/classes and accessing constants/variables.

**Must Follow:** embed mode is primary (`Py_InitializeFromConfig`, `PyRun_String`,
`PyObject_CallObject`, `PyErr_Occurred`→`raise`) with a GIL-safe mutex; subprocess mode (Phase 9,
JSON protocol) is the no-ABI fallback.

**Important Note:** Python ABI is pinned at link time (`-lpythonX.Y`) — a version mismatch must be a
clear error, and `PyEval_SaveThread`/`RestoreThread` bracketing is mandatory around non-Python code.

> Reference: CPython — the embed entry points `Py_Initialize*`/`PyRun_String`/`PyObject_Call*` are
> declared in `cpython/Include/Python.h` (verified), with `cpython/Lib/ctypes` as the
> table-driven type-mapper precedent for the marshal table.

**Files/components affected:** `src/ffi/lang/python.cpp` (new), `tools/kirn.cpp`
(`python3-config --embed` detection), `stdlib/lib/os.kn`, lexer/parser (`python` tag),
`examples/x-ffi/python_*`.

**Required architectural changes & approach:**

- **Embed mode (primary):** link `libpython` into the KIRN binary (`Py_InitializeFromConfig`,
  `PyRun_String` to define embedded source, `PyObject_CallObject` for calls, `PyObject_GetAttrString`
  for member access, `PyErr_Occurred`→KIRN `raise`). Must `PyEval_SaveThread`/`RestoreThread` around
  non-Python code and serialize access via a GIL-held mutex. ABI pinning to a specific Python is the
  main cost — emit it as a linker dependency (`-lpythonX.Y` / `libpythonX.Y.a`).
- **Subprocess mode (fallback):** reuse Phase 9 to run `python3 script.py` with a JSON protocol;
  works with any Python, no ABI pinning, slower, naturally isolated. **Recommendation:** embed mode
  for performance, subprocess mode for no-dependency CI/toolchains without headers.

**Implementation steps:** embed driver + marshalling (`PyList`, `PyUnicode`, `PyLong`, `PyFloat`,
`PyBool`); attribute/`__getattr__` binding for classes and module vars; embedded-source
registration; a `python3-config --embed --cflags --ldflags` probe producing include/lib flags on
POSIX and a documented `pythonXY.lib` on Windows.

**Syntax/AST changes:** new `python` tag; `use python { … }` forwards text to `PyRun_String`.

**Compiler/runtime changes:** `VK::Foreign` module backed by a `PyObject*`; finalization in
`Py_FinalizeEx`.

**Toolchain/build:** Python dev headers/library (embed mode) or a `python3` executable (subprocess).

**Error handling:** Python exceptions → KIRN `raise` with traceback text; `ImportError`/`NameError`
mapped to `KIRN-FFI-E05xx`; GIL/re-entrancy errors.

**Security:** embed mode is in-process; subprocess mode is isolated; both documented.

**Cross-platform:** identity/ordering of Python on PATH vs dev-lib version; Windows layout.

**Code example (from the proposal):**

```kirn
use python {
    fn print_hello(name):
        print(f"hello {name}")
}
print_hello("kirn");
```

**Testing:** embed-mode call + class instantiation + exception; subprocess-mode identity; GIL
safety under `spawn`.

**Expected result:** Python modules and embedded Python are importable and callable.

---

### Phase 11 — Compilation cache & dependency tracking

**Goal:** only recompile foreign code when its source (or a dependency) changes; enable
incremental, parallel builds.

**Must Follow:** one content-addressed key (lang, source hash, toolchain version, target triple,
flags) and `.d`-style dependency invalidation — no per-language bespoke cache.

**Important Note:** the cache is a pure build-time optimization; a built executable must run without
recompiling, and corrupted/stale entries are discarded and rebuilt, never repaired in place.

**Files/components affected:** `tools/kirn.cpp` (cache), `src/ffi/cache.*` (new), `CMakeLists.txt`.

**Required architectural changes:** a content-addressed cache keyed by (lang, source hash, toolchain
version, target triple, flags); store artifacts under `<project>/.kirn-ffi-cache/<lang>/<hash>/`;
record a `.d`-style dependency file (headers/imports) to invalidate.

**Implementation steps:** hash function; cache-lookup/write; dependency scrape (C/C++ `-MD`,
Rust `cargo` fingerprint, Go build cache, Python pyc mtimes); parallelize across independent modules
with a small thread pool.

**Syntax/AST changes:** none.

**Compiler/runtime changes:** the built executable must be able to _run_ without recompiling; cache
is a build-time optimization.

**Toolchain/build:** none extra.

**Error handling:** stale/mismatched cache detected and rebuilt; corruption → clean rebuild.

**Security:** never execute foreign build recipes from the cache blindly; re-verify hashes.

**Cross-platform:** other-OS caches are naturally invalidated via target-triple key.

**Testing:** touch a header → rebuild only dependents; two modules build in parallel; byte-identical
source reuses artifact.

**Expected result:** repeated builds are fast and correct.

---

### Phase 12 — Dependency management (`kirn add/install` foreign deps)

**Goal:** declare and fetch external source packages that contain foreign modules, consistent with
KIRN's existing package system (`kirn install`/`kirn add`, `kirn_libs/`, `kirn.toml`).

**Must Follow:** extend the existing manifest with one `[foreign]` table (languages, sources, include
paths, toolchain versions); resolve `use import <lang>:<stem>` through the same `libDirsFor`/
`gatherEmbedded` path.

**Important Note:** foreign packages imply native execution — require explicit `--allow-foreign` on
install and fail missing deps with `KIRN-FFI-E06xx` (package + install hint).

**Files/components affected:** `tools/kirn.cpp` (package packaging/unpacking ~`680-1170`), new
`foreign-deps` manifest in `kirn.toml`, `src/ffi/`.

**Required architectural changes:** extend package metadata with a `[foreign]` table listing
languages, source files, include paths, and required toolchain versions; the resolver matches
`use import <lang>:<stem>` against installed packages and records build args.

**Implementation steps:** manifest schema; resolver wiring into `libDirsFor`/`gatherEmbedded`;
toolchain-version constraint checking at install time.

**Syntax/AST changes:** none (config only).

**Compiler/runtime changes:** registry gains foreign-aware metadata.

**Toolchain/build:** as declared per package.

**Error handling:** missing dep → clear `KIRN-FFI-E06xx` naming the package + install hint.

**Security:** packages are third-party code — same trust model as today, but now with native
execution implied; require explicit `--allow-foreign` on install for packages containing foreign
code.

**Cross-platform:** package may carry per-OS sources (`.c` vs `.cpp`, win/linux/darwin dirs).

**Testing:** install a foreign package, import it, verify toolchain-version gate.

**Expected result:** foreign modules install and resolve like first-party ones.

---

### Phase 13 — Ruby support

**Goal:** `use import ruby:main;`, `use from ruby:main import …;`, `use ruby { … }`.

**Must Follow:** embed mode is primary (`ruby_init`/`ruby_init_loadpath`, `rb_eval_string`,
`rb_funcall`, `rb_protect`→`raise`) with `VALUE` marshalling via the `LL2NUM`/`DBL2NUM`/`rb_str_new*`
macros; subprocess mode reuses Phase 9.

**Important Note:** full CRuby embed is ratified for compatibility; mruby stays a documented opt-in
build flag. Ruby VM lifecycle (init → finalize) is owned by Kirn, exactly once.

> Reference: Ruby — the C-API headers under `ruby/include` (verified) for the `VALUE`, `rb_*`, and
> `rb_protect` surface this phase links against.

**Files/components affected:** `src/ffi/lang/ruby.cpp` (new), `tools/kirn.cpp` (Ruby headers),
`stdlib/lib/os.kn`, lexer/parser (`ruby` tag), `examples/x-ffi/ruby_*`.

**Required architectural changes & approach:**

- **Embed mode (primary):** link `libruby`; `ruby_init`/`ruby_init_loadpath`, `rb_require` or
  `rb_eval_string` for embedded source, `rb_funcall(recv, rb_intern(name), n, …)` for calls,
  `rb_protect` to catch exceptions, `rb_gc_start`/`rb_finalize` on shutdown. `VALUE` marshalling via
  `LL2NUM`/`NUM2LL`, `DBL2NUM`/`NUM2DBL`, `rb_str_new2`, `rb_ary_new*`, `rb_hash_new`.
- **Subprocess mode (fallback):** reuse Phase 9 with a JSON protocol like Python.
- **Alternative (optional): mruby** — a small embeddable Ruby that sidesteps full-CRuby init/GC
  complexity and is single-to-link, at the cost of a reduced stdlib. **Recommendation:** full CRuby
  embed for compatibility; mruby as a documented optional build flag.

**Implementation steps:** embed driver; `rb_protect` wrapper; embedded-source eval; top-level
`self` target for module-level defs; `VK::Foreign` Ruby module.

**Syntax/AST changes:** new `ruby` tag.

**Compiler/runtime changes:** Ruby VM lifecycle managed by KIRN; finalizer on program exit.

**Toolchain/build:** Ruby dev headers/library (or `ruby` executable for subprocess).

**Error handling:** `rb_protect` error flag → KIRN `raise` with `rb_errinfo` inspect; load errors.

**Security:** embed executes Ruby in-process (trusted); subprocess mode isolated.

**Cross-platform:** Ruby's C ABI is stable-ish but version-pinned; Windows build often needs the
`x64-msvcrt` builds.

**Code example:**

```kirn
use ruby {
    fn kirn_greet(who)
        "hello, " + who
    end
}
puts kirn_greet("kirn");
```

**Testing:** embed-mode method call + return + exception; subprocess fallback; GC/finalize stability.

**Expected result:** Ruby methods/modules are importable and callable.

---

### Phase 14 — Symbol discovery & validation

**Goal:** automatically derive KIRN signatures for imported symbols (`use from cpp:main import
something;`) and validate them at build time for both compiled and script languages.

**Must Follow:** one per-language inspector each producing a typed `std::map<string, FuncSig|VarSig>`;
`extern fn`/`@extern` declarations stay authoritative and discovery only cross-checks them.

**Important Note:** only whitelisted exports (`//export`, `@export`, `extern "C"`, `#[no_mangle]`,
non-`static` C functions) are visible; unknown signatures degrade to `dynamic`/`any` with a warning,
never to a runtime guess.

**Files/components affected:** `src/ffi/discover.*` (new), `tools/kirn.cpp`, `src/sema/checker.cpp`.

**Required architectural changes:** a per-language "inspector" that yields a typed signature table:

- C/C++: parse the real header (`clang -x c -emit-ast` / regex-light tokenizer, or a declared
  `@extern` signature) — pragmatic v1: `@extern`/`extern fn` declarations are authoritative, and
  discovery only _cross-checks_ the symbol exists.
- Rust: `cbindgen`-generated header or `rustc --emit=metadata` symbol list.
- Go: the generated `<stem>.h` from `-buildmode=c-shared`.
- Python: `inspect.signature` + runtime `dir()`.
- Ruby: `Method#arity`/`obj.methods`.

**Implementation steps:** for each module, produce a `std::map<string, FuncSig|VarSig>`; run a
build-time checker that reports mismatches between KIRN `extern fn`s and the foreign truth;
fall back to `dynamic`/`any` typing when unknown.

```kn
use from cpp:geometry import area_of;   # signature discovered from the real header
print(area_of(3, 4));                    # type-checked at build, not at runtime
```

**Syntax/AST changes:** none (supported by existing `extern fn`); a `use from` binds discovered
signatures.

**Compiler/runtime changes:** checker consumes the discovery table to type-check `use from`
calls.

**Toolchain/build:** per-language inspectors; headers for C/C++/Rust/Go optional when `extern fn`
explicit.

**Error handling:** symbol missing or signature mismatch → compile-time error; unknown → warn +
dynamic.

**Security:** symbol whitelisting: only `//export`, `@export`, `extern "C"`, `#[no_mangle]` or
non-`static` C functions are exposed (prevents accidentally exposing KIRN internals).

**Testing:** a Go module imported via `use from` with cgo-derived signature; a Python function with
a wrong arg count flagged.

**Expected result:** imported-symbol calls are type-checked before runtime.

---

### Phase 15 — Parallel builds, platform polish & error UX

**Goal:** harden build performance, diagnostics, and cross-platform robustness across all six
languages.

**Must Follow:** one build-graph scheduler (dependencies → topological order → small thread pool) and
one `KIRN-FFI` error taxonomy with remedies — no per-language diagnostic dialects.

**Important Note:** no diagnostic path may leak foreign pointers/addresses; `kirn ffi --doctor`
reuses the §7.1 probes and is the single support entry point.

**Performance:** the six-language integration benchmark must show Kirn FFI call overhead below each
reference binding (cgo, Py_* embed, rb_* embed) on both the dlopen and native-linked paths, measured
with `kirn bench`.

**Files/components affected:** `tools/kirn.cpp`, `src/ffi/*`, `docs/`, `examples/x-ffi/`.

**Required architectural changes:** a small build-graph scheduler (dependencies → topological
order → thread pool) so multiple foreign modules compile concurrently; a unified diagnostics
surface with per-language error snippets; a `kirn doctor` / `kirn ffi --doctor` subcommand that
reports which toolchains are present (reusing the §7.1 probes).

**Implementation steps:** thread pool for independent compile units; structured error formatting
(foreign stderr attached); `kirn ffi --doctor`; per-OS docs (paths, dev-packages, env vars).

**Syntax/AST changes:** none.

**Compiler/runtime changes:** none beyond performance.

**Toolchain/build:** all six.

**Error handling:** a single `KIRN-FFI` error taxonomy with remedies.

**Security:** review that no diagnostic path leaks foreign pointers/addresses.

**Cross-platform:** verify all six languages on Windows, Linux, macOS CI.

**Testing:** a six-language integration example built and run on all three OSes; parallel-build
timing.

**Expected result:** `kirn ffi --doctor` and a clean multi-language build/run everywhere.

---

### Phase 16 — Go embed & cross-callbacks, advanced FFI

**Goal:** (optional / stretch) richer cross-language features — Go genuine embedding (via a thin
C-ABI host module compiled by cgo), foreign→KIRN callbacks, struct/packed-layout by value, and a
higher-level binding generator.

**Must Follow:** callbacks route through one registry + trampoline that holds the interpreter lock;
Go embed stays a single cgo host `.so`; by-value layouts are opt-in `@packed`/`align` mapping to C
`repr(C)`.

**Important Note:** callbacks must detect re-entrancy/deadlock, and the Go single-runtime rule is a
hard error — none of this may weaken the Phase 1–15 semantics.

**Files/components affected:** `src/ffi/callback.*`, `src/ffi/discover.*`, `tools/kirn.cpp`.

**Required architectural changes:**

- **Cross-callbacks:** register a KIRN function pointer as a C callback and hand it to
  C/C++/Rust/Go; use a registry to route foreign callbacks back into KIRN (important for
  `qsort`-style APIs and event loops). Must hold the interpreter lock appropriately.
- **Go native embed (Linux/macOS):** build a small host `.so` with cgo that wraps a Go library and
  is `dlopen`ed by KIRN; document the single-runtime-per-process limitation and enforce one
  embedded Go guest.
- **By-value struct/buffer layout:** opt-in `@packed`/`align` annotations on KIRN `struct` so it
  maps to C/Rust `repr(C)` layouts for pass-by-value calls.

**Implementation steps:** callback registry + trampoline; Go host scaffolding; layout descriptor
for by-value FFI; a `kirn bindgen` prototype producing KIRN `extern fn` wrappers from foreign
headers.

**Syntax/AST changes:** `@packed`/`align` attributes; `@cb` callback registration.

**Compiler/runtime changes:** callback trampolines and layout marshalling.

**Toolchain/build:** cgo host toolchain for Go embed.

**Error handling:** callback re-entrancy/deadlock detection; Go-runtime-collision hard error.

**Security:** callbacks re-enter KIRN — enforce re-entrancy and foreign-only argument binding.

**Cross-platform:** Go embed Linux/macOS only; callbacks portable.

**Code example (callback):**

```kirn
use import c:sortlib;
use from c:sortlib import qsort;
fn cmp(a: *Char, b: *Char) -> i32 { return cmp_str(a, b); }
qsort(arr, len, 8, callback(cmp));   # foreign calls KIRN cmp
```

**Testing:** `qsort` with a KIRN comparer; by-value struct call; Go embed on Linux.

**Expected result:** bidirectional native interop including callbacks and by-value structs.

---

## 10. Cross-cutting notes & design decisions

**Goal:** record the ratified cross-cutting rules that bind every phase together.

**Must Follow:** `kirn run` dlopens/embeds at process start and `kirn build --native` links foreign
artifacts, both through `src/ffi/marshal.*`; one platform-default C ABI (no `__stdcall`) everywhere.

**Important Note:** `extern fn` + `@extern` is always the most reliable declaration and
auto-discovery is only a validation layer; these decisions are ratified and not phase-local.

- **Compiler vs interpreter:** `kirn run` uses `dlopen`/embed at process start; `kirn build
--native` links foreign artifacts (Phase 5). Both share `src/ffi/marshal.*` so behavior is
  identical.
- **Why not subprocess for everything?** Correctness of performance and ergonomics — embed/FFI is
  faster and richer. Subprocess is the _portable fallback_ and the **default for Go** due to Go's
  runtime-in-dll limitation (Windows) and general fragility of a second Go runtime.
- **`extern fn` first:** explicit `extern fn` with `@extern` is always the most reliable way to
  declare a foreign symbol; auto-discovery (Phase 14) is a convenience/validation layer, never a
  requirement.
- **Calling convention:** all compiled FFI uses the platform default C ABI (no `__stdcall`), which
  C, C++ (with `extern "C"`), Rust (`extern "C"`), and Go (`-buildmode=c-shared`) all target.
- **Version compatibility:** each compiled artifact is cached with its toolchain version; script
  languages pin the embedded runtime's version at link time; a mismatch produces a clear error.

---

## 11. Testing strategy

**Goal:** prove the FFI contract on real toolchains, not just unit mocks.

**Must Follow:** marshalling-matrix unit tests + per-language `examples/x-ffi/<lang>_*.kn` integration
tests on all three OSes, plus negative tests (missing toolchain/symbol, signature mismatch, timeout).

**Important Note:** existing `examples/24_ffi_unsafe.kn` and `native_main.kn` are golden regression
tests for Phase 3 and must pass unchanged.

- **Unit:** `src/ffi` marshalling matrix (`tests/ffi/*_test.kn`).
- **Integration:** `examples/x-ffi/<lang>_*.kn` each build+run+assert on all three OSes (CI).
- **Negative:** missing toolchain, missing symbol, compile error, signature mismatch, timeout.
- **Golden:** ensure existing `examples/24_ffi_unsafe.kn` and `native_main.kn` still pass unchanged
  (regression of Phase 3).
- **Scripts:** `scripts/runall.ps1` style runner extended to build foreign examples; a new
  `scripts/ffi_all.ps1` that runs `kirn ffi --doctor`, then each language example.

---

## 12. Glossary / dependency summary

**Goal:** one table mapping each language to its primary mechanism, requirements, and portable
fallback.

**Must Follow:** the mechanism column is fixed by the architecture decision (§4) — C/C++/Rust via
C-ABI dlopen, Go via c-shared-or-subprocess, Ruby/Python via embed-or-subprocess.

**Important Note:** every row's fallback ends at the same Phase 9 subprocess API; no language adds a
new dependency mechanism.

| Language | Primary mechanism                        | Requires               | Portable-subprocess fallback |
| -------- | ---------------------------------------- | ---------------------- | ---------------------------- |
| C        | dlopen SO/DLL                            | `cc`                   | n/a (native)                 |
| C++      | dlopen SO/DLL (`extern "C"`)             | `c++`/`cl`             | n/a                          |
| Rust     | dlopen cdylib                            | `rustc` + C toolchain  | —                            |
| Go       | c-shared (Linux/macOS) **or** subprocess | `go` (+cgo)            | subprocess (default)         |
| Ruby     | embed `libruby` **or** subprocess        | Ruby dev / `ruby`      | subprocess                   |
| Python   | embed `libpython` **or** subprocess      | Python dev / `python3` | subprocess                   |

---

_This plan reuses KIRN's existing `extern fn`, module-import resolver, native-build pipeline, and
`std::system` subprocess pattern. It does not invent new pipeline stages; it extends the ones that
exist. Implement Phase 1 first: it validates the whole foundation on the simplest ABI and every
later phase is a delegate of the same machinery._
