#pragma once

#include "tcpsync/file_store.hpp"
#include "tcpsync/net.hpp"
#include "tcpsync/protocol.hpp"
#include "tcpsync/thread_pool.hpp"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_set>

namespace tcpsync {

struct ServerConfig {
    std::string host = "0.0.0.0";
    uint16_t port = 9000;  // 0 = let the OS pick; read it back with Server::port()
    std::string root = "./storage";
    size_t workers = 8;
    size_t max_queue = 64;  // connections waiting for a worker before "ERR busy"
    int idle_timeout_sec = 30;
    uint64_t max_file_size = 1ULL << 30;
};

// Threading model: run() is the only thread that calls accept(). Each accepted connection
// becomes one task on the ThreadPool, and a worker serves that connection's requests
// until it disconnects. All shared state lives in FileStore (its own locking) and conns_.
class Server {
public:
    // Binds and listens right away, so port() is valid before run().
    explicit Server(ServerConfig config);
    ~Server();

    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    uint16_t port() const noexcept { return port_; }

    // Accept loop. Blocks until stop(), then aborts open connections and joins workers.
    void run();

    // Thread-safe and idempotent; only write()s to a pipe.
    void stop() noexcept;

private:
    void handle_connection(UniqueFd conn, const std::string& peer);
    void serve(int fd, const std::string& peer);
    bool dispatch(int fd, Reader& reader, const Request& request, const std::string& peer);
    bool do_upload(int fd, Reader& reader, const std::string& name, uint64_t size, const std::string& peer);
    void do_download(int fd, const std::string& name, const std::string& peer);
    void do_list(int fd);
    void do_delete(int fd, const std::string& name, const std::string& peer);
    void reject_and_close(int fd, Reader& reader, const std::string& response);
    void shutdown_connections();

    const ServerConfig config_;
    FileStore store_;
    UniqueFd listener_;
    UniqueFd wake_read_;   // self-pipe: stop() writes a byte here to wake poll()
    UniqueFd wake_write_;
    uint16_t port_ = 0;
    std::atomic<bool> stopping_{false};

    std::mutex conns_mu_;              // guards conns_
    std::unordered_set<int> conns_;    // fds currently served, so stop() can unblock them

    // Declared last so it is destroyed FIRST: worker threads use every member above.
    ThreadPool pool_;
};

}  // namespace tcpsync
