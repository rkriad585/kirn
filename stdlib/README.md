# Kirn Standard Library (`stdlib/`)

The standard library ships with Kirn and lives in `stdlib/lib/`. Each module is
written in Kirn (`.co`) and is accompanied by a matching `*_test.co` acceptance
suite.

## Modules

| Module | File | Core responsibilities |
|--------|------|------------------------|
| `core` | `core.co` | Built-in types, optionals/results, primitives, helpers |
| `io` | `io.co` | Input/output: `print`, `input`, streams |
| `math` | `math.co` | `sqrt`, `pow`, `floor`, trig, constants, etc. |
| `strings` | `strings.co` | String building, searching, splitting, transformation |
| `collections` | `collections.co` | `list`, `dict`, `set`, `table`, iterators, sorting |
| `json` | `json.co` | JSON parse/serialize |
| `os` | `os.co` | Environment, filesystem, process helpers |
| `path` | `path.co` | Path normalization and manipulation |
| `regexp` | `regexp.co` | Regular expressions |
| `time` | `time.co` | Clocks, durations, timers |
| `text/slug` | `text/` | String-slug utilities |

## Testing

Each `*_test.co` runs under `kirn test`. Add a new module by dropping
`name.co` + `name_test.co` here and registering it in the build/tooling.

## Conventions

- Public entry points are declared `pub def` (visible to importers); see
  `examples/21_modules_visibility.co`.
- Keep modules dependency-light and side-effect-free at import time.
