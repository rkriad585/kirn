# COCO Editor & Syntax Extension Implementation Plan

- **Status:** DRAFT (roadmap, not yet executed)
- **Author:** RK Riad Khan (`rkriad585`)
- **Date:** 2026-09-04
- **Repo:** `github.com/rkriad585/kirn`
- **Language:** Coco (see `grammar/coco.ebnf`, a.k.a. "the normative grammar")

---

## 0. Executive summary

This plan defines how to give the **Coco programming language** high-quality,
consistent developer tooling across VS Code, Vim, Neovim, Micro, Zed,
Highlight.js, and JetBrains IDEs â€” and, later, a full **Language Server**
(`coco-lsp`).

The strategy is deliberately **not "one grammar everywhere."** Modern editors
each have native integrations that are best served by different technologies.
We therefore build on three shared foundations, then adapt per editor:

1. **A shared language definition** (scopes, keywords, operators, literals,
   comments, bracket pairs, indentation, folding) that every integration
   derives from â€” so all editors stay in sync with one source of truth.
2. **A TextMate grammar (`coco.tmLanguage.json`)** as the mainstream,
   regex-based highlighter used by VS Code, JetBrains, Sublime, and the
   editor-coloring backends of Micro and others.
3. **A tree-sitter grammar (`tree-sitter-coco`)** for the modern, incremental,
   error-tolerant editors (Zed, Neovim, Helix, GitHub) and for structural
   features (folding, indent, structural selection).
4. **A language server (`coco-lsp`)** phase that reuses the compiler's own
   lexer, parser, and checker to provide diagnostics, completion, hover,
   go-to-definition, and semantic highlighting.

A build/generation pipeline keeps the TextMate and tree-sitter grammars and
every editor highlighter synchronized whenever `grammar/coco.ebnf`, the lexer
keyword list (`src/lex/lexer.cpp`), or the standard library change.

---

## 1. Understanding COCO (facts the plan is grounded on)

Everything below is derived from actual source; nothing is invented.

### 1.1 Files and extension

- Source files use the **`.co`** extension (enforced in `.gitattributes`:
  `*.co text eol=lf`). Primary detection extension for every editor.
- The `.gitattributes` also declares `*.cob` and `*.cocolib` as binary
  (compiled/archive output), so they must **not** be treated as Coco source.
- The compiler CLI and tools already exist in `tools/`:
  - `coco` â€” main driver.
  - `cococheck` â€” check-only (no codegen).
  - `cocolex` â€” dump tokens (`Tok`, line, col).
  - `cocoparse` / `cocoparse --ast` â€” parse only, exit 0/1, emits
    `<file>:<line>:<col>: error: <msg>`.
  - `cocorun` â€” run through the interpreter.
- **Implication for LSP:** the compiler's lexer/parser emit the exact `line:col`
  diagnostics an LSP needs. This sharply lowers the cost of building `coco-lsp`.

### 1.2 Lexical model (canonical, from `src/lex/token.h` + `lexer.cpp`)

Token kinds: `Eof, Newline, Indent, Dedent, Ident, Int, Float, Char,
StrNormal, StrRaw, StrByte, StrC, FStringStart/Text/LBrace/RBrace/Colon/Spec/
End, Op, Punct`.

- **Comments:** line-only, `# ...` to end of line. **No block comments.**
  (The lexer skips `#` â†’ end-of-line at `lexer.cpp`.)
- **Line continuation:** trailing `\` at end of line (explicit continuation,
  no NEWLINE token).
- **Indentation is insignificant** â€” the grammar is C-style: `{ }` blocks and
  `;` terminators. `INDENT`/`DEDENT` are emitted but replaced by whitespace
  skipping in the lexer. This matters for *indentation* and *folding* rules in
  editors: auto-indent follows `{`/`}` depth, not Python-style indentation
  blocks.
- **Keywords (frozen v1 set, `isKeyword` in `lexer.cpp`):**

  ```
  def fn var let if elif else while for in return break continue match case
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

### 1.3 Standard library modules (for completion/diagnostics phases)

`stdlib/lib/`: `core`, `io`, `math`, `strings`, `collections`, `json`, `os`,
`path`, `regexp`, `time`; plus `stdlib/text/slug.co`. Public entry points use
`pub def`, imports use `import lib.<mod>;` / `import <mod>;` /
`from <mod> import <name> as <alias>;`.

### 1.4 Existing label / scope naming

No TextMate scope, tree-sitter, or Linguist entry exists yet. This is greenfield.
We will register `source.coco` scope, `.co` extension, and the language id
`coco` everywhere.

---

## 2. The shared source-of-truth: `coco-syntax`

Before writing per-editor files, establish a **single normative definition** that
all integrations consume.

### 2.1 Canonical files (checked into the repo)

```
editor/
  coco-syntax/
    README.md                     # how the package works + how to regenerate
    package.json                  # consumes grammar/spec and emits artifacts
    spec/
      coco.json                   # THE shared language definition (below)
      keyword-frozen.json         # snapshot: keywords + operators + literals
      builtins.json               # stdlib module/function inventory (for LSP)
    grammars/
      coco.tmLanguage.json        # TextMate
      tree-sitter-coco/           # tree-sitter grammar + queries (Phase 3)
    highlight/                    # generated per-platform highlighters
    test/
      corpus/                     # .co snippets with expected scopes/tokens
      run.mjs                     # test runner
```

### 2.2 `spec/coco.json` â€” the single definition

Content is generated from `grammar/coco.ebnf` + the lexer keyword/operator
lists (see Â§2.3 synchronization). It captures:

