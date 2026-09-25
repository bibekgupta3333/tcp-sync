# C++ cheatsheet for Python developers

This cheatsheet maps each concept in this codebase to its Python equivalent. The C++ examples are taken from, or simplified from, `src/` and `include/`.

## Mental model shifts

| Topic | Python | C++ |
|---|---|---|
| Memory | GC + refcounting; you rarely think about it | You decide lifetime. **RAII**: the destructor runs deterministically at scope exit |
| Types | Dynamic, duck-typed | Static and checked at compile time; templates provide compile-time duck typing |
| Errors | Exceptions everywhere | Exceptions *plus* C APIs returning `-1` and setting `errno`, which you must check |
| Threads | GIL: one thread runs Python bytecode at a time | Real parallelism. Data races are **undefined behavior**, not just wrong answers |
| Copies | Assignment binds a name (reference semantics) | Assignment **copies** by default; `std::move` transfers; `&` is a reference |
| Build | Interpreted; `python x.py` | Compile + link: headers (`.hpp`) declare, sources (`.cpp`) define |

## Syntax quick map

| Python | C++17 |
|---|---|
| `x = 5` | `int x = 5;` or `auto x = 5;` |
| `s = "hi"` | `std::string s = "hi";` |
| `lst = [1, 2]` | `std::vector<int> v{1, 2};` |
| `d = {"a": 1}` | `std::unordered_map<std::string, int> d{{"a", 1}};` (hash) / `std::map` (sorted) |
| `st = {1, 2}` | `std::unordered_set<int> st{1, 2};` |
| `collections.deque` / `queue.Queue` | `std::deque` / `std::queue` + mutex + condition_variable |
| `None` | `std::nullopt` in `std::optional<T>`, or `nullptr` for pointers |
| `for k, v in d.items():` | `for (const auto& [k, v] : d) {}` |
| `lambda x: x + 1` | `[](int x) { return x + 1; }` |
| closure over `y` | `[y](int x) { return x + y; }` (copy) / `[&y]` (reference) |
| `f"{a}:{b}"` | `a + ":" + std::to_string(b)` or `std::ostringstream` |
| `s.split()` | `split_words(line)` → `std::vector<std::string_view>` (hand-written) |
| `int("42")` | `std::from_chars` (no exceptions, no locale), see `parse_u64` |
| `def f(x: int) -> str:` | `std::string f(int x);` |
| `class A(B):` | `class A : public B {};` |
| `__init__` / `__del__` | constructor / **destructor** (reliably called at scope exit) |
| `@staticmethod` | `static` member function |
| `raise ValueError("x")` | `throw std::invalid_argument("x");` |
| `except ValueError as e:` | `catch (const std::invalid_argument& e) {}` |
| `import x` | `#include "x.hpp"` (textual) + linking the `.o` |

## RAII vs `with`

```python
with open("f", "rb") as f:   # closed at end of block
    data = f.read()
```
```cpp
{
    UniqueFd fd(::open("f", O_RDONLY));   // include/tcpsync/net.hpp
    // ... use fd.get() ...
}   // ~UniqueFd() closes it here, even if an exception was thrown
```
In C++, *every* object is effectively a context manager, and you don't need a `with` keyword. The codebase leans on this:
- `UniqueFd` closes sockets and files.
- `FileStore::Upload` deletes its temp file unless you called `commit()`.
- `std::lock_guard` / `std::unique_lock` / `std::shared_lock` release mutexes.

## Ownership and smart pointers

| Python | C++ | Used in this repo |
|---|---|---|
| every object is refcounted | `std::shared_ptr<T>` (atomic refcount) | Carrying a move-only `UniqueFd` inside a copyable `std::function` (`server.cpp`) |
| — | `std::unique_ptr<T>` (single owner, zero overhead) | `addrinfo` with a custom deleter `freeaddrinfo` (`net.cpp`) |
| — | move-only class (`= delete` copy) | `UniqueFd`, `FileStore::Upload`, `Client` |
| `copy.copy(x)` | copy constructor (default for values) | — |
| handing an object over | `std::move(x)` — x is left "valid but unspecified" | `handle_connection(std::move(*shared), peer)` |

**Rule of thumb:** use values by default, `unique_ptr` when you need the heap, and `shared_ptr` only when ownership really is shared. Never write `new`/`delete` yourself.

## Threads and synchronization

| Python (`threading`) | C++ (`<thread>`, `<mutex>`, …) | Notes |
|---|---|---|
| `t = Thread(target=f); t.start()` | `std::thread t(f);` (starts immediately) | You **must** `join()` or `detach()` before destruction, or `std::terminate` |
| `t.join()` | `t.join()` | |
| `lock = Lock()` | `std::mutex mu;` | |
| `with lock:` | `std::lock_guard<std::mutex> g(mu);` | scope-bound |
| `RLock` | `std::recursive_mutex` (usually a design smell) | |
| — | `std::shared_mutex` + `std::shared_lock` (read) / `std::unique_lock` (write) | `FileStore::mu_`: many readers, one writer |
| `Condition()` / `cond.wait_for(pred)` | `std::condition_variable` + `cv.wait(lock, pred)` | `ThreadPool` (always pass the predicate) |
| `Event` | `std::promise<void>` / `std::future` | pool tests |
| `ThreadPoolExecutor` | none in std; see `include/tcpsync/thread_pool.hpp` | |
| `queue.Queue(maxsize=n)` | `std::queue` + mutex + cv + size check | bounded queue → `ERR busy` |
| module-level `int` is "safe enough" under the GIL | `std::atomic<int>`; a plain `int` shared across threads is a data race (UB) | `stopping_`, test counters |
| exception in thread → printed, thread dies | exception escaping a `std::thread` → **whole process aborts** | hence the `try` in `worker_loop` |
| `signal.signal(SIGINT, h)` (runs in main thread) | block signals + `sigwait()` in a dedicated thread | `server_main.cpp` |

