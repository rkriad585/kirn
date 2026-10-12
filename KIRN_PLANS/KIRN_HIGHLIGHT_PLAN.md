# KIRN Editor & Syntax Extension Implementation Plan

## Goal

Define Kirn's syntax-highlighting and grammar plan to industry standard —
TextMate / VS Code plus LSP semantic tokens, with a tree-sitter grammar where an
editor uses one — all generated from ONE grammar source (the `kirn` / `kirnc`
front end and `grammar/kirn.ebnf`) so tokenization stays identical across every
editor and across AI/agent tooling.

## Must Follow

- **(a) Two binaries only.** The toolchain build ships EXACTLY `kirn` (driver)
  and `kirnc` (checker/compiler); all highlighting/grammar output is generated,
  never a third executable.
- **(b) No over-engineering.** Reuse the existing grammar/EBFN as the single
  source of truth; never hand-maintain parallel keyword/operator/scope copies.
- **(c) AI-first.** The same generated token data must feed both the LSP
  semantic tokens and agent tokenization, so humans and agents see identical
  tokenization.
- **(d) Kirn-style spellings.** Lowercase width micros (`i4..u256`,
  `f4..uf256`) and capitalized primitives (`Int`, `Float`, `Bool`, `Str`,
  `String`, `Char`, `BigInt`, `Rational`, `UInt`/`UFloat`, `UInt4..UInt256`,
  `Int4..Int256`, `Float4..Float256`, `ISize`/`USize`, `Bool8..`, `Char8..`);
  free builtins/types are CapitalCase (`Str`, `BigInt`, `__Sqrt`, `Min`, `Max`),
  methods lowercase.
- **(e) No user-identifier special-casing.** Highlight rules apply only to
  grammar-defined keywords, builtins, types, and literal forms — never to
  arbitrary user identifiers.

## Important Note

Keep every existing per-token and per-editor plan in this document; these
sections add the mandate, they do not replace it. The grammar and highlight
tests (`editor/test/run.mjs`, `tree-sitter test`) MUST stay green on every
change.

---

- **Status:** DRAFT (roadmap, not yet executed)
- **Author:** RK Riad Khan (`rkriad585`)
- **Date:** 2026-09-04
- **Repo:** `github.com/rkriad585/kirn`
- **Language:** Kirn (see `grammar/kirn.ebnf`, a.k.a. "the normative grammar")

---

## 0. Executive summary

**Goal:** frame the one-source → many-editors pipeline so every highlighter stays in sync.
**Must Follow:** one normative definition; ship only `kirn`+`kirnc`; keep the three shared foundations; no per-editor keyword forks.
**Important Note:** this summary is the contract the phases below must honor, not a replacement for them.

This plan defines how to give the **Kirn programming language** high-quality,
consistent developer tooling across VS Code, Vim, Neovim, Micro, Zed,
Highlight.js, and JetBrains IDEs â€” and, later, a full **Language Server**
(`kirn-lsp`).

The strategy is deliberately **not "one grammar everywhere."** Modern editors
each have native integrations that are best served by different technologies.
We therefore build on three shared foundations, then adapt per editor:

1. **A shared language definition** (scopes, keywords, operators, literals,
   comments, bracket pairs, indentation, folding) that every integration
   derives from â€” so all editors stay in sync with one source of truth.
2. **A TextMate grammar (`kirn.tmLanguage.json`)** as the mainstream,
   regex-based highlighter used by VS Code, JetBrains, Sublime, and the
   editor-coloring backends of Micro and others.
3. **A tree-sitter grammar (`tree-sitter-kirn`)** for the modern, incremental,
   error-tolerant editors (Zed, Neovim, Helix, GitHub) and for structural
   features (folding, indent, structural selection).
4. **A language server (`kirn-lsp`)** phase that reuses the compiler's own
   lexer, parser, and checker to provide diagnostics, completion, hover,
   go-to-definition, and semantic highlighting.

A build/generation pipeline keeps the TextMate and tree-sitter grammars and
every editor highlighter synchronized whenever `grammar/kirn.ebnf`, the lexer
keyword list (`src/lex/lexer.cpp`), or the standard library change.

---

## 1. Understanding KIRN (facts the plan is grounded on)

**Goal:** ground the plan in verified lexer/grammar facts, not invented tokens.
**Must Follow:** cite `grammar/kirn.ebnf` and `src/lex/*`; record Kirn type/builtin spellings and contextual keywords exactly.
**Important Note:** re-verify these facts whenever the lexer changes.

Everything below is derived from actual source; nothing is invented.

### 1.1 Files and extension

**Goal:** pin the file extension and language id every editor registers.
**Must Follow:** keep `.kn`/`.kn` source and never treat `.cob`/`.kirnlib` as source; register scope `source.kirn`.
**Important Note:** the extension is metadata; tokenization always comes from the shared grammar.

- Source files use the **`.kn`** extension (enforced in `.gitattributes`:
  `*.kn text eol=lf`). Primary detection extension for every editor.
- The `.gitattributes` also declares `*.cob` and `*.kirnlib` as binary
  (compiled/archive output), so they must **not** be treated as Kirn source.
- The compiler CLI and tools already exist in `tools/`:
  - `kirn` â€” main driver.
  - `kirncheck` â€” check-only (no codegen).
  - `kirnlex` â€” dump tokens (`Tok`, line, col).
  - `kirnparse` / `kirnparse --ast` â€” parse only, exit 0/1, emits
    `<file>:<line>:<col>: error: <msg>`.
  - `kirnrun` â€” run through the interpreter.
- **Implication for LSP:** the compiler's lexer/parser emit the exact `line:col`
  diagnostics an LSP needs. This sharply lowers the cost of building `kirn-lsp`.

**Language-contribution sample (how editors detect the file):**

```json
{
  "id": "kirn",
  "scopeName": "source.kirn",
  "extensions": [".kn", ".kn"],
  "aliases": ["Kirn"],
  "configuration": "./language-configuration.json"
}
```

### 1.2 Lexical model (canonical, from `src/lex/token.h` + `lexer.cpp`)

**Goal:** mirror the lexer's token classes exactly so no editor guesses.
**Must Follow:** cover comments, continuation, numbers, chars, all string forms incl. f-strings and operators; stay case-sensitive (`self`/`Self`, `none`/`None`).
**Important Note:** the `Tok` enum is the token-kind source; contextual words (`gather`, `yield`, `item`) are not reserved.

Token kinds: `Eof, Newline, Indent, Dedent, Ident, Int, Float, Char,
StrNormal, StrRaw, StrByte, StrC, FStringStart/Text/LBrace/RBrace/Colon/Spec/
End, Op, Punct`.

- **Comments:** line-only, `# ...` to end of line. **No block comments.**
  (The lexer skips `#` â†’ end-of-line at `lexer.cpp`.)
- **Line continuation:** trailing `\` at end of line (explicit continuation,
  no NEWLINE token).
- **Indentation is insignificant** â€” the grammar is C-style: `{ }` blocks and
  `;` terminators. `INDENT`/`DEDENT` are emitted but replaced by whitespace
  skipping in the lexer. This matters for _indentation_ and _folding_ rules in
  editors: auto-indent follows `{`/`}` depth, not Python-style indentation
  blocks.
- **Keywords (frozen v1 set, `isKeyword` in `lexer.cpp`):**

  ```
  fn var if elif else while for in return break continue match case
  struct enum trait impl import export pub defer spawn chan select try raise
  catch unsafe extern new box self Self and or not is as true false none pass
  class interface record implements extends dynamic None del pr local global
  temp bucket
  ```

  `gather`, `yield`, and `item` are **contextual** (used in specific
  statements but not reserved) â€” a truthful highlighter should treat them as
  context words, not universal keywords.

- **Operators (longest-match order):**
  - 3-char: `<<= >>= ..= .?.`
  - 2-char: `** // == != <= >= << >> += -= *= /= %= &= |= ^= -> <- .. =>`
  - 1-char: `+ - * / % & | ^ ~ < > = ? @`
  - Punctuation: `( ) [ ] { } , : . ;`
- **Number literals:** decimal; `0x` hex, `0o` octal, `0b` binary; `_`
  digit separators; float with `.` and `e`/`E` exponent (`1_000.5e10`); invalid
  to mix with an identifier suffix (`123abc` is an error).
- **Character literals:** `'a'`, `'\n'`, `'\u{1F600}'` etc.
- **String literals:**
  - Normal: `"..."` (`StrNormal`).
  - Raw: `r"..."` (`StrRaw`) â€” no escape processing.
  - Byte: `b"..."` (`StrByte`).
  - C-string: `c"..."` (`StrC`).
  - **f-strings:** `f"..."` with interpolation `{expr}`, optional format
    spec after `:` (e.g. `f"{x:08}"`), and nested braces inside `expr`.
    These are multi-token in the lexer (`FStr# ... FStrEnd`) and are the
    single trickiest token class for regex grammars.

**Token-zoo sample (`token_zoo.kn`):**