```json
{
  "languageId": "coco",
  "scopeName": "source.coco",
  "fileExtensions": [".co"],
  "lineComment": "#",
  "blockCommentStart": null,
  "blockCommentEnd": null,
  "caseInsensitive": false,
  "keywords": [ "def", "fn", "var", "...", "bucket" ],
  "contextualKeywords": [ "gather", "yield", "item" ],
  "literals": ["true", "false", "none", "None"],
  "operators3": ["<<=", ">>=", "..=", ".?."],
  "operators2": ["**", "//", "==", "!=", "<=", ">=", "<<", ">>",
                 "+=", "-=", "*=", "/=", "%=", "&=", "|=", "^=",
                 "->", "<-", "..", "=>"],
  "operators1": ["+","-","*","/","%","&","|","^","~","<",">","=","?","@"],
  "punct": "()[]{},:.;",
  "identifier": "[A-Za-z_][A-Za-z0-9_]*",
  "numberPatterns": {
    "int":    "\\b(?:0[xX][0-9a-fA-F_]+|0[oO][0-7_]+|0[bB][01_]+|\\d[\\d_]*)\\b",
    "float":  "\\b\\d[\\d_]*(?:\\.\\d[\\d_]*)?(?:[eE][+-]?\\d+)?(?!\\w)"
  },
  "charLiteral": "'(?:[^'\\\\\\n]|\\\\.|\\\\u\\{[0-9a-fA-F]+\\})'",
  "string": {
    "normal": "\"(?:[^\"\\\\\\n]|\\\\.)*\"",
    "raw":    "r\"(?:[^\"]|\\\")*\"",
    "byte":   "b\"(?:[^\"\\\\\\n]|\\\\.)*\"",
    "c":      "c\"(?:[^\"\\\\\\n]|\\\\.)*\""
  },
  "fstring": {
    "start": "f\"",
    "interpolation": "\\{[^{}]*(?:\\{[^{}]*\\}[^{}]*)*\\}",
    "formatSpec": "\\{[^{}]*:[^{}]*\\}"
  },
  "stdlib": {
    "modules": ["core","io","math","strings","collections","json","os",
                "path","regexp","time"],
    "builtinsFile": "spec/builtins.json"
  }
}
```

### 2.3 Staying synchronized with language changes (critical process)

Because the compiler is under active change, definitions **must not drift**.
Adopt a **generation + golden-test** flow wired into CI:

- A small generator (initially a PowerShell/JScript or Node script in
  `editor/tools`) parses:
  - the keyword list from `src/lex/lexer.cpp` (`isKeyword` array),
  - the operator lists (`kOps3/kOps2/kOp1/kPunct1`),
  - the standard-library module/function names from `stdlib/lib/*.co`
    (`pub def` + module files),
  - (optional) productions from `grammar/coco.ebnf`.
- It regenerates `editor/coco-syntax/spec/coco.json`, then each downstream
  artifact (Â§4 generators). Any diff in the *generated* files against the
  committed copies fails CI, so a language change that forgets to update editor
  support is caught automatically.
- Adding a **new keyword** to the lexer automatically re-highlights it in every
  editor on the next regeneration â€” no manual per-editor edits.

---

## 3. Technology selection per editor (rationale)

| Target | Primary mechanism | Why | Also |
|---|---|---|---|
| **VS Code** | TextMate grammar + LSP (when available) | VS Code tokenizer is TextMate; richest extension host | semantic highlighting from LSP; `language-configuration.json` for brackets/comments/indent |
| **Vim** | `syntax/coco.vim` regex highlighter | Vim 8+/9 does not ship tree-sitter; regex syntax is standard, zero-dependency | `ftdetect`, `indent/coco.vim`, optional `coc.vim`/`vim-lsp` for LSP |
| **Neovim** | `tree-sitter-coco` via `nvim-treesitter` | Neovim >=0.5 has native tree-sitter; gives highlighting, indent, folding, text objects | `nvim-lspconfig` for `coco-lsp` |
| **Micro** | `.yaml` syntax file (regex) | Micro's `micro-syntax` is YAML-based TextMate-influenced regex | bracket/pair config in `settings.json` |
| **Zed** | `tree-sitter-coco` extension | Zed language extensions are tree-sitter grammars + `languages/<lang>/config.toml` | LSP adapter for `coco-lsp` |
| **Highlight.js** | custom language definition (built-in or CDN build) | CommonMark, docs sites, static site generators render code | used by the planned VitePress docs site |
| **JetBrains IDEs** | TextMate bundle initially, then Grammar-Kit plugin | Can add `.tmLanguage` with zero code; Grammar-Kit gives parser/PSI + full IDE features later | Grammar-Kit `.bnf` (parser) + `.flex` (lexer) from our grammar |
| **GitHub** | Linguist + `tree-sitter-coco` | `.co` should be recognized on GitHub; Linguist supports tree-sitter grammars | adds repo language stats + highlighting |
| **Helix** | `tree-sitter-coco` | Helix is tree-sitter-first + LSP | uses the same grammar as Neovim/Zed |
| **Sublime** | TextMate bundle (`.sublime-syntax`) | reuse the shared TextMate grammar | also covers `source.coco` |

**Principle:** *structurally-aware editors (Zed, Neovim, Helix, GitHub, and
eventually VS Code) use tree-sitter; regex-based editors (Vim, Micro, VS Code
default, JetBrains initial, highlight.js, Sublime) use the shared TextMate
grammar; all of them optionally connect to `coco-lsp` for semantic features.*
This follows the industry consensus (2026): tree-sitter for fast structural
feedback, LSP for semantic intelligence â€” complementary, not competing.

---

## 4. Shared generators (the "one definition â†’ many artifacts" pipeline)

A single `editor/tools/generate` step produces or converts:

- `grammars/coco.tmLanguage.json` from `spec/coco.json` (TextMate regex rules).
- `syntax/coco.vim` from `spec/coco.json` (Vim `syn keyword/region`).
- `syntax/coco.yaml` (Micro) from `spec/coco.json`.
- `grammars/coco.sublime-syntax` (Sublime / JetBrains tmLanguage) from the
  TextMate JSON.
