# Data model

## Is a database required? No.

The service stores **files**, and the metadata per file is tiny: a size, a hash and a version. The filesystem is already the database here:

- `rename()` gives atomic replacement, which is the "transaction" we need.
- A directory listing plus hashing rebuilds the index at startup in O(total bytes), so the index never has to be persisted separately. That means it can't drift from what's on disk.
- An in-memory `std::unordered_map` answers every lookup in O(1) under a `std::shared_mutex`.

Adding SQLite would buy multi-field queries, history and faster restarts for very large stores, at the cost of a dependency and a second source of truth to keep consistent with the files. That's worth it only if you add version history, users/ACLs or directory trees.

## Logical model

```mermaid
erDiagram
    FILE_STORE ||--o{ FILE_ENTRY : "index_ (unordered_map)"
    FILE_STORE ||--o{ TEMP_UPLOAD : "in-flight"
    FILE_ENTRY ||--|| STORED_FILE : "root/<name>"
    TEMP_UPLOAD ||--|| TEMP_FILE : "root/.tmp.<pid>.<n>"
    CLIENT_DIR ||--o{ SYNC_STATE_ROW : ".tcpsync-state"
    SYNC_STATE_ROW }o--o| FILE_ENTRY : "same name (compared by hash)"

    FILE_STORE {
        string root
        shared_mutex mu_
        uint64 next_version_
        atomic_uint64 temp_counter_
    }
    FILE_ENTRY {
        string name PK "is_valid_name, max 255"
        uint64 size
        uint64 hash "FNV-1a 64"
        uint64 version "store-wide commit sequence"
    }
    STORED_FILE {
        bytes content "immutable once committed"
    }
    TEMP_UPLOAD {
        string name "target name"
        string temp_path
        int fd
        uint64 bytes_written
        Fnv1a64 hasher "running hash"
        bool done "committed or moved-from"
    }
    TEMP_FILE {
        bytes partial_content
    }
    CLIENT_DIR {
        path dir
    }
    SYNC_STATE_ROW {
        string name PK
        uint64 base_hash "hash at last successful sync"
    }
```

## Physical layout

```
server: <root>/
├── notes.txt                 committed file (immutable; replaced only via rename)
├── report.pdf
└── .tmp.41822.17             upload in progress (deleted on abort and at startup)

client: <sync dir>/
├── notes.txt
├── notes.txt.conflict        local copy set aside in a conflict (never synced)
├── .tcpsync-state            "<hash16> <name>" per line, rewritten atomically
└── .tcpsync-part-notes.txt   download in progress (renamed into place when verified)
```

## In-memory structures (STL choices)

| Structure | Type | Why |
|---|---|---|
| File index | `std::unordered_map<std::string, FileMeta>` | O(1) lookup by name; `LIST` copies it and sorts outside the lock |
| Active connections | `std::unordered_set<int>` | O(1) insert/erase of fds; iterated only on shutdown |
| Work queue | `std::queue<std::function<void()>>` | FIFO fairness between accepted connections |
| Workers | `std::vector<std::thread>` | Fixed size, joined on shutdown |
| Sync maps (client) | `std::map<std::string, uint64_t>` | Ordered, so sync output and the state file are deterministic |