```kirn
# Kirn token zoo
fn main(x: Int) -> Str {
    var ratio: Float = 1_000.5e10
    var raw = r"C:\path"
    var msg = f"x={x:04}, ratio={ratio}"
    return msg
}
```

### 1.3 Standard library modules (for completion/diagnostics phases)

**Goal:** inventory stdlib modules for completion and builtin highlighting.
**Must Follow:** derive names from `stdlib/lib/*.kn` `pub fn`; keep free builtins CapitalCase (`__Sqrt`, `Min`, `Max`).
**Important Note:** the builtin inventory is generated, never hand-typed.

`stdlib/lib/`: `core`, `io`, `math`, `strings`, `collections`, `json`, `os`,
`path`, `regexp`, `time`; plus `stdlib/text/slug.kn`. Public entry points use
`pub fn`, imports use `import lib.<mod>;` / `import <mod>;` /
`from <mod> import <name> as <alias>;`.

**Stdlib/import sample (`norm.kn`):**

```kirn
from math import Sqrt
import strings

fn norm(v: Float) -> Float {
    return Sqrt(v)
}
```

### 1.4 Existing label / scope naming

**Goal:** establish scope/label naming for every integration.
**Must Follow:** use scope `source.kirn`, extension `.kn`, id `kirn`; no integration invents its own scope vocabulary.
**Important Note:** this is greenfield; the scope contract lives in §6.

No TextMate scope, tree-sitter, or Linguist entry exists yet. This is greenfield.
We will register `source.kirn` scope, `.kn` extension, and the language id
`kirn` everywhere.

---

**Scope sample (type names only, never user identifiers):**

```jsonc
{ "match": "\\b(Int|Float|Bool|Str|BigInt)\\b", "name": "support.type.kirn" }
```

## 2. The shared source-of-truth: `kirn-syntax`

**Goal:** make `kirn-syntax` the one normative definition all editors consume.
**Must Follow:** generated artifacts only; no editor-specific keyword lists; produce them via the `kirn`/`kirnc` toolchain.
**Important Note:** a language change that skips regeneration must fail CI.

Before writing per-editor files, establish a **single normative definition** that
all integrations consume.

### 2.1 Canonical files (checked into the repo)

**Goal:** list the canonical files checked into the repo.
**Must Follow:** keep `spec/kirn.json` as THE definition; every grammar/highlighter is generated from it.
**Important Note:** never edit generated artifacts by hand.

```
editor/
  kirn-syntax/
    README.md                     # how the package works + how to regenerate
    package.json                  # consumes grammar/spec and emits artifacts
    spec/
      kirn.json                   # THE shared language definition (below)
      keyword-frozen.json         # snapshot: keywords + operators + literals
      builtins.json               # stdlib module/function inventory (for LSP)
    grammars/
      kirn.tmLanguage.json        # TextMate
      tree-sitter-kirn/           # tree-sitter grammar + queries (Phase 3)
    highlight/                    # generated per-platform highlighters
    test/
      corpus/                     # .kn snippets with expected scopes/tokens
      run.mjs                     # test runner
```

**Generated-spec sample (`spec/kirn.json`, do not hand-edit):**

```jsonc
{
  "languageId": "kirn",
  "scopeName": "source.kirn",
  "fileExtensions": [".kn"],
  "lineComment": "#",
  "tokenKinds": ["Eof", "Ident", "Int", "Float", "StrNormal", "Op", "Punct"]
}
```

### 2.2 `spec/kirn.json` â€” the single definition

**Goal:** define the single JSON definition every generator reads.
**Must Follow:** generate it from `grammar/kirn.ebnf` + the lexer keyword/operator lists; include the ordered token-kind list.
**Important Note:** the `tokenKinds` array is the AI/agent contract — agents tokenize Kirn from it, never from a re-implemented lexer.

Content is generated from `grammar/kirn.ebnf` + the lexer keyword/operator
lists (see Â§2.3 synchronization). It captures:

```json
{
  "languageId": "kirn",
  "scopeName": "source.kirn",
  "fileExtensions": [".kn"],
  "lineComment": "#",
  "blockCommentStart": null,
  "blockCommentEnd": null,
  "caseInsensitive": false,
  "keywords": ["fn", "fn", "var", "...", "bucket"],
  "contextualKeywords": ["gather", "yield", "item"],
  "literals": ["true", "false", "none", "None"],
  "operators3": ["<<=", ">>=", "..=", ".?."],
  "operators2": [
    "**",
    "//",
    "==",
    "!=",
    "<=",
    ">=",
    "<<",
    ">>",
    "+=",
    "-=",
    "*=",
    "/=",
    "%=",
    "&=",
    "|=",
    "^=",
    "->",
    "<-",
    "..",
    "=>"
  ],
  "operators1": ["+", "-", "*", "/", "%", "&", "|", "^", "~", "<", ">", "=", "?", "@"],
  "punct": "()[]{},:.;",
  "identifier": "[A-Za-z_][A-Za-z0-9_]*",
  "numberPatterns": {
    "int": "\\b(?:0[xX][0-9a-fA-F_]+|0[oO][0-7_]+|0[bB][01_]+|\\d[\\d_]*)\\b",
    "float": "\\b\\d[\\d_]*(?:\\.\\d[\\d_]*)?(?:[eE][+-]?\\d+)?(?!\\w)"
  },
  "charLiteral": "'(?:[^'\\\\\\n]|\\\\.|\\\\u\\{[0-9a-fA-F]+\\})'",
  "string": {
    "normal": "\"(?:[^\"\\\\\\n]|\\\\.)*\"",
    "raw": "r\"(?:[^\"]|\\\")*\"",
    "byte": "b\"(?:[^\"\\\\\\n]|\\\\.)*\"",
    "c": "c\"(?:[^\"\\\\\\n]|\\\\.)*\""
  },
  "fstring": {
    "start": "f\"",
    "interpolation": "\\{[^{}]*(?:\\{[^{}]*\\}[^{}]*)*\\}",
    "formatSpec": "\\{[^{}]*:[^{}]*\\}"
  },
  "stdlib": {
    "modules": [
      "core",
      "io",
      "math",
      "strings",
      "collections",
      "json",
      "os",
      "path",
      "regexp",
      "time"
    ],
    "builtinsFile": "spec/builtins.json"
  }
}
```