- A VS Code `language-configuration.json` (bracket pairs, auto-closing pairs,
  comments, folding markers) generated from `spec/coco.json`.
- `highlightjs: module.exports = ...` (highlight.js definition) from the TextMate
  grammar / spec.
- `builtins.json` for the LSP (from `stdlib/lib/*.co`).

Golden-test fixtures in `editor/test/corpus/*.co` each carry an
"expected scopes/keywords" annotation; the runner checks every platform
generator highlights them identically, catching drift.

---

## 5. Phased roadmap

Phases are ordered to deliver value fast (a working highlighter on the most
common platforms) before investing in slow, complex integrations (LSP, full
JetBrains plugin). Each phase is self-contained and independently shippable.

Phase summary:

| # | Phase | Editors/tooling | Effort |
|---|-------|-----------------|--------|
| 0 | Foundations + repo skeleton + generators | all | M |
| 1 | VS Code extension (TextMate first) | VS Code | S |
| 2 | Vim + Neovim (regex + treesitter) | Vim, Neovim | Sâ€“M |
| 3 | tree-sitter grammar | Neovim, Zed, Helix, GitHub, VS Code later | L |
| 4 | Zed + Helix extensions | Zed, Helix | S |
| 5 | Micro + highlight.js | Micro, highlight.js (docs) | S |
| 6 | JetBrains (TextMate bundle, then Grammar-Kit) | IntelliJ/Rider/GoLand/â€¦ | Mâ€“L |
| 7 | Language Server `coco-lsp` | all editors | L |
| 8 | Debugging support & advanced features | VS Code DAP, JetBrains | M |
| 9 | Release, publishing, docs, e2e validation | all | M |

---

### Phase 0 â€” Foundations, shared spec, generator skeleton

**Goal:** stand up `editor/coco-syntax` as the single source of truth and get
the generation pipeline working end-to-end, so every later phase just consumes
it.

**Target editor/tool:** repository infrastructure (no user-facing output yet).

**Required files:**
- `editor/coco-syntax/spec/coco.json` (hand-seeded from the facts in Â§1).
- `editor/coco-syntax/package.json`.
- `editor/tools/extract.ps1` (reads lexer.cpp keyword/operator + stdlib defs).
- `editor/tools/generate.mjs` (emits all artifact skeletons).
- `editor/test/corpus/*.co` (a first corpus: `hello.co`, `fstring.co`,
  `match_patterns.co`, `structs_enums.co`, `try_catch.co`, `oop.co`,
  `stdlib_import.co`).
- `editor/test/run.mjs` (golden-token comparison runner).

**Implementation approach:** write a generator that, given `spec/coco.json`,
emits every highlighter skeleton in zero-external-dependency format (Node
built-ins + PowerShell). Add a CI job (see Â§12) that fails on stale generated
files.

**COCO features covered:** token classes only at this phase (keywords,
operators, numbers, chars, all string forms including f-strings, `#` comments,
identifiers).

**Code example (corpus fixture, `editor/test/corpus/fstring.co`):**
```coco
# interpolated string: FStr{...}
def greet(name: string, score: int) -> string {
    return f"Hello {name}, score={score:04}";   # f-string + format spec
}
```
Expected tokens: `def`/`string`/`return`/`f"Hello ` etc.; the fixture asserts
the `{name}` and `{score:04}` interpolation regions highlight as embedded
expressions, not plain string text.

**Testing/validation:** `node editor/test/run.mjs` parses each `.co` corpus file
with every generator that exists and asserts the golden token map. No editor
needed.

**Expected result:** `editor/coco-syntax/spec/coco.json` is stable, generators
run, tests pass, and CI enforces freshness.

**Potential limitations:** the generator must be kept simple; full grammar
parsing of `coco.ebnf` is out of scope at this phase (keyword/operator
extraction only).

---

### Phase 1 â€” VS Code extension (TextMate first)

**Goal:** a VS Code extension `coco-vscode` providing syntax highlighting,
comments, bracket matching, auto-indent, folding of braces, and (later)
semantic highlighting + diagnostics.

**Target editor/tool:** Visual Studio Code.

**Required files:**
```
editor/vscode/
  package.json            # language contribution, activation, commands
  language-configuration.json   # comments, brackets, auto-close, folding
  syntaxes/coco.tmLanguage.json # generated from spec/coco.json
  client/  server/        # later: LSP client
  test/                   # extension tests
  CHANGELOG.md
```

**Implementation approach:**
- Contribute the language: `contributes.languages[].id = "coco"`,
  `extensions = [".co"]`, `aliases = ["Coco","coco"]`,
  `configuration = "./language-configuration.json"`.
- Contribute the grammar: `contributes.grammars[] = {
  language:"coco", scopeName:"source.coco", path:"./syntaxes/coco.tmLanguage.json" }`.
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

**COCO syntax/features covered:** full tokenization; later phases add semantic
highlighting (mutable vs immutable bindings, unresolved names) via `coco-lsp`.

**Code example (`syntaxes/coco.tmLanguage.json`, partial):**
```jsonc
{
  "name": "Coco",
  "scopeName": "source.coco",
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
      "captures": { "1": { "name": "punctuation.definition.comment.coco" } },
      "name": "comment.line.number-sign.coco"
    },
    "keyword": {
      "patterns": [
        { "match": "\\b(def|fn|struct|enum|trait|impl|match|case|try|raise|catch)\\b",
          "name": "keyword.control.coco" },
        { "match": "\\b(var|let|and|or|not|is|as|in|new|defer|spawn|chan|select)\\b",
          "name": "keyword.operator.coco" },
        { "match": "\\b(true|false|none|None|self|Self)\\b",
          "name": "constant.language.coco" }
      ]
    },
    "fstring": {
      "begin": "f\"",
      "beginCaptures": { "0": { "name": "punctuation.definition.string.begin.coco" } },
      "end": "\"",
      "name": "string.quoted.double.coco",
      "patterns": [
        {
          "begin": "\\{",
          "end": "\\}",
          "name": "meta.embedded.expression.coco",
          "patterns": [ { "include": "source.coco" } ]
        }
      ]
    }
  }
}
```

