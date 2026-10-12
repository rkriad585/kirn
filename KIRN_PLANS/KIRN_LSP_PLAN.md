# KIRN Language Server â€” Implementation Plan

## Goal

An LSP implementation plan for Kirn that is industry-standard (LSP 3.17 over JSON-RPC 2.0), FASTER and lighter than equivalent Rust/Go servers, and AI-first: the LSP must expose machine-consumable structured data that both editors and code agents can consume/reuse directly from the `kirnc` checker. The server is built by reusing `kirnc` (checker/front-end) as a read-only analysis library (no third shipped binary) and follows Kirn style (minimal, no over-engineering), shipping only `kirn` and `kirnc` from the toolchain.

## Must Follow

- (a) Build ships ONLY `kirn` + `kirnc` — the LSP must reuse `kirnc` as a library, NOT add a third shipped binary unless it is a hidden dev tool.
- (b) No over-engineering — implement the LSP capabilities `kirnc`'s checker can already support, then grow.
- (c) AI-first: diagnostics/hover/completion must be exact, structured, incremental (I/O bound only on edits); expose semantic tokens + type info to agents.
- (d) Kirn-style spellings: lowercase width micros + capitalized primitives (Int, Float, Bool, Str, String, Char, BigInt, Rational, UInt/UFloat, UInt4..UInt256, Int4..Int256, Float4..Float256, u4..u256, i4..i256, f4..f256, UFloAT4..UFloat256 and capitalized width families). Free builtins/types Capitalized (Str(...), BigInt(...), __Sqrt), methods lowercase. Compiler front-end is `kirnc`.
- (e) Performance: hover/complete < X ms on large files, targeting faster than rust-analyzer/gopls cold-start typicals with minimal memory.
- (f) Proven patterns only: adopt best practices from rust/go/cpython/ruby/Nim/Odin/v/zig/llvm toolchains with surgical reuse (no speculative frameworks).

## Important Note

Skip-skip existing structure (capability matrix, phase plan, existing checker features that power it), keep it intact. This is an in-place enhancement: all current sections, tables, prose remain; triples and minimal additions are inserted without deletion.

- **Status:** DRAFT (roadmap, not yet executed)
- **Author:** RK Riad Khan (`rkriad585`)
- **Date:** 2026-09-04
- **Repo:** `github.com/rkriad585/kirn`
- **Language:** Kirn (Kirn heritage, `grammar/kirn.ebnf` still normative during transition)
- **Server name:** `kirn-lsp` (transitioning to `kirn-lsp` alongside naming)
- **Protocol:** LSP 3.17 over JSON-RPC 2.0 / stdio

---

## 0. Executive summary

`kirn-lsp` makes Kirn editing smart in every editor at once: diagnostics,
completion, hover, go-to-definition, references, document/workspace symbols,
rename, signature help, formatting, code actions, semantic highlighting, inlay
hints, and stdlib-aware completion. Because it speaks the **Language Server
Protocol**, it is written once and reused by VS Code, Neovim, Vim
(`vim-lsp`), Zed, Helix, JetBrains, Emacs/Eglot, and any other LSP client.

The single most important finding of this research is that **most of the hard
parts already exist in the Kirn compiler** and can be reused directly:

| Compiler component           | File                                                                       | What `kirn-lsp` reuses                              |
| ---------------------------- | -------------------------------------------------------------------------- | --------------------------------------------------- |
| Precise source locations     | `src/ast/ast.h` `Span{line,col,endLine,endCol}`                            | LSP `Range` (inclusive-end) with no translation     |
| Lexer                        | `src/lex/lexer.cpp` (`Lexer::lexAll`)                                      | tokenization; token extents; f-string sub-tokens    |
| Parser (with error recovery) | `src/parser/parser.cpp` (`syncToStatementEnd`)                             | syntax diagnostics on incomplete code               |
| Semantic analysis            | `src/sema/checker.cpp` (`checkModule`, `typeOf`, 2 passes)                 | type errors, symbols, per-expression resolved types |
| Symbol table                 | `src/sema/symbols.h` (`Symbol`, `Scope`, `FuncSig`)                        | completion, hover, fn/ref, signature help           |
| Type system                  | `src/sema/type.h` (`Ty`, `toString`)                                       | hover type text, completion detail                  |
| Structured diagnostics       | `src/support/diag.h` (`Diag`, `SpanRange`, `Sev`, `code`, `FixIt`, `Note`) | almost 1:1 mapping to LSP `Diagnostic`              |
| Whole-file pipeline          | `tools/kirn.cpp` `frontEnd(path, src, diags)`                              | repr: lex â†’ parse â†’ check, then render          |
| Module/project model         | `tools/kirn.cpp` `resolveEntry`, `libDirsFor`, `resolveSource`             | workspace root, import resolution, stdlib indexing  |
| Stdlib API surface           | `stdlib/lib/*.kn` (`pub fn`)                                               | completion/hover of `core`, `io`, `math`, etc.      |
| Acceptance suite             | `tests/negative/*.kn`, `*_test.kn`                                         | golden diagnostics fixtures                         |

Strategy: start with a **durable, single-file diagnostics engine** that wraps the
existing C++ `Lexer`+`Parser`+`Checker`, expose it over LSP stdio, and grow
workspace-wide features (imports, stdlib indexing, references, semantic
tokens) phase by phase. No compiler component is invented; each LSP feature is
mapped to the compiler data that already backs it, and anything that must be
added (e.g. a persistent workspace symbol table, a formatter, inlay-hint
positions) is called out explicitly.

---

## 1. KIRN facts this plan is grounded in

Read and confirmed from source:

- **Files**: `.kn` (source), `.cob`/`.kirnlib` (binary build artifacts â€” never
  opened as source).