**AI-first token/list contract.** `spec/kirn.json` MUST also emit a generated,
ordered token-kind list (the lexer's `Tok` enum mirrored as `tokenKinds`) plus the
keyword/operator/builtin lists verbatim. Code-generation agents consume this file
to tokenize Kirn without re-implementing the lexer, guaranteeing that agent
tokenization and the LSP semantic-token types agree.

> Reference: Ruby — `prism/internal/tokens.h` (token list as a contract); CPython — `Lib/token.py` + `Lib/tokenize.py` (generated token names).

### 2.3 Staying synchronized with language changes (critical process)

**Goal:** keep generated definitions from drifting from the language.
**Must Follow:** extraction + generation + golden tests in CI; any stale generated file fails the build.
**Important Note:** a new keyword/operator in the lexer must re-highlight every editor on regeneration only.

Because the compiler is under active change, definitions **must not drift**.
Adopt a **generation + golden-test** flow wired into CI:

- A small generator (initially a PowerShell/JScript or Node script in
  `editor/tools`) parses:
  - the keyword list from `src/lex/lexer.cpp` (`isKeyword` array),
  - the operator lists (`kOps3/kOps2/kOp1/kPunct1`),
  - the standard-library module/function names from `stdlib/lib/*.kn`
    (`pub fn` + module files),
  - (optional) productions from `grammar/kirn.ebnf`.
- It regenerates `editor/kirn-syntax/spec/kirn.json`, then each downstream
  artifact (Â§4 generators). Any diff in the _generated_ files against the
  committed copies fails CI, so a language change that forgets to update editor
  support is caught automatically.
- Adding a **new keyword** to the lexer automatically re-highlights it in every
  editor on the next regeneration â€” no manual per-editor edits.

**Generated token-kind contract (the agent-facing list):**

```json
{
  "tokenKinds": [
    "Eof",
    "Newline",
    "Indent",
    "Dedent",
    "Ident",
    "Int",
    "Float",
    "Char",
    "StrNormal",
    "StrRaw",
    "StrByte",
    "StrC",
    "FStrStart",
    "FStrText",
    "FStrLBrace",
    "FStrRBrace",
    "FStrColon",
    "FStrSpec",
    "FStrEnd",
    "Op",
    "Punct"
  ]
}
```

> Reference: CPython — `Grammar/python.gram` + `Tools/peg_generator` (one grammar → generated parser); Rust — `src/etc/generate-keyword-tests.py` (keyword list drives generated tests).

---

## 3. Technology selection per editor (rationale)

**Goal:** choose the right mechanism per editor (tree-sitter vs regex vs LSP).
**Must Follow:** structural editors use tree-sitter; regex editors use the shared TextMate grammar; all optionally use `kirn-lsp`.
**Important Note:** mechanisms differ, scopes must not — §6 is the shared contract.

> Reference: Go — `misc/editors` (editor grammars deliberately kept out of tree).

| Target             | Primary mechanism                                  | Why                                                                                          | Also                                                                                       |
| ------------------ | -------------------------------------------------- | -------------------------------------------------------------------------------------------- | ------------------------------------------------------------------------------------------ |
| **VS Code**        | TextMate grammar + LSP (when available)            | VS Code tokenizer is TextMate; richest extension host                                        | semantic highlighting from LSP; `language-configuration.json` for brackets/comments/indent |
| **Vim**            | `syntax/kirn.vim` regex highlighter                | Vim 8+/9 does not ship tree-sitter; regex syntax is standard, zero-dependency                | `ftdetect`, `indent/kirn.vim`, optional `coc.vim`/`vim-lsp` for LSP                        |
| **Neovim**         | `tree-sitter-kirn` via `nvim-treesitter`           | Neovim >=0.5 has native tree-sitter; gives highlighting, indent, folding, text objects       | `nvim-lspconfig` for `kirn-lsp`                                                            |
| **Micro**          | `.yaml` syntax file (regex)                        | Micro's `micro-syntax` is YAML-based TextMate-influenced regex                               | bracket/pair config in `settings.json`                                                     |
| **Zed**            | `tree-sitter-kirn` extension                       | Zed language extensions are tree-sitter grammars + `languages/<lang>/config.toml`            | LSP adapter for `kirn-lsp`                                                                 |
| **Highlight.js**   | custom language definition (built-in or CDN build) | CommonMark, docs sites, static site generators render code                                   | used by the planned VitePress docs site                                                    |
| **JetBrains IDEs** | TextMate bundle initially, then Grammar-Kit plugin | Can add `.tmLanguage` with zero code; Grammar-Kit gives parser/PSI + full IDE features later | Grammar-Kit `.bnf` (parser) + `.flex` (lexer) from our grammar                             |
| **GitHub**         | Linguist + `tree-sitter-kirn`                      | `.kn` should be recognized on GitHub; Linguist supports tree-sitter grammars                 | adds repo language stats + highlighting                                                    |
| **Helix**          | `tree-sitter-kirn`                                 | Helix is tree-sitter-first + LSP                                                             | uses the same grammar as Neovim/Zed                                                        |
| **Sublime**        | TextMate bundle (`.sublime-syntax`)                | reuse the shared TextMate grammar                                                            | also covers `source.kirn`                                                                  |

**Principle:** _structurally-aware editors (Zed, Neovim, Helix, GitHub, and
eventually VS Code) use tree-sitter; regex-based editors (Vim, Micro, VS Code
default, JetBrains initial, highlight.js, Sublime) use the shared TextMate
grammar; all of them optionally connect to `kirn-lsp` for semantic features._
This follows the industry consensus (2026): tree-sitter for fast structural
feedback, LSP for semantic intelligence â€” complementary, not competing.

**Scope-mapping sample (same construct, one scope for all regex editors):**

```jsonc
{ "match": "\\b(if|elif|else|while|for|match|case)\\b", "name": "keyword.control.kirn" }
```

---

## 4. Shared generators (the "one definition â†’ many artifacts" pipeline)

**Goal:** generate every artifact from `spec/kirn.json` in one step.
**Must Follow:** zero optional dependencies; emit LF; each generator small and idempotent; never fork keyword lists per editor.
**Important Note:** generated files are committed and CI-checked for staleness.

A single `editor/tools/generate` step produces or converts:

- `grammars/kirn.tmLanguage.json` from `spec/kirn.json` (TextMate regex rules).
- `syntax/kirn.vim` from `spec/kirn.json` (Vim `syn keyword/region`).
- `syntax/kirn.yaml` (Micro) from `spec/kirn.json`.
- `grammars/kirn.sublime-syntax` (Sublime / JetBrains tmLanguage) from the
  TextMate JSON.
- A VS Code `language-configuration.json` (bracket pairs, auto-closing pairs,
  comments, folding markers) generated from `spec/kirn.json`.
- `highlightjs: module.exports = ...` (highlight.js definition) from the TextMate
  grammar / spec.
- `builtins.json` for the LSP (from `stdlib/lib/*.kn`).

Golden-test fixtures in `editor/test/corpus/*.kn` each carry an
"expected scopes/keywords" annotation; the runner checks every platform
generator highlights them identically, catching drift.

**Generated-TextMate-rule sample (`grammars/kirn.tmLanguage.json`):**

```jsonc
{
  "keyword": {
    "patterns": [
      { "match": "\\b(fn|var)\\b", "name": "storage.type.function.kirn" },
      { "match": "\\b(if|elif|else|while|for|match|case)\\b", "name": "keyword.control.kirn" }
    ]
  }
}
```

---

## 5. Phased roadmap

**Goal:** sequence phases so value ships early and complexity later.
**Must Follow:** every phase is self-contained and shippable; no phase blocks the shared spec.
**Important Note:** effort estimates (S/M/L) are indicative, not commitments.

Phases are ordered to deliver value fast (a working highlighter on the most
common platforms) before investing in slow, complex integrations (LSP, full
JetBrains plugin). Each phase is self-contained and independently shippable.

Phase summary:

| #   | Phase                                         | Editors/tooling                           | Effort |
| --- | --------------------------------------------- | ----------------------------------------- | ------ |
| 0   | Foundations + repo skeleton + generators      | all                                       | M      |
| 1   | VS Code extension (TextMate first)            | VS Code                                   | S      |
| 2   | Vim + Neovim (regex + treesitter)             | Vim, Neovim                               | Sâ€“M  |
| 3   | tree-sitter grammar                           | Neovim, Zed, Helix, GitHub, VS Code later | L      |
| 4   | Zed + Helix extensions                        | Zed, Helix                                | S      |
| 5   | Micro + highlight.js                          | Micro, highlight.js (docs)                | S      |
| 6   | JetBrains (TextMate bundle, then Grammar-Kit) | IntelliJ/Rider/GoLand/â€¦                 | Mâ€“L  |
| 7   | Language Server `kirn-lsp`                    | all editors                               | L      |
| 8   | Debugging support & advanced features         | VS Code DAP, JetBrains                    | M      |
| 9   | Release, publishing, docs, e2e validation     | all                                       | M      |

---

### Phase 0 â€” Foundations, shared spec, generator skeleton

**Goal:** stand up `editor/kirn-syntax` as the single source of truth and get
the generation pipeline working end-to-end, so every later phase just consumes
it.

**Must Follow:** generate from the lexer/grammar only; emit `kirn.json` + artifacts in one pass; ship no extra binary.
**Important Note:** this phase is the whole plan's dependency — keep the generator deliberately simple.

**Target editor/tool:** repository infrastructure (no user-facing output yet).

**Required files:**

- `editor/kirn-syntax/spec/kirn.json` (hand-seeded from the facts in Â§1).
- `editor/kirn-syntax/package.json`.
- `editor/tools/extract.ps1` (reads lexer.cpp keyword/operator + stdlib defs).
- `editor/tools/generate.mjs` (emits all artifact skeletons).
- `editor/test/corpus/*.kn` (a first corpus: `hello.kn`, `fstring.kn`,
  `match_patterns.kn`, `structs_enums.kn`, `try_catch.kn`, `oop.kn`,
  `stdlib_import.kn`).
- `editor/test/run.mjs` (golden-token comparison runner).

**Implementation approach:** write a generator that, given `spec/kirn.json`,
emits every highlighter skeleton in zero-external-dependency format (Node
built-ins + PowerShell). Add a CI job (see Â§12) that fails on stale generated
files.

**KIRN features covered:** token classes only at this phase (keywords,
operators, numbers, chars, all string forms including f-strings, `#` comments,
identifiers).

**Code example (corpus fixture, `editor/test/corpus/fstring.kn`):**

```kirn
# interpolated string: FStr{...}
fn greet(name: Str, score: Int) -> Str {
    return f"Hello {name}, score={score:04}";   # f-string + format spec
}
```

Expected tokens: `fn`/`string`/`return`/`f"Hello ` etc.; the fixture asserts
the `{name}` and `{score:04}` interpolation regions highlight as embedded
expressions, not plain string text.

**Testing/validation:** `node editor/test/run.mjs` parses each `.kn` corpus file
with every generator that exists and asserts the golden token map. No editor
needed.

**Expected result:** `editor/kirn-syntax/spec/kirn.json` is stable, generators
run, tests pass, and CI enforces freshness.

**Potential limitations:** the generator must be kept simple; full grammar
parsing of `kirn.ebnf` is out of scope at this phase (keyword/operator
extraction only).

---

### Phase 1 â€” VS Code extension (TextMate first)

**Goal:** a VS Code extension `kirn-vscode` providing syntax highlighting,
comments, bracket matching, auto-indent, folding of braces, and (later)
semantic highlighting + diagnostics.

**Must Follow:** contribute scope `source.kirn` with `.kn`; TextMate rules only for grammar-defined tokens; no user-identifier rules.
**Important Note:** TextMate f-string handling is approximate; tree-sitter/LSP fix correctness later.

**Target editor/tool:** Visual Studio Code.

**Required files:**

```
editor/vscode/
  package.json            # language contribution, activation, commands
  language-configuration.json   # comments, brackets, auto-close, folding
  syntaxes/kirn.tmLanguage.json # generated from spec/kirn.json
  client/  server/        # later: LSP client
  test/                   # extension tests
  CHANGELOG.md
```

**Implementation approach:**

- Contribute the language: `contributes.languages[].id = "kirn"`,
  `extensions = [".kn"]`, `aliases = ["Kirn","kirn"]`,
  `configuration = "./language-configuration.json"`.
- Contribute the grammar: `contributes.grammars[] = {
language:"kirn", scopeName:"source.kirn", path:"./syntaxes/kirn.tmLanguage.json" }`.
- `language-configuration.json`:
  - `comments.lineComment = "#"`, no block comment.
  - bracket pairs: `( )`, `[ ]`, `{ }`; auto-closing pairs likewise.
  - `surroundingPairs`: `("", ""), ((), ()), ([], []), ({}, {})`.
  - `folding.markers`: use `{`/`}` regionStart/regionEnd via indentation-based
    folding plus brace matching.
- TextMate grammar must handle (a) `#` comments, (b) all string forms with the
  f-string interpolation as an embedded expression region, (c) keyword and
  type scopes, (d) numeric/char literals, (e) operator list.
- Use the **Scope Inspector** (`Developer: Inspect Editor Tokens and Scopes`)
  to verify scopes on a corpus file.

**KIRN syntax/features covered:** full tokenization; later phases add semantic
highlighting (mutable vs immutable bindings, unresolved names) via `kirn-lsp`.

**Code example (`syntaxes/kirn.tmLanguage.json`, partial):**

```jsonc
{
  "name": "Kirn",
  "scopeName": "source.kirn",
  "patterns": [
    { "include": "#comment-line" },
    { "include": "#fstring" },
    { "include": "#string" },
    { "include": "#number" },
    { "include": "#keyword" },
    { "include": "#identifier" }
  ],
  "repository": {
    "comment-line": {
      "match": "(#).*$",
      "captures": { "1": { "name": "punctuation.definition.comment.kirn" } },
      "name": "comment.line.number-sign.kirn"
    },
    "keyword": {
      "patterns": [
        {
          "match": "\\b(fn|fn|struct|enum|trait|impl|match|case|try|raise|catch)\\b",
          "name": "keyword.control.kirn"
        },
        {
          "match": "\\b(var|and|or|not|is|as|in|new|defer|spawn|chan|select)\\b",
          "name": "keyword.operator.kirn"
        },
        { "match": "\\b(true|false|none|None|self|Self)\\b", "name": "constant.language.kirn" }
      ]
    },
    "fstring": {
      "begin": "f\"",
      "beginCaptures": { "0": { "name": "punctuation.definition.string.begin.kirn" } },
      "end": "\"",
      "name": "string.quoted.double.kirn",
      "patterns": [
        {
          "begin": "\\{",
          "end": "\\}",
          "name": "meta.embedded.expression.kirn",
          "patterns": [{ "include": "source.kirn" }]
        }
      ]
    }
  }
}
```

**Testing/validation:** `@vscode/test-electron` runs the extension in a real
VS Code instance, opens each corpus `.kn`, and asserts token scopes; plus
`vscode-tmlanguage`-style scope snapshots in CI.
**Package:** `vsce package` produces `kirn-vscode-<ver>.vsix`; publish to the
Marketplace (`vsce publish`) and Open VSX (`ovsx publish`).

**Expected result:** `.kn` files open with correct colors, `#` comments toggle
with a comment shortcut, braces auto-close, block folding works.

**Potential limitations:** TextMate cannot truly parse the context-sensitive
f-string format specs or nested interpolation; the regex approach is
approximate but visually correct. Tree-sitter (Phase 3) later replaces the
tokenizer for verbatim correctness and schema-aware features.

---

### Phase 2 â€” Vim and Neovim

**Goal:** working Kirn highlighting in classic Vim (regex), and in Neovim use
tree-sitter (Phase 3) plus an optional LSP (Phase 7).

**Must Follow:** generate `syntax/kirn.vim` from the shared spec; link to standard groups; no hand-written keyword lists.
**Important Note:** Vim regex cannot nest f-string braces; acceptable for color, not correctness.

**Target editor/tool:** Vim â‰¥ 8, Neovim â‰¥ 0.5.

**Required files (Vim):**

- `editor/vim/ftdetect/kirn.vim` â€” `au BufRead,BufNewFile *.kn setf kirn`.
- `editor/vim/syntax/kirn.vim` â€” generated `syn` rules.
- `editor/vim/indent/kirn.vim` â€” brace-based indent.
- `editor/vim/ftplugin/kirn.vim` â€” `setlocal commentstring=# %s`, folding, etc.
- `editor/vim/README.md` â€” install via vim-plug / pathogen / manual.

**Implementation approach (Vim):**

**Goal:** color keywords/strings/comments via generated `syn` rules.
**Must Follow:** groups link to `Statement`/`String`/`Comment`; generated from the shared spec.
**Important Note:** f-string braces are approximate; no user-identifier scopes.

> Reference: V — `editors/vim/syntax/v.vim` (+ `ftdetect/v.vim`); LLVM — `llvm/utils/vim/syntax/llvm.vim` and `llvm/utils/emacs/llvm-mode.el`.

- Generate `syn keyword kirnKeyword fn var ...` with different groups
  (`kirnKeywordControl`, `kirnKeywordOperator`, `kirnConstant`).
- `syn region kirnString start=+"+ end=+"+` plus raw/byte/c- variants.
- `syn region kirnFString start=+f"+ end=+"+ contains=kirnInterp` with
  `kirnInterp` = `{...}` region.
- `syn region kirnComment start=+#+ end=+$+`.
- `syn match kirnNumber '\<0[xX][0-9a-fA-F_]\+\|\<[0-9][0-9_]*\%(\.[0-9_]*\)\?\%([eE][+-]\?[0-9]\+\)\?'`.
- link groups to `Statement`, `String`, `Comment`, `Number`, `Type`, etc. so
  any colorscheme works.
- `indent` uses `shiftwidth` keyed on `{`/`}` balance (grammar has no
  significant indentation, but blocks are `{ }`).

**Implementation approach (Neovim):**

**Goal:** tree-sitter highlighting + folding via the Phase 3 queries.
**Must Follow:** register filetype `kn`; reuse `queries/highlights.scm`; LSP optional.
**Important Note:** prefer tree-sitter over regex on Neovim.

- Register `tree-sitter-kirn` (Phase 3) in `nvim-treesitter` via a small
  `queries/highlights.scm` (already provided by the grammar package).
- `lua/kirn/plugin.lua` OR simple README instructions:
  ```lua
  -- after nvim-treesitter is installed and the grammar built:
  vim.filetype.add({ extension = { co = "kirn" } })
  vim.treesitter.language.register("kirn", "kirn")
  ```
- Later: `nvim-lspconfig` config alias `kirn_lsp` once `kirn-lsp` exists.

**KIRN features covered:** tokenization + (Neovim) structural highlighting and
folding via tree-sitter; `#` comments; all string forms; f-string interpolation.

**Vim/Neovim sample (`vim_demo.kn`):**

```kirn
# comment toggles with '#'; braces drive indent
fn classify(n: Int) -> Str {
    if n < 0 {
        return f"neg {n}"
    }
    return "nonneg"
}
```

**Testing/validation:** Vim `syntax` snapshot via `:syn-sync` + `g:loaded_syntax`
manual check; golden output compared. For Neovim, run the tree-sitter parser
over the corpus and compare the CST/highlight against expected queries.

**Expected result:** opening `x.kn` in Vim colors keywords/strings/comments;
Neovim additionally gives tree-sitter folding and text objects.

**Potential limitations:** Vim regex cannot handle nested f-string braces
perfectly; acceptable for highlighting. Tree-sitter (Phase 3) is the
recommended path on Neovim.

---

### Phase 3 â€” `tree-sitter-kirn` grammar (the modern core)

**Goal:** a compliant tree-sitter grammar for Kirn plus queries for
highlighting, indentation, and folding that power Neovim, Zed, Helix, GitHub,
and later VS Code.

**Must Follow:** author `grammar.js` from `grammar/kirn.ebnf`; queries map to §6 scopes; rosetta-parse the whole repo.
**Important Note:** this is the largest phase; precedence and f-string sub-expressions are the hard parts.

> Reference: LLVM/MLIR — `mlir/utils/tree-sitter-mlir/grammar.js` + `queries/highlights.scm`.

**Target editor/tool:** shared by Zed, Neovim, Helix, GitHub, and (via adapter)
others.

**Required files:**

```
editor/tree-sitter-kirn/
  grammar.js             # the tree-sitter DSL grammar
  package.json
  bindings/node/ bindings/rust/ bindings/swift/   # generated bindings
  src/parser.c           # generated C parser (committed or built)
  queries/
    highlights.scm
    indents.scm
    folds.scm
    locals.scm           # scoping for text objects / structure
  test/corpus/*.txt      # tree-sitter's own corpus tests (inline snapshots)
  LICENSE, README.md
```

**Implementation approach:**

- Author `grammar.js` faithfully from `grammar/kirn.ebnf` (v0.2) plus the
  lexer facts. Key productions to model:
  - statements: `simple_stmt` (`;`-terminated), compound `if/elif/else`,
    `while`, `for â€¦ in`, `match/case`, `try/catch`, `defer`, `spawn`,
    `select`, `gather`, `struct/enum/trait/impl/class/interface/record`.
  - expressions: full precedence chain from `grammar/kirn.ebnf` Â§2 â€” including
    `..` / `..=` ranges, `?.` optional chaining, `??` (if/when introduced),
    `try` as prefix expression, `match` as expression, lambda `(x) => expr`,
    groups, indexed/attr/slice access, `call`.
  - patterns: `case` patterns â€” bindings, literals, `constants`, tuples,
    slices with `..`, guards, `|` alternation, `@` alias, `&` ref pattern.
  - literals: all number forms, char (incl. `\u{...}`), all string forms, and
    **f-strings** as `FString` nodes containing an interleaved `expression`
    sub-tree.
  - tokens/lexer rules must reproduce keyword/operator longest-match behavior.
- This is the **largest and most effortful** phase; precedence and
  left-associativity for arithmetic/call chains are the hard parts (tree-sitter
  `precedence`, `prec.left`, `assoc`).
- Use `tree-sitter test` (inline corpus snapshots) and `tree-sitter parse
--debug` to iterate.

**KIRN features covered:** everything â€” this becomes the reference structural
parser for the whole ecosystem.

**Code example (`grammar.js`, fragment):**

```javascript
module.exports = grammar({
  name: 'kirn',
  extras: $ => [ /[ \t\r\n]+/, $.line_continuation, $.comment ],
  keywords: [ 'fn','var','if','elif','else','while','for','in',
              'return','break','continue','match','case','struct','enum',
              'trait','impl','import','export','pub','defer','spawn','chan',
              'select','try','raise','catch','unsafe','extern','new','box',
              'self','Self','and','or','not','is','as','true','false','none',
              'pass','class','interface','record','implements','extends',
              'dynamic','None','del','pr','local','global','temp','bucket' ],
  rules: {
    source_file: $ => repeat($.statement),

    statement: $ => choice(
      $.simple_stmt, $.if_stmt, $.while_stmt, $.for_stmt, $.match_stmt,
      $.try_stmt, $.spawn_stmt, $.select_stmt, $.gather_stmt, ...),

    // if c { } elif c { } else { }
    if_stmt: $ => seq(
      'if', field('cond', $.expr), field('then', $.block),
      repeat(seq('elif', field('cond', $.expr), field('then', $.block))),
      optional(seq('else', field('else', $.block))),
      ';'),

    block: $ => seq('{', repeat($.statement), '}'),

    // f-string with embedded expression
    fstring: $ => seq('f"', repeat(choice($._fstring_text, $.interpolation)), '"'),
    interpolation: $ => seq('{', $.expr, optional(seq(':', $.format_spec)), '}'),
    ...
  }
});
```

**Testing/validation:** tree-sitter corpus tests (named snippets with expected
parse trees) plus rosetta validation: parse **every file under `examples/`,
`stdlib/lib/*.kn`, `tests/`** with `kirnparse` and with
tree-sitter, and byte-compare the **structure** (tree-sitter has no parse
errors where `kirnparse` succeeds). This is the single strongest correctness
check.

**Expected result:** a grammar that parses the whole real corpus with zero
errors, with queries giving correct highlighting/indent/folds.

**Potential limitations:** tree-sitter DSL divergence from hand-written recursive
descent (e.g. operator precedence is expressed via `precedence` declarations,
not code); f-string embedded sub-expressions need careful lexer/extra setup and
may require `token.immediate` handling.

---

### Phase 4 â€” Zed and Helix extensions

**Goal:** make Kirn a first-class language in Zed and Helix by packaging the
Phase 3 grammar plus (Phase 7) LSP adapters.

**Must Follow:** reuse the Phase 3 queries; register `source.kirn`, `.kn`, `comment-tokens = ["#"]`.
**Important Note:** editor tree-sitter runtime versions vary; pin and test them.

> Reference: Rust — `src/etc/rust_analyzer_zed.json` + `rust_analyzer_helix.toml` (official editor configs shipped in-tree).

**Target editor/tool:** Zed, Helix.

**Required files (Zed):**

**Goal:** package the Phase 3 grammar as a Zed extension.
**Must Follow:** reuse `highlights.scm`/`indents.scm`/`folds.scm`; register `source.kirn`, `.kn`, `line_comments = ["#"]`.
**Important Note:** Zed's tree-sitter runtime may lag newest grammar features.

```
editor/zed/kirn.json          # config used by "zed: install extension"
editor/zed/languages/kirn/config.toml
editor/zed/languages/kirn/highlights.scm
editor/zed/languages/kirn/indents.scm
editor/zed/languages/kirn/folds.scm
editor/zed/languages/kirn/outline.scm
editor/zed/extensions.toml
```

`config.toml` (fragment):

```toml
name = "Kirn"
grammar = "kirn"
path_suffixes = ["co"]
line_comments = ["#"]
brackets = [
  { start = "{", end = "}", close = true, newline = true },
  { start = "[", end = "]", close = true },
  { start = "(", end = ")", close = true },
]
scope_override = { "variable" = "..." }
```

**For Helix:**

**Goal:** register the same grammar + queries in `languages.toml`.
**Must Follow:** reuse the Phase 3 queries; `comment-tokens = ["#"]`; optional LSP.
**Important Note:** config/paths differ from Zed, but scopes must not.

- `languages/highlights.scm`, `indents.scm`, `textobjects.scm`, `folds.scm`
  â€” reuse the same tree-sitter queries.
- `languages.toml`: register `name="kirn"`, `scope="source.kirn"`,
  `file-types=["co"]`, `comment-tokens=["#"]`, `language-server={...}` for LSP.

**Implementation approach:** mostly config + reuse of Phase 3 queries. Zed
extensions are zip files with a `languages/` layout; Helix reads language
config + queries from `$XDG_CONFIG_HOME/helix` or the runtime directory.

**KIRN features covered:** highlighting, indentation, folding, structural
selections, LSP integration (diagnostics/completion/hover) once `kirn-lsp`
ships.

**Code example (Zed `folds.scm`):**

```
[
  (if_stmt) (while_stmt) (for_stmt) (match_stmt) (try_stmt)
  (struct_def) (enum_def) (trait_def) (impl_def)
] @fold
```

**Testing/validation:** install locally via `zed: install extension` and Helix
`runtime` path; open corpus and verify no parse errors, expected colors/folds.

**Expected result:** consistent Kirn editing in both modern terminal-native
editors with mutating, incremental highlighting.

**Potential limitations:** version pinning of the editor's tree-sitter runtime;
Zed/Helix occasionally lag on newest tree-sitter features.

---

### Phase 5 â€” Micro and highlight.js

**Goal:** Kirn support in the Micro terminal editor and on the Web via
highlight.js (used by the VitePress docs and any static site).

**Must Follow:** generate `kirn.yaml` and `kirn.js` from the shared spec; PascalCase type heuristic only for known types, never for user identifiers.
**Important Note:** regex highlighters are display-only approximations.

**Target editor/tool:** Micro, highlight.js (and the docs site in
`KIRN_DOCS_PLAN.md`).

**Required files:**

- `editor/micro/syntax/kirn.yaml` (regex-based, Micro `micro-syntax`).
- `editor/micro/settings.json` fragment (comment `#`, brackets, tab).
- `editor/highlightjs/kirn.js` (a CommonJS/ESM language definition).

**Implementation approach (Micro `kirn.yaml`):**

**Goal:** regex highlighting from `kirn.yaml` generated by the shared spec.
**Must Follow:** keep rules minimal; no user-identifier matching; `#` comment + bracket pairs.
**Important Note:** display-only; no semantic guarantees.

```yaml
filetype: kirn
detect:
  filename: "\\.kn$"
rules:
  - comment: "#.*$"
  - statement:
      - keyword: "fn|var|if|elif|else|while|for|in|return|break|continue|match|case|struct|enum|trait|impl|import|export|pub|defer|spawn|chan|select|try|raise|catch|unsafe|extern|new|box|self|Self|and|or|not|is|as|pass|class|interface|record|implements|extends|dynamic|del|pr|local|global|temp|bucket"
  - constant:
      - "true|false|none|None"
  - type: "string|int|float|bool|char|list|dict|tuple|option|result|byte|any|void|...|Point|[A-Z][A-Za-z0-9_]*"
  - string: '".*?"'
  - number: "\\b(0[xX][0-9a-fA-F_]+|0[oO][0-7_]+|0[bB][01_]+|\\d+[\\d_]*(\\.[\\d_]*)?([eE][+-]?\\d+)?)\\b"
```

Add bracket/comment config in `settings.json`:

```json
{
  "comment": "#",
  "matchPairs": { "{": "}", "(": ")", "[": "]" },
  "autoPairs": { "{": "}", "(": ")", "[": "]", "\"": "\"" }
}
```

**Implementation approach (highlight.js `kirn.js`):**

**Goal:** Web/docs rendering from `kirn.js` generated by the shared spec.
**Must Follow:** PascalCase `type` rule only for known types; never user identifiers.
**Important Note:** submit upstream only after golden fixtures pass.
Reuse `spec/kirn.json` to build a definition:

```javascript
module.exports = function (hljs) {
  const KEY = [
    "fn",
    "var",
    "if",
    "elif",
    "else",
    "while",
    "for",
    "in",
    "return",
    "break",
    "continue",
    "match",
    "case",
    "struct",
    "enum",
    "trait",
    "impl",
    "import",
    "export",
    "pub",
    "defer",
    "spawn",
    "chan",
    "select",
    "try",
    "raise",
    "catch",
    "unsafe",
    "extern",
    "new",
    "box",
    "self",
    "Self",
    "and",
    "or",
    "not",
    "is",
    "as",
    "true",
    "false",
    "none",
    "pass",
    "class",
    "interface",
    "record",
    "implements",
    "extends",
    "dynamic",
    "None",
    "del",
    "pr",
    "local",
    "global",
    "temp",
    "bucket"
  ];
  return {
    name: "Kirn",
    aliases: ["kirn", "co"],
    case_insensitive: false,
    keywords: {
      keyword: KEY,
      literal: ["true", "false", "none", "None"],
      built_in: ["print", "import", "export"]
    },
    contains: [
      hljs.HASH_COMMENT_MODE, // '#' comments
      hljs.QUOTE_STRING_MODE,
      { className: "string", begin: /f"/, ...interpolation },
      hljs.C_NUMBER_MODE,
      { className: "number", begin: /0[xX][0-9a-fA-F_]+/ },
      { className: "type", begin: /\b[A-Z][A-Za-z0-9_]*\b/ }
    ]
  };
};
```

Optionally submit upstream as `highlight.js/src/languages/kirn.js` so that
`highlight.js` CDN builds include it (great for the docs site and GitHub-flavored
markdown on many platforms).

**KIRN features covered:** tokenization; `#` comments; strings incl. f-strings
via a small embedded-expression regex; keywords; type-name heuristics
(PascalCase).

**Testing/validation (highlight.js):** use the official
`highlight.js/test/detect` + markup-fixture tests; assert a corpus renders with
the expected `<span class="hljs-...">` classes (DOM/cheerio snapshot).

**Expected result:** `kirn` used as a fencelang in markdown
` ```kirn ` on the docs site colors correctly; Micro opens `.kn` colored.

**Potential limitations:** highlight.js and Micro are regex-based; f-string
interpolation and multi-line constructs are approximations only. Fine for
display.

---

### Phase 6 â€” JetBrains IDEs

**Goal:** Kirn support in IntelliJ-based IDEs (IDEA, PyCharm, GoLand, Rider,
CLion, WebStorm), starting with a zero-code TextMate bundle and graduating to a
Grammar-Kit plugin.

**Must Follow:** Stage A reuses the shared TextMate grammar; Stage B derives `Kirn.bnf`/lexer from `grammar/kirn.ebnf` + the lexer.
**Important Note:** no native JNI bridge; LSP is the intended long-term route.

**Target editor/tool:** JetBrains IntelliJ Platform IDEs (2022.3+, Java 17).

**Required files (TextMate stage):**

- `editor/jetbrains/kirn.tmLanguage` (XML plist, or reuse the shared JSON) plus
  a bundle `syntaxes/` folder; installed via
  `Settings â†’ Editor â†’ TextMate Bundles`.
- `editor/jetbrains/kirn.dic` (for spell-check dictionary; optional).

**Required files (Grammar-Kit stage):**

```
editor/jetbrains-kirn-plugin/
  build.gradle.kts
  src/main/grammar/Kirn.bnf     # Grammar-Kit PEG grammar (from kirn.ebnf)
  src/main/grammar/KirnLexer.flex  # JFlex lexer (from lexer.cpp)
  src/main/gen/...              # generated parser + PSI
  src/main/java/...             # ParserDefinition, Annotator, Completion, etc.
  src/main/resources/META-INF/plugin.xml
```

**Implementation approach:**

- **Stage A (days):** ship a TextMate bundle. Uses `src/grammar` scopes and
  gives immediate highlighting with zero Java code.
  - **Goal:** day-one highlighting with zero Java.
  - **Must Follow:** reuse the shared TextMate scopes; no forked keyword list.
  - **Important Note:** temporary until the Grammar-Kit plugin lands.
- **Stage B (weeks):** Grammar-Kit plugin.
  - **Goal:** full PSI/IDE features via Grammar-Kit.
  - **Must Follow:** derive `.bnf`/lexer from `grammar/kirn.ebnf` + the lexer; no JNI.
  - **Important Note:** generated PSI may need in-IDE generation.
  - Author `Kirn.bnf` mapping the canonical `kirn.ebnf` productions to
    Grammar-Kit PEG (choice `|`, optional `[ ]`, repetition `*` â€” Grammar-Kit
    accepts left-recursion in many cases but prefer iterative rules).
  - Write `KirnLexer.flex` matching the C++ lexer token classes and keywords
    (JFlex longest-match, ignores `#` comments and inserts whitespace).
  - Generate parser + PSI (`Ctrl+Shift+G`), add `ParserDefinition`,
    `SyntaxHighlighter`, `BraceMatcher`, `QuoteHandler`, `CodeStyleSettings`
    (auto-indent on `{`...`}`), `Annotator` (syntax errors), `CompletionContributor`
    (stdlib `pub fn` names), `GotoDeclarationHandler` (go-to-fn), and
    `FindUsagesProvider`.
  - Optionally reuse the **pre-existing C++** lexer/parser logic conceptually;
    do _not_ try to JNI-bridge yet (see Phase 7 note about an LSP-based approach
    that avoids a native bridge entirely).

**KIRN features covered:** full PSI/IDE experience â€” highlighting, bracket
matching, auto-indent, folding, code completion of stdlib, go-to-definition,
find usages, refactoring-safe structure view.

**Code example (`Kirn.bnf` fragment):**

```bnf
{
  parserClass="kirn.lang.parser.KirnParser"
  extends="com.intellij.extapi.psi.ASTWrapperPsiElement"
  psiPackage="kirn.lang.psi"
  elementTypeHolderClass="kirn.lang.psi.KirnTypes"
  tokenTypeClass="kirn.lang.psi.KirnTokenType"
  psiImplUtilClass="kirn.lang.psi.impl.KirnPsiImplUtil"
  tokens = [
    comment='regexp:#[^\r\n]*'
    STRING='regexp:"[^"\n]*"'
    ...
    DEF='fn'  FN='fn'  IF='if'  ...
  ]
}

source_file ::= item_*   { recoverWhile="..."; pin=1 }
item_ ::= function | struct_ | enum_ | trait_ | impl_ | import_ | statement | COMMENT

function ::= (DEF|FN) IDENTIFIER param_list ':' type block    { mixin="..." }
fact ::= (DEF|FN) IDENTIFIER param_list ':' type block
```

**Testing/validation:** Gradle `test` with **plugin tests** using the IntelliJ
Platform test framework; run `./gradlew verifyPlugin`. Use PsiViewer to inspect
PSI on corpus files; assert no redeclaration errors and that every
`examples/*.kn` parses to a full tree.

**Publishing:** JetBrains **Marketplace** via `./gradlew publishPlugin`
(needs Marketplace token). Also distribute the TextMate bundle independently
so non-IntelliJ helpers benefit.

**Expected result:** full-featured Kirn editing inside JetBrains IDEs without
any native code.

**Potential limitations:** Grammar-Kit requires Java 17 + recent IDEA; the
`mixin`/`psiImplUtilClass` limitations in the Gradle Grammar-Kit plugin mean
generated PSI may need in-IDE generation. This is the highest-effort per-editor
phase.

---

### Phase 7 â€” Language Server `kirn-lsp`

**Goal:** a Language Server Protocol server giving cross-editor semantic
features: diagnostics (syntax + type), hover, completion, go-to-definition,
reference finding, rename, formatting, document symbols, and **semantic
highlighting**.

**Must Follow:** reuse `src/lex`, `src/parser`, `src/sema`; emit LSP semantic tokens from the shared `tokenKinds`; no codegen.
**Important Note:** semantic tokens and agent tokenization must come from the same generated contract.

> Reference: Rust — rust-analyzer `crates/rust-analyzer/src/lsp/semantic_tokens.rs`; Nim — `nimsuggest/nimsuggest.nim`.

**Target editor/tool:** all editors that speak LSP (VS Code via client, Neovim
via nvim-lspconfig, Zed, Helix, JetBrains via the LSP plugin or direct,
Emacs/Eglot, etc.).

**Please read this and the source before starting this phase** â€” it is a
research-heavy task â€” but importantly the pieces already exist:

- `src/lex/lexer.cpp`: tokenizer (with line/col), including
  the f-string sub-tokens needed for accurate semantic tokens.
- `src/parser/parser.cpp`: parser emitting
  `file:line:col: error:` diagnostics â€” exactly the LSP `Diagnostic` shape.
- `src/sema/checker.cpp` + `symbols.h` + `type.h`: type checking, symbol
  resolution, borrow checking â€” the semantic layer for hover/definition/
  references and semantic tokens.
- `src/vm/compiler.cpp`, `src/interp/runtime.*`, `src/backend/native.*`:
  not needed by an LSP (skip codegen entirely).

**Recommended server architecture:** native server in C++ wrapping
`Lexer`/`Parser`/`Checker` directly, speaking JSON-RPC over stdio (implementing
the LSP wire protocol in C++). Pros: uses the exact production front end, no
drift. Cons: must implement JSON-RPC and a small LSP layer in C++.

The server uses the _module + workspace_ model:

- Parse + type-check a workspace; cache per-file ASTs; incremental re-parse on
  change (tree-sitter or the Kirn parser), and re-run the checker for the
  affected file + dependents.
- Publish `textDocument/publishDiagnostics` for:
  - lexical errors (unterminated strings/chars, bad numeric literals),
  - parse errors,
  - semantic errors (undefined variable/field/function, type mismatch,
    immutable reassignment, exhaustiveness, borrow violations) â€” reusing the
    existing `tests/negative/n*.kn` expectations as acceptance fixtures.
- `textDocument/semanticTokens` with token types: `variable` (with modifiers
  `readonly`/`mutable`), `function`, `type` (struct/enum/trait/class/interface/
  record), `keyword`, `string`, `number`, `comment`, `property` (field access),
  `namespace` (module). This gives the best-in-class highlighting the Tree-sitter
  research points to (e.g. immutable vs mutable bindings).
- `textDocument/hover`: type + doc-comment (`#` block just above the fn).
- `textDocument/completion`: stdlib `pub fn` names from `stdlib/lib/**`,
  keywords, struct fields, local scopes, `import` module names.
- `textDocument/definition`, `textDocument/references`, `textDocument/rename`
  via the symbol table.
- `textDocument/formatting`: a Kirn formatter (space-after-keyword, 4-space
  indent, brace style) â€” deliverable of a later phase, but the capability is
  declared here.
- `textDocument/documentSymbol` + `foldingRange` (blocks) + `documentLink`
  (from `import`).

**Code example (semantic token encoding, subset table):**

```
tokenTypes = ["variable","function","type","keyword","string","number","comment","property","namespace","module"]
tokenModifiers = ["declaration","readonly","mutable","defaultLibrary","definition"]
// Encoded as relative row/col + lengths, per LSP semanticTokens
```

**Testing/validation:**

- Protocol conformance tests with `lsif`/`ClientCapabilities` stubs.
- Golden diagnostics: open every `tests/negative/*.kn` and assert the exact
  LSP `Diagnostic` messages match what `kirnparse`/`kirncheck` would print
  (reuse the existing negative test suite verbatim).
- E2E smoke: open `examples/capstone_wordcount.kn`, run hover on an
  identifier, complete a stdlib function, go-to-fn from a call site.
- `kirnlex`/`kirnparse --ast` as the oracle for token/AST agreement.

**Publishing/install:** distribute `kirn-lsp` binary (release assets) + the
client glue in each editor package (VS Code `client/`, `nvim-lspconfig`,
Zed/Helix adapter). Use a JSON-RPC stdio transport; Windows must avoid console
attach issues (detached stdio).

**Expected result:** every editor that connects to `kirn-lsp` gets real-time,
compiler-accurate diagnostics and rich semantic editing regardless of which
highlighter it uses.

**Potential limitations:** full type-checking of a workspace needs module
resolution and incremental caching (the checker currently analyzes single
files/modules); memory/time on very large projects.

---

### Phase 8 â€” Debugging support

**Goal:** debugging Kirn programs from the editor (set breakpoints, step,
inspect locals) using a Debug Adapter Protocol (DAP) server if the interpreter/
VM supports cooperative debugging.

**Must Follow:** expose stack/scopes/line-col through a minimal `DebugSession`; DAP only, no new compiler binary.
**Important Note:** `spawn`/`chan` debugging is best-effort; ship display-only if hooks are not ready.

**Target editor/tool:** VS Code (Debug Adapter host), JetBrains, Vim/Neovim
(DAP clients).

**Required files:**

- `editor/kirn-dap/src/` â€” a DAP server that drives `kirnrun`/the interpreter
  with break/step/variables, or drives the VM via an added debug interface.
- Per-editor debug launch glue.

**Approach & feasibility:** the interpreter (`src/interp/runtime.cpp`) and VM
(`src/vm/`) execute Kirn; a debug interface must expose the stack, scopes, and
the `line`/`col` of each frame (the lexer already tracks precise line/col on
tokens). Add a minimal `DebugSession` in the runtime that can pause/step and
read local bindings, then expose it via DAP JSON-RPC. If not feasible in this
cycle, ship **display-only** debugging limits and defer to a later milestone.

**Debug sample (`debug_demo.kn`):**

```kirn
fn main() {
    var total = 0
    for i in 1..=3 { total = total + i }   # breakpoint on this line
    print(total)
}
```

**KIRN features covered:** breakpoints by file:line, step into/over/out,
locals & inspection (strings, ints, option/result values, collections).

**Testing/validation:** a tiny scripted DAP session (send config â†’ set BP â†’
continue â†’ inspect frame) asserted against expected `variables` output.

**Expected result:** `kirn` debugging session launches from the editor with
working break/step/inspect.

**Potential limitations:** requires interpreter or VM debug hooks; concurrency
(`spawn`/`chan`) debug is complex and can be marked "best effort."

---

### Phase 9 â€” Release, publishing, docs, end-to-end validation

**Goal:** make all editor integrations easy to install and keep them consistent.

**Must Follow:** one version tag drives all packages; publish syntax → grammar → editors → LSP; run the full test matrix.
**Important Note:** the §2.3 freshness gate is what keeps every highlighter truthful.

**Approach/required pieces:**

- **Versioning strategy:** single git tag drives all packages. Keep a
  **`kirn-syntax` version** equal to the **language grammar version**
  (`grammar/kirn.ebnf` PHASE/tag), and bump every editor package whose artifact
  depends on it. Use tags like `kirn-syntax-0.1.x`, `kirn-vscode-0.1.x`,
  `tree-sitter-kirn-0.1.x`, `kirn-lsp-0.1.x`. A **release checklist** runs the
  generator + tests, then publishes in dependency order: syntax â†’ grammar â†’
  per-editor â†’ LSP. Currently the repo pins the language at `0.0.1-beta` (README
  status: WIP), so all editor packages start at `0.0.1-beta` and follow semver.

- **Publishing matrix:**
  - VS Code: `.vsix` via `vsce publish` (Marketplace) + `ovsx publish` (Open
    VSX).
  - Vim: GitHub release + README install; optionally a `vim-plug` snippet.
  - Neovim: `tree-sitter-kirn` on the **`tree-sitter-parsers`** registry so
    `:TSInstall kirn` works; plus `nvim-treesitter` config docs.
  - Micro: put the YAML syntax file in `~/.config/micro/syntax/`.
  - Zed: package as an **extension zip** (Zed registry) + `zed: install`.
  - Helix: runtime file + `languages.toml` snippet.
  - highlight.js: submitted upstream (registered language) so CDN builds load
    `kirn`; plus the VitePress site uses it.
  - JetBrains: Marketplace plugin; plus standalone TextMate bundle.
  - GitHub: add `kirn` + `.kn` to **Linguist** (and the tree-sitter grammar) so
    `.kn` is detected, highlighted, and counted.
  - `kirn-lsp`: GitHub Release binary + editor client glue.

- **Automated tests (syntax/parse/highlight):** a central
  `editor/test/` that runs on every CI job:
  - `node editor/test/run.mjs` (golden token maps across all generators),
  - `tree-sitter test` (grammar corpus),
  - rosetta: parse the whole repo corpus with `kirnparse` **and** tree-sitter,
  - VS Code extension test (`@vscode/test-electron`),
  - JetBrains plugin test (`./gradlew test`),
  - highlight.js markup fixtures,
  - `kirn-lsp` diagnostics golden test against `tests/negative/*.kn`.

- **Cross-platform:** all generators and tests must run on Windows, macOS,
  and Linux. Prefer Node.js built-ins and PowerShell + a small Node runner.
  Guard against CRLF: `.gitattributes` already pins `*.kn` to LF; the generator
  reads `spec/kirn.json` and writes files with LF explicitly. The LSP's stdio
  transport must set binary mode on Windows (avoid EOL translation corrupting
  JSON-RPC framing).

- **Synchronization:** the Â§2.3 generation + CI freshness gate is the contract
  that keeps every highlighter truthful as the language evolves. Any change to
  `src/lex/lexer.cpp` keywords/operators, `grammar/kirn.ebnf`, or
  `stdlib/lib/*.kn` signature that is not accompanied by regenerated editor
  artifacts fails CI.

- **Docs:** a `docs/editor-support.md` (or integration into the VitePress
  docs site from `KIRN_DOCS_PLAN.md`) listing per-editor quickstarts, the
  semantics of the `source.kirn` scopes, and how to regenerate. Include a
  `CONTRIBUTING.md` section for editor tooling.

**Code example â€” the central test runner contract (conceptual):**

```
generate --check        # fail if any generated file is stale
test golden             # token scopes equal for all generators on corpus
test treesitter         # tree-sitter corpus + rosetta vs kirnparse
test vscode             # @vscode/test-electron on corpus
test jetbrains          # ./gradlew test
test highlightjs        # markup fixtures
test lsp                # diagnostics vs tests/negative/*
```

**Expected result:** `kirn` is installable and correct across all targeted
editors and platforms with one versioned, synchronized toolchain.

**Potential limitations:** coordinating per-provider publish tokens/credentials
in CI; evergreen compatibility constraints (e.g. LTS vs bleeding-edge editor
versions); reviewing size of the per-editor test matrices.

---

## 6. Shared ending scope names (the contract all grammars must honor)

**Goal:** fix one scope/capture vocabulary every grammar honors.
**Must Follow:** keyword/type/builtin scopes only; never a scope keyed on a user identifier.
**Important Note:** tree-sitter captures map to these same conceptual names.

Adopt a consistent scope vocabulary (TextMate scope names / tree-sitter capture
names) so themes behave identically:

| Scope / capture                                          | Meaning                                                                    |
| -------------------------------------------------------- | -------------------------------------------------------------------------- |
| `comment.line.number-sign.kirn`                          | `#` comment                                                                |
| `keyword.control.kirn`                                   | control flow: `if elif else while for in match case break continue return` |
| `keyword.operator.kirn`                                  | `and or not is as in new defer spawn chan select try raise catch`          |
| `storage.type.kirn`                                      | `struct enum trait impl class interface record` + type-id like `fn`        |
| `storage.modifier.kirn`                                  | `pub extern unsafe`                                                        |
| `constant.language.kirn`                                 | `true false none None self Self`                                           |
| `string.quoted.double.kirn`                              | `"..."` (+ `r`/`b`/`c`)                                                    |
| `meta.embedded.expression.kirn`                          | f-string `{expr}`                                                          |
| `constant.numeric.kirn` / `constant.character.kirn`      | int/float / char                                                           |
| `entity.name.type.kirn`                                  | struct/enum/class/interface/record names                                   |
| `entity.name.function.kirn`                              | `fn`/`fn` names                                                            |
| `variable.other.kirn`                                    | identifiers                                                                |
| `support.function.builtin.kirn` / `support.class.*.kirn` | stdlib `pub fn` / module names (LSP)                                       |
| `meta.identifier-declaration.kirn`                       | declarations (semantic)                                                    |

Tree-sitter queries (`highlights.scm`) map captures to these same conceptual
names so a theme written for `source.kirn` on one editor works on all others.

**TM-scope sample (contract honored by every generator):**

```jsonc
{ "match": "\\b(true|false|none)\\b", "name": "constant.language.kirn" }
```

---

## 7. Repository layout (final recommendation)

**Goal:** place all editor tooling under one top-level directory.
**Must Follow:** keep `kirn-syntax/spec/kirn.json` as the shared contract even if subprojects split to their own repos.
**Important Note:** mirrors/symlinks are dev convenience only.

Add an `~/Projects/kirn-highlight/` top-level directory to the Kirn repo (with its own
`package.json` at the root for tooling):

```
~/Projects/kirn-highlight/
  README.md
  package.json               # tooling + test runner deps
  kirn-syntax/               # source of truth (Phase 0)
    spec/ kirn.json builtins.json
    grammars/ kirn.tmLanguage.json tree-sitter-kirn/
    highlight/
  tools/
    extract.ps1  generate.mjs
  test/
    corpus/*.kn  run.mjs  snapshots/
  vscode/                    # Phase 1   (publishes kirn-vscode)
  vim/                       # Phase 2
  neovim/                    # Phase 2/3/7
  tree-sitter-kirn/          # Phase 3   (publishes tree-sitter-kirn; Neovim/Zed/Helix/GitHub consume)
  zed/                       # Phase 4
  helix/                     # Phase 4
  micro/                     # Phase 5
  highlightjs/               # Phase 5
  jetbrains/  jetbrains-kirn-plugin/   # Phase 6
  lsp/                       # Phase 7   (kirn-lsp)
  dap/                       # Phase 8
```

Where a subproject wants its own repo (I recommend `tree-sitter-kirn` and
`kirn-lsp` eventually live standalone), keep a mirror/symlink here for
monorepo ease during development and split later. The single
`kirn-syntax/spec/kirn.json` remains the shared contract regardless.

---

## 8. Cross-cutting technical notes

**Goal:** capture the token classes that break naive highlighters.
**Must Follow:** model f-strings structurally; treat `\` continuation as whitespace; emit no block-comment rules.
**Important Note:** case sensitivity and brace-based (not indentation-based) blocks are non-negotiable.

- **f-strings are the hard case.** Every regex highlighter must special-case
  `f"..."` with `{...}` interpolation; tree-sitter models it as a first-class
  node with embedded expressions; the LSP reports `FStr*` sub-token kinds.
  Tests for f-strings (interpolation, format specs, nested braces, escapes)
  belong in the corpus in every phase.
- **`#` is both comment and not a directive.** No directives; keep it a plain
  line comment. Micro/Vim comment toggle uses `#`.
- **Case sensitivity.** `self`/`Self`, `none`/`None` are distinct tokens â€” keep
  grammars case-sensitive (`case_insensitive: false`).
- **`\` continuation** at EOL joins lines; grammars should treat it as
  whitespace so a `{` at the end of a continued line still opens a block.
- **No significant indentation** means auto-indent should follow `{ }` depth,
  not column parity (differs from Python-style editors).
- **Block comments absent:** do not emit `/* */` comment rules.

**Continuation/block sample (`continuation.kn`):**

```kirn
fn total() -> Int {
    return 1 + \
           2
}
```

---

## 9. Risks & mitigations

**Goal:** surface the highest-risk gaps and their mitigations.
**Must Follow:** every risk maps to a concrete CI or golden-test mitigation.
**Important Note:** definition drift is the top risk; §2.3 is the primary control.

| Risk                                              | Mitigation                                                                                                     |
| ------------------------------------------------- | -------------------------------------------------------------------------------------------------------------- |
| Definition drift as Kirn evolves                  | Â§2.3 generator + CI freshness gate; golden tests                                                              |
| f-string / contextual keyword highlighting errors | dedicated corpus tests; tree-sitter models structurally; LSP supplies semantic tokens                          |
| High effort on JetBrains & LSP phases             | Stage A (TextMate bundle) delivers value cheaply; LSP reuses existing lexer/parser/checker                     |
| tree-sitter precedence/conflicts                  | iterative `tree-sitter test` + rosetta parse of full repo                                                      |
| Editor version compatibility                      | pin supported versions per package; publish artifacts under their own semver while tying to cog syntax version |
| Windows stdio/CRLF issues for LSP/generators      | LF enforcement via `.gitattributes`; binary-mode stdio; per-OS CI matrix                                       |
| Concurrency debugging                             | mark `spawn`/`chan` debugging best-effort; document limits                                                     |

---

## 10. Immediate next steps (fastest path to working tooling)

**Goal:** give the fastest path to working editor tooling.
**Must Follow:** start at Phase 0 (shared spec + generator + CI), then VS Code, then tree-sitter.
**Important Note:** no editor work begins before the shared spec exists.

1. Phase 0: create `editor/` skeleton + `kirn-syntax/spec/kirn.json` + generator
   - corpus + `run.mjs`; wire a CI job that runs `generate --check` and tests.
2. Phase 1: build `kirn-vscode` TextMate extension (scope `source.kirn`,
   `.kn`), pass corpus golden tests, `vsce package`.
3. Phase 3: author `tree-sitter-kirn/grammar.js`; validate against the full
   repo with `kirnparse`-equivalence rosetta; enable in Neovim and Zed.
4. Phase 5: `highlightjs/kirn.js` wired into the VitePress docs site.
5. Phase 7 (research + scaffold): confirm `kirn-lsp` reuses
   `src/lex/lexer.cpp` + `src/parser/parser.cpp` + `src/sema/checker.cpp`; stand
   up stdio JSON-RPC server publishing diagnostics; connect VS Code + Neovim.

**Hello sample (`hello.kn`):**

```kirn
fn greet(name: Str) -> Str {
    return f"Hello, {name}"
}

fn main() { print(greet("Kirn")) }
```

---

_End of plan. This document is a roadmap; each phase should open with a focused
"research + source review" task exactly as this plan was written â€” grounded in
`grammar/kirn.ebnf`, `src/lex/lexer.cpp`, `src/parser/parser.cpp`,
`src/sema/checker.cpp`, `stdlib/lib/*.kn`, and
`tools/*.cpp` â€” before any code is generated._
