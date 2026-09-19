# COCO Documentation Website â€” Implementation & Research Plan

**Deliverable:** `COCO_DOCS_PLAN.md`
**Author:** rkriad585 Â· **Date:** 2026-09-03
**Scope:** a complete, handed-off roadmap an implementer can follow to build a modern **Coco language documentation website** at `~/Projects/coco-docs` using **Vite + TypeScript**.
**Method:** the project (source code, tooling, stdlib, docs, branding) was analyzed *first*; the actual compiler was built and run against the example corpus to establish ground truth of what really works; 2026 web research on documentation frameworks, developer-experience best practices, and developer pain points informed the architecture and information design.

---

## 0. Before you build â€” what the research actually found (read this first)

### 0.1 This is a real, working language (verified, not aspirational)
I did **not** trust the README. I built the compiler from `src/` (C++20, CMake, `build/`) and ran it against the example corpus. It genuinely works. These features are **verified working** and can be documented with real, runnable examples:

| Feature | Proof of work |
|---|---|
| Hello World, f-strings, format specs | `examples/01_hello.co` â†’ `Hello, World!` |
| Variables, immutability-by-default, type annotations | `examples/02_variables.co` |
| Conditionals as expressions (`if`/`elif`/`else`) | `examples/04_conditionals.co` |
| Loops/ranges (`..`, `..=`, `while`) | `examples/05_loops_ranges.co` |
| Functions/params/closures/captures | `examples/06,07` |
| `match` + guards + range patterns | `examples/09_match_guards_ranges.co` |
| structs, methods, value semantics | `examples/11_structs_methods.co` |
| enums + exhaustive match | `examples/12_enums_exhaustive.co` |
| traits + static/dynamic dispatch | `examples/13_traits_dispatch.co` |
| generics + bounds `[T is Bound]` | `examples/14_generics_bounds.co` |
| collections, slices, dict/set displays | `examples/16_collections_slices.co` |
| optionals / nil-safety (`T?`, `.?.`) | `examples/18_optionals_nil_safe.co` |
| results / `try` propagation | `examples/19_results_try_propagation.co` |
| `defer` / `panic` | `examples/20_defer_panic.co` |
| concurrency: `spawn`, `chan`, `select`, `join` | `examples/22,23` â†’ green threads + channels |
| FFI + `unsafe` | `examples/24_ffi_unsafe.co` |
| value semantics copy, weak refs, arenas | `examples/27,28,29` |
| optionals/patterns power, generators (`yield`) | `examples/38,41` |
| OOP: `class`/`interface`/`record`/`extends` | `examples/36_oop.co` |
| `any`/`dynamic` duck typing | `examples/37_dynamic_any.co` |
| **Tests** (`*_test.co`), **build** (native `.exe` + bytecode `.cob`) | `coco test`, `coco build --target=...` |

**The docs site must document THIS â€” the verified feature set â€” not the README's inflated promises.**

### 0.2 The README overstates the language (critical honesty rule)
The current `README.md` makes **unsupported, unverifiable claims**: "outperforms C/C++ in many scenarios", "surpassing C/C++, Python, Rust, Ruby", "blazing fast performance", "Rust-inspired ownership model", "advanced IDE support", "networking" in the stdlib. Several of these are **not present in the code today**:
- There is **no observed naming/ownership model** equal to Rust's borrow checker in the corpus (only value-semantics copy plus arenas/weak refs seen).
- There is **no networking module** in `stdlib/lib/` (only `core, io, math, strings, collections, json, os, path, regexp, time, text/slug`).
- There is **no IDE plugin / debugger** in the repo.
- "Concurrency" is real (spawn/channels) but modest; "JIT" is claimed in the README but the pipeline is **ahead-of-time** (tree-walk â†’ VM â†’ native/C++).

**Consequence for the plan:** every performance/feature claim in the docs site must be backed by a verifiable artifact (code you can run, a stdlib module that exists, a command that works). The plan includes a **"Claim Gate"** (Â§1.3) â€” a checklist that runs before publishing any page that makes a comparison or performance statement. The docs earn trust by being honest; the plan treats "no unsupported claims" as a hard requirement, exactly as the brief demands.

### 0.3 Architecture and tooling (verified)
- **Compiler:** C++20; pipeline `src/lex â†’ src/parser â†’ src/sema â†’ src/vm (compiler+bytecode) â†’ src/interp (tree-walk) / src/backend/native (C++ lowering)`.
- **CLI driver `coco`** (plus `cocorun`, `cococheck`, `cocolex`, `cocoparse`). Real subcommands (from `coco --help`, verified): `run`, `new`, `new lib`, `test`, `install/-g`, `add`, `update`, `remove`, `clone`, `build` (+`--release/--debug/--target/--native/--asan/-S/-O/-o`), `targets`, `build lib`, **`doc`**, `list`, `list online`.
- **Built-in doc server exists:** `coco doc <lib|dir> [--port N]` "serve markdown docs + API ref". The website's content should **align with** (and can pull from) this so docs don't drift.
- **Targets:** `windows/linux/darwin Ã— amd64/arm64`; bytecode bundles `.cob`, libraries `.cocolib`, native `.exe`.
- **Project scaffold** (`coco new demo`): `coco.toml` manifest (`[package] name/version/type/main/docs/description/license/author/repo/homepage`, `[dependencies]`), `code/main.co`, `tests/`, `docs/`, `.gitignore`.
- **stdlib modules** (each with a `_test.co`): `core, io, math, strings, collections, json, os, path, regexp, time` plus `text/slug`. Public API surface verified (e.g. `read_file`, `write_file`, `sqrt/abs/clamp/min2/max2/ipow/fpow`, `contains/starts_with/ends_with/replace_all/split/join/repeat/reverse/pad_left/pad_right/strip`, `unique/shuffle/rand_float`, `dumps/loads`, `args/getenv/exit`, â€¦).
- **Grammar:** normative EBNF in `grammar/coco.ebnf` (557 lines) â€” the source of truth for the **Language Reference** pages. Plus the **canonical keyword list** in `src/lex/lexer.cpp` (authoritative for a syntax-highlighting grammar â€” see Â§6.3).

### 0.4 Canonical keyword / operator set (for the syntax-highlighting grammar)
From `src/lex/lexer.cpp` (authoritative â€” use this, not the README):
```
def fn var let if elif else while for in return break continue match case
struct enum trait impl import export pub defer spawn chan select try raise catch
unsafe extern new box self Self and or not is as true false none pass class
interface record implements extends dynamic None del pr local global temp bucket
```
Operators (multi-char): `<<= >>= ..= .?.` ; `** // == != <= >= << >> += -= *= /= %= &= |= ^= -> <- .. =>`; single `+ - * / % & | ^ ~ < > = ? @`; punctuation `()[]{},:.;`. Types: `int, usize, bool, string, float, list, dict, set, any, dynamic, fn`. This is the data you'll register as a Shiki custom language (Â§6.3).

