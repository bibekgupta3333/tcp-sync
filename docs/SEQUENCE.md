# Sequence diagrams

## 1. Connection setup and UPLOAD

```mermaid
sequenceDiagram
    autonumber
    participant C as Client
    participant A as Accept thread
    participant Q as ThreadPool queue
    participant W as Worker
    participant S as FileStore
    participant D as Disk

    C->>A: TCP connect
    A->>A: accept(), set SO_RCVTIMEO/SO_SNDTIMEO, TCP_NODELAY
    A->>Q: try_submit(handle_connection)
    alt queue full
        A-->>C: ERR busy server at capacity
    else queued
        Q->>W: pop task (condition_variable)
        W->>W: lock conns_mu_, check stopping_, register fd
    end
    C->>W: UPLOAD report.pdf 1048576\n
    W->>W: parse_request + is_valid_name
    W->>S: begin_upload("report.pdf")
    S->>D: open(".tmp.<pid>.<n>", O_EXCL)
    loop until 1048576 bytes (no lock held)
        C->>W: payload chunk
        W->>D: write(temp) + hasher.update
    end
    W->>S: upload.commit()
    activate S
    Note over S: unique_lock(mu_)
    S->>D: rename(temp, "report.pdf")  (atomic)
    S->>S: index_[name] = {size, hash, ++version}
    deactivate S
    W-->>C: OK 42 9f3c...e1
    C->>C: compare server hash with own running hash
```

## 2. DOWNLOAD: snapshot read while a writer commits

```mermaid
sequenceDiagram
    autonumber
    participant R as Reader worker
    participant S as FileStore
    participant W as Writer worker
    participant D as Disk

    R->>S: open_for_read("a.bin")
    activate S
    Note over S: shared_lock(mu_)
    S->>D: open("a.bin") -> fd (inode v7)
    S-->>R: {fd, meta v7}
    deactivate S
    R-->>R: send "OK <size v7> <hash v7>"
    par streaming without a lock
        loop chunks
            R->>D: read(fd)  (still inode v7)
            R-->>R: send chunk to client
        end
    and concurrent commit
        W->>S: commit()
        Note over S: unique_lock(mu_)
        S->>D: rename(temp, "a.bin") -> name now points to inode v8
    end
    Note over R,D: The reader's fd still refers to v7, so the client receives<br/>exactly the bytes its header described. The v7 inode is freed after close(fd).
```

## 3. Same-name upload race

```mermaid
sequenceDiagram
    autonumber
    participant W1 as Worker 1
    participant W2 as Worker 2
    participant S as FileStore
    participant D as Disk

    par both stream in parallel, no lock
        W1->>D: write .tmp.1 (version A bytes)
    and
        W2->>D: write .tmp.2 (version B bytes)
    end
    W2->>S: commit (unique_lock)
    S->>D: rename(.tmp.2, f)  -> v10
    W1->>S: commit (waits for lock, then unique_lock)
    S->>D: rename(.tmp.1, f)  -> v11
    Note over S,D: Last commit wins. f is always entirely A or entirely B, never mixed.
```

## 4. Rejected upload (too_large) with lingering close

```mermaid
sequenceDiagram
    autonumber
    participant C as Client
    participant W as Worker

    C->>W: UPLOAD huge.iso 5368709120\n
    C->>W: first chunk
    W-->>C: ERR too_large ... exceeds the limit
    W->>W: shutdown(SHUT_WR), SO_RCVTIMEO = 1s
    C->>C: poll(): readable before next chunk, so stop sending
    C->>C: read ERR -> throw ServerError(too_large)
    loop drain (<= 16 MiB)
        W->>W: read and discard client bytes
    end
    C->>W: close()  (FIN)
    W->>W: read returns 0, then close(): no RST, so the ERR line isn't lost
```

## 5. Two-way sync with a conflict

```mermaid
sequenceDiagram
    autonumber
    participant A as Client A (dir A)
    participant Srv as Server
    participant B as Client B (dir B)

    Note over A,B: Both last synced notes.txt with hash h0 (B = h0 in each .tcpsync-state)
    A->>A: edit: notes.txt = hA
    B->>B: edit: notes.txt = hB
    A->>Srv: LIST
    Srv-->>A: notes.txt h0
    Note over A: L=hA, R=h0, B=h0 -> only local changed
    A->>Srv: UPLOAD notes.txt
    Srv-->>A: OK v9 hA
    A->>A: state: notes.txt = hA
    B->>Srv: LIST
    Srv-->>B: notes.txt hA
    Note over B: L=hB, R=hA, B=h0 -> both changed: CONFLICT
    B->>B: rename notes.txt -> notes.txt.conflict
    B->>Srv: DOWNLOAD notes.txt
    Srv-->>B: OK size hA + bytes
    B->>B: verify hash, rename .tcpsync-part-notes.txt -> notes.txt
    B->>B: state: notes.txt = hA
```

## 6. Graceful shutdown

```mermaid
sequenceDiagram
    autonumber
    participant OS as Kernel
    participant G as Signal thread
    participant A as Accept thread
    participant W as Workers
    participant P as ThreadPool

    OS->>G: SIGINT (blocked everywhere, delivered to sigwait)
    G->>A: stop(): stopping_=true, write(self-pipe)
    A->>A: poll() wakes, leave accept loop
    A->>W: shutdown(fd, SHUT_RDWR) for every fd in conns_ (under conns_mu_)
    W->>W: recv() returns 0, loop exits, Upload RAII deletes any temp file
    W->>W: unregister fd (under conns_mu_), then close(fd)
    A->>P: pool_.shutdown(): stopping_, notify_all
    P->>W: queued tasks see stopping_ and return, workers exit
    P->>A: join all workers
    A->>G: main: pthread_kill(signal thread) is a no-op here, join
    Note over A: ~Server: pool_ destroyed first (declared last)
```
