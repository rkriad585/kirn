# Kirn POWER PLAN — default-on async runtime, serialization, data parallelism, and ergonomic errors

## Goal

Make Kirn **significantly more powerful out of the box** by folding the five most
valuable capabilities of the Rust ecosystem — `tokio` (async runtime), `serde`
(serialization), `rayon` (data parallelism), `anyhow` (dynamic error ergonomics),
and `thiserror` (typed error derivation) — into Kirn's **default** standard
library and language surface, so a Kirn program gets an async runtime, generic
serialization, data-parallel collections, and ergonomic error handling **without
adding any dependency**. Attribute-driven codegen uses Kirn's `@[...]` bracket
attributes and `@decorator` forms (the Kirn analogue of Rust's `#[...]`, with
Python-decorator semantics). Each capability is specced against the complete Rust
source vendored locally under
`langs-source-code/rust/{tokio,serde,rayon,anyhow,thiserror}` so the plan follows
proven, source-verified designs rather than inventing novelty.

## Must Follow

- **Default-on, zero-dependency.** Async runtime, serialization, data parallelism, and the error types ship in the default stdlib/prelude — no package install, no feature flag. They are the "batteries" of Kirn.
- **Build ships exactly `kirn` + `kirnc`; one owner per topic.** `kirn` and `kirnc` remain the only shipped executables; every capability here is a stdlib module or a compiler feature — never a new executable.
- **No over-engineering; proven patterns only.** Each API must map to a verified reference implementation under `langs-source-code/rust/{tokio,serde,rayon,anyhow,thiserror}`; cite the file. Adopt the idiom, drop the baggage.
- **Kirn-style spellings.** `fn`/`var`, CapCase types (`Str`/`Int`/`list[T]`/`result[T,E]`/`any`), lowercase methods, `ok(...)`/`err(...)` constructors, `import pet.…`. Rust `#[…]` becomes Kirn `@[… ]` (attribute list) or `@name`/`@name(...)` (decorator); keep one spelling per meaning.
- **AI-first.** Provide the exact surface an agent needs to write correct concurrent/async/serializing code: small, regular grammar, one obvious API per concept, exact docstrings, and machine-checkable diagnostics.
- **Gate-aligned.** Every phase exits by running `kirn test` and `scripts/gates_linux.sh` (and `scripts/golden_linux.sh` for observable-behavior changes); the suite stays green (currently 188/0).

## Important Note

This plan is the **owner of the default-on power layer**. It subsumes the stretch/deferred entries that previously sat in `EXP_PLAN.md` (async/await, `@derive`), `SYNTAX_PLAN.md` (decorators `SP-1`/`SP-6`/`SP-15`), `STD_LIBS_PLAN.md` (serialization formats), and `DATA_TYPE_PLAN.md` (error interface): those remain the syntax/data-type _bodies_ that this plan composes, but the async runtime, the serde-style serialization framework, data parallelism, and the ergonomic-error layer are owned here. `SYNTAX_PLAN` still owns the raw `@` grammar; this plan consumes it. Everything below is a plan, not shipped behavior yet — the current `@` operator is pattern-alias only and `async`/`await` are not keywords (`src/lex/lexer.cpp`, `grammar/kirn.ebnf`).

---

## 0. Positioning and the Rust → Kirn mapping

**Goal:** one table that fixes what "Rust crate X becomes Kirn" means, so implementers and agents share a single vocabulary.
**Must Follow:** each row maps an external crate to exactly one Kirn module or keyword set; no row adds an executable; names are Kirn-style.
**Important Note:** the attribute spelling is the linchpin — Rust `#[…]` ⇒ Kirn `@[…]` and/or `@decorator`, both lowering to one decorator list on the declaration.

| Rust reference | What it provides                                                   | Kirn home                           | Kirn surface                                                                     |
| -------------- | ------------------------------------------------------------------ | ----------------------------------- | -------------------------------------------------------------------------------- |
| `tokio`        | async runtime, tasks, timers, async I/O/net/fs, `select!/join!`    | `pet.async` + language keywords     | `async fn`, `await`, `@[main]`, `@[test]`, `Task[T]`, `select`/`join`/`try_join` |
| `serde`        | serialization framework (`Serialize`/`Deserialize`), derive, attrs | `pet.serde` + derive                | `@[derive(Serialize, Deserialize)]`, `@[serde(...)]`                             |
| `rayon`        | data-parallel iterators, work-stealing pool, par sort              | `pet.parallel` + collection methods | `.par_iter()`, `.par_sort()`, `pool(n)`, `join(a,b)`, `scope`                    |
| `anyhow`       | type-erased dynamic error, context, `bail!`/`ensure!`              | `pet.error` + prelude               | `Error`, `result[T]`, `.context(...)`, `bail(...)`, `ensure(...)`                |
| `thiserror`    | typed errors via derive, `#[from]`/`#[source]`                     | `@[derive(Error)]`                  | `@[error("…")]`, `@[from]`, `@[source]`, `@[error(transparent)]`                 |

**Rust attribute → Kirn decorator.** Rust spells meta-programming `#[attr]`, `#[attr(...)]`, `#[derive(...)]`. Kirn spells the same thing two ways, both lowering to an ordered `decorators: list[Decorator]` attached to a declaration:

```kirn
# decorator form (Python-like), stacked top-to-bottom
@logged
@route("/users/{id}")
fn get_user(id: Str) -> User { ... }

# bracket attribute form (Rust-like), comma-separated
@[derive(Serialize, Deserialize), serde(rename_all = "camelCase")]
struct User { user_name: Str, age: Int }

# both forms mix; order of application: bottom-up / left-to-right
@[main]
async fn main() -> result[()] { ... }
```

> Reference: rust — attribute syntax `#[…]` / `#[derive(…)]` (`rust/serde/serde_derive/src/lib.rs`, `rust/thiserror/impl/src/lib.rs`).

**Phase 0 exit:** `grammar/kirn.ebnf` gains an `attribute`/`decorator` production (owned by `SYNTAX_PLAN` SP-1); `examples/33_attributes.kn` parses and round-trips through `kirnlex`/`kirnparse` golden dumps.

---

## 1. Attribute & decorator surface (`@[…]` and `@name`)

**Goal:** one grammar for Rust-style attributes and Python-style decorators, so `@[derive(...)]`, `@[serde(...)]`, `@[error(...)]`, `@logged`, and `@route(...)` all parse and lower to a single AST decorator list.
**Must Follow:** `@` at declaration start is a decorator; `@` inside a pattern stays the alias operator (`name @ subpat`); unknown Tier-1 attribute names are compile errors; Tier-2 decorators are runtime functions (reflection-gated); stacking is deterministic.
**Important Note:** `SYNTAX_PLAN` SP-1 owns the raw grammar and the AST field; this plan supplies the attribute _set_ and their semantics. `@[a, b(c)]` is sugar for consecutive `@a`/`@b(c)`; no new lexer token is needed (`@` is already `Tok::Op`).

### 1.1 Grammar (planned)

```ebnf
decorator      = bracket_attrs | at_decorator ;
bracket_attrs  = "@" , "[" , attr { "," , attr } , "]" ;
at_decorator   = "@" , identifier , [ "(" , [ call_arg { "," , call_arg } ] , ")" ] ;
attr           = identifier , [ "(" , [ call_arg { "," , call_arg } ] , ")" ] ;
(*
  Decorators attach to struct/enum/trait/impl/fn/module/field/variant and to
  `async fn`. They precede the declaration (and each field/variant) and are
  collected left-to-right / top-to-bottom.
*)
```

### 1.2 Tier-1 built-in attributes (compile-time, zero runtime cost)

| Attribute                                                                    | Effect                                                                                                                   |
| ---------------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------ |
| `@[derive(T1, T2, …)]`                                                       | Generate trait impls (see §3, §5): `Serialize`, `Deserialize`, `Error`, `Debug`, `Eq`, `Hash`, `Ord`, `Clone`, `Default` |
| `@[serde(…)]`                                                                | Field/container serde options (§3.2)                                                                                     |
| `@[error("…")]`, `@[from]`, `@[source]`, `@[backtrace]`                      | thiserror-style error codegen (§5.3)                                                                                     |
| `@[main]`, `@[test]`                                                         | Entry-point/runtime wiring for async (§2.1)                                                                              |
| `@[inline]`, `@[pure]`, `@[deprecated("…")]`, `@[warn]`/`@[allow]`/`@[deny]` | Optimization/lint attributes (`SYNTAX_PLAN` SP-1)                                                                        |
| `@[par]`                                                                     | Parallelize a loop/function body via the work-stealing pool (§4.4)                                                       |
| `@[extern("sym")]`                                                           | Pin a foreign symbol (`KIRN_CROSS_PLAN`)                                                                                 |

### 1.3 Tier-2 user decorators (runtime, Python semantics)

A bare `@name` / `@name(args)` where `name` is not a built-in is a **runtime decorator**: a function `decorator(target) -> target` applied once at import time, bottom-up, using the reflection layer (`SYNTAX_PLAN` SP-6, `MISSING_PLAN` reflect). The prelude ships first-party decorators:

```kirn
@inject(pool)                 # DI
@route("/users/{id}")         # routing
@json                         # auto (de)serializable handler
@record                      # structural record semantics
@memo
@retry(attempts = 3, backoff = "exponential")
@timeout(ms = 250)
fn load_user(id: Str) -> User { ... }
```

> Reference: cpython — decorator stack application (`ruby/object.c` for method objects; cpython decorators); rust — `#[…]` attribute placement (`rust/serde/serde_derive/src/lib.rs`).

**Phase 1 exit:** `examples/33_attributes.kn` (Tier-1) and `examples/60_decorators.kn` (Tier-2) pass; `kirnparse` golden includes the decorator list.

---

## 2. Async runtime (`tokio`-like) — `pet.async`

**Goal:** make Kirn's existing `spawn`/`chan`/`select` primitives into a coherent, default-on async runtime with `async fn`/`await`, a work-stealing scheduler, async I/O/net/fs, and timers — matching tokio's API surface idiomatically.
**Must Follow:** `async fn` and `await` are keywords; one runtime, two flavors (multi-thread work-stealing default, current-thread for determinism); async tasks are cancel-safe by drop; `select`/`join` replace tokio's macros with first-class Kirn forms; the runtime is in the prelude (no import needed for `async`/`await`).
**Important Note:** Kirn already has OS-thread `spawn`, buffered `chan[T]`, and a polling `select` (`src/interp/runtime.cpp:6250`, `:6271`). The async runtime must run **on top of** these, not replace them: `spawn`/`chan` remain the CPU/Go-style layer; `async`/`await` + `pet.async` are the I/O-concurrency layer. `PLAN.md`/`DO_FIRST_PLAN.md` Phase 11 own the worker pool + structured concurrency that this builds on.

### 2.1 Runtime and entry points

```kirn
@[main]                          # builders a multi-thread runtime, block_on(main)
async fn main() -> result[()] { ... }

@[main(flavor = "current_thread")]   # deterministic single-thread runtime
async fn main() -> result[()] { ... }

@[test]                          # async test: builds a runtime per test
async fn parses_ids() -> result[()] { ... }

fn main() {
    var rt = async.Runtime.multi_thread(workers = 4);   # explicit runtime
    rt.block_on(async fn () { ... });
    rt.shutdown_timeout(ms = 500);
}
```

- `@[main]` / `@[test]` lower to `Runtime.multi_thread().block_on(body)` / `.current_thread()`, mirroring tokio's `#[tokio::main]`/`#[tokio::test]` expansion.
- `async.Runtime` exposes `multi_thread(workers=…)`, `current_thread()`, `block_on(fut)`, `spawn`, `spawn_blocking`, `handle()`, `shutdown_timeout`, `shutdown_background`.

> Reference: rust — `#[tokio::main]` expansion (`langs-source-code/rust/tokio/tokio-macros/src/entry.rs`); runtime builder/flavors (`rust/tokio/tokio/src/runtime/builder.rs`); `block_on` (`rust/tokio/tokio/src/runtime/runtime.rs:343`).

### 2.2 `async fn` and `await`

- `async fn f(args) -> T` declares a coroutine returning `Task[T]` (the future).
- `await expr` is a prefix expression that drives a future to completion. Prefix `await` (Python-flavored) is chosen over Rust's postfix `.await` to avoid the method-lookup/field ambiguity Kirn already flags (`PLAN.md`).
- `async fn` may contain `await`, `spawn`, `select`, `join`; borrowing across `await` follows the existing borrow rules.

```kirn
async fn fetch(url: Str) -> result[Str] {
    var (host, port) = parse_url(url)?;
    var s = await net.TcpStream.connect(host, port)?;
    await s.write_all(b"GET / HTTP/1.1\r\nHost: {host}\r\n\r\n");
    var body = await io.read_to_string(s)?;
    return ok(body);
}
```

Rejected: postfix `.await` (collides with field/method resolution); `async` blocks as expressions in v1 (kept to `async fn`).

### 2.3 Tasks

| Kirn                                                           | tokio analogue                    | Notes                                                |
| -------------------------------------------------------------- | --------------------------------- | ---------------------------------------------------- |
| `spawn(async fn () { … }) -> Task[T]`                          | `tokio::spawn`                    | schedules on current runtime; `Task[T]` is awaitable |
| `await task`                                                   | `JoinHandle::await`               | → `result[T, TaskError]`                             |
| `task.abort()` / `task.is_finished()`                          | `JoinHandle::abort`/`is_finished` | cancellation at next `await`                         |
| `async.TaskSet[T]` `.spawn(...)` `.join_next()` `.abort_all()` | `JoinSet`                         | structured concurrency                               |
| `spawn_blocking(fn () { … })`                                  | `task::spawn_blocking`            | dedicated blocking pool for CPU/blocking calls       |
| `await async.yield_now()`                                      | `task::yield_now`                 | cooperative yield                                    |

Dropping a `Task` detaches; dropping the runtime cancels remaining tasks (same as tokio).

```kirn
@[main]
async fn main() -> result[()] {
    var mut set = async.TaskSet[Int].new();
    for i in 0..8 { set.spawn(async fn () { return compute(i); }); }
    while var Some(r) = await set.join_next() { print(r?); }
    return ok(());
}
```

> Reference: rust — `tokio::spawn` (`rust/tokio/tokio/src/task/spawn.rs`), `JoinSet` (`rust/tokio/tokio/src/task/join_set.rs`), `spawn_blocking` (`rust/tokio/tokio/src/task/blocking.rs`), `yield_now` (`rust/tokio/tokio/src/task/yield_now.rs`).

### 2.4 Channels (`pet.async`)

Kirn's `chan[T]` becomes the **bounded mpsc** by default; the module adds the rest:

| Kirn                                   | tokio analogue            | Semantics                                                 |
| -------------------------------------- | ------------------------- | --------------------------------------------------------- |
| `chan[T](cap)` / `.send()` / `.recv()` | `mpsc::channel(n)`        | bounded, backpressuring, cloneable sender                 |
| `async.unbounded[T]()`                 | `mpsc::unbounded_channel` | unbounded; `send` never blocks                            |
| `async.oneshot[T]()`                   | `sync::oneshot`           | single value, single consumer                             |
| `async.broadcast[T](cap)`              | `sync::broadcast`         | all receivers see every value; lagged → error             |
| `async.watch[T](init)`                 | `sync::watch`             | latest-value state cell; `changed()`                      |
| `Task`/`mpsc` cancel-safety            | documented                | `recv`/`accept` cancel-safe; `read_exact`/`write_all` not |

```kirn
var (tx, rx) = async.mpsc[Job](cap = 32);
spawn(async fn () { for j in jobs { await tx.send(j)?; } });
while var Some(j) = await rx.recv() { handle(j); }
```

> Reference: rust — `mpsc` (`rust/tokio/tokio/src/sync/mpsc/bounded.rs`, `unbounded.rs`), `oneshot` (`rust/tokio/tokio/src/sync/oneshot.rs`), `broadcast` (`rust/tokio/tokio/src/sync/broadcast.rs`), `watch` (`rust/tokio/tokio/src/sync/watch.rs`).

### 2.5 Async synchronization

| Kirn                                               | tokio analogue               |
| -------------------------------------------------- | ---------------------------- |
| `async.Mutex[T]` `.lock()` `.try_lock()`           | `sync::Mutex` (no poisoning) |
| `async.RwLock[T]` `.read()` `.write()`             | `sync::RwLock`               |
| `async.Semaphore(n)` `.acquire()` `.try_acquire()` | `sync::Semaphore`            |
| `async.Notify` `.notify_one()` `.notified()`       | `sync::Notify`               |
| `async.OnceCell[T]` `.get_or_init(f)`              | `sync::OnceCell`             |
| `async.Barrier(n)` `.wait()`                       | `sync::Barrier`              |

These complement `MISSING_PLAN` M9 / `DATA_TYPE_PLAN` Phase 12 (sync versions: `Mutex`, `RwMutex`, `WaitGroup`, `Once`, `Cond`, `Atomic[T]`). The async variants live in `pet.async`; the sync variants live in `pet.sync`.

### 2.6 Async I/O

`AsyncRead` / `AsyncWrite` / `AsyncBufRead` / `AsyncSeek` traits; `async.io.BufReader`/`BufWriter`; `io.copy(r, w)`; extension methods `read`, `read_exact`, `read_to_end`, `read_to_string`, `write_all`, `flush`, `read_line`, `lines`. `b`-strings and `bytes` (the planned `bytes` type, `DATA_TYPE_PLAN`) are the byte buffers.

> Reference: rust — `AsyncRead`/`AsyncWrite` (`rust/tokio/tokio/src/io/async_read.rs`, `async_write.rs`), buffered adapters (`rust/tokio/tokio/src/io/util/buf_reader.rs`), `copy` (`rust/tokio/tokio/src/io/util/copy.rs`).

### 2.7 Networking