### 0.5 Branding, license, version, hosting facts
- **License:** MIT Â© 2024 RK Riad Khan (`LICENSE.txt`) â€” your docs site is free to reuse inputs; keep attribution.
- **Version source:** `.version` file at repo root (currently **empty**) and the `coco.toml` `version = "0.0.1-beta"` scaffold default + SemVer in `[package]`. There is **no CLI `--version`** (verified: `coco --version` prints usage). The **website needs a version display**, so this is a required input â€” see Phase 3 (version config).
- **Hosting:** resource list says Documentation is `https://kirn-lang.github.io`. **VitePress deploys as static output** â†’ GitHub Pages is the natural target (`gh-pages` branch or `{username}.github.io`). Deployment strategy in Â§8.
- **Branding â†’ rebrand caution:** the repo's `kirn-logos/` directory contains **`kirn-*` branded assets** (this is the "rename project" tracked in sibling plan `COCO_TO_KIRN_PLAN.md`). The provided branding URLs point at the `kirn-*` logos. **The docs site's visual identity must track whichever name wins.** This plan builds with a **single source of truth for the brand** (a `theme` object in config) so the "Coco" â‡¢ "Kirn" swap is a one-line change â€” see Phase 2 (Â§2.5) and the branding note (Â§9).

---

## 1. Architecture & core decisions (decide these once)

### 1.1 Framework: **VitePress** (satisfies "Vite + TypeScript" with the fewest dependencies)
The brief mandates **Vite + TypeScript** and "the simplest appropriate architecture rather than adding unnecessary dependencies." The 2026 research (Docusaurus vs VitePress vs Starlight vs Nextra) converges on: **VitePress is the Vite-native, zero-config, fastest docs framework** â€” "the most polished docs framework in 2026 â€” Vue-powered, Vite-built, zero-config, used by Vue.js, Vite, and Rollup themselves." It is literally built on Vite and configured in TypeScript, and it gives us, for free:

| Requirement | VitePress capability |
|---|---|
| Search | **Built-in local full-text search** (MiniSearch) â€” `search.provider: 'local'`, no API key, always current, works offline. Optional Algolia/Pagefind upgrade path. |
| Syntax highlighting | **Shiki** built in; supports **custom language aliases** â†’ register `co`/`coco` as a language (Â§6.3) |
| Copy-code | **Built-in copy button on code blocks** |
| Dark/light theme | **Built-in theme switcher, auto (respects system), light/dark** â€” satisfies both theme + a11y contrast |
| Responsive / mobile | Built-in responsive default theme; sidebar collapses to mobile drawer |
| Accessibility | Default theme is WCAG-conscious (keyboard nav, `prefers-reduced-motion`, focus styles, semantic landmarks); near-100 Lighthouse by default |
| Version info | **Versioning** (snapshot docs per release); plus our own `<VersionBadge/>` component |
| SEO | Built-in meta/OG description, `sitemap.xml` plugin, clean routes, static HTML (great for crawlers) |
| i18n | First-class (we can add languages later without re-architecture) |
| Performance | Static pre-rendered HTML; near-zero JS; fastest build in class |

**Alternative considered and rejected:** hand-rolling a Vite + React/Vue SPA docs app would require building search, theming, sidebar, copy-to-clipboard, syntax highlighting, and SEO ourselves â€” exactly the "unnecessary dependencies" the brief warns against. Docusaurus/Starlight add React/Astro and a bigger footprint for no benefit here. **Decision: VitePress.** (If you truly must avoid Vue, the plan describes the exact shims in Phase 13 â€” but it is not recommended.)

### 1.2 Content model: Markdown-first, structured in two "great basins" per the 2026 DX research
The developer-experience research (APIScout DX Gap 2026, GitBook State of Docs) is unambiguous: docs fail when they give **reference-only** content, and succeed when they lead with a **problem-solving learning journey** then provide **complete, example-rich reference**. So the site has two top-level "modes":

1. **Learn (the journey)** â€” tutorial-style, ordered, each page = one concept, ends in a runnable example. Mirrors the "Rust Book" / "Go Tour" / "Python tutorial" pattern (research confirms these are the gold standard).
2. **Reference (the API)** â€” exhaustive, browsable: Language Reference (from `grammar/coco.ebnf` + lexer keywords) and Standard Library Reference (one page per stdlib module with every `pub def`, signature, and a verified example).

These are separate sidebar trees with distinct URL prefixes (`/learn/` and `/ref/`), so search and navigation don't mix them. A third, smaller **Guides** area (`/guide/`) holds tooling, project structure, comparison, and contributing content.

### 1.3 "Claim Gate" â€” the no-unsupported-claims rule (hard requirement)
Before any page that asserts performance, safety, or "better than X" is merged, it must pass this checklist. It is documented in the repo as `docs/CONTRIBUTING_DOCS.md` and enforced in review (and optionally by a script in Phase 13):

- [ ] **Verifiable:** every code sample actually runs (`coco run`/`coco test`) â€” verified in CI.
- [ ] **Measured, not asserted:** any performance claim has a reproducible benchmark (the repo has `scripts/bench.ps1` + empty `benchmarks/`) and cites the harness â€” never "we are faster than X" without numbers we produced.
- [ ] **Existential:** any stdlib/feature/tool I reference is present in the repo (no networking module, no IDE plugin, no debugger today â†’ they are **"Roadmap"/"FAQ", never "What Coco provides"**).
- [ ] **Sourced:** every fact maps to a source (source file, stdlib module, harness, or public doc), recorded as a comment or a `Sources` footer on the page.

Every phase's content milestones run the Claim Gate on the pages it writes.

### 1.4 Information architecture (site tree)
```
/
â”œâ”€â”€ /                     Landing page (what Coco is, install CTA, logo, links)
â”œâ”€â”€ /learn/               The learning journey (tutorial, ordered)
â”‚   â”œâ”€â”€ introduction      Why Coco, what it is, design goals (honest)
â”‚   â”œâ”€â”€ installation      Install the executable (cross-platform)
â”‚   â”œâ”€â”€ quick-start       "Hello, World" in <5 minutes (P0 per research)
â”‚   â”œâ”€â”€ basics/           variables, types, functions, control flow, matches â€¦
â”‚   â”œâ”€â”€ data/             structs, enums, collections, optionals, results/errors
â”‚   â”œâ”€â”€ beyond/           traits, generics, OOP, concurrency, FFI/unsafe
â”‚   â””â”€â”€ build-project/    a guided end-to-end project (e.g. the word-count capstone)
â”œâ”€â”€ /guide/               Tooling & ecosystem (task-oriented)
â”‚   â”œâ”€â”€ cli/              run, new, build, test, install/add, doc â€¦
â”‚   â”œâ”€â”€ project/          coco.toml, directory layout, modules & imports, packaging
â”‚   â”œâ”€â”€ toolchain/        targets, native vs bytecode, .cob/.cocolib
â”‚   â”œâ”€â”€ comparison/       Coco vs Python / Go / Rust / C / Ruby (honest, Â§1.3)
â”‚   â”œâ”€â”€ faq               FAQ (incl. "what's not built yet")
â”‚   â””â”€â”€ contributing      How to contribute (mirrors README) + docs-writing guide
â”œâ”€â”€ /ref/                 Reference (exhaustive)
â”‚   â”œâ”€â”€ language/         Lexical structure, syntax & keywords, operators, types,
â”‚   â”‚                     statements, expressions, modules â€” from grammar/coco.ebnf
â”‚   â””â”€â”€ stdlib/           core, io, math, strings, collections, json, os, path,
â”‚                         regexp, time, text/slug â€” one page per module
â””â”€â”€ /versions/            (VitePress versioning: current + previous)
```

