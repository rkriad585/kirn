# Coco Standard Library Plan (`STD_LIBS_PLAN.md`)

**Status:** Living design document for building Coco's standard library as real,
importable Coco source modules under `stdlib/lib/`.

**Goal:** A batteries-included, dogfoodable standard library — written in Coco —
that (a) gives the language usable breadth now, (b) acts as the shared **substrate**
for the compiler's own tooling (formatter, LSP, docs), and (c) mirrors
the mature module layouts of the four reference interpreters whose sources live in
`C:\Users\rkriad585\Projects\go-rust-ruby-cpython-source-code`:

| Reference | Stdlib root (surveyed) | Evidence used |
|---|---|---|
| CPython | `cpython-main/Lib`, `cpython-main/Modules` | survey in §Appendix A |
| Go | `go-master/src` (`math`, `os`, `io`, `fmt`, `encoding`, `crypto`, `net`, `sync`, …) | survey in §Appendix B |
| Rust | `rust-main/library` (`std`, `core`, `alloc`) | survey in §Appendix C |
| Ruby | `ruby-master` (`lib/*.rb`, `ext/*`) | survey in §Appendix D |

We borrow **APIs** (exact names, shapes, idioms) from these languages, but implement
everything in **Coco source** layered over the C++ substrate's runtime
(`src/interp/runtime.cpp`). Modules reuse each other freely.

---

## 0. Where we are today (baseline)

Existing Coco-written stdlib modules in `stdlib/lib/` (Phase 6 of `DO_FIRST_PLAN.md`):

| Module | Contents | Native support it sits on |
|---|---|---|
| `lib.core` | `read_file`, `write_file`, `i2s`, `c2s` | `io.open`, File `read/write/close` |
| `lib.collections` | `HashMap` struct; `split`, `join`, `contains`, `unique`, `shuffle`, `rand_float` | runtime dict, list methods |
| `lib.strings` | `contains/starts_with/ends_with/replace/split/join/repeat/reverse/pad_left/pad_right/strip` | string builtins |
| `lib.path` | `join/dirname/basename/extname/stem` | pure Coco (sep `/`) |
| `lib.regexp` | glob `match`, `find`, `contains`, `replace`, `split` | pure Coco (no native engine) |
| `lib.io` | `read_file`, `append_file` | `io.open`, File `read/write/close` |
| `lib.os` | `args`, `getenv`, `exit` | `os.args/getenv/exit` |
| `lib.json` | `dumps` (native), `loads` (recursive-descent parser — **known stack-overflow bug on nested containers**) | native `json.dumps` |
| `lib.math` | `PI`, `E`, `sqrt`, `abs`, `clamp`, `min2`, `max2`, `ipow`, `fpow` | `sqrt/abs` native |
| `lib.time` | `now`, `now_ms`, `sleep`, `ordinal` | `time.now/sleep` |