- **Lexer** (`src/lex/token.h`, `lexer.cpp`): tokens `Eof/Newline/Indent/Dedent/
Ident/Int/Float/Char/StrNormal/StrRaw/StrByte/StrC/FString*/Op/Punct`;
  keywords (frozen) in `isKeyword`; operators longest-match
  (`<<= >>= ..= .?.` â†’ `** // == != <= >= << >> += -= *= /= %= &= |= ^= -> <-
.. =>` â†’ `+-*/%&|^~<>=?@`); `#` line comments (no block); `\` line
  continuation; C-style `{}`+`;` (indentation insignificant).
- **Parser** (`src/parser/parser.cpp`): recursive descent over `grammar/kirn.ebnf`;
  `parseProgram()` returns `std::vector<ast::StmtP>`; on a statement-level error it
  reports a diagnostic and **synchronizes to the next statement end**
  (`syncToStatementEnd`) and continues â€” i.e. built-in error recovery for
  incomplete code.
- **AST** (`src/ast/ast.h`): every node carries `Span{line,col,endLine,endCol}`
  (1-based, inclusive end); `ExKind`/`StKind`/`TyKind`/`PatKind` enums.
- **Checker** (`src/sema/checker.h/.cpp`): `Checker(DiagEngine&)`,
  `checkModule(prog)` (predeclared builtins â†’ `registerNominals` â†’
  `fillNominals` â†’ `registerTopLevel` â†’ body-checking pass), **`typeOf(expr)`**
  returns the resolved `TyP` recorded in `typeCache_` for any expression node,
  `methodLookup(recv, name)` returns an optional `FuncSig`, exhaustiveness and
  borrow checks built in, `LintConfig{allow,deny}` controls lint severities.
- **Symbols** (`src/sema/symbols.h`): `Symbol{kind, name, type, mut, pub,
builtin, used, mutated, declLine, declCol, sig{params,names,ret,variadic,
required}, fields, methods, payloads, variants, enumOf, typeDefaults}`;
  `Scope::find`, `Scope::declare`, lexical scope chain.
- **Types** (`src/sema/type.h`): `Ty{TyK, name, variant, args, inner}` with
  `toString(const Ty&)` and constructor helpers; incl. `Opt`, `Result` sugar via
  `unwrapOpt`/`unwrapResult`.
- **Diagnostics** (`src/support/diag.h`): `Diag{line,col,multi,sev,code,span,notes,
fixIts}` where `span` is a `SpanRange{line,col,endLine,endCol}`; fluent
  builder `engine.error(span).code("E0027").msg("...").note(...).fixit(...)`;
  `SourceMap` + `renderDiags` for the CLI.
- **Project & modules** (`tools/kirn.cpp`):
  - `frontEnd(path, src, diags)` = lexâ†’parseâ†’check one file.
  - `resolveEntry(m, dir)`: `kirn.toml [package] main` â†’ `code/main.kn` â†’
    `main.kn` â†’ `code/pin.kn` â†’ `pin.kn`.
  - `libDirsFor(script)`: `$KIRN_LIBS`, `<proj>/kirn_libs/libs`,
    `<proj>/kirn_libs`, `~/.kirn/kirn-pkg/libs`, `~/.kirn/kirn-pkg`,
    `<proj>/../stdlib`, `<proj>/../../stdlib`, `$KIRN_STDLIB`, `<proj>`.
  - `resolveSource(dotted, dirs, path, src)`: normalizes a dotted import
    (`strip ".kn"`, `.`/`/`â†’`/`, append `.kn`), searches dirs; package-entry
    resolution via `kirn.toml main` / `mod.kn` / `<dir>.kn` / lone `.kn`.
  - `collectImports(src, names)`: lists `import`/`from ... import` module names.
  - CLI: `kirn run/test/build/doc/install/add/update/remove/clone/new`.
  - `kirn doc` already **regenerates an API reference** from `code/` packages â€”
    a natural seed for a symbol index.

---

## 2. Architecture

### 2.1 Process & transport

`kirn-lsp` is a native process speaking **JSON-RPC 2.0 over stdio**
(`Content-Length` framed, exactly like `clangd`/`gopls`/`rust-analyzer`).
stdio is the default transport every LSP client supports; `--stdio` is the
canonical mode. Optional `--log=<path>` writes a framed trace of every message
for debugging.

```
 â”Œâ”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”   JSON-RPC 2.0 over stdio    â”Œâ”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”
 â”‚   Editor     â”‚ â—„â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â–º â”‚       kirn-lsp          â”‚
 â”‚ LSP client   â”‚   Content-Length framed       â”‚  dispatch/transport     â”‚
 â”‚              â”‚                               â”‚  â”Œâ”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”â”‚
 â”‚ (VS Code,    â”‚   textDocument/*              â”‚  â”‚ CompilerService     â”‚â”‚
 â”‚  Neovim,     â”‚   workspace/*                 â”‚  â”‚  Lexer â†’ Parser     â”‚â”‚
 â”‚  Zed, ...)   â”‚   window/logMessage           â”‚  â”‚       â†’ Checker      â”‚â”‚
 â””â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”˜                               â”‚  â”‚  (reused, unchanged) â”‚â”‚
                                                â”‚  â””â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”˜â”‚
                                                â”‚  + ProjectModel (mod idx)â”‚
                                                â”‚  + SymbolIndex (cache)  â”‚
                                                â””â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”˜
```

### 2.5 AI-first design

**Goal:** expose semantic tokens + type info that let an agent (a) jump to symbol origin and (b) complete with type-aware candidates, and maintain a stable machine summary endpoint.

**Must Follow:** semantic tokens must be exact (derived from `typeOf`/Symbol state), definition must resolve to stable symbol origin (declared file/span), completion must include type-aware candidates (locals, top-levels, stdlib members) with signatures, and the server must expose a stable machine summary (document/workspace symbols + module index) consumable by code agents. Incremental updates only on edits; no speculative over-fetching.

**Important Note:** This is additive to editor UX and central to Kirn being AI-first. The summary is machine-consumable JSON; keep it stable and minimal.

> Reference: rust-analyzer — ide-db/semantic tokens (langs-source-code/rust/rust-analyzer); gopls — type-aware features (langs-source-code/go/gopls); pyright — semantic tokens (langs-source-code/cpython/pyright); ruby-lsp — structured navigation (langs-source-code/ruby/ruby-lsp)

### 2.2 The `CompilerService` (thin, read-only wrapper)

The critical design decision: **use the production compiler components as-is**,
as a _read-only_ analysis library, with **no codegen**. A single service wraps:

```cpp
namespace lsp {
struct CompileResult {
    bool lexOk, parseOk, checkOk;
    std::vector<ast::StmtP> body;        // keep AST alive
    std::vector<kirn::Diag> diags;       // structured, spans
    std::unordered_map<const ast::Expr*, sema::TyP> types; // from Checker
};
class CompilerService {
public:
    // one-shot: full lex/parse/check of a buffer
    CompileResult analyze(const std::string& uri, const std::string& src,
                          const LintConfig& cfg);
    // typed stream of top-level symbols for indexing (see Â§5.1)
    void collectSymbols(const std::vector<ast::StmtP>& body,
                        std::vector<lsp::DocSymbol>& out);
};
}
```

Because `Checker` keeps the whole symbol/type state inside one `checkModule`
call scoped to its lifetime, and `ast::Stmt` nodes are `unique_ptr`, the server
caches the resulting AST + `DiagEngine` per open document.

### 2.3 LSP glue

A small hand-rolled LSP layer (initially in C++, later optionally Kirn) handles:
`initialize` (capability negotiation), `initialized`, `shutdown`/`exit`,
`textDocument/didOpen|didChange|didClose|didSave`, and the feature methods. The
spec's mandatory sync (`didOpen`/`didChange`/`didClose`) is implemented with
`TextDocumentSyncKind::Incremental` (2), applying the client's range edits to an
in-memory buffer before re-analysis (Section 6.4).

### 2.4 Project model (`ProjectModel`)

On `initialize`, discover the workspace root:

1. Let `rootUri` (from the client) be the workspace folder.
2. Confirm a project root exists by looking for `kirn.toml`, or canonical
   entries `code/main.kn`/`main.kn`/`code/pin.kn`/`pin.kn` (mirror
   `resolveEntry`). If none, treat the file's folder as a loose "script" scope.
3. Build the **module search dirs** via the exact `libDirsFor(root)` rules and
   pre-resolve the **stdlib** (`stdlib/lib/*.kn`) plus any installed
   `kirn_libs/**`/`~/.kirn/kirn-pkg/**` for indexing.

This keeps `kirn-lsp` import resolution **byte-for-byte identical** to `kirn
run/build`, so "it compiles in the editor" always matches "it compiles in a
terminal".

---

## 3. Capability mapping: what KIRN actually supports

Declared LSP capabilities are keyed to real compiler data. Features without a
reliable backend are **not** advertised until their backend exists (each is
spelled out in a phase).

| LSP feature                                           | Compiler-backing today                           | Phase |
| ----------------------------------------------------- | ------------------------------------------------ | ----- |
| `textDocument/publishDiagnostics` (syntax + semantic) | `Lexer`+`Parser`+`Checker` â†’ `Diag`            | 1     |
| `textDocument/hover`                                  | `Symbol.type` + `Ty::toString` + doc `#` comment | 3     |
| `textDocument/completion` (stdlib, locals)            | `Scope` chain + stdlib `pub fn` index            | 3     |
| `textDocument/signatureHelp`                          | `FuncSig{names, params, ret, required}`          | 3     |
| `textDocument/definition` / `references` / `rename`   | `Symbol.declLine/declCol` + occurrence index     | 4     |
| `textDocument/documentSymbol` / `workspace/symbol`    | top-level `Symbol` collection                    | 4     |
| `textDocument/semanticTokens`                         | `typeOf(expr)` (mutable/immutable) + `Ty`        | 5     |
| `textDocument/inlayHints`                             | `typeOf(expr)` + `FuncSig` (elem types, ret)     | 6     |
| `textDocument/formatting` (a Kirn formatter)          | **must be written** (no formatter exists)        | 7     |
| `textDocument/codeAction`                             | `Diag.FixIt` â†’ quick-fix code actions          | 7     |
| full workspace type-analysis across modules           | **must be built** (Checker is single-module)     | 8     |

---

## 4. Phased roadmap

Each phase is independently shippable and ends with a working, testable server.

| #   | Phase                                         | LSP features                                   | Goal                  |
| --- | --------------------------------------------- | ---------------------------------------------- | --------------------- |
| 0   | Scaffold + transport + project root           | initialize/shutdown, logging                   | a talking server      |
| 1   | Single-file diagnostics                       | publishDiagnostics                             | real errors in editor |
| 2   | Diagnostics fidelity + lint config            | severity mapping, workspaceDiagnostics         | matches CLI exactly   |
| 3   | Hover, completion, signature help             | hover, completion, signatureHelp               | editing intelligence  |
| 4   | Navigation: fn/ref/rename, symbols            | definition, references, rename, documentSymbol | navigate code         |
| 5   | Semantic highlighting + inlay hints           | semanticTokens, inlayHint                      | rich rendering        |
| 6   | Formatting + code actions                     | formatting, codeAction                         | passive auto-edit     |
| 7   | Stdlib-driven features + `kirn doc` API index | stdlib completion/hover, registerPackage       | library-aware         |
| 8   | Workspace/module analysis + incremental cache | cross-module diagnostics, go-to across files   | project intelligence  |
| 9   | Client glue + publishing + Windows/CI         | all clients, release matrix                    | everyone can use it   |

---

### Phase 0 â€” Scaffold, transport, initialization

**Goal:** a `kirn-lsp` binary that handshakes with an LSP client, logs traffic, and cleanly shuts down (reuse kirnc library, no third shipped binary).

**Problem being solved:** there is no LSP server today; the first deliverable is
a correct wire protocol so every later phase plugs into a stable transport.

**Files/components affected:**

- New `lsp/` directory (or `tools/lsp/` to match existing tools):
  - `lsp/transport.h/.cpp` â€” Content-Length frame reader/writer (stdio).
  - `lsp/json.h/.cpp` â€” minimal JSON value + serializer (or vendor a tiny
    header-only JSON; no external dependency).
  - `lsp/rpc.h/.cpp` â€” message dispatch, method routing, request id tracking.
  - `lsp/server.h/.cpp` â€” session state, capability tables, logging.
  - `lsp/main.cpp` â€” `--stdio`, `--log=<path>`, `--version`, `--help`.
- `CMakeLists.txt` â€” build the `kirn-lsp` target (reuse the existing
  `kirn_lex/kirn_parser/kirn_sema/kirn_ast` link libs confirmed in
  `tools/kirn.cpp`).

**Implementation steps:**

1. Implement the JSON-RPC framing: read `Content-Length: N\r\n\r\n` then N bytes;
   write the same header for responses. Reject/handle `Content-Type` and
   `Content-Encoding: gzip` (log-and-skip gzip unless we add compression).
2. Handle `initialize`: read `rootUri`/`workspaceFolders`, `capabilities`;
   return `{ capabilities: { textDocumentSync: {openClose:true, change:2},
hoverProvider, ... } }` advertising only what each phase enabled.
3. Handle `initialized`, `shutdown` (â†’ respond), `exit` (â†’ terminate), and
   `$/cancelRequest`.
4. Add `window/logMessage` + `window/showMessage` for lifecycle events.
5. Project root detection stub: derive `rootUri`, run `resolveEntry`-style probe,
   cache `libDirsFor(root)`.

**Code example (transport framing, conceptual):**

```cpp
// read one request from stdin
struct Frame { std::string method; json::Value params; json::Id id; };
Frame Transport::read() {
    std::string headers;
    long length = 0;
    while (std::getline(std::cin, headers) && headers != "\r") {
        if (headers.rfind("Content-Length:", 0) == 0)
            length = std::stol(headers.substr(15));
    }
    std::string body(length, '\0');
    std::cin.read(&body[0], length);          // binary-safe on Windows
    return json::parse(body);
}
```

**LSP concepts introduced:** `initialize`/`initialized`/`shutdown`/`exit`,
`$/cancelRequest`, `window/logMessage`, capability negotiation, stdio framing.

> Reference: rust-analyzer — LSP transport; gopls — initialize flow; clangd — stdio framing (all in langs-source-code verified)

**Kirn-style example (minimal `.kn` context + JSON-RPC):**

```kn
# minimal context for editor session
mod lsp_demo {
  pub fn ping(x: Int) -> Int { x + 1 }
}
```

```json
{
  "jsonrpc": "2.0",
  "id": 1,
  "method": "initialize",
  "params": { "processId": 123, "rootUri": "file:///project", "capabilities": {} }
}
```

> Reference: rust-analyzer — diagnostics pipeline; gopls — diag publishing; pyright — structured diagnostics (langs-source-code verified)

**Kirn-style example (`.kn` + LSP publishDiagnostics):**

```kn
fn main() {
  x = 10  # inference example
  y: Str = x  # type mismatch
}
```

```json
{
  "jsonrpc": "2.0",
  "method": "textDocument/publishDiagnostics",
  "params": {
    "uri": "file:///p/t.kn",
    "diagnostics": [
      {
        "range": { "start": { "line": 2, "character": 11 }, "end": { "line": 2, "character": 12 } },
        "severity": 1,
        "code": "E0027",
        "source": "kirnc",
        "message": "Type mismatch"
      }
    ]
  }
}
```

> Reference: rust-analyzer — hover/completion; gopls — type-aware completion; pyright — signature help (langs-source-code verified)

**Kirn-style example + hover/completion:**

```kn
fn add(a: Int, b: Int) -> Int { a + b }
```

```json
{
  "jsonrpc": "2.0",
  "method": "textDocument/hover",
  "params": {
    "textDocument": { "uri": "file:///p/t.kn" },
    "position": { "line": 0, "character": 4 }
  }
}
```

```json
{
  "jsonrpc": "2.0",
  "id": 2,
  "method": "textDocument/completion",
  "params": {
    "textDocument": { "uri": "file:///p/t.kn" },
    "position": { "line": 0, "character": 10 }
  }
}
```

> Reference: rust-analyzer — semantic tokens; gopls — semantic highlighting; pyright — semantic tokens (langs-source-code verified)

**Kirn-style example + semantic tokens request:**

```kn
fn f() {
  var x: Int = 1
  var y = x + 1
}
```

```json
{
  "jsonrpc": "2.0",
  "id": 3,
  "method": "textDocument/semanticTokens/full",
  "params": { "textDocument": { "uri": "file:///p/t.kn" } }
}
```

> Reference: rust-analyzer — go-to-fn/refs; gopls — references/rename; pyright — navigation (langs-source-code verified)

**Kirn-style example (`.kn` + definition):**

```kn
fn add(a: Int, b: Int) -> Int { a + b }
var x = add(1,2)
```

```json
{
  "jsonrpc": "2.0",
  "id": 4,
  "method": "textDocument/definition",
  "params": {
    "textDocument": { "uri": "file:///p/t.kn" },
    "position": { "line": 1, "character": 8 }
  }
}
```

**Testing & validation:**

- A `tools/lsp_selftest.*` harness that pipes a scripted sequence of
  `Content-Length` frames into the binary and asserts the `initialize` result.
- Reuse `scripts/*.ps1` pattern: add `scripts/lsp_smoke.ps1`.
- Frame-level unit tests for the header parser (empty body, CRLF variants,
  large bodies).

**Expected result:** `kirn-lsp --stdio` completes an LSP handshake with
`vscode`/`neovim` and logs each message to the trace file.

**Potential limitations:** hand-rolling JSON/RPC is more code than using an SDK;
acceptable because the dependency surface must stay zero and the compiler libs
are C++ (no mainstream LSP SDK is part of the repo today). Guard against Windows
stdio binary-mode corruption (set `stdin`/`stdout` to binary in `main.cpp`).

---

### Phase 1 â€” Single-file diagnostics

**Goal:** compile each open `.kn` file in the editor and publish real syntax and semantic diagnostics with correct ranges (kirnc checker reused).

**Must Follow:** reuse `kirnc` library; publish exact compiler diagnostics; error recovery via existing parser; incremental only on edits.

**Important Note:** Diagnostics must match CLI output byte-for-byte in structure (severities/codes/notes).

**Problem being solved:** the editor currently has no error feedback; this is the
highest-value LSP feature and directly exercises the reused compiler.

**Files/components affected:**

- New: `lsp/analysis.h/.cpp` â€” `CompilerService` (Section 2.2) over
  `Lexer`+`Parser`+`Checker`.
- New: `lsp/diagnostics.h/.cpp` â€” `Diag â†’ LSP Diagnostic` mapping.
- Modify: `lsp/server.cpp` â€” register `textDocument/didOpen|didChange|didClose`,
  publish after each change.

**Implementation steps:**

1. Implement `CompilerService::analyze(uri, src)` = build a `DiagEngine`,
   run `Lexer.lexAll()`, then (if no lex errors) `Parser.parseProgram()`, then
   (if no parse errors) `Checker.checkModule(body)`, collecting `diags` +
   `body` + `typeCache_`.
2. Map every `kirn::Diag` to an LSP `Diagnostic`:
   - `range` â† `span` (`SpanRange{line,col,endLine,endCol}`; convert 1-based
     inclusive to 0-based LSP by `start = (line-1, col-1)`,
     `end = (max(line,endLine)-1, endCol-1)` â€” see Â§4.1 note).
   - `severity` â† `Sev::Error|Warning|Note` â†’ `Error|Warning|Information`.
   - `code` â† `d.code` (e.g. `"E0027"`), `codeDescription` â† the diagnostic doc
     URL `https://â€¦/diagnostics#<code>`; `source = "kirn"`.
   - `relatedInformation` â† `d.notes` (each has a `SpanRange` + message).
3. On `didChange`, re-run `analyze` and `textDocument/publishDiagnostics` with
   the document's `uri` and the mapped array; send `[]` on `didClose`.
4. **Error recovery for incomplete code:** the parser already `syncToStatementEnd`
   and continues, so a half-typed `fn foo(` yields a bounded set of diagnostics
   on the statement rather than spamming the whole file â€” no rework needed.

**LSP features introduced:** `publishDiagnostics`, `textDocumentSync` (openClose

- incremental), `DiagnosticSeverity`, `RelatedInformation`.

> Reference: rust-analyzer — LSP transport; gopls — initialize flow; clangd — stdio framing (all in langs-source-code verified)

**Kirn-style example (minimal `.kn` context + JSON-RPC):**

```kn
# minimal context for editor session
mod lsp_demo {
  pub fn ping(x: Int) -> Int { x + 1 }
}
```

```json
{
  "jsonrpc": "2.0",
  "id": 1,
  "method": "initialize",
  "params": { "processId": 123, "rootUri": "file:///project", "capabilities": {} }
}
```

> Reference: rust-analyzer — diagnostics pipeline; gopls — diag publishing; pyright — structured diagnostics (langs-source-code verified)

**Kirn-style example (`.kn` + LSP publishDiagnostics):**

```kn
fn main() {
  x = 10  # inference example
  y: Str = x  # type mismatch
}
```

```json
{
  "jsonrpc": "2.0",
  "method": "textDocument/publishDiagnostics",
  "params": {
    "uri": "file:///p/t.kn",
    "diagnostics": [
      {
        "range": { "start": { "line": 2, "character": 11 }, "end": { "line": 2, "character": 12 } },
        "severity": 1,
        "code": "E0027",
        "source": "kirnc",
        "message": "Type mismatch"
      }
    ]
  }
}
```

> Reference: rust-analyzer — hover/completion; gopls — type-aware completion; pyright — signature help (langs-source-code verified)

**Kirn-style example + hover/completion:**

```kn
fn add(a: Int, b: Int) -> Int { a + b }
```

```json
{
  "jsonrpc": "2.0",
  "method": "textDocument/hover",
  "params": {
    "textDocument": { "uri": "file:///p/t.kn" },
    "position": { "line": 0, "character": 4 }
  }
}
```

```json
{
  "jsonrpc": "2.0",
  "id": 2,
  "method": "textDocument/completion",
  "params": {
    "textDocument": { "uri": "file:///p/t.kn" },
    "position": { "line": 0, "character": 10 }
  }
}
```

> Reference: rust-analyzer — semantic tokens; gopls — semantic highlighting; pyright — semantic tokens (langs-source-code verified)

**Kirn-style example + semantic tokens request:**

```kn
fn f() {
  var x: Int = 1
  var y = x + 1
}
```

```json
{
  "jsonrpc": "2.0",
  "id": 3,
  "method": "textDocument/semanticTokens/full",
  "params": { "textDocument": { "uri": "file:///p/t.kn" } }
}
```

> Reference: rust-analyzer — go-to-fn/refs; gopls — references/rename; pyright — navigation (langs-source-code verified)

**Kirn-style example (`.kn` + definition):**

```kn
fn add(a: Int, b: Int) -> Int { a + b }
var x = add(1,2)
```

```json
{
  "jsonrpc": "2.0",
  "id": 4,
  "method": "textDocument/definition",
  "params": {
    "textDocument": { "uri": "file:///p/t.kn" },
    "position": { "line": 1, "character": 8 }
  }
}
```

**Testing & validation:**

- Golden tests: for every `tests/negative/n*.kn`, run `CompilerService`, serialize
  the mapped LSP diagnostics sorted by range, and compare to a committed
  `<name>.lsp.json` snapshot that matches what `kirncheck`/`kirnparse` would print.
- Corpus: for every `examples/*.kn`, assert `publishDiagnostics == []` (they
  must be clean), mirroring the existing acceptance contract.
- `tools/kirnlex.cpp`/`kirnparse.cpp` act as the oracle for lex/parse agreement.

**Expected result:** opening any `.kn` shows compiler-accurate squiggles with
ranges and stable codes; type errors appear alongside syntax errors; a broken
statement does not destroy the rest of the file's diagnostics.

**Potential limitations:** full single-module check per keystroke is cheap for
typical files but not for huge ones; Phase 2/8 add the incremental/caching path.
Ranges on multi-line (f-string) tokens are approximate until Phase 2.

---

### Phase 2 â€” Diagnostic fidelity, lint config, workspace diagnostics

**Goal:** make editor diagnostics match the CLI **exactly**, support lint allow/deny, and surface dependency diagnostics.

**Must Follow:** reuse `kirnc` checker (LintConfig); publish multi-URI diagnostics; incremental dependency invalidation.

**Important Note:** Keep CLI parity as rosetta gate.

**Problem being solved:** today `Diag` and the CLI agree, but we must (a) render
`note`/`fixit`/warning severities like the CLI, (b) apply `LintConfig` from a
workspace setting, and (c) check imported files too (so an error in an imported
module is not silent).

**Implementation steps:**

1. Severity mapping pass: ensure `Sev::Warning` and `Sev::Note` (â†’
   `DiagnosticSeverity.Information/Hint`) and `fixIt` â†’ `codeAction` (Phase 6)
   are consistent with `renderDiags` output in `diag.h`.
2. Workspace `kirn.toml` (or a settings section) â†’ `LintConfig{allow,deny}`;
   feed into `Checker` (already supported) so lint diagnostics honor the config.
3. **Dependency diagnostics:** on each change, also run the transitive closure of
   imports (using `collectImports` + `resolveSource` along `libDirsFor(root)`)
   and publish diagnostics for the _other files_ whose modules changed, so the
   editor shows errors inside imported stdlib/`kirn_libs` files.
4. Multi-line/note rendering: pass `SourceMap` from the open buffer so carets and
   related spans are exact; correct the `endCol` inclusiveâ†’exclusive for LSP.

**LSP features introduced:** `Diagnostic.tags` (unnecessary/deprecated, from
`used`/`mutated`), `workspace/didChangeConfiguration` (re-read lint config),
publishing for multiple URIs.

> Reference: rust-analyzer — LSP transport; gopls — initialize flow; clangd — stdio framing (all in langs-source-code verified)

**Kirn-style example (minimal `.kn` context + JSON-RPC):**

```kn
# minimal context for editor session
mod lsp_demo {
  pub fn ping(x: Int) -> Int { x + 1 }
}
```

```json
{
  "jsonrpc": "2.0",
  "id": 1,
  "method": "initialize",
  "params": { "processId": 123, "rootUri": "file:///project", "capabilities": {} }
}
```

> Reference: rust-analyzer — diagnostics pipeline; gopls — diag publishing; pyright — structured diagnostics (langs-source-code verified)

**Kirn-style example (`.kn` + LSP publishDiagnostics):**

```kn
fn main() {
  x = 10  # inference example
  y: Str = x  # type mismatch
}
```

```json
{
  "jsonrpc": "2.0",
  "method": "textDocument/publishDiagnostics",
  "params": {
    "uri": "file:///p/t.kn",
    "diagnostics": [
      {
        "range": { "start": { "line": 2, "character": 11 }, "end": { "line": 2, "character": 12 } },
        "severity": 1,
        "code": "E0027",
        "source": "kirnc",
        "message": "Type mismatch"
      }
    ]
  }
}
```

> Reference: rust-analyzer — hover/completion; gopls — type-aware completion; pyright — signature help (langs-source-code verified)

**Kirn-style example + hover/completion:**

```kn
fn add(a: Int, b: Int) -> Int { a + b }
```

```json
{
  "jsonrpc": "2.0",
  "method": "textDocument/hover",
  "params": {
    "textDocument": { "uri": "file:///p/t.kn" },
    "position": { "line": 0, "character": 4 }
  }
}
```

```json
{
  "jsonrpc": "2.0",
  "id": 2,
  "method": "textDocument/completion",
  "params": {
    "textDocument": { "uri": "file:///p/t.kn" },
    "position": { "line": 0, "character": 10 }
  }
}
```

> Reference: rust-analyzer — semantic tokens; gopls — semantic highlighting; pyright — semantic tokens (langs-source-code verified)

**Kirn-style example + semantic tokens request:**

```kn
fn f() {
  var x: Int = 1
  var y = x + 1
}
```

```json
{
  "jsonrpc": "2.0",
  "id": 3,
  "method": "textDocument/semanticTokens/full",
  "params": { "textDocument": { "uri": "file:///p/t.kn" } }
}
```

> Reference: rust-analyzer — go-to-fn/refs; gopls — references/rename; pyright — navigation (langs-source-code verified)

**Kirn-style example (`.kn` + definition):**

```kn
fn add(a: Int, b: Int) -> Int { a + b }
var x = add(1,2)
```

```json
{
  "jsonrpc": "2.0",
  "id": 4,
  "method": "textDocument/definition",
  "params": {
    "textDocument": { "uri": "file:///p/t.kn" },
    "position": { "line": 1, "character": 8 }
  }
}
```

**Testing & validation:** extend the Phase 1 snapshot suite with warning/note
fixtures; a `kirn.toml` fixture with `[lint] allow=["W0101"]` asserts the lint is
suppressed. Compare floating-point-equivalent diagnostics between `kirncheck`
and `kirn-lsp` over the whole `examples/`+`tests/`+`stdlib/` tree.

**Expected result:** the editor's underline set is identical to `kirn`'s console
output, lint rules are configurable, and imported-module errors surface
including caret/notes.

**Potential limitations:** full dependency-closure re-check on every keystroke
can be slow; mitigated by unchanged-module caching in Phase 8.

---

### Phase 3 â€” Hover, completion, signature help

**Goal:** explain symbols and offer completions while typing with type-aware, exact info from kirnc checker.

**Must Follow:** hover uses Symbol.type + Ty::toString; completion from lexical scopes + stdlib index; signatureHelp from FuncSig with required/variadic; structured Markdown.

**Important Note:** AI-first: candidates must include type detail and stable signatures.

**Problem being solved:** beyond errors, developers need type info and
completion; the checker already resolves all of it.

**Implementation steps:**

1. **Hover (`textDocument/hover`):**
   - Locate the symbol under the cursor: find the innermost AST token at the
     position (walk `body` using spans), then resolve it via the `Scope` chain
     (the checker's scopes) or the symbol tables (`funcs_`, `structs_`, â€¦).
     Where the checker has recorded `typeOf(expr)`, use `Ty::toString`.
   - Compose Markdown: a code block with `Kind: name`, the type
     (`: int` / `-> string`), visibility, and any `#` doc-comment lines
     immediately above the declaration (harvested from the buffer).
2. **Completion (`textDocument/completion`):**
   - Locals in the innermost `Scope` (walk parents) â†’ `Variable` items with the
     type as detail.
   - Top-level `funcs_`/`structs_`/`enums_`/`traits_`/`classes` of the module â†’
     `Function`/`Struct`/`Enum`/`Trait`/`Class` items.
   - After `import lib.foo;`, offer `foo.<member>` from the indexed stdlib
     (Phase 7); within `import mod;`+`.` offer that module's **`pub`** symbols.
   - Keywords from the frozen keyword list (with snippets for `fn`, `if`,
     `match`, `for in`), filtered by what the parser accepts at the position.
   - Respect `isIncomplete` when indexing grows.
3. **Signature help (`textDocument/signatureHelp`):**
   - On a call position, resolve the callee symbol â†’ its `FuncSig{names, params,
ret, required, variadic}`; emit `SignatureInformation` with parameter labels
     and a `MarkupContent` snippet; track `activeParameter` from the cursor.
   - Handle `matchArgs` variadic/optional params (the checker's `required`
     min-arg-count).

**LSP features introduced:** `hover`, `completion` (+ `InsertText`/`snippet`),
`signatureHelp`, `MarkupContent`, `CompletionItemKind` mapping
(`Symbol.kind` â†’ LSP kind).

> Reference: rust-analyzer — LSP transport; gopls — initialize flow; clangd — stdio framing (all in langs-source-code verified)

**Kirn-style example (minimal `.kn` context + JSON-RPC):**

```kn
# minimal context for editor session
mod lsp_demo {
  pub fn ping(x: Int) -> Int { x + 1 }
}
```

```json
{
  "jsonrpc": "2.0",
  "id": 1,
  "method": "initialize",
  "params": { "processId": 123, "rootUri": "file:///project", "capabilities": {} }
}
```

> Reference: rust-analyzer — diagnostics pipeline; gopls — diag publishing; pyright — structured diagnostics (langs-source-code verified)

**Kirn-style example (`.kn` + LSP publishDiagnostics):**

```kn
fn main() {
  x = 10  # inference example
  y: Str = x  # type mismatch
}
```

```json
{
  "jsonrpc": "2.0",
  "method": "textDocument/publishDiagnostics",
  "params": {
    "uri": "file:///p/t.kn",
    "diagnostics": [
      {
        "range": { "start": { "line": 2, "character": 11 }, "end": { "line": 2, "character": 12 } },
        "severity": 1,
        "code": "E0027",
        "source": "kirnc",
        "message": "Type mismatch"
      }
    ]
  }
}
```

> Reference: rust-analyzer — hover/completion; gopls — type-aware completion; pyright — signature help (langs-source-code verified)

**Kirn-style example + hover/completion:**

```kn
fn add(a: Int, b: Int) -> Int { a + b }
```

```json
{
  "jsonrpc": "2.0",
  "method": "textDocument/hover",
  "params": {
    "textDocument": { "uri": "file:///p/t.kn" },
    "position": { "line": 0, "character": 4 }
  }
}
```

```json
{
  "jsonrpc": "2.0",
  "id": 2,
  "method": "textDocument/completion",
  "params": {
    "textDocument": { "uri": "file:///p/t.kn" },
    "position": { "line": 0, "character": 10 }
  }
}
```

> Reference: rust-analyzer — semantic tokens; gopls — semantic highlighting; pyright — semantic tokens (langs-source-code verified)

**Kirn-style example + semantic tokens request:**

```kn
fn f() {
  var x: Int = 1
  var y = x + 1
}
```

```json
{
  "jsonrpc": "2.0",
  "id": 3,
  "method": "textDocument/semanticTokens/full",
  "params": { "textDocument": { "uri": "file:///p/t.kn" } }
}
```

> Reference: rust-analyzer — go-to-fn/refs; gopls — references/rename; pyright — navigation (langs-source-code verified)

**Kirn-style example (`.kn` + definition):**

```kn
fn add(a: Int, b: Int) -> Int { a + b }
var x = add(1,2)
```

```json
{
  "jsonrpc": "2.0",
  "id": 4,
  "method": "textDocument/definition",
  "params": {
    "textDocument": { "uri": "file:///p/t.kn" },
    "position": { "line": 1, "character": 8 }
  }
}
```

**Testing & validation:**

- Fixtures per `examples/` file: put the cursor on a symbol of interest and
  assert the returned hover Markdown contains the expected type string (compare
  against `Ty::toString` oracle).
- Completion snapshot: for a set of cursor positions, assert the set of
  completion labels (locals + top-levels + stdlib).
- Signature help: for `math.pow(a,b)` and `collections.make_list(...)`, assert
  `activeParameter` and parameter names.
- A small headless client (`lsp/client_test.mjs` or reuse the selftest harness)
  drives requests and checks responses.

**Expected result:** hovering a name shows its type; typing `.` after an import
or expression offers members; calling a function shows its signature with the
current parameter highlighted.

**Potential limitations:** without a formatter or a full workspace binder,
completion inside expressions is limited to lexical scope + resolved maps
(Phase 7/8 widen it). Doc comments are picked up from `#` lines only in the same
buffer until Phase 7 indexes them.

---

### Phase 4 â€” Go-to-definition, references, rename, symbols

**Goal:** navigate code and rename identifiers with stable symbol origin.

**Must Follow:** definition must point to declaration span (stable symbol origin); references include declaration by option; rename safe (no builtins/keywords).

**Important Note:** Cross-file in Phase 8; single-file precise now.

**Problem being solved:** symbols carry declaration sites but there is currently
no occurrence index; this phase builds a cheap one and the LSP methods around it.

**Implementation steps:**

1. **Symbol/occurrence index per document:** on `analyze`, walk the AST; for every
   `Ident`/declaration record `{name â†’ list of spans}`. Keep:
   - declaration spans (from `Symbol.declLine/declCol` and `Span`s),
   - reference spans (each `Ident` occurrence),
   - `Symbol.declLine/declCol` as the primary target.
2. **Definition (`textDocument/definition`):** resolve the symbol under the cursor
   to its `SyntaxKind::Function/Struct/â€¦` + `Position{line-1, col-1}` in the
   declaring file (same buffer now; cross-file in Phase 8). If the resolved type
   is from an imported module, point at the module's file once indexed.
3. **References (`textDocument/references`):** return all spans for the symbol
   `name` in the file (options: include declaration). For workspace-wide refs,
   Phase 8 merges across modules.
4. **Rename (`textDocument/rename`):** if the symbol under the cursor is
   user-declared (not builtin / not `ImportRoot`), return `WorkspaceEdit` replacing
   every occurrence span with the new name; reject renaming keywords/`self`/
   builtins with `prepareRename`-style guard (Phase later).
5. **Document symbols (`textDocument/documentSymbol`):** from
   `CompilerService::collectSymbols` â€” top-level `fn/fn/struct/enum/trait/
impl/class/interface/record/const`, each with its `Span` range, plus nested
   `impl` methods. `Workspace symbols` (`workspace/symbol`) in Phase 7 via the
   stdlib + project index.

**LSP features introduced:** `definition`, `references`, `rename` (+
`prepareRename`), `documentSymbol` (hierarchical `DocumentSymbol[]`), later
`workspace/symbol`.

> Reference: rust-analyzer — LSP transport; gopls — initialize flow; clangd — stdio framing (all in langs-source-code verified)

**Kirn-style example (minimal `.kn` context + JSON-RPC):**

```kn
# minimal context for editor session
mod lsp_demo {
  pub fn ping(x: Int) -> Int { x + 1 }
}
```

```json
{
  "jsonrpc": "2.0",
  "id": 1,
  "method": "initialize",
  "params": { "processId": 123, "rootUri": "file:///project", "capabilities": {} }
}
```

> Reference: rust-analyzer — diagnostics pipeline; gopls — diag publishing; pyright — structured diagnostics (langs-source-code verified)

**Kirn-style example (`.kn` + LSP publishDiagnostics):**

```kn
fn main() {
  x = 10  # inference example
  y: Str = x  # type mismatch
}
```

```json
{
  "jsonrpc": "2.0",
  "method": "textDocument/publishDiagnostics",
  "params": {
    "uri": "file:///p/t.kn",
    "diagnostics": [
      {
        "range": { "start": { "line": 2, "character": 11 }, "end": { "line": 2, "character": 12 } },
        "severity": 1,
        "code": "E0027",
        "source": "kirnc",
        "message": "Type mismatch"
      }
    ]
  }
}
```

> Reference: rust-analyzer — hover/completion; gopls — type-aware completion; pyright — signature help (langs-source-code verified)

**Kirn-style example + hover/completion:**

```kn
fn add(a: Int, b: Int) -> Int { a + b }
```

```json
{
  "jsonrpc": "2.0",
  "method": "textDocument/hover",
  "params": {
    "textDocument": { "uri": "file:///p/t.kn" },
    "position": { "line": 0, "character": 4 }
  }
}
```

```json
{
  "jsonrpc": "2.0",
  "id": 2,
  "method": "textDocument/completion",
  "params": {
    "textDocument": { "uri": "file:///p/t.kn" },
    "position": { "line": 0, "character": 10 }
  }
}
```

> Reference: rust-analyzer — semantic tokens; gopls — semantic highlighting; pyright — semantic tokens (langs-source-code verified)

**Kirn-style example + semantic tokens request:**

```kn
fn f() {
  var x: Int = 1
  var y = x + 1
}
```

```json
{
  "jsonrpc": "2.0",
  "id": 3,
  "method": "textDocument/semanticTokens/full",
  "params": { "textDocument": { "uri": "file:///p/t.kn" } }
}
```

> Reference: rust-analyzer — go-to-fn/refs; gopls — references/rename; pyright — navigation (langs-source-code verified)

**Kirn-style example (`.kn` + definition):**

```kn
fn add(a: Int, b: Int) -> Int { a + b }
var x = add(1,2)
```

```json
{
  "jsonrpc": "2.0",
  "id": 4,
  "method": "textDocument/definition",
  "params": {
    "textDocument": { "uri": "file:///p/t.kn" },
    "position": { "line": 1, "character": 8 }
  }
}
```

**Testing & validation:** for each cursor-on-identifier fixture, assert
definition LSP positions match the golden snapshot generated by a Kirn scripted
walk; assert `references` counts on a `tests/*` file; assert `rename` produces
the expected full-file edit; assert document symbols tree shape for an
`examples/*.kn` containing structs/enums/impl.

**Expected result:** Ctrl/Cmd-click on a call jumps to its `fn`; find-references
lists the occurrences in the file; rename rewrites all occurrences; the outline
(and breadcrumbs) shows the module's structure.

**Potential limitations:** without a workspace file index (Phase 8),
references/definition are single-file; shadowing across nested scopes is handled
only approximately by name+span (a resolved symbol object is the precise fix).

---

### Phase 5 â€” Semantic highlighting + inlay hints

**Goal:** theming beyond regex, and inline type labels derived from kirnc typeOf/Symbol state.

**Must Follow:** semantic tokens exact; mutable vs readonly from Symbol.mut; full/delta incremental; inlay hints only when inferred.

**Important Note:** AI-first: semantic tokens feed agent understanding of types/mutability.

**Problem being solved:** TextMate/tree-sitter regex highlighting can't show that a
binding is `mutable` vs `readonly`, or that a name is a type/param/function â€” but
the checker's resolved types can. Inlay hints show inferred element/return types.

**Implementation steps:**

1. **Semantic tokens (`textDocument/semanticTokens`):** walk the AST and for each
   expression with a `typeOf` result emit a token:
   - `Ident` resolving to `Symbol.mut ? "variable"+"mutable" : "variable"+"readonly"`,
   - function/callee â†’ `"function"`,
   - struct/enum/class/interface/trait name â†’ `"type"`,
   - `Ty` via `typeOf` â†’ map `TyK` to token types (`string`,`number`,`boolean`),
   - `self`/`Self` â†’ `"keyword"` (+`defaultLibrary` for builtins),
   - declarations carry the `declaration` modifier.
     Optionally use the **`tokens/full/delta`** variant for incremental updates.
2. **Inlay hints (`textDocument/inlayHint`):** at every `VarDecl`/`ConstDecl`/
   `for`-binding whose type is inferred (no explicit `: T`), emit
   `InlayHint{ position: end-of-target, label: ": " + Ty::toString(type) }`;
   optionally a return-type hint after `fn ... ->` when elided, and element type
   hints on comprehension binds. Respect client capability
   `inlayHint.valueHandling`.
3. Encode tokens using the standard LSP `u32` delta encoding (when the client
   advertises `range`/`full` support).

**LSP features introduced:** `semanticTokens` (+ full/delta), `inlayHint`.

> Reference: rust-analyzer — LSP transport; gopls — initialize flow; clangd — stdio framing (all in langs-source-code verified)

**Kirn-style example (minimal `.kn` context + JSON-RPC):**

```kn
# minimal context for editor session
mod lsp_demo {
  pub fn ping(x: Int) -> Int { x + 1 }
}
```

```json
{
  "jsonrpc": "2.0",
  "id": 1,
  "method": "initialize",
  "params": { "processId": 123, "rootUri": "file:///project", "capabilities": {} }
}
```

> Reference: rust-analyzer — diagnostics pipeline; gopls — diag publishing; pyright — structured diagnostics (langs-source-code verified)

**Kirn-style example (`.kn` + LSP publishDiagnostics):**

```kn
fn main() {
  x = 10  # inference example
  y: Str = x  # type mismatch
}
```

```json
{
  "jsonrpc": "2.0",
  "method": "textDocument/publishDiagnostics",
  "params": {
    "uri": "file:///p/t.kn",
    "diagnostics": [
      {
        "range": { "start": { "line": 2, "character": 11 }, "end": { "line": 2, "character": 12 } },
        "severity": 1,
        "code": "E0027",
        "source": "kirnc",
        "message": "Type mismatch"
      }
    ]
  }
}
```

> Reference: rust-analyzer — hover/completion; gopls — type-aware completion; pyright — signature help (langs-source-code verified)

**Kirn-style example + hover/completion:**

```kn
fn add(a: Int, b: Int) -> Int { a + b }
```

```json
{
  "jsonrpc": "2.0",
  "method": "textDocument/hover",
  "params": {
    "textDocument": { "uri": "file:///p/t.kn" },
    "position": { "line": 0, "character": 4 }
  }
}
```

```json
{
  "jsonrpc": "2.0",
  "id": 2,
  "method": "textDocument/completion",
  "params": {
    "textDocument": { "uri": "file:///p/t.kn" },
    "position": { "line": 0, "character": 10 }
  }
}
```

> Reference: rust-analyzer — semantic tokens; gopls — semantic highlighting; pyright — semantic tokens (langs-source-code verified)

**Kirn-style example + semantic tokens request:**

```kn
fn f() {
  var x: Int = 1
  var y = x + 1
}
```

```json
{
  "jsonrpc": "2.0",
  "id": 3,
  "method": "textDocument/semanticTokens/full",
  "params": { "textDocument": { "uri": "file:///p/t.kn" } }
}
```

> Reference: rust-analyzer — go-to-fn/refs; gopls — references/rename; pyright — navigation (langs-source-code verified)

**Kirn-style example (`.kn` + definition):**

```kn
fn add(a: Int, b: Int) -> Int { a + b }
var x = add(1,2)
```

```json
{
  "jsonrpc": "2.0",
  "id": 4,
  "method": "textDocument/definition",
  "params": {
    "textDocument": { "uri": "file:///p/t.kn" },
    "position": { "line": 1, "character": 8 }
  }
}
```

**Testing & validation:** snapshot semantic-token lists for a corpus file and
assert the mutable-vs-readonly distinction (matches `Symbol.mut` oracle);
assert inlay hints appear only where the type is truly inferred; deltas match
full when applied sequentially.

**Expected result:** bindings declared with `var` render as mutable, plain
assignments as readonly; inferred local/return types show as faint inline
labels; all without any regex grammar.

**Potential limitations:** semantic tokens are slow on very large files unless
the server uses the incremental delta protocol; token ordering must follow the
document order strictly, which requires a deterministic AST walk.

---

### Phase 6 â€” Formatting + code actions

**Goal:** one-click formatting and quick-fix suggestions.

**Problem being solved:** there is no Kirn formatter, and the compiler already
emits `FixIt`s that can be surfaced as code actions.

**Implementation steps:**

1. **Code actions (`textDocument/codeAction`):** map every `Diag.FixIt`
   (`SpanRange` + replacement) to a `CodeAction{kind: quickfix}` with an
   `Edit` over that range; plus `source.fixAll` that applies all fixIts of the
   document (deduplicated, non-overlapping). Code-action kinds:
   `quickfix`, `source.fixAll`, and `refactor.rewrite` for future rewrites.
2. **Formatter (`textDocument/formatting`)** â€” _must be authored_ (no formatter
   exists today). Design: a whitespace-preserving pretty-printer over the AST
   that:
   - normalizes brace style: `fn f(...) {` , `if c {`, matching repo style,
   - 1 blank line between top-level items, standard 4-space body indent,
   - normalizes spacing around `=`/operators and after `,` while **never**
     touching the token text or comments except leading indentation and
     trailing whitespace,
   - leaves f-string contents and string literals byte-identical.
     Emit `TextEdit`s (whole file) or use the semantic approach for ranges.
     Gate behind a capability and a `formatOnSave` client setting.
3. Optional `documentOnTypeFormatting` for `enter` after `{` (block autoindent).

**LSP features introduced:** `codeAction` (+ `source.fixAll`), `formatting`
(+ `documentOnTypeFormatting`), `TextEdit`/`WorkspaceEdit`.

> Reference: rust-analyzer — LSP transport; gopls — initialize flow; clangd — stdio framing (all in langs-source-code verified)

**Kirn-style example (minimal `.kn` context + JSON-RPC):**

```kn
# minimal context for editor session
mod lsp_demo {
  pub fn ping(x: Int) -> Int { x + 1 }
}
```

```json
{
  "jsonrpc": "2.0",
  "id": 1,
  "method": "initialize",
  "params": { "processId": 123, "rootUri": "file:///project", "capabilities": {} }
}
```

> Reference: rust-analyzer — diagnostics pipeline; gopls — diag publishing; pyright — structured diagnostics (langs-source-code verified)

**Kirn-style example (`.kn` + LSP publishDiagnostics):**

```kn
fn main() {
  x = 10  # inference example
  y: Str = x  # type mismatch
}
```

```json
{
  "jsonrpc": "2.0",
  "method": "textDocument/publishDiagnostics",
  "params": {
    "uri": "file:///p/t.kn",
    "diagnostics": [
      {
        "range": { "start": { "line": 2, "character": 11 }, "end": { "line": 2, "character": 12 } },
        "severity": 1,
        "code": "E0027",
        "source": "kirnc",
        "message": "Type mismatch"
      }
    ]
  }
}
```

> Reference: rust-analyzer — hover/completion; gopls — type-aware completion; pyright — signature help (langs-source-code verified)

**Kirn-style example + hover/completion:**

```kn
fn add(a: Int, b: Int) -> Int { a + b }
```

```json
{
  "jsonrpc": "2.0",
  "method": "textDocument/hover",
  "params": {
    "textDocument": { "uri": "file:///p/t.kn" },
    "position": { "line": 0, "character": 4 }
  }
}
```

```json
{
  "jsonrpc": "2.0",
  "id": 2,
  "method": "textDocument/completion",
  "params": {
    "textDocument": { "uri": "file:///p/t.kn" },
    "position": { "line": 0, "character": 10 }
  }
}
```

> Reference: rust-analyzer — semantic tokens; gopls — semantic highlighting; pyright — semantic tokens (langs-source-code verified)

**Kirn-style example + semantic tokens request:**

```kn
fn f() {
  var x: Int = 1
  var y = x + 1
}
```

```json
{
  "jsonrpc": "2.0",
  "id": 3,
  "method": "textDocument/semanticTokens/full",
  "params": { "textDocument": { "uri": "file:///p/t.kn" } }
}
```

> Reference: rust-analyzer — go-to-fn/refs; gopls — references/rename; pyright — navigation (langs-source-code verified)

**Kirn-style example (`.kn` + definition):**

```kn
fn add(a: Int, b: Int) -> Int { a + b }
var x = add(1,2)
```

```json
{
  "jsonrpc": "2.0",
  "id": 4,
  "method": "textDocument/definition",
  "params": {
    "textDocument": { "uri": "file:///p/t.kn" },
    "position": { "line": 1, "character": 8 }
  }
}
```

**Testing & validation:**

- Code actions: a fixture with a `FixIt` (e.g. an obvious correction) asserts the
  generated `CodeAction` edits the right range; `source.fixAll` cleans a set.
- Formatter: run the formatter over every `examples/*.kn` and assert **output is
  a fixed point** (formatting twice = once) and that formatting a correct file
  does not change tokenization (`kirnlex` before == after tokens) except
  whitespace. Add idempotency + token-preservation CI checks.
- Golden formatted outputs for representative files.

**Expected result:** the editor offers quick-fixes from compiler hints and a
format command that yields consistent, token-preserving style.

**Potential limitations:** a formatter must be built and carefully tested for
idempotency across all syntax forms (f-strings, nested lambdas, patterns);
the plan defers it to this phase so the AST/structure is already stable.

---

### Phase 7 â€” Stdlib-driven completion, hover, and API index

**Goal:** make the standard library and installed packages first-class
(completion, hover, cross-file go-to-definition, `workspace/symbol`).

**Problem being solved:** stdlib modules (`core`, `io`, `math`, `strings`,
`collections`, `json`, `os`, `path`, `regexp`, `time`) are the most-used API
surface, but today they are opaque until you read the source. `kirn doc` already
generates an API reference; we reuse that for a machine index.

**Implementation steps:**

1. **Index builder:** on `initialize`/workspace load, for each
   `stdlib/lib/*.kn` and each package under `kirn_libs/**`/`~/.kirn/kirn-pkg/**`,
   run `CompilerService` once and `collectSymbols` into a global **ModuleIndex**
   keyed by normalized module name:
   ```
   ModuleIndex: { mod: "math" â†’ Symbol[] (pub only) â†’ {name, sig, type, declSpan, file} }
   ```
   Reuse `kirn doc`'s regeneration logic (which already walks `code/`) so the
   index and docs never diverge.
2. **Completion across modules:** after `import lib.math;`, typing `math.` offers
   `math`'s `pub fn` names; `from math import pow` offers the imported names.
   Complete module names in `import`/`from` positions from the index.
3. **Hover across modules:** hovering `math.pow` returns the indexed signature +
   doc comment (`#` above the `pub fn`), not just a local string.
4. **Go-to-definition across modules:** definition resolves through the ModuleIndex
   to `file://<stdlib/math.kn>` at the `pub fn` span.
5. **`workspace/symbol`:** query the ModuleIndex + open-document indexes for the
   query string (with a prefix/substring match), returning `SymbolInformation[]`.

**LSP features introduced:** `workspace/symbol`, cross-file `definition`, module
scoping in `completion`, a read-only **library index** ~ the "stdlib symbol table".

> Reference: rust-analyzer — LSP transport; gopls — initialize flow; clangd — stdio framing (all in langs-source-code verified)

**Kirn-style example (minimal `.kn` context + JSON-RPC):**

```kn
# minimal context for editor session
mod lsp_demo {
  pub fn ping(x: Int) -> Int { x + 1 }
}
```

```json
{
  "jsonrpc": "2.0",
  "id": 1,
  "method": "initialize",
  "params": { "processId": 123, "rootUri": "file:///project", "capabilities": {} }
}
```

> Reference: rust-analyzer — diagnostics pipeline; gopls — diag publishing; pyright — structured diagnostics (langs-source-code verified)

**Kirn-style example (`.kn` + LSP publishDiagnostics):**

```kn
fn main() {
  x = 10  # inference example
  y: Str = x  # type mismatch
}
```

```json
{
  "jsonrpc": "2.0",
  "method": "textDocument/publishDiagnostics",
  "params": {
    "uri": "file:///p/t.kn",
    "diagnostics": [
      {
        "range": { "start": { "line": 2, "character": 11 }, "end": { "line": 2, "character": 12 } },
        "severity": 1,
        "code": "E0027",
        "source": "kirnc",
        "message": "Type mismatch"
      }
    ]
  }
}
```

> Reference: rust-analyzer — hover/completion; gopls — type-aware completion; pyright — signature help (langs-source-code verified)

**Kirn-style example + hover/completion:**

```kn
fn add(a: Int, b: Int) -> Int { a + b }
```

```json
{
  "jsonrpc": "2.0",
  "method": "textDocument/hover",
  "params": {
    "textDocument": { "uri": "file:///p/t.kn" },
    "position": { "line": 0, "character": 4 }
  }
}
```

```json
{
  "jsonrpc": "2.0",
  "id": 2,
  "method": "textDocument/completion",
  "params": {
    "textDocument": { "uri": "file:///p/t.kn" },
    "position": { "line": 0, "character": 10 }
  }
}
```

> Reference: rust-analyzer — semantic tokens; gopls — semantic highlighting; pyright — semantic tokens (langs-source-code verified)

**Kirn-style example + semantic tokens request:**

```kn
fn f() {
  var x: Int = 1
  var y = x + 1
}
```

```json
{
  "jsonrpc": "2.0",
  "id": 3,
  "method": "textDocument/semanticTokens/full",
  "params": { "textDocument": { "uri": "file:///p/t.kn" } }
}
```

> Reference: rust-analyzer — go-to-fn/refs; gopls — references/rename; pyright — navigation (langs-source-code verified)

**Kirn-style example (`.kn` + definition):**

```kn
fn add(a: Int, b: Int) -> Int { a + b }
var x = add(1,2)
```

```json
{
  "jsonrpc": "2.0",
  "id": 4,
  "method": "textDocument/definition",
  "params": {
    "textDocument": { "uri": "file:///p/t.kn" },
    "position": { "line": 1, "character": 8 }
  }
}
```

**Testing & validation:**

- Assert `math.` completion contains `sqrt`, `pow`, `floor`, etc. from
  `stdlib/lib/math.kn`'s `pub fn`s (verify those exact names exist in the file
  first).
- Hover on `json.parse(...)` returns the indexed signature + doc comment.
- Go-to-fn from an app file jumps into `stdlib/lib/json.kn`.
- `workspace/symbol "ser"` over a fixture workspace returns the expected matches.

**Expected result:** stdlib is fully browsable/complete/clickable inside the
editor, identical to what `kirn doc` documents.

**Potential limitations:** the index is a snapshot taken at startup + on package
install; watching many installed packages for changes needs a file watcher
(optional `workspace/didChangeWatchedFiles`) â€” deferred to Phase 8.

---

### Phase 8 â€” Workspace/module analysis + incremental caching

**Goal:** cross-module diagnostics, references and go-to-definition across the
whole project, with caching so per-keystroke analysis stays fast.

**Problem being solved:** today the checker analyzes a single module in one
`checkModule` call; imports are resolved and checked at runtime but not as a
linked workspace. We need a persistent binder/index and invalidation.

**Implementation steps:**

1. **Project binder (`ProjectModel`):** at workspace load, resolve the entry via
   `resolveEntry` and its transitive imports via `collectImports`+`resolveSource`
   (mirror `gatherEmbedded` in `tools/kirn.cpp`). Build a **module graph**:
   `mod â†’ AST + symbols + per-document index`.
2. **Partial re-analysis:**
   - On `didChange` of file F, re-`analyze` F and invalidate only F's
     dependents (files that import F, via reverse edges).
   - Re-check dependent modules lazily and publish their diagnostics too.
   - Keep each open buffer's `typeOf` cache alive; cache module ASTs keyed by
     resolved path (mirror `Interpreter::loadedModules_`).
3. **Cross-file navigation:** resolve references/definition/rename through the
   module graph (an identifier's resolved `Symbol` â†’ declaring module + span).
   Rename across files batched into one `WorkspaceEdit`.
4. **Reference indexing:** optional lightweight "name â†’ modules" posting list so
   workspace references don't re-scan the whole project for every query.
5. **File watching:** advertise `workspace/didChangeWatchedFiles` to refresh the
   index when a `.kn` under the project/stdlib changes on disk.

**LSP features introduced:** `workspace/didChangeWatchedFiles`, cross-file
`references`/`definition`/`rename`, dependency-aware `publishDiagnostics`,
`workspace/workspaceFolders` handling (multi-root).

> Reference: rust-analyzer — LSP transport; gopls — initialize flow; clangd — stdio framing (all in langs-source-code verified)

**Kirn-style example (minimal `.kn` context + JSON-RPC):**

```kn
# minimal context for editor session
mod lsp_demo {
  pub fn ping(x: Int) -> Int { x + 1 }
}
```

```json
{
  "jsonrpc": "2.0",
  "id": 1,
  "method": "initialize",
  "params": { "processId": 123, "rootUri": "file:///project", "capabilities": {} }
}
```

> Reference: rust-analyzer — diagnostics pipeline; gopls — diag publishing; pyright — structured diagnostics (langs-source-code verified)

**Kirn-style example (`.kn` + LSP publishDiagnostics):**

```kn
fn main() {
  x = 10  # inference example
  y: Str = x  # type mismatch
}
```

```json
{
  "jsonrpc": "2.0",
  "method": "textDocument/publishDiagnostics",
  "params": {
    "uri": "file:///p/t.kn",
    "diagnostics": [
      {
        "range": { "start": { "line": 2, "character": 11 }, "end": { "line": 2, "character": 12 } },
        "severity": 1,
        "code": "E0027",
        "source": "kirnc",
        "message": "Type mismatch"
      }
    ]
  }
}
```

> Reference: rust-analyzer — hover/completion; gopls — type-aware completion; pyright — signature help (langs-source-code verified)

**Kirn-style example + hover/completion:**

```kn
fn add(a: Int, b: Int) -> Int { a + b }
```

```json
{
  "jsonrpc": "2.0",
  "method": "textDocument/hover",
  "params": {
    "textDocument": { "uri": "file:///p/t.kn" },
    "position": { "line": 0, "character": 4 }
  }
}
```

```json
{
  "jsonrpc": "2.0",
  "id": 2,
  "method": "textDocument/completion",
  "params": {
    "textDocument": { "uri": "file:///p/t.kn" },
    "position": { "line": 0, "character": 10 }
  }
}
```

> Reference: rust-analyzer — semantic tokens; gopls — semantic highlighting; pyright — semantic tokens (langs-source-code verified)

**Kirn-style example + semantic tokens request:**

```kn
fn f() {
  var x: Int = 1
  var y = x + 1
}
```

```json
{
  "jsonrpc": "2.0",
  "id": 3,
  "method": "textDocument/semanticTokens/full",
  "params": { "textDocument": { "uri": "file:///p/t.kn" } }
}
```

> Reference: rust-analyzer — go-to-fn/refs; gopls — references/rename; pyright — navigation (langs-source-code verified)

**Kirn-style example (`.kn` + definition):**

```kn
fn add(a: Int, b: Int) -> Int { a + b }
var x = add(1,2)
```

```json
{
  "jsonrpc": "2.0",
  "id": 4,
  "method": "textDocument/definition",
  "params": {
    "textDocument": { "uri": "file:///p/t.kn" },
    "position": { "line": 1, "character": 8 }
  }
}
```

**Testing & validation:** the flagship check â€” **rosetta validation**: for the
whole `examples/`, `stdlib/`, and `tests/`, the LSP's cross-module diagnostics
must equal running the real build (`kirn build`/`kirncheck`) over the same tree.
A large synthetic workspace with many modules tests incremental invalidation and
cache correctness (edits to a leaf module only re-check its dependents).

**Expected result:** editing any imported file updates dependent errors; go-to-fn
and find-references work across files; re-analysis is incremental and passes the
rosetta-oracle equivalence test.

**Potential limitations:** cross-module _type_ checking would require reworking
`Checker` into a cross-module binder (currently private per-module state); the
plan scopes this phase to **diagnostics + symbol navigation** across files, and
defers true cross-module type unification to a research spike, clearly flagged as
future work (Section 7).

---

### Phase 9 â€” Client glue, publishing, Windows/CI

**Goal:** every target editor connects to the same `kirn-lsp` on all platforms.

**Must Follow:** stdio binary-mode; URI<->path correct; ship prebuilt binaries; CI includes rosetta oracle.

**Important Note:** Single contract is CLI --stdio; client configs kept minimal.

**Problem being solved:** a server is useless until wired into clients and shipped.

**Implementation steps â€” per-client glue (all reuse the same binary):**

- **VS Code (optional but richest):** a `kirn-vscode` extension contributes the
  language (`id: kirn`, `extensions: [.kn]`) and a **language-server client**
  using `vscode-languageclient`, `initializeOptions` passing the workspace root,
  plus semantic-token/inlay-hint client capabilities.
- **Neovim** (`nvim-lspconfig`):
  ```lua
  require('lspconfig.configs').kirn = {
    default_config = {
      cmd = { 'kirn-lsp' },                    -- on PATH
      filetypes = { 'kirn' },
      root_dir = function(fname)
        return vim.fs.root(fname, { 'kirn.toml', 'code/main.kn', 'main.kn' })
          or vim.fn.getcwd()
      end,
      settings = { kirn = { lint = { deny = { 'W0101' } } } },
    },
  }
  -- also register the filetype:
  vim.filetype.add({ extension = { co = 'kirn' } })
  ```
  Optionally `nvim-treesitter` for structural highlighting (from
  `KIRN_HIGHLIGHT_PLAN.md`), with `kirn-lsp` handling semantic features.
- **Vim:** `vim-lsp`/`vim-lsp-settings` entry:
  ```vim
  let g:lsp_diagnostics_enabled = 1
  call lsp#register_server({
      \ 'name': 'kirn',
      \ 'cmd': {server_info->['kirn-lsp']},
      \ 'allowlist': ['kirn'],
      \ })
  ```
- **Zed:** an extension with `languages/kirn/config.toml` setting
  `language_servers = ["kirn-lsp"]` + a `command`/`args` entry, reusing the
  tree-sitter grammar for highlighting and `kirn-lsp` for intelligence.
- **Helix:** `languages.toml`:
  ```toml
  [[language]]
  name = "kirn"
  scope = "source.kirn"
  file-types = ["co"]
  comment-tokens = "#"
  [language-server.kirn-lsp]
  command = "kirn-lsp"
  ```
- **JetBrains:** bundle the TextMate grammar (highlighting, from
  `KIRN_HIGHLIGHT_PLAN.md`) + enable the **LSP plugin** configured to run
  `kirn-lsp`; or later the Grammar-Kit plugin fingers the same binary.
- **Emacs/Eglot, Kate, Sublime LSP, â€¦:** point the standard LSP client at
  `kirn-lsp` with `.kn` â†’ `kirn`.

**Release & distribution:**

- GitHub Releases carry prebuilt `kirn-lsp` binaries for Windows/macOS/Linux
  (host + cross matrix already supported by the repo's build system).
- A `KIRN_LSP_VERSION`/tag aligned with the grammar version (language pins
  `0.0.1-beta`); each new release runs the full test matrix (Â§9 below).
- Provide `kirn-lsp --version` and `--help`.

**Cross-platform & Windows notes:**

- stdio must be binary-mode (`_setmode(_fileno(stdin),_O_BINARY)` etc.) so JSON
  framing is not corrupted.
- Paths: normalize `/` vs `\`; URIâ†”path conversion (file scheme, percent-encode).
- `libDirsFor`/`globalPkgDir` already handle `USERPROFILE`/`HOME` correctly.
- CI (`.github/workflows/ci.yml`) adds a job: build all targets on
  win/mac/linux, run `lsp_selftest` + the full golden suite.

**Testing & validation (whole plan):**

- Unit: transport, JSON, URIâ†”path, Diagâ†’Diagnostic mapping.
- Golden: per-`tests/negative`, per-`examples`, per-stdlib diagnostics/completion/
  hover/fn/symbol snapshots.
- End-to-end: drive each editor's LSP client against the binary and assert a
  scripted openâ†’editâ†’hoverâ†’completeâ†’fn cycle.
- Rosetta: LSP diagnostics == `kirncheck`/`kirnparse`/`kirn build` over the whole
  repo, as the authoritative equivalence gate.

**Expected result:** `kirn-lsp` installable and correct in every listed editor
and on all three platforms, with a CI gate guaranteeing the editor never
disagrees with the compiler.

**Potential limitations:** maintaining several client glue projects is
migration work; mitigations: centralize the wiring table in one place, keep the
client configs tiny, and treat the CLI `--stdio` mode as the single contract.

---

## 5. Server internals detail

### 5.1 Symbol collection for indexes

`CompilerService::collectSymbols` walks top-level statements and returns a
flat list of `lsp::DocSymbol{kind, name, detail(sig), range, selectionRange,
children}` using `Span` offsets. For a struct it includes field names; for an
`impl Block`, its methods as children â€” feeding `documentSymbol`, the ModuleIndex
(Phase 7), and the project binder (Phase 8).

### 5.2 Diagnostic mapping table

| `kirn::Diag`                   | LSP `Diagnostic`              | Notes                                                   |
| ------------------------------ | ----------------------------- | ------------------------------------------------------- |
| `span.line/col/endLine/endCol` | `range`                       | 1-based inclusive â†’ 0-based; guard `endCol` â‰¥ `col` |
| `sev`                          | `severity`                    | Error/Warning/Information/Hint                          |
| `code`                         | `code` + `codeDescription`    | `"E0027"` stable                                        |
| `message`                      | `message`                     |                                                         |
| `notes[]`                      | `relatedInformation[]`        | same file for now                                       |
| `fixIts[]`                     | `codeAction`/`edit` (Phase 6) |                                                         |

### 5.3 Position convention

KIRN is 1-based inclusive (`Span.endCol` = last char). LSP is 0-based,
half-open. Provide a single `toLspRange(SpanRange)` used everywhere so the
conversion is never fudged per call site.

### 5.4 Incremental document sync

`TextDocumentSyncKind=2`. Maintain an in-memory buffer per URI; on `didChange`
apply the client's `[start, end]` text edits to the buffer, then re-`analyze`
the whole buffer (fall back to full re-parse until Phase 8 adds true
incremental _parse_ caching; the buffer edit itself is always incremental).

---

## 6. Frequently asked questions (grounded answers)

- **Why reuse the C++ front end instead of writing the LSP in Kirn?** The
  compiler, checker, symbols, types, diagnostics, and module model are already a
  battle-tested C++ library (see Â§1) and are reused unchanged.
- **Why not implement LSP in Node/Python?** The analysis library is C++; a
  native binary ships as a single static server with no runtime, matching the
  repo's zero-dependency tooling philosophy and its existing C++ build/link
  target list.
- **How do we avoid LSP/compiler divergence?** The rosetta oracle (`kirn-lsp`
  diagnostics == `kirncheck`/`kirn build`) runs in CI over the whole corpus.

---

## 7. Explicitly-required additions (do not already exist)

These are the only things the plan genuinely needs to _add_; everything else is
reuse:

1. **JSON-RPC / LSP transport + dispatch layer** (Phase 0).
2. **A Kirn formatter** â€” no formatter exists today; must be written (Phase 6).
3. **A persistent workspace binder + module graph / index** (Phase 7/8) â€” the
   checker is single-module and does not expose cross-module type unification;
   we build a binder for diagnostics + navigation (not for full cross-module type
   checking, which is flagged as future research).
4. **Semantic-token encoder + inlay-hint position logic** (Phase 5) â€” the data
   exists (`typeOf`, `Symbol.mut`), the LSP encoding does not.

Each addition is minimal, well-scoped, and justified by the feature it enables.

---

_End of plan. Like every other KIRN plan, each phase should open with a short
"research + source review" note (citing `src/sema/*`, `src/parser/*`,
`src/lex/*`, `tools/kirn.cpp`, `stdlib/lib/*`, `tests/negative/*`) before code â€”
the goal is a server that reproduces the compiler's behavior exactly, reusing
what exists and flagging only what must be built._