`async.net.TcpListener` (`bind`, `accept`), `TcpStream` (`connect`, `read`, `write`, `split`, `into_split`), `UdpSocket` (`bind`, `send_to`, `recv_from`), `lookup_host`. Zero-cost `split()` → borrowed `(ReadHalf, WriteHalf)`; `into_split()` → owned halves usable across tasks.

> Reference: rust — TCP (`rust/tokio/tokio/src/net/tcp/listener.rs`, `net/tcp/stream.rs`), UDP (`rust/tokio/tokio/src/net/udp.rs`), `lookup_host` (`rust/tokio/tokio/src/net/lookup_host.rs`).

### 2.8 Time

`sleep(dur)`, `sleep_until(instant)`, `timeout(dur, fut) -> result[T, Elapsed]`, `interval(period)` + `.tick()`, `Instant`, `Duration`, and `MissedTickBehavior` (`Burst`/`Delay`/`Skip`).

> Reference: rust — `sleep` (`rust/tokio/tokio/src/time/sleep.rs`), `timeout` (`rust/tokio/tokio/src/time/timeout.rs`), `interval` (`rust/tokio/tokio/src/time/interval.rs`), `Instant` (`rust/tokio/tokio/src/time/instant.rs`).

### 2.9 Filesystem, process, signals

`async.fs`: `read`, `read_to_string`, `write`, `copy`, `rename`, `remove_file`, `create_dir_all`, `metadata`, `read_dir`; `async.fs.File` implements `AsyncRead/AsyncWrite/AsyncSeek`. `async.process.Command`/`Child` (spawn, `wait`, `wait_with_output`, kill). `async.signal.ctrl_c()` and `async.signal.stream(sig)`.

> Reference: rust — `fs` (`rust/tokio/tokio/src/fs/mod.rs`, `fs/file.rs`), `process` (`rust/tokio/tokio/src/process/mod.rs`), signals (`rust/tokio/tokio/src/signal/ctrl_c.rs`).

### 2.10 `select` / `join` / `try_join`

- `select { … }` (already a keyword) is extended to await multiple async sources on one task, returning on the first ready arm, cancelling the others; supports `else` and `biased` ordering. Cancellation-safety rules are documented per operation.
- `join(f1, f2, …) -> (T1, T2, …)` awaits all concurrently, returns the tuple.
- `try_join(f1, f2, …) -> result[(T1, T2, …)]` short-circuits on the first `err`.

```kirn
select {
    v = await rx.recv()      => handle(v),
    _ = await time.sleep(1s) => print("idle"),
    else                     => break,
}
var (a, b) = await join(fetch(u1), fetch(u2));
var (x, y) = (await try_join(load(a), load(b)))?;
```

> Reference: rust — `select!` (`rust/tokio/tokio/src/macros/select.rs`), `join!`/`try_join!` (`rust/tokio/tokio/src/macros/join.rs`, `try_join.rs`), macro expansion (`rust/tokio/tokio-macros/src/entry.rs`).

**Phase 2 exit:** `examples/22–23` (spawn/chan/select) still pass; new `examples/61_async_io.kn`, `examples/62_async_channels.kn`, `examples/63_async_net.kn`; sendability checker from `DO_FIRST_PLAN` Phase 11 green; `scripts/gates_linux.sh` green.

---

## 3. Serialization (`serde`-like) — `pet.serde`

**Goal:** a format-agnostic (de)serialization framework with `Serialize`/`Deserialize` traits, a derive that generates the impls, Rust-style `@[serde(...)]` options, and JSON/TOML/YAML/CBOR enabled by default.
**Must Follow:** traits are format-agnostic; formats implement `Encoder`/`Decoder`; `@[derive(Serialize, Deserialize)]` works on structs, enums, and records; field options cover `rename`, `rename_all`, `skip`, `skip_serializing_if`, `default`, `flatten`, `with`, `tag`/`content`/`untagged`, `deny_unknown_fields`, `transparent`; the default formats ship in the stdlib.
**Important Note:** `STD_LIBS_PLAN` owns the concrete format modules (`json` already exists at `stdlib/pet/json.kn`; `toml`, `yaml`, `csv` planned). This plan owns the framework (traits + derive + `@[serde(...)]`) that they plug into. The `json.loads` nested-container stack-overflow bug (`STD_LIBS_PLAN.md:62`) is fixed under `STD_LIBS_PLAN` Phase 1a first.

### 3.1 Traits

```kirn
trait Serialize   { fn serialize[S is Encoder](self, e: S) -> result[()]; }
trait Deserialize { static fn deserialize(dec: Decoder) -> result[Self]; }

trait Encoder {
    fn write_bool(self, v: Bool) -> result[()];
    fn write_int(self, v: Int) -> result[()];
    fn write_float(self, v: Float) -> result[()];
    fn write_str(self, v: Str) -> result[()];
    fn write_nil(self) -> result[()];
    fn begin_seq(self, n: Int) -> result[SeqWriter];
    fn begin_map(self, n: Int) -> result[MapWriter];
    fn begin_struct(self, name: Str, n: Int) -> result[StructWriter];
    fn begin_enum(self, name: Str, variant: Str, n: Int) -> result[EnumWriter];
}
trait Decoder { fn read_any[T](self) -> result[T]; ... }   # Visitor-based, mirrors serde
```

A `@[derive(Serialize)]` walks fields and calls `e.write_*` / `begin_struct`; `@[derive(Deserialize)]` drives a `Visitor` with `visit_seq`/`visit_map`. This mirrors serde's `Serializer`/`Deserializer`/`Visitor` trio.

### 3.2 Derive and attribute options

```kirn
@[derive(Serialize, Deserialize)]
@[serde(rename_all = "camelCase", deny_unknown_fields)]
struct User {
    user_name: Str,                                  # -> "userName"
    @[serde(default)] age: Int,
    @[serde(skip_serializing_if = "is_empty")] tags: list[Str],
    @[serde(rename = "id")] user_id: UInt,
}

@[derive(Serialize, Deserialize)]
@[serde(tag = "type", rename_all = "snake_case")]
enum Msg {
    Hello { name: Str },
    @[serde(rename = "bye")] Goodbye,
    @[serde(untagged)] Raw(any),
}

# custom field codec
@[serde(with = "timestamps")] created_at: Instant;
```

Attribute table: `rename`, `rename_all` (case rules: lower/upper/pascal/camel/snake/screaming_snake/kebab/screaming_kebab), `rename_all_fields`, `skip`, `skip_serializing`, `skip_deserializing`, `skip_serializing_if`, `default`, `default = "path"`, `flatten`, `with`, `serialize_with`, `deserialize_with`, `tag`, `content`, `untagged`, `deny_unknown_fields`, `transparent`, `alias`, `borrow`, `bound`.

> Reference: rust — `Serialize` (`rust/serde/serde_core/src/ser/mod.rs`), `Deserialize` (`rust/serde/serde_core/src/de/mod.rs`), `Visitor` (`rust/serde/serde_core/src/de/mod.rs`), attribute parsing (`rust/serde/serde_derive_internals/src/attr.rs`, `case.rs`).

### 3.3 Default formats

| Module | API                                                       | Notes                                               |
| ------ | --------------------------------------------------------- | --------------------------------------------------- |
| `json` | `to_string`, `from_string[T]`, `to_writer`, `from_reader` | exists (`stdlib/pet/json.kn`); fix nested bug first |
| `toml` | `to_string`, `from_string[T]`                             | `STD_LIBS_PLAN` Phase 5c                            |
| `yaml` | `to_string`, `from_string[T]`                             | was "optional"; promoted by this plan               |
| `cbor` | `to_bytes`, `from_bytes[T]`                               | binary; depends on `bytes` type (`DATA_TYPE_PLAN`)  |

```kirn
var s = json.to_string(user);
var back = json.from_string[User](s)?;
var bytes = cbor.to_bytes(user);
```

**Phase 3 exit:** `examples/64_serde_derive.kn`, `examples/65_serde_formats.kn`; round-trip tests in `stdlib/pet/serde_test.kn`; `kirn test` green.

---

## 4. Data parallelism (`rayon`-like) — `pet.parallel`

**Goal:** turn sequential collection operations into parallel ones with the same ergonomics as sequential ones, backed by a default-on work-stealing thread pool.
**Must Follow:** `.par_iter()`/`.into_par_iter()`/`.par_iter_mut()` mirror the sequential iterators; combinators return the same element types; `par_sort` is stable by default; the pool size defaults to `hardware_concurrency()` and honors `KIRN_NUM_THREADS`; work is stolen, not sharded by hand.
**Important Note:** Kirn has no parallel story today beyond the speculative `@par` note (`SYNTAX_PLAN.md:1006`); `spawn` is one OS thread per call (`src/interp/runtime.cpp:6250`). This plan owns the parallel layer, reusing the worker pool from `DO_FIRST_PLAN` Phase 11. `SIMD`/GPU (EXP_PLAN stretch) remains separate.

### 4.1 Thread pool

```kirn
var pool = parallel.Pool(threads = 8);      # default: hardware_concurrency()
await pool.install(fn () { heavy(); });
parallel.join(fn () { a() }, fn () { b() });          # run two closures, return tuple
parallel.scope(fn (s) { s.spawn(fn () { use(&data); }); });   # borrow stack data
var per_thread = parallel.broadcast(fn () { parallel.current_num_threads() });
```

Pool API: `Pool(n)`, `.install(f)`, `.join`, `.scope`, `.spawn`, `.broadcast`, `current_num_threads()`, `current_thread_index()`, `yield_now()`.

> Reference: rust — `ThreadPoolBuilder`/`ThreadPool` (`rust/rayon/rayon-core/src/thread_pool/mod.rs`, `rust/rayon/rayon-core/src/lib.rs`), work-stealing registry (`rust/rayon/rayon-core/src/registry.rs`), `join`/`scope` (`rust/rayon/rayon-core/src/join/mod.rs`, `scope/mod.rs`).

### Update (2026-10-20) — §4.1 thread pool (shipped)

**Path chosen:** Kirn already runs every parallel closure on the shared task machinery (the one behind `spawn`/`await`/`parallel.join`/`invoke`), so a `Pool` is a _logical_ handle over those workers rather than a private registry — `install`/`join`/`broadcast`/`scope` route work there and block for the result, and `threads` bounds only how many workers `broadcast` fans out to. Reference read from `langs-source-code/rust/rayon/rayon-core/src/{thread_pool/mod.rs,scope/mod.rs,join/mod.rs}`.

Delivered:

- **`parallel.Pool(threads: n)`** (stdlib struct in `pet/parallel.kn`) with `.size()`, `.install(f)` (run `f` on a worker, return its value, via `spawn f().join()`), `.join(a, b)` (concurrent, `[ra, rb]`), `.broadcast(f)` (once per pool worker, results in worker order), and `.scope(body)` (see below).
- **`parallel.scope`** via `Pool.scope(body)`: `body(scope)` receives a `PoolScope` whose `.spawn(f)` queues a closure; all queued tasks run concurrently with `parallel.invoke` and are joined before `scope` returns, and `body`'s value is returned.
- **`parallel.yield_now()`** — a cooperative scheduling hint backed by a new native `parallel.yield_now` (`std::this_thread::yield`), distinct from `pet.async.yield_now` (which is an `async fn` awaiting a zero-delay timer).
- **Examples/tests**: `examples/86_parallel_pool.kn` + three goldens (examples gate now **83**); `tests/syntax/parallel_pool_ok.kn`; `stdlib/pet/parallel_test.kn` extended to assert `install`/`join`/`broadcast`/`scope`/`yield_now`.

Verification: gates **353/0/0 hung** (1_runall 83, 2b_syntax 57, 4_vm_diff 101, 8_bundle 15, 11_native 11, …), golden **348/0** (exactly the three new files recorded), crosscheck **897/0** (148 `.kn` files), stdlib **17/0**; `kirncheck` clean on the new/changed files.

Deviations / not shipped (documented):

- **No private work-stealing registry.** `Pool` does not own threads and `Pool(n).install` does not cap parallelism to `n`; it shares the global pool (whose width is `KIRN_NUM_THREADS` / `hardware_concurrency()`). `threads` only sizes `broadcast`. There is no per-pool `num_threads` enforcement or `ThreadPoolBuilder` surface.
- **`scope` is not borrow-checked and does not truly overlap with the body.** There is no borrow checker or `&`/lifetime system, so `scope.spawn` closures capture shared values by handle (a captured `list`/`dict` is shared, as everywhere in `parallel`); mutations from a spawned task are observable and unsynchronized. Tasks are collected and run when the body returns, so they do not run _while_ the body executes — the join-before-return guarantee holds, the concurrency-with-body does not.
- **No `scope_fifo`, `Scope::spawn_fifo`, panic propagation across the scope, or `ThreadPool::spawn`/`install_barrier`.** A failing task cannot abort the scope; `invoke` would surface the first panic by the task machinery's existing rules.
- **`broadcast` returns `invoke`'s result order**, which is worker-order (deterministic per run), not rayon's unspecified `Vec<R>` order.
- **`Pool.join(a, b)` / `parallel.join` take exactly two closures** (no variadic `join`/`join_context`), and `install` cannot return a borrowed reference — only a value.
- **`yield_now` is a scheduling hint only** (no task re-queueing/yield-point semantics); it does not make the tree-walker and VM suspend.

### 4.2 Parallel iterators

`.par_iter()` on `list[T]`, `set`, `map`, ranges; combinators: `map`, `filter`, `filter_map`, `flat_map`, `for_each`, `for_each_init`, `fold`, `reduce`, `try_reduce`, `sum`, `product`, `min`/`max`(`_by`/`_by_key`), `count`, `collect`, `find_any`/`find_first`/`find_last`, `any`/`all`, `zip`, `enumerate`, `take`/`skip`/`step_by`. Fallible combinators return `result`.

```kirn
var xs = 1..=1_000_000;
var total = xs.par_iter()
              .filter(fn (x) { return x % 2 == 0; })
              .map(fn (x) { return x * x; })
              .sum();
var best = xs.par_iter().max_by_key(fn (x) { return f(x); });
```

> Reference: rust — `ParallelIterator` (`rust/rayon/src/iter/mod.rs`), `IndexedParallelIterator` (`rust/rayon/src/iter/mod.rs`), `FromParallelIterator`/`par_extend`.

### 4.3 Slice / string / collection ops

`par_sort`, `par_sort_by`, `par_sort_by_key`, `par_sort_unstable`; `par_chunks`/`par_chunks_mut`, `par_windows`, `par_split`, `par_chunk_by`; `parallel.String.par_lines`/`par_split`/`par_bytes`. `ParallelDrainRange` for `list`/`Str`.

```kirn
var mut rows = load_rows(path);
rows.par_sort_by_key(fn (r) { return r.score; });
rows.par_chunks(1024).for_each(fn (chunk) { ingest(chunk); });
```

> Reference: rust — `ParallelSlice`/`ParallelSliceMut` (`rust/rayon/src/slice/mod.rs`, `slice/chunks.rs`, `slice/windows.rs`), `ParallelString` (`rust/rayon/src/str.rs`), `par_sort` (`rust/rayon/src/slice/mod.rs`).

### Update (2026-10-20) — §4.3 slice / string ops (shipped)

**Path chosen:** the slice/string forms are stdlib additions on the existing `ParIter`: sorts reuse the stable merge sort (paths already parallelise above the 8192-element grain), and the string helpers split a `Str` and hand the pieces to `par_iter`. Reference read from `langs-source-code/rust/rayon/src/{slice/mod.rs,slice/chunks.rs,str.rs}`.

Delivered (`stdlib/pet/parallel.kn`):

- **`par_sort_by(xs, cmp)`** — stable parallel sort with an `(a, b) -> Int` comparator (negative means `a` first), mirroring `par_sort_by`/`merge_cmp`/`merge_sort_cmp` next to the existing `par_sort`/`par_sort_by_key`.
- **`par_sort_unstable(xs)`** — the rayon in-place name; delegates to the stable merge sort (see deviation).
- **String helpers** over `parameter.ParallelString`'s surface: `par_lines(s)` (split on `\n`), `par_str_split(s, sep)` (split on a separator), and `par_bytes(s)` (walk the code points). Each returns a `ParIter` so the §4.2 combinators compose.
- **Examples/tests**: `examples/87_parallel_slice_string.kn` + three goldens (examples gate now **84**); `tests/syntax/parallel_slice_string_ok.kn`; `stdlib/pet/parallel_test.kn` extended with sort and string assertions.

Verification: gates **356/0/0 hung** (1_runall 84, 2b_syntax 58, 4_vm_diff 102, 8_bundle 15, 11_native 11, …), golden **351/0** (exactly the three new files recorded), crosscheck **903/0** (149 `.kn` files), stdlib **17/0**; `kirncheck` clean on the new/changed files.

Deviations / not shipped (documented):

- **No in-place mutation (`par_chunks_mut`, `par_sort*` mutating the receiver).** Kirn has no references or `&mut`, and list/str receivers are passed by value, so every sort returns a new list. `par_sort_unstable` therefore cannot be a true in-place/unstable sort — it is the stable merge sort under rayon's name (identical output).
- **No `ParallelDrainRange`** (`par_drain`): draining requires mutating the source in place, which the value/reference model cannot express.
- **No `parallel.String` namespace.** The string helpers are free functions (`par_lines`/`par_str_split`/`par_bytes`) rather than `Str` methods, because the checker has no `Str`-method table to hang them on and `par_split` is already taken by the slice form (`par_split(xs, sep)`).
- **`par_bytes` yields characters, not bytes.** Kirn `Str` indexes and iterates by character and has no byte/`u8` type, so `par_bytes("hi")` produces a `ParIter` of `char` values (map with `Str(c)` to compare against strings). `par_split` on a string is `par_str_split` to avoid the list-`par_split` clash.
- **Splits are eager:** `par_lines`/`par_str_split`/`par_bytes` materialise the pieces (via `Str.split`/indexing) before fanning out, rather than lazily streaming as rayon's `ParallelString` does; `par_sort` on a very small input stays sequential.