> **Implementation status (dedup/audit note):** the 10 modules in the table above **already exist
> and pass `coco test`** — they are `[IMPLEMENTED]`, not future work. The phases below that
> re-plan an existing module (Phase 1c `io`, 1d `os`, 1e `path`, 2a `strings`, 3a `math`,
> 3c `time`, 5a `regexp`, 1a `json`) should be read as **"enhance/extend the existing `stdlib/lib/
> <mod>.co`"**, not greenfield builds; each such phase is tagged `[NEEDS-ENHANCE]` at its heading
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
- **Module system:** `import lib.<mod>`; `pub def`/`pub const`/`pub var` and now
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
  primitives. Building these in pure Coco is impossible without new native FFI
  stubs. **This plan therefore includes a small, targeted "native substrate
  extension" phase** (Phase 0) to expose OS/bytes/network/process/crypto via new
  pseudo-modules, keeping Coco modules thin wrappers.
- Pseudo-module self-reference hazard: a module named like a pseudo-module
  (`io`, `os`, `time`, `json`, `math`) must capture the pseudo-module member at load
  time (`priv_xxx = time.now;`) because the consumer's import rebinds the shared
  `globals_` name. This convention is already used and MUST be preserved.
- `coco test` runs every `*_test.co` as a program (rc 0 = PASS); it auto-adds
  `"./stdlib"` to the import dirs (`tools/coco.cpp:1369`).

---

## 1. Design principles

1. **Coco-first.** Modules are *real Coco source* in `stdlib/lib/*.co` with a
   `*_test.co` companion. Native C++ only where Coco cannot express it (OS calls,
   sockets, crypto, compression).
2. **Layered.** Thin native substrate → pure-Coco modules → higher modules compose
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
   transitively imports it); enforce via a `coco check` cycle guard.
3. **Stable, documented public surface.** Every module has a one-line comment header,
   `pub`-exports only, and a `*_test.co` that is green under `coco test`.
4. **Naming mirrors the references** where unambiguous (`math.sqrt`, `os.mkdir`,
   `json.dumps/loads`, `csv.reader/writer`, `base64.b64encode`, `hashlib.sha256`,
   `sqlite3.connect`), but we harmonize to **one canonical form** where the four
   languages diverge (e.g. Go `strings.Join(sep, xs)` vs Python `str.join` → we pick
   `strings.join(xs, sep)` and keep a thin alias).
5. **Type exports work.** Because Phase 6 allows `pub struct/class/enum/trait`, modules
   export types that consumers construct: `collections.HashMap(...)`,
   `datetime.Date(...)`, `sqlite3.Connection(...)`.
6. **Errors are catchable.** Use `raise`/`catch e {}` (Phase 1). Modules raise on
   domain errors (e.g. `math.sqrt(-1)`), missing files, parse failures. `errors`
   module standardizes error values/messages.

---

## 2. Execution model & the native substrate plan (Phase 0)

Most requested modules need OS/`syscalls`/network/crypto/bytes that the current
runtime does **not** expose. To keep the plan honest, Phase 0 extends the C++
runtime's pseudo-modules (the *only* place native primitives live) so every later
phase is a Coco file. New native pseudo-modules:

| Pseudo-module | Native surface (new stubs) | Used by Coco modules |
|---|---|---|
| `os` (extend) | `cwd`, `chdir`, `mkdir`, `mkdir_all`, `rmdir`, `remove`, `rename`, `listdir`, `stat` (→ `os.Stat` map: `size,mtime,is_dir,is_file,mode`), `exist`, `is_dir`, `is_file`, `home`, `tmpdir`, `getpid`, `cpu_count`, `hostname`, `environ` (dict), `getenv/setenv/exit/args` (have) | `os`, `path`, `psutil`, `tempfile`, `fileutils` |
| `bytes` (new) | `Bytes` value + `len`, `get(i)`, `slice`, `concat`, `from_str`, `to_str`, `hex`, `unhex`, `from_base64`, `to_base64`; `Bytes.new` | `bytes`, `base64`, `hex`, `crypto`, `zip`, `tar`, `zlib`, `mem` |
| `process` (new) | `spawn(cmd,args,env,cwd)` → `Process` (`.wait`→status, `.stdout`, `.stderr`, `.pid`, `.kill`); one-shot `run(cmd,args)` → `(status,stdout,stderr)` | `os.exec`, `sys`, `psutil` |
| `sys` (new) | `stdin.read_line`, `stdout.write`, `stderr.write`, `argv`, `platform`, `set_recursion_limit`, `get_recursion_limit`, `version`, `exit` | `sys`, `io`, `console`, `log` |
| `socket` (new) | `TcpListener`, `TcpStream`, `UdpSocket` value types + `bind/listen/accept/connect/send/recv/close/set_timeout/shutdown`; helpers `resolve(host,port)` | `socket`, `http`, `smtp`, `tls` |
| `crypto` (new) | hash update objects: `md5/sha1/sha256/sha512` with `update/hexdigest/digest`; `hmac`; `random_bytes`; `constant_time_eq`; later `aes/rsa` (big-int dependent) | `hashlib`, `hmac`, `secrets`, `base64` |
| `compress` (new) | `deflate(bytes)->bytes`, `inflate(bytes)->bytes`, `b64_crc32`; later `gzip` framing in Coco | `zlib`, `gzip`, `zip`, `tar` |
| `sqlite` (new) | `connect(path)` → `Db` (`.exec(sql)`, `.query(sql)`→list-of-row-tuples, `.close`); optionally link SQLite | `sqlite3` |
| `keyboard` (new) | `read_key(blocking)` → key struct, `capture`/`release` | `keyboard` |
| `getch/console` (new) | raw terminal mode, cursor, `getch()` | `console`, `keyboard` |
| `bigint` (new) | arbitrary-precision integer value + add/sub/mul/div/mod/pow/gcd/`from_str/to_str`/`is_prime`/`mulmod/powmod` | `toml`(big ints), `crypto`(RSA/DH), `random.big`, `math.factorial` |
| `time` (extend) | `local_breakdown(epoch)->map{year,month,day,hour,min,sec,wday,yday}`, `mktime(fields)`, `monotonic`, `now_ns`, `format` | `time`, `datetime` |

> **vs.** some designs put everything in C++ builtins; here the native layer stays
> ~15 thin modules so the *policy*, *formatting*, and *logic* live in Coco
> (proving the substrate is usable by the compiler's own tooling).

**Why Phase 0 first:** nearly every later phase depends on `os`, `bytes`, `path`,
`io`, `sys` being real. Without truncate/stat/listdir, `os`/`path`/`fileutils`/
`csv`/`zip`/`tar` cannot be written honestly.

---

## Phase 0 — Native substrate extension (`build/native`)

**Goal:** add the pseudo-modules/values of §2 + `io` fill gaps (`File.write` modes
`w/a`, `truncate`), all in `src/interp/runtime.cpp`, with `*_test.co` smoke tests.

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

**Exit (Phase 0):** all new pseudo-modules emit `*_test.co` green; existing
`collections/io/os/time/math/json` untouched behavior; `coco test` recursive green.

**Dependencies:** none (foundation). **Consumed by:** everything downstream.

---

## Phase 1 — Fix `json`, add `errors` + `io` + `os` + `path` real versions

> **Status/dedup:** `lib.json`, `lib.io`, `lib.os`, `lib.path` **already exist** (§0 table).
> **1a `json` = `[NEEDS-ENHANCE]`** (the `loads` recursion bug is real and open). **1c `io`, 1d
> `os`, 1e `path` = `[NEEDS-ENHANCE]`** (extend the existing modules' function set; the pure-Coco
> `path`/`collections` helpers already cover much of 1e). **1b `errors` = PENDING** (greenfield).

**Goal:** stabilize the substrate layer used by everything else and fix the known
`json.loads` recursion bug.

### 1a. Fix `json.loads` (blocking)
The recursive-descent parser currently stack-overflows on nested containers
(`{"a":[1]}`, `[[1]]`). Root-cause verified: **self-mutation DOES propagate across
recursive struct method calls** (top-level and recursive write-back works), so the
parser logic is correct in isolation (`tools/test_p2.co` parsed `[[1]]` fine) —
meaning the bug is either (i) a stack-depth guard missing (deep nesting), or
(ii) an interaction when the parser runs *loaded as a module*. Action:
- Reproduce minimal module case; if it is depth, add an explicit iteration depth
  cap (raise `json: nested too deep`) before the C++ stack dies.
- If it is module-interaction specific, bisect against `test_p2mod.co`.

```coco
# stdlib/lib/json.co (fix) — depth guard in parse_value
def parse_value(self) {
    self.depth = self.depth + 1;
    if self.depth > 512 { raise("json: nesting too deep"); }
    self.skip_ws();
    c = self.peek();
    ...
}
```

**Exit:** `json.loads`/`dumps` round-trip **nested** dict/list/str/float/bool/nil;
`json_test.co` covers `{"a":[1]}`, `[[1]]`, `{"nested":{"k":"v"}}`, escaped strings,
unicode, `NaN`/`inf` optional.

### 1b. `lib.errors`
Canonical error values + sentinels (Python `errors`, Go `errors`).
```coco
pub def new(msg: string) -> string? { return msg; }
pub const NotFound: string = "not found";
pub const Exists: string = "already exists";
pub const PermissionDenied: string = "permission denied";
pub const InvalidJson: string = "invalid json";
pub def message(e) -> string { return str(e); }
pub def is_error(e) -> bool { return e is not nil; }
```
Hmm — Coco module `errors` shadows nothing; fine. Consumers: `os`, `io`, `path`.

### 1c. `lib.io` (real, over Phase-0 substrate)
```coco
priv_open = io.open;          # capture pseudo-module (self-shadow guard)
pub def read(path: string) -> string { f = priv_open(path); s = f.read(); f.close(); return s; }
pub def read_text(path) -> string { return read(path); }
pub def read_bytes(path) -> bytes { ... }          # Phase 0 bytes
pub def write(path: string, data: string) { f = priv_open(path); f.write_w(data); f.close(); }
pub def append(path: string, data: string) { f = priv_open(path); f.write_a(data); f.close(); }
pub def read_lines(path: string) -> list { return read(path).split("\n"); }
pub def write_lines(path: string, lines: list) { write(path, strings.join(lines, "\n") + "\n"); }
pub def exists(path) -> bool { return os.exist(path); }     # Phase 0
pub def is_dir(path) -> bool { return os.is_dir(path); }
pub def copy(src, dst) { write(dst, read(src)); }
```

### 1d. `lib.os` (real, over Phase-0 `os`)
```coco
pub def getcwd() -> string { return os.cwd(); }
pub def chdir(p) { os.chdir(p); }
pub def mkdir(p, parents=true) -> bool { if parents { return os.mkdir_all(p); } return os.mkdir(p); }
pub def rmdir(p) { os.rmdir(p); }
pub def remove(p) { os.remove(p); }
pub def rename(src, dst) { os.rename(src, dst); }
pub def listdir(p=".") -> list { return os.listdir(p); }
pub def walk(root) -> list { ... }            # [(dir,[subdirs],[files]), ...], Python-style
pub def env(name, default=nil) { v = os.getenv(name); if v is nil { return default; } return v; }
pub def environ() -> dict { return os.environ(); }
pub def setenv(k, v) { os.setenv(k, v); }
pub def getpid() -> int { return os.getpid(); }
pub def cpu_count() -> int { return os.cpu_count(); }
pub def hostname() -> string { return os.hostname(); }
pub def system(cmd: string) -> int { return process.run(cmd).status; }   # Phase 0 process
pub const linesep: string = "\n";              # platform-adjusted in Phase 0
pub const sep: string = "/";
```
`lib.os.exec`: `run(cmd, args=[])`, `spawn`, `output`, `look_path`.

### 1e. `lib.path` (real, platform separator + glob)
```coco
pub const sep: string = os.sep();
pub def join(parts: list) -> string { ... }       # honor sep on windows ('\\')
pub def abs(p) -> string { return os.realpath(p); }
pub def norm(p) -> string { ... }                 # collapse '..' '.'
pub def is_abs(p) -> bool { ... }
pub def basename(p), dirname(p), extname(p), stem(p) -> string  { ... }
pub def glob(pattern) -> list { ... }             # '*','?','[]','**'
pub def exists(p) -> bool { return os.exist(p); }
pub def expanduser(p) -> string { ... }           # '~' → home
```
Adds `rel`, `commonpath`, `clean` (Go `path/filepath`), `split_drive`.

**Exit:** `json_test`(with nested), `errors_test`, `io_test`, `os_test`, `path_test`
green. **Consumes:** Phase 0. **Consumed by:** all later phases.

---

## Phase 2 — Strings, characters, collections breadth

> **Status/dedup:** **2a `strings` = `[DONE]`** (the listed function set already ships in
> `stdlib/lib/strings.co` — extend only where new names are added). **2b `characters` = PENDING.**
> **2c `collections` = `[NEEDS-ENHANCE]`** (the existing `collections.co` has `HashMap` + helper
> fns; the deque/Counter/ordered-collections TYPES are pending — type model in `DATA_TYPE_PLAN.md`
> Phase 10, module here). **2d/2e `itertools`/sort = PENDING.**

**Goal:** rich text + data-structure modules used by nearly every other module.

### 2a. `lib.strings` (extend; keep existing names)
Add (Python/Go/Ruby convergent set):
```coco
pub def index(s, sub) -> int { return s.index(sub); }          # -1 if absent / Python
pub def rindex(s, sub) -> int { ... }
pub def find_all(s, sub) -> list { ... }                       # all start indices
pub def count(s, sub) -> int { ... }
pub def capitalize(s) -> string { ... }
pub def title(s) -> string { ... }                             # Go strings.Title
pub def swapcase(s) -> string { ... }
pub def fields(s) -> list { ... }                              # split on whitespace (Go Fields)
pub def split_whitespace(s) -> list { return fields(s); }
pub def strip_prefix(s, pre) -> string? { ... }                # Rust strip_prefix
pub def strip_suffix(s, suf) -> string? { ... }
pub def byte_len(s) -> int { return s.len(); }
pub def char_count(s) -> int { return s.chars().len(); }
pub def sub(s, start, end) -> string { return s[start..end]; } # Go substring
pub def index_of_char(s, c) -> int { ... }
pub def is_whitespace(s) -> bool { return s.trim() == ""; }
pub def lower(s), upper(s) -> string                        # aliases to builtins
pub def format(fmt: string, args: list) -> string { ... }     # printf → string (Go fmt.Sprintf)
```
Conversions to/from bytes live in `lib.bytes`.

### 2b. `lib.characters` (new) — Unicode classification
```coco
pub def is_digit(c) -> bool { ... }       # '0'..'9'
pub def is_alpha(c) -> bool { ... }
pub def is_alnum(c) -> bool { ... }
pub def is_space(c) -> bool { ... }
pub def is_upper(c)/is_lower(c) -> bool { ... }
pub def to_upper(c)/to_lower(c) -> string { ... }
pub def ord(c) -> int { return ord(c); }
pub def chr(n) -> string { return chr(n); }
```
(Python `str.isdigit`/`isalpha`; Rust `char::is_*`.)

### 2c. `lib.collections` (extend with `HashSet`, `Deque`, `Counter`, `SortedList`)
```coco
pub struct HashSet {
    var buckets: dict;
    def add(self, v) { self.buckets[str(v)] = true; }
    def has(self, v) -> bool { return str(v) in self.buckets; }
    def remove(self, v) { if str(v) in self.buckets { self.buckets.remove(str(v)); } }
    def size(self) -> int { return self.buckets.len(); }
    def to_list(self) -> list { return self.buckets.keys(); }
    def union(self, o), intersection(self, o), difference(self, o) -> HashSet { ... }
}
pub struct Deque {                                   # Python collections.deque
    var items: list;
    var head: int;
    def append(self, v) { self.items.append(v); }
    def append_left(self, v) { ... }
    def pop(self) { ... } def popleft(self) { ... }
    def rotate(self, n) { ... }
    def len(self) -> int { ... } def get(self, i) { ... }
}
pub struct Counter {                                 # Python Counter
    var counts: dict;
    def add(self, v) { ... } def get(self, v) { ... }
    def most_common(self, n) -> list { ... }         # [(k,count), ...]
    def total(self) -> int { ... }
}
pub def chain(xs, ys) -> list, flatten(ls) -> list, range_step(a,b,step) -> list { ... }
pub def zip2(a, b) -> list { ... }                   # list of 2-tuples (Rust/Go iter.zip)
pub def dedup(xs) -> list { ... }
pub def binary_search(xs, v) -> int { ... }          # index or -1 (Go sort.Search)
pub def partition(xs, pred) -> (list, list) { ... }
```

### 2d. `lib.itertools` (new; Python itertools + Rust Iterator adapters)
```coco
pub def chain(xs) -> list, product(xs) -> list, permutations(xs, r) -> list,
       combinations(xs, r) -> list, cycle(xs) -> list, repeat(v, n) -> list,
       accumulate(xs) -> list, groupby(xs, key) -> list, pairwise(xs) -> list,
       starmap(f, xs) -> list, count(start, step) -> list, islice(xs, a, b) -> list,
       zip_longest(a, b) -> list, take(n, xs) -> list, drop(n, xs) -> list,
       takewhile(pred, xs), dropwhile(pred, xs), tee(xs) -> (list, list)
```

### 2e. `lib.sort` (new; Go sort + Rust slice::sort)
```coco
pub def sort(xs: list) -> list { return sorted(xs); }
pub def sort_by(xs, key) -> list { ... }             # key = function
pub def search(xs, v) -> int { ... }                 # Go sort.Search → insertion point
pub def is_sorted(xs) -> bool { ... }
pub def reverse(xs) -> list { return reversed(xs); }
pub def stable_sort(xs) -> list { ... }
```

**Exit:** `strings_test`, `characters_test`, `collections_test`, `itertools_test`,
`sort_test` green. **Consumes:** Phase 1 (`os` not needed here). **Consumed by:**
`csv`, `regexp`, `path`, `args`, `toml`, `html`, `template`, nearly everything.

---

## Phase 3 — Time, DateTime, math, random, statistics

> **Status/dedup:** **3a `math` and 3c `time` = `[NEEDS-ENHANCE]`** (extend the existing
> `stdlib/lib/math.co` and `time.co`). The **builtin names** for math/random are owned by
> `EXP_PLAN.md` Phases 1 & 15 (→); the **float/time TYPES** by `DATA_TYPE_PLAN.md` Phases 2 & 13
> (→). **3b `random`, 3d `datetime`, 3e `statistics` = PENDING** (greenfield modules).

**Goal:** numeric + temporal modules (Python `datetime/time/math/random/statistics`,
Go `time/math/rand`, Rust `std::time/f64`, Ruby `Time/Date/Math`).

### 3a. `lib.math` (extend to full reference surface)
Import with `import lib.math;` uses **qualified** `math.sqrt(...)` so it does not
shadow the native `math` pseudo-module; no self-shadow hazard.
```coco
pub const PI: float = 3.1415926535897932384;
pub const E: float = 2.7182818284590452353;
pub const TAU: float = 6.2831853071795864769;
pub fn-ish ...
pub def sqrt(x) -> float { ... }        # have (Newton)
pub def cbrt(x) -> float { ... }
pub def pow(base, exp) -> float { ... } # fpow + float result for float exp
pub def exp(x)/exp2(x)/expm1(x) -> float { ... }
pub def log(x)/log2(x)/log10(x)/log1p(x) -> float { ... }
pub def sin/cos/tan(x) -> float { if x < 0 { x = x + TAU; } ... }  # native math.sin
pub def asin/acos/atan(x), atan2(y, x) -> float { ... }
pub def sinh/cosh/tanh(x), asinh/acosh/atanh(x) -> float { ... }
pub def hypot(a, b) -> float { return sqrt(a*a + b*b); }
pub def floor/ceil/trunc/round(x) -> int { ... }
pub def fabs(x) -> float { return abs(x); }
pub def copysign(x, y) -> float { ... }
pub def fmod(a, b) -> float { ... }
pub def isclose(a, b) -> bool { d = a - b; if d < 0 { d = -d; } return d < 1e-9; }
pub def isnan(x) -> bool { return x != x; }
pub def isinf(x) -> bool { return x == inf or x == -inf; }
pub def isfinite(x) -> bool { return not (isnan(x) or isinf(x)); }
pub def degrees(r) -> float { return r * 180.0 / PI; }
pub def radians(d) -> float { return d * PI / 180.0; }
pub def gcd(a, b) -> int { while b != 0 { t = b; b = a % b; a = t; } return abs(a); }
pub def lcm(a, b) -> int { if a == 0 or b == 0 { return 0; } return abs(a / gcd(a, b) * b); }
pub def factorial(n) -> int { r = 1; i = 2; while i <= n { r = r * i; i = i + 1; } return r; }   # big via bigint later
pub def comb(n, k) -> int { ... }
pub def clamp(x, lo, hi) -> float { ... }   # have
pub def min2/max2(a, b) { ... }             # have
pub def fsum(xs) -> float { s = 0.0; for x in xs { s = s + float(x); } return s; }
```
> `math` free functions overlap with global builtins (`sqrt`, `abs`). Keep the
> module-qualified forms (`math.sqrt`) and, where a module-internal use needs the
> global, capture at load: `priv_sqrt = sqrt;`.

### 3b. `lib.random` (new; Python `random`, Go `math/rand`, Ruby `Random`)
Needs a **PRNG**. Phase 0 `crypto.random_bytes` gives entropy seeds; implement
deterministic PRNG in Coco (Mersenne-Twister or xorshift128+ / PCG).
```coco
pub struct Rng {
    var state: int;
    def init(self, seed: int) { self.state = seed; }
    def next(self) -> int { ... }                 # 32-bit value from the PRNG
    def random(self) -> float { return float(self.next() % 100000) / 100000.0; }  # [0,1)
    def randrange(self, lo, hi) -> int { ... }
    def choice(self, xs) { ... }
    def shuffle(self, xs) -> list { ... }
    def sample(self, xs, k) -> list { ... }
    def gauss(self) -> float { ... }              # Box-Muller
}
pub def seed(n) { ... }
pub def random() -> float { ... }                 # module-level default Rng
pub def randint(a, b) -> int { ... }
pub def choice(xs) { ... }
pub def shuffle(xs) -> list { ... }
```

### 3c. `lib.time` (extend; Python `time`, Rust `std::time`, Go `time`)
```coco
pub def now() -> float { return float(time.now()) / 1000.0; }   # have
pub def now_ms() -> int { return time.now(); }                   # have
pub def monotonic() -> float { return time.monotonic(); }        # Phase 0
pub def localtime() -> dict { return time.local_breakdown(time.now()); }  # Phase 0: {year,month,day,hour,min,sec,...}
pub def gmtime() -> dict { ... }
pub def mktime(fields: dict) -> int { return time.mktime(fields); }
pub def strftime(fmt: string, t) -> string { ... }               # %Y %m %d %H %M %S map + apply
def pad2(n) -> string { if n < 10 { return "0" + str(n); } return str(n); }
pub def format_iso(t) -> string { f = localtime(); return str(f.year) + "-" + pad2(f.month) + "-" + pad2(f.day); }
```
Keep `sleep`, `ordinal`.

### 3d. `lib.datetime` (new; Python `datetime`, Ruby `Date/DateTime`, Go `time.Date`)
```coco
pub struct Date {
    var year: int; var month: int; var day: int;
    def init(self, y, m, d) { self.year = y; self.month = m; self.day = d; }
    def is_leap(self) -> bool { y = self.year; return (y % 4 == 0 and y % 100 != 0) or y % 400 == 0; }
    def days_in_month(self) -> int { ... }
    def weekday(self) -> int { ... }            # 0=Mon..6=Sun (Python weekday)
    def to_ordinal(self) -> int { ... }         # days since 0001-01-01
    def add_days(self, n) -> Date { ... }
    def iso(self) -> string { return str(self.year) + "-" + pad2(self.month) + "-" + pad2(self.day); }
    def strftime(self, fmt) -> string { ... }
}
pub def today() -> Date { f = time.local_breakdown(time.now()); return Date(f.year, f.month, f.day); }
pub def parse(s) -> Date { ... }                # YYYY-MM-DD
pub def from_ordinal(n) -> Date { ... }
pub def add_months(d, n) -> Date { ... }

pub struct DateTime {                            # date + time + tz offset minutes
    var year, month, day, hour, min, sec, tz_min: int;
    ...
    def to_epoch(self) -> int { ... }
    def iso(self) -> string { return self.date_iso() + "T" + self.time_iso(); }
}
pub def now() -> DateTime { ... }
pub def from_epoch(sec) -> DateTime { ... }

pub struct Duration {                            # Rust std::time::Duration
    var secs: int; var nanos: int;
    def from_secs(s) -> Duration { return Duration(s, 0); }
    def from_millis(ms) -> Duration { return Duration(ms / 1000, (ms % 1000) * 1000000); }
    def as_secs(self) -> int { ... } def as_millis(self) -> int { ... } def as_nanos(self) -> int { ... }
    def add(self, o) -> Duration { ... } def sub(self, o) -> Duration { ... }
}
```

### 3e. `lib.statistics` (new; Python `statistics`)
```coco
pub def mean(xs) -> float { s = 0.0; for x in xs { s = s + float(x); } return s / float(xs.len()); }
pub def median(xs) -> float { xs2 = sorted(xs); n = xs2.len(); if n % 2 == 1 { return float(xs2[n / 2]); } return (float(xs2[n/2-1]) + float(xs2[n/2])) / 2.0; }
pub def mode(xs) { m = collections.Counter(); for x in xs { m.add(x); } return m.most_common(1)[0][0]; }
pub def stdev(xs) -> float { ... }
pub def variance(xs) -> float { ... }
pub def quantiles(xs, n=4) -> list { ... }
pub def cov(xs, ys), corr(xs, ys) -> float { ... }
pub def linear_regression(xs, ys) -> (float, float) { ... }
```

**Exit:** `math_test`, `random_test`, `time_test`, `datetime_test`, `statistics_test`
green. **Consumes:** Phase 0 (time breakdown), Phase 2 (collections), `math` core.
**Consumed by:** `psutil`, `benchmark`, `args`, `http` (date headers), `csv`.

---

## Phase 4 — bytes, encoding (base64/hex), hashlib/hmac/secrets, crypto

**Goal:** binary + integrity/security primitives (Python `base64/hashlib/hmac/
secrets/binascii`, Go `encoding/base64/encoding/hex/hash/crypto`, Ruby
`Base64/Digest/SecureRandom`).

### 4a. `lib.bytes` (new)
```coco
pub def new(cap: int=0) -> bytes { return bytes.new(cap); }
pub def from_str(s: string) -> bytes { return bytes.from_str(s); }
pub def from_list(xs: list) -> bytes { ... }
pub def to_str(b: bytes) -> string { return b.to_str(); }
pub def to_list(b: bytes) -> list { ... }
pub def len(b: bytes) -> int { return b.len(); }
pub def concat(a: bytes, b: bytes) -> bytes { return bytes.concat(a, b); }
pub def slice(b: bytes, i, j) -> bytes { return b.slice(i, j); }
pub def get(b: bytes, i) -> int { return b.get(i); }
pub def set(b: bytes, i, v) { b.set(i, v); }
pub def equals(a, b) -> bool { ... }
```

### 4b. `lib.base64` (new; Python `base64`, Go `encoding/base64`, Ruby `Base64`)
```coco
const ALPHABET: string = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
const URL_ALPHABET: string = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
pub def b64encode(data: string, urlsafe=false) -> string { ... }   # uses bytes internally
pub def b64decode(data: string, urlsafe=false) -> string { ... }
pub def hex(a: string) -> string { return bytes.from_str(a).hex(); }   # ≠ hex module
```
> Keep these **pure-Coco string algorithms** (no Phase-0 bytes needed) so they float
> independently; bytes-typed variants in Phase 8 once `bytes` is stable. Actually to
> avoid duplication: `b64encode` operates on `bytes`; provide string convenience.

### 4c. `lib.hex` (new; Python `binascii.hexlify`, Ruby `String#unpack1('H*')`)
```coco
pub def encode(b: bytes) -> string { return b.hex(); }
pub def decode(s: string) -> bytes { return bytes.unhex(s); }
pub def encode_str(s: string) -> string { return bytes.from_str(s).hex(); }
pub def decode_str(s: string) -> string { return bytes.unhex(s).to_str(); }
```

### 4d. `lib.hashlib` (new; Python `hashlib`, Go `hash`, Ruby `Digest`)
```coco
pub struct Hash {
    var algo: string; var h: crypto;   # Phase-0 crypto object
    def init(self, name: string) { self.algo = name; self.h = crypto.digest(name); }
    def update(self, data: string) { self.h.update(data); }
    def hexdigest(self) -> string { return self.h.hexdigest(); }
    def digest(self) -> bytes { return self.h.digest(); }
    def copy(self) -> Hash { ... }
}
pub def md5(data="") -> Hash { h = Hash(); h.init("md5"); if data != "" { h.update(data); } return h; }
pub def sha1(data="") -> Hash, sha224(...), sha256(...), sha384(...), sha512(...) -> Hash { ... }
pub def new(name: string) -> Hash { ... }
```

### 4e. `lib.hmac` (new; Python `hmac`) & `lib.secrets` (Python `secrets`, Ruby `SecureRandom`)
```coco
# hmac
pub def new(key: string, msg="") -> Hmac { ... }
pub def hmac_sha256(key: string, msg: string) -> string { ... }
# secrets (crypto.random_bytes = Phase 0, OS entropy)
pub def token_bytes(n) -> string { return crypto.random_bytes(n).to_str(); }
pub def token_hex(n) -> string { return hex.encode_str(secrets_bytes...); }
pub def compare_digest(a, b) -> bool { return crypto.constant_time_eq(a, b); }
```

### 4f. `lib.crypto` (new; Go `crypto/*`, Rust crates note, OpenSSL)
Symmetric + asymmetric primitives over Phase-0 `crypto`/`bigint`.
```coco
# symmetric (AES/ChaCha via substrate)
pub def aes_encrypt(key, data) -> bytes, aes_decrypt(...) -> bytes { ... }
pub def chacha20(key, nonce, data) -> bytes { ... }
# asymmetric (RSA) over bigint
pub struct RsaKey { var n, e, d: bigint; def encrypt(self, m), decrypt(self, c), sign, verify ... }
pub def generate_rsa(bits) -> RsaKey { ... }
pub def sha256_hmac(key, msg) -> string { ... }
pub def random_prime(bits) -> bigint, random_number(n) -> bigint { ... }
```
**Exit:** `bytes_test`, `base64_test`, `hex_test`, `hashlib_test`, `hmac_test`,
`secrets_test`, `crypto_test` green. **Consumes:** Phase 0 (bytes, crypto, bigint),
Phase 2 (strings). **Consumed by:** `zip`(crc32), `tar`, `http`(tls), `toml`(hash),
`args`(hash passwords), `crypto`'s own RSA.

---

## Phase 5 — regexp, glob, csv, toml, json breadth

> **Status/dedup:** **5a `regexp` = `[NEEDS-ENHANCE]`** (the existing `stdlib/lib/regexp.co` is a
> glob-only subset; the real NFA/backtracking `Pattern`/`Match` engine is the open work here). Note:
> this plan (Phase 0+5a) proposes the regexp engine as **pure Coco**, whereas `PLAN.md` Phase 7.2
> proposed a C++ RE2-style module — **pick one owner; recommendation: keep the pure-Coco approach
> here** (better dogfooding; amend `PLAN.md`). **5b–5e (csv/toml/json breadth) = PENDING.**

**Goal:** parsing/serialization modules (Python `re/csv/tomllib/json`, Go
`regexp/encoding/csv`, Ruby `Regexp/CSV`, `encoding/…`).

### 5a. `lib.regexp` (real engine — Phase 5, replacing glob subset)
Implement a **backtracking NFA** in pure Coco over 1-char substrings (careful: the
substrate has no native regex; do it in Coco). Keep existing `match/find/split/
replace` signatures; add:
```coco
pub struct Pattern {
    var src: string;
    def init(self, p) { self.src = p; }
    def search(self, s) -> Match? { ... }        # first match or nil
    def findall(self, s) -> list { ... }
    def sub(self, s, repl) -> string { ... }
    def split(self, s) -> list { ... }
    def test(self, s) -> bool { ... }
}
pub struct Match {
    var start: int; var end: int; var groups: list;
    def span(self) -> (int, int) { return (self.start, self.end); }
    def group(self, i) { ... }
}
pub def compile(p) -> Pattern { return Pattern(p); }
pub def search(pat, s) -> Match? { ... }
pub def match(pat, s) -> bool { ... }            # keep glob-compatible (used by path.glob)
pub def findall(pat, s) -> list, sub(pat, s, repl) -> string, split(pat, s) -> list { ... }
```
Syntax subset: `.` `*` `+` `?` `|` `()` `[]` `\d \w \s` `^` `$` `{n,m}` `\b`.

### 5b. `lib.csv` (new; Python `csv`, Go `encoding/csv`, Ruby `CSV`)
```coco
pub struct Reader {
    var data: list; var sep: string; var quote: string;
    var rows: list; var i: int;
    def init(self, text, sep=",", quote="\"") { self.parse(text); }
    def parse(self, text) { ... }
    def read_row(self) -> list? { ... }
    def read_all(self) -> list { return self.rows; }
}
pub struct Writer {
    var buf: string; var sep: string; var quote: string;
    def init(self, sep=",", quote="\"") { self.buf = ""; }
    def write_row(self, row: list) { ... }
    def to_string(self) -> string { return self.buf; }
}
pub def reader(text, sep=",") -> Reader { ... }
pub def writer(sep=",") -> Writer { ... }
pub def parse(text, sep=",") -> list { return reader(text, sep).read_all(); }
pub def generate(rows: list, sep=",") -> string { ... }
```

### 5c. `lib.toml` (new; Python `tomllib`, Go `BurntSushi/toml` pattern, Rust `toml`; Ruby has **none**, note)
```coco
pub def loads(s: string) -> dict { ... }        # tables, arrays-of-tables, inline, dotted
pub def dumps(d: dict) -> string { ... }
pub def load(path) -> dict { return loads(io.read(path)); }
pub def dump(d: dict, path) { io.write(path, dumps(d)); }
```
Support scalars (int/float/bool/string/datetime), `[table]`, `[[array.of.tables]]`,
inline `{a=1}`, dotted `a.b.c`.

### 5d. `lib.json` (extend; add options)
```coco
pub def dumps(v) -> string { return priv_dumps(v); }        # native
pub def loads(s) { ... }                                     # fixed parser (Phase 1)
# options (optional, does not break existing):
pub def dumps_pretty(v) -> string { ... }                    # indent 2
pub def loads_from_file(path) { return loads(io.read(path)); }
pub def dumps_to_file(path, v) { io.write(path, dumps(v)); }
pub def valid(s) -> bool { try { loads(s); return true; } catch _ { return false; } }
```
**Exit:** `regexp_test`, `csv_test`, `toml_test`, `json_test` green.
**Consumes:** Phase 1 (`io`), Phase 2 (`strings`), Phase 4 (`bytes` for toml datetime
optional). **Consumed by:** `http`, `args`(config), `psutil`, `template`, `yaml`.

---

## Phase 6 — args, console, log, benchmark, psutil, sys

**Goal:** CLI/observability/system modules (Python `argparse/sys/logging/
platform`, Go `flag/log/testing`, Ruby `OptionParser/Benchmark`, Rust `env/process`).

### 6a. `lib.args` (new; Python `argparse`, Go `flag`, Ruby `OptionParser`)
```coco
pub class ArgumentParser {
    var prog: string; var description: string;
    var specs: list;                      # [{flags, dest, type, default, nargs, help}]
    var values: dict;
    def __init__(self, prog="", description="") { ... }   # or class + def init
    def add(self, flags: list, dest="", type="str", default=nil, nargs=1, help="") {
        self.specs.append({flags: flags, dest: dest, type: type, default: default, nargs: nargs, help: help});
    }
    def parse(self, argv: list) { ... }   # fills self.values
    def get(self, name) { return self.values[name]; }
    def help_text(self) -> string { ... }
}
pub def getopt(argv: list, short: string, long: list) -> (list, list) { ... }   # Python getopt
```

### 6b. `lib.console` (new; Ruby/`getch`, Go `github.com/eiannone/keyboard`-style)
```coco
pub def getch() -> string { return keyboard.read_key(true).to_str(); }   # Phase 0 keyboard
pub def clear() { print("\x1b[2J\x1b[H"); }
pub def set_color(code) { printf("\x1b[%dm", code); }
pub def reset() { printf("\x1b[0m"); }
pub def read_line(prompt) -> string { ... }
pub def input(prompt) -> string { ... }        # Python input
```

### 6c. `lib.keyboard` (new)
```coco
pub struct Key { var code: int; var name: string; var shift: bool; var ctrl: bool; }
pub def read_key(blocking=true) -> Key? { return keyboard.read_key(blocking); }
pub def is_printable(k) -> bool { ... }
```

### 6d. `lib.log` (new; Go `log`, Python `logging` simplified, Rust `log`)
```coco
pub struct Logger {
    var level: string; var prefix: string; var out: string;   # 'stdout'|'stderr'|file
    def init(self, prefix="", out="stderr") { ... }
    def debug(self, msg) { ... } def info(self, msg) { ... }
    def warn(self, msg) { ... } def error(self, msg) { ... }
    def fatal(self, msg) { print msg; os.exit(1); }
}
pub def default() -> Logger { ... }
pub def info(msg), warn(msg), error(msg), debug(msg) { ... }   # module-level to default
```

### 6e. `lib.benchmark` (new; Ruby `Benchmark`, Go `testing.B`, Python `timeit`)
```coco
pub def realtime(f) -> float { t0 = time.monotonic(); f(); return time.monotonic() - t0; }
pub def measure(f) -> string { t = realtime(f); return str(t) + "s"; }
pub def bm(fns: list) -> list { ... }           # [{name,seconds}]
```

### 6f. `lib.psutil` (new; Python `psutil` — system/process statistics)
Over Phase-0 `os`/`process`.
```coco
pub def cpu_percent(interval=0.0) -> float { ... }
pub def cpu_count(logical=true) -> int { return os.cpu_count(); }
pub def virtual_memory() -> dict { ... }        # {total, available, percent}
pub def disk_usage(path) -> dict { ... }
pub def pid_exists(pid) -> bool { ... }
pub def process_iter() -> list { ... }          # [{pid, name, ...}]
pub def boot_time() -> float { ... }
pub def users() -> list { ... }
```

### 6g. `lib.sys` (new; Python `sys`, Go `os`, Rust `std::env/process`, Ruby `ENV`)
```coco
pub def argv() -> list { return os.args(); }
pub def stdin_line() -> string { return sys.stdin_read_line(); }
pub def stdout_write(s) { sys.stdout_write(s); }
pub def stderr_write(s) { sys.stderr_write(s); }
pub def exit(code=0) { os.exit(code); }
pub def platform() -> string { return sys.platform(); }
pub def version() -> string { return sys.version(); }
pub const max_int: int = 9007199254740991;
pub def set_recursion_limit(n) { sys.set_recursion_limit(n); }
pub def get_recursion_limit() -> int { ... }
```

**Exit:** `args_test`, `console_test`, `keyboard_test` (may be interactive; gate by
env var), `log_test`, `benchmark_test`, `psutil_test`, `sys_test` green.
**Consumes:** Phase 0 (os/process/sys/keyboard), Phase 1 (os), Phase 3 (time).
**Consumed by:** `http`(argparse for CLI tools), `tar`, sample apps.

---

## Phase 7 — http, socket, url, tls, smtp, wsgi

**Goal:** networking modules (Python `socket/http/urllib/ssl/wsgiref`, Go
`net/net/http/net/url/net/smtp`, Rust `std::net`, Ruby `socket/net/http/open-uri`).
These sit on the Phase-0 `socket` substrate.

### 7a. `lib.socket` (new)
```coco
pub struct Socket {
    var fd: socket;
    def connect(self, host, port) { ... }
    def send(self, data) -> int { ... }
    def recv(self, n) -> string { ... }
    def recv_line(self) -> string { ... }
    def close(self) { ... }
    def set_timeout(self, ms) { ... }
}
pub struct Server {
    var fd: socket;
    def bind(self, host, port) { ... }
    def listen(self, backlog) { ... }
    def accept(self) -> Socket { ... }
    def close(self) { ... }
}
pub def tcp_connect(host, port) -> Socket { ... }
pub def tcp_server(host, port) -> Server { ... }
pub def resolve(host, port) -> string { return socket.resolve(host, port); }
```

### 7b. `lib.url` (new; Python `urllib.parse`, Go `net/url`, Rust `Url`)
```coco
pub struct Url {
    var scheme, host, path, query, fragment: string;
    var port: int;
    def parse(s) -> Url { ... }
    def to_string(self) -> string { ... }
    def quote(self), unquote(self) -> string { ... }
    def encode_query(self, params: dict) -> string { ... }
}
pub def parse(s) -> Url { return Url.parse(s); }
pub def quote(s) -> string, unquote(s) -> string, encode_form(params) -> string { ... }
```

### 7c. `lib.http` (new; Go `net/http`, Python `http.client/urllib.request`, Ruby `Net::HTTP`)
```coco
pub struct Request {
    var method, url, path, body: string;
    var headers: dict;
}
pub struct Response {
    var status: int; var reason, body: string; var headers: dict;
}
pub def get(url) -> Response { return request("GET", url, "", {}); }
pub def post(url, body) -> Response { return request("POST", url, body, {}); }
pub def request(method, url, body, headers) -> Response { ... }   # over socket + url
pub def client(url) -> HttpClient { ... }                          # connection reuse
pub struct Server {   # minimal HTTP server (Go http.Server / wsgi)
    def route(self, method, path, handler) { ... }
    def listen(self, port) { ... }
    def handle_request(self, sock) -> Response { ... }
}
```

### 7d. `lib.http.cookies` / `lib.http.mime` (new)
```coco
pub def parse_cookie(header) -> dict { ... }
pub def set_cookie(name, value, opts) -> string { ... }
pub def guess_type(path) -> string { ... }    # via extension table
```

### 7e. `lib.tls` (new; Python `ssl`, Go `crypto/tls`, Ruby `OpenSSL::SSL`)
```coco
pub struct TlsContext {
    var verify: bool; var ca_file: string;
    def load_cert(self, cert, key) { ... }
    def wrap(self, sock) -> Socket { ... }
}
pub fn-ish ...
pub def client(host, port, opts) -> Socket { ... }   # TCP + TLS handshake
```

### 7f. `lib.smtp` (new; Go `net/smtp`, Python `smtplib`)
```coco
pub struct Smtp {
    var socket: Socket; var host: string; var port: int; var tls: bool;
    def connect(self, host, port, tls=false) { ... }
    def login(self, user, passwd) -> bool { ... }
    def sendmail(self, from, to: list, msg) -> bool { ... }
    def quit(self) { ... }
}
```

### 7g. `lib.wsgi` (new; Python `wsgiref` — server gateway spec)
Define a portable server/application contract modeling `WSGI`:
```coco
# an app is a function: app(environ: dict) -> (status: int, headers: dict, body: string)
pub def run(app, port) { ... }          # serve app on port (blocks)
pub def make_environ(url, method, headers, body) -> dict { ... }
```

**Exit:** `url_test`, `http_test` (against a local test server), `socket_test`,
`smtp_test`(dependency-gated), `wsgi_test`. These need network; run behind a
`COCO_TEST_NET=1` guard so `coco test` stays hermetic by default.
**Consumes:** Phase 0 (socket), Phase 5 (`json` for REST), Phase 6 (no), Phase 2
(strings), Phase 7's own (url). **Consumed by:** `console`(http clients), sample CLI.

---

## Phase 8 — bigint, math big, crypto RSA/DH (pure-Coco over Phase-0 bigint)

**Goal:** arbitrary-precision integer arithmetic + public-key crypto (Python `int`
is arbitrary precision; Go `math/big`; Rust `num-bigint` crate; PHP `gmp`).
Skip if `bigint` substrate deferred.

### 8a. `lib.bigint` (new; wrapper + helpers)
```coco
pub const BN: string = "__bigint__";
pub def from_str(s) -> bigint { return bigint.from_str(s); }
pub def from_int(n) -> bigint { ... }
pub def to_str(b) -> string { return b.to_str(); }
pub def add(a, b) -> bigint, sub, mul, div, mod, pow, gcd, modinv, is_prime { ... }
pub def compare(a, b) -> int { ... }
pub def to_int(b) -> int { ... }
```
### 8b. `lib.crypto` big-key ops
`generate_rsa`, `rsa_encrypt/decrypt/sign/verify`, `diffie_hellman`, `random_prime`.

**Exit:** `bigint_test`, `crypto_big_test` green. **Consumes:** Phase 0 (bigint),
Phase 4 (crypto). **Consumed by:** `crypto`(RSA), `binascii`-style big ints in `json`/
`toml`, `random.randint` for big.

---

## Phase 9 — compression & archives: zlib, gzip, zip, tar

**Goal:** (Python `zlib/gzip/zipfile/tarfile`, Go `compress/* archive/*`, Ruby
`Zlib`). Sit on Phase-0 `compress`.

### 9a. `lib.zlib` (new)
```coco
pub def compress(data: string, level=-1) -> bytes { return compress.deflate(bytes.from_str(data), level); }
pub def decompress(data: bytes) -> string { return compress.inflate(data).to_str(); }
pub def crc32(data: string) -> int { return compress.b64_crc32(bytes.from_str(data)); }
pub def adler32(data: string) -> int { ... }
```

### 9b. `lib.gzip` (new)
```coco
pub def compress(data: string) -> bytes { ... }    # gzip header + deflate + trailer
pub def decompress(data: bytes) -> string { ... }
pub def read_file(path) -> string { return decompress(bytes.from_str(io.read(path))); }
```

### 9c. `lib.zip` (new; Python `zipfile`, Go `archive/zip`)
```coco
pub struct Entry { var name, data: string; var crc: int; }
pub struct ZipFile {
    var entries: list;
    def read(self) -> list { ... }                 # local + central directory parse
    def names(self) -> list { ... }
    def read_entry(self, name) -> string? { ... }
}
pub def open(path) -> ZipFile { ... }
pub def to_bytes(entries: list) -> bytes { ... }   # build a zip
pub def write(path, entries: list) { io.write_bytes(path, to_bytes(entries)); }
```

### 9d. `lib.tar` (new; Python `tarfile`, Go `archive/tar`)
```coco
pub struct TarEntry { var name, mode, type: string; var data: bytes; }
pub def open(path) -> list { ... }                 # header + body parse (ustar POSIX)
pub def to_bytes(entries: list) -> bytes { ... }
pub def write(path, entries: list) { io.write_bytes(path, to_bytes(entries)); }
```

**Exit:** `zlib_test`, `gzip_test`, `zip_test`, `tar_test` green.
**Consumes:** Phase 0 (compress), Phase 4 (bytes/base64/hex/crc32), Phase 1 (`io`),
Phase 2 (`path`). **Consumed by:** archive tooling, `http`(multipart not needed).

---

## Phase 10 — template, html, yaml

**Goal:** text generation + markup (Go `text/template`/`html/template`, Python
`string.Template`, Ruby `ERB`).

### 10a. `lib.template` (new)
```coco
pub def render(tpl: string, ctx: dict) -> string { ... }   # support {{var}} {{each xs}} {{if}}
pub def compile(tpl) -> (func) -> string { ... }           # precompile
```

### 10b. `lib.html` (new; Python `html`, Go `html`)
```coco
pub def escape(s: string) -> string { ... }      # &, <, >, ", '
pub def unescape(s: string) -> string { ... }
pub def strip_tags(s: string) -> string { ... }
```

### 10c. `lib.yaml` (new; only if desired — Ruby `YAML`/Psych, Python `yaml`
external; Go `gopkg.in/yaml.v3` external)
```coco
pub def dump(v) -> string { ... }
pub def load(s: string) { ... }
pub def load_file(path) { ... }
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
Sits on Phase-0 `sqlite`.

```coco
pub struct Connection {
    var db: sqlite;
    def init(self, path) { self.db = sqlite.connect(path); }
    def close(self) { self.db.close(); }
    def execute(self, sql: string, params: list=[]) { ... }     # rowcount via db.exec
    def query(self, sql: string, params: list=[]) -> list { ... }  # list of row-tuples + column names
    def query_one(self, sql, params) -> tuple? { ... }
    def last_insert_id(self) -> int { ... }
    def transaction(self, fn) { self.db.exec("BEGIN"); try { fn(); self.db.exec("COMMIT"); } catch e { self.db.exec("ROLLBACK"); raise e; } }
}
pub def connect(path) -> Connection { return Connection(path); }
```

**Exit:** `sqlite3_test` green (temp in-memory/`:memory:`). **Consumes:** Phase 0
(sqlite), Phase 2 (strings). **Consumed by:** sample CRUD apps, `psutil`(metrics db).

---

## Phase 12 — keyboard polish, console raw-mode, term input (final I/O ergonomics)

Refine `keyboard`/`console` with raw terminal, termios-style modes:
```coco
# console raw mode (Phase 0 getch already gives keys)
pub def raw_mode(on: bool) { ... }
pub def get_key() -> Key { ... }        # returns printable char or name ('UP','F1')
pub def term_size() -> (int, int) { ... }
pub def read_password(prompt) -> string { ... }   # no echo
```
**Exit:** `keyboard_test`, `console_test` green under a driver. **Consumes:**
Phase 0 (getch/console), Phase 6 (keyboard). **Consumed by:** TUI sample.

---

## Phase 13 — `sys`, `os.exec`, `tempfile`, `fileutils`, `shutil`

**Goal:** filesystem + process orchestration (Python `os/shutil/tempfile`, Go
`os/exec`, Ruby `FileUtils/Tempfile`).

### 13a. `lib.fileutils` (new; Ruby `FileUtils`, `shutil`)
```coco
pub def cp(src, dst) { io.copy(src, dst); }
pub def cp_r(src, dst) { ... }                 # recursive
pub def mv(src, dst) { os.rename(src, dst); }
pub def rm(p) { os.remove(p); }
pub def rm_r(p) { ... }                        # recursive delete
pub def mkdir_p(p) { os.mkdir_all(p); }
pub def touch(p) { if not io.exists(p) { io.write(p, ""); } }
pub def rmtree(p) { ... }                      # shutil.rmtree
pub def walk(top) -> list { return os.walk(top); }
pub def copytree(src, dst) { ... }
```

### 13b. `lib.tempfile` (new; Python `tempfile`, Ruby `Tempfile`, Go `os.CreateTemp`)
```coco
pub def mktemp(suffix="", prefix="") -> string { ... }   # os.tmpdir + random
pub def mkdtemp(prefix="") -> string { ... }
pub struct TempFile {
    var path: string; var f: File;
    def write(self, data) { ... } def read(self) -> string { ... } def close(self, delete=true) { ... }
}
pub def NamedTemporaryFile(suffix="") -> TempFile { ... }
```

### 13c. `lib.os.exec` (new; Go `os/exec`, Ruby `Kernel#system`, Python `subprocess`)
```coco
pub struct Process { var pid: int; var status: int; var stdout, stderr: string;
    def wait(self) -> int { ... } def exit_code(self) -> int { return self.status; } }
pub def run(cmd: string, args: list=[]) -> Process { ... }        # blocks
pub def spawn(cmd: string, args: list=[]) -> Process { ... }       # non-blocking
pub def output(cmd: string, args: list=[]) -> string { return run(cmd, args).stdout; }
pub def look_path(cmd: string) -> string? { ... }
```

**Exit:** `fileutils_test`, `tempfile_test`, `os_exec_test` green.
**Consumes:** Phase 0 (os/process), Phase 1 (io/os/path), Phase 3 (random).
**Consumed by:** `http`(cgi), `tar`, sample apps.

---

## Phase 14 — random breadth, monotonics, signal, logging final

**Goal:** remaining runtime-ish modules (Ruby `Signal`, Python `signal/sys`, Go
`os/signal`).
```coco
# lib.signal
pub def trap(sig: string, handler) { os.signal(sig, handler); }
pub def kill(pid, sig="TERM") { os.kill(pid, sig); }
pub def list_signals() -> dict { return os.signal_list(); }
# lib.random additions
pub def randbytes(n) -> bytes, gauss(mu, sigma) -> float, expovariate(l), normal_seed(n) { ... }
# lib.time additions
pub def perf_counter() -> float { return time.perf_counter(); }
pub def process_time() -> float { ... }
```
**Exit:** `signal_test`, `random_test`(extended), `time_test`(extended) green.

---

## Phase 15 — `sql`, `orm`, `mime`, `mem`, `crypto` completeness

Stretch modules (longer lead, team can split):
- `lib.sql` / `lib.orm` — query builders over `sqlite3`.
- `lib.mime` — MIME type table, multipart parser.
- `lib.mem` — the Phase-0 `Arena` plus Coco helpers (`alloc/free_all/reset`).
- `lib.crypto` completeness — Ed25519, ChaCha20-Poly1305, blake2, SHA-3.
- `lib.benchmark`/`lib.profiler` — function-call profiling using `time.process_time`.
- `lib.net.cgi`, `lib.net.websocket` (over socket).

---

## Beyond Phase 15 — future / stretch

- **`ui`/`tui`** — terminal UI widgets on `keyboard`/`console`.
- **`async`** — promise/await over spawn+chan+select.
- **`db`** — full clients (Postgres/MySQL) once sockets + protocol parse land.
- **`ffi`** — `extern def`-based dynamic loading wrappers for host libs.
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
phase with explicit acyclic marker). A `coco check` adds a cycle detector so an
accidental cycle fails CI.

---

## 4. Test strategy

- Each module ships `stdlib/lib/<name>_test.co`; `coco test` runs them all (rc 0).
- **Hermetic default:** modules that touch network/OS-files/`keyboard` are skipped
  (or use temp dirs) unless `COCO_TEST_NET=1` / `COCO_TEST_OS=1`. `*_test.co` guards:
```coco
def main() {
    if os.getenv("COCO_TEST_OS") == nil { print("skipped"); return; }
    ...
}
```
- **Differential tests** against the reference implementations where valuable
  (e.g. `base64`, `hex`, `json`, `csv`, `datetime.iso`, `math` constants): compute
  expected vectors with Python and assert exact equality in Coco.
- `regexp`, `toml`, `csv`, `zip`, `tar` use golden files checked in under
  `stdlib/testdata/`.

---

## 5. Per-module API inventory (curated, cross-language)

The following **canonical Coco APIs** are the agreed surface (names lifted from the
four references, harmonized). This is the source of truth the phases implement.

### math
`PI E TAU inf nan`, `sqrt cbrt pow exp log log2 log10 sin cos tan asin acos atan
atan2 sinh cosh tanh hypot fabs floor ceil trunc round fmod copysign isnan isinf
isfinite degrees radians gcd lcm factorial isqrt comb perm fsum isclose clamp
min2 max2`.
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

```coco
# tools/apitool.co
import lib.args;
import lib.http;
import lib.json;
import lib.sqlite3;
import lib.time;
import lib.log;
import lib.crypto;

def main() {
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

Coco has `spawn`/`chan`/`select`. A `lib.sync` module (Go `sync`: `Mutex/RWMutex/
Once/WaitGroup/Cond`; Rust `std::sync`: `Mutex/RwLock/atomic`) is **deliberately
deferred** until `DO_FIRST_PLAN` Phase 11 ("Concurrency & GC finalize") lands the
safe worker-pool substrate. Design stub here for later:
```coco
pub struct Mutex { var ch: chan; def init(self) { self.ch = chan.buffer(1); }
    def lock(self) { self.ch.send(1); } def unlock(self) { self.ch.recv(); } }
pub struct WaitGroup { ... }
```
> Keep the API names (Mutex/RWMutex/Once/WaitGroup) identical to Go/Rust so the
> substrate drop-in is seamless.

---

## 8. Verification / Definition of Done

For each phase:
1. `coco check` on every module (0 errors; W-lints clean).
2. `coco test` runs the phase's `*_test.co` green.
3. Full `coco test` (all phases) green; no regressions to existing modules.
4. Differential vectors (base64/hex/json/csv/datetime/math) match the reference.
5. Dependency graph acyclic (checked).
6. Modules dogfood: at least one `tools/*.co` real tool (e.g. `apitool.co`) imports
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
7. **Test names:** `<module>_test.co`; net/OS-dependent tests gated by env vars.

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

| Phase | Focus | Key modules | Depends on |
|---|---|---|---|
| 0 | Native substrate | os, bytes, process, sys, socket, crypto, compress, sqlite, bigint, keyboard, console, time breakdown; io truncate/write modes | — |
| 1 | Fix json + base layer | json fix, errors, io, os, path | 0 |
| 2 | Text + collections | strings, characters, collections, itertools, sort | 1 |
| 3 | Numeric + temporal | math, random, time, datetime, statistics | 0,2 |
| 4 | Bytes + security | bytes, base64, hex, hashlib, hmac, secrets, crypto | 0,2 |
| 5 | Parsing/serialization | regexp, csv, toml, json options | 1,2,4 |
| 6 | CLI/system | args, console, log, benchmark, psutil, sys, keyboard | 0,1,3 |
| 7 | Networking | socket, url, http, tls, smtp, wsgi, cookies, mime | 0,5 |
| 8 | Big int + PK crypto | bigint, crypto(RSA/ECC), math big | 0,4 |
| 9 | Compression/archives | zlib, gzip, zip, tar | 0,4,1,2 |
| 10 | Text gen | template, html, yaml(opt) | 2,5 |
| 11 | Databases | sqlite3, sql, orm | 0,2 |
| 12 | Terminal | keyboard, console raw, term size | 0,6 |
| 13 | FS/process | fileutils, tempfile, os.exec | 0,1,3 |
| 14 | Extras | signal, random/time breadth | 0,4 |
| 15 | Stretch | sql/orm, mime, mem, crypto completeness | all |

> Modules only import from strictly-earlier phases (or explicit same-phase acyclic
> edges), enforced by a `coco check` cycle detector, so the graph stays acyclic and
> each phase is independently shippable + testable.

---

_This document is a living plan; as substrate capabilities land (Phase 0) the API
inventory in §5 becomes normative and per-phase `*_test.co` files are the contract._
