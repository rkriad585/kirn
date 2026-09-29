# Kirn Examples (`examples/`)

These programs are the **normative corpus** for the language: they exercise the
syntax (per `grammar/kirn.ebnf`), semantics, and standard library, and are the
ground-truth targets the compiler — and any grammar or tooling change — must
keep working. Numbered files (`n_*.kn`) each demonstrate a focused feature;
`native*` and `pets/` cover native and packaged-library scenarios.

| #   | File                            | Exercises                                                                                        |
| --- | ------------------------------- | ------------------------------------------------------------------------------------------------ |
| 01  | `01_hello.kn`                   | `def`, `main`, f-strings, format specs                                                           |
| 02  | `02_variables.kn`               | `const`/`var`/`let`, immutability default, type annotations                                      |
| 03  | `03_literals_casts.kn`          | int widths, hex/bin/oct, raw/byte strings, checked `as`                                          |
| 04  | `04_conditionals.kn`            | `if`/`elif`/`else` + conditional-expression form                                                 |
| 05  | `05_loops_ranges.kn`            | `..` / `..=`, `while`, `break`/`continue`                                                        |
| 06  | `06_functions_params.kn`        | defaults, variadics, fn-type params, named args                                                  |
| 07  | `07_closures_captures.kn`       | lambdas, nested `def`s, captured state                                                           |
| 08  | `08_comprehensions.kn`          | list comprehensions + generator views                                                            |
| 09  | `09_match_guards_ranges.kn`     | literal / range patterns, guards, wildcard                                                       |
| 10  | `10_patterns_destructuring.kn`  | tuples, named/positional ctor patterns, `is` type tests                                          |
| 11  | `11_structs_methods.kn`         | field defaults, methods, value-semantics copy                                                    |
| 12  | `12_enums_exhaustive.kn`        | unit/named variants, exhaustive `match`                                                          |
| 13  | `13_traits_dispatch.kn`         | sig-only + default methods, static vs dynamic dispatch                                           |
| 14  | `14_generics_bounds.kn`         | generic fns/structs, `T is Trait` bounds                                                         |
| 15  | `15_operator_overloading.kn`    | `Add`/`Eq`/`Index` trait lowering to operators                                                   |
| 16  | `16_collections_slices.kn`      | list/dict/set displays, slicing, membership                                                      |
| 17  | `17_tuples_swap.kn`             | tuple types/literals, swap assignment                                                            |
| 18  | `18_optionals_nil_safe.kn`      | `T?`, `none` matching, `.?.`                                                                     |
| 19  | `19_results_try_propagation.kn` | `result[T,E]`, `raise`, `try`/`?` forms                                                          |
| 20  | `20_defer_panic.kn`             | `defer` LIFO, `panic`, catch boundary                                                            |
| 21  | `21_modules_visibility.kn`      | `import`/`from..as`, `pub`, `_` privacy                                                          |
| 22  | `22_spawn_channels_join.kn`     | buffered/unbuffered `chan`, `spawn` handles                                                      |
| 23  | `23_select_multiplex.kn`        | binding arms + `<-` discard arm, timers                                                          |
| 24  | `24_ffi_unsafe.kn`              | `extern defs`, c-strings, `unsafe` blocks                                                        |
| 25  | `25_operator_precedence.kn`     | `**` associativity, `//`, shifts, chained comparisons                                            |
| 26  | `26_iterators_views.kn`         | custom `Iterator` impl, lazy `map`/`filter` chains                                               |
| 27  | `27_value_semantics_copy.kn`    | copy vs `new` heap-handle aliasing                                                               |
| 28  | `28_weak_references.kn`         | strong vs weak fields, cycle breaking                                                            |
| 29  | `29_arena_allocation.kn`        | `mem.Arena` bulk alloc/reset pattern                                                             |
| 30  | `30_capstone_wordcount.kn`      | concurrent word-count combining feature sets                                                     |
| 31  | `31_advanced_patterns.kn`       | ranges, guards, `@` aliases, disjoint or-patterns                                                |
| 32  | `32_conventions.kn`             | `main.kn` entry + `pin.kn` package initializer (run-once)                                        |
| 34  | `34_batteries.kn`               | stdlib breadth: json/string/table/list utilities                                                 |
| 35  | `35_try_catch.kn`               | `try { } catch e { }` statement + `try`-expr propagation                                         |
| 36  | `36_oop.kn`                     | `class`/`interface`/`record`/`fn`; `extends`, virtual dispatch, record `==`                      |
| 37  | `37_dynamic_any.kn`             | `any`/`dynamic`: dynamic typing + duck-typed calls                                               |
| 38  | `38_patterns_power.kn`          | slice/`..`/rest patterns, nested `@`, ref `&pat`                                                 |
| 39  | `39_builtin_methods.kn`         | `len`, `repeat`, `contains`, `replace`, `find`, `extend`, `reverse`, `clear`, `setdefault`, etc. |
| 40  | `40_keywords.kn`                | `None` type, `del`, `pr`, `local`/`global`, `temp`, `bucket`                                     |
| 41  | `41_generators.kn`              | `yield` + generator functions (`-> gen[T]`), iteration, `filter`/`map`/`collect`                 |
| 42  | `42_control_goto_gather.kn`     | `goto`/labeled control and `gather`                                                              |
| 43  | `43_nested_constructs.kn`       | deeply nested compound constructs                                                                |
| 44  | `44_block_closures.kn`          | block-scoped closures                                                                            |

Plus:

- `native_main.kn`, `native_scalar_mix.kn` — native-backend scenarios.
- `pets/greet/` — a packaged-library example (`code/pin.kn` initializer).

**Rule:** any grammar or semantic change must be validated against this corpus
before merge; expected behavior is documented in each file's header.