### 4.4 `@[par]` decorator

`@[par]` on a `for` loop or a map/fold lowers the body to the parallel iterator equivalent. `@[par]` on a pure `fn` marks it safe to run on the pool.

```kirn
@[par]
for row in rows { row.score = score(row); }

# equivalently:
rows.par_iter_mut().for_each(fn (row) { row.score = score(row); });
```

**Phase 4 exit:** `examples/66_parallel_iter.kn`, `examples/67_parallel_sort.kn`; `@[par]` equals the manual par form; gates green.

**Phase 4 exit update (2026-10-20):** §4.1–§4.4 are all shipped. Example numbering diverged (the sequence reached `85`/`86`/`87`), so `examples/85_parallel_combinators.kn` (combinators), `86_parallel_pool.kn` (pool), and `87_parallel_slice_string.kn` (slice/string) stand in for the plan's `66`/`67`; the `@[par]` equality check is covered by the §4.4 tests. Gates green (356).

---

## 5. Ergonomics: typed and dynamic errors (`anyhow` + `thiserror`)

**Goal:** make error handling pleasant by default — a type-erased `Error` with `.context(...)`, `bail`/`ensure`, and a `@[derive(Error)]` that generates typed errors with `@[from]`/`@[source]`/`@[error(transparent)]` — integrated with existing `result[T,E]`, `raise`, `?`, and `catch`.
**Must Follow:** `pet.error.Error` is the type-erased dynamic error; `result[T]` defaults `E = Error`; `?` on a `result[T, E2]` auto-converts `E2 → E` when a `@[from]` mapping exists; `@[derive(Error)]` generates the error trait impls and `Display`; no implicit conversions beyond `@[from]`.
**Important Note:** Kirn already has `result[T,E]`, `raise`, `try`/`?`, `catch`, `panic`, `defer`, `catch_panic` (`src/interp/runtime.cpp`, `src/sema/checker.cpp`); it has no dynamic error and no `From`-style conversion (research: `assignable()` is structural only). `DATA_TYPE_PLAN` owns the error-type hierarchy/interface; this plan owns the ergonomic layer (context, derive, auto-conversion). `panic` stays for unrecoverable bugs; errors use `result`/`raise`.

### 5.1 Dynamic `Error` and `Context`

```kirn
import pet.error;

fn load(path: Str) -> error.result[Config] {       # == result[Config, error.Error]
    var text = read_to_string(path).context("reading config")?;
    var cfg  = json.from_string[Config](text).context("parsing config")?;
    return ok(cfg);
}

# inspect / recover a concrete cause
match load("app.toml") {
    ok(c) => use(c),
    err(e) => {
        print(e);                                  # Display chain
        for cause in e.chain() { print("  caused by: {cause}"); }
        if var Some(io) = e.downcast[io.Error]() { handle_io(io); }
    }
}
```

API: `Error`, `Error.new(e)`, `Error.msg(s)`, `.context(c)` / `.with_context(f)` (on `result` and `option`), `.chain()`, `.root_cause()`, `.downcast[T]()` / `.is[T]()`, `.backtrace()`.

### 5.2 `bail` / `ensure`

```kirn
fn check(user: Int) -> error.result[()] {
    ensure(user == 0, "only user 0 is allowed");   # if !cond -> bail(msg)
    bail("nope");                                  # return err(error.msg("nope"))
}
```

`bail(msg)` returns `err(error.msg(msg))`; `ensure(cond, msg)` bails when `cond` is false; both accept format args.

> Reference: rust — `anyhow::Error` (`rust/anyhow/src/error.rs`), `Context` (`rust/anyhow/src/lib.rs`, `context.rs`), `anyhow!`/`bail!`/`ensure!` (`rust/anyhow/src/macros.rs`, `ensure.rs`).

### 5.3 Typed errors via `@[derive(Error)]`

```kirn
@[derive(Error, Debug)]
enum DataStoreError {
    @[error("data store disconnected")]
    Disconnect(@[from] io.Error),

    @[error("invalid header (expected {expected:?}, found {found:?})")]
    InvalidHeader { expected: Str, found: Str },

    @[error(transparent)]
    Other(@[from] error.Error),
}

# generates: Display, Error trait (message/source), and From<io.Error> / From<error.Error>
fn open(p: Str) -> result[Store, DataStoreError] {
    var f = io.File.open(p)?;      # io.Error auto-converts via @[from]
    return ok(Store(f));
}
```

- `@[error("…")]` sets the `Display` message; `{field}` / `{0}` / `{field:?}` interpolation.
- `@[from]` on a field generates a `From<FieldTy>` conversion used by `?`/`raise`.
- `@[source]` returns the field from the error's `source()`; `@[backtrace]` forwards a backtrace; `@[error(transparent)]` delegates `Display` and `source` to the sole inner field.

> Reference: rust — `#[derive(Error)]` (`rust/thiserror/impl/src/lib.rs`), display formatting (`rust/thiserror/src/lib.rs`, `impl/src/fmt.rs`), `#[from]`/`#[source]`/`#[backtrace]`/`transparent` (`rust/thiserror/impl/src/attr.rs`, `impl/src/expand.rs`).

### 5.4 Auto-conversion for `?`

Extend the checker's `?` (currently `checker.cpp:2978`) so that `expr?` on `result[T, E2]` in a function returning `result[_, E]` converts `E2 → E` when an impl `From[E2, E]` exists (derived by `@[from]`) or when `E = error.Error` (always convertible). This is the Rust `?`/`From` contract, scoped to errors.

**Phase 5 exit:** `examples/68_error_derive.kn`, `examples/69_error_context.kn`; existing `examples/19`, `20`, `35` still pass; `stdlib/pet/error_test.kn`.

---

## 6. Default-on enablement (prelude + stdlib layout)

**Goal:** these capabilities require no import ceremony for the common path — keywords, decorators, the dynamic `Error`, and the async entry points are in the prelude; the modules are in the default stdlib.
**Must Follow:** `async`/`await` are keywords; `@[derive(...)]`/`@[serde(...)]`/`@[error(...)]` are built-in attributes; `pet.async`, `pet.serde`, `pet.parallel`, `pet.error`, `pet.sync` ship with `kirn`; importing them is optional (needed only to use namespaced symbols like `time.sleep`).
**Important Note:** enabling by default must not break the existing `examples/` corpus or gates; every capability has a "sequential/non-async" fallback so programs compile without the runtime when unused.

Prelude additions: `async`, `await` (keywords); `Error`, `Result[T]`, `ok`, `err`, `bail`, `ensure` (prelude names); `Task`, `chan`, `oneshot`, `broadcast`, `watch` (prelude namespace `async`). Default stdlib modules: `pet.async`, `pet.serde`, `pet.parallel`, `pet.error`, `pet.sync`, plus formats `json`/`toml`/`yaml`/`cbor`.

| Capability                  | Ships by default? | Needs import?          | Runtime cost when unused |
| --------------------------- | ----------------- | ---------------------- | ------------------------ |
| `async`/`await` + runtime   | yes               | no                     | none (lazy runtime init) |
| `serde` traits + derive     | yes               | derive attribute only  | none                     |
| `json`/`toml`/`yaml`/`cbor` | yes               | `import pet.json` etc. | none                     |
| parallel iterators          | yes               | `import pet.parallel`  | none                     |
| dynamic `Error` + context   | yes               | no (prelude)           | none                     |
| `@[derive(Error)]`          | yes               | no                     | none                     |

**Phase 6 exit:** `examples/01_hello.kn` and the full corpus build without importing anything new; `kirn new` scaffolds use these by default; gates green.

---

## 7. Phasing & gates

**Goal:** sequence the work so each phase ends at a green gate and a runnable example.
**Must Follow:** each phase is independently shippable; no phase breaks the corpus; each ends with `kirn test` + `scripts/gates_linux.sh` (and `scripts/golden_linux.sh` when observable output changes).
**Important Note:** order matters — attributes (§1) unblock derive (§3, §5); the worker pool (`DO_FIRST_PLAN` Phase 11) unblocks async (§2) and parallel (§4).

| Phase | Deliverable                                                              | Exit / gate                               |
| ----- | ------------------------------------------------------------------------ | ----------------------------------------- |
| P0    | `@[…]`/`@name` decorator grammar + AST (with `SYNTAX_PLAN` SP-1)         | `examples/33_attributes.kn`; golden parse |
| P1    | built-in attribute set + Tier-2 user decorators (reflection)             | `examples/60_decorators.kn`               |
| P2    | worker pool + structured concurrency + sendability (`DO_FIRST_PLAN` P11) | `examples/22–23`; gates                   |
| P3    | `async fn`/`await` + runtime + `@[main]`/`@[test]`                       | `examples/61_async_io.kn`                 |
| P4    | async tasks + channels + sync + timers + I/O + net + fs                  | `examples/62–63`; async tests             |
| P5    | `select`/`join`/`try_join` on async sources                              | `examples/63_async_net.kn`                |
| P6    | serde traits + `@[derive(Serialize, Deserialize)]` + `@[serde(...)]`     | `examples/64_serde_derive.kn`             |
| P7    | default formats json/toml/yaml/cbor                                      | `examples/65_serde_formats.kn`            |
| P8    | parallel iterators + pool + `par_sort`                                   | `examples/66–67`                          |
| P9    | dynamic `Error` + `context`/`bail`/`ensure`                              | `examples/69_error_context.kn`            |
| P10   | `@[derive(Error)]` + `@[from]`/`@[source]` + `?` auto-convert            | `examples/68_error_derive.kn`             |
| P11   | default-on prelude + `kirn new` scaffolds                                | full corpus; gates                        |

---

## 8. Risks & open decisions

