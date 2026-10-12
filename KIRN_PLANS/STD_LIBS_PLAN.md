# Kirn Standard Library Plan (`STD_LIBS_PLAN.md`)

## Goal

One consolidated **pet** (Kirn standard-library) roadmap: an industry-standard,
batteries-included yet minimal library that is **AI-first** and **faster than
Rust/Go on the common paths**, taking the best of Go's flat stdlib, Rust's
crate/module structure, and Python's batteries — implemented as real Kirn pet
units (`stdlib/pet/*.kn`; the `lib/*` names below are those same units) over the
native substrate.

## Must Follow

- The toolchain build ships **exactly two executables**: `kirn` (driver) and `kirnc` (checker/compiler). No other binaries.
- **No over-engineering** — keep the smallest proven pet set that matches the reference langs; extend an existing pet before adding a new one.
- **AI-first**: every pet's public API must be discoverable and checkable — explicit signatures, doc comments, and a `kirn test` suite (`*_test.kn`).
- **Kirn style**: lowercase width micros (`u8`..`u256`, `i8`..`i256`, `f8`..`f256`), capitalized primitives (`Int Float Bool Str Char BigInt Rational`), lowercase methods (`.bit_length()`), CapCase free builtins/types (`__Sqrt`, `BigInt`, `Str`, `Min`, `Max`).
- Pet internals must **not reimplement the builtin runtime**; performance is measured with `kirn bench` and each hot pet must beat its Go/Rust counterpart on the common path.

## Important Note

The existing structure is kept and remains authoritative: the pet catalogue
(baseline + native substrate), the per-module plans (Phases 0–15), and the test
strategy. Each phase below is a **work unit**, not a rewrite; APIs stay
`import pet.<mod>` / `mod.fn(...)`. Catalogues and per-module plans are preserved. The
default-on serialization framework (serde-style `Serialize`/`Deserialize` traits +
`@[derive(...)]`) that the `json`/`toml`/`yaml`/`cbor` formats plug into is owned by
`POWER_PLAN.md`.

**Status:** Living design document for building Kirn's standard library as real,
importable Kirn source modules under `stdlib/lib/`.

**Goal:** A batteries-included, dogfoodable standard library — written in Kirn —
that (a) gives the language usable breadth now, (b) acts as the shared **substrate**
for the compiler's own tooling (formatter, LSP, docs), and (c) mirrors
the mature module layouts of the four reference interpreters whose sources live in
`C:\Users\rkriad585\Projects\go-rust-ruby-cpython-source-code`:

| Reference | Stdlib root (surveyed)                                                              | Evidence used         |
| --------- | ----------------------------------------------------------------------------------- | --------------------- |
| CPython   | `cpython-main/Lib`, `cpython-main/Modules`                                          | survey in §Appendix A |
| Go        | `go-master/src` (`math`, `os`, `io`, `fmt`, `encoding`, `crypto`, `net`, `sync`, …) | survey in §Appendix B |
| Rust      | `rust-main/library` (`std`, `core`, `alloc`)                                        | survey in §Appendix C |
| Ruby      | `ruby-master` (`lib/*.rb`, `ext/*`)                                                 | survey in §Appendix D |

We borrow **APIs** (exact names, shapes, idioms) from these languages, but implement
everything in **Kirn source** layered over the C++ substrate's runtime
(`src/interp/runtime.cpp`). Modules reuse each other freely.

---

## 0. Where we are today (baseline)

Existing Kirn-written stdlib modules in `stdlib/lib/` (Phase 6 of `DO_FIRST_PLAN.md`):

| Module            | Contents                                                                                                 | Native support it sits on          |
| ----------------- | -------------------------------------------------------------------------------------------------------- | ---------------------------------- |
| `lib.core`        | `read_file`, `write_file`, `i2s`, `c2s`                                                                  | `io.open`, File `read/write/close` |
| `lib.collections` | `HashMap` struct; `split`, `join`, `contains`, `unique`, `shuffle`, `rand_float`                         | runtime dict, list methods         |
| `lib.strings`     | `contains/starts_with/ends_with/replace/split/join/repeat/reverse/pad_left/pad_right/strip`              | string builtins                    |
| `lib.path`        | `join/dirname/basename/extname/stem`                                                                     | pure Kirn (sep `/`)                |
| `lib.regexp`      | glob `match`, `find`, `contains`, `replace`, `split`                                                     | pure Kirn (no native engine)       |
| `lib.io`          | `read_file`, `append_file`                                                                               | `io.open`, File `read/write/close` |
| `lib.os`          | `args`, `getenv`, `exit`                                                                                 | `os.args/getenv/exit`              |
| `lib.json`        | `dumps` (native), `loads` (recursive-descent parser — **known stack-overflow bug on nested containers**) | native `json.dumps`                |
| `lib.math`        | `PI`, `E`, `sqrt`, `abs`, `clamp`, `min2`, `max2`, `ipow`, `fpow`                                        | `sqrt/abs` native                  |
| `lib.time`        | `now`, `now_ms`, `sleep`, `ordinal`                                                                      | `time.now/sleep`                   |

