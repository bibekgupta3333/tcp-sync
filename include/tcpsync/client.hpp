#pragma once

#include "tcpsync/net.hpp"
#include "tcpsync/protocol.hpp"

#include <cstdint>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace tcpsync {

// The server answered "ERR <code> <message>".
class ServerError : public std::runtime_error {
public:
    ServerError(std::string code, const std::string& message)
        : std::runtime_error(code + ": " + message), code_(std::move(code)) {}
    const std::string& code() const noexcept { return code_; }

private:
    std::string code_;
};

struct RemoteFile {
    std::string name;
    uint64_t size = 0;
    uint64_t hash = 0;
    uint64_t version = 0;
};

struct TransferResult {
    uint64_t size = 0;
    uint64_t hash = 0;     // verified end to end: client and server hashes must agree
    uint64_t version = 0;  // uploads only
};

// One persistent connection. Not thread-safe: use one Client per thread.
// After a failed upload/download the connection is closed and later calls throw NetError.
class Client {
public:
    Client(const std::string& host, uint16_t port, int timeout_sec = 30);

    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;

    std::vector<RemoteFile> list();
    TransferResult upload_file(const std::string& local_path, const std::string& remote_name);
    TransferResult upload_data(const std::string& remote_name, const std::string& data);
    // Writes to a hidden temp file next to local_path, then renames it into place.
    TransferResult download_file(const std::string& remote_name, const std::string& local_path);
    std::string download_data(const std::string& remote_name);
    void remove(const std::string& remote_name);
    void quit();

private:
    using ChunkSource = std::function<size_t(char* buf, size_t cap)>;
    using ChunkSink = std::function<void(const char* data, size_t len)>;

    TransferResult upload(const std::string& remote_name, uint64_t size, const ChunkSource& source);
    TransferResult download(const std::string& remote_name, const ChunkSink& sink);
    void send_text(const std::string& text);
    Response expect_ok();
    std::optional<ServerError> read_rejection() noexcept;

    UniqueFd sock_;
    Reader reader_;
};

struct SyncReport {
    std::vector<std::string> uploaded;
    std::vector<std::string> downloaded;
    std::vector<std::string> conflicts;  // local copy kept as "<name>.conflict"
    std::vector<std::string> deleted_local;
    std::vector<std::string> deleted_remote;
    size_t unchanged = 0;
};

// Two-way sync of a flat directory using a 3-way compare (local, remote, and the state
// recorded at the last sync in <dir>/.tcpsync-state). See docs/SEQUENCE.md.
SyncReport sync_directory(Client& client, const std::string& dir);

}  // namespace tcpsync
