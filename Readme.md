# tcp-sync

A multithreaded TCP file synchronization service in C++17. Many clients upload, download and two-way sync files at the same time. It is built on POSIX sockets, `std::thread`, mutexes/`shared_mutex`/condition variables and STL containers, with no third-party dependencies.

- **Concurrent and race-free.** Uploads stream to private temp files with no lock held, then commit atomically (`rename` under an exclusive lock). Downloads read a consistent snapshot through an open fd while writers keep committing.
- **Bounded.** A fixed thread pool with a bounded queue answers `ERR busy` under overload, and per-connection timeouts stop stalled clients from pinning workers.
- **Verified.** Every transfer is checked end to end with an FNV-1a 64 hash.
- **Robust.**
  - Path-traversal guard
  - Lingering close, so error replies survive
  - SIGPIPE-safe
  - Graceful SIGINT/SIGTERM shutdown via `sigwait`
  - Crash cleanup of temp files at startup
- **Sync.** 3-way merge (local / remote / last-synced state) with conflict copies and deletion propagation.

## Build and run

```sh
make                      # build/tcpsync-server, build/tcpsync-client, build/unit_tests
make test                 # unit tests + multi-process integration test
make docker-check         # Linux container: test + ASan + TSan + Valgrind

build/tcpsync-server --port 9000 --root ./storage --workers 8

build/tcpsync-client upload ./report.pdf
build/tcpsync-client list
build/tcpsync-client download report.pdf ./copy.pdf
build/tcpsync-client delete report.pdf
build/tcpsync-client sync ~/shared          # run from several machines/dirs

printf 'LIST\nQUIT\n' | nc 127.0.0.1 9000    # the protocol is plain text
```

Server options: `--host --port --root --workers --queue --timeout --max-size --quiet`. Client options: `--host --port --timeout`.

## Docs

| Doc | Contents |
|---|---|
| [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) | Components, protocol, threading model, locking design, race-condition table, shutdown, sync rules |
| [docs/DATA_MODEL.md](docs/DATA_MODEL.md) | Why there's no database; ER diagram of the index, on-disk layout, sync state |
| [docs/SEQUENCE.md](docs/SEQUENCE.md) | Upload, snapshot download, same-name race, rejection, sync conflict, shutdown |
| [docs/WBS.md](docs/WBS.md) | Work breakdown, packages, critical path, milestones |
| [docs/CPP_VS_PYTHON.md](docs/CPP_VS_PYTHON.md) | C++ cheatsheet mapped to Python equivalents |
| [docs/DEBUGGING.md](docs/DEBUGGING.md) | gdb/lldb mapping, Valgrind, sanitizers, six hands-on bug drills |

## Layout

```
include/tcpsync/   net, protocol, hash, log, thread_pool, file_store, server, client headers
src/               implementations + server_main.cpp / client_main.cpp
tests/             unit_tests.cpp (incl. in-process E2E), integration_test.sh (real processes)
docs/              design docs (Mermaid diagrams render on GitHub)
```
