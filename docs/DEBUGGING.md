# Debugging: GDB / LLDB, Valgrind, sanitizers

## Platform notes

| Tool | Linux | macOS (Apple Silicon) |
|---|---|---|
| gdb | ✅ | ❌ not supported, so use **lldb** (same concepts, commands below) |
| Valgrind | ✅ memcheck, helgrind, drd | ❌ not supported, so use `make docker-check` |
| ASan/TSan | ✅ | ⚠️ broken with Command Line Tools clang 17 on macOS 26.4 (TSan segfaults and ASan hangs, even on a trivial program), so use `make docker-check` |

`make docker-check` builds an Ubuntu 24.04 image and runs, in order:
- `make test`
- `make asan`
- `make tsan`
- `make valgrind`

## gdb ↔ lldb cheat sheet

| Task | gdb | lldb |
|---|---|---|
| start | `gdb --args build/tcpsync-server --port 9000` | `lldb -- build/tcpsync-server --port 9000` |
| break on function | `b tcpsync::FileStore::commit` | `b tcpsync::FileStore::commit` |
| break file:line | `b file_store.cpp:95` | `b file_store.cpp:95` |
| conditional break | `b server.cpp:180 if size > 1000000` | `br s -f server.cpp -l 180 -c 'size > 1000000'` |
| run / continue | `r` / `c` | `r` / `c` |
| step over / into / out | `n` / `s` / `finish` | `n` / `s` / `finish` |
| backtrace | `bt` | `bt` |
| all threads' stacks | `thread apply all bt` | `bt all` |
| switch thread | `thread 3` | `thread select 3` |
| print | `p meta`, `p index_.size()` | `p meta`, `p index_.size()` |
| watch a variable | `watch next_version_` | `w s v next_version_` |
| don't stop on SIGPIPE | `handle SIGPIPE nostop noprint pass` | `pro hand -s false -p true SIGPIPE` |
| attach to running server | `gdb -p $(pgrep tcpsync-server)` | `lldb -p $(pgrep tcpsync-server)` |
| core file | `ulimit -c unlimited; gdb build/tcpsync-server core` | `lldb -c /cores/core.<pid>` |

In Docker, run `docker run --rm -it --cap-add=SYS_PTRACE tcpsync-dev bash` to get a shell where gdb can attach.

## Drills: reproduce, observe, explain

Each drill introduces a bug on purpose, shows how to catch it with a tool, and points at the line that prevents it.

### Drill 1: the data race TSan catches
Temporarily remove the lock in `FileStore::stat()` (`file_store.cpp`):
```cpp
// std::shared_lock lock(mu_);
```
Then run `make tsan` (Linux or Docker). The report shows `WARNING: ThreadSanitizer: data race` on `unordered_map` internals, with two stacks: `stat` (read) and `commit` (write). **Lesson:** a map that one thread mutates while others read it is UB. `shared_mutex` makes readers cheap without making them unsafe.

### Drill 2: the leak Valgrind catches
In `FileStore::begin_upload`, replace `UniqueFd fd(::open(...))` with a raw `int` that is never closed. Then run `valgrind --track-fds=yes build/unit_tests file_store`. The exit summary shows `FILE DESCRIPTORS: N open`, with the `open()` stack for each leaked fd. **Lesson:** RAII types make leaks structurally impossible on every early-return and exception path.

### Drill 3: the deadlock you inspect with gdb
Call `store_.list()` from inside `FileStore::commit()` while holding `unique_lock`. A `shared_mutex` isn't recursive, so the first upload hangs. Attach with `gdb -p`, then run `thread apply all bt`. The worker is parked in `pthread_rwlock_rdlock` under `commit`. **Lesson:** never call back into a locked component. The design holds at most one lock at a time.

### Drill 4: the fd-reuse race (why unregister precedes close)
In `Server::handle_connection`, swap the order so that `conn.close()` runs before the `conns_.erase(fd)` block. Under load, with a `stop()` in the middle, `shutdown()` can hit a *new* connection that was given the same fd number. The symptom is random disconnects of healthy clients during shutdown. **Lesson:** fd numbers are recycled right away, so a registry keyed by fd has to be updated before the fd is released.

### Drill 5: SIGINT ignored under a background job (a real bug found while building this)
Remove the two `std::signal(..., SIG_DFL)` lines from `server_main.cpp` and run `tests/integration_test.sh`. The final step fails with "server still running 5s after SIGINT". Non-interactive shells start `cmd &` with SIGINT set to `SIG_IGN`, and POSIX discards an ignored signal instead of leaving it pending, so `sigwait()` never sees it. Check with `grep SigIgn /proc/<pid>/status` on Linux.

### Drill 6: the RST that eats an error reply
In `Server::reject_and_close`, delete the drain loop so the function just returns. Run `build/unit_tests server_rejects_oversized_upload` repeatedly. Sometimes the client reports `recv: Connection reset by peer` instead of `too_large`. That happens because closing a socket with unread data sends an RST. **Lesson:** the lingering-close pattern exists for this reason.

## Valgrind recipes (Linux)

```sh
make valgrind                          # memcheck over all unit + in-process E2E tests
valgrind --leak-check=full --track-fds=yes build/tcpsync-server --port 9000   # then run clients
make helgrind                          # lock-order and race analysis (some std:: false positives)
valgrind --tool=drd build/unit_tests   # alternative race detector
```

## Useful runtime observation

```sh
TCPSYNC_VERBOSE=1 build/unit_tests server_          # tests with server logs
lsof -p $(pgrep tcpsync-server) | grep -c TCP       # open connections
printf 'LIST\nQUIT\n' | nc 127.0.0.1 9000           # speak the protocol by hand
```