**Testing/validation:** `@vscode/test-electron` runs the extension in a real
VS Code instance, opens each corpus `.co`, and asserts token scopes; plus
`vscode-tmlanguage`-style scope snapshots in CI.
**Package:** `vsce package` produces `coco-vscode-<ver>.vsix`; publish to the
Marketplace (`vsce publish`) and Open VSX (`ovsx publish`).

**Expected result:** `.co` files open with correct colors, `#` comments toggle
with a comment shortcut, braces auto-close, block folding works.

**Potential limitations:** TextMate cannot truly parse the context-sensitive
f-string format specs or nested interpolation; the regex approach is
approximate but visually correct. Tree-sitter (Phase 3) later replaces the
tokenizer for verbatim correctness and schema-aware features.

---

### Phase 2 â€” Vim and Neovim

**Goal:** working Coco highlighting in classic Vim (regex), and in Neovim use
tree-sitter (Phase 3) plus an optional LSP (Phase 7).

**Target editor/tool:** Vim â‰¥ 8, Neovim â‰¥ 0.5.

**Required files (Vim):**
- `editor/vim/ftdetect/coco.vim` â€” `au BufRead,BufNewFile *.co setf coco`.
- `editor/vim/syntax/coco.vim` â€” generated `syn` rules.
- `editor/vim/indent/coco.vim` â€” brace-based indent.
- `editor/vim/ftplugin/coco.vim` â€” `setlocal commentstring=# %s`, folding, etc.
- `editor/vim/README.md` â€” install via vim-plug / pathogen / manual.

**Implementation approach (Vim):**
- Generate `syn keyword cocoKeyword def fn var let ...` with different groups
  (`cocoKeywordControl`, `cocoKeywordOperator`, `cocoConstant`).
- `syn region cocoString start=+"+ end=+"+` plus raw/byte/c- variants.
- `syn region cocoFString start=+f"+ end=+"+ contains=cocoInterp` with
  `cocoInterp` = `{...}` region.
- `syn region cocoComment start=+#+ end=+$+`.
- `syn match cocoNumber '\<0[xX][0-9a-fA-F_]\+\|\<[0-9][0-9_]*\%(\.[0-9_]*\)\?\%([eE][+-]\?[0-9]\+\)\?'`.
- link groups to `Statement`, `String`, `Comment`, `Number`, `Type`, etc. so
  any colorscheme works.
- `indent` uses `shiftwidth` keyed on `{`/`}` balance (grammar has no
  significant indentation, but blocks are `{ }`).

**Implementation approach (Neovim):**
- Register `tree-sitter-coco` (Phase 3) in `nvim-treesitter` via a small
  `queries/highlights.scm` (already provided by the grammar package).
- `lua/coco/plugin.lua` OR simple README instructions:
  ```lua
  -- after nvim-treesitter is installed and the grammar built:
  vim.filetype.add({ extension = { co = "coco" } })
  vim.treesitter.language.register("coco", "coco")
  ```
- Later: `nvim-lspconfig` config alias `coco_lsp` once `coco-lsp` exists.

**COCO features covered:** tokenization + (Neovim) structural highlighting and
folding via tree-sitter; `#` comments; all string forms; f-string interpolation.

**Testing/validation:** Vim `syntax` snapshot via `:syn-sync` + `g:loaded_syntax`
manual check; golden output compared. For Neovim, run the tree-sitter parser
over the corpus and compare the CST/highlight against expected queries.

**Expected result:** opening `x.co` in Vim colors keywords/strings/comments;
Neovim additionally gives tree-sitter folding and text objects.

**Potential limitations:** Vim regex cannot handle nested f-string braces
perfectly; acceptable for highlighting. Tree-sitter (Phase 3) is the
recommended path on Neovim.

---

### Phase 3 â€” `tree-sitter-coco` grammar (the modern core)

**Goal:** a compliant tree-sitter grammar for Coco plus queries for
highlighting, indentation, and folding that power Neovim, Zed, Helix, GitHub,
and later VS Code.

**Target editor/tool:** shared by Zed, Neovim, Helix, GitHub, and (via adapter)
others.

