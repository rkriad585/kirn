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

### 4.4 `@[par]` decorator

`@[par]` on a `for` loop or a map/fold lowers the body to the parallel iterator equivalent. `@[par]` on a pure `fn` marks it safe to run on the pool.

```kirn
@[par]
for row in rows { row.score = score(row); }

# equivalently:
rows.par_iter_mut().for_each(fn (row) { row.score = score(row); });
```

**Phase 4 exit:** `examples/66_parallel_iter.kn`, `examples/67_parallel_sort.kn`; `@[par]` equals the manual par form; gates green.

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