### 1.5 Repo & data sources mapping (where each page's truth comes from)
| Docs area | Source of truth |
|---|---|
| Learn journey examples | `examples/*.co` (all runnable, verified) + `coco test` harnesses |
| Language Reference | `grammar/coco.ebnf` + `src/lex/lexer.cpp` keywords/operators |
| Stdlib Reference | `stdlib/lib/*.co` (public fn signatures) |
| Project structure | `coco new` scaffold + `coco.toml` manifest |
| Tooling/CLI | `coco --help` output + `tools/*.cpp` usage strings |
| Version | `.version` + `coco.toml` (`version = "0.0.1-beta"`) |
| License/author | `LICENSE.txt` (`MIT Â© 2024 RK Riad Khan`), author info in Â§10 |
| Comparison & roadmap | `docs/COCO_PLAN.md` (vision), `docs/FEATURE_GAP_ANALYSIS.md` (Go/Rust gap) â€” **used as roadmap, not as implemented-feature claims** |

---

## 2. Phase-by-phase implementation roadmap

Each phase has: **Goal Â· What to implement Â· Why it matters Â· Relevant files/components Â· Implementation approach Â· Content requirements Â· Code examples Â· Testing/verification Â· Expected result Â· Issues/trade-offs.**

Phases are ordered so the site is **buildable and deployable from Phase 1** and each phase adds value independently. The full roadmap is Phases 1â€“14 (includes a clean-up/alternatives phase 13 and a maintenance phase 14).

---

### Phase 1 â€” Scaffold the VitePress + TypeScript project, base theme, and brand assets
- **Goal:** a buildable VitePress site at `~/Projects/coco-docs` with the Coco brand, running locally with `npm run docs:dev`.
- **What to implement:**
  - `npm create vite`? No â€” this is a docs site; initialize a VitePress project directly:
    `npx create-vitepress@latest coco-docs --template default` then add TypeScript config.
  - Full project skeleton: `package.json`, `docs/.vitepress/config.ts`, `docs/.vitepress/theme/`, `docs/index.md`.
  - Brand assets dropped into `docs/public/` (logo, favicon) â€” see Â§0.5/Â§2.5.
  - Base `themeConfig`: site title "Coco", nav (Learn/Guide/Reference), sidebar skeleton, footer, logo link, GitHub link.
- **Why:** every later phase builds on a runnable shell; deploying an empty-but-branded site first proves the pipeline and gives a deploy target.
- **Relevant files:**
  ```
  coco-docs/
    package.json
    tsconfig.json
    docs/
      index.md
      .vitepress/
        config.ts          # VitePress config (TypeScript)
        theme/
          index.ts
          style.css
      public/
        logo.svg|png       # Coco logo + favicon
  ```
- **Implementation approach:** use VitePress's TS templates; keep `package.json` minimal (VitePress + Vue are the only runtime deps; everything else is dev tooling). Set up `npm run docs:dev` / `docs:build` / `docs:preview`.
- **Code example (`.vitepress/config.ts`):**
  ```ts
  import { defineConfig } from 'vitepress'

  export default defineConfig({
    title: 'Coco',
    lang: 'en-US',
    description: 'The Coco programming language â€” fast, safe, concurrency-ready.',
    base: '/coco-docs/',          // GitHub Pages path; '/' for user/org pages
    lastUpdated: true,
    cleanUrls: true,
    themeConfig: {
      logo: '/logo.svg',
      nav: [
        { text: 'Learn', link: '/learn/' },
        { text: 'Guide', link: '/guide/' },
        { text: 'Reference', link: '/ref/' },
      ],
      sidebar: { '/learn/': [], '/guide/': [], '/ref/': [] }, // filled in later phases
      search: { provider: 'local' }, // built-in full-text search (MiniSearch)
      socialLinks: [{ icon: 'github', link: 'https://github.com/rkriad585/kirn' }],
      footer: {
        message: 'Released under the MIT License.',
        copyright: 'Â© 2024 RK Riad Khan',
      },
    },
  })
  ```
- **Content requirements:** `index.md` landing page with hero (title, tagline "Python-like syntax Â· Go-style compilation Â· native performance", CTA buttons "Get Started" â†’ `/learn/installation` and "GitHub"), plus the Coco logo and a one-paragraph honest intro.
- **Testing/verification:** `npm run docs:dev` serves; `npm run docs:build` produces static output in `docs/.vitepress/dist/` with no errors; dark/light toggle works; mobile layout renders; Lighthouse â‰¥ 95.
- **Expected result:** a live, branded, deployable site documenting nothing yet but structuring everything.
- **Issues/trade-offs:** branding mismatch risk (see Â§2.5) â€” mitigate by putting all brand values in one `theme` object now. `base` must match the GitHub Pages repo name or assets 404.

---

### Phase 2 â€” Branding system & design tokens
- **Goal:** a single, swappable brand config (name, tagline, colors, logo, favicon, fonts) and a clean CSS variable theme (light + dark).
- **What to implement:**
  - `docs/.vitepress/theme/` with CSS custom properties for brand colors, typography scale, spacing, code accent.
  - A `brand.ts` module exporting `{ name, tagline, colors, logo, repo }` used by components + config.
  - Favicon + OG image.
- **Why:** a docs site's credibility depends on consistent visual identity; and because the "Coco â‡¢ Kirn" rename is in flight (Â§0.5), a **single brand variable** makes the swap trivial and safe.
- **Relevant files:** `.vitepress/theme/index.ts`, `.vitepress/theme/style.css`, `public/favicon.ico`, `public/og.png`.
- **Implementation approach:** derive the palette from the **Coco orange/white/black logo assets** (each logo is exactly one of those). Define one canonical orange as `--coco-brand` and wire it into VitePress's `--vp-c-brand-*` variables so the accent color, buttons, and links all come from it in light and dark mode.
- **Code example (style.css brand tokens):**
  ```css
  :root {
    --coco-brand: #e85d2a;            /* from the orange logo */
    --vp-c-brand-1: var(--coco-brand);
    --vp-c-brand-2: color-mix(in srgb, var(--coco-brand) 80%, white);
    --coco-ink: #1a1a1a;              /* from black logo */
    --coco-paper: #ffffff;            /* from white logo */
  }
  .dark {
    --vp-c-brand-1: color-mix(in srgb, var(--coco-brand) 70%, white);
  }
  ```
  ```ts
  // .vitepress/theme/brand.ts  â€” the single source of truth for the brand
  export const brand = {
    name: 'Coco',                 // â† flip to 'Kirn' when the rename lands
    tagline: 'Python-like syntax, Go-style compilation, native performance.',
    repo: 'https://github.com/rkriad585/kirn',
    logo: '/logo.svg',
    accent: '#e85d2a',
  } as const
  ```