> **Implementation status (dedup/audit note):** the 10 modules in the table above **already exist
> and pass `kirn test`** — they are `[IMPLEMENTED]`, not future work. The phases below that
> re-plan an existing module (Phase 1c `io`, 1d `os`, 1e `path`, 2a `strings`, 3a `math`,
> 3c `time`, 5a `regexp`, 1a `json`) should be read as **"enhance/extend the existing `stdlib/lib/
<mod>.kn`"**, not greenfield builds; each such phase is tagged `[NEEDS-ENHANCE]` at its heading
> with a one-line gap note. The **type** internals for new types (bigint, bytes, collections,
> time/Duration, errors) live in `DATA_TYPE_PLAN.md` (→); the builtin **function names** live in
> `EXP_PLAN.md` (→); this plan owns only the **module packaging/APIs**.

**Native runtime surface (the substrate we build on)** — see `runtime.cpp:738-1094`
and `runtime.cpp:1385-1502`:

- **Free builtins:** `print`, `len`, `sqrt`, `ord`, `chr`, `assert`, `assert_eq`,
  `range`, `panic`, `catch_panic`, `printf`, `strlen`, `str`, `int`, `float`, `bool`,
  `type`, `repr`, `sum`, `min`, `max`, `any`, `all`, `sorted`, `reversed`,
  `enumerate`, `map`, `filter`, `reduce`, `upper`, `lower`, `trim`, `contains`,
  `starts_with`, `ends_with`, `replace`, `split`, `join`.
- **Dict methods:** `keys`, `values`, `items`, `get`, `has/contains_key`, `pop`,
  `remove`, `clear`, `len`. **List methods:** `append`, `len`, `extend`, `pop`,
  `shift`, `next`. **Str methods:** `upper`, `lower`, `to_int`, `trim`, `split`,
  `starts_with/ends_with`, `contains`, `replace`, `chars`, `len`, `index`.
- **Pseudo-modules (native stubs):** `math` (`pi,e,sqrt,sin,cos,tan,floor,ceil,log,
exp,abs`), `time` (`sleep,after,now`), `io` (`open`), `mem` (`Arena`), `json`
  (`dumps`), `text` (`slug`), `os` (`exit,args,getenv`).
- **File value:** `read` (all), `write` (append), `close`. No truncate, no stat, no
  existence probe, no directory listing, no mkdir.
- **Module system:** `import lib.<mod>`; `pub fn`/`pub const`/`pub var` and now
  `pub struct`/`pub enum`/`pub trait` are exported through the file-backed module
  namespace (`runtime.cpp:1244-1316`). Modules bind under the last path segment.
  Module env is a child of `globals_`.

**Known gaps / hazards (all fixed or designed-around in this plan):**

- `json.loads` stack-overflows on nested containers (debugging in progress; see
  §Phase 1).
- `File.write` **appends** only; there is **no truncate** primitive. Any module that
  needs "overwrite" must either get a native `io.write/truncate` primitive (add it)
  or use a temp-file rename.
- No native directory / stat / mkdir / listdir / process / socket / crypto
  primitives. Building these in pure Kirn is impossible without new native FFI
  stubs. **This plan therefore includes a small, targeted "native substrate
  extension" phase** (Phase 0) to expose OS/bytes/network/process/crypto via new
  pseudo-modules, keeping Kirn modules thin wrappers.
- Pseudo-module self-reference hazard: a module named like a pseudo-module
  (`io`, `os`, `time`, `json`, `math`) must capture the pseudo-module member at load
  time (`priv_xxx = time.now;`) because the consumer's import rebinds the shared
  `globals_` name. This convention is already used and MUST be preserved.
- `kirn test` runs every `*_test.kn` as a program (rc 0 = PASS); it auto-adds
  `"./stdlib"` to the import dirs (`tools/kirn.cpp:1369`).

---

## 1. Design principles

1. **Kirn-first.** Modules are _real Kirn source_ in `stdlib/lib/*.kn` with a
   `*_test.kn` companion. Native C++ only where Kirn cannot express it (OS calls,
   sockets, crypto, compression).
2. **Layered.** Thin native substrate → pure-Kirn modules → higher modules compose
   lower ones. Example dependency graph:

```
crypto  ──► bytes/hex/base64  ──► characters/strings
tar     ──► zlib/gzip ──► bytes        os/path ──► io
zip     ──► zlib/io/path
http    ──► socket/tls/io/url
csv     ──► io/strings
toml/json/yaml/args ──► text/strings/collections
time/datetime ──► math
statistics ──► math/random       psutil ──► os/path/sys
```

There must be **no import cycles** (a module must not import one that
transitively imports it); enforce via a `kirn check` cycle guard. 3. **Stable, documented public surface.** Every module has a one-line comment header,
`pub`-exports only, and a `*_test.kn` that is green under `kirn test`. 4. **Naming mirrors the references** where unambiguous (`math.sqrt`, `os.mkdir`,
`json.dumps/loads`, `csv.reader/writer`, `base64.b64encode`, `hashlib.sha256`,
`sqlite3.connect`), but we harmonize to **one canonical form** where the four
languages diverge (e.g. Go `strings.Join(sep, xs)` vs Python `str.join` → we pick
`strings.join(xs, sep)` and keep a thin alias). 5. **Type exports work.** Because Phase 6 allows `pub struct/class/enum/trait`, modules
export types that consumers construct: `collections.HashMap(...)`,
`datetime.Date(...)`, `sqlite3.Connection(...)`. 6. **Errors are catchable.** Use `raise`/`catch e {}` (Phase 1). Modules raise on
domain errors (e.g. `math.sqrt(-1)`), missing files, parse failures. `errors`
module standardizes error values/messages.

---

## 2. Execution model & the native substrate plan (Phase 0)

Most requested modules need OS/`syscalls`/network/crypto/bytes that the current
runtime does **not** expose. To keep the plan honest, Phase 0 extends the C++
runtime's pseudo-modules (the _only_ place native primitives live) so every later
phase is a Kirn file. New native pseudo-modules:

| Pseudo-module         | Native surface (new stubs)                                                                                                                                                                                                                                                      | Used by Kirn modules                                               |
| --------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | ------------------------------------------------------------------ |
| `os` (extend)         | `cwd`, `chdir`, `mkdir`, `mkdir_all`, `rmdir`, `remove`, `rename`, `listdir`, `stat` (→ `os.Stat` map: `size,mtime,is_dir,is_file,mode`), `exist`, `is_dir`, `is_file`, `home`, `tmpdir`, `getpid`, `cpu_count`, `hostname`, `environ` (dict), `getenv/setenv/exit/args` (have) | `os`, `path`, `psutil`, `tempfile`, `fileutils`                    |
| `bytes` (new)         | `Bytes` value + `len`, `get(i)`, `slice`, `concat`, `from_str`, `to_str`, `hex`, `unhex`, `from_base64`, `to_base64`; `Bytes.new`                                                                                                                                               | `bytes`, `base64`, `hex`, `crypto`, `zip`, `tar`, `zlib`, `mem`    |
| `process` (new)       | `spawn(cmd,args,env,cwd)` → `Process` (`.wait`→status, `.stdout`, `.stderr`, `.pid`, `.kill`); one-shot `run(cmd,args)` → `(status,stdout,stderr)`                                                                                                                              | `os.exec`, `sys`, `psutil`                                         |
| `sys` (new)           | `stdin.read_line`, `stdout.write`, `stderr.write`, `argv`, `platform`, `set_recursion_limit`, `get_recursion_limit`, `version`, `exit`                                                                                                                                          | `sys`, `io`, `console`, `log`                                      |
| `socket` (new)        | `TcpListener`, `TcpStream`, `UdpSocket` value types + `bind/listen/accept/connect/send/recv/close/set_timeout/shutdown`; helpers `resolve(host,port)`                                                                                                                           | `socket`, `http`, `smtp`, `tls`                                    |
| `crypto` (new)        | hash update objects: `md5/sha1/sha256/sha512` with `update/hexdigest/digest`; `hmac`; `random_bytes`; `constant_time_eq`; later `aes/rsa` (big-int dependent)                                                                                                                   | `hashlib`, `hmac`, `secrets`, `base64`                             |
| `compress` (new)      | `deflate(bytes)->bytes`, `inflate(bytes)->bytes`, `b64_crc32`; later `gzip` framing in Kirn                                                                                                                                                                                     | `zlib`, `gzip`, `zip`, `tar`                                       |
| `sqlite` (new)        | `connect(path)` → `Db` (`.exec(sql)`, `.query(sql)`→list-of-row-tuples, `.close`); optionally link SQLite                                                                                                                                                                       | `sqlite3`                                                          |
| `keyboard` (new)      | `read_key(blocking)` → key struct, `capture`/`release`                                                                                                                                                                                                                          | `keyboard`                                                         |
| `getch/console` (new) | raw terminal mode, cursor, `getch()`                                                                                                                                                                                                                                            | `console`, `keyboard`                                              |
| `bigint` (new)        | arbitrary-precision integer value + add/sub/mul/div/mod/pow/gcd/`from_str/to_str`/`is_prime`/`mulmod/powmod`                                                                                                                                                                    | `toml`(big ints), `crypto`(RSA/DH), `random.big`, `math.factorial` |
| `time` (extend)       | `local_breakdown(epoch)->map{year,month,day,hour,min,sec,wday,yday}`, `mktime(fields)`, `monotonic`, `now_ns`, `format`                                                                                                                                                         | `time`, `datetime`                                                 |

> **vs.** some designs put everything in C++ builtins; here the native layer stays
> ~15 thin modules so the _policy_, _formatting_, and _logic_ live in Kirn
> (proving the substrate is usable by the compiler's own tooling).

**Why Phase 0 first:** nearly every later phase depends on `os`, `bytes`, `path`,
`io`, `sys` being real. Without truncate/stat/listdir, `os`/`path`/`fileutils`/
`csv`/`zip`/`tar` cannot be written honestly.

---

## Phase 0 — Native substrate extension (`build/native`)

**Goal:** add the pseudo-modules/values of §2 + `io` fill gaps (`File.write` modes
`w/a`, `truncate`), all in `src/interp/runtime.cpp`, with `*_test.kn` smoke tests.
**Must Follow:** native stays ~15 thin pseudo-modules (policy/formatting/logic live
in Kirn pets); register every new pseudo-module in the `installBuiltins` prebind
loop and the checker allowlist; pets never reimplement a builtin.
**Important Note:** the only phase that touches C++ — every later phase is a pure
Kirn pet file; the build still yields exactly `kirn` + `kirnc`.

**Key C++ work items**

- `io.open(path)` → return a `File` value carrying a mode; add `File.write(mode)`
  honoring `"w"` (truncate) vs `"a"` (append); add `File.truncate()`; add `File.seek`.
- Add `os.*`, `bytes.Bytes`, `process.spawn`, `sys.*`, `socket.*`, `crypto.*`,
  `compress.*`, `sqlite.*`, `bigint.*`, `keyboard.*`, `time.*` member dispatch in
  `moduleMember(...)` (`runtime.cpp:1385`).
- For `bigint`, wrap a C bignum or link GMP. For `compress`, link zlib + liblzma +
  bzip2. For `sqlite`, link libsqlite3 (optional compile flag).
- Register new pseudo-module names in `installBuiltins` prebind loop
  (`runtime.cpp:1091`) and in the checker's pseudo-module allowlist
  (`checker.cpp:475`).

```cpp
// runtime.cpp — new os member (illustrative)
if (mod == "os" && name == "mkdir_all")
    return biFn({"path"}, [](std::vector<Value>& a) -> Value {
        std::error_code ec;
        std::filesystem::create_directories(toStr(a[0]), ec);
        return Value::boolean(!ec);
    });
```

**Exit (Phase 0):** all new pseudo-modules emit `*_test.kn` green; existing
`collections/io/os/time/math/json` untouched behavior; `kirn test` recursive green.

```kn
# stdlib/pet/os_smoke_test.kn — exercise the Phase-0 substrate from Kirn
import os;
import io;

fn main() {
    assert_eq(os.mkdir_all("/tmp/kirn_p0"), true);
    f = io.open("/tmp/kirn_p0/x.txt");
    f.write_w("hello");                    # truncate/write mode
    f.close();
    assert_eq(io.read("/tmp/kirn_p0/x.txt"), "hello");
    assert_eq(os.listdir("/tmp/kirn_p0").len(), 1);
}
```

> Reference: Go — flat-stdlib split `os` / `io` / `path/filepath` and the
> `MkdirAll`/`ReadDir`/`Stat` shapes in `go/src/os`; Nim — `pure/os`
> (`Nim/lib/pure/os.nim`) for the `walkDir`/`stat` surface.

**Dependencies:** none (foundation). **Consumed by:** everything downstream.

---

## Phase 1 — Fix `json`, add `errors` + `io` + `os` + `path` real versions

> **Status/dedup:** `lib.json`, `lib.io`, `lib.os`, `lib.path` **already exist** (§0 table).
> **1a `json` = `[NEEDS-ENHANCE]`** (the `loads` recursion bug is real and open). **1c `io`, 1d
> `os`, 1e `path` = `[NEEDS-ENHANCE]`** (extend the existing modules' function set; the pure-Kirn
> `path`/`collections` helpers already cover much of 1e). **1b `errors` = PENDING** (greenfield).

**Goal:** stabilize the substrate layer used by everything else and fix the known
`json.loads` recursion bug.
**Must Follow:** extend the existing `json`/`io`/`os`/`path` pets in place (tagged
`[NEEDS-ENHANCE]`); add no new pseudo-modules here; preserve the `priv_*` self-shadow
capture convention.
**Important Note:** `json` is the only true blocker — the rest are API extensions
over Phase 0.

### 1a. Fix `json.loads` (blocking)

**Goal:** make `json.loads`/`dumps` round-trip nested containers without stack overflow.
**Must Follow:** raise `json: nested too deep` from an explicit depth cap before the
C++ stack dies; keep the parser in Kirn.
**Important Note:** if it is module-load-specific, bisect against `test_p2mod.kn`; do
not move parsing into native code.

The recursive-descent parser currently stack-overflows on nested containers
(`{"a":[1]}`, `[[1]]`). Root-cause verified: **self-mutation DOES propagate across
recursive struct method calls** (top-level and recursive write-back works), so the
parser logic is correct in isolation (`tools/test_p2.kn` parsed `[[1]]` fine) —
meaning the bug is either (i) a stack-depth guard missing (deep nesting), or
(ii) an interaction when the parser runs _loaded as a module_. Action:

- Reproduce minimal module case; if it is depth, add an explicit iteration depth
  cap (raise `json: nested too deep`) before the C++ stack dies.
- If it is module-interaction specific, bisect against `test_p2mod.kn`.

```kirn
# stdlib/lib/json.kn (fix) — depth guard in parse_value
fn parse_value(self) {
    self.depth = self.depth + 1;
    if self.depth > 512 { raise("json: nesting too deep"); }
    self.skip_ws();
    c = self.peek();
    ...
}
```

**Exit:** `json.loads`/`dumps` round-trip **nested** dict/list/str/float/bool/nil;
`json_test.kn` covers `{"a":[1]}`, `[[1]]`, `{"nested":{"k":"v"}}`, escaped strings,
unicode, `NaN`/`inf` optional.

### 1b. `lib.errors`

**Goal:** canonical error values + sentinels shared by every failing pet.
**Must Follow:** predicates/optional getters return `nil`/`?`; contract failures raise;
messages are plain `Str`.
**Important Note:** `errors` shadows no pseudo-module; consumers are `os`, `io`, `path`.

Canonical error values + sentinels (Python `errors`, Go `errors`).

```kirn
pub fn new(msg: string) -> string? { return msg; }
pub const NotFound: string = "not found";
pub const Exists: string = "already exists";
pub const PermissionDenied: string = "permission denied";
pub const InvalidJson: string = "invalid json";
pub fn message(e) -> string { return str(e); }
pub fn is_error(e) -> bool { return e is not nil; }
```

Hmm — Kirn module `errors` shadows nothing; fine. Consumers: `os`, `io`, `path`.

### 1c. `lib.io` (real, over Phase-0 substrate)

**Goal:** real file-I/O pet over the Phase-0 substrate (`read/read_text/read_bytes/read_lines/write/append/exists/is_dir/copy`).
**Must Follow:** capture `io.open` at load into `priv_open` (self-shadow guard); overwrite = truncate mode `w`, append = mode `a`.
**Important Note:** wraps the builtin `File` runtime — no reimplementation of `read/write/close`.

```kirn
priv_open = io.open;          # capture pseudo-module (self-shadow guard)
pub fn read(path: string) -> string { f = priv_open(path); s = f.read(); f.close(); return s; }
pub fn read_text(path) -> string { return read(path); }
pub fn read_bytes(path) -> bytes { ... }          # Phase 0 bytes
pub fn write(path: string, data: string) { f = priv_open(path); f.write_w(data); f.close(); }
pub fn append(path: string, data: string) { f = priv_open(path); f.write_a(data); f.close(); }
pub fn read_lines(path: string) -> list { return read(path).split("\n"); }
pub fn write_lines(path: string, lines: list) { write(path, strings.join(lines, "\n") + "\n"); }
pub fn exists(path) -> bool { return os.exist(path); }     # Phase 0
pub fn is_dir(path) -> bool { return os.is_dir(path); }
pub fn copy(src, dst) { write(dst, read(src)); }
```

### 1d. `lib.os` (real, over Phase-0 `os`)

**Goal:** OS/filesystem/process/FS pet over Phase-0 `os` + `process`.
**Must Follow:** one canonical form per op (`mkdir(p, parents=true)`, `env(name, default=nil)`); no import cycles with `path`/`io`.
**Important Note:** `walk` is Python-style `[(dir,[subdirs],[files])]`; `system` delegates to `process.run`.

```kirn
pub fn getcwd() -> string { return os.cwd(); }
pub fn chdir(p) { os.chdir(p); }
pub fn mkdir(p, parents=true) -> bool { if parents { return os.mkdir_all(p); } return os.mkdir(p); }
pub fn rmdir(p) { os.rmdir(p); }
pub fn remove(p) { os.remove(p); }
pub fn rename(src, dst) { os.rename(src, dst); }
pub fn listdir(p=".") -> list { return os.listdir(p); }
pub fn walk(root) -> list { ... }            # [(dir,[subdirs],[files]), ...], Python-style
pub fn env(name, default=nil) { v = os.getenv(name); if v is nil { return default; } return v; }
pub fn environ() -> dict { return os.environ(); }
pub fn setenv(k, v) { os.setenv(k, v); }
pub fn getpid() -> int { return os.getpid(); }
pub fn cpu_count() -> int { return os.cpu_count(); }
pub fn hostname() -> string { return os.hostname(); }
pub fn system(cmd: string) -> int { return process.run(cmd).status; }   # Phase 0 process
pub const linesep: string = "\n";              # platform-adjusted in Phase 0
pub const sep: string = "/";
```

`lib.os.exec`: `run(cmd, args=[])`, `spawn`, `output`, `look_path`.

### 1e. `lib.path` (real, platform separator + glob)

**Goal:** platform-correct path pet (separator + glob) matching Go `path/filepath`.
**Must Follow:** honor `os.sep()`; `glob` supports `* ? [] **`; keep pure Kirn (no native dep).
**Important Note:** extends the existing pure-Kirn `path`; here `abs(p)` is the path function, **not** the numeric `__Abs` builtin.

```kirn
pub const sep: string = os.sep();
pub fn join(parts: list) -> string { ... }       # honor sep on windows ('\\')
pub fn abs(p) -> string { return os.realpath(p); }
pub fn norm(p) -> string { ... }                 # collapse '..' '.'
pub fn is_abs(p) -> bool { ... }
pub fn basename(p), dirname(p), extname(p), stem(p) -> string  { ... }
pub fn glob(pattern) -> list { ... }             # '*','?','[]','**'
pub fn exists(p) -> bool { return os.exist(p); }
pub fn expanduser(p) -> string { ... }           # '~' → home
```

Adds `rel`, `commonpath`, `clean` (Go `path/filepath`), `split_drive`.

**Exit:** `json_test`(with nested), `errors_test`, `io_test`, `os_test`, `path_test`
green. **Consumes:** Phase 0. **Consumed by:** all later phases.

---

## Phase 2 — Strings, characters, collections breadth

> **Status/dedup:** **2a `strings` = `[DONE]`** (the listed function set already ships in
> `stdlib/lib/strings.kn` — extend only where new names are added). **2b `characters` = PENDING.**
> **2c `collections` = `[NEEDS-ENHANCE]`** (the existing `collections.kn` has `HashMap` + helper
> fns; the deque/Counter/ordered-collections TYPES are pending — type model in `DATA_TYPE_PLAN.md`
> Phase 10, module here). **2d/2e `itertools`/sort = PENDING.**

**Goal:** rich text + data-structure pets used by nearly every other pet.
**Must Follow:** extend `strings`/`collections` in place; new types (`Deque`/`Counter`) package here while their type model lives in `DATA_TYPE_PLAN.md` Phase 10.
**Important Note:** `strings` and `collections` are hot-path — `kirn bench` must show them beating Go `strings`/`container` on common ops.

### 2a. `lib.strings` (extend; keep existing names)

**Goal:** complete the Python/Go/Ruby-convergent string set in one pet.
**Must Follow:** keep existing names stable; add only harmonized names; `join(xs, sep)` (Go is the odd one out).
**Important Note:** **performance-critical** — `contains`/`index`/`split`/`replace` must beat Go `strings` on the common path (delegate to native builtins; no per-char Kirn scans).

> Reference: Nim — `strutils` (`Nim/lib/pure/strutils.nim`) for `find/rfind/startsWith/split` naming; Odin — `core:strings` (`Odin/core/strings`).

Add (Python/Go/Ruby convergent set):

```kirn
pub fn index(s, sub) -> int { return s.index(sub); }          # -1 if absent / Python
pub fn rindex(s, sub) -> int { ... }
pub fn find_all(s, sub) -> list { ... }                       # all start indices
pub fn count(s, sub) -> int { ... }
pub fn capitalize(s) -> string { ... }
pub fn title(s) -> string { ... }                             # Go strings.Title
pub fn swapcase(s) -> string { ... }
pub fn fields(s) -> list { ... }                              # split on whitespace (Go Fields)
pub fn split_whitespace(s) -> list { return fields(s); }
pub fn strip_prefix(s, pre) -> string? { ... }                # Rust strip_prefix
pub fn strip_suffix(s, suf) -> string? { ... }
pub fn byte_len(s) -> int { return s.len(); }
pub fn char_count(s) -> int { return s.chars().len(); }
pub fn sub(s, start, end) -> string { return s[start..end]; } # Go substring
pub fn index_of_char(s, c) -> int { ... }
pub fn is_whitespace(s) -> bool { return s.trim() == ""; }
pub fn lower(s), upper(s) -> string                        # aliases to builtins
pub fn format(fmt: string, args: list) -> string { ... }     # printf → string (Go fmt.Sprintf)
```

Conversions to/from bytes live in `lib.bytes`.

### 2b. `lib.characters` (new) — Unicode classification

**Goal:** Unicode classification/conversion helpers (Python `str.is*`, Rust `char::is_*`).
**Must Follow:** delegate to native `ord`/`chr`; predicate names mirror Python (`is_digit`, `is_alpha`).
**Important Note:** distinct from `strings` — operates on single `Char`s, not whole `Str`s.

```kirn
pub fn is_digit(c) -> bool { ... }       # '0'..'9'
pub fn is_alpha(c) -> bool { ... }
pub fn is_alnum(c) -> bool { ... }
pub fn is_space(c) -> bool { ... }
pub fn is_upper(c)/is_lower(c) -> bool { ... }
pub fn to_upper(c)/to_lower(c) -> string { ... }
pub fn ord(c) -> int { return ord(c); }
pub fn chr(n) -> string { return chr(n); }
```

(Python `str.isdigit`/`isalpha`; Rust `char::is_*`.)

### 2c. `lib.collections` (extend with `HashSet`, `Deque`, `Counter`, `SortedList`)

**Goal:** `HashSet`/`Deque`/`Counter` (+ helper fns) alongside the existing `HashMap`.
**Must Follow:** reuse the runtime dict/list; types exported via `pub struct`; no native reimplementation.
**Important Note:** type internals belong to `DATA_TYPE_PLAN.md` Phase 10; this pet owns packaging/APIs only.

> Reference: Rust — `std::collections` (`rust/library/std`) for `VecDeque`/`HashSet` op names.

```kirn
pub struct HashSet {
    var buckets: dict;
    fn add(self, v) { self.buckets[str(v)] = true; }
    fn has(self, v) -> bool { return str(v) in self.buckets; }
    fn remove(self, v) { if str(v) in self.buckets { self.buckets.remove(str(v)); } }
    fn size(self) -> int { return self.buckets.len(); }
    fn to_list(self) -> list { return self.buckets.keys(); }
    fn union(self, o), intersection(self, o), difference(self, o) -> HashSet { ... }
}
pub struct Deque {                                   # Python collections.deque
    var items: list;
    var head: int;
    fn append(self, v) { self.items.append(v); }
    fn append_left(self, v) { ... }
    fn pop(self) { ... } fn popleft(self) { ... }
    fn rotate(self, n) { ... }
    fn len(self) -> int { ... } fn get(self, i) { ... }
}
pub struct Counter {                                 # Python Counter
    var counts: dict;
    fn add(self, v) { ... } fn get(self, v) { ... }
    fn most_common(self, n) -> list { ... }         # [(k,count), ...]
    fn total(self) -> int { ... }
}
pub fn chain(xs, ys) -> list, flatten(ls) -> list, range_step(a,b,step) -> list { ... }
pub fn zip2(a, b) -> list { ... }                   # list of 2-tuples (Rust/Go iter.zip)
pub fn dedup(xs) -> list { ... }
pub fn binary_search(xs, v) -> int { ... }          # index or -1 (Go sort.Search)
pub fn partition(xs, pred) -> (list, list) { ... }
```

### 2d. `lib.itertools` (new; Python itertools + Rust Iterator adapters)

**Goal:** Python-`itertools` + Rust `Iterator` adapters as list-returning helpers.
**Must Follow:** eager list results (no lazy iterators yet); names match Python.
**Important Note:** composes `collections`; low-allocation loops on hot paths.

> Reference: Rust — `Iterator` adapters (`rust/library/core/src/iter`) for `zip/chain/take/drop/tee` semantics.

```kirn
pub fn chain(xs) -> list, product(xs) -> list, permutations(xs, r) -> list,
       combinations(xs, r) -> list, cycle(xs) -> list, repeat(v, n) -> list,
       accumulate(xs) -> list, groupby(xs, key) -> list, pairwise(xs) -> list,
       starmap(f, xs) -> list, count(start, step) -> list, islice(xs, a, b) -> list,
       zip_longest(a, b) -> list, take(n, xs) -> list, drop(n, xs) -> list,
       takewhile(pred, xs), dropwhile(pred, xs), tee(xs) -> (list, list)
```

### 2e. `lib.sort` (new; Go sort + Rust slice::sort)

**Goal:** sorting/searching pet over the native `sorted`/`reversed` builtins.
**Must Follow:** `sort_by` takes a key function; `search` returns the insertion point (Go `sort.Search`).
**Important Note:** stable variants must be stable; `kirn bench` vs Go `sort`/`slices`.

```kirn
pub fn sort(xs: list) -> list { return sorted(xs); }
pub fn sort_by(xs, key) -> list { ... }             # key = function
pub fn search(xs, v) -> int { ... }                 # Go sort.Search → insertion point
pub fn is_sorted(xs) -> bool { ... }
pub fn reverse(xs) -> list { return reversed(xs); }
pub fn stable_sort(xs) -> list { ... }
```

**Exit:** `strings_test`, `characters_test`, `collections_test`, `itertools_test`,
`sort_test` green. **Consumes:** Phase 1 (`os` not needed here). **Consumed by:**
`csv`, `regexp`, `path`, `args`, `toml`, `html`, `template`, nearly everything.

---

## Phase 3 — Time, DateTime, math, random, statistics

> **Status/dedup:** **3a `math` and 3c `time` = `[NEEDS-ENHANCE]`** (extend the existing
> `stdlib/lib/math.kn` and `time.kn`). The **builtin names** for math/random are owned by
> `EXP_PLAN.md` Phases 1 & 15 (→); the **float/time TYPES** by `DATA_TYPE_PLAN.md` Phases 2 & 13
> (→). **3b `random`, 3d `datetime`, 3e `statistics` = PENDING** (greenfield modules).

**Goal:** numeric + temporal modules (Python `datetime/time/math/random/statistics`,
Go `time/math/rand`, Rust `std::time/f64`, Ruby `Time/Date/Math`).
**Must Follow:** builtin names owned by `EXP_PLAN.md` (Phases 1 & 15), float/time types by
`DATA_TYPE_PLAN.md` (Phases 2 & 13); use qualified `math.*` to avoid pseudo-module shadowing.
**Important Note:** `math`/`random` hot loops must beat Go `math/rand` on the common path;
CapCase renames apply here — see 3a (`abs` → `__Abs`, `rational` → `Rational`/`__Rational`).

### 3a. `lib.math` (extend to full reference surface)

**Goal:** full reference math surface (constants + fns) in one pet, matching Python/Go/Rust names.
**Must Follow:** qualified `math.*`; capture `priv_sqrt = __Sqrt` at load where a global is needed; `Float` results throughout.
**Important Note:** **CapCase rename** — the old lowercase `abs(` builtin is now `__Abs` (the pet wrapper stays `math.fabs`), and `rational(` is now `Rational(...)`/`__Rational(...)`; methods stay lowercase (`.bit_length()`). `kirn bench` must match or beat Go `math`.

Import with `import lib.math;` uses **qualified** `math.sqrt(...)` so it does not
shadow the native `math` pseudo-module; no self-shadow hazard.

```kirn
pub const PI: float = 3.1415926535897932384;
pub const E: float = 2.7182818284590452353;
pub const TAU: float = 6.2831853071795864769;
pub fn-ish ...
pub fn sqrt(x) -> float { ... }        # have (Newton)
pub fn cbrt(x) -> float { ... }
pub fn pow(base, exp) -> float { ... } # fpow + float result for float exp
pub fn exp(x)/exp2(x)/expm1(x) -> float { ... }
pub fn log(x)/log2(x)/log10(x)/log1p(x) -> float { ... }
pub fn sin/cos/tan(x) -> float { if x < 0 { x = x + TAU; } ... }  # native math.sin
pub fn asin/acos/atan(x), atan2(y, x) -> float { ... }
pub fn sinh/cosh/tanh(x), asinh/acosh/atanh(x) -> float { ... }
pub fn hypot(a, b) -> float { return sqrt(a*a + b*b); }
pub fn floor/ceil/trunc/round(x) -> int { ... }
pub fn fabs(x) -> float { return abs(x); }
pub fn copysign(x, y) -> float { ... }
pub fn fmod(a, b) -> float { ... }
pub fn isclose(a, b) -> bool { d = a - b; if d < 0 { d = -d; } return d < 1e-9; }
pub fn isnan(x) -> bool { return x != x; }
pub fn isinf(x) -> bool { return x == inf or x == -inf; }
pub fn isfinite(x) -> bool { return not (isnan(x) or isinf(x)); }
pub fn degrees(r) -> float { return r * 180.0 / PI; }
pub fn radians(d) -> float { return d * PI / 180.0; }
pub fn gcd(a, b) -> int { while b != 0 { t = b; b = a % b; a = t; } return abs(a); }
pub fn lcm(a, b) -> int { if a == 0 or b == 0 { return 0; } return abs(a / gcd(a, b) * b); }
pub fn factorial(n) -> int { r = 1; i = 2; while i <= n { r = r * i; i = i + 1; } return r; }   # big via bigint later
pub fn comb(n, k) -> int { ... }
pub fn clamp(x, lo, hi) -> float { ... }   # have
pub fn min2/max2(a, b) { ... }             # have
pub fn fsum(xs) -> float { s = 0.0; for x in xs { s = s + float(x); } return s; }
```

> `math` free functions overlap with global builtins (`sqrt`, `abs`). Keep the
> module-qualified forms (`math.sqrt`) and, where a module-internal use needs the
> global, capture at load: `priv_sqrt = __Sqrt;`.
> Reference: Go — `math` package (bit-exact constants, `Sqrt/Log/Exp/Sin/Cos`) in
> `go/src/math`; Rust — `f64` methods + `PI/TAU/E` consts in `rust/library/core`.

### 3b. `lib.random` (new; Python `random`, Go `math/rand`, Ruby `Random`)

**Goal:** deterministic PRNG + Python-`random`/Go-`math/rand` API.
**Must Follow:** seed entropy from Phase-0 `crypto.random_bytes`; PRNG (xorshift128+ / PCG) in Kirn — never reimplement the builtin RNG.
**Important Note:** expose a module-level default `Rng`; `kirn bench` must beat Go `math/rand` on bulk draws.

> Reference: Go — `math/rand` (`Seed/Intn/Shuffle/NormFloat64`) in `go/src/math/rand`; Ruby — `Random`/`Random.urandom` (`ruby/lib/securerandom.rb`); V — module-level RNG API in `v/vlib/rand`.

Needs a **PRNG**. Phase 0 `crypto.random_bytes` gives entropy seeds; implement
deterministic PRNG in Kirn (Mersenne-Twister or xorshift128+ / PCG).

```kirn
pub struct Rng {
    var state: int;
    fn init(self, seed: int) { self.state = seed; }
    fn next(self) -> int { ... }                 # 32-bit value from the PRNG
    fn random(self) -> float { return float(self.next() % 100000) / 100000.0; }  # [0,1)
    fn randrange(self, lo, hi) -> int { ... }
    fn choice(self, xs) { ... }
    fn shuffle(self, xs) -> list { ... }
    fn sample(self, xs, k) -> list { ... }
    fn gauss(self) -> float { ... }              # Box-Muller
}
pub fn seed(n) { ... }
pub fn random() -> float { ... }                 # module-level default Rng
pub fn randint(a, b) -> int { ... }
pub fn choice(xs) { ... }
pub fn shuffle(xs) -> list { ... }
```

### 3c. `lib.time` (extend; Python `time`, Rust `std::time`, Go `time`)

**Goal:** extend the existing `time` pet with breakdown/monotonic/format helpers.
**Must Follow:** capture pseudo `time.*` at load (`priv_*`); formatting/locale logic stays in Kirn.
**Important Note:** `now_ns`/`monotonic`/`local_breakdown` come from Phase 0; no wall-clock logic in C++.

> Reference: Rust — `std::time` `Instant/Duration` (`rust/library/std/src/time.rs`).

```kirn
pub fn now() -> float { return float(time.now()) / 1000.0; }   # have
pub fn now_ms() -> int { return time.now(); }                   # have
pub fn monotonic() -> float { return time.monotonic(); }        # Phase 0
pub fn localtime() -> dict { return time.local_breakdown(time.now()); }  # Phase 0: {year,month,day,hour,min,sec,...}
pub fn gmtime() -> dict { ... }
pub fn mktime(fields: dict) -> int { return time.mktime(fields); }
pub fn strftime(fmt: string, t) -> string { ... }               # %Y %m %d %H %M %S map + apply
fn pad2(n) -> string { if n < 10 { return "0" + str(n); } return str(n); }
pub fn format_iso(t) -> string { f = localtime(); return str(f.year) + "-" + pad2(f.month) + "-" + pad2(f.day); }
```

Keep `sleep`, `ordinal`.

### 3d. `lib.datetime` (new; Python `datetime`, Ruby `Date/DateTime`, Go `time.Date`)

**Goal:** `Date`/`DateTime`/`Duration` value types (Python/Ruby/Go).
**Must Follow:** civil-date math in Kirn; epoch conversions delegate to Phase-0 `time`.
**Important Note:** `Duration` names mirror Rust `std::time::Duration`; `strftime` handled via `time`.

> Reference: CPython — `Lib/datetime.py` (`date`/`datetime`/`timedelta` algorithms).

```kirn
pub struct Date {
    var year: int; var month: int; var day: int;
    fn init(self, y, m, d) { self.year = y; self.month = m; self.day = d; }
    fn is_leap(self) -> bool { y = self.year; return (y % 4 == 0 and y % 100 != 0) or y % 400 == 0; }
    fn days_in_month(self) -> int { ... }
    fn weekday(self) -> int { ... }            # 0=Mon..6=Sun (Python weekday)
    fn to_ordinal(self) -> int { ... }         # days since 0001-01-01
    fn add_days(self, n) -> Date { ... }
    fn iso(self) -> string { return str(self.year) + "-" + pad2(self.month) + "-" + pad2(self.day); }
    fn strftime(self, fmt) -> string { ... }
}
pub fn today() -> Date { f = time.local_breakdown(time.now()); return Date(f.year, f.month, f.day); }
pub fn parse(s) -> Date { ... }                # YYYY-MM-DD
pub fn from_ordinal(n) -> Date { ... }
pub fn add_months(d, n) -> Date { ... }

pub struct DateTime {                            # date + time + tz offset minutes
    var year, month, day, hour, min, sec, tz_min: int;
    ...
    fn to_epoch(self) -> int { ... }
    fn iso(self) -> string { return self.date_iso() + "T" + self.time_iso(); }
}
pub fn now() -> DateTime { ... }
pub fn from_epoch(sec) -> DateTime { ... }

pub struct Duration {                            # Rust std::time::Duration
    var secs: int; var nanos: int;
    fn from_secs(s) -> Duration { return Duration(s, 0); }
    fn from_millis(ms) -> Duration { return Duration(ms / 1000, (ms % 1000) * 1000000); }
    fn as_secs(self) -> int { ... } fn as_millis(self) -> int { ... } fn as_nanos(self) -> int { ... }
    fn add(self, o) -> Duration { ... } fn sub(self, o) -> Duration { ... }
}
```

### 3e. `lib.statistics` (new; Python `statistics`)

**Goal:** descriptive statistics (`mean/median/mode/stdev/quantiles/corr`).
**Must Follow:** reuse `sort` + `collections.Counter`; accumulate via `math.fsum`.
**Important Note:** match Python `statistics` semantics; deterministic and allocation-light.

> Reference: CPython — `statistics` (pure-Python module; same algorithm set).

```kirn
pub fn mean(xs) -> float { s = 0.0; for x in xs { s = s + float(x); } return s / float(xs.len()); }
pub fn median(xs) -> float { xs2 = sorted(xs); n = xs2.len(); if n % 2 == 1 { return float(xs2[n / 2]); } return (float(xs2[n/2-1]) + float(xs2[n/2])) / 2.0; }
pub fn mode(xs) { m = collections.Counter(); for x in xs { m.add(x); } return m.most_common(1)[0][0]; }
pub fn stdev(xs) -> float { ... }
pub fn variance(xs) -> float { ... }
pub fn quantiles(xs, n=4) -> list { ... }
pub fn cov(xs, ys), corr(xs, ys) -> float { ... }
pub fn linear_regression(xs, ys) -> (float, float) { ... }
```

**Exit:** `math_test`, `random_test`, `time_test`, `datetime_test`, `statistics_test`
green. **Consumes:** Phase 0 (time breakdown), Phase 2 (collections), `math` core.
**Consumed by:** `psutil`, `benchmark`, `args`, `http` (date headers), `csv`.

---

## Phase 4 — bytes, encoding (base64/hex), hashlib/hmac/secrets, crypto

**Goal:** binary + integrity/security primitives (Python `base64/hashlib/hmac/
secrets/binascii`, Go `encoding/base64/encoding/hex/hash/crypto`, Ruby
`Base64/Digest/SecureRandom`).
**Must Follow:** all hashing/crypto primitives come from Phase-0 pseudo-modules; pets are thin wrappers + policy.
**Important Note:** **performance-critical** — `base64`/`hex`/sha256 must beat Go
`encoding/base64`/`crypto/sha256` on the common path (`kirn bench`).

### 4a. `lib.bytes` (new)

**Goal:** bytes pet wrapping the Phase-0 `Bytes` value.
**Must Follow:** no byte loops in Kirn where a native `Bytes` op exists; str↔bytes conversions are explicit.
**Important Note:** Phase 0 must land first; every later binary pet builds on this.

```kirn
pub fn new(cap: int=0) -> bytes { return bytes.new(cap); }
pub fn from_str(s: string) -> bytes { return bytes.from_str(s); }
pub fn from_list(xs: list) -> bytes { ... }
pub fn to_str(b: bytes) -> string { return b.to_str(); }
pub fn to_list(b: bytes) -> list { ... }
pub fn len(b: bytes) -> int { return b.len(); }
pub fn concat(a: bytes, b: bytes) -> bytes { return bytes.concat(a, b); }
pub fn slice(b: bytes, i, j) -> bytes { return b.slice(i, j); }
pub fn get(b: bytes, i) -> int { return b.get(i); }
pub fn set(b: bytes, i, v) { b.set(i, v); }
pub fn equals(a, b) -> bool { ... }
```

### 4b. `lib.base64` (new; Python `base64`, Go `encoding/base64`, Ruby `Base64`)

**Goal:** base64 encode/decode (std + urlsafe) matching Python `base64`/Go `encoding/base64`.
**Must Follow:** operate on `bytes`; string convenience wrappers; alphabet tables are constants.
**Important Note:** **must beat Go `encoding/base64`** — native bulk path, never per-char Kirn.

> Reference: Go — `encoding/base64` (`EncodeToString`/`DecodeString`) in `go/src/encoding/base64`; CPython — `Lib/base64.py`.

```kirn
const ALPHABET: string = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
const URL_ALPHABET: string = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
pub fn b64encode(data: string, urlsafe=false) -> string { ... }   # uses bytes internally
pub fn b64decode(data: string, urlsafe=false) -> string { ... }
pub fn hex(a: string) -> string { return bytes.from_str(a).hex(); }   # ≠ hex module
```

> Keep these **pure-Kirn string algorithms** (no Phase-0 bytes needed) so they float
> independently; bytes-typed variants in Phase 8 once `bytes` is stable. Actually to
> avoid duplication: `b64encode` operates on `bytes`; provide string convenience.

### 4c. `lib.hex` (new; Python `binascii.hexlify`, Ruby `String#unpack1('H*')`)

**Goal:** hex encode/decode over `bytes`.
**Must Follow:** delegate to native `Bytes.hex`/`unhex`; str convenience wrappers.
**Important Note:** distinct from `base64.hex`; canonical byte order (Python `binascii`, Ruby `H*`).

```kirn
pub fn encode(b: bytes) -> string { return b.hex(); }
pub fn decode(s: string) -> bytes { return bytes.unhex(s); }
pub fn encode_str(s: string) -> string { return bytes.from_str(s).hex(); }
pub fn decode_str(s: string) -> string { return bytes.unhex(s).to_str(); }
```

### 4d. `lib.hashlib` (new; Python `hashlib`, Go `hash`, Ruby `Digest`)

**Goal:** `Hash` update-object API (md5/sha1/sha2 family).
**Must Follow:** wrap Phase-0 `crypto` digest objects; `update` streams, `hexdigest`/`digest` finalize.
**Important Note:** native digest is the perf path; `copy` clones the digest state.

> Reference: Go — `hash.Hash` (`Write`/`Sum`) in `go/src/hash`; CPython — `Lib/hashlib.py`.

```kirn
pub struct Hash {
    var algo: string; var h: crypto;   # Phase-0 crypto object
    fn init(self, name: string) { self.algo = name; self.h = crypto.digest(name); }
    fn update(self, data: string) { self.h.update(data); }
    fn hexdigest(self) -> string { return self.h.hexdigest(); }
    fn digest(self) -> bytes { return self.h.digest(); }
    fn copy(self) -> Hash { ... }
}
pub fn md5(data="") -> Hash { h = Hash(); h.init("md5"); if data != "" { h.update(data); } return h; }
pub fn sha1(data="") -> Hash, sha224(...), sha256(...), sha384(...), sha512(...) -> Hash { ... }
pub fn new(name: string) -> Hash { ... }
```

### 4e. `lib.hmac` (new; Python `hmac`) & `lib.secrets` (Python `secrets`, Ruby `SecureRandom`)

**Goal:** HMAC + cryptographically-strong token helpers.
**Must Follow:** `hmac.new` mirrors Python; entropy only via `crypto.random_bytes`; constant-time comparisons.
**Important Note:** `compare_digest` must be constant-time (no data-dependent branch).

> Reference: Ruby — `SecureRandom` (`bytes`/`hex`/`base64`) in `ruby/lib/securerandom.rb`.

```kirn
# hmac
pub fn new(key: string, msg="") -> Hmac { ... }
pub fn hmac_sha256(key: string, msg: string) -> string { ... }
# secrets (crypto.random_bytes = Phase 0, OS entropy)
pub fn token_bytes(n) -> string { return crypto.random_bytes(n).to_str(); }
pub fn token_hex(n) -> string { return hex.encode_str(secrets_bytes...); }
pub fn compare_digest(a, b) -> bool { return crypto.constant_time_eq(a, b); }
```

### 4f. `lib.crypto` (new; Go `crypto/*`, Rust crates note, OpenSSL)

**Goal:** symmetric + asymmetric primitives over Phase-0 `crypto`/`bigint`.
**Must Follow:** RSA math in Kirn over `bigint`; AES/ChaCha delegate to native; no hand-rolled primitives.
**Important Note:** public-key work is gated on Phase 8 `bigint`; constant-time discipline required.

Symmetric + asymmetric primitives over Phase-0 `crypto`/`bigint`.

```kirn
# symmetric (AES/ChaCha via substrate)
pub fn aes_encrypt(key, data) -> bytes, aes_decrypt(...) -> bytes { ... }
pub fn chacha20(key, nonce, data) -> bytes { ... }
# asymmetric (RSA) over bigint
pub struct RsaKey { var n, e, d: bigint; fn encrypt(self, m), decrypt(self, c), sign, verify ... }
pub fn generate_rsa(bits) -> RsaKey { ... }
pub fn sha256_hmac(key, msg) -> string { ... }
pub fn random_prime(bits) -> bigint, random_number(n) -> bigint { ... }
```

**Exit:** `bytes_test`, `base64_test`, `hex_test`, `hashlib_test`, `hmac_test`,
`secrets_test`, `crypto_test` green. **Consumes:** Phase 0 (bytes, crypto, bigint),
Phase 2 (strings). **Consumed by:** `zip`(crc32), `tar`, `http`(tls), `toml`(hash),
`args`(hash passwords), `crypto`'s own RSA.

---

## Phase 5 — regexp, glob, csv, toml, json breadth

> **Status/dedup:** **5a `regexp` = `[NEEDS-ENHANCE]`** (the existing `stdlib/lib/regexp.kn` is a
> glob-only subset; the real NFA/backtracking `Pattern`/`Match` engine is the open work here). Note:
> this plan (Phase 0+5a) proposes the regexp engine as **pure Kirn**, whereas `PLAN.md` Phase 7.2
> proposed a C++ RE2-style module — **pick one owner; recommendation: keep the pure-Kirn approach
> here** (better dogfooding; amend `PLAN.md`). **5b–5e (csv/toml/json breadth) = PENDING.**

**Goal:** parsing/serialization modules (Python `re/csv/tomllib/json`, Go
`regexp/encoding/csv`, Ruby `Regexp/CSV`, `encoding/…`).
**Must Follow:** engines stay pure Kirn (dogfooding); keep the existing glob-compatible `regexp.match`; no native parser.
**Important Note:** **performance-critical** — `regexp` must beat Go `regexp` on common patterns and `json` must beat Go `encoding/json` on parse/dump of typical payloads.

### 5a. `lib.regexp` (real engine — Phase 5, replacing glob subset)

**Goal:** real backtracking NFA `Pattern`/`Match` engine replacing the glob subset.
**Must Follow:** pure Kirn over 1-char substrings; keep `match/find/split/replace` signatures (`path.glob` uses `match`); subset `. * + ? | () [] \d \w \s ^ $ {n,m} \b`.
**Important Note:** this plan owns the pure-Kirn engine (not `PLAN.md`'s C++ RE2) — amend `PLAN.md`; must beat Go `regexp` on the common path (`kirn bench`).

> Reference: Go — `regexp` (match/findall/sub/split semantics) in `go/src/regexp`.

Implement a **backtracking NFA** in pure Kirn over 1-char substrings (careful: the
substrate has no native regex; do it in Kirn). Keep existing `match/find/split/
replace` signatures; add:

```kirn
pub struct Pattern {
    var src: string;
    fn init(self, p) { self.src = p; }
    fn search(self, s) -> Match? { ... }        # first match or nil
    fn findall(self, s) -> list { ... }
    fn sub(self, s, repl) -> string { ... }
    fn split(self, s) -> list { ... }
    fn test(self, s) -> bool { ... }
}
pub struct Match {
    var start: int; var end: int; var groups: list;
    fn span(self) -> (int, int) { return (self.start, self.end); }
    fn group(self, i) { ... }
}
pub fn compile(p) -> Pattern { return Pattern(p); }
pub fn search(pat, s) -> Match? { ... }
pub fn match(pat, s) -> bool { ... }            # keep glob-compatible (used by path.glob)
pub fn findall(pat, s) -> list, sub(pat, s, repl) -> string, split(pat, s) -> list { ... }
```

Syntax subset: `.` `*` `+` `?` `|` `()` `[]` `\d \w \s` `^` `$` `{n,m}` `\b`.

### 5b. `lib.csv` (new; Python `csv`, Go `encoding/csv`, Ruby `CSV`)

**Goal:** CSV `Reader`/`Writer` (Python `csv`, Go `encoding/csv`, Ruby `CSV`).
**Must Follow:** correct quoting/escaping; `reader(text, sep)`, `writer(sep)` factories.
**Important Note:** must beat Go `encoding/csv` on large files; single-pass parse.

```kirn
pub struct Reader {
    var data: list; var sep: string; var quote: string;
    var rows: list; var i: int;
    fn init(self, text, sep=",", quote="\"") { self.parse(text); }
    fn parse(self, text) { ... }
    fn read_row(self) -> list? { ... }
    fn read_all(self) -> list { return self.rows; }
}
pub struct Writer {
    var buf: string; var sep: string; var quote: string;
    fn init(self, sep=",", quote="\"") { self.buf = ""; }
    fn write_row(self, row: list) { ... }
    fn to_string(self) -> string { return self.buf; }
}
pub fn reader(text, sep=",") -> Reader { ... }
pub fn writer(sep=",") -> Writer { ... }
pub fn parse(text, sep=",") -> list { return reader(text, sep).read_all(); }
pub fn generate(rows: list, sep=",") -> string { ... }
```

### 5c. `lib.toml` (new; Python `tomllib`, Go `BurntSushi/toml` pattern, Rust `toml`; Ruby has **none**, note)

**Goal:** TOML loads/dumps (tables, arrays-of-tables, inline, dotted).
**Must Follow:** scalars int/float/bool/str/datetime; `load`/`dump` wrap `io`.
**Important Note:** Ruby ships none — cite Go `BurntSushi/toml`/Rust `toml` crates only.

> Reference: CPython — `Lib/tomllib` (stdlib TOML reader).

```kirn
pub fn loads(s: string) -> dict { ... }        # tables, arrays-of-tables, inline, dotted
pub fn dumps(d: dict) -> string { ... }
pub fn load(path) -> dict { return loads(io.read(path)); }
pub fn dump(d: dict, path) { io.write(path, dumps(d)); }
```

Support scalars (int/float/bool/string/datetime), `[table]`, `[[array.of.tables]]`,
inline `{a=1}`, dotted `a.b.c`.

### 5d. `lib.json` (extend; add options)

**Goal:** extend the fixed `json` pet with options + file helpers.
**Must Follow:** keep native `dumps`; parser is the Phase-1 fix; all options additive (non-breaking).
**Important Note:** must beat Go `encoding/json` on typical payloads; no reflection-heavy encode.

> Reference: Go — `encoding/json` (Marshal/Unmarshal/Valid) in `go/src/encoding/json`; CPython — `json` stdlib; Nim — `Nim/lib/pure/json.nim`.

```kirn
pub fn dumps(v) -> string { return priv_dumps(v); }        # native
pub fn loads(s) { ... }                                     # fixed parser (Phase 1)
# options (optional, does not break existing):
pub fn dumps_pretty(v) -> string { ... }                    # indent 2
pub fn loads_from_file(path) { return loads(io.read(path)); }
pub fn dumps_to_file(path, v) { io.write(path, dumps(v)); }
pub fn valid(s) -> bool { try { loads(s); return true; } catch _ { return false; } }
```

**Exit:** `regexp_test`, `csv_test`, `toml_test`, `json_test` green.
**Consumes:** Phase 1 (`io`), Phase 2 (`strings`), Phase 4 (`bytes` for toml datetime
optional). **Consumed by:** `http`, `args`(config), `psutil`, `template`, `yaml`.

---

## Phase 6 — args, console, log, benchmark, psutil, sys

**Goal:** CLI/observability/system modules (Python `argparse/sys/logging/
platform`, Go `flag/log/testing`, Ruby `OptionParser/Benchmark`, Rust `env/process`).
**Must Follow:** thin wrappers over Phase-0 `os`/`process`/`sys`/`keyboard`; no native CLI parsing in C++.
**Important Note:** `args`/`sys` must be startup-cheap; `benchmark` uses `time.monotonic`. Interactive pets (console/keyboard) are env-gated in tests.

### 6a. `lib.args` (new; Python `argparse`, Go `flag`, Ruby `OptionParser`)

**Goal:** `ArgumentParser` + `getopt` (Python `argparse`, Go `flag`).
**Must Follow:** declarative specs + one canonical `add`/`parse`/`get`; parsing in Kirn.
**Important Note:** must beat Go `flag` startup on small CLIs; `kirn bench` micro-bench.

> Reference: CPython — `Lib/argparse.py` (`ArgumentParser`/`add_argument`).

```kirn
pub class ArgumentParser {
    var prog: string; var description: string;
    var specs: list;                      # [{flags, dest, type, default, nargs, help}]
    var values: dict;
    fn __init__(self, prog="", description="") { ... }   # or class + fn init
    fn add(self, flags: list, dest="", type="str", default=nil, nargs=1, help="") {
        self.specs.append({flags: flags, dest: dest, type: type, default: default, nargs: nargs, help: help});
    }
    fn parse(self, argv: list) { ... }   # fills self.values
    fn get(self, name) { return self.values[name]; }
    fn help_text(self) -> string { ... }
}
pub fn getopt(argv: list, short: string, long: list) -> (list, list) { ... }   # Python getopt
```

### 6b. `lib.console` (new; Ruby/`getch`, Go `github.com/eiannone/keyboard`-style)

**Goal:** terminal helpers (`getch`/`clear`/`set_color`/`reset`/`input`) over `keyboard`/`sys`.
**Must Follow:** ANSI escapes only; no native console dep beyond Phase 0.
**Important Note:** interactive — tests gated by env var; no TTY in CI.

```kirn
pub fn getch() -> string { return keyboard.read_key(true).to_str(); }   # Phase 0 keyboard
pub fn clear() { print("\x1b[2J\x1b[H"); }
pub fn set_color(code) { printf("\x1b[%dm", code); }
pub fn reset() { printf("\x1b[0m"); }
pub fn read_line(prompt) -> string { ... }
pub fn input(prompt) -> string { ... }        # Python input
```

### 6c. `lib.keyboard` (new)

**Goal:** `Key` value + `read_key` wrapper over Phase-0 `keyboard`.
**Must Follow:** no key-encoding in Kirn; return `Key?`.
**Important Note:** gated by env var; exercised by a driver in Phase 12.

```kirn
pub struct Key { var code: int; var name: string; var shift: bool; var ctrl: bool; }
pub fn read_key(blocking=true) -> Key? { return keyboard.read_key(blocking); }
pub fn is_printable(k) -> bool { ... }
```

### 6d. `lib.log` (new; Go `log`, Python `logging` simplified, Rust `log`)

**Goal:** simple leveled `Logger` (Go `log`, Rust `log`).
**Must Follow:** levels debug/info/warn/error/fatal; output to stdout/stderr/file.
**Important Note:** module-level fns target a default logger; no per-call allocation when disabled.

```kirn
pub struct Logger {
    var level: string; var prefix: string; var out: string;   # 'stdout'|'stderr'|file
    fn init(self, prefix="", out="stderr") { ... }
    fn debug(self, msg) { ... } fn info(self, msg) { ... }
    fn warn(self, msg) { ... } fn error(self, msg) { ... }
    fn fatal(self, msg) { print msg; os.exit(1); }
}
pub fn default() -> Logger { ... }
pub fn info(msg), warn(msg), error(msg), debug(msg) { ... }   # module-level to default
```

### 6e. `lib.benchmark` (new; Ruby `Benchmark`, Go `testing.B`, Python `timeit`)

**Goal:** tiny timing helpers (Ruby `Benchmark`, Go `testing.B`, Python `timeit`).
**Must Follow:** use `time.monotonic`; return seconds/records; no printing by default.
**Important Note:** the basis for `kirn bench` measurements cited across this plan.

```kirn
pub fn realtime(f) -> float { t0 = time.monotonic(); f(); return time.monotonic() - t0; }
pub fn measure(f) -> string { t = realtime(f); return str(t) + "s"; }
pub fn bm(fns: list) -> list { ... }           # [{name,seconds}]
```

### 6f. `lib.psutil` (new; Python `psutil` — system/process statistics)

**Goal:** process/system stats over Phase-0 `os`/`process`.
**Must Follow:** report dicts with stable keys; no native stats beyond Phase 0.
**Important Note:** best-effort/optional — not all platforms expose every field.

Over Phase-0 `os`/`process`.

```kirn
pub fn cpu_percent(interval=0.0) -> float { ... }
pub fn cpu_count(logical=true) -> int { return os.cpu_count(); }
pub fn virtual_memory() -> dict { ... }        # {total, available, percent}
pub fn disk_usage(path) -> dict { ... }
pub fn pid_exists(pid) -> bool { ... }
pub fn process_iter() -> list { ... }          # [{pid, name, ...}]
pub fn boot_time() -> float { ... }
pub fn users() -> list { ... }
```

### 6g. `lib.sys` (new; Python `sys`, Go `os`, Rust `std::env/process`, Ruby `ENV`)

**Goal:** Python-`sys`/Go-`os` introspection pet.
**Must Follow:** `argv/stdin_line/stdout_write/stderr_write/exit/platform/version/...`; delegate to pseudo `sys`/`os`.
**Important Note:** must not shadow the `sys` pseudo-module — capture at load (`priv_*`).

```kirn
pub fn argv() -> list { return os.args(); }
pub fn stdin_line() -> string { return sys.stdin_read_line(); }
pub fn stdout_write(s) { sys.stdout_write(s); }
pub fn stderr_write(s) { sys.stderr_write(s); }
pub fn exit(code=0) { os.exit(code); }
pub fn platform() -> string { return sys.platform(); }
pub fn version() -> string { return sys.version(); }
pub const max_int: int = 9007199254740991;
pub fn set_recursion_limit(n) { sys.set_recursion_limit(n); }
pub fn get_recursion_limit() -> int { ... }
```

**Exit:** `args_test`, `console_test`, `keyboard_test` (may be interactive; gate by
env var), `log_test`, `benchmark_test`, `psutil_test`, `sys_test` green.
**Consumes:** Phase 0 (os/process/sys/keyboard), Phase 1 (os), Phase 3 (time).
**Consumed by:** `http`(argparse for CLI tools), `tar`, sample apps.

---

## Phase 7 — http, socket, url, tls, smtp, wsgi

**Goal:** networking modules (Python `socket/http/urllib/ssl/wsgiref`, Go
`net/net/http/net/url/net/smtp`, Rust `std::net`, Ruby `socket/net/http/open-uri`).
**Must Follow:** sit on Phase-0 `socket`; TLS/certs from Phase-0; protocol parsing in Kirn.
**Important Note:** **performance-critical** — the HTTP client must beat Go `net/http` on small-request latency (connection reuse, minimal copies).

These sit on the Phase-0 `socket` substrate.

### 7a. `lib.socket` (new)

**Goal:** `Socket`/`Server` wrappers over Phase-0 `socket`.
**Must Follow:** timeout-aware, non-blocking options pass through; no native reimplementation.
**Important Note:** tests behind `KIRN_TEST_NET=1`.

> Reference: Go — `net` package (Listen/Accept/Dial/Conn) in `go/src/net`.

```kirn
pub struct Socket {
    var fd: socket;
    fn connect(self, host, port) { ... }
    fn send(self, data) -> int { ... }
    fn recv(self, n) -> string { ... }
    fn recv_line(self) -> string { ... }
    fn close(self) { ... }
    fn set_timeout(self, ms) { ... }
}
pub struct Server {
    var fd: socket;
    fn bind(self, host, port) { ... }
    fn listen(self, backlog) { ... }
    fn accept(self) -> Socket { ... }
    fn close(self) { ... }
}
pub fn tcp_connect(host, port) -> Socket { ... }
pub fn tcp_server(host, port) -> Server { ... }
pub fn resolve(host, port) -> string { return socket.resolve(host, port); }
```

### 7b. `lib.url` (new; Python `urllib.parse`, Go `net/url`, Rust `Url`)

**Goal:** URL parse/quote/unquote/query (Python `urllib.parse`, Go `net/url`).
**Must Follow:** pure Kirn over `strings`; `Url` value type.
**Important Note:** must beat Go `net/url` on typical parse; single-pass split.

```kirn
pub struct Url {
    var scheme, host, path, query, fragment: string;
    var port: int;
    fn parse(s) -> Url { ... }
    fn to_string(self) -> string { ... }
    fn quote(self), unquote(self) -> string { ... }
    fn encode_query(self, params: dict) -> string { ... }
}
pub fn parse(s) -> Url { return Url.parse(s); }
pub fn quote(s) -> string, unquote(s) -> string, encode_form(params) -> string { ... }
```

### 7c. `lib.http` (new; Go `net/http`, Python `http.client/urllib.request`, Ruby `Net::HTTP`)

**Goal:** minimal HTTP client + server (Go `net/http`).
**Must Follow:** `Request`/`Response` values, connection reuse, keep-alive; routes in Kirn; headers via `time`/`url`.
**Important Note:** server rendering reuses `html`/`template`; network tests gated.

> Reference: Go — `net/http` (`Get/NewRequest/ServeMux`) in `go/src/net/http`.

```kirn
pub struct Request {
    var method, url, path, body: string;
    var headers: dict;
}
pub struct Response {
    var status: int; var reason, body: string; var headers: dict;
}
pub fn get(url) -> Response { return request("GET", url, "", {}); }
pub fn post(url, body) -> Response { return request("POST", url, body, {}); }
pub fn request(method, url, body, headers) -> Response { ... }   # over socket + url
pub fn client(url) -> HttpClient { ... }                          # connection reuse
pub struct Server {   # minimal HTTP server (Go http.Server / wsgi)
    fn route(self, method, path, handler) { ... }
    fn listen(self, port) { ... }
    fn handle_request(self, sock) -> Response { ... }
}
```

### 7d. `lib.http.cookies` / `lib.http.mime` (new)

**Goal:** cookie parse/set + MIME type lookup.
**Must Follow:** pure Kirn tables; extension→type mapping only.
**Important Note:** used by the `http` server; keep tables small and data-driven.

```kirn
pub fn parse_cookie(header) -> dict { ... }
pub fn set_cookie(name, value, opts) -> string { ... }
pub fn guess_type(path) -> string { ... }    # via extension table
```

### 7e. `lib.tls` (new; Python `ssl`, Go `crypto/tls`, Ruby `OpenSSL::SSL`)

**Goal:** TLS client/context wrapper over Phase-0 crypto/socket.
**Must Follow:** verification on by default; cert/key loading; no pure-Kirn crypto.
**Important Note:** depends on Phase-4 `crypto`; env-gated tests.

```kirn
pub struct TlsContext {
    var verify: bool; var ca_file: string;
    fn load_cert(self, cert, key) { ... }
    fn wrap(self, sock) -> Socket { ... }
}
pub fn-ish ...
pub fn client(host, port, opts) -> Socket { ... }   # TCP + TLS handshake
```

### 7f. `lib.smtp` (new; Go `net/smtp`, Python `smtplib`)

**Goal:** SMTP client (Go `net/smtp`, Python `smtplib`).
**Must Follow:** plain + STARTTLS; auth; protocol in Kirn over `socket`/`tls`.
**Important Note:** dependency/network gated; keep minimal.

```kirn
pub struct Smtp {
    var socket: Socket; var host: string; var port: int; var tls: bool;
    fn connect(self, host, port, tls=false) { ... }
    fn login(self, user, passwd) -> bool { ... }
    fn sendmail(self, from, to: list, msg) -> bool { ... }
    fn quit(self) { ... }
}
```

### 7g. `lib.wsgi` (new; Python `wsgiref` — server gateway spec)

**Goal:** portable server/app contract (Python `wsgiref`).
**Must Follow:** app = `fn(environ: dict) -> (status: int, headers: dict, body: string)`; `run`/`make_environ`.
**Important Note:** models the contract only; no framework.

Define a portable server/application contract modeling `WSGI`:

```kirn
# an app is a function: app(environ: dict) -> (status: int, headers: dict, body: string)
pub fn run(app, port) { ... }          # serve app on port (blocks)
pub fn make_environ(url, method, headers, body) -> dict { ... }
```

**Exit:** `url_test`, `http_test` (against a local test server), `socket_test`,
`smtp_test`(dependency-gated), `wsgi_test`. These need network; run behind a
`KIRN_TEST_NET=1` guard so `kirn test` stays hermetic by default.
**Consumes:** Phase 0 (socket), Phase 5 (`json` for REST), Phase 6 (no), Phase 2
(strings), Phase 7's own (url). **Consumed by:** `console`(http clients), sample CLI.

---

## Phase 8 — bigint, math big, crypto RSA/DH (pure-Kirn over Phase-0 bigint)

**Goal:** arbitrary-precision integer arithmetic + public-key crypto (Python `int`
is arbitrary precision; Go `math/big`; Rust `num-bigint` crate; PHP `gmp`).
**Must Follow:** bigint value comes from Phase 0; RSA/DH math in Kirn over `bigint`; no bignum in pets.
**Important Note:** skip cleanly if the `bigint` substrate is deferred.

### 8a. `lib.bigint` (new; wrapper + helpers)

**Goal:** thin wrapper + helpers over the Phase-0 `bigint` value.
**Must Follow:** `from_str/from_int/to_str/add/sub/mul/div/mod/pow/gcd/modinv/is_prime`; delegate operations.
**Important Note:** RSA relies on `modinv`/`powmod`; constant-time where keys are involved.

> Reference: Go — `math/big` (`Int` methods) in `go/src/math/big`; LLVM — `APInt` fixed-width big-int ops in `llvm/llvm/include/llvm/ADT/APInt.h`.

```kirn
pub const BN: string = "__bigint__";
pub fn from_str(s) -> bigint { return bigint.from_str(s); }
pub fn from_int(n) -> bigint { ... }
pub fn to_str(b) -> string { return b.to_str(); }
pub fn add(a, b) -> bigint, sub, mul, div, mod, pow, gcd, modinv, is_prime { ... }
pub fn compare(a, b) -> int { ... }
pub fn to_int(b) -> int { ... }
```

### 8b. `lib.crypto` big-key ops

**Goal:** RSA/DH/signature operations over `bigint`.
**Must Follow:** reuse the Phase-4 `crypto` pet; primes via Miller-Rabin in Kirn; no hand-rolled padding.
**Important Note:** key generation is slow — expose bit-size limits; constant-time modular exponentiation.

`generate_rsa`, `rsa_encrypt/decrypt/sign/verify`, `diffie_hellman`, `random_prime`.

```kn
# stdlib/pet/crypto_rsa_test.kn
import crypto;

fn main() {
    key = crypto.generate_rsa(2048);
    msg = crypto.bigint_from_str("123456789");
    c = key.encrypt(msg);
    assert_eq(key.decrypt(c).to_str(), "123456789");
}
```

**Exit:** `bigint_test`, `crypto_big_test` green. **Consumes:** Phase 0 (bigint),
Phase 4 (crypto). **Consumed by:** `crypto`(RSA), `binascii`-style big ints in `json`/
`toml`, `random.randint` for big.

---

## Phase 9 — compression & archives: zlib, gzip, zip, tar

**Goal:** (Python `zlib/gzip/zipfile/tarfile`, Go `compress/* archive/*`, Ruby
`Zlib`). Sit on Phase-0 `compress`.
**Must Follow:** deflate/inflate native (Phase 0); gzip/zip/tar framing in Kirn; crc32 via native.
**Important Note:** must beat Go `compress/gzip` on decompress throughput (`kirn bench`).

### 9a. `lib.zlib` (new)

**Goal:** zlib compress/decompress + crc32/adler32.
**Must Follow:** delegate raw deflate/inflate to Phase-0 `compress`; crc via native.
**Important Note:** bulk path native; streaming API later.

```kirn
pub fn compress(data: string, level=-1) -> bytes { return compress.deflate(bytes.from_str(data), level); }
pub fn decompress(data: bytes) -> string { return compress.inflate(data).to_str(); }
pub fn crc32(data: string) -> int { return compress.b64_crc32(bytes.from_str(data)); }
pub fn adler32(data: string) -> int { ... }
```

### 9b. `lib.gzip` (new)

**Goal:** gzip framing over zlib.
**Must Follow:** header/trailer in Kirn; `read_file`/write helpers via `io`.
**Important Note:** beat Go `compress/gzip` decompress on common files.

> Reference: Go — `compress/gzip` (`Writer`/`Reader`) in `go/src/compress/gzip`.

```kirn
pub fn compress(data: string) -> bytes { ... }    # gzip header + deflate + trailer
pub fn decompress(data: bytes) -> string { ... }
pub fn read_file(path) -> string { return decompress(bytes.from_str(io.read(path))); }
```

### 9c. `lib.zip` (new; Python `zipfile`, Go `archive/zip`)

**Goal:** zip read/build (local + central directory).
**Must Follow:** parse in Kirn over `bytes`; per-entry deflate via zlib; crc32 from zlib.
**Important Note:** stream large entries; no native zip.

> Reference: Go — `archive/zip` (`Reader`/`Writer`) in `go/src/archive/zip`.

```kirn
pub struct Entry { var name, data: string; var crc: int; }
pub struct ZipFile {
    var entries: list;
    fn read(self) -> list { ... }                 # local + central directory parse
    fn names(self) -> list { ... }
    fn read_entry(self, name) -> string? { ... }
}
pub fn open(path) -> ZipFile { ... }
pub fn to_bytes(entries: list) -> bytes { ... }   # build a zip
pub fn write(path, entries: list) { io.write_bytes(path, to_bytes(entries)); }
```

### 9d. `lib.tar` (new; Python `tarfile`, Go `archive/tar`)

**Goal:** POSIX ustar tar read/build.
**Must Follow:** header parse in Kirn; no native tar.
**Important Note:** pairs with gzip for `.tar.gz`; golden-file tests.

```kirn
pub struct TarEntry { var name, mode, type: string; var data: bytes; }
pub fn open(path) -> list { ... }                 # header + body parse (ustar POSIX)
pub fn to_bytes(entries: list) -> bytes { ... }
pub fn write(path, entries: list) { io.write_bytes(path, to_bytes(entries)); }
```

**Exit:** `zlib_test`, `gzip_test`, `zip_test`, `tar_test` green.
**Consumes:** Phase 0 (compress), Phase 4 (bytes/base64/hex/crc32), Phase 1 (`io`),
Phase 2 (`path`). **Consumed by:** archive tooling, `http`(multipart not needed).

---

## Phase 10 — template, html, yaml

**Goal:** text generation + markup (Go `text/template`/`html/template`, Python
`string.Template`, Ruby `ERB`).
**Must Follow:** template engine in Kirn; html escaping table constant.
**Important Note:** `yaml` is community/optional (not Python/Go stdlib); `toml`/`http` remain stdlib.

### 10a. `lib.template` (new)

**Goal:** `{{var}}`/`{{each}}`/`{{if}}` template renderer.
**Must Follow:** compile once → callable; escape-by-default in html mode later.
**Important Note:** keep the tiny core; no full Jinja.

> Reference: Go — `text/template` (`New/Parse/Execute`) in `go/src/text/template`.

```kirn
pub fn render(tpl: string, ctx: dict) -> string { ... }   # support {{var}} {{each xs}} {{if}}
pub fn compile(tpl) -> (func) -> string { ... }           # precompile
```

### 10b. `lib.html` (new; Python `html`, Go `html`)

**Goal:** `escape`/`unescape`/`strip_tags`.
**Must Follow:** cover `& < > " '`; pure Kirn.
**Important Note:** server rendering reuses this via `http`.

```kirn
pub fn escape(s: string) -> string { ... }      # &, <, >, ", '
pub fn unescape(s: string) -> string { ... }
pub fn strip_tags(s: string) -> string { ... }
```

### 10c. `lib.yaml` (new; only if desired — Ruby `YAML`/Psych, Python `yaml`

external; Go `gopkg.in/yaml.v3` external)

**Goal:** optional YAML load/dump.
**Must Follow:** pure Kirn; explicitly non-stdlib/community.
**Important Note:** unlike `toml`/`http`, YAML is not in Python/Go stdlib — keep it out of the core pet set.

```kirn
pub fn dump(v) -> string { ... }
pub fn load(s: string) { ... }
pub fn load_file(path) { ... }
```

Note: neither Python nor Go ship YAML in stdlib; treat `yaml`/`toml`/`http`
differently — `toml` and `http` ARE stdlib in Python/Go, YAML is **not**. Mark
`yaml` as optional/community.

**Exit:** `template_test`, `html_test`, (`yaml_test` optional) green.
**Consumes:** Phase 2 (strings/characters), Phase 5 (json for nested ctx? no).
**Consumed by:** `http`(server rendering), sample apps.

---

## Phase 11 — database & sqlite3

**Goal:** embedded SQL (Python `sqlite3`, Go `database/sql`, Rust `rusqlite`).
**Must Follow:** DB-API-style `Connection`; transactions via explicit BEGIN/COMMIT/ROLLBACK; all SQL execution in the Phase-0 `sqlite` substrate.
**Important Note:** sqlite is an optional compile flag; the pet stays thin.

Sits on Phase-0 `sqlite`.

```kirn
pub struct Connection {
    var db: sqlite;
    fn init(self, path) { self.db = sqlite.connect(path); }
    fn close(self) { self.db.close(); }
    fn execute(self, sql: string, params: list=[]) { ... }     # rowcount via db.exec
    fn query(self, sql: string, params: list=[]) -> list { ... }  # list of row-tuples + column names
    fn query_one(self, sql, params) -> tuple? { ... }
    fn last_insert_id(self) -> int { ... }
    fn transaction(self, fn) { self.db.exec("BEGIN"); try { fn(); self.db.exec("COMMIT"); } catch e { self.db.exec("ROLLBACK"); raise e; } }
}
pub fn connect(path) -> Connection { return Connection(path); }
```

**Exit:** `sqlite3_test` green (temp in-memory/`:memory:`). **Consumes:** Phase 0
(sqlite), Phase 2 (strings). **Consumed by:** sample CRUD apps, `psutil`(metrics db).

---

## Phase 12 — keyboard polish, console raw-mode, term input (final I/O ergonomics)

**Goal:** refine `keyboard`/`console` with raw terminal + termios-style modes.
**Must Follow:** raw mode via Phase-0 getch/console; no per-key C++ logic in the pet.
**Important Note:** interactive — gate tests behind an env var/driver.

Refine `keyboard`/`console` with raw terminal, termios-style modes:

```kirn
# console raw mode (Phase 0 getch already gives keys)
pub fn raw_mode(on: bool) { ... }
pub fn get_key() -> Key { ... }        # returns printable char or name ('UP','F1')
pub fn term_size() -> (int, int) { ... }
pub fn read_password(prompt) -> string { ... }   # no echo
```

**Exit:** `keyboard_test`, `console_test` green under a driver. **Consumes:**
Phase 0 (getch/console), Phase 6 (keyboard). **Consumed by:** TUI sample.

---

## Phase 13 — `sys`, `os.exec`, `tempfile`, `fileutils`, `shutil`

**Goal:** filesystem + process orchestration (Python `os/shutil/tempfile`, Go
`os/exec`, Ruby `FileUtils/Tempfile`).
**Must Follow:** compose `io`/`os`/`path`; recursive ops in Kirn; no native fs reimplementation.
**Important Note:** `os.exec` wraps Phase-0 `process`; tests use temp dirs.

### 13a. `lib.fileutils` (new; Ruby `FileUtils`, `shutil`)

**Goal:** Ruby-`FileUtils`/`shutil`-style copy/move/remove/walk.
**Must Follow:** `rm_r`/`copytree` recurse safely; reuse `os`/`io`.
**Important Note:** destructive ops must validate paths.

```kirn
pub fn cp(src, dst) { io.copy(src, dst); }
pub fn cp_r(src, dst) { ... }                 # recursive
pub fn mv(src, dst) { os.rename(src, dst); }
pub fn rm(p) { os.remove(p); }
pub fn rm_r(p) { ... }                        # recursive delete
pub fn mkdir_p(p) { os.mkdir_all(p); }
pub fn touch(p) { if not io.exists(p) { io.write(p, ""); } }
pub fn rmtree(p) { ... }                      # shutil.rmtree
pub fn walk(top) -> list { return os.walk(top); }
pub fn copytree(src, dst) { ... }
```

### 13b. `lib.tempfile` (new; Python `tempfile`, Ruby `Tempfile`, Go `os.CreateTemp`)

**Goal:** temp file/dir creation (Python `tempfile`, Ruby `Tempfile`, Go `os.CreateTemp`).
**Must Follow:** random names from Phase-0 entropy; optional delete-on-close; honor `os.tmpdir()`.
**Important Note:** TempFile cleanup must be exception-safe.

```kirn
pub fn mktemp(suffix="", prefix="") -> string { ... }   # os.tmpdir + random
pub fn mkdtemp(prefix="") -> string { ... }
pub struct TempFile {
    var path: string; var f: File;
    fn write(self, data) { ... } fn read(self) -> string { ... } fn close(self, delete=true) { ... }
}
pub fn NamedTemporaryFile(suffix="") -> TempFile { ... }
```

### 13c. `lib.os.exec` (new; Go `os/exec`, Ruby `Kernel#system`, Python `subprocess`)

**Goal:** subprocess run/spawn/output (Go `os/exec`, Python `subprocess`).
**Must Follow:** wrap Phase-0 `process`; blocking `run`, non-blocking `spawn`.
**Important Note:** env/cwd/stdio pass through; no shell unless explicitly asked.

```kirn
pub struct Process { var pid: int; var status: int; var stdout, stderr: string;
    fn wait(self) -> int { ... } fn exit_code(self) -> int { return self.status; } }
pub fn run(cmd: string, args: list=[]) -> Process { ... }        # blocks
pub fn spawn(cmd: string, args: list=[]) -> Process { ... }       # non-blocking
pub fn output(cmd: string, args: list=[]) -> string { return run(cmd, args).stdout; }
pub fn look_path(cmd: string) -> string? { ... }
```

**Exit:** `fileutils_test`, `tempfile_test`, `os_exec_test` green.
**Consumes:** Phase 0 (os/process), Phase 1 (io/os/path), Phase 3 (random).
**Consumed by:** `http`(cgi), `tar`, sample apps.

---

## Phase 14 — random breadth, monotonics, signal, logging final

**Goal:** remaining runtime-ish modules (Ruby `Signal`, Python `signal/sys`, Go
`os/signal`).
**Must Follow:** all delegate to Phase-0 `os`/`time`; no native signal handling in the pet.
**Important Note:** `perf_counter`/`process_time` come from the `time` pseudo-module.

```kirn
# lib.signal
pub fn trap(sig: string, handler) { os.signal(sig, handler); }
pub fn kill(pid, sig="TERM") { os.kill(pid, sig); }
pub fn list_signals() -> dict { return os.signal_list(); }
# lib.random additions
pub fn randbytes(n) -> bytes, gauss(mu, sigma) -> float, expovariate(l), normal_seed(n) { ... }
# lib.time additions
pub fn perf_counter() -> float { return time.perf_counter(); }
pub fn process_time() -> float { ... }
```

**Exit:** `signal_test`, `random_test`(extended), `time_test`(extended) green.

---

## Phase 15 — `sql`, `orm`, `mime`, `mem`, `crypto` completeness

**Goal:** stretch pets over already-landed substrate (query builders, MIME table, arena helpers, crypto completeness).
**Must Follow:** each is additive and independently shippable; reuse lower pets; no new native surface unless justified.
**Important Note:** long-lead — split across owners; `mem` helpers wrap the Phase-0 `Arena`.

```kn
# stdlib/pet/mem_smoke_test.kn — Arena helpers over the Phase-0 arena
import mem;

fn main() {
    a = mem.Arena();
    p = a.alloc(64);
    a.reset();
    assert_eq(a.used(), 0);
}
```

> Reference: Zig — `std.mem`/Allocator shape (`zig/lib/std/mem.zig`) for the arena helpers.

Stretch modules (longer lead, team can split):

- `lib.sql` / `lib.orm` — query builders over `sqlite3`.
- `lib.mime` — MIME type table, multipart parser.
- `lib.mem` — the Phase-0 `Arena` plus Kirn helpers (`alloc/free_all/reset`).
- `lib.crypto` completeness — Ed25519, ChaCha20-Poly1305, blake2, SHA-3.
- `lib.benchmark`/`lib.profiler` — function-call profiling using `time.process_time`.
- `lib.net.cgi`, `lib.net.websocket` (over socket).

---

## Beyond Phase 15 — future / stretch

**Goal:** speculative pets (ui/tui, async, db clients, ffi, serialization, http2/quic).
**Must Follow:** only after Phases 0–15 are stable; still no new executables (`kirn` + `kirnc` only).
**Important Note:** not committed — a backlog of proven patterns, not a plan of record.

- **`ui`/`tui`** — terminal UI widgets on `keyboard`/`console`.
- **`async`** — promise/await over spawn+chan+select.
- **`db`** — full clients (Postgres/MySQL) once sockets + protocol parse land.
- **`ffi`** — `extern fn`-based dynamic loading wrappers for host libs.
- **`serialization`** — msgpack, protobuf (over `bytes`).
- **`http2`/`quic`** — protocol upgrades over sockets.

---

## 3. Cross-module dependency map (no cycles)

```
Phase 0 (native substrate)
 ├─ lib.io, lib.os, lib.path, lib.errors   ───────┐
 ├─ lib.bytes, lib.base64, lib.hex               │
 ├─ lib.hashlib, lib.hmac, lib.secrets, lib.crypto
 ├─ lib.zlib, lib.gzip, lib.zip, lib.tar
 ├─ lib.socket, lib.url, lib.tls, lib.http, lib.smtp, lib.wsgi
 └─ lib.sqlite3, lib.keyboard, lib.console, lib.bigint

Phase1 ─► Phase2/3 ─► Phase4 ─► Phase5 ─► Phase6 ─► Phase7 ─► 8..15
```

Recommended build order enforces: import only modules from earlier phases (or same
phase with explicit acyclic marker). A `kirn check` adds a cycle detector so an
accidental cycle fails CI.

---

## 4. Test strategy

- Each module ships `stdlib/lib/<name>_test.kn`; `kirn test` runs them all (rc 0).
- **Hermetic default:** modules that touch network/OS-files/`keyboard` are skipped
  (or use temp dirs) unless `KIRN_TEST_NET=1` / `KIRN_TEST_OS=1`. `*_test.kn` guards:

```kirn
fn main() {
    if os.getenv("KIRN_TEST_OS") == nil { print("skipped"); return; }
    ...
}
```

- **Differential tests** against the reference implementations where valuable
  (e.g. `base64`, `hex`, `json`, `csv`, `datetime.iso`, `math` constants): compute
  expected vectors with Python and assert exact equality in Kirn.
- `regexp`, `toml`, `csv`, `zip`, `tar` use golden files checked in under
  `stdlib/testdata/`.

---

## 5. Per-module API inventory (curated, cross-language)

The following **canonical Kirn APIs** are the agreed surface (names lifted from the
four references, harmonized). This is the source of truth the phases implement.

### math

`PI E TAU inf nan`, `sqrt cbrt pow exp log log2 log10 sin cos tan asin acos atan
atan2 sinh cosh tanh hypot fabs floor ceil trunc round fmod copysign isnan isinf
isfinite degrees radians gcd lcm factorial isqrt comb perm fsum isclose clamp
min2 max2`.

> **CapCase rename (Kirn style):** the free builtins previously spelled lowercase
> are now CapCase — `abs(` → `__Abs`, `rational(` → `Rational` / `__Rational`,
> `min(`/`max(` → `Min`/`Max`. Pet wrappers keep lowercase module methods
> (`math.fabs`, `math.min2`) and object methods stay lowercase (`x.bit_length()`).

### os

`cwd chdir mkdir mkdir_all rmdir remove rename listdir walk stat env getenv setenv
environ args exit getpid cpu_count hostname linesep sep system home tmpdir`.

### path

`join basename dirname extname stem abs norm clean is_abs glob exists expanduser
rel commonpath split_drive`.

### strings

`contains starts_with ends_with find rfind index count replace split join repeat
reverse title capitalize swapcase fields split_whitespace lower upper trim
strip_prefix strip_suffix pad_left pad_right char_count byte_len sub format
index_of_char is_whitespace`.

### characters

`is_digit is_alpha is_alnum is_space is_upper is_lower to_upper to_lower ord chr`.

### collections

`HashMap HashSet Deque Counter`, `split join contains unique shuffle binary_search
partition zip2 chain flatten dedup`.

### itertools

`chain product permutations combinations cycle repeat accumulate groupby pairwise
starmap count islice zip_longest take drop takewhile dropwhile tee`.

### sort

`sort sort_by search is_sorted reverse stable_sort`.

### random

`Rng`, `seed random randint randrange choice shuffle sample gauss`,
`randbytes`.

### time

`now now_ms monotonic sleep localtime gmtime mktime strftime format_iso ordinal
perf_counter`.

### datetime

`Date DateTime Duration`, `today parse from_epoch now from_ordinal`,
`strftime iso weekday`; Duration `from_secs from_millis as_secs as_millis as_nanos`.

### statistics

`mean median mode stdev variance quantiles cov corr linear_regression`.

### bytes / base64 / hex

`bytes`: `new from_str from_list to_str to_list len concat slice get set equals`;
`base64`: `b64encode b64decode b32encode b32decode b16encode b16decode
urlsafe_encode urlsafe_decode`; `hex`: `encode decode encode_str decode_str`.

### hashlib / hmac / secrets / crypto

`md5 sha1 sha224 sha256 sha384 sha512`, `Hash.update hexdigest digest`, `new`;
`hmac`: `new hmac_sha256`; `secrets`: `token_bytes token_hex compare_digest
randbelow`; `crypto`: `aes_encrypt aes_decrypt chacha20 generate_rsa rsa_encrypt
rsa_decrypt rsa_sign rsa_verify sha256_hmac random_prime random_number`.

### regexp

`Pattern Match`, `compile search findall sub split match test`, `Match.group span`.

### csv

`Reader Writer`, `reader writer parse generate read_all read_row write_row`.

### toml

`loads dumps load dump`.

### json

`dumps loads dumps_pretty loads_from_file dumps_to_file valid`.

### args / console / log / benchmark / psutil / sys

`args`: `ArgumentParser add parse get help_text`, `getopt`;
`console`: `getch clear set_color reset read_line input read_password raw_mode
term_size`;
`log`: `Logger debug info warn error fatal`, `default debug info warn error`;
`benchmark`: `realtime measure bm`;
`psutil`: `cpu_percent cpu_count virtual_memory disk_usage pid_exists process_iter
boot_time users`;
`sys`: `argv stdin_line stdout_write stderr_write exit platform version
set_recursion_limit get_recursion_limit`.

### socket / url / http / tls / smtp / wsgi

`socket`: `Socket Server tcp_connect tcp_server resolve`;
`url`: `Url parse quote unquote encode_form`;
`http`: `Request Response get post request client Server`;
`tls`: `TlsContext client`; `smtp`: `Smtp connect login sendmail quit`;
`wsgi`: `run make_environ`.

### bigint

`from_str from_int to_str to_int add sub mul div mod pow gcd modinv is_prime compare`.

### compression / archives

`zlib`: `compress decompress crc32 adler32`; `gzip`: `compress decompress read_file`;
`zip`: `Entry ZipFile open to_bytes write read_entry names`;
`tar`: `TarEntry open to_bytes write`.

### template / html / yaml

`template`: `render compile`; `html`: `escape unescape strip_tags`;
`yaml`: `dump load load_file` (optional).

### sqlite3

`Connection connect execute query query_one last_insert_id transaction close`.

### keyboard / console

`Key read_key is_printable`, `console `raw_mode get_key term_size read_password`.

### os.exec / fileutils / tempfile

`exec`: `Process run spawn output look_path`; `fileutils`: `cp cp_r mv rm rm_r
mkdir_p touch rmtree walk copytree`; `tempfile`: `mktemp mkdtemp TempFile
NamedTemporaryFile`.

### signal

`trap kill list_signals`.

---

## 6. Example: a small program using several modules together

The point of "lib can use each other": an HTTP-API cache tool draws on `args`,
`http`, `json`, `sqlite3`, `time`, `log`, `crypto`:

```kirn
# tools/apitool.kn
import lib.args;
import lib.http;
import lib.json;
import lib.sqlite3;
import lib.time;
import lib.log;
import lib.crypto;

fn main() {
    ap = args.ArgumentParser("apitool", "fetch + cache a JSON API");
    ap.add(["-u", "--url"], dest="url", type="str", required=true, help="endpoint");
    ap.add(["-d", "--db"], dest="db", default="cache.db", help="sqlite path");
    ap.add(["-t", "--ttl"], dest="ttl", default=30, type="int", help="cache ttl (s)");
    ap.parse(args.argv());
    url = ap.get("url"); if url is nil { print(ap.help_text()); return; }

    db = sqlite3.connect(ap.get("db"));
    db.execute("CREATE TABLE IF NOT EXISTS cache (url TEXT PRIMARY KEY, body TEXT, at INT)");
    row = db.query_one("SELECT body FROM cache WHERE url = ? AND at > ?", [url, time.now_ms() / 1000 - int(ap.get("ttl"))]);

    if row is not nil {
        log.info("cache hit: " + url);
        print(row[0]);
    } else {
        r = http.get(url);
        if r.status == 200 {
            body = r.body;
            db.execute("INSERT OR REPLACE INTO cache (url, body, at) VALUES (?, ?, ?)",
                       [url, body, time.now_ms() / 1000]);
            print(body);
        } else {
            log.error("status " + str(r.status));
        }
    }
    db.close();
}
```

This composes `args` (CLI) → `http` (network) → `json` (not shown but could parse the
body) → `sqlite3` (persist) → `time` (TTL) → `log` (observability). All modules are
`import lib.<name>` and interchangeable; no import cycle.

---

## 7. Concurrency & synchronization (deferred to Phase 11 of `DO_FIRST_PLAN`)

**Goal:** a `lib.sync` pet (`Mutex/RWMutex/Once/WaitGroup/Cond`) modeling Go `sync` / Rust `std::sync`.
**Must Follow:** keep API names identical to Go/Rust so the substrate drop-in is seamless; build on `chan`/`spawn`/`select`.
**Important Note:** **deliberately deferred** until the safe worker-pool substrate lands; do not ship a half-safe mutex.

> Reference: Go — `sync` (`Mutex/RWMutex/Once/WaitGroup/atomic`) in `go/src/sync`; Rust — `std::sync` (`rust/library/std/src/sync`).

Kirn has `spawn`/`chan`/`select`. A `lib.sync` module (Go `sync`: `Mutex/RWMutex/
Once/WaitGroup/Cond`; Rust `std::sync`: `Mutex/RwLock/atomic`) is **deliberately
deferred** until `DO_FIRST_PLAN` Phase 11 ("Concurrency & GC finalize") lands the
safe worker-pool substrate. Design stub here for later:

```kirn
pub struct Mutex { var ch: chan; fn init(self) { self.ch = chan.buffer(1); }
    fn lock(self) { self.ch.send(1); } fn unlock(self) { self.ch.recv(); } }
pub struct WaitGroup { ... }
```

> Keep the API names (Mutex/RWMutex/Once/WaitGroup) identical to Go/Rust so the
> substrate drop-in is seamless.

---

## 8. Verification / Definition of Done

For each phase:

1. `kirn check` on every module (0 errors; W-lints clean).
2. `kirn test` runs the phase's `*_test.kn` green.
3. Full `kirn test` (all phases) green; no regressions to existing modules.
4. Differential vectors (base64/hex/json/csv/datetime/math) match the reference.
5. Dependency graph acyclic (checked).
6. Modules dogfood: at least one `tools/*.kn` real tool (e.g. `apitool.kn`) imports
   several modules together and runs.

---

## 9. Decided conventions (ratify before code)

1. **Qualified vs unqualified:** modules accessed `import lib.<mod>` and used
   `mod.fn(...)` / `mod.Type(...)`. Never `from ... import *`.
2. **Self-shadow guard:** any module whose name equals a pseudo-module
   (`io os time json math`) MUST capture pseudo-members at load into `priv_*`.
3. **Argument order:** follow Python where 2+ languages conflict
   (`json.dumps/loads`, `csv.reader/writer`, `datetime.strftime`). For `join`, expose
   `join(xs, sep)` (Go is the odd one out).
4. **Errors:** `raise(string)`, `catch e {}`; canonical messages from `lib.errors`.
5. **Return `nil`/`?` vs raise:** predicate/optional getters return `nil`; operations
   that are expected to fail by contract raise (`json.loads`, `int()`).
6. **Files:** `io.write` overwrites, `io.append` appends (after Phase-0 truncate).
7. **Test names:** `<module>_test.kn`; net/OS-dependent tests gated by env vars.

---

## Appendix A — CPython stdlib (survey highlights)

_Module / purpose / key API — the exact reference for Phases 3,5,6,7,9,11._

- `math`: `floor ceil trunc sqrt cbrt pow exp exp2 expm1 log log1p log2 log10 sin
cos tan asin acos atan atan2 sinh cosh tanh hypot fabs copysign fmod remainder
isclose isnan isinf isfinite fsum prod gcd lcm comb perm factorial isqrt degrees
radians frexp ldexp modf gamma lgamma erf erfcinf`; consts `pi e tau inf nan`.
- `statistics`: `mean fmean geometric_mean harmonic_mean median median_low
median_high median_grouped mode multimode quantiles variance pvariance stdev
pstdev covariance correlation linear_regression NormalDist`.
- `random`: `Random/SystemRandom`; `random randint randrange randbytes uniform
choice choices shuffle sample gauss normalvariate expovariate gammavariate
betavariate binomialvariate seed getstate setstate getrandbits`.
- `datetime`: `date time datetime timedelta tzinfo timezone`; `today now utcnow
fromtimestamp combine fromisoformat strptime strftime isoformat timestamp`; attrs
  `year month day hour minute second microsecond weekday isoweekday isocalendar`;
  `timedelta.total_seconds`.
- `time`: `time time_ns sleep gmtime localtime mktime asctime ctime strftime
strptime monotonic perf_counter process_time thread_time get_clock_info`;
  `struct_time` = `tm_year..tm_isdst`.
- `os`: `stat lstat access chdir chmod chown mkdir rmdir remove unlink rename listdir
open read write fdopen readlink symlink link truncate utime umask makedirs
removedirs renames walk fwalk getenv putenv cpu_count getcwd getpid getppid kill
system linesep sep pathsep`; `os.environ`.
- `os.path`: `join basename dirname split splitext normpath abspath realpath
relpath expanduser expandvars normcase isabs commonpath exists isfile isdir islink`.
- `pathlib.Path`: `name stem suffix parts parent joinpath with_name with_suffix
is_absolute as_posix match stat exists is_file is_dir open read_text read_bytes
write_text write_bytes iterdir glob rglob absolute resolve touch mkdir rmdir
unlink rename cwd home`.
- `glob`: `glob iglob escape` (`* ? [] **`).
- `io`: `open` modes `r w x a + b t`; `read readline readlines write writelines
seek tell truncate flush close`; `StringIO BytesIO`.
- `sys`: `argv modules path platform version version_info stdin stdout stderr
maxsize exit exc_info getsizeof`.
- `json`: `dumps loads dump load` + `ensure_ascii indent sort_keys skipkeys
separators allow_nan`; `JSONEncoder JSONDecoder`.
- `re`: `compile search match fullmatch findall finditer split sub subn escape`;
  `pattern.search/match/findall/sub/split`; `match.group/groups/start/end/span`;
  named `(?P<name>)`; flags `I M S X A U L`.
- `base64`: `b64encode b64decode urlsafe_b64encode urlsafe_b64decode b32encode
b32decode b16encode b16decode b85/a85`.
- `hashlib`: `md5 sha1 sha224 sha256 sha384 sha512 sha3 shake blake2`; `new
update digest hexdigest copy`; `digest_size block_size name`.
- `hmac`: `new update digest hexdigest`; `compare_digest`.
- `secrets`: `token_bytes token_hex token_urlsafe randbelow compare_digest`.
- `csv`: `reader writer DictReader DictWriter`; `writerow writerows read_all
fieldnames`; dialects + `delimiter quotechar escapechar`.
- `zlib/gzip/tarfile/zipfile`: `compress decompress compressobj decompressobj` /
  `open compress decompress` / `TarFile.add extract extractall getmember` /
  `ZipFile.read write namelist extract extractall`.
- `sqlite3`: `connect Connection.execute cursor fetchone fetchall executemany
commit rollback`; DB-API 2.0.
- `socket/http.client/urllib/ssl`: `socket bind listen accept connect send recv
sendall`; `HTTPConnection.request getresponse`; `urlopen urlparse quote unquote`;
  `SSLContext wrap_socket`.
- `argparse`: `ArgumentParser add_argument parse_args`; `GetoptError`.
- `collections`: `deque defaultdict Counter namedtuple OrderedDict` (+ `abc`).
- `itertools`: `count cycle repeat accumulate product permutations combinations
chain groupby starmap islice zip_longest pairwise batched takewhile dropwhile
filterfalse compress tee`.

---

## Appendix B — Go stdlib (survey highlights)

_Module / purpose / key API — the exact reference for Phases 2-7,9._

- `math`: `Abs Ceil Floor Trunc Round Sqrt Cbrt Pow Exp Log Log10 Log2 Sin Cos Tan
Asin Acos Atan Atan2 Hypot Max Min Mod Remainder`; consts `Pi E Sqrt2 Inf NaN`.
- `math/rand`: `Seed Intn Int Float64 Perm Shuffle NormFloat64 ExpFloat64`.
- `time`: `Now Since Until Sleep Unix UnixMilli Date Duration Parse Format After Tick
NewTimer RFC3339`.
- `os`: `Getenv Setenv Getwd Chdir Mkdir MkdirAll Remove RemoveAll Rename Open
Create ReadFile WriteFile ReadDir Stat Getpid Exit Args Hostname CreateTemp
UserHomeDir`.
- `path/filepath`: `Join Split Base Dir Ext Clean Abs Rel Glob Walk EvalSymlinks
IsAbs VolumeName`.
- `io/bufio/fmt`: `ReadAll WriteString Copy EOF Reader Writer Scanner SplitLines
ScanWords ScanBytes ReadLine`; `Printf Sprintf Fprintf Sprint Println Errorf`.
- `strings/strconv`: `Contains HasPrefix HasSuffix Index LastIndex Replace Split
Join Trim TrimSpace ToUpper ToLower Repeat Fields Title`; `Itoa Atoi ParseInt
ParseFloat FormatFloat Quote`.
- `sort/slices`: `Sort Search IsSorted Reverse`; `slices.Sort Contains Index`.
- `container/heap, container/list`.
- `encoding/json`: `Marshal Unmarshal MarshalIndent NewEncoder NewDecoder Valid` +
  `struct tags` + `Marshaler/Unmarshaler`.
- `encoding/base64, encoding/hex`: `EncodeToString DecodeString`.
- `regexp`: `Compile Match MatchString FindString FindAllString ReplaceAllString
Split FindStringSubmatch`.
- `hash/crypto`: `hash.Hash`, `md5.New sha256.New sha512.New` (`Write Sum`),
  `sha1`, `aes`, `des`, `rsa`, `ecdsa`, `hmac.New`, `crypto/rand.Read`.
- `net/net/http/net/url/net/smtp`: `TcpListener TcpStream UdpSocket`,
  `http.Get Post NewRequest ServeMux NewServeMux Handle HandlerFunc ListenAndServe`,
  `url.Parse Query`, `smtp.SendMail`.
- `compress/zlib gzip; archive/zip tar`: `compress.NewWriter inflate`; `zip.NewWriter
w.Create w.Close`, `tar.NewWriter`.
- `database/sql`: `Open Query Exec Prepare`, `Rows.Scan`.
- `os/exec`: `Command Run Output LookPath`, `Cmd.Stdin/Stdout/Stderr/Env`.
- `sync`: `Mutex RWMutex Once WaitGroup atomic`.
- `flag/testing/log/text/template`: `flag.String Bool Int Parse`; `testing.B`; `log
Printf Println Fatal New SetOutput`; `template.Must New Parse Execute`.

---

## Appendix C — Rust std (survey highlights)

_Module / purpose / key API — the exact reference for Phases 2,3,8, sync-deferral._

- `std::f64/f32`: `abs sqrt cbrt powi powf exp exp2 exp_m1 ln log log2 log10 ln_1p
sin cos tan asin acos atan atan2 sinh cosh tanh floor ceil round trunc fract
div_euclid rem_euclid mul_add hypot min max clamp is_nan is_infinite is_finite
to_degrees to_radians`; consts `PI TAU E SQRT_2 INFINITY NEG_INFINITY NAN`.
- `std::time`: `Instant::now elapsed duration_since Duration::new from_secs
from_millis from_nanos as_secs as_millis as_nanos subsec_nanos add sub
SystemTime UNIX_EPOCH`.
- `std::collections`: `HashMap HashSet BTreeMap VecDeque BinaryHeap LinkedList`:
  `insert get contains_key remove keys values iter entry len is_empty clear
push/pop/front/back rotate append split_off into_sorted_vec union intersection
difference`.
- `std::env`: `args args_os vars var var_os current_dir set_current_dir temp_dir
home_dir current_exe join_paths split_paths` (NOTE: set_var removed on this branch).
- `std::fs`: `read read_to_string write copy create_dir create_dir_all remove_dir
remove_dir_all remove_file rename metadata canonicalize read_dir exists`; `File
Metadata.len is_file is_dir is_symlink modified`; `DirEntry.path file_name`.
- `std::path`: `Path.join file_name parent extension file_stem is_absolute
components to_str as_os_str exists is_file is_dir`; `PathBuf.push pop set_file_name
set_extension`.
- `std::io`: `Read/Write/BufRead/Seek` traits; `read_to_end read_to_string
read_line lines`; `BufReader BufWriter stdout stdin stderr copy`.
- `std::str`: `len is_empty chars contains starts_with ends_with find rfind split
split_whitespace trim trim_start trim_end to_uppercase to_lowercase replace repeat
splitn strip_prefix strip_suffix`.
- `std::string::String`: `push_str push from with_capacity as_str clear pop remove
insert split_off`.
- `std::sync`: `Mutex RwLock Condvar Arc Weak Once atomic AtomicUsize Ordering mpsc
channel Sender Receiver recv try_recv Barrier`.
- `std::thread/process`: `spawn sleep current yield_now Builder JoinHandle`; `Command
spawn output status args env current_dir Stdio exit id`.
- `std::vec::Vec`: `push pop insert remove contains sort sort_by reverse extend len
is_empty iter clear truncate dedup`.
- `std::iter`: `map filter enumerate zip chain take skip rev cycle fold reduce sum
product count any all find position min max collect`.
- `std::option/result`: `Some None Ok Err unwrap map and_then is_some is_ok
unwrap_or`.
- `std::hash/cmp/char/num/array/slice`: hashers; `Ordering min max clamp`; `char
from_u32 to_lowercase to_uppercase is_alphanumeric is_numeric is_digit
is_whitespace`; `checked_* wrapping_* saturating_*`; array/slice `sort
binary_search contains windows chunks reverse`.
- **NOTE:** `crypto` is NOT in Rust std — external crates (`sha2`, `md-5`, `rand`).

---

## Appendix D — Ruby stdlib (survey highlights)

_Module / purpose / key API — the exact reference for Phases 5,6,7,9,13._

- `Math`: `sqrt cbrt log log2 log10 exp expm1 log1p sin cos tan asin acos atan
atan2 sinh cosh tanh hypot frexp ldexp erf erfc gamma lgamma`; `PI E`.
- `Time`: `now at gm/utc local/mktime year month day hour min sec wday yday to_i
to_f utc? localtime utc_offset zone strftime + - <=> iso8601`.
- `Random`: `rand srand Random.new rnd.rand Random.urandom rnd.bytes`.
- `Date/DateTime`: `today new jd civil parse strptime iso8601 year month mday wday
yday + - next next_month prev_month strftime to_time leap? monday?`; `DateTime.now
hour min sec offset`.
- `File/IO`: `open read write binread binwrite readlines foreach exists? file?
directory? delete rename mtime expand_path basename dirname extname split join
stat read gets each_line rewind seek close`.
- `Dir`: `new open glob entries children foreach mkdir rmdir getwd chdir home
exist?`.
- `ENV`: `[] []= fetch keys values each delete key? size empty? to_a`.
- `String`: `length empty? include? start_with? end_with? index rindex split strip
upcase downcase sub gsub reverse [] << concat to_i to_f ord each_char`.
- `Regexp/MatchData`: `Regexp.new === =~ match match? source escape named_captures`;
  `MatchData[0] captures begin end pre_match post_match`.
- `JSON`: `generate parse pretty_generate to_json`.
- `Base64`: `strict_encode64 strict_decode64 urlsafe_encode64 urlsafe_decode64
encode64 decode64`.
- `Digest`: `SHA256.hexdigest/digest/base64digest`, `new update digestsize
digest_length block_length`.
- `SecureRandom`: `bytes hex base64 urlsafe_base64 random_number alphanumeric uuid`.
- `socket/net/http/open-uri/ssl`: `TCPSocket TCPServer UDPSocket accept read write
recvfrom`; `Net::HTTP.get get_response post start`; `URI.open`; `SSLSocket
SSLContext cert key verify_mode`.
- `CSV`: `parse generate foreach open read`; `Row Table`.
- `Zlib`: `Deflate.deflate Inflate.inflate gzip gunzip GzipWriter GzipReader
adler32 crc32`.
- `Tempfile/FileUtils/Pathname`: `Tempfile.new create`; `FileUtils.cp mv rm rm_rf
mkdir_p touch`; `Pathname.join basename dirname extname exist? file? read write`.
- `OptionParser`: `OptionParser.new on parse parse!`.
- `Benchmark`: `realtime measure bm bmbm Tms`.
- `Set`: `new add delete include? size union intersection difference subset?`.
- `Signal/Thread/Mutex/Queue`: `trap kill list`; `Thread.new join value current`;
  `Mutex lock unlock synchronize`; `Queue push pop empty? sized`.

---

## 10. Suggested implementation sequencing (phased summary table)

| Phase | Focus                 | Key modules                                                                                                                   | Depends on |
| ----- | --------------------- | ----------------------------------------------------------------------------------------------------------------------------- | ---------- |
| 0     | Native substrate      | os, bytes, process, sys, socket, crypto, compress, sqlite, bigint, keyboard, console, time breakdown; io truncate/write modes | —          |
| 1     | Fix json + base layer | json fix, errors, io, os, path                                                                                                | 0          |
| 2     | Text + collections    | strings, characters, collections, itertools, sort                                                                             | 1          |
| 3     | Numeric + temporal    | math, random, time, datetime, statistics                                                                                      | 0,2        |
| 4     | Bytes + security      | bytes, base64, hex, hashlib, hmac, secrets, crypto                                                                            | 0,2        |
| 5     | Parsing/serialization | regexp, csv, toml, json options                                                                                               | 1,2,4      |
| 6     | CLI/system            | args, console, log, benchmark, psutil, sys, keyboard                                                                          | 0,1,3      |
| 7     | Networking            | socket, url, http, tls, smtp, wsgi, cookies, mime                                                                             | 0,5        |
| 8     | Big int + PK crypto   | bigint, crypto(RSA/ECC), math big                                                                                             | 0,4        |
| 9     | Compression/archives  | zlib, gzip, zip, tar                                                                                                          | 0,4,1,2    |
| 10    | Text gen              | template, html, yaml(opt)                                                                                                     | 2,5        |
| 11    | Databases             | sqlite3, sql, orm                                                                                                             | 0,2        |
| 12    | Terminal              | keyboard, console raw, term size                                                                                              | 0,6        |
| 13    | FS/process            | fileutils, tempfile, os.exec                                                                                                  | 0,1,3      |
| 14    | Extras                | signal, random/time breadth                                                                                                   | 0,4        |
| 15    | Stretch               | sql/orm, mime, mem, crypto completeness                                                                                       | all        |

> Modules only import from strictly-earlier phases (or explicit same-phase acyclic
> edges), enforced by a `kirn check` cycle detector, so the graph stays acyclic and
> each phase is independently shippable + testable.

---

_This document is a living plan; as substrate capabilities land (Phase 0) the API
inventory in §5 becomes normative and per-phase `*_test.kn` files are the contract._
