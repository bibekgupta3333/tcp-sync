#pragma once

// Thread-safe file store: a flat directory of files plus an in-memory metadata index.
//
// Concurrency design (see docs/ARCHITECTURE.md):
//   * Stored files are immutable. An upload streams into a private temp file with NO lock
//     held, then commit() takes the exclusive lock only for rename() + index update, so a
//     new version replaces the old one atomically.
//   * open_for_read() takes the shared lock just long enough to look up the metadata and
//     open() the file. The fd keeps referring to that exact version even if a later commit
//     renames a new file over it, so the download streams with no lock held.
//   * Slow work (network and disk I/O) never happens while holding mu_.

#include "tcpsync/hash.hpp"
#include "tcpsync/net.hpp"

#include <atomic>
#include <cstdint>
#include <optional>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace tcpsync {

struct FileMeta {
    uint64_t size = 0;
    uint64_t hash = 0;
    uint64_t version = 0;  // store-wide monotonically increasing commit sequence number
};

struct FileEntry {
    std::string name;
    FileMeta meta;
};

struct OpenedFile {
    UniqueFd fd;
    FileMeta meta;  // describes exactly the bytes readable from fd
};

class FileStore {
public:
    class Upload;

    // Creates `root` if needed, deletes stale temp files and rebuilds the index from disk.
    explicit FileStore(std::string root);

    FileStore(const FileStore&) = delete;
    FileStore& operator=(const FileStore&) = delete;

    std::vector<FileEntry> list() const;  // snapshot, sorted by name
    std::optional<FileMeta> stat(const std::string& name) const;

    // Throws std::invalid_argument for bad names, std::system_error for I/O failures.
    Upload begin_upload(const std::string& name);
    std::optional<OpenedFile> open_for_read(const std::string& name) const;
    bool remove(const std::string& name);

    const std::string& root() const noexcept { return root_; }

    // Temp files start with '.', which is_valid_name() rejects, so they can never
    // collide with, or be listed as, a real file.
    static constexpr const char* kTempPrefix = ".tmp.";

private:
    FileMeta commit(const std::string& name, const std::string& temp_path, uint64_t size, uint64_t hash);
    std::string path_for(const std::string& name) const;
    void rebuild_index();

    const std::string root_;
    mutable std::shared_mutex mu_;  // guards index_ and next_version_
    std::unordered_map<std::string, FileMeta> index_;
    uint64_t next_version_ = 1;
    std::atomic<uint64_t> temp_counter_{0};
};

// An in-progress upload. If it is destroyed without commit() (client disconnected,
// disk full, exception...), the temp file is deleted: RAII cleanup on every path.
class FileStore::Upload {
public:
    Upload(Upload&& other) noexcept;
    Upload& operator=(Upload&&) = delete;
    Upload(const Upload&) = delete;
    Upload& operator=(const Upload&) = delete;
    ~Upload();

    void write(const void* data, size_t len);
    FileMeta commit();
    uint64_t bytes_written() const noexcept { return size_; }

private:
    friend class FileStore;
    Upload(FileStore& store, std::string name, std::string temp_path, UniqueFd fd);

    FileStore* store_;
    std::string name_;
    std::string temp_path_;
    UniqueFd fd_;
    uint64_t size_ = 0;
    Fnv1a64 hasher_;
    bool done_ = false;
};

}  // namespace tcpsync