**Goal:** name the traps before they bite and record the decisions that need ratification.
**Must Follow:** resolve each open decision with an RFC before implementing the dependent phase; keep the language minimal (reject features that don't pay for themselves).
**Important Note:** the biggest risks are keyword/grammar collision (`@`, `await`, `async`) and the runtime's interaction with the existing OS-thread `spawn`/`chan`/`select`.

| Risk                                       | Mitigation                                                                                                           |
| ------------------------------------------ | -------------------------------------------------------------------------------------------------------------------- |
| `await` as a bare identifier vs keyword    | reserve `await` as a contextual keyword only inside `async fn` bodies, or a hard keyword (RFC)                       |
| `@[…]` vs pattern-alias `@`                | disambiguate by position (declaration start = decorator; expression/pattern = alias), parser-only                    |
| Two `spawn` semantics (OS thread vs task)  | keep `spawn` for OS threads; async tasks use `spawn` only inside `async` context; `spawn_blocking` bridges; document |
| `select` statement vs `select { await … }` | extend the existing statement; `join`/`try_join` are new builtins/expressions                                        |
| Serialization reflection dependency        | derive first (compile-time), Tier-2 dynamic serde later via reflect (`MISSING_PLAN`)                                 |
| Error auto-conversion scope                | only `@[from]` mappings and `E = error.Error`; no arbitrary implicit conversions                                     |
| Runtime cost for non-async programs        | lazy runtime init; no thread pool until first `spawn`/`par_*`                                                        |
| Pool vs GC (interp)                        | coordinate with `DO_FIRST_PLAN` Phase 11 GC/sendability before parallel mutation                                     |

Open decisions needing ratification: (1) `await` prefix vs postfix; `await` hard vs contextual keyword; (2) whether `@[…]` and `@name` are both allowed or one is canonical; (3) whether `chan[T]` becomes the async mpsc or stays sync with `async.mpsc` added; (4) default formats list (json/toml/yaml/cbor) and whether `cbor` waits for the `bytes` type.

---

## Appendix A — full Rust → Kirn API map

| Rust                                                           | Kirn                                                                   |
| -------------------------------------------------------------- | ---------------------------------------------------------------------- |
| `#[tokio::main]` / `#[tokio::test]`                            | `@[main]` / `@[test]` on `async fn`                                    |
| `async fn` / `fut.await`                                       | `async fn` / `await expr`                                              |
| `tokio::spawn`, `JoinHandle`, `JoinSet`                        | `spawn`, `Task[T]`, `async.TaskSet[T]`                                 |
| `task::spawn_blocking`, `yield_now`                            | `spawn_blocking`, `await async.yield_now()`                            |
| `sync::mpsc`, `oneshot`, `broadcast`, `watch`                  | `chan`/`async.mpsc`, `async.oneshot`, `async.broadcast`, `async.watch` |
| `sync::{Mutex,RwLock,Semaphore,Notify,OnceCell,Barrier}`       | `async.{Mutex,RwLock,Semaphore,Notify,OnceCell,Barrier}`               |
| `io::{AsyncRead,AsyncWrite,AsyncBufRead}`, `BufReader`, `copy` | `async.io.*`                                                           |
| `net::{TcpListener,TcpStream,UdpSocket}`                       | `async.net.*`                                                          |
| `time::{sleep,timeout,interval,Instant}`                       | `async.time.*`                                                         |
| `fs`, `process`, `signal`                                      | `async.fs`, `async.process`, `async.signal`                            |
| `select!`, `join!`, `try_join!`                                | `select {}`, `join(...)`, `try_join(...)`                              |
| `#[derive(Serialize, Deserialize)]`                            | `@[derive(Serialize, Deserialize)]`                                    |
| `#[serde(...)]`                                                | `@[serde(...)]`                                                        |
| `serde_json::{to_string,from_str}`                             | `json.to_string` / `json.from_string[T]`                               |
| `rayon` `par_iter`/`par_sort`/`join`/`scope`/`broadcast`       | `pet.parallel` `.par_iter()`/`.par_sort()`/`join`/`scope`/`broadcast`  |
| `anyhow::Error`, `Context`, `anyhow!`/`bail!`/`ensure!`        | `error.Error`, `.context`, `bail`/`ensure`                             |
| `#[derive(Error)]`, `#[error(...)]`, `#[from]`, `#[source]`    | `@[derive(Error)]`, `@[error(...)]`, `@[from]`, `@[source]`            |

## Appendix B: reference index (verified locally)

- tokio: `rust/tokio/tokio/src/runtime/{builder.rs,runtime.rs,handle.rs}`, `runtime/scheduler/multi_thread/`, `task/{spawn.rs,join_set.rs,blocking.rs,yield_now.rs}`, `sync/{mpsc/bounded.rs,mpsc/unbounded.rs,oneshot.rs,broadcast.rs,watch.rs,mutex.rs,rwlock.rs,semaphore.rs,notify.rs,once_cell.rs,barrier.rs}`, `io/{async_read.rs,async_write.rs,util/}`, `net/{tcp/stream.rs,tcp/listener.rs,udp.rs,lookup_host.rs}`, `time/{sleep.rs,timeout.rs,interval.rs,instant.rs}`, `fs/`, `process/`, `signal/`, `macros/{select.rs,join.rs,try_join.rs}`, `tokio-macros/src/entry.rs`.
- rayon: `rust/rayon/src/iter/mod.rs`, `rust/rayon/src/slice/{mod.rs,chunks.rs,windows.rs}`, `rust/rayon/src/{str.rs,vec.rs,prelude.rs}`, `rust/rayon/rayon-core/src/{lib.rs,registry.rs,join/mod.rs,scope/mod.rs,thread_pool/mod.rs,broadcast/mod.rs}`.
- serde: `rust/serde/serde_core/src/{ser/mod.rs,de/mod.rs}`, `rust/serde/serde/src/lib.rs`, `rust/serde/serde_derive/src/{lib.rs,ser.rs,de.rs}`, `rust/serde/serde_derive_internals/src/{attr.rs,symbol.rs,case.rs}`.
- anyhow: `rust/anyhow/src/{lib.rs,error.rs,context.rs,macros.rs,ensure.rs}`.
- thiserror: `rust/thiserror/src/lib.rs`, `rust/thiserror/impl/src/{lib.rs,attr.rs,expand.rs,fmt.rs,valid.rs}`.

---

## Implementation status

### Update (2026-10-17) — Phase 0 & Phase 1: attributes & decorators (shipped)

Delivered:

- **Grammar** (`grammar/kirn.ebnf`): new `decorators`/`bracket_attrs`/`at_decorator`/`attr`/`decorator_arg` productions; all declaration rules (`func_def`, `struct_def`, `enum_def`, `trait_def`, `impl_def`, `class_def`, `interface_def`, `record_def`, `const_decl`) and members (`field_def`, `variant`, `payload_param`) now carry a leading `decorators`.
- **AST** (`src/ast/ast.h`): `Decorator { name, args, bracket, span }`; `decorators` on `Stmt`, `FieldDecl`, `Variant`, `VariantPayload`.
- **Parser** (`src/parser/parser.cpp`): `parseDecorators()`; `@` at declaration start is a decorator list, while `@comptime(...)` stays an expression and pattern `@` stays the alias operator. `@[a, b(c)]` lowers to consecutive `@a`/`@b(c)`. Decorator args accept both `name = value` (Rust) and `name: value` (Kirn).
- **Dump** (`src/ast/ast_dump.cpp`): `Decorator`/`Arg`/`ArgName` nodes; parse golden pins the decorator list.
- **Checker** (`src/sema/checker.cpp`): Tier-1 `@[...]` names are validated against the built-in set (unknown ⇒ error). `@[deprecated("…")]` is registered order-independently and warns (`W0110`) at each use (ident and direct call). Tier-2 `@name` names are marked used (no false `W0103`).
- **Interpreter** (`src/interp/runtime.cpp`): Tier-2 decorators applied once at program collection, bottom-up; `@name` ⇒ `name(target)`, `@name(args)` ⇒ `name(args...)(target)` (Python semantics). Result shadows the raw `funcs_` entry.
- **Examples/tests**: `examples/33_attributes.kn` (Tier-1), `examples/51_decorators.kn` (Tier-2), `tests/syntax/attributes_ok.kn`, `tests/negative/n21_unknown_attribute.kn`; goldens recorded.

Verification: gates **284/0**, golden **273/0**, crosscheck **621/0**, stdlib **9/0**.

Example-number deviations (low slots are taken by existing examples): the plan's `examples/60_decorators.kn` is delivered as `examples/51_decorators.kn` (60 = `60_reflect.kn`). Later phases will take `63`+ (61/62 are also taken).

Known pre-existing quirk (not introduced here, tracked for later phases): assigning to a module-global from inside a function without the `global name;` declaration binds a fresh local first, so `c = c + 1` reads the unbound local and panics. Examples use `global name;` for global mutation.

### Update (2026-10-17) — Phase 2: `pet.async` core (shipped)

Delivered:

- **Lexer** (`src/lex/lexer.cpp`): `async`/`await` are hard keywords (verified no prior use in examples/stdlib/tests).
- **AST** (`src/ast/ast.h`): `Stmt::isAsync` (for `async fn`) and `Expr::isAsync` (for `async fn (…) {…}` closures).
- **Parser** (`src/parser/parser.cpp`): `parseFnDecl(pub, allowBody)` consumes an optional `async` before `fn` and is wired into `parseTopOrStmt`, `pub`, struct bodies and class bodies (interface/trait signatures unchanged). `await` parses in `parseUnary` (tight binding) as `Unary op="await"`. `async fn (…) {…}` closures are recognized in the expression parser.
- **Dump** (`src/ast/ast_dump.cpp`): `async` marker on `FuncDef` and `Lambda`; `await` shows as `Unary op="await"`.
- **Checker** (`src/sema/checker.cpp`): `asyncDepth_` is set for `async fn` bodies and `async` closures (saved/restored). `await` outside an async body ⇒ error `'await' is only allowed inside an 'async fn' body`; otherwise `await e` has the type of `e`. `spawn` accepts a call **or** an `async` closure. **v1 type-level deviation:** an async call is typed as its declared return `T` (the `Task[T]` wrapper is elided); `await` is identity on types.
- **Interpreter** (`src/interp/runtime.{h,cpp}`): runtime grows a task handle (`ThreadImpl` with result/panic capture). `spawnTask`/`awaitTask` run a task on an OS thread and join it; `threadEntry` captures the result and re-panics on a task panic. `callValue` and the top-level call sites spawn for async fns/methods/closures; `invokeMethod` spawns async methods (self is arg 0); module-exported `pub fn`s that are `async` also spawn. `runFunc` forces the tree-walker for `async` bodies (never VM/native). `await` on a task joins it, on a `time.after` timer sleeps to its deadline, else is identity. `async fn main` runs synchronously on the main thread (deterministic) and drives its tasks.
- **Stdlib** (`stdlib/pet/async.kn`): `sleep(ms)`, `yield_now()`, `now_ms()` over the built-in `time` pseudo-module. Because `async` is a keyword the module must be imported under a name (`import pet.async as a;`).
- **Bug fixed along the way**: module-exported functions were wrapped as `VK::Builtin` with empty `biParams`, so **named arguments to module functions** (`mod.f(x: 1)`) were silently dropped and the callee panicked with "missing argument". The export now carries parameter names.
- **Examples/tests**: `examples/63_async_basics.kn`, `examples/64_async_channels.kn`, `examples/65_async_timers.kn`; `tests/syntax/async_ok.kn`; `tests/negative/n22_await_outside_async.kn`; `stdlib/pet/async_test.kn`.

Verification: gates **293/0**, golden **285/0**, crosscheck **651/0**, stdlib **10/0**.

Example-number deviations: the plan's Phase 2 exits named `61_async_io`/`62_async_channels`/`63_async_net`, but 61/62 are taken (`native_new_surface`, `incdec_ternary`), so async basics/channels/timers ship as `63/64/65`. Async **net/fs/io** and the full tokio sync-primitive set are deferred (documented): the v1 runtime is thread-per-task over the existing `spawn`/`chan`/`select` layer. Because `async`/`await` are keywords, the runtime API is exposed via `stdlib/pet/async.kn` under an alias rather than a `pet.async` member named `async`.

Known pre-existing flake (not introduced here): `stdlib/pet/io_test.kn` appends to `.io_tmp_test.txt` without truncating, so a leftover artifact from an earlier run makes `assert(s == "hello")` fail; delete the file before a suite run.

### Update (2026-10-17) — Phase 3: `pet.serde` (shipped, reflection-based)

**Path chosen:** after exploring the front end, Phase 3 shipped the **Tier-2 dynamic (reflection-based) serde** rather than the trait + derive design written in §3.1/§3.2. Reasons (checked, not assumed):

- `@[derive(...)]` is **inert**: recognized and validated against `kBuiltinAttrs` (`src/sema/checker.cpp:2868`) but generates no code.
- The language cannot express §3.1's `trait Serialize[S is Encoder]`: **no generic impls** (`impl[T]` params are discarded, `src/parser/parser.cpp:884`), **no generic signature-only trait methods** (`src/parser/parser.cpp:863`), no `static fn`, and no call-site type arguments (`f[Int](...)`). Generic _free_ functions work inference-only.

So serde works without any derive: it walks structs/enums structurally via the existing `reflect` pseudo-module.

Delivered:

- **Runtime reflect additions** (`src/interp/runtime.cpp`): `reflect.variant`, `reflect.payload`, `reflect.variants(T)`, `reflect.enum_payload_names(T, v)`, `reflect.enum_payload_type_names(T, v)`, `reflect.enum_new(T, v, payloadList)`, `reflect.type_ref(name)`, `reflect.field_type_names(T)`, `reflect.field_generics(T)`; static helpers `typeBaseName(const ast::Type&)` and `typeGenericNames(const ast::Type&)`. `reflect.new(T, fields)` now takes an optional field dict so a decoded struct can be rebuilt field-by-field.
- **Latent crash fixed**: printing a module/type-ref value infinitely recursed `toStr`↔`repr` (both `default:` arms call each other) and segfaulted. `toStr` now has a `VK::Module` arm that strips the `__struct__:`/`__enum__:`/`__trait__:`/`__loaded__:` prefixes.
- **Stdlib** (`stdlib/pet/serde.kn`): `to_json`/`encode` (Kirn-side encoder — the native `json.dumps` returns `null` for struct/enum values), `from_json`/`decode` (dynamic tree), `from_json_as(T, s)`/`decode(T, s)` (typed rebuild + coercion). Encoding: struct → object, enum → externally tagged `{"Variant": payload}` (`null` for unit, array otherwise), list/dict/scalars per JSON. Decoding coerces scalars back to declared types (JSON has one number type), recurses nested structs/enums via `field_type_names`/`field_generics`, and rebuilds with `reflect.new` / `reflect.enum_new`.
- **`stdlib/pet/json.kn` fixes** (the `STD_LIBS_PLAN.md:62` Phase 1a item): a `depth` guard on `parse_value` (a pathological `[[[[…]]]]` now raises instead of overflowing the C++ stack), **integer preservation** (no `.`/`e` ⇒ `Int`, was always `Float`), and `skip_ws` now skips `\n`/`\t`/`\r`.
- **Examples/tests**: `examples/66_serde_basic.kn`, `examples/67_serde_enum.kn`, `tests/syntax/serde_ok.kn`, `stdlib/pet/serde_test.kn` (round-trip + int coercion + escaping + list-of-enum); goldens recorded.
- **Gate hygiene**: `scripts/gates_linux.sh` gate 6 hardcoded `'9 passed, 0 failed'`, which was stale after Phase 2 added `async_test.kn`; updated to `'11 passed, 0 failed'` (Phase 2's own `kirn_test_no_args` golden was likewise stale at 9).

Verification: gates **298/0**, golden **291/0**, crosscheck **681/0**, stdlib **11/0**.

Example-number deviation: the plan's Phase 3 exits named `64_serde_derive`/`65_serde_formats`, but 64/65 are taken by async, so serde ships as `66/67`.

Not shipped (documented deviation, blocked on the front end above): `trait Serialize`/`Deserialize`, `@[derive(Serialize, Deserialize)]`, `@[serde(...)]` field options (`rename`/`skip`/`flatten`/`tag`/…), the `Encoder`/`Decoder`/`Visitor` trio, and the `toml`/`yaml`/`cbor` formats (JSON only for now). These are gated behind generic impls, signature-only generic trait methods, `static fn`, and call-site type arguments.

### Update (2026-10-18) — Phase 4: `pet.parallel` (shipped, spawn-based)

**Path chosen:** Phase 4 shipped a **spawn-based parallel stdlib** rather than the work-stealing rayon-mirror design in §4 — there is no task scheduler in the runtime to build a stealing pool on. Primitives were verified before committing: `spawn` + `h.join()` gives real parallelism (measured ~2x on a compute example), closures capture loop variables **by value**, and `/` on ints yields `Float` (so chunk math must use `//`).

Delivered:

- **Native `parallel` module** (`src/interp/runtime.cpp`, after the `time` block): `join(a, b)` (spawn both, run one inline, `awaitTask` → `[ra, rb]`), `invoke(fns)` (spawn N−1, run the last inline, await all, results in order), `current_num_threads()` (`KIRN_NUM_THREADS` via `strtol`, else `hardware_concurrency`, min 1), `current_thread_index()` (static atomic + thread-local). `ThreadH .join()` now returns `awaitTask(...)` instead of `none`, so `spawn`ed tasks can yield results. Registered in `checker.cpp`'s `importRoots_` and the runtime prebound list; **not** added to the VM mod set (works under the VM anyway, like `reflect`).
- **`src/sema/checker.cpp`**: thread-struct `.join()` retyped `noneTy()` → `unkTy()` (it now yields a value).
- **Stdlib** (`stdlib/pet/parallel.kn`): `ParIter` whose `map`/`filter` inline the chunk+`invoke` pool logic — struct methods run with `globals_`, so they cannot see module-private helpers, which forced prebinding `parallel` in `globals_` and inlining the pool calls; plus `collect`/`count`/`sum`/`product`/`for_each`/`reduce`/`min`/`max`/`any`/`all`; `par_iter(xs)` (materializes any iterable); `par_sort`/`par_sort_by_key` (merge sort with a spawn+`h.join()` parallel branch above 8192 elements); `broadcast`.
- **Examples/tests**: `examples/68_parallel_iter.kn`, `examples/69_parallel_sort.kn`, `tests/syntax/parallel_ok.kn`, `stdlib/pet/parallel_test.kn` (12th test); goldens recorded; gate 6 count `11` → `12`.
- **Gate harness split** (separate `chore` commit): `scripts/gates_linux.sh` became a thin driver over `scripts/gates/_common.sh` + one file per gate in `scripts/gates/`, because the monolith forced a full run (gate 11 native alone takes ~15m). No args runs every gate (unchanged behaviour); `gates_linux.sh 6 2b` runs a subset.

Verification: gates **303/0** (13 gates, run individually), golden **297/0**, crosscheck **705/0**, stdlib **12/0**.

Example-number deviation: the plan's Phase 4 exits named `66_par_iter`/`67_par_sort`, but 66/67 are taken by serde, so parallel ships as `68/69`.

Deviations (documented): the pool is **spawn-based** (one OS thread per task), not work-stealing; `par_iter(xs)` is a **function**, not a `.par_iter()` method (no `.into_par_iter()`/`.par_iter_mut()`). Two items here were fixed on 2026-10-19 (see the §4.4 update below): `@[par] for …` now **is** wired (the tree-walker lowers it, and the bytecode VM hands the enclosing function back to the tree-walker when it sees the decorator), and `for_each` now **fans out across the pool** (it was sequential). Sendability is still unchecked, so a parallel closure must not race on shared state — `par_each` stays dropped.

### Update (2026-10-18) — Phase 5: `pet.error` (shipped, anyhow/thiserror-style)

**Path chosen:** Phase 5 ships §5's ergonomic layer as **native runtime support** — a type-erased `error.Error` value, `.context`/`chain`/`root_cause`/`downcast`, `bail`/`ensure`, `?` auto-conversion at the result boundary, and a **minimal `@[derive(Error)]` Display generator** — rather than the thiserror derive/codegen design, because the front end still generates no code from `@[derive(...)]` (documented since Phase 3) and has no `From` impls. §5.4's conversion is implemented in the **runtime's `runFunc` result boundary**, not `checker.cpp:2978`, because every `raise`/`?` already funnels through `SignalRaise` there.

Delivered:

- **Dynamic `Error`** (`src/interp/runtime.cpp`): `kErrorType = "error.Error"` struct with `msg`/`source`/`origin`; helpers `mkError`, `isErrorStruct`, `isPetError`, `errorFieldStr`, `errorChainStr` (anyhow `{:#}` → `"top: cause1: cause2"`), `errorWithContext`. `print(e)`/`toStr` on an Error shows the Display chain (`toStr`'s `VK::Struct` arm), not the raw struct.
- **Module API** (`moduleMember`, `mod == "error"`): `msg`, `new`, `context(r, c)`, `chain`, `root_cause`, `is(e, T)`/`downcast(e, T)`, `display`, `bail(msg)`, `ensure(cond, msg)`. Registered in `checker.cpp`'s `importRoots_` and the runtime prebound-module list.
- **Context on `result`** (`memberRead` `VK::Result`): `.context(c)` / `.with_context(f)` are native bound builtins — `ok` passes through unchanged, `err` pushes a layer onto the source chain (`with_context` captures `[this]` to call the closure). Added to `checker.cpp`'s `methodLookup` result protocol (params **and** names must be pushed together or `matchArgs` reads `names[]` out of bounds).
- **Error methods** (`invokeMethod`, `VK::Struct` + `isErrorStruct`, before the user-method lookup): `chain()`, `root_cause()` (returns the deepest **Error value** so `is`/`downcast` still work on it), `is(T)`, `downcast(T)` (matched over the `origin` field, with namespace-tolerant type comparison), `backtrace()` (returns `none` — not captured yet).
- **§5.4 `?`/`raise` auto-conversion** (`runFunc`'s `catch (SignalRaise&)`): when the function's declared return is a single-argument `result[T]` (E defaults to `error.Error`) and the payload is not already a `pet.error` Error, it is wrapped as `mkError(toStr(payload), none, payload)` — the original stays reachable as `origin`, so `error.is`/`error.downcast` recover it. Two-argument `result[T, E]` fns propagate the payload unchanged, so existing typed-error examples (`19`) are untouched.
- **§5.3 `@[derive(Error)]` Display (minimal)**: a file-level `g_errDerivedEnums` registry filled by `registerEnumDerive(s)` at the three enum-registration sites; `toStr`'s new `VK::EnumV` arm renders the variant's `@[error("…")]` template with `{field}` / `{0}` / `{field:?}` / `{{`/`}}` interpolation, and `@[error(transparent)]` delegates to the sole payload. Enums without the derive keep the `repr` form (no regression — the repo had no `@[derive(Error)]` enums before this change).
- **Lexer bug fixed along the way** (`src/lex/lexer.cpp`): `Lexer::fill()` ran `skipInlineWs()`/comment handling **before** dispatching to the f-string literal scanner, so whitespace/`#` right after a `}` was eaten (`f"a {u} b"` → `a 0b`; `examples/01` printed `Kirnsays hi`). `FsMode::Text` is now dispatched at the top of the loop; six goldens (lex/parse 01/04/31) re-recorded.
- **Examples/tests**: `examples/70_error_derive.kn` (§5.3 Display + §5.4 `?` + `is`/`downcast` round-trip), `examples/71_error_context.kn` (§5.1 context/chain/root_cause + §5.2 ensure/bail), `tests/syntax/error_ok.kn` (warning-free, tree==VM), `stdlib/pet/error_test.kn` (13th test); goldens recorded; gate 6 count `12` → `13` (`scripts/gates/6_test.sh`, `scripts/gates_linux.sh` header).

Verification: gates **308/0** (13 gates), golden **303/0**, crosscheck **723/0**, stdlib **13/0**.

Example-number deviation: the plan's Phase 5 exits named `68_error_derive`/`69_error_context`, but 68/69 are taken by Phase 4 parallel, so error ships as `70/71`.

Deviations / not shipped (documented):

- **`@[from]` / `@[source]` / `@[backtrace]` derive codegen and `From[E2, E]` impls are not implemented** — there is still no derive-codegen phase (see Phase 3), so `?` auto-converts only to `E = error.Error`; a typed-error enum is reached via `origin` + `downcast`, not `From`.
- **§5.4's conversion lives in the runtime result boundary, not the checker.**
- **`.context()` on `option` is not wired** (only `result`).
- **`error.result[T]` type alias is not implemented** (dotted type names do not parse); use `result[T]` (E defaults) or `result[T, ConcreteEnum]`.
- **`backtrace()` returns `none`** (backtraces are not captured).
- Front-end limits hit while writing the examples/tests (all pre-existing, verified by probe): enum payload variants need **named** fields (`Disconnect(reason: Str)`, not `Disconnect(Str)`); struct-style variant payloads (`Variant { … }`) do not parse; `ok(x)`/`err(x)` are not expressions (get err values via `raise` or `.context`/methods); `match` uses `case ok(v) { … }` (not `=>`); string interpolation is **f-strings** (`f"…"`), plain strings do not interpolate; `main` needs `-> none`.

### Update (2026-10-19) — Phase 6: default-on prelude + `kirn new` scaffolds (shipped)

**Path chosen:** §6's prelude ships as **zero-import name resolution plus runtime globals**, not an auto-imported module — `bail`/`ensure` are real builtins, `ok`/`err` are prebound globals, `Error` resolves to the type-erased top type, and `Result[T]` aliases the existing `result` nominal. Nothing loads when unused, and no existing import changes meaning.

Delivered:

- **Prelude names** (`src/sema/checker.cpp`, `src/interp/runtime.cpp`): bare `bail(msg)` / `ensure(cond, msg)` are declared in `predeclareBuiltins` and installed as runtime globals — they `raise` through the same `SignalRaise` path as the `error.` module forms; bare `ok(v)` / `err(v)` are installed as globals returning `Value::resultOk`/`resultErr` (the checker had always accepted them as enum constructors, but the runtime bound them to nothing — calling either panicked).
- **`Error` and `Result[T]` as bare types**: `resolveType` maps `Error` → the top/dynamic type (member access on an unregistered nominal errors, so `e.chain()`/`.root_cause()`/`.context()` would have been unusable) and `Result` → the existing `result` nominal with its type args; `assignable` now accepts `result`-variant values as `Result[T]`, which fixes `return ok(5)` (the return check compared the resolved nominal and reported _"got result.ok, expected result"_). Bare `Result` (no args) parses too.
- **`Result` recognized everywhere `result` was**: `isResultRet`, the runtime type test (`t == "result"`), and `native.cpp`'s AST-return-name check all accept `Result`, so annotated functions keep their result-boundary treatment (§5.4 conversion, return wrapping) and stay off the native backend.
- **Checker/runtime root alignment**: the runtime's prebound-module list gains `sync`/`reflect` (the checker already allowed them — `sync.X` typechecked and then panicked at runtime) and `importRoots_` gains `text` (the runtime already had it). The two lists had silently drifted.
- **`kirn run` with no target** now means "current directory" (the scaffold's own hint says `cd <project> && kirn run`); an entryless directory still gets the fix-it error (exit 66).
- **`kirn new` scaffolds repaired and taught the prelude**: every generated `fn main()` declares `-> none` (the checker rejects bare `fn main()` — the reason the app scaffold was rotten; **no gate ever ran `kirn new`**, so it rotted unnoticed); app `code/main.kn` generates a `greeting` that uses `ensure`/`bail`/`f-string`/`match ok|err`/`error.display` with zero imports, and app `tests/main_test.kn` asserts through `match ok(7)`; pet `code/pin.kn`'s `string` annotations become `Str`, and `shout` now uses the existing `upper()` builtin (the original manual loop called `ord`/`chr`, which exist nowhere in the language or stdlib).
- **New gate coverage** (`scripts/gates/5_conventions.sh`): `kirn new app` → bare `kirn run` + `kirn test .`, and `kirn new pet` → `kirn test .` + `kirn build` packing a `.pet` — four tests closing exactly the hole that let the scaffolds rot.
- **`toStr`/`repr` mutual recursion fixed** (`src/interp/runtime.cpp`): 15 value kinds (`Fn`, `Builtin`, `Chan`, `ThreadH`, `Timer`, `File`, `Arena`, `Ptr`, `Mutex`, `RwMutex`, `WaitGroup`, `Once`, `Cond`, `Atomic`, `Weak`) were in neither function's case list, so `print(f)` fell into `toStr`'s `default → repr` → `repr`'s `default → toStr` and **segfaulted** (~25k stack frames deep; the same failure mode Phase 3 fixed for `VK::Module`). `print(chan)`/`print(handle)` crashed identically.
- **Detached-task teardown race fixed** (`Interpreter::shutdownThreads`): it **detached** every live task, so a task outliving `run()` kept touching `globals_`/`env` while they were destroyed — `var h = spawn f;` then exiting crashed ~5 runs in 8 (flaky use-after-free at exit, on any program that spawned without joining; a pre-existing bug, reproducible with a two-line script). It now **joins**: `threadEntry` catches every panic and never rethrows, so joining cannot abort the process, and the gates report `0 hung`, confirming no corpus task blocks. Stress: 0/20 failures after (was ~5/8).
- **Corpus proves the default**: `import pet.error;` stripped from `tests/syntax/error_ok.kn`, `examples/70_error_derive.kn`, `examples/71_error_context.kn` — all three now run on the bare prelude (4 lex/parse goldens re-recorded); the import is kept in `stdlib/pet/error_test.kn` so the import path itself stays tested.

Verification: gates **312/0/0 hung** (13 gates), golden **303/0**, crosscheck **723/0**, stdlib **13/0**; both scaffolds green end-to-end (`app`: bare `kirn run`, `kirn test .`, `kirnc check`; `pet`: `kirnc check`, `kirn test .`, `kirn build` → `.pet` packed).

Deviations / not shipped (documented):

- **`Error` is the type-erased top type, not a nominal struct** — a nominal would make every `.chain()`/`.context()` call a _"struct has no field"_ error (member access only sees registered structs). Dynamic errors are still identified at runtime by `isErrorStruct`.
- **The prelude namespace `async` (`Task`, `oneshot`, `broadcast`, `watch`) is not implemented**: `async` is a hard keyword and cannot name a namespace (Phase 2 precedent). The equivalents are reachable as plain names (`chan`, `spawn`, …) instead.
- **`pet.sync`, `toml`, `yaml`, `cbor` are not shipped** (only `pet.json` plus the native sync primitives); §6's stdlib-breadth row remains open. The `sync`/`reflect` roots are registered but their module files do not exist yet — a reference now fails with a normal module-not-found instead of a runtime panic.
- **`Result` without type args carries no payload type** — `Result[T]` is the form to use.
- **`ok(x)`/`err(x)` _are_ expressions now** — this supersedes the Phase 5 front-end note above.
- **`kirn run` on a library still exits through the runtime's `panic: no main() defined`** rather than a tailored _"this project has no `fn main`"_ fix-it.
- **`ord`/`chr` remain unimplemented** (the pet scaffold no longer needs them); if they land later, the manual-char-loop example style in the scaffold could return.

### Update (2026-10-19) — Phase 7 (plan P7): default formats json/toml/yaml/cbor (shipped)

**Path chosen:** one shared reflection core plus per-format parse/render. `pet.codec` owns `enc_tree` (any → tree) and `coerce_type` (tree → `T`), both extracted verbatim from `pet.serde`; every format module is then a thin renderer/parser over that tree. Dependencies stay acyclic: `codec` ← `{json, toml, yaml, cbor}`, `serde` → `json`, never in reverse (the runtime has no cyclic-import support — the module cache is written only after execution, so a cycle recurses forever).

Delivered:

- **`stdlib/pet/codec.kn`**: `enc_tree` (struct → field dict, enum → externally-tagged `{variant: payload-list|none}`, seqs → list, maps → dict with string keys, inf/nan float → none) + `coerce_type`/`decode_struct`/`decode_enum`/`coerce` with `list`/`dict` generic element coercion — moved out of `pet.serde` so four formats share one implementation.
- **`pet.json`**: gains `render` (tree → JSON text; the old serde quote/enc logic moved here), serde-aware `to_string(v)` and `from_string(T, s)`; `dumps`/`loads` and the pure-Kirn parser untouched. `pet.serde` slimmed to `to_json = json.render(codec.enc_tree(v))` / `from_json_as = codec.coerce_type(T, json.loads(s))` — byte-for-byte identical output (serde_test's exact-string assertions stayed green through the refactor).
- **`pet.toml`**: subset parser (bare/quoted/dotted keys, `[table]` and `[[array-of-tables]]`, inline tables, arrays with comments/trailing commas, basic/literal/multiline strings, `0x`/`0o`/`0b`, `_` separators, inf/nan, `#` comments, date/time tokens kept as strings, 128-level depth guard) and a block renderer; tree round-trip verified both directions.
- **`pet.cbor`**: RFC 8949 major types 0/1/3/4/5/7 (uint, negative int, text, array, map, false/true/null, float64/float32) over `bytearray`; encodings are **byte-exact against the appendix A vectors** (`00 01 0a 17 1818 1819 1864 1903e8 20 29 3863 f5 f4 f6 fb3ff8000000000000 626869 83010203 a1616101 …`). Floats use a hand-rolled IEEE754 binary64 codec (no `struct.pack` in the language); normal/subnormal decode, exact round-trips at both ends of the magnitude range.
- **`pet.yaml`**: block-style parser (indentation, `- key: value` items with continuation lines, `- - 1` nested sequences via line rewriting, minimal flow `{a: 1}`/`[2, 3]`, single/double-quoted scalars incl. `''` escapes, `#` comment stripping that is quote-aware, tabs rejected) and a renderer that quotes number-like/keyword strings so `"42"` and `42` round-trip distinctly.
- **`examples/72_formats.kn`**: a `Profile` struct + `Shape` enum round-tripped through all four formats; three goldens recorded (`check`/`lex`/`parse`); plan-named `65_serde_formats.kn` remains taken by async (serde-phase example-number deviation).
- **Tests**: `toml_test.kn` (44 assertions incl. AoT, dotted keys, multiline strings, bases, dates-as-strings, struct/enum/generic-list round-trips), `cbor_test.kn` (RFC vectors, float/int boundary matrix, struct/enum incl. unit variant), `yaml_test.kn` (block/flow/quoting/round-trip) — `kirn test .` now **16/0**; `scripts/gates/6_test.sh`, the `gates_linux.sh` header, and `scripts/golden/kirn_test_no_args.txt` bump 13 → 16.
- **Language quirks hit while writing the parsers (no compiler changes made)**: struct methods cannot see module-level free functions (so all parser helpers are methods — renderer helpers stay free, free→free works); implicit local bindings may only be _reassigned_ inside loops (outside → "cannot reassign immutable binding"); parameters are immutable; struct fields must be declared after the types they reference; `import math` binds a namespace _different_ from the prebound `math` global (bare `math.floor_div`/`math.pow` work, imported do not).

Verification: gates **314/0/0 hung**, golden **306/0**, crosscheck **771/0**, stdlib **16/0**, `kirnc check` clean on all six new/changed modules.

Deviations / not shipped (documented):

- **Type-as-value parameter instead of call-site generics**: `from_string(T, s)` / `from_bytes(T, b)` take the type as a first value argument, matching serde's `from_json_as` convention — Kirn has no `f[T](x)` generic call syntax on module members (the spec's `from_string[User](s)` shape).
- **`to_writer` / `from_reader` (json)**: not shipped — v1 stdlib has no writer/reader handles.
- **TOML subset edges**: `\b`/`\f`/`\uXXXX` escapes (no `chr`/`ord` in Kirn), the space-separated datetime form, and native datetime values are out of scope; **unit enum variants cannot survive TOML** (it has no null, so `none` is omitted and the empty table decodes as an ambiguous enum — JSON/YAML/CBOR keep them). This supersedes the Phase 6 deviation row that listed `toml`/`yaml`/`cbor` as unshipped.
- **CBOR subset edges**: byte strings (major 2), tags (major 6), indefinite-length items, and float16 decode raise.
- **YAML subset edges**: anchors/aliases, tags, directives, block scalars (`|`/`>`), multi-document streams, and complex (`?`) keys.
- **`pet.sync` (Barrier/Semaphore over the native primitives) remains deferred** to the next phase.

### Update (2026-10-19) — Phase 8 (plan P6/P7 follow-up): `pet.sync` (shipped)

**Path chosen:** the native runtime already exposes `Mutex`, `RwMutex`, `WaitGroup`, `Once`, `Cond` and `Atomic` (globals and the prebound `sync` namespace), so `pet.sync` only adds the three tokio::sync primitives that were missing — `Barrier`, `Semaphore` and `Notify` — each a factory `fn` over a small state struct built from `Mutex` + `Cond`. No compiler changes.

Delivered:

- **`stdlib/pet/sync.kn`**: `Barrier(parties)` (`wait() -> Int` returns the generation, reusable across rounds), `Semaphore(permits)` (`acquire`/`acquire_n`/`try_acquire`/`try_acquire_n`/`release`/`release_n`/`available`), `Notify()` (`notify_one` stores one permit when no waiter is registered, `notify_waiters` wakes every current waiter without storing a permit, `wait` consumes one notification, `waiter_count` for deterministic tests). All waits are condition checks under the lock, so signal-before-wait never loses a wakeup.
- **`stdlib/pet/sync_test.kn`**: 8 tests — barrier single-party reuse and a four-party two-round barrier, semaphore counter/try/limits, semaphore release waking two waiters, permit stored before the waiter arrives, and notify one/two waiters (polls `waiter_count` before broadcasting so the wakeup can never fire into an empty waiter set). `kirn test .` now **17/0**; `scripts/gates/6_test.sh`, the `gates_linux.sh` header and `scripts/golden/kirn_test_no_args.txt` bump 16 → 17.
- **`examples/73_sync.kn`**: a three-worker reusable barrier, a two-permit semaphore, and a pre-arrival notification; deterministic output across runs; three goldens recorded.
- **The design constraint that shaped the module (no compiler changes)**: **a method receives `self` as a _copy_ of the receiver, and scalar fields are written back only when the call returns** (`fn bump(self)` twice yields 2 — copy-out works — while a method that increments an `Int` field and then blocks inside `Cond.wait` publishes nothing to other threads and deadlocks the primitive). Reference fields — **lists and sync values — are shared through that copy** (verified: a list element written under a receiver copy is visible to another thread mid-call; a `Mutex` stored inside a list still serializes two threads, 501 ms for two serialized 250 ms holds). So every piece of mutable state lives in one list per primitive (`st = [mutex, cond, …counters]`); receiver copies then share the lock, the condition and the counters. This also resolved the initial deadlock where each party operated on its own snapshot.

Verification: gates **316/0/0 hung**, golden **309/0**, crosscheck **789/0** (129 `.kn` files), stdlib **17/0**, `kirnc check` clean on both new modules.

Deviations / not shipped (documented):

- **`oneshot` / `broadcast` / `watch` channels**: not shipped — the native `chan` already covers the channel use cases; the notification/rendezvous cases are `Notify`.
- **Timed waits**: `acquire`/`wait` have no timeout/millis variant — the runtime `Cond` exposes no timed wait, and the plan does not require one.
- **`Notify` permit accounting**: `notify_one` stores at most one permit (tokio's `notify_one` likewise coalesces); a `notify_waiters` with no registered waiter is a no-op, exactly like tokio's.
- **`Semaphore` has no `MAX_PERMITS` guard**: `release_n` simply adds; overflow is not checked.
- **No re-export of the native primitives**: `sync.Mutex`/`sync.Cond` remain the prebound runtime namespace; `import pet.sync as sync` shadows it with this module, so code that wants both imports the file under a different alias.

### Update (2026-10-19) — Phase 9 (plan §6 follow-up): `pet.sync` channels (shipped)

**Path chosen:** §6 lists `oneshot` / `broadcast` / `watch` under the `async` prelude namespace, but `async` is a keyword, so the namespace cannot exist as a module name. The native `chan` already covers plain FIFO hand-off, so the missing pieces are the three tokio::sync _channels_; they are built in `pet.sync` (the natural home for tokio::sync parity) from the same `Mutex` + `Cond` + single-`st`-list pattern Phase 8 established. No compiler changes. Reference read directly from `langs-source-code/rust/tokio/tokio/src/sync/{oneshot,broadcast,watch}.rs`.

Delivered:

- **`Oneshot()`** — single-use channel: `send(v)` (non-blocking, may run on any thread), `recv()` (blocks until a value or close), `try_recv()`, `close()`, `is_empty()`, `is_closed()`. The value is stored, so a `send` before `recv` is never lost.
- **`Broadcast(capacity)`** — multi-producer/multi-consumer ring buffer: `send(v) -> Int` (returns the receiver count), `subscribe() -> BroadcastRx`, `receiver_count()`, `len()`/`is_empty()`, `close()`/`is_closed()`. `BroadcastRx`: `recv()` (blocks), `try_recv()`, `len()`/`is_empty()` (pending for this receiver), and `lagged()`. A receiver that falls more than `capacity` behind skips the overwritten values, resumes at the oldest retained one, and accumulates the number skipped in `lagged()`.
- **`Watch(initial)`** — last-value channel with per-receiver seen-versioning: `send(v)`, `send_replace(v) -> any` (returns the old value), `borrow()`, `subscribe() -> WatchRx`, `receiver_count()`, `close()`/`is_closed()`. `WatchRx`: `borrow()`, `borrow_and_update()`, `has_changed()`, `changed() -> Bool` (blocks; false only when closed _and_ already seen), `mark_changed()`, `mark_unchanged()`, `is_closed()`.
- **`stdlib/pet/sync_test.kn`**: 10 new tests (18 total) — oneshot roundtrip + close, broadcast fan-out, subscribe-sees-future-only, lag/resume, close-drains; watch `changed`, `borrow` vs `borrow_and_update`, `mark_changed`/`mark_unchanged`, and close semantics.
- **`examples/74_channels.kn`** + three goldens; the examples gate now reports **71** files.

Verification: gates **317/0/0 hung** (1_runall 71, 8_bundle 15, 8b_HIR 3, 9_MIR 10, 11_native 11, …), golden **312/0**, crosscheck **795/0** (130 `.kn` files), stdlib **18/18**; `kirnc check` clean. Threaded probes (spawn + join around `recv`/`changed`) were run three times each with identical output.

Deviations / not shipped (documented):

- **No `Result`-typed errors.** Kirn has no `Result`/`?` for the module's callers here, so `recv`/`try_recv` return `none` on the empty/closed path (in `Oneshot` this makes a stored `none` indistinguishable from "nothing arrived" — documented widening), and `send` on a closed channel, or a `Broadcast.send` with no receivers, raises instead of returning `Err`.
- **Values are shared by reference, not cloned.** tokio clones `T` per broadcast receiver; Kirn has no implicit deep copy, so every receiver of a `Broadcast`/`Watch` gets the same underlying object. For `Int`/`Str`/`Bool`/`Float` payloads this is identical to a clone; a mutable list payload is shared.
- **No receiver destructors.** Kirn has no deterministic drop hook, so `receiver_count` only grows and a `Broadcast.send` with zero subscribers is the only "no receivers" state (tokio errors when the last receiver is dropped). `close()` is explicit.
- **`Broadcast` capacity is exact, not rounded up** to a power of two, and lag is reported via the queryable `lagged()` counter rather than a `RecvError::Lagged`; lost values are never buffered past `capacity`. No `WeakSender`, `try_recv` error distinction, or sender garbage-collection bookkeeping.
- **`Watch` `send_modify`/`send_if_modified`** are not shipped; `send` and `send_replace` cover the publishing cases.
- **Timed variants** (`recv_timeout`, `changed_timeout`) are absent for the same reason as Phase 8: the runtime `Cond` has no timed wait.

### Update (2026-10-19) — Phase 10 (plan P5): `pet.async` join / try_join (shipped)

**Path chosen:** §2.10's `join`/`try_join` build directly on the existing async runtime — calling an `async fn` already schedules the task and `await` drives it — so the combinators are two `async fn`s in `pet.async` over a list of task handles. `select { await … }` needed grammar/runtime work and is not extended (the existing channel/timer `select` in `examples/23` covers multiplexing); documented below.

Delivered:

- **Runtime fix (prerequisite, own commit `c4051a5`)**: a task body that called `raise(...)` previously fell into `threadEntry`'s `catch(...)`, becoming the generic panic `"unknown error in task"` — the payload was lost and `try/catch` could not recover it. `threadEntry` now catches `SignalRaise` separately and stores the payload on the `ThreadImpl` (`raised`/`raiseValue`); `awaitTask` (also reached via `ThreadH.join()`) re-throws it, so a task's `raise` surfaces at the await site as an ordinary catchable exception. No corpus file spawns a raising task, so no golden was affected.
- **`stdlib/pet/async.kn`**: `join(ts: list) -> list` awaits every task and returns results in list order — if any task raised, the **first** error is re-raised only after all tasks have finished (so `join` never returns with a task still unjoined). `try_join(ts: list) -> list` awaits in order and short-circuits on the first raise. Both are `async fn`s, called as `await a.join([t1, t2, …])`.
- **`stdlib/pet/async_test.kn`**: join ordering (`[10, 20, 30]`), try_join short-circuit, and the join-waits-all-then-raises-first-error case.
- **`examples/75_async_join.kn`** + three goldens; the examples gate now reports **72** files. Deterministic across runs.

Verification: gates **320/0/0 hung** (1_runall 72, 4_vm_diff 90, 8_bundle 15, 11_native 11, …), golden **315/0**, crosscheck **801/0** (131 `.kn` files), stdlib **18/0**; `kirnc check` clean.

Deviations / not shipped (documented):

- **List argument and list result**, not the plan's varargs + tuple (`join(f1, f2, …) -> (T1, T2, …)`), because Kirn has no variadic free functions and a tuple return is awkward to destructure here; pass `[h1, h2, …]` and index the result.
- **`select` is not extended to `await` arms.** The existing `select { case v = ch.recv() … / case <-time.after(…) … }` covers channel/timer multiplexing; awaiting async sources in an arm needs parser + `selectExec` work and is deferred.
- **No cancellation.** `try_join` stops _awaiting_ after the first error, but a skipped task keeps running detached, because Kirn tasks are eager OS threads with no cancellation token.
- **`join` re-raises rather than returning errors as values.** Kirn has no `Result` in this position; the plan's `try_join` `?`-style short-circuit is expressed as the raise propagating to the caller's `try/catch`.

### Update (2026-10-19) — §4.4: `@[par] for` (shipped)

**Path chosen:** `@[par]` on a `for` loop lowers to the hand-written parallel form, `pet.parallel.par_iter(xs).for_each(fn (it) { … })`. Because the bytecode VM is now the default runner and only compiles simple `for x in …` loops, the two executors needed coordinating: the **tree-walker** does the lowering, and the **VM compiler** hands the enclosing function back to the tree-walker (sets `em.f.interpreted = true`) when it sees a `@[par]` decorator — the same policy it already uses for `Try`/`Match`/`Select` and non-bind loop patterns. This is the only part of §4.4 shipped; `@[par]` on map/fold and on a `fn` (marking it pool-safe) are not shipped.

Delivered:

- **Tree-walker lowering** (`src/interp/runtime.cpp`, `case StKind::For`): when the statement carries a `@[par]` decorator, it evaluates the iterable, builds a synthetic native closure (`biFn`) that re-binds the original loop pattern (`matchPat`) against each element and runs the body in a child env, then dispatches it through `par_iter` (resolved from `pet.parallel`, loaded on demand) and `invokeMethod(…, "for_each", …)`. A struct method must go through `invokeMethod`, not `memberRead` — `memberRead` on a struct only sees fields. The lowering mirrors a serial `for` (per-iteration child env; `break`/`continue` inside a `@[par]` body are unsupported because the body runs as a closure).
- **VM hand-off** (`src/vm/compiler.cpp`, `case StKind::For`): a `@[par]` decorator marks the whole function interpreted so the default VM path and the `--no-vm` tree-walker produce identical results (gate 4_vm_diff and the syntax test both assert tree == VM).
- **`for_each` is now parallel** (`stdlib/pet/parallel.kn`): it previously ran `for e in xs { f(e); }` sequentially, which would have made `@[par]` a no-op. It now uses the same chunk-and-`parallel.invoke` shape as `map`/`filter` (threshold 4096, honoring `KIRN_NUM_THREADS`), so `@[par]` — and the manual `par_iter(...).for_each(f)` — genuinely fan out. A 5000×1ms body on 4 threads dropped from ~5s to ~1.5s.
- **`examples/76_par_decorator.kn`** + three goldens; the examples gate now reports **73** files. The example is deterministic: small inputs (sequential path) plus a `sync.Atomic` accumulator over `1..=20000` (parallel path, order-independent integer sum = `200010000`). **`tests/syntax/par_decorator_ok.kn`** pins tree == VM for the list, tuple-pattern, and parallel-atomic cases.

Verification: gates **323/0/0 hung** (1_runall 73, 2b_syntax 47, 4_vm_diff 91, 8_bundle 15, 11_native 11, …), golden **318/0**, crosscheck **807/0** (132 `.kn` files), stdlib **17/0**; `kirncheck` clean on the new files.

Deviations / not shipped (documented):

- **`@[par]` only on `for`.** The plan also describes `@[par]` on a map/fold and on a `fn` (marking it pool-safe); neither is shipped — a `fn` decorator would need the checker to bless a body it cannot prove race-free.
- **No result collection.** `for_each` returns `none`, so `@[par] for` yields nothing; the body must reach a result through an order-independent effect (`sync.Atomic`, or a `Mutex`-guarded structure). A shared plain `+=` over a large parallel input races and is the documented anti-pattern.
- **`break`/`continue` unsupported in the body.** The body runs as a closure handed to `for_each`, so those signals have no enclosing loop to unwind to.
- **The 4096-element threshold** (shared with `map`/`filter`) means small loops run on the calling thread — identical output to a serial `for`, which is what keeps the example and test deterministic.

### Update (2026-10-20) — §4.3: grouping iterators (`chunks`/`windows`/`split`/`chunk_by`)

**Path chosen:** §4.3's list-shaping ops are pure `ParIter` methods — each makes a cheap sequential pass to build the groups — plus `par_chunks`/`par_windows`/`par_split`/`par_chunk_by` free-function entry points that delegate to the method form, matching how `par_iter` already fronts the struct. The important part is that a shaped iterator is **`divided`**: the downstream `map`/`filter`/`for_each` then fans out one _group_ per worker instead of one element, so `par_iter(rows).chunks(1024).for_each(ingest)` actually parallelizes. `par_sort`/`par_sort_by`/`par_sort_by_key` shipped earlier under Phase 8.

Delivered:

- **`ParIter` gains a `divided: Bool` field** (`stdlib/pet/parallel.kn`) and a `grain()` helper: the split threshold is 4096 elements normally, but only **2 groups** once the sequence has been shaped — otherwise a handful of chunks would never leave one thread and `chunks(1024).for_each(...)` would silently run serially. `map`/`filter` thread the flag through appropriately (`map` over groups produces elements again, so it clears it; `filter` preserves it).
- **Grouping methods**: `chunks(n)` (fixed size, last group is the remainder), `windows(n)` (slide by one, overlapping; empty below `n`), `split(sep)` (drop separators, adjacent separators give empty groups), `chunk_by(f)` (group _consecutive_ equal-key elements). `chunks`/`windows` raise on `n <= 0` rather than spinning. Each returns `ParIter(xs: groups, divided: true)`.
- **Free-function entry points** `par_chunks`/`par_windows`/`par_split`/`par_chunk_by`, equivalent to `par_iter(xs).<op>(...)`.
- **`examples/77_parallel_grouping.kn`** + three goldens; a timing probe with four 300 ms chunks in `for_each` runs in ~0.3 s vs ~1.2 s for four plain elements (one group per worker). **`tests/syntax/parallel_ok.kn`** gained the four ops (tree == VM).

Verification: gates **328/0/0 hung** (1_runall 74, 4_vm_diff 92, 8_bundle 15, 11_native 11, …), golden **321/0**, crosscheck **813/0**, stdlib **17/0**; `kirncheck` clean on the new files.

Loader bug fixed along the way (`src/interp/runtime.cpp`, same commit): `pet/parallel.kn` begins with `import parallel;` (the **native** module). When the entry file lived in `stdlib/pet/` — which the loader adds as an import dir because it is the entry's own directory — the bare name `parallel` resolved to the sibling file `stdlib/pet/parallel.kn`, i.e. the module bound _itself_, and `parallel.current_num_threads()` recursed until the stack overflowed (segfault). A bare `import m;` for a known prebound module name (`math`/`time`/`io`/`mem`/`json`/`text`/`os`/`parallel`/`error`/`sync`/`reflect`) now always binds the native module and never a same-named file. The module is also registered in `loadedModules_` **before** its body runs, so a re-entrant load sees it. This unblocked putting `parallel_test.kn` in `stdlib/pet/` alongside the other pet tests.

Deviations / not shipped (documented):

- **`par_chunks_mut` and `ParallelDrainRange` are absent.** Kirn lists are reference values with no in-place parallel mutation or drain-range protocol; use `map` to build the transformed list instead.
- **No `parallel.String.*`** (`par_lines`/`par_split`/`par_bytes`). The ops are list-only; split a string into lines/`chars` first (`str.split`) and run `par_chunk_by`/`par_split` on that.
- **Group sessions don't nest.** `chunks(...).chunks(...)` reshapes the already-grouped list (each element being a group), which is well-defined but rarely what you want; group once, then `map`.
- **`par_sort` has no `_unstable`/in-place variants** (carried over from Phase 8): it is a stable mergesort returning a new list.

### Update (2026-10-20) — §2.8: async time (`pet.async`)

**Path chosen:** build the time API on the existing native `time` pseudo-module instead of adding a new reactor, matching how `pet.async` already fronts `time.after`. Two small native primitives were missing: an **absolute-deadline** timer (`after_at`) for `sleep_until`/`timeout_at`, and a **wait-with-timeout** on a task handle (`wait`) for `timeout` — the latter cannot be a plain `await` because the point is to stop waiting before the task finishes. Instants and Durations are **Int milliseconds** (Instants are epoch ms on the `time.after` clock), so they interoperate with `time.now`/`time.after` and arithmetic without adding nominal types; `MissedTickBehavior` is a set of `pub const` strings.

Delivered:

- **Native `time` additions** (`src/interp/runtime.cpp`, `time` module handler): `time.after_at(deadline_ms)` returns a `VK::Timer` whose deadline is the absolute instant (immediate if already past); `time.wait(handle, ms) -> Bool` locks the task's `ThreadImpl` and waits on its condition variable until it is done or the budget elapses (returns `true` if done, `false` on timeout; `true` for a non-task handle). `time.wait` is the primitive `timeout` is built on.
- **`stdlib/pet/async.kn` §2.8 block**: `instant`, `elapsed` (saturating), `duration_since` (`none` if the later instant precedes the earlier, mirroring `checked_duration_since`), `checked_add`; `sleep_until(deadline_ms)`; `async fn timeout(ms, fut) -> ok(v) | err("elapsed")` and `timeout_at(deadline_ms, fut)`; `BURST`/`DELAY`/`SKIP` consts; `struct Interval` + `interval(period_ms)` / `interval_at(start_ms, period_ms)`. `Interval` ports tokio's `next_timeout` logic exactly: the schedule advances from the **fired deadline** by one period (`Burst`), from `now` (`Delay`), or to the next tick aligned to the original phase (`Skip`), with tokio's 5 ms missed-tick tolerance. `interval`'s first tick is immediate (start = now), like tokio; zero/negative periods raise.
- **`Interval.tick()` is a plain (non-`async`) method that advances the schedule and returns an awaitable timer**, so `await iv.tick()` both waits and persists the new `next_ms` in the receiver. An `async fn` method cannot: its receiver is a per-task copy and field writes are not written back to the caller. The schedule advances when `tick()` is called (the same point tokio's `poll_tick` samples `now`), so `Burst`/`Skip` stay phase-aligned.
- **`examples/78_async_time.kn`** + three goldens; the examples gate now reports **75** files. **`tests/syntax/async_time_ok.kn`** pins tree-walker == VM; **`stdlib/pet/async_test.kn`** gained the Instant/Duration, `sleep_until`, `timeout` ok/elapsed/`timeout_at`, interval cadence/behaviour, and zero-period assertions.

Verification: gates **328/0/0 hung** (1_runall 75, 2b_syntax 48, 4_vm_diff 93, 8_bundle 15, 11_native 11, …), golden **324/0**, crosscheck **819/0** (134 `.kn` files), stdlib **17/0**; `kirncheck` clean on the new files.

Deviations / not shipped (documented):

- **`Instant`/`Duration` are `Int` ms, not nominal types**, and `MissedTickBehavior` is `Str` consts (`BURST`/`DELAY`/`SKIP`), not an enum. This keeps them usable with the existing clock and arithmetic; the plan named types, not representations.
- **`timeout` returns `err("elapsed")`** (a `Str` payload), not `err(Elapsed)`: Kirn has no dedicated `Elapsed` error type. It re-raises a task's own error unchanged when the task finishes first.
- **`await iv.tick()` yields `none`**, not the tick's `Instant`: `await` on a timer is defined to yield `none`. Read `iv.next_ms` for the schedule, or `a.instant()` around the call if a timestamp is needed.
- **`tick()` is not cancel-safe.** Because the schedule advances at call time, cancelling the await (e.g. losing a `select` race) still consumes a tick. Tokio's `poll_tick` is cancel-safe; matching that would require advancing inside the wait, which the copy-receiver async-method semantics currently prevent.
- **`timeout` detaches the slow task.** Kirn tasks are eager OS threads with no cancellation token, so on timeout the task keeps running and its result is discarded rather than aborted.
- **`timeout` polls a single handle** via `time.wait`; it is not a general select over a timer and a future (that is §2.10's `select`, still unextended).

### Update (2026-10-20) — §2.9: async filesystem, process, signals

**Path chosen:** extend the native `os` pseudo-module with the blocking syscalls, then wrap each in an `async fn` in sibling stdlib modules — `pet.async_fs`, `pet.async_process`, `pet.async_signal` — re-exported from `pet.async` as `a.fs`, `a.process`, `a.signal`. An `async fn` runs on a task thread, so awaiting it yields to the caller; this is the same spawn-blocking shape `tokio::fs` uses, and avoids building a reactor.

Delivered:

- **Native filesystem** (`src/interp/runtime.cpp`, `os`): `read_file`/`read_bytes`, `write_file` (truncate), `copy_file` (overwrite; returns bytes copied), `rename`, `remove_file`, `create_dir_all`, `exists`, `metadata` (`{is_file, is_dir, size, modified_ms}`), `read_dir` (entry names), all via `<filesystem>`.
- **Native process** (`os`): `run(argv)` → `{code, stdout, stderr}`; `spawn(argv)` → `{pid, out_path, err_path, in_path}`; `wait(pid)` → status; `kill(pid)` → SIGKILL. fork/exec with temp-file redirection; **argv is built before `fork`** so the child of this multithreaded runtime only calls async-signal-safe functions before `execvp`.
- **Native signals** (`os`): `install_ctrl_c` (SIGINT handler sets a `volatile sig_atomic_t`), `ctrl_c_pending`, `reset_ctrl_c`.
- **`stdlib/pet/async_fs.kn`**: `read`, `read_to_string`, `write`, `copy`, `rename`, `remove_file`, `create_dir_all`, `exists`, `metadata`, `read_dir` (all `async fn`).
- **`stdlib/pet/async_process.kn`**: `command`/`arg`/`args` build a dict handle; `output`/`status`/`spawn` are `async fn`; `wait`/`kill`/`wait_with_output` operate on the child dict and clean up its temp files.
- **`stdlib/pet/async_signal.kn`**: `install`/`pending`/`reset`, plus `async fn ctrl_c()` (polls the flag on a 10 ms timer) and `stream(sig)` (one-item batch; SIGINT only).
- **`examples/79_async_fs_process.kn`** + three goldens (examples gate now **76**); **`tests/syntax/async_io_ok.kn`** pins tree-walker == VM; **`stdlib/pet/async_test.kn`** covers the fs round-trip, metadata/copy/rename/read_dir, process output/status, and the signal flag.

Verification: gates **331/0/0 hung** (1_runall 76, 2b_syntax 49, 4_vm_diff 94, 8_bundle 15, 11_native 11, …), golden **327/0**, crosscheck **843/0** (138 `.kn` files), stdlib **17/0**; `kirncheck` clean on the new files.

Deviations / not shipped (documented):

- **POSIX only.** `spawn` raises on Windows (filesystem and `run` still work); CI here is Linux.
- **`Command`/`Child` are dict values driven by free functions**, not structs with methods. At the time this phase shipped, a method on a struct declared in an imported module was dispatched with the _entry_ globals, so its body could not see the module's `os` import. That limitation was fixed in the §2.6/§2.7 update below; the dict shape is kept as the documented `a.process` API.
- **No stdin piping** (spawned children read EOF), and **no `fs.File`** with `AsyncRead`/`AsyncWrite`/`AsyncSeek`; fs operations are whole-file.
- **`signal.stream(sig)` is a one-shot batch**, not a stream (Kirn has no stream type); only SIGINT is supported, and `ctrl_c` **polls** every 10 ms rather than being woken by the handler.
- **No process cancellation/timeout**; use `kill`. `wait`/`wait_with_output` are single-shot (a second `waitpid` on the same pid fails).

### Update (2026-10-20) — §2.6 async I/O and §2.7 networking (`pet.async`)

**Path chosen:** extend the native layer with the blocking byte and socket calls, then expose faithful struct APIs in two sibling stdlib modules — `pet.async_io` (re-exported as `a.io`) and `pet.async_net` (`a.net`). Each wrapper that can block is an `async fn`, so it runs on a task thread and `await` yields to the caller: the same spawn-blocking shape used by `tokio::fs`/`tokio::net` without a reactor. This phase also fixed the blocker that had forced §2.9's process API to be free functions: method dispatch in an imported module now resolves names against **that module's** environment, so methods can call the module's own imports.

**Module-environment fix (`src/interp/runtime.cpp`, `src/interp/runtime.h`, included here):** added `structEnv_` (struct name → defining `Env`) plus `ImplEntry::env`. It is populated when struct definitions are collected (`globals_`), when a module body executes (the module env), and by `exec`'s StructDef (the current env). `invokeMethod` resolves the defining env and passes it to both `runFunc` (sync) and `spawnTask` (`fv.env`, async). Before this, both paths used `globals_`, so a method in `pet.async` saw neither its own `DELAY`/`SKIP` consts nor its imports; `Interval` had to spell the budget as string literals. Those consts are back in use, and `File`/`TcpStream` methods below call `os`/`net` directly. Inherited structs still fall back to `globals_`.

Delivered:

- **Native filesystem additions** (`os`): `read_at(path, offset, len)` → `bytes` (`len < 0` reads to EOF), `write_at(path, offset, data)` → bytes written (creates, never truncates), `file_size(path)` → `Int`. These make a cursor-based `File` possible without an OS handle.
- **Native networking** (`net`, POSIX only): `tcp_bind(addr)` → `{fd, addr}` (listens, `SO_REUSEADDR`, ephemeral port read back via `getsockname`), `tcp_accept(fd)` → `{fd, peer}`, `tcp_connect(addr)` → `{fd, peer}`, `tcp_read(fd, n)` → `bytes` (empty at EOF), `tcp_write(fd, data)` → bytes written (loops over partial sends), `close(fd)`; `udp_bind(addr)` → `{fd, addr}`, `udp_send_to(fd, data, addr)`, `udp_recv_from(fd, n)` → `{data, addr}`; `lookup_host(host)` → list of addresses (no port). Addresses are `host:port`, IPv6 as `[::1]:port`; helpers resolve with `getaddrinfo` and format with `getnameinfo(NI_NUMERICHOST)`.
- **`stdlib/pet/async_io.kn`**: `struct File` keeps `{path, pos}` in a **dict** field — dict/list fields are shared with the receiver copy, so the cursor persists across method calls and task hops (scalar field writes are not written back by `async fn`). Methods: `read`, `read_exact`, `read_to_end`, `read_to_string` (UTF-8), `read_line`, `lines`, `write`, `write_all`, `flush`, `seek`, `position`, `size`, `close`. Free functions `open`, `append`, `create`, `copy(reader, writer)`, `buffered_reader`, `buffered_writer`; `BufReader` delegates to `File`, `BufWriter` buffers `Str` chunks until `flush`.
- **`stdlib/pet/async_net.kn`**: `TcpListener` (`accept`, `local_addr`, `close`), `TcpStream` (`read`, `write`, `write_all`, `peer_addr`, `close`, `split`/`into_split`), `ReadHalf`/`WriteHalf`, `UdpSocket` (`send_to`, `recv_from`, `local_addr`, `close`); free `bind`, `async connect`, `udp_bind`, `lookup_host`. All handles are `Int` fds plus the formatted address.
- **Re-exports:** `pet.async` now binds `import pet.async_io as io;` and `import pet.async_net as net;`, so `a.io`/`a.net` are available next to `a.fs`/`a.process`/`a.signal`.
- **`examples/80_async_net_io.kn`** + three goldens (examples gate now **77**); **`tests/syntax/async_byteio_ok.kn`** pins tree-walker == VM over file I/O plus a TCP echo and a UDP datagram; **`stdlib/pet/async_test.kn`** gained read_line/copy/read_to_string and a localhost TCP echo.

Verification: gates **334/0/0 hung** (1_runall 77, 2b_syntax 50, 4_vm_diff 95, 8_bundle 15, 11_native 11, …), golden **330/0**, crosscheck **861/0** (141 `.kn` files), stdlib **17/0**; `kirncheck` clean on the new files; `netprobe`/`ioprobe` match byte-for-byte across tree-walker and VM.

Deviations / not shipped (documented):

- **No formal `AsyncRead`/`AsyncWrite`/`AsyncBufRead`/`AsyncSeek` traits.** Kirn has no trait-object machinery; the method set is a structural interface, and `copy` accepts any reader/writer with `read_to_end`/`write_all`.
- **`File` is a path cursor, not an OS handle.** Every read/write reopens the path at the recorded offset. There is no shared file description, no locking beyond the OS, and no `close` side effect. `write` does not truncate (use `create`). `append` records EOF at open time, so concurrent appenders can overwrite.
- **`split`/`into_split` share one fd** rather than duplicating it; the halves are values, not ownership transfer, and closing a half is not supported (close the stream). This is deliberate: the plan's borrowed/owned distinction has no analogue without lifetimes.
- **Networking is POSIX-only and blocking.** On Windows the `net` members raise. `await` yields, but there is no readiness reactor, so a blocked `accept` occupies a task thread. No `TcpSocket`/`UdpSocket` options (nodelay, TTL, broadcast) yet.
- **`lookup_host` returns address strings without ports** (the native resolver is called with a null service); combine with a port manually. No `ToSocketAddrs` port parsing beyond the `host:port` form used by `bind`/`connect`.
- **`BufReader` does not buffer** — whole-file reads are one syscall, and per-`read` buffering only complicates the cursor. `BufWriter` buffers `Str` chunks only.
- **No cancellation.** Dropping an await leaves the blocking syscall on its thread until it returns; `split` halves do not add independent shutdown.

### Update (2026-10-20) — §2.10 `select` over async sources, `else`, `biased`

**Path chosen:** extend the existing `select` statement rather than add a macro over futures. `select` already multiplexed channel receives, timers, and truthy "gates"; this update adds **task handles** as sources, a non-blocking **`else`** tail, and **`biased`** ordering. `join`/`try_join` were already shipped in the §2.8 commit on top of eagerly-scheduled tasks, so no further work was needed there.

Delivered:

- **Task arms.** A `case v = EXPR` arm whose `EXPR` evaluates to a task handle (`async fn` call) is ready once the task is done; the arm binds the task's result and joins it. A task's `raise` propagates at the select site as a catchable error, exactly like `await`; a panic still aborts.
- **`else`.** `select { case … else { … } }` runs the tail when one full pass finds no ready source. Arms are still evaluated fresh on each pass (timers are re-armed per pass, as before).
- **`biased`.** `select biased { … }` checks arms in source order. A plain `select` rotates its starting arm on every invocation (tracked by `selectTurn_`), so an always-ready arm cannot starve the others. Rotation was chosen over tokio's randomization because it is deterministic and testable.
- **Parser/AST/HIR/MIR.** `ast::Stmt::selBiased` and the reused `elseBody` are parsed; HIR `payload::Select` gained `biased`/`elseBody` and lowers/dumps/verifies them; MIR lowers the tail. `cloneStmt` now copies `selArms`/`selBiased` as well (it previously dropped select arms when cloning a body).
- **`examples/81_select_async.kn`** + three goldens (examples gate now **78**); **`tests/syntax/select_ok.kn`** pins tree-walker == VM; probes cover biased first-ready, both-ready bias, rotation (`[2,2]`), channel/`else`, timer/`else`, immediate timer, channel-vs-timer, and a raising task.

Verification: gates **337/0/0 hung** (1_runall 78, 2b_syntax 51, 4_vm_diff 96, 8_bundle 15, 11_native 11, …), golden **333/0**, crosscheck **867/0** (142 `.kn` files), stdlib **17/0**; `kirncheck` clean on the new files. Functions containing `select` are marked `interpreted` by the VM compiler and fall back to the tree-walker, so both engines produce identical output.

Deviations / not shipped (documented):

- **Losing arms are not cancelled.** A task that lost the race keeps running (and its result is discarded); there is no cancellation token. `select` also does not spawn: it polls in the calling task, sleeping 1 ms between passes, like the previous implementation.
- **`join`/`try_join` take a list**, `await a.join([t1, t2, t3])`, not positional varargs: Kirn only supports `...` in `extern` declarations. `join` waits for every task before re-raising the first error; `try_join` stops awaiting at the first error and leaves the rest running.
- **`biased`/rotation is per select invocation, not per poll.** Tokio randomizes the arm order on every poll of a single select; here the start index is fixed for the duration of one `select` execution. With one always-ready arm this is indistinguishable; with multiple, the per-invocation rotation prevents long-run starvation.
- **No `select!`-style `if` guards or per-arm patterns.** An arm is still `case [bind =] expr`; use an `if` inside the body for conditional logic.
- **`select` arms cannot bind `none` from a timer.** Timer arms bind `none` (as before); task arms bind the result value.

### Update (2026-10-20) — §2.5 async synchronization (`pet.async`)

**Path chosen:** thin `async fn` wrappers over the existing blocking primitives. Native `Mutex`/`RwMutex`/`Once` already exist, and `pet.sync` already implements `Semaphore`/`Notify`/`Barrier` on top of `Mutex` + `Cond` with all mutable state in a shared list; duplicating those state machines would add no value. `pet.async` re-exports the factories under the plan's names, so `a.Mutex(v)`/`a.RwLock(v)`/`a.Semaphore(n)`/`a.Notify()`/`a.OnceCell()`/`a.Barrier(n)` all live next to the rest of `pet.async`. A native `try_rlock` was the only missing operation.

Delivered:

- **Native `sync.RwMutex`**: added `try_rlock` (alias `try_read_lock`), a non-blocking shared read acquire used by `RwLock.try_read`.
- **`stdlib/pet/async.kn` §2.5 block** (imports the native namespace as `sysnc` and `pet.sync` as `psync`):
  - `Mutex(value)` → `MutexState` with `async lock()` → `MutexGuard`, `try_lock()` → guard|`none`, `get`, `async with(f)` (unlocks via `defer`, including on a raise), `is_locked`; guard `get`/`set`/`unlock`.
  - `RwLock(value)` → `RwLockState` with `async read()`/`write()`, `try_read()`/`try_write()`, `get`; `ReadGuard.get`/`unlock`, `WriteGuard.get`/`set`/`unlock`.
  - `Semaphore(permits)` → `AsyncSemaphore` with `async acquire`/`acquire_n`, `try_acquire`/`try_acquire_n`, `release`/`release_n`, `available`.
  - `Notify()` → `AsyncNotify` with `notify_one`/`notify_waiters`, `async notified()`, `waiter_count`.
  - `OnceCell()` → `OnceCellState` with `get`, `is_set`, `async get_or_init(f)`; initialisation is guarded by native `Once`, so concurrent callers run `f` at most once.
  - `Barrier(parties)` → `AsyncBarrier` with `async wait()` returning the generation.
- **Loader fix (`src/interp/runtime.cpp`)**: inside a module, `import NATIVE as alias;` now binds `alias`. The old code bound a prebound module only when the import had no alias, so `import sync as sysnc;` in `pet.async` left `sysnc` undefined when a factory ran. Entry-file imports already handled aliases; this closes the module-file gap (and is why the async sync API can name its factories `Mutex(...)` while still reaching the native constructor through an alias).
- **`examples/82_async_sync.kn`** + three goldens (examples gate now **79**); **`tests/syntax/async_sync_ok.kn`** pins tree-walker == VM; **`stdlib/pet/async_test.kn`** covers 4×100 mutex increments, `with`, `try_lock` contention, read/write/try guards, semaphore permits, a stored `Notify` permit, single-run `OnceCell`, and a two-party `Barrier`.

Verification: gates **340/0/0 hung** (1_runall 79, 2b_syntax 52, 4_vm_diff 97, 8_bundle 15, 11_native 11, …), golden **336/0**, crosscheck **873/0** (143 `.kn` files), stdlib **17/0**; `kirncheck` clean on the new files; the §2.5 probe matches across tree-walker and VM.

Deviations / not shipped (documented):

- **No poisoning and no RAII.** A `raise`/panic while holding a guard does not mark the lock poisoned; guards must be released with `unlock()`. `Mutex.with` is the exception: it uses `defer` so it unlocks even on a raise, but it passes the _value_, not a mutable guard.
- **`try_lock`/`try_read`/`try_write` return a guard or `none`**, not `Result`; callers test `== none`. `Mutex.try_lock` on a lock already held by the _same_ OS thread inherits `std::mutex` semantics (non-recursive; not a supported pattern).
- **`Mutex.get` is an unlocked peek**, for diagnostics/single-threaded reads; use a guard for shared mutation.
- **Waits block the task thread.** `lock`/`read`/`write`/`acquire`/`notified`/`wait` are `async fn`s for API shape and to keep `await` uniform, but the underlying primitive blocks; there is no async-aware waiter queue.
- **`OnceCell.get_or_init` takes a plain function value** (no `async` initialiser, no `await` inside `f`), and there is no `set`/`get_or_try_init`. `OnceCell.get()` returns `none` before initialisation (Kirn's `Option`).
- **`Notify` follows `pet.sync` semantics** (one stored permit, `notify_waiters` wakes current waiters), and `Barrier` is reusable with generation numbers, matching `pet.sync` rather than adding new behaviour.

### Update (2026-10-20) — §3.2 serde attribute options (shipped, reflection-based)

**Path chosen:** `@[derive(Serialize, Deserialize)]` is still inert and §3.1's traits remain blocked on generic impls / `static fn` / call-site type args, so this closes the §3.2 gap **where serde actually lives**: the shared reflection codec. The parser already stored `@[serde(...)]` on structs, fields and variants (Phase 0/1) but nothing read it. Attributes are now exposed as data through `reflect`, and `pet.codec` applies them to the tree, so JSON/TOML/YAML/CBOR all inherit the options without per-format work.

Delivered:

- **Native reflection** (`src/interp/runtime.cpp`):
  - `reflect.type_attrs(T)`, `reflect.field_attrs(T)`, `reflect.variant_attrs(T)`, `reflect.payload_attrs(T, variant)` return `{attrName: [{name, value}, …]}`. Arguments are literal-only (str/int/float/bool/list/`none`), so reflection never evaluates user code; a list of args (not a map) preserves repeated options such as `alias`.
  - Bare flags (`@[serde(skip)]`, `@[serde(untagged)]`) parse as positional idents and are normalised to `{name: "skip", value: true}`, so option lookup is uniform.
  - `reflect.case(name, rule, "field"|"variant")` implements serde's eight `rename_all` rules byte-for-byte from `serde_derive_internals::case` (including the field/variant split: snake variants insert `_` before capitals, snake fields are unchanged, pascal/camel strip `_`).
- **`stdlib/pet/codec.kn`** honours, in both directions:
  - container: `rename_all` (fields for structs, variants for enums), `deny_unknown_fields`, `transparent`, `tag`, `content`, `untagged`;
  - field: `rename`, `alias` (decode), `skip`, `skip_serializing`, `skip_deserializing`, `flatten` (`deny_unknown_fields` + `flatten` is rejected, as in serde);
  - variant: `rename`, `alias`, `skip`, `skip_serializing`, `skip_deserializing`.
  - Enum encodings: externally tagged (unchanged default), internally tagged `{tag: variant, <payload names>}` (uses the declared payload names), adjacently tagged `{tag, content}` (keeps the list payload convention), and `untagged` (unit → `null`, single payload → the value, multiple → a list).
  - Untagged decode tries variants in declaration order with a shape check (`type_ok`) before coercion, so the first variant no longer always wins through the lenient scalar coercion.
- **`reflect.new` defaults fixed**: field types spelled `Str`/`Bool`/`Float` now get `""`/`false`/`0.0` (they previously fell through to `none`), which is what makes `@[serde(skip)]` and missing fields produce the declared default.
- **`examples/83_serde_options.kn`** + three goldens (examples gate now **80**), including a TOML render with camelCase keys; **`tests/syntax/serde_options_ok.kn`** pins tree == VM; **`stdlib/pet/serde_test.kn`** gained rename/skip/default/flatten/transparent/deny/alias and all three enum tagging modes.

Verification: gates **343/0/0 hung** (1_runall 80, 2b_syntax 53, 4_vm_diff 98, 8_bundle 15, 11_native 11, …), golden **339/0**, crosscheck **879/0** (144 `.kn` files), stdlib **17/0**; `kirncheck` clean on the new/changed files; golden record produced exactly the three new files.

Deviations / not shipped (documented):

- **`with`, `serialize_with`, `deserialize_with`, `skip_serializing_if` and `default = "path"` were initially not implemented** — they name user functions, and module-exported functions are not reachable by name at runtime. `default` is accepted as a bare flag; behavior is unchanged because missing fields already receive the declared type's default. **The function-valued options shipped in the follow-up below.**
- **Missing required fields do not raise.** serde errors unless a field is `default`/`Option`; Kirn's `reflect.new` fills missing fields with type defaults, so `default` is effectively implicit. `deny_unknown_fields` covers only unknown keys.
- **No `Option`-aware skipping by default**: `none` is encoded as `null` and never omitted; `skip_serializing_if` (shipped in the follow-up) is the way to omit it.
- **`rename_all_fields`, container `rename`, `borrow`, `bound`** are parsed but were initially ignored (struct-style enum variants do not parse in Kirn, and the bundled formats do not use a type name). **`rename_all_fields` shipped in the follow-up; the rest remain ignored.**
- **Per-variant `untagged` is applied on encode only**; decode uses the enum-level `untagged` mode. Untagged matching is shape-based, not a serde `Visitor`.
- **Internal tagging requires named payloads**; an unnamed payload is emitted under a positional `vN` key. External unit variants stay `{"Variant": null}` (Kirn's existing convention, not serde's bare `"Variant"`), and TOML still cannot round-trip unit variants (pre-existing, documented in Phase 7).

### Update (2026-10-20) — §2.3 tasks: cancellation, TaskSet, spawn_blocking

**Path chosen:** the runtime is thread-per-task, so this ships tokio's _handle_ surface (`abort`, `is_finished`, `JoinSet`) on top of the eager tasks rather than a cooperative cancellation token woven through every await. `abort()` sets an atomic on the `ThreadImpl`; `await`/`join` on a cancelled, still-running handle returns `none` instead of blocking, and `is_finished()` reports true. The OS thread keeps executing (there is no stack unwinding/drop), which is the honest semantic for eager threads and is what the OS-thread layer can guarantee without rewriting the task model. `select` now also cancels the task arms it did not pick, and `try_join` cancels the handles it never reached.

Delivered:

- **Native task handle** (`src/interp/runtime.{h,cpp}`): `ThreadImpl::aborted` (atomic); `thread` protocol gains `is_finished()` (done || aborted), `is_aborted()`, and `abort()`. `awaitTask` returns `none` immediately for an aborted, unfinished task (a task that already finished still returns its result or re-raises); `is_finished` is checked under the task mutex.
- **`select` cancellation** (`selectExec`): after an arm is chosen, every other `Prepared::Task` handle is aborted; an arm whose handle was already aborted is treated as ready with payload `none` (so a select cannot spin on a cancelled task).
- **`pet.async`**:
  - `TaskSet()` → `TaskSetState` with `spawn(t)`/`add(t)`, `len`, `is_empty`, `async join_next` (FIFO: shifts the oldest handle), `async join_all`, `abort_all`, and `async shutdown` (abort-all then drain).
  - `spawn_blocking(f)` = `spawn f()`, available in any (sync) context.
  - `try_join` now aborts every handle after the one that raised, matching its documented short-circuit.
- **Checker** (`src/sema/checker.cpp`): the `thread` protocol types `is_finished`/`is_aborted` → `Bool` and `abort` → `none`.
- **Examples/tests**: `examples/84_task_control.kn` + three goldens (examples gate **81**), `tests/syntax/async_tasks_ok.kn`, and `async_test.kn` coverage for abort → `none`, `is_finished`, TaskSet FIFO/join_all/shutdown, `try_join` aborting un-reached handles, select cancelling the losing task, and `spawn_blocking`.

Verification: gates **346/0/0 hung** (1_runall 81, 2b_syntax 54, 4_vm_diff 99, 8_bundle 15, 11_native 11 — one earlier 11_native run hit a transient build timeout and passed on rerun), golden **342/0**, crosscheck **885/0** (145 `.kn` files), stdlib **17/0**; `kirncheck` clean on the new/changed files.

Deviations / not shipped (documented):

- **Cancellation is a handle flag, not an interrupt.** The eager OS thread runs to completion; there are no cancellation points, no drop-based cancellation, and `shutdownThreads` still joins outstanding tasks (an aborted long task delays process exit). This is the §2 "cancel-safe by drop" goal only at the handle level, not in the body.
- **Awaiting a cancelled task yields `none`**, not a `Result`/`JoinError::Cancelled`; callers cannot distinguish "aborted" from "returned none" by value (use `is_aborted()` before/after).
- **`join_next` is FIFO**, not tokio's completion order (the thread-per-task runtime has no completion wakeup to order by). `TaskSet.spawn(t)` _tracks_ an already-started handle; it cannot accept a lazy future.
- **`spawn_blocking` is `spawn` with tokio's name** — there is no separate blocking pool or worker cap.
- **`select` aborts task arms only**; channel/timer arms hold no resource to cancel. A losing task arm's spawned work still runs its current body.
- **Task typing is still erased**: `spawn(...)` is `thread`, but a direct `async fn` call is typed as its declared return `T` (the §2.2 deviation), so store such handles in `any` before calling `abort()`/`is_finished()`.

### Update (2026-10-20) — §3.2 follow-up: function-valued serde options

The §3.2 deviation "function-valued options are not implemented" was caused by name resolution, not by the codec: attribute strings like `"crate::helpers::skip"` could not be turned into callables. This adds the resolver and then wires the remaining field options through the shared tree codec, so all four formats inherit them.

Delivered:

- **`reflect.fn(name)`** (native): resolves a callable by name, mirroring ident lookup — entry `globals_` first (decorated functions/`const` bindings), then plain top-level functions in `funcs_` (materialized as `VK::Fn` with the entry env), then every `loadedModules_` environment, then each struct's defining module (`structEnv_`). Serde paths use only the final `::` segment, so `"crate::skip"`/`"helpers::skip"`/`"skip"` are equivalent. Missing names raise.
- **`pet.codec` field options**, encode and decode:
  - `skip_serializing_if = "f"` — `f(value)` is consulted before encoding (and inside `transparent`); a true result omits the field.
  - `default = "f"` — `f()` supplies a missing field (including `skip_deserializing` fields). Bare `default` remains implicit.
  - `serialize_with = "f"` / `deserialize_with = "f"` — because this codec works on trees, `serialize_with(f)` returns the **tree** to emit (not a `Serializer` write) and `deserialize_with(f)` consumes the decoded **tree**; the results bypass scalar coercion.
  - `rename_all_fields = "rule"` — applies `rename_all` to internally-tagged enum payload names on encode and decode (serde's field rules apply, so `snake_case` is a no-op on already-snake fields; `camelCase`/`PascalCase` transform).
- **Examples/tests**: `examples/83_serde_options.kn` gained a `Server` struct (skip-if-zero, default path, custom point codec) and a `Motion` enum (`rename_all_fields = "camelCase"`), with goldens re-recorded; new `tests/syntax/serde_hooks_ok.kn` (clean, tree == VM); `stdlib/pet/serde_test.kn` asserts the same surface.

Verification: gates **346/0/0 hung** (1_runall 81, 2b_syntax **54**, 4_vm_diff 99, 8_bundle 15, 11_native 11 — one earlier 11_native run hit a transient build timeout and passed on rerun), golden **342/0**, crosscheck **885/0** (145 `.kn` files), stdlib **17/0**; `kirncheck` clean on the new/changed files.

Deviations / not shipped (documented):

- **Hook contract is tree-based, not serde's `Serializer`/`Deserializer`.** A `serialize_with` hook must return a JSON-tree value (dict/list/scalar); a `deserialize_with` hook receives that tree. Hooks that expect to write bytes or stream cannot be expressed this way.
- **No `with = "module"`**: serde's module form needs two well-known entry points (`serialize`/`deserialize`) in a named module; use `serialize_with`/`deserialize_with` instead.
- **Name resolution is flat.** Hooks are found by final segment across the entry globals, all loaded modules, and struct-defining modules; there is no visibility/ambiguity check, so two modules exporting the same hook name make the result depend on map iteration. Attribute arguments also remain literal-only (a hook cannot take extra parameters).
- **`borrow`/`bound`/container `rename` remain ignored**; `borrow` is a compile-time lifetime hint and `rename` on a container is not emitted by the tree codec (type names never appear in the output).
- Hook functions see the caller's module environment only through the resolver's search; closures/lambdas cannot be named by an attribute path.

### Update (2026-10-20) — §4.2 parallel iterator combinators (shipped)

**Path chosen:** the spawn-based `ParIter` (§4, Phase 4) already fans a slice per worker and reassembles in order, so the remaining rayon combinators are pure stdlib additions on top of it — no compiler change. A new `slices()` helper cuts `xs` into one contiguous slice per worker (honouring the `divided` grain so grouping iterators still fan out per group); the item-preserving adaptors map each slice, then `parallel.invoke` runs them and the partials are concatenated in order, which is exactly the sequential result. Reference read directly from `langs-source-code/rust/rayon/src/iter/{filter_map,flat_map,find,positions,min_by,max_by,zip,enumerate,take,skip,step_by,fold,try_reduce,for_each_init}.rs`.

Delivered (`stdlib/pet/parallel.kn`):

- **Item-preserving adaptors**: `filter_map(f)` (`f` returns a value to keep or `none` to drop), `flat_map(f)` (`f` returns an iterable, concatenated in element order), `flatten()` (one level).
- **Searching**: `find_any`/`find_first` (first in order), `find_last` (reverse), `positions(f)` (indices), each deterministic because the fan-out reassembles in order.
- **Ordered min/max**: `min_by`/`max_by` (a `(a, b) -> Int` comparator, as rayon's `min_by`/`max_by`) and `min_by_key`/`max_by_key` (by a key function).
- **Reshaping**: `zip(other)` (pairs `[a, b]`, truncated to the shorter input), `enumerate()` (pairs `[i, e]`, since Kirn has no tuple value), `take`/`skip`/`step_by` (index-space rewrites returning a fresh, undivided iterator; `step_by` rejects `n <= 0`).
- **Reducers**: `fold(identity, op)` (per-slice seed from `identity()`, partials combined pairwise with `op`), `try_reduce(init, op)` (`op` returns `ok`/`err`; the first `err` in order short-circuits and is returned, else `ok(final)`), `for_each_init(seed, f)` (one `seed()` per worker, then `f(state, e)` for its slice).
- **Examples/tests**: `examples/85_parallel_combinators.kn` + three goldens (examples gate now **82**); `tests/syntax/empty_closure_ok.kn`; `stdlib/pet/parallel_test.kn` extended with ordered and large-input (`20000`) coverage of every combinator.

**Incidental bug fixed** (found while testing `for_each_init`): an **empty-body block closure** (`fn (…) { }`) was classified by `lambdaBody.empty()` as an arrow lambda, so invoking it raised `too many lambda arguments` (and it took no parameters). A new `ast::Expr::isBlockClosure` flag is set by the parser and used by `callValue`, the checker, and the HIR lowering, so an empty block closure now takes its declared parameters and returns `none`.

Verification: gates **350/0/0 hung** (1_runall 82, 2b_syntax 56, 4_vm_diff 100, 8_bundle 15, 11_native 11, …), golden **345/0** (exactly the three new files recorded), crosscheck **891/0** (146 `.kn` files), stdlib **17/0**; `kirncheck` clean on the new/changed files.

Deviations / not shipped (documented):

- **No `.par_iter()` method form**, `.into_par_iter()`, or `.par_iter_mut()` — `par_iter(xs)` remains a free function (the checker has no `ParIter` return type to hang list methods on), and mutation across threads is still unrepresentable.
- **`fold` is a seed-and-combine reduce, not rayon's lazy `Fold` iterator.** It returns the reduced value directly; combining partials needs an associative `op` for a stable result (rayon has the same caveat).
- **`try_reduce` is sequential per slice** (it does not fan out, because a short-circuit at an arbitrary point would make the winner nondeterministic): the first `err` in element order wins. `ok`/`err` values are used instead of a `Try` trait.
- **`find_any` is deterministic** (first in order), not rayon's racy first-found; `find_first`/`find_last` are aliases for the two directions.
- **`zip` truncates** to the shorter input instead of raising `zip_eq`-style, and produces `[a, b]` lists (no tuple value); likewise `enumerate` yields `[i, e]`.
- **`positions` of a large input is sequential** (index order matters); the item-preserving adaptors are the parallel ones.
- **No `chain`/`interleave`/`rev`/`cloned`/`copied`/`inspect`/`map_with`/`while_some`/`flatten_iter`/`par_bridge`** yet; the §4.2 list named the combinators above.
- **Sendability is still unchecked.** A closure that mutates captured shared state inside `for_each`/`for_each_init`/`flat_map` can race; use per-slice `init()` state (or `@[par] for`, which the tree-walker lowers) for mutations.

### Update (2026-10-20) — `pet.error` module file + native gate split

- **`stdlib/pet/error.kn`** now exists so `import pet.error` resolves a real, file-backed module like the other `pet` modules. It re-exports the native surface (`msg`, `context`, `chain`, `root_cause`, `is`, `downcast`, `display`, `bail`, `ensure`); the `Error` value, Display/chain internals, `?` conversion, and `@[derive(Error)]` remain native because they need runtime type identity. `error.new` cannot be declared in `.kn` (`new` is a reserved keyword), so `moduleMember`'s file-backed branch falls back to the native pseudo-module of the same base name; `import pet.error; error.new(...)` therefore still works. Covered by `tests/syntax/error_module_ok.kn`.
- **Race-free Pool examples**: `Pool.scope` tasks run concurrently, so the §4.1 examples/tests now count with `sync.Atomic` instead of appending to a shared list (the old form was an unsynchronized data race and made `86_parallel_pool.kn` flake in gate 4). Output is unchanged; only 86's lex/parse goldens changed (new `pet.sync` import).
- **Native gate split**: `scripts/gates/11_native.sh` (one ~13–20m script that often timed out under load) is replaced by three standalone gates — `11a_native.sh` (KIRN_HAS_NATIVE surface, fast), `11b_native.sh` (new-surface + optional-param), `11c_native.sh` (const-inline + coalesce-assign) — totalling the same 11 checks. Per-build timeout raised 300s → 900s.
- Verification: gates **357/0/0 hung** (1_runall 84, 2b_syntax 59, 4_vm_diff 102, 8_bundle 15, 11a 7, 11b 2, 11c 2, …), golden **351/0**, crosscheck **909/0**, stdlib **17/0**.

### Update (2026-10-21) — §2.4 channels: `Mpsc` / `Unbounded` in `pet.sync` and `pet.async` (shipped)

**Path chosen:** add bounded backpressuring mpsc and unbounded mpsc in `stdlib/pet/sync.kn` using the established `Mutex` + `Cond` + shared-list pattern (no compiler changes). The API returns `[tx, rx]`; `send`/`try_send` and `recv`/`try_recv` follow tokio-style semantics. Re-exported from `stdlib/pet/async.kn` (`Mpsc`/`Unbounded`/`mpsc`/`unbounded`) alongside the existing oneshot/broadcast/watch helpers.

Delivered:

- **`pet.sync`**: `Mpsc(capacity)` and `Unbounded()` returning `[MpscSender, MpscReceiver]`; sender methods `send(v)`, `try_send(v)`, `close()`, `is_closed()`; receiver methods `recv()` (returns `none` on close), `try_recv()`, `close()`, `is_closed()`, `len()`, `is_empty()`. `Unbounded` uses a shared list with no capacity limit; `Mpsc` enforces backpressure and `try_send` returns `false` when full.
- **`pet.async`**: re-exports `Mpsc`/`Unbounded`/`mpsc`/`unbounded`/`oneshot`/`broadcast`/`watch`.
- **Tests**: `stdlib/pet/sync_test.kn` extended with `test_mpsc_unbounded`, `test_mpsc_bounded`, `test_mpsc_close`.

Verification: `kirn test stdlib/pet/sync_test.kn` PASS (1 passed); `kirnrun --no-vm stdlib/pet/sync_test.kn` and VM both report the new mpsc tests OK. Both engines produce identical behavior for the new surface.

### Update (2026-10-21) — §4.2 parallel iterators: `rev` / `chain` / `inspect` / `cloned` / `copied` / `interleave` / `flatten_iter` / `while_some` / `map_with` / `par_bridge` (shipped)

**Path chosen:** stdlib additions on the spawn-based `ParIter` (no compiler changes). `rev`/`chain` return new `ParIter`s with `divided` set appropriately so downstream fanning honors the grain; `inspect(f)` maps through `f` for side effects; `cloned`/`copied` are identity maps; `interleave`/`flatten_iter`/`while_some`/`map_with` added; `par_bridge(xs)` provides a compatibility hook.

Delivered (`stdlib/pet/parallel.kn`): `ParIter.rev()`, `ParIter.chain(other)`, `ParIter.inspect(f)`, `ParIter.cloned()`, `ParIter.copied()`, `ParIter.interleave(other)`, `ParIter.flatten_iter()`, `ParIter.while_some(f)`, `ParIter.map_with(init, map_fn)`, `par_bridge(xs)`.

Verification: quick probes confirm VM and `--no-vm` behavior; `kirn test stdlib/pet/parallel_test.kn` PASS, `scripts/gates/6_test.sh` green.

### Update (2026-10-21) — §2.10 `select` over `await` arms (shipped)

**Path chosen:** extend `Interpreter::selectExec` to recognize `case v = await EXPR`. Evaluating the awaited operand spawns async fn calls / yields task handles or timers; those are routed to the existing Task/Timer arms so the first ready await wins, with the losing task arms cancelled as before. No parser or checker changes required (await is already parsed/checked).

Delivered: `src/interp/runtime.cpp` `selectExec` await-arm handling; `examples/88_select_await.kn` + three goldens. Works in both VM and `--no-vm`.

Verification: `88_select_await.kn` prints `10 timer`; gates 1_runall now **85 passed, 0 failed**.

### Update (2026-10-21) — timed waits for sync primitives (shipped)

**Path chosen:** add `Cond.wait_timeout`/`wait_timeout_ms` to native `Cond` using `std::condition_variable::wait_for`; extend `pet.sync` with `recv_timeout` on `Oneshot`/`MpscReceiver` and `changed_timeout` on `WatchRx`. Return values follow existing conventions (`none` on close/empty for channels, `['timeout']` marker on timeout, `false` for watch).

Delivered: `src/interp/runtime.cpp`, `stdlib/pet/sync.kn`.

Verification: timed probes return timeout as expected; `scripts/gates/6_test.sh` green.