**The GIL difference matters.** In Python, `counter += 1` from many threads loses updates but never crashes. In C++ the same code is undefined behavior: the compiler may cache the value in a register, tear writes, or reorder them. ThreadSanitizer (`make tsan`) finds these races.

## Sockets

| Python `socket` | POSIX C API (what C++ calls) | Gotcha handled in `net.cpp` |
|---|---|---|
| `socket.create_server(("", 9000))` | `getaddrinfo` → `socket` → `setsockopt(SO_REUSEADDR)` → `bind` → `listen` | IPv4/IPv6 via `AF_UNSPEC` |
| `conn, addr = s.accept()` | `int fd = accept(lfd, ...)` | `EINTR`, `ECONNABORTED`, `EMFILE` |
| `s.sendall(b)` | loop over `send()`; it may write only part | `send_all` |
| `s.recv(n)` | `recv()` returns `0` on EOF, `-1` + `errno` on error | `Reader::recv_into` |
| `s.makefile().readline()` | hand-written buffered reader | `Reader::read_line` (so payload bytes after the header aren't lost) |
| `s.settimeout(30)` | `setsockopt(SO_RCVTIMEO / SO_SNDTIMEO)` → `EAGAIN` | `configure_stream_socket` |
| `BrokenPipeError` exception | **SIGPIPE kills the process** by default | `MSG_NOSIGNAL` / `SO_NOSIGPIPE` / `SIG_IGN` |
| `selectors` / `select.poll` | `poll()` | accept loop + self-pipe wakeup |
| `s.close()` | `close(fd)`; `shutdown(fd, SHUT_RDWR)` wakes blocked readers in other threads | `Server::shutdown_connections` |

## Strings and bytes

| Python | C++ |
|---|---|
| `str` (Unicode) vs `bytes` | `std::string` is just bytes; can hold `'\0'` and binary data |
| `memoryview` / slicing without a copy | `std::string_view` (non-owning; **must not outlive** the source) |
| `b"".join(chunks)` | `out.append(ptr, len)` |
| `hex(x)` | `snprintf("%016llx")` / `std::from_chars(..., 16)` |

## Error handling

```python
try:
    os.rename(a, b)
except OSError as e:
    print(e.errno)
```
```cpp
if (::rename(a.c_str(), b.c_str()) != 0)                 // C API: check return code
    throw std::system_error(errno, std::generic_category(), "rename");   // then raise
```
The repo's exception types are `NetError` (socket), `ProtocolError` (malformed message), `ServerError` (the server said `ERR`), `std::system_error` (disk) and `CheckFailure` (tests).

## Build and tooling

| Python | C++ |
|---|---|
| `pip install` | system packages / vendoring (this repo has zero deps) |
| `python -m pytest` | `make test` (custom runner in `tests/unit_tests.cpp`) |
| `pdb` | `gdb` / `lldb` (see [DEBUGGING.md](DEBUGGING.md)) |
| `tracemalloc` | Valgrind memcheck, AddressSanitizer |
| — (the GIL hides races) | ThreadSanitizer, Helgrind |
| `mypy` | the compiler itself, plus `-Wall -Wextra -Wpedantic -Wshadow` |

## The same server in ~25 lines of Python (for comparison)

```python
import socket, threading, os
lock = threading.Lock()                      # C++: std::shared_mutex (readers don't block each other)

def handle(conn):
    f = conn.makefile("rb")
    for line in f:
        cmd, *args = line.decode().split()
        if cmd == "UPLOAD":
            name, size = args[0], int(args[1])   # C++: is_valid_name() first!
            tmp = f".tmp.{threading.get_ident()}"
            with open(tmp, "wb") as out:
                out.write(f.read(size))          # C++: streamed in 64 KiB chunks
            with lock:
                os.replace(tmp, name)            # C++: rename() under unique_lock
            conn.sendall(b"OK\n")
        elif cmd == "DOWNLOAD":
            with lock:
                fh = open(args[0], "rb")         # C++: open() under shared_lock
            data = fh.read()
            conn.sendall(f"OK {len(data)}\n".encode() + data)
    conn.close()

srv = socket.create_server(("0.0.0.0", 9000))
while True:
    c, _ = srv.accept()
    threading.Thread(target=handle, args=(c,), daemon=True).start()   # C++: bounded pool
```
What the C++ version adds, and why:
- a bounded pool, so a flood of connections can't spawn unlimited threads
- timeouts
- name validation, so `../../etc/passwd` is rejected
- streaming, so a 1 GiB upload doesn't need 1 GiB of RAM
- end-to-end hashes
- lingering close
- graceful shutdown
- crash cleanup
- deterministic resource release
