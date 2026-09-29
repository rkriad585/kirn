# Kirn Standard Library (`stdlib/`)

The standard library ships with Kirn and lives in `stdlib/pet/`. Each module is
written in Kirn (`.kn`) and is accompanied by a matching `*_test.kn` acceptance
suite.

## Modules

| Module | File | Core responsibilities |
|--------|------|------------------------|
| `core` | `core.kn` | Built-in types, optionals/results, primitives, helpers |
| `io` | `io.kn` | Input/output: `print`, `input`, streams |
| `math` | `math.kn` | `sqrt`, `pow`, `floor`, trig, constants, etc. |
| `strings` | `strings.kn` | String building, searching, splitting, transformation |
| `collections` | `collections.kn` | `list`, `dict`, `set`, `table`, iterators, sorting |
| `json` | `json.kn` | JSON parse/serialize |
| `os` | `os.kn` | Environment, filesystem, process helpers |
| `path` | `path.kn` | Path normalization and manipulation |
| `regexp` | `regexp.kn` | Regular expressions |
| `time` | `time.kn` | Clocks, durations, timers |
| `text/slug` | `text/` | String-slug utilities |

## Testing

Each `*_test.kn` runs under `kirn test`. Add a new module by dropping
`name.kn` + `name_test.kn` here and registering it in the build/tooling.

## Conventions

- Public entry points are declared `pub def` (visible to importers); see
  `examples/21_modules_visibility.kn`.
- Keep modules dependency-light and side-effect-free at import time.
