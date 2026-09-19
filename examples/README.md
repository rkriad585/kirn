# Kirn Examples (`examples/`)

These programs are the **normative corpus** for the language: they exercise the
syntax (per `grammar/coco.ebnf`), semantics, and standard library, and are the
ground-truth targets the compiler — and any grammar or tooling change — must
keep working. Numbered files (`n_*.co`) each demonstrate a focused feature;
`native*` and `coco_libs/` cover native and packaged-library scenarios.

| # | File | Exercises |
|---|------|-----------|
| 01 | `01_hello.co` | `def`, `main`, f-strings, format specs |
| 02 | `02_variables.co` | `const`/`var`/`let`, immutability default, type annotations |
| 03 | `03_literals_casts.co` | int widths, hex/bin/oct, raw/byte strings, checked `as` |
| 04 | `04_conditionals.co` | `if`/`elif`/`else` + conditional-expression form |
| 05 | `05_loops_ranges.co` | `..` / `..=`, `while`, `break`/`continue` |
| 06 | `06_functions_params.co` | defaults, variadics, fn-type params, named args |
| 07 | `07_closures_captures.co` | lambdas, nested `def`s, captured state |
| 08 | `08_comprehensions.co` | list comprehensions + generator views |
| 09 | `09_match_guards_ranges.co` | literal / range patterns, guards, wildcard |
| 10 | `10_patterns_destructuring.co` | tuples, named/positional ctor patterns, `is` type tests |
| 11 | `11_structs_methods.co` | field defaults, methods, value-semantics copy |
| 12 | `12_enums_exhaustive.co` | unit/named variants, exhaustive `match` |
| 13 | `13_traits_dispatch.co` | sig-only + default methods, static vs dynamic dispatch |
| 14 | `14_generics_bounds.co` | generic fns/structs, `T is Trait` bounds |
| 15 | `15_operator_overloading.co` | `Add`/`Eq`/`Index` trait lowering to operators |
| 16 | `16_collections_slices.co` | list/dict/set displays, slicing, membership |
| 17 | `17_tuples_swap.co` | tuple types/literals, swap assignment |
| 18 | `18_optionals_nil_safe.co` | `T?`, `none` matching, `.?.` |
| 19 | `19_results_try_propagation.co` | `result[T,E]`, `raise`, `try`/`?` forms |
| 20 | `20_defer_panic.co` | `defer` LIFO, `panic`, catch boundary |
| 21 | `21_modules_visibility.co` | `import`/`from..as`, `pub`, `_` privacy |
| 22 | `22_spawn_channels_join.co` | buffered/unbuffered `chan`, `spawn` handles |
| 23 | `23_select_multiplex.co` | binding arms + `<-` discard arm, timers |
| 24 | `24_ffi_unsafe.co` | `extern defs`, c-strings, `unsafe` blocks |
| 25 | `25_operator_precedence.co` | `**` associativity, `//`, shifts, chained comparisons |
| 26 | `26_iterators_views.co` | custom `Iterator` impl, lazy `map`/`filter` chains |
| 27 | `27_value_semantics_copy.co` | copy vs `new` heap-handle aliasing |
| 28 | `28_weak_references.co` | strong vs weak fields, cycle breaking |
| 29 | `29_arena_allocation.co` | `mem.Arena` bulk alloc/reset pattern |
| 30 | `30_capstone_wordcount.co` | concurrent word-count combining feature sets |
| 31 | `31_advanced_patterns.co` | ranges, guards, `@` aliases, disjoint or-patterns |
| 32 | `32_conventions.co` | `main.co` entry + `pin.co` package initializer (run-once) |
| 34 | `34_batteries.co` | stdlib breadth: json/string/table/list utilities |
| 35 | `35_try_catch.co` | `try { } catch e { }` statement + `try`-expr propagation |
| 36 | `36_oop.co` | `class`/`interface`/`record`/`fn`; `extends`, virtual dispatch, record `==` |
| 37 | `37_dynamic_any.co` | `any`/`dynamic`: dynamic typing + duck-typed calls |
| 38 | `38_patterns_power.co` | slice/`..`/rest patterns, nested `@`, ref `&pat` |
| 39 | `39_builtin_methods.co` | `len`, `repeat`, `contains`, `replace`, `find`, `extend`, `reverse`, `clear`, `setdefault`, etc. |
| 40 | `40_keywords.co` | `None` type, `del`, `pr`, `local`/`global`, `temp`, `bucket` |
| 41 | `41_generators.co` | `yield` + generator functions (`-> gen[T]`), iteration, `filter`/`map`/`collect` |
| 42 | `42_control_goto_gather.co` | `goto`/labeled control and `gather` |
| 43 | `43_nested_constructs.co` | deeply nested compound constructs |
| 44 | `44_block_closures.co` | block-scoped closures |

Plus:

- `native_main.co`, `native_scalar_mix.co` — native-backend scenarios.
- `coco_libs/greet/` — a packaged-library example (`code/pin.co` initializer).

**Rule:** any grammar or semantic change must be validated against this corpus
before merge; expected behavior is documented in each file's header.