- **Content requirements:** none beyond tokens; ensures visual consistency everywhere.
- **Testing/verification:** both themes pass WCAG AA contrast (orange on white/black); logo renders in header, footer, hero, favicon, OG image; mobile renders.
- **Expected result:** every future page inherits a polished, consistent, swappable identity.
- **Issues/trade-offs:** choosing the orange vs white vs black logo as primary is a taste call; keep all three in `public/` and let `brand.logo` point to the chosen one. Color-mix requires modern browsers (fine for 2026).

---

### Phase 3 â€” Versioning, release metadata, and "last updated"
- **Goal:** every page shows which Coco version it documents, and the site supports version snapshots (VitePress built-in).
- **What to implement:**
  - A `docs/.vitepress/versions.ts` (or read `.version`/`coco.toml` at build time) exposing `current = "0.0.1-beta"`, `releases: []`.
  - A `<VersionBadge/>` Vue component shown in page headers and the footer.
  - VitePress versioning directories (`docs/0.0.1-beta/`) when a second release exists â€” until then, single-version with a badge.
- **Why:** version confusion is a top documentation failure (#3 DX scorecard axis, research-verified). The project's `.version` is currently empty and there's no `--version` flag, so **this phase also identifies a small gap in the compiler** (a `coco version`/`--version` flag is a natural follow-up, listed in the FAQ/roadmap â€” see Â§1.3 existential rule).
- **Relevant files:** `.vitepress/config.ts`, `.vitepress/theme/components/VersionBadge.vue`, `docs/.vitepress/versions.ts`.
- **Implementation approach:** keep one `versions.ts`; wire into the theme via `injections: true` + a computed. Use VitePress's documented versioning layout when a second release appears.
- **Code example (VersionBadge.vue):**
  ```vue
  <script setup lang="ts">
  import { useData } from 'vitepress'
  const { theme } = useData()
  const current = theme.value.currentVersion // '0.0.1-beta'
  </script>
  <template>
    <span class="version-badge" aria-label="Documented for Coco 0.0.1-beta">
      v{{ current }}
    </span>
  </template>
  ```
- **Content requirements:** a `releases.md` or changelog stub listing 0.0.1-beta (first documented release).
- **Testing/verification:** badge renders on all pages; version dropdown appears after a second release snapshot is added; build succeeds.
- **Expected result:** readers always know which version they're reading, and future releases get a clean migration path.
- **Issues/trade-offs:** until the compiler exposes a real version, the site pins the manifest's `0.0.1-beta`. Keep `versions.ts` the single source so a real version release is a 1-line change.

---

### Phase 4 â€” Navigation & information architecture (the two-basin sidebar)
- **Goal:** implement the full nav + sidebar tree from Â§1.2 so the site is navigable even while pages are thin.
- **What to implement:** the three sidebars (`Learn`, `Guide`, `Reference`) with folders and page stubs (each stub = a heading + purpose note + TODO), plus breadcrumbs and prev/next links.
- **Why:** IA is the backbone; a well-formed nav makes partial content coherent and guides the writing effort (per the "Rust Book / Go Tour" pattern).
- **Relevant files:** `.vitepress/config.ts` (sidebar arrays), the `learn/guide/ref` folder structures + stub `index.md` files.
- **Implementation approach:** define sidebar as data (arrays) in config; create empty `index.md` per section with frontmatter `{ title, description, draft: true }`. Enable VitePress's `lastUpdated`, `docFooter { prev, next }`.
- **Code example (sidebar data):**
  ```ts
  referenceSidebar: [
    {
      text: 'Language Reference',
      items: [
        { text: 'Lexical structure', link: '/ref/language/lexical' },
        { text: 'Keywords', link: '/ref/language/keywords' },
        { text: 'Operators', link: '/ref/language/operators' },
        { text: 'Types', link: '/ref/language/types' },
        { text: 'Statements & expressions', link: '/ref/language/statements' },
        { text: 'Modules & imports', link: '/ref/language/modules' },
      ],
    },
    {
      text: 'Standard Library',
      items: [
        { text: 'core', link: '/ref/stdlib/core' },
        { text: 'io', link: '/ref/stdlib/io' },
        { text: 'math', link: '/ref/stdlib/math' },
        { text: 'strings', link: '/ref/stdlib/strings' },
        { text: 'collections', link: '/ref/stdlib/collections' },
        { text: 'json', link: '/ref/stdlib/json' },
        { text: 'os', link: '/ref/stdlib/os' },
        { text: 'path', link: '/ref/stdlib/path' },
        { text: 'regexp', link: '/ref/stdlib/regexp' },
        { text: 'time', link: '/ref/stdlib/time' },
      ],
    },
  ],
  ```
- **Content requirements:** section stub pages only (headings + a sentence + TODO).
- **Testing/verification:** all three sidebars expand/collapse; mobile drawer works; prev/next chain links correctly; no broken internal links (VitePress warns on broken links at build).
- **Expected result:** the full site structure is navigable; writers fill in pages in later phases.
- **Issues/trade-offs:** sidebar data duplication with content files â€” mitigate by keeping links in config and page files lean.

---

### Phase 5 â€” Learning journey part 1: foundations (intro, install, quick start, basics)
- **Goal:** the first genuinely teachable path â€” a reader goes from zero to writing working Coco in minutes, then masters the foundations.
- **What to implement:** the `Introduction`, `Why Coco`, `Installation`, `Quick Start`, and `Basics` pages (variables, data types, functions, control flow, matches).
- **Why:** research ranks a **sub-5-minute getting-started** as the #1 P0 priority; the first-session experience decides adoption.
- **Relevant files:** `docs/learn/introduction.md`, `why.md`, `installation.md`, `quick-start.md`, `basics/{variables,types,functions,control-flow,match}.md`.
- **Implementation approach:** write tutorial-style prose with a **verified, runnable example per page** (pull from `examples/*.co`, which I confirmed run). Every code block is copied verbatim from a file under `examples/` so the "copy-code" button always copies working code. Install page documents the real cross-platform executable install (the README says "Install the cross platform coco executable" â€” document the actual download/install path; if binaries aren't published yet, give the build-from-source steps `cmake -S . -B build && cmake --build build --config Debug` â€” which I verified works).
- **Code example â€” quick start (runnable, verified output):**
  ```coco
  # main.co
  def main() {
      name = "World";
      print("Hello, ", name, "!");
  }
  ```
  ```bash
  $ coco run main.co
  Hello, World!
  ```
- **Content requirements:** each page: concept â†’ minimal example â†’ explanation â†’ a "try it" snippet. Mark `BASIC` level.
- **Testing/verification:** run **every** example in CI against the real compiler (`scripts/runall.ps1` pattern) and assert the documented output matches; Claim Gate pass; Lighthouse â‰¥ 95; search finds "variable", "hello".
- **Expected result:** users succeed in their first session and learn the foundations with runnable code.
- **Issues/trade-offs:** the README's "main.co" uses `let ... = "World"` while current examples use `name = "World"` (immutable default). **Verify exact current syntax before publishing each snippet** (the plan's testing gate catches drift). Prefer `examples/*.co` as canonical snippets over README snippets.

---

### Phase 6 â€” Learning journey part 2: data & control (structs, enums, collections, optionals, results/errors)
- **Goal:** the type-system and data-model middle of the journey.
- **What to implement:** pages for structs/methods, enums + exhaustive `match`, collections (list/dict/set), slices & iterators, optionals/nil-safety, results & `try`/error propagation, `defer`/`panic`.
- **Why:** these are the language's core differentiators (verified working) and map directly to real readability/safety advantages over the pain points users cite (memory-safety, explicit errors, exhaustive handling).
- **Relevant files:** `docs/learn/data/{structs,enums,collections,slices,optionals,results,defer}.md`.
- **Implementation approach:** each page = one concept, one idiomatic verified example, plus a short "how Coco compares" note (honest, Â§1.3). E.g. optionals page demonstrates `T?`, `none`, `.?.`; results page demonstrates `try`/`raise`.
- **Code example â€” optionals (verified construct, mirror `examples/18`):**
  ```coco
  def lookup(key: string, table: dict) -> int? {
      return table.get(key);     # int? or none
  }
  def main() {
      t: dict = { "answer": 42 };
      v = lookup("answer", t);
      print(if v is none { "missing" } else { str(v) });   # safe access
  }
  ```
- **Content requirements:** mark each page `INTERMEDIATE`; include a "common pitfalls" callout (e.g. pointer-vs-value, exhaustive match).
- **Testing/verification:** every snippet runs; output asserted in CI; Claim Gate.
- **Expected result:** learners can model real data and handle errors idiomatically.
- **Issues/trade-offs:** dict/list built-in method names vary (e.g. `get`, `setdefault`, `append`, `extend`) â€” document the **verified** method names by grepping `examples/*.co` and `stdlib/lib/collections.co` rather than guessing.

---

### Phase 7 â€” Learning journey part 3: power features (traits, generics, OOP, concurrency)
- **Goal:** the advanced/differentiator layer of the journey.
- **What to implement:** traits + static/dynamic dispatch, generics + bounds, OOP (`class`/`interface`/`record`/`extends`), concurrency (`spawn`/`chan`/`select`/`join`), FFI + `unsafe`.
- **Why:** this is where Coco's pitch â€” Python ease with compiled concurrency and type abstractions â€” becomes concrete and where wrong claims are most tempting (must pass Â§1.3).
- **Relevant files:** `docs/learn/beyond/{traits,generics,oop,concurrency,ffi}.md`.
- **Implementation approach:** lead each page with a **real, verified example** (e.g. the spawn/channels example from `examples/22` which I ran successfully, and the trait/`impl` pattern from `examples/13`). Position concurrency as "green threads + channels" (verified) â€” **not** "goroutines/async-await/actor model" unless the code supports it.
- **Code example â€” concurrency (verified output, adapted from examples/22):**
  ```coco
  def worker(jobs: chan[int], done: chan[int]) {
      for j in jobs { done.send(j); }
  }
  def main() {
      jobs = chan[int](cap: 8);
      done = chan[int]();
      for _ in 1..=3 { spawn worker(jobs, done); }
      for j in 1..=5 { jobs.send(j); }
      jobs.close();
      for _ in 1..=5 { print(done.recv()); }
  }
  ```
- **Content requirements:** mark `ADVANCED`; add an explicit "Scope / current limits" note per page listing what is **not** yet supported (no async/await, limited stdlib networking, etc.) â€” this directly satisfies "without making unsupported claims."
- **Testing/verification:** full CI run of these examples; concurrency output nondeterminism handled by sorting before asserting (as the repo itself does); Claim Gate.
- **Expected result:** the advanced layer is credible and complete, with honest boundary notes.
- **Issues/trade-offs:** concurrency examples are racy by nature â€” document deterministic assertions (the repo sorts results before comparing). Don't oversell concurrency power.

---

### Phase 8 â€” Guides: tooling, CLI, project structure, packaging, deployment
- **Goal:** task-oriented guidance for everyday usage.
- **What to implement:** CLI reference (`coco run/new/build/test/install/add/doc/â€¦` with real flags from `--help`), project structure (from `coco new` scaffold), modules/imports/visibility, `coco.toml` manifest, targets & native-vs-bytecode (`--target`, `.cob`/`.cocolib`), and the built-in `coco doc` server.
- **Why:** tooling onboarding friction is a top pain point (research Â§pain points: "environment setup & onboarding friction"); documenting real commands solves it.
- **Relevant files:** `docs/guide/{cli,project,toolchain,packaging}.md`.
- **Implementation approach:** one page per tool/task; the CLI page is generated-friendly structured data (flags table) sourced from `coco --help`. Include a "build a CLI binary" and "build a library (.cocolib)" walkthrough with verified commands.
- **Code example â€” CLI table data (from verified `coco --help`):**
  ```bash
  coco run [dir|file]          # run a program or project
  coco build <file.co>         # compile one file -> ./<stem>.exe
  coco test [.|file|dir ...]   # run *_test.co files
  coco build --target=linux-amd64   # cross/portable bytecode (.cob)
  coco build lib               # check + pack -> .cocolib
  coco doc <lib|dir>           # serve markdown docs + API ref
  ```
- **Content requirements:** every command verified; note platform-specific toolchain requirements for native cross-compiles (e.g. `x86_64-w64-mingw32-g++` for Windows cross, `COCO_CXX_LINUX_AMD64` env for Linux native).
- **Testing/verification:** run each documented command in a temp project in CI; capture real flags from `--help` rather than inventing them; Claim Gate.
- **Expected result:** users can install, scaffold, test, build, and package without guessing.
- **Issues/trade-offs:** flags/behavior are documented from the current binary â€” keep the CLI page regenerated on release changes (note in Phase 14 maintenance).

---

### Phase 9 â€” Standard Library reference (exhaustive API)
- **Goal:** a complete, browsable, example-bearing reference for every stdlib module.
- **What to implement:** one page per module (`core, io, math, strings, collections, json, os, path, regexp, time, text/slug`) with a table of each `pub def` (signature + a verified usage example). Also index pages.
- **Why:** the DX research is explicit that **complete reference with real examples** is what turns both humans and AI tools into reliable users. This is the content most likely to be "auto-consumed" â€” accuracy is critical.
- **Relevant files:** `docs/ref/stdlib/{core,io,math,strings,collections,json,os,path,regexp,time}.md`, plus `_test.co` outputs to verify behavior.
- **Implementation approach:** semi-automate: a small extraction script (Phase 13 script or a dev-time Node/PowerShell helper) scans `stdlib/lib/*.co` for `pub def` signatures and emits a skeleton Markdown table per module; a human then adds one verified example per function (using the module's own `*_test.co` as the source of expected behavior).
- **Code example â€” reference page table:**
  ```md
  ## math
  | Function | Signature | Example |
  |----------|-----------|---------|
  | `sqrt` | `sqrt(x: float) -> float` | `sqrt(16.0)` â†’ `4.0` |
  | `abs`  | `abs(x) -> num`            | `abs(-3)` â†’ `3` |
  | `clamp`| `clamp(x: float, lo, hi) -> float` | `clamp(5, 0, 3)` â†’ `3.0` |
  | `min2`/`max2` | `(a, b) -> num`    | `min2(3, 7)` â†’ `3` |
  | `ipow`/`fpow` | `(int|float, exp) -> â€¦`  | `ipow(2, 10)` â†’ `1024` |
  ```
- **Content requirements:** every function is real (from source) and every example runs; module docs cross-link to the Learn pages that use them.
- **Testing/verification:** a script runs each stdlib example and compares output to the documented value; CI asserts; Claim Gate.
- **Expected result:** a trustable, complete standard-library reference.
- **Issues/trade-offs:** the `_test.co` files are the authority for expected behavior but may test internals â€” prefer the `pub` API examples. Some function return types use loose `-> num`/`-> list` (untyped); document them as observed, not reified.

---

### Phase 10 â€” Language reference (from grammar + lexer)
- **Goal:** a normative, exhaustive syntax reference aligned with `grammar/coco.ebnf`.
- **What to implement:** Lexical structure (comments, whitespace, line-join), identifiers, **keywords** (canonical list from lexer), operators/precedence, literals, types, statements, expressions, modules/visibility, function/value semantics.
- **Why:** reference completeness is a P0 per DX research; and the normative EBNF + lexer keywords are the single authoritative source (better than any prose).
- **Relevant files:** `docs/ref/language/*.md`, sourced from `grammar/coco.ebnf` + `src/lex/lexer.cpp`.
- **Implementation approach:** translate each EBNF section into a readable page that starts with a plain-language explanation, shows the EBNF rule in a collapsible block, then gives 1â€“2 verified examples. The **Keywords** page enumerates the exact lexer list (Â§0.4).
- **Code example â€” EBNF â†’ doc:**
  ```md
  ### Conditional (an expression)
  ````ebnf
  if_expr = "if" expr block { "elif" expr block } [ "else" block ]
  ````
  In Coco `if/elif/else` is a **value-yielding expression** (no dangling-else ambiguity):
  ````coco
  let verdict = if grade >= 70 { "pass" } else { "fail" }
  ````
  ```
- **Content requirements:** normative fidelity; a `Language Reference` note that the EBNF is canonical and the pages mirror it.
- **Testing/verification:** every example compiles/runs; cross-check each EBNF nonterminal is represented on a page (coverage script lists grammar nonterminals vs pages); Claim Gate.
- **Expected result:** an authoritative reference that stays in sync with the normative grammar.
- **Issues/trade-offs:** EBNF is detailed but dense â€” don't dump raw EBNF without plain-language framing; mark `[PROVISIONAL]` items from the grammar as provisional on the site to match the spec.

---

### Phase 11 â€” Comparison, FAQ, Roadmap, Contributing (honest positioning)
- **Goal:** answer "why Coco?" honestly via comparisons, address common developer pain points without overclaiming, and document contribution.
- **What to implement:**
  - **Comparison pages** (Coco vs Python/Go/Rust/C/Ruby): focus on **concrete, verifiable differences** (syntax, trailing semicolons, value semantics, native vs interpreted, spawn/channels) â€” using the Claim Gate and never asserting unmeasured performance supremacy.
  - **Pain-point pages**: "Coco for Python developers", "Coco for Rust developers", "Coco for Go developers" â€” map each pain point to a verified Coco feature, or to the roadmap if not yet present.
  - **FAQ** incl. honest "what's not built yet" (networking stdlib, IDE plugin, debugger, JIT, formal ownership borrow-checker).
  - **Contributing** (GitHub workflow from README) + **docs contributing guide** (Claim Gate + style).
- **Why:** the brief explicitly asks to "show how COCO can address relevant problems without making unsupported claims"; honest comparison builds the trust the README's hype erodes.
- **Relevant files:** `docs/guide/comparison/*.md`, `docs/guide/faq.md`, `docs/guide/contributing.md`, `docs/CONTRIBUTING_DOCS.md`.
- **Implementation approach:** a `comparison` page starts from the verified feature table (Â§0.1). For each counterpart language, a short table of "Coco's verified stance" (e.g. "Coco is ahead-of-time compiled â†’ no GIL; Coco has green threads via spawn, verified"). Roadmap items are labeled **"Planned â€” not yet available"**.
- **Code example â€” comparison table:**
  ```md
  | Aspect | Python | Coco (verified) |
  |--------|--------|-----------------|
  | Compilation | Interpreted | AOT (native `.exe` or bytecode `.cob`) |
  | Typing | Dynamic | Inferred static `int`,`float`,`string`,`bool`,generics |
  | Concurrency | GIL + threads | `spawn` green threads + `chan`/`select` |
  | Errors | Exceptions | `try`/`raise`, optionals `?`, results |
  | Memory | GC | Value semantics + arenas/weak refs (no GC observed) |
  ```
- **Content requirements:** every row cited; "planned" clearly separated from "available".
- **Testing/verification:** Claim Gate on every comparison/FAQs statement; a CI script greps the site build for banned unsupported phrases (e.g. "outperforms C/C++", "surpasses Rust") from the README â€” see Phase 13.
- **Expected result:** honest, defensible positioning that increases trust.
- **Issues/trade-offs:** comparison pages are opinion-heavy â€” the Claim Gate is the guardrail; avoid absolute superlatives.

---

### Phase 12 â€” Full-quality pass: a11y, SEO, performance, search tuning, papercuts
- **Goal:** polish the site to production quality on accessibility, SEO, performance, and search.
- **What to implement:**
  - **A11y:** semantic landmarks, keyboard nav audit, focus-visible styles, contrast check (light+dark), `prefers-reduced-motion`.
  - **SEO:** per-page `description`/`og` frontmatter, `sitemap.xml` (VitePress `sitemap` plugin), canonical URLs, `robots.txt`, JSON-LD (Product/SoftwareApplication) on the landing page, OpenGraph image.
  - **Performance:** verify near-zero-JS static pages; lazy-load images; audit with Lighthouse (â‰¥95 across Core Web Vitals).
  - **Search tuning:** ensure local search covers reference + learn; exclude drafts; tune MiniSearch options.
  - **Copy-code:** confirm built-in copy button appears + has aria-label; add "copied" feedback.
- **Why:** research confirms these drive retention and discoverability; Lighthouse-default strength is a selling point for a newcomer language.
- **Relevant files:** `.vitepress/config.ts` (head, sitemap, lastUpdated), `docs/public/robots.txt`, page frontmatter, `.vitepress/theme/`.
- **Implementation approach:** VitePress provides `markdown` frontmatter; add a tiny `head` config for JSON-LD; enable the `sitemap` plugin. Run automated audits (Lighthouse CI) as a gate.
- **Code example â€” config SEO/sitemap:**
  ```ts
  import { defineConfig } from 'vitepress'
  import { sitemap as sitemapPlugin } from 'vitepress-plugin-sitemap'

  export default defineConfig({
    sitemap: { hostname: 'https://kirn-lang.github.io' },
    head: [
      ['meta', { property: 'og:type', content: 'website' }],
      ['meta', { property: 'og:image', content: '/og.png' }],
      ['link', { rel: 'canonical', href: 'https://kirn-lang.github.io' }],
    ],
    markdown: { lineNumbers: true },
  })
  ```
- **Content requirements:** frontmatter `title` + `description` on every page (a lint step verifies).
- **Testing/verification:** Lighthouse CI â‰¥ 95 on home + a learn page + a ref page (light + dark); `axe-core` scan clean; sitemap contains all pages; no 404s on build.
- **Expected result:** a polished, discoverable, accessible site with strong Core Web Vitals.
- **Issues/trade-offs:** dark-mode contrast with the orange accent needs a careful variant (handled in Phase 2); over-tuning search can hide results â€” keep default MiniSearch.

---

### Phase 13 â€” Verification tooling, docs-generation, and (documented) alternatives handy
- **Goal:** make correctness reproducible and the content self-maintaining, and document the non-recommended alternative.
- **What to implement:**
  - A `scripts/` set in `coco-docs`:
    - `run_examples.ps1` â€” runs every code block extracted from the site's Markdown against the real `coco` and asserts documented output (the Claim Gate's mechanical half).
    - `gen_stdlib.ps1` / `gen_lang.ps1` â€” generate reference skeletons from `stdlib/lib/*.co` and `grammar/coco.ebnf` (keeps ref current).
    - `banned_claims.ps1` â€” greps the built `dist` for the unsupported superlatives from Â§0.2 and fails the build if found.
    - `check_links.ps1` â€” internal-link validity (VitePress already warns; here enforced in CI).
  - A `docs/CONTRIBUTING_DOCS.md` encoding the Claim Gate + page template.
  - A section documenting the **non-recommended alternative** (hand-rolled Vite+React SPA) for completeness, with the exact shims (search, theme, copy-button, highlighting, SEO) it would require â€” for any implementer who must avoid Vue.
- **Why:** docs rot silently; the only way to keep "test everything to work perfectly" true for a website is automation.
- **Relevant files:** `coco-docs/scripts/*`, `docs/CONTRIBUTING_DOCS.md`, `docs/ARCHITECTURE_ALTERNATIVES.md`.
- **Implementation approach:** PowerShell scripts mirror the existing repo's `scripts/*.ps1` style (the repo already uses `runall.ps1`/`types.ps1`). Wire them into a `package.json` scripts + a GitHub Actions workflow that (a) builds, (b) runs examples, (c) runs banned-claims, (d) lighthouse.
- **Code example â€” run_examples.ps1 sketch:**
  ```powershell
  # extracts ```coco blocks and runs each; asserts stdout equals the "â†’" annotation
  param([string]$DocsDir, [string]$Coco = "coco")
  foreach ($md in Get-ChildItem -Recurse $DocsDir -Filter *.md) {
    $text = [IO.File]::ReadAllText($md.FullName)
    foreach ($m in [regex]::Matches($text, '```coco\s*\n(.*?)```', 'Singleline')) {
      $file = [IO.Path]::GetTempFileName() + ".co"
      Set-Content -LiteralPath $file -Value $m.Groups[1].Value -Encoding UTF8
      $out = & $Coco run $file 2>&1
      if ($LASTEXITCODE -ne 0) {
        Write-Error "Example failed in $($md.Name): $out"; exit 1
      }
      # compare $out to the annotated `# => expected` line if present
    }
  }
  ```
- **Content requirements:** none (tooling + docs).
- **Testing/verification:** the whole CI pipeline passes end-to-end on a clean machine; a deliberately-wrong example fails the build (proving the gate works).
- **Expected result:** content correctness is enforced, and reference pages can be regenerated as the language evolves.
- **Issues/trade-offs:** example extraction via regex can mis-parse nested fences â€” use a note that examples live in `examples/` and are linked, and the script runs those; keep the site's inline examples small and simple.

---

### Phase 14 â€” Deployment, CI, and maintenance playbook
- **Goal:** ship to GitHub Pages, plus a maintenance routine.
- **What to implement:** GitHub Actions workflow that builds and deploys the VitePress `dist` to `gh-pages` (or the Pages deployment), seeded `versions/`, and a maintenance playbook (`docs/MAINTENANCE.md`: how to add a page, run the verification scripts, cut a new version, update stdlib refs).
- **Why:** "documentation is key to adoption" and CI deployment means a reviewer PR can be previewed before publishing (a Class-A docs feature per research).
- **Relevant files:** `.github/workflows/docs.yml`, `docs/MAINTENANCE.md`.
- **Implementation approach:** use VitePress's `base` (set in Phase 1) + `actions/deploy-pages` (Pages) or `actions/upload-pages-artifact`. Add a PR "preview" job that builds into an artifact. Release workflow snapshots `/ref` and `/learn` into `versions/<tag>/` when a new Coco version tag appears.
- **Code example â€” docs CI (`docs.yml`):**
  ```yaml
  name: docs
  on:
    push: { branches: [main] }
    pull_request:
  jobs:
    build:
      runs-on: ubuntu-latest
      steps:
        - uses: actions/checkout@v4
        - uses: actions/setup-node@v4
          with: { node-version: 20, cache: npm, cache-dependency-path: docs/package-lock.json }
        - run: npm ci
          working-directory: docs
        - run: npm run docs:build
          working-directory: docs
        - run: ./scripts/run_examples.ps1
          shell: pwsh
        - run: ./scripts/banned_claims.ps1
          shell: pwsh
        - uses: actions/upload-pages-artifact@v3
          with: { path: docs/.vitepress/dist }
    deploy:
      needs: build
      if: github.event_name == 'push'
      permissions:
        pages: write; id-token: write
      environment: github-pages
      steps:
        - uses: actions/deploy-pages@v4
  ```
- **Content requirements:** the maintenance playbook references the phase-by-phase rules.
- **Testing/verification:** run the workflow; deploy to the real `kirn-lang.github.io`/Pages URL; confirm sitemap + search work in production; verify the PR preview.
- **Expected result:** automatic, repeatable, preview-able deployment; a documented hand-off for long-term upkeep.
- **Issues/trade-offs:** `base` must match the Pages repo path (a mismatch breaks assets â€” the single most common VitePress-Pages failure); document that prominently in MAINTENANCE.md.

---

## 3. Project structure (final target, `~/Projects/coco-docs`)
```
coco-docs/
â”œâ”€â”€ package.json                # vitepress, vue (runtime); ts, helpers (dev)
â”œâ”€â”€ tsconfig.json
â”œâ”€â”€ .github/workflows/docs.yml  # CI + Pages deploy
â”œâ”€â”€ docs/
â”‚   â”œâ”€â”€ index.md                # landing
â”‚   â”œâ”€â”€ learn/                  # tutorial journey  (learn/*.md per Phase 5â€“7)
â”‚   â”œâ”€â”€ guide/                  # tooling & ecosystem (Phase 8, 11)
â”‚   â”œâ”€â”€ ref/
â”‚   â”‚   â”œâ”€â”€ language/           # lexical, keywords, operators, types, stmts, modules (Phase 10)
â”‚   â”‚   â””â”€â”€ stdlib/             # core, io, math, strings, collections, json, os, path, regexp, time (Phase 9)
â”‚   â”œâ”€â”€ versions/               # VitePress version snapshots (Phase 3/14)
â”‚   â””â”€â”€ .vitepress/
â”‚       â”œâ”€â”€ config.ts           # all config (nav, sidebars, search, seo, sitemap, base)
â”‚       â””â”€â”€ theme/
â”‚           â”œâ”€â”€ index.ts
â”‚           â”œâ”€â”€ style.css       # design tokens (Phase 2)
â”‚           â”œâ”€â”€ brand.ts        # single source of truth for brand (Phase 2)
â”‚           â””â”€â”€ components/     # VersionBadge, LearnCard, CopyButton(assist)
â”œâ”€â”€ scripts/                    # run_examples, gen_stdlib, gen_lang, banned_claims, check_links (Phase 13)
â””â”€â”€ docs/
    â”œâ”€â”€ CONTRIBUTING_DOCS.md    # Claim Gate + page template (Phase 11/13)
    â”œâ”€â”€ MAINTENANCE.md          # upkeep playbook (Phase 14)
    â””â”€â”€ ARCHITECTURE_ALTERNATIVES.md
```

---

## 4. Search architecture
- **Default:** VitePress **local search** (`search.provider: 'local'`, MiniSearch) â€” in-browser full-text index, fuzzy, no API key, always current with the build, works offline, no external crawl delay (a documented advantage over Algolia DocSearch for OSS, per research). Tune `search.options` to:
  - index Learn + Guide + Reference; **exclude** drafts (`frontmatter.search:false` or `*` drafts).
  - configure `miniSearchOptions` for phrase search + prefix matching so code identifiers and stdlib function names (`read_file`, `chan`, `spawn`) are findable.
- **DocsKeywords index:** VitePress local search supports per-word boosting via frontmatter or a `_other` override if needed â€” not required initially (keep simple).
- **Upgrade path (documented, not implemented):** `vitepress-plugin-pagefind` (build-time, Algolia-like UI) or `vitepress-plugin-search` only if local search proves insufficient â€” do not add in Phase 1.

---

## 5. Theme system
- **Two modes, auto-first:** `color-scheme: light dark` via VitePress's built-in toggle; default follows OS (research: "Dark mode â€” Automatic / respects system" is the 5/5 DX score). CSS custom props from `brand.ts` (Â§2.5) feed `--vp-c-brand-*` so one variable drives accent in both modes.
- **Components:** `<VersionBadge/>` (version, Â§2.3), `<LearnCard/>` (nice-to-have link cards on Learn index), and the built-in `<Badge type="...">`/containers (`::: tip/warning/caution`) for notes.
- **Custom containers for claims:** `::: info` for "verified fact", `::: warning` for "planned / not yet available", `::: tip` for "Best practice" â€” this makes the Claim Gate legible on-page.

---

## 6. Syntax highlighting (Shiki custom language)
VitePress uses Shiki. Register a **custom `coco` language** using the canonical keyword/operator/type lists from Â§0.4, so every ` ```coco ` block and the repo's `.co` files highlight accurately.
- **Where:** `.vitepress/config.ts` â†’ `markdown.shikiSetup`/`markdown.languages` (add `coco`) or a `.vitepress/shiki/coco.mjs` grammar.
- **Implementation:** provide a TextMate-ish grammar: keywords, types, operators, numbers, strings, `#` comments (Coco uses `#` for comments, **not** `//`), and f-string interpolation via `f"..."` / `{...}`.
- **Code example (grammar sketch, `coco.mjs`):**
  ```js
  export default {
    name: 'coco',
    scopeName: 'source.coco',
    patterns: [
      { match: /#.*$/,       name: 'comment.line.number-sign.coco' },
      { match: /\b(def|fn|struct|enum|trait|match|spawn|chan|if|else|for|while|return|try|catch|defer|unsafe|extern|new|none)\b/, name: 'keyword.control.coco' },
      { match: /\b(int|usize|bool|string|float|list|dict|set|any|dynamic)\b/, name: 'storage.type.coco' },
      { begin: /f"/, end: /"/, name: 'string.quoted.double.coco',
        patterns: [{ match: /\{[^}]*\}/, name: 'interpolation.coco' }] },
      { match: /\b\d[\d_]*(\.\d+)?\b/, name: 'constant.numeric.coco' },
    ],
  }
  ```
- **Verification:** visually check that keywords vs operators vs strings render distinctly in both themes.
- **Caveat:** the repo uses `#` for comments throughout; do **not** copy a Python/hash-style grammar assumption â€” confirm against actual `.co` files before finalizing.

---

## 7. Content workflow & quality bar (Claim Gate recap)
- Every page has frontmatter: `title`, `description`, `level` (BASIC/INTERMEDIATE/ADVANCED/REFERENCE), `search` (bool), and, when it asserts anything comparative/performance, a `Sources` footer.
- Every code sample is either copied from an existing, verified `examples/*.co` / stdlib behavior, or wrapped with a `# => expected` annotation that `run_examples.ps1` asserts.
- No page may use: "outperforms", "surpasses", "blazing", "world's fastest", "better than <all>" â€” these are the README's banned superlatives enforced by `banned_claims.ps1`.
- Performance claims require a benchmark from `scripts/bench.ps1` + a link to the numbers.
- Roadmap-only items are marked `::: warning Planned â€” not yet available`.

---

## 8. Deployment strategy (GitHub Pages)
1. Static build output: `docs/.vitepress/dist`.
2. Deploy via `.github/workflows/docs.yml` â†’ GitHub **Actions Pages** (`deploy-pages`) or push to `gh-pages`. Target `https://kirn-lang.github.io` (per the resource list) â€” which implies either the `coco` org Pages or the user Pages with the repo published under it; `base` must be set accordingly (`/` for user/org page, `/coco-docs/` for project page).
3. Optional local preview: `npm run docs:preview` (`vitepress preview`) or copy `dist/` to any static host (Netlify/Cloudflare/Vercel) â€” all same static output.
4. PR preview: the workflow builds every PR into a Pages-preview environment.
5. Versioned deploys: on a Coco release, snapshot `/ref` + `/learn` into `versions/<tag>/` (VitePress versioning) so old docs stay reachable â€” mimics the Class-A multi-version pattern from research.

---

## 9. Branding & the "Coco â‡¢ Kirn" rename â€” how this site stays correct
- The repo's `kirn-logos/` currently ships **`kirn-*` assets**, and the sibling plan (`COCO_TO_KIRN_PLAN.md`) renames the language to **Kirn** and `.co â†’ .kn`. The provided branding URLs point at the `kirn-*` logos.
- **This docs plan is name-agnostic by design:** all identity lives in `.vitepress/theme/brand.ts` (name, tagline, logo, accent) and the `base`/config. Swapping Coco â‡¢ Kirn is:
  1. change `brand.ts.name` (+ tagline),
  2. swap `brand.logo` to the chosen `kirn-*.png`,
  3. global-rename the docs prose `Coco`â†’`Kirn` and `coco`â†’`kirn` (command + `.co`â†’`.kn` in examples),
  4. rerun `run_examples.ps1` + `banned_claims.ps1`.
- A note in `MAINTENANCE.md` flags this as a coordinated change with the rename plan's Phase 12. Until the rename is final, the site documents the language as it is **now** (Coco, `.co`) and is structured so the rebrand is low-risk.
- **Logo choice:** primary = `kirn-logos/kirn-logo.png` (matches accent); `kirn-logos/kirn-icon.png` for dark header, `kirn-logos/kirn-icon-on-light.png` for light paper/footer, banner `kirn-logos/kirn-1280x640.png`. All four live in `public/`.