**Required files:**
```
editor/tree-sitter-coco/
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
- Author `grammar.js` faithfully from `grammar/coco.ebnf` (v0.2) plus the
  lexer facts. Key productions to model:
  - statements: `simple_stmt` (`;`-terminated), compound `if/elif/else`,
    `while`, `for â€¦ in`, `match/case`, `try/catch`, `defer`, `spawn`,
    `select`, `gather`, `struct/enum/trait/impl/class/interface/record`.
  - expressions: full precedence chain from `grammar/coco.ebnf` Â§2 â€” including
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

**COCO features covered:** everything â€” this becomes the reference structural
parser for the whole ecosystem.

**Code example (`grammar.js`, fragment):**
```javascript
module.exports = grammar({
  name: 'coco',
  extras: $ => [ /[ \t\r\n]+/, $.line_continuation, $.comment ],
  keywords: [ 'def','fn','var','let','if','elif','else','while','for','in',
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
`stdlib/lib/*.co`, `tests/`** with `cocoparse` and with
tree-sitter, and byte-compare the **structure** (tree-sitter has no parse
errors where `cocoparse` succeeds). This is the single strongest correctness
check.

**Expected result:** a grammar that parses the whole real corpus with zero
errors, with queries giving correct highlighting/indent/folds.

**Potential limitations:** tree-sitter DSL divergence from hand-written recursive
descent (e.g. operator precedence is expressed via `precedence` declarations,
not code); f-string embedded sub-expressions need careful lexer/extra setup and
may require `token.immediate` handling.

---

### Phase 4 â€” Zed and Helix extensions

**Goal:** make Coco a first-class language in Zed and Helix by packaging the
Phase 3 grammar plus (Phase 7) LSP adapters.

**Target editor/tool:** Zed, Helix.

**Required files (Zed):**
```
editor/zed/coco.json          # config used by "zed: install extension"
editor/zed/languages/coco/config.toml
editor/zed/languages/coco/highlights.scm
editor/zed/languages/coco/indents.scm
editor/zed/languages/coco/folds.scm
editor/zed/languages/coco/outline.scm
editor/zed/extensions.toml
```
`config.toml` (fragment):
```toml
name = "Coco"
grammar = "coco"
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
- `languages/highlights.scm`, `indents.scm`, `textobjects.scm`, `folds.scm`
  â€” reuse the same tree-sitter queries.
- `languages.toml`: register `name="coco"`, `scope="source.coco"`,
  `file-types=["co"]`, `comment-tokens=["#"]`, `language-server={...}` for LSP.

**Implementation approach:** mostly config + reuse of Phase 3 queries. Zed
extensions are zip files with a `languages/` layout; Helix reads language
config + queries from `$XDG_CONFIG_HOME/helix` or the runtime directory.

**COCO features covered:** highlighting, indentation, folding, structural
selections, LSP integration (diagnostics/completion/hover) once `coco-lsp`
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

**Expected result:** consistent Coco editing in both modern terminal-native
editors with mutating, incremental highlighting.

**Potential limitations:** version pinning of the editor's tree-sitter runtime;
Zed/Helix occasionally lag on newest tree-sitter features.

---

### Phase 5 â€” Micro and highlight.js

**Goal:** Coco support in the Micro terminal editor and on the Web via
highlight.js (used by the VitePress docs and any static site).

**Target editor/tool:** Micro, highlight.js (and the docs site in
`COCO_DOCS_PLAN.md`).

**Required files:**
- `editor/micro/syntax/coco.yaml` (regex-based, Micro `micro-syntax`).
- `editor/micro/settings.json` fragment (comment `#`, brackets, tab).
- `editor/highlightjs/coco.js` (a CommonJS/ESM language definition).

**Implementation approach (Micro `coco.yaml`):**
```yaml
filetype: coco
detect:
  filename: "\\.co$"
rules:
  - comment: "#.*$"
  - statement:
      - keyword: "def|fn|var|let|if|elif|else|while|for|in|return|break|continue|match|case|struct|enum|trait|impl|import|export|pub|defer|spawn|chan|select|try|raise|catch|unsafe|extern|new|box|self|Self|and|or|not|is|as|pass|class|interface|record|implements|extends|dynamic|del|pr|local|global|temp|bucket"
  - constant:
      - "true|false|none|None"
  - type: "string|int|float|bool|char|list|dict|tuple|option|result|byte|any|void|...|Point|[A-Z][A-Za-z0-9_]*"
  - string: "\".*?\""
  - number: "\\b(0[xX][0-9a-fA-F_]+|0[oO][0-7_]+|0[bB][01_]+|\\d+[\\d_]*(\\.[\\d_]*)?([eE][+-]?\\d+)?)\\b"
```
Add bracket/comment config in `settings.json`:
```json
{
  "comment": "#",
  "matchPairs": { "{":"}", "(":")", "[":"]" },
  "autoPairs": { "{":"}", "(":")", "[":"]", "\"":"\"" }
}
```

**Implementation approach (highlight.js `coco.js`):**
Reuse `spec/coco.json` to build a definition:
```javascript
module.exports = function(hljs) {
  const KEY = ['def','fn','var','let','if','elif','else','while','for','in',
    'return','break','continue','match','case','struct','enum','trait',
    'impl','import','export','pub','defer','spawn','chan','select','try',
    'raise','catch','unsafe','extern','new','box','self','Self','and','or',
    'not','is','as','true','false','none','pass','class','interface',
    'record','implements','extends','dynamic','None','del','pr','local',
    'global','temp','bucket'];
  return {
    name: 'Coco',
    aliases: ['coco', 'co'],
    case_insensitive: false,
    keywords: { keyword: KEY, literal: ['true','false','none','None'],
                built_in: ['print','import','export'] },
    contains: [
      hljs.HASH_COMMENT_MODE,                // '#' comments
      hljs.QUOTE_STRING_MODE,
      { className: 'string', begin: /f"/, ... interpolation },
      hljs.C_NUMBER_MODE,
      { className: 'number', begin: /0[xX][0-9a-fA-F_]+/ },
      { className: 'type', begin: /\b[A-Z][A-Za-z0-9_]*\b/ }
    ]
  };
};
```
Optionally submit upstream as `highlight.js/src/languages/coco.js` so that
`highlight.js` CDN builds include it (great for the docs site and GitHub-flavored
markdown on many platforms).

**COCO features covered:** tokenization; `#` comments; strings incl. f-strings
via a small embedded-expression regex; keywords; type-name heuristics
(PascalCase).

**Testing/validation (highlight.js):** use the official
`highlight.js/test/detect` + markup-fixture tests; assert a corpus renders with
the expected `<span class="hljs-...">` classes (DOM/cheerio snapshot).

**Expected result:** `coco` used as a fencelang in markdown
```` ```coco ```` on the docs site colors correctly; Micro opens `.co` colored.

**Potential limitations:** highlight.js and Micro are regex-based; f-string
interpolation and multi-line constructs are approximations only. Fine for
display.

---

### Phase 6 â€” JetBrains IDEs

**Goal:** Coco support in IntelliJ-based IDEs (IDEA, PyCharm, GoLand, Rider,
CLion, WebStorm), starting with a zero-code TextMate bundle and graduating to a
Grammar-Kit plugin.

**Target editor/tool:** JetBrains IntelliJ Platform IDEs (2022.3+, Java 17).

**Required files (TextMate stage):**
- `editor/jetbrains/coco.tmLanguage` (XML plist, or reuse the shared JSON) plus
  a bundle `syntaxes/` folder; installed via
  `Settings â†’ Editor â†’ TextMate Bundles`.
- `editor/jetbrains/coco.dic` (for spell-check dictionary; optional).

**Required files (Grammar-Kit stage):**
```
editor/jetbrains-coco-plugin/
  build.gradle.kts
  src/main/grammar/Coco.bnf     # Grammar-Kit PEG grammar (from coco.ebnf)
  src/main/grammar/CocoLexer.flex  # JFlex lexer (from lexer.cpp)
  src/main/gen/...              # generated parser + PSI
  src/main/java/...             # ParserDefinition, Annotator, Completion, etc.
  src/main/resources/META-INF/plugin.xml
```

**Implementation approach:**
- **Stage A (days):** ship a TextMate bundle. Uses `src/grammar` scopes and
  gives immediate highlighting with zero Java code.
- **Stage B (weeks):** Grammar-Kit plugin.
  - Author `Coco.bnf` mapping the canonical `coco.ebnf` productions to
    Grammar-Kit PEG (choice `|`, optional `[ ]`, repetition `*` â€” Grammar-Kit
    accepts left-recursion in many cases but prefer iterative rules).
  - Write `CocoLexer.flex` matching the C++ lexer token classes and keywords
    (JFlex longest-match, ignores `#` comments and inserts whitespace).
  - Generate parser + PSI (`Ctrl+Shift+G`), add `ParserDefinition`,
    `SyntaxHighlighter`, `BraceMatcher`, `QuoteHandler`, `CodeStyleSettings`
    (auto-indent on `{`...`}`), `Annotator` (syntax errors), `CompletionContributor`
    (stdlib `pub def` names), `GotoDeclarationHandler` (go-to-def), and
    `FindUsagesProvider`.
  - Optionally reuse the **pre-existing C++** lexer/parser logic conceptually;
    do *not* try to JNI-bridge yet (see Phase 7 note about an LSP-based approach
    that avoids a native bridge entirely).

**COCO features covered:** full PSI/IDE experience â€” highlighting, bracket
matching, auto-indent, folding, code completion of stdlib, go-to-definition,
find usages, refactoring-safe structure view.

**Code example (`Coco.bnf` fragment):**
```bnf
{
  parserClass="coco.lang.parser.CocoParser"
  extends="com.intellij.extapi.psi.ASTWrapperPsiElement"
  psiPackage="coco.lang.psi"
  elementTypeHolderClass="coco.lang.psi.CocoTypes"
  tokenTypeClass="coco.lang.psi.CocoTokenType"
  psiImplUtilClass="coco.lang.psi.impl.CocoPsiImplUtil"
  tokens = [
    comment='regexp:#[^\r\n]*'
    STRING='regexp:"[^"\n]*"'
    ...
    DEF='def'  FN='fn'  IF='if'  ...
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
`examples/*.co` parses to a full tree.

**Publishing:** JetBrains **Marketplace** via `./gradlew publishPlugin`
(needs Marketplace token). Also distribute the TextMate bundle independently
so non-IntelliJ helpers benefit.

**Expected result:** full-featured Coco editing inside JetBrains IDEs without
any native code.

**Potential limitations:** Grammar-Kit requires Java 17 + recent IDEA; the
`mixin`/`psiImplUtilClass` limitations in the Gradle Grammar-Kit plugin mean
generated PSI may need in-IDE generation. This is the highest-effort per-editor
phase.

---

### Phase 7 â€” Language Server `coco-lsp`

**Goal:** a Language Server Protocol server giving cross-editor semantic
features: diagnostics (syntax + type), hover, completion, go-to-definition,
reference finding, rename, formatting, document symbols, and **semantic
highlighting**.

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

The server uses the *module + workspace* model:
- Parse + type-check a workspace; cache per-file ASTs; incremental re-parse on
  change (tree-sitter or the Coco parser), and re-run the checker for the
  affected file + dependents.
- Publish `textDocument/publishDiagnostics` for:
  - lexical errors (unterminated strings/chars, bad numeric literals),
  - parse errors,
  - semantic errors (undefined variable/field/function, type mismatch,
    immutable reassignment, exhaustiveness, borrow violations) â€” reusing the
    existing `tests/negative/n*.co` expectations as acceptance fixtures.
- `textDocument/semanticTokens` with token types: `variable` (with modifiers
  `readonly`/`mutable`), `function`, `type` (struct/enum/trait/class/interface/
  record), `keyword`, `string`, `number`, `comment`, `property` (field access),
  `namespace` (module). This gives the best-in-class highlighting the Tree-sitter
  research points to (e.g. immutable vs mutable bindings).
- `textDocument/hover`: type + doc-comment (`#` block just above the def).
- `textDocument/completion`: stdlib `pub def` names from `stdlib/lib/**`,
  keywords, struct fields, local scopes, `import` module names.
- `textDocument/definition`, `textDocument/references`, `textDocument/rename`
  via the symbol table.
- `textDocument/formatting`: a Coco formatter (space-after-keyword, 4-space
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
- Golden diagnostics: open every `tests/negative/*.co` and assert the exact
  LSP `Diagnostic` messages match what `cocoparse`/`cococheck` would print
  (reuse the existing negative test suite verbatim).
- E2E smoke: open `examples/capstone_wordcount.co`, run hover on an
  identifier, complete a stdlib function, go-to-def from a call site.
- `cocolex`/`cocoparse --ast` as the oracle for token/AST agreement.

**Publishing/install:** distribute `coco-lsp` binary (release assets) + the
client glue in each editor package (VS Code `client/`, `nvim-lspconfig`,
Zed/Helix adapter). Use a JSON-RPC stdio transport; Windows must avoid console
attach issues (detached stdio).

**Expected result:** every editor that connects to `coco-lsp` gets real-time,
compiler-accurate diagnostics and rich semantic editing regardless of which
highlighter it uses.

**Potential limitations:** full type-checking of a workspace needs module
resolution and incremental caching (the checker currently analyzes single
files/modules); memory/time on very large projects.

---

### Phase 8 â€” Debugging support

**Goal:** debugging Coco programs from the editor (set breakpoints, step,
inspect locals) using a Debug Adapter Protocol (DAP) server if the interpreter/
VM supports cooperative debugging.

**Target editor/tool:** VS Code (Debug Adapter host), JetBrains, Vim/Neovim
(DAP clients).

**Required files:**
- `editor/coco-dap/src/` â€” a DAP server that drives `cocorun`/the interpreter
  with break/step/variables, or drives the VM via an added debug interface.
- Per-editor debug launch glue.

**Approach & feasibility:** the interpreter (`src/interp/runtime.cpp`) and VM
(`src/vm/`) execute Coco; a debug interface must expose the stack, scopes, and
the `line`/`col` of each frame (the lexer already tracks precise line/col on
tokens). Add a minimal `DebugSession` in the runtime that can pause/step and
read local bindings, then expose it via DAP JSON-RPC. If not feasible in this
cycle, ship **display-only** debugging limits and defer to a later milestone.

**COCO features covered:** breakpoints by file:line, step into/over/out,
locals & inspection (strings, ints, option/result values, collections).

**Testing/validation:** a tiny scripted DAP session (send config â†’ set BP â†’
continue â†’ inspect frame) asserted against expected `variables` output.

**Expected result:** `coco` debugging session launches from the editor with
working break/step/inspect.

**Potential limitations:** requires interpreter or VM debug hooks; concurrency
(`spawn`/`chan`) debug is complex and can be marked "best effort."

---

### Phase 9 â€” Release, publishing, docs, end-to-end validation

**Goal:** make all editor integrations easy to install and keep them consistent.

**Approach/required pieces:**

- **Versioning strategy:** single git tag drives all packages. Keep a
  **`coco-syntax` version** equal to the **language grammar version**
  (`grammar/coco.ebnf` PHASE/tag), and bump every editor package whose artifact
  depends on it. Use tags like `coco-syntax-0.1.x`, `coco-vscode-0.1.x`,
  `tree-sitter-coco-0.1.x`, `coco-lsp-0.1.x`. A **release checklist** runs the
  generator + tests, then publishes in dependency order: syntax â†’ grammar â†’
  per-editor â†’ LSP. Currently the repo pins the language at `0.0.1-beta` (README
  status: WIP), so all editor packages start at `0.0.1-beta` and follow semver.

- **Publishing matrix:**
  - VS Code: `.vsix` via `vsce publish` (Marketplace) + `ovsx publish` (Open
    VSX).
  - Vim: GitHub release + README install; optionally a `vim-plug` snippet.
  - Neovim: `tree-sitter-coco` on the **`tree-sitter-parsers`** registry so
    `:TSInstall coco` works; plus `nvim-treesitter` config docs.
  - Micro: put the YAML syntax file in `~/.config/micro/syntax/`.
  - Zed: package as an **extension zip** (Zed registry) + `zed: install`.
  - Helix: runtime file + `languages.toml` snippet.
  - highlight.js: submitted upstream (registered language) so CDN builds load
    `coco`; plus the VitePress site uses it.
  - JetBrains: Marketplace plugin; plus standalone TextMate bundle.
  - GitHub: add `coco` + `.co` to **Linguist** (and the tree-sitter grammar) so
    `.co` is detected, highlighted, and counted.
  - `coco-lsp`: GitHub Release binary + editor client glue.

- **Automated tests (syntax/parse/highlight):** a central
  `editor/test/` that runs on every CI job:
  - `node editor/test/run.mjs` (golden token maps across all generators),
  - `tree-sitter test` (grammar corpus),
  - rosetta: parse the whole repo corpus with `cocoparse` **and** tree-sitter,
  - VS Code extension test (`@vscode/test-electron`),
  - JetBrains plugin test (`./gradlew test`),
  - highlight.js markup fixtures,
  - `coco-lsp` diagnostics golden test against `tests/negative/*.co`.

- **Cross-platform:** all generators and tests must run on Windows, macOS,
  and Linux. Prefer Node.js built-ins and PowerShell + a small Node runner.
  Guard against CRLF: `.gitattributes` already pins `*.co` to LF; the generator
  reads `spec/coco.json` and writes files with LF explicitly. The LSP's stdio
  transport must set binary mode on Windows (avoid EOL translation corrupting
  JSON-RPC framing).

- **Synchronization:** the Â§2.3 generation + CI freshness gate is the contract
  that keeps every highlighter truthful as the language evolves. Any change to
  `src/lex/lexer.cpp` keywords/operators, `grammar/coco.ebnf`, or
  `stdlib/lib/*.co` signature that is not accompanied by regenerated editor
  artifacts fails CI.

- **Docs:** a `docs/editor-support.md` (or integration into the VitePress
  docs site from `COCO_DOCS_PLAN.md`) listing per-editor quickstarts, the
  semantics of the `source.coco` scopes, and how to regenerate. Include a
  `CONTRIBUTING.md` section for editor tooling.

**Code example â€” the central test runner contract (conceptual):**
```
generate --check        # fail if any generated file is stale
test golden             # token scopes equal for all generators on corpus
test treesitter         # tree-sitter corpus + rosetta vs cocoparse
test vscode             # @vscode/test-electron on corpus
test jetbrains          # ./gradlew test
test highlightjs        # markup fixtures
test lsp                # diagnostics vs tests/negative/*
```

**Expected result:** `coco` is installable and correct across all targeted
editors and platforms with one versioned, synchronized toolchain.

**Potential limitations:** coordinating per-provider publish tokens/credentials
in CI; evergreen compatibility constraints (e.g. LTS vs bleeding-edge editor
versions); reviewing size of the per-editor test matrices.

---

## 6. Shared ending scope names (the contract all grammars must honor)

Adopt a consistent scope vocabulary (TextMate scope names / tree-sitter capture
names) so themes behave identically:

| Scope / capture | Meaning |
|---|---|
| `comment.line.number-sign.coco` | `#` comment |
| `keyword.control.coco` | control flow: `if elif else while for in match case break continue return` |
| `keyword.operator.coco` | `and or not is as in new defer spawn chan select try raise catch` |
| `storage.type.coco` | `struct enum trait impl class interface record` + type-id like `fn` |
| `storage.modifier.coco` | `pub extern unsafe` |
| `constant.language.coco` | `true false none None self Self` |
| `string.quoted.double.coco` | `"..."` (+ `r`/`b`/`c`) |
| `meta.embedded.expression.coco` | f-string `{expr}` |
| `constant.numeric.coco` / `constant.character.coco` | int/float / char |
| `entity.name.type.coco` | struct/enum/class/interface/record names |
| `entity.name.function.coco` | `def`/`fn` names |
| `variable.other.coco` | identifiers |
| `support.function.builtin.coco` / `support.class.*.coco` | stdlib `pub def` / module names (LSP) |
| `meta.identifier-declaration.coco` | declarations (semantic) |

Tree-sitter queries (`highlights.scm`) map captures to these same conceptual
names so a theme written for `source.coco` on one editor works on all others.

---

## 7. Repository layout (final recommendation)

Add an `~/Projects/coco-highlight/` top-level directory to the Coco repo (with its own
`package.json` at the root for tooling):

```
~/Projects/coco-highlight/
  README.md
  package.json               # tooling + test runner deps
  coco-syntax/               # source of truth (Phase 0)
    spec/ coco.json builtins.json
    grammars/ coco.tmLanguage.json tree-sitter-coco/
    highlight/
  tools/
    extract.ps1  generate.mjs
  test/
    corpus/*.co  run.mjs  snapshots/
  vscode/                    # Phase 1   (publishes coco-vscode)
  vim/                       # Phase 2
  neovim/                    # Phase 2/3/7
  tree-sitter-coco/          # Phase 3   (publishes tree-sitter-coco; Neovim/Zed/Helix/GitHub consume)
  zed/                       # Phase 4
  helix/                     # Phase 4
  micro/                     # Phase 5
  highlightjs/               # Phase 5
  jetbrains/  jetbrains-coco-plugin/   # Phase 6
  lsp/                       # Phase 7   (coco-lsp)
  dap/                       # Phase 8
```

Where a subproject wants its own repo (I recommend `tree-sitter-coco` and
`coco-lsp` eventually live standalone), keep a mirror/symlink here for
monorepo ease during development and split later. The single
`coco-syntax/spec/coco.json` remains the shared contract regardless.

---

## 8. Cross-cutting technical notes

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

---

## 9. Risks & mitigations

| Risk | Mitigation |
|---|---|
| Definition drift as Coco evolves | Â§2.3 generator + CI freshness gate; golden tests |
| f-string / contextual keyword highlighting errors | dedicated corpus tests; tree-sitter models structurally; LSP supplies semantic tokens |
| High effort on JetBrains & LSP phases | Stage A (TextMate bundle) delivers value cheaply; LSP reuses existing lexer/parser/checker |
| tree-sitter precedence/conflicts | iterative `tree-sitter test` + rosetta parse of full repo |
| Editor version compatibility | pin supported versions per package; publish artifacts under their own semver while tying to cog syntax version |
| Windows stdio/CRLF issues for LSP/generators | LF enforcement via `.gitattributes`; binary-mode stdio; per-OS CI matrix |
| Concurrency debugging | mark `spawn`/`chan` debugging best-effort; document limits |

---

## 10. Immediate next steps (fastest path to working tooling)

1. Phase 0: create `editor/` skeleton + `coco-syntax/spec/coco.json` + generator
   + corpus + `run.mjs`; wire a CI job that runs `generate --check` and tests.
2. Phase 1: build `coco-vscode` TextMate extension (scope `source.coco`,
   `.co`), pass corpus golden tests, `vsce package`.
3. Phase 3: author `tree-sitter-coco/grammar.js`; validate against the full
   repo with `cocoparse`-equivalence rosetta; enable in Neovim and Zed.
4. Phase 5: `highlightjs/coco.js` wired into the VitePress docs site.
5. Phase 7 (research + scaffold): confirm `coco-lsp` reuses
   `src/lex/lexer.cpp` + `src/parser/parser.cpp` + `src/sema/checker.cpp`; stand
   up stdio JSON-RPC server publishing diagnostics; connect VS Code + Neovim.

---

_End of plan. This document is a roadmap; each phase should open with a focused
"research + source review" task exactly as this plan was written â€” grounded in
`grammar/coco.ebnf`, `src/lex/lexer.cpp`, `src/parser/parser.cpp`,
`src/sema/checker.cpp`, `stdlib/lib/*.co`, and
`tools/*.cpp` â€” before any code is generated._
