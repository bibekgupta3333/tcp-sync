#include "tcpsync/server.hpp"

#include "tcpsync/hash.hpp"
#include "tcpsync/log.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <memory>
#include <optional>
#include <system_error>
#include <thread>
#include <vector>

#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace tcpsync {

namespace {

// After rejecting a request we read and discard at most this much of the client's
// payload before closing (see reject_and_close).
constexpr uint64_t kMaxDrainBytes = 16 * 1024 * 1024;

}  // namespace

Server::Server(ServerConfig config)
    : config_(std::move(config)),
      store_(config_.root),
      listener_(listen_tcp(config_.host, config_.port)),
      pool_(config_.workers, config_.max_queue) {
    int fds[2];
    if (::pipe(fds) != 0) throw std::system_error(errno, std::generic_category(), "pipe");
    wake_read_ = UniqueFd(fds[0]);
    wake_write_ = UniqueFd(fds[1]);
    port_ = local_port(listener_.get());
}

Server::~Server() {
    stop();
    pool_.shutdown();
}

void Server::stop() noexcept {
    stopping_.store(true);
    const char byte = 1;
    // write() is async-signal-safe, so stop() could even be called from a signal handler.
    [[maybe_unused]] ssize_t n = ::write(wake_write_.get(), &byte, 1);
}

void Server::run() {
    log_info("listening on ", config_.host, ":", port_, " root=", config_.root, " workers=", pool_.worker_count(),
             " queue=", config_.max_queue);
    while (!stopping_.load()) {
        pollfd fds[2] = {{listener_.get(), POLLIN, 0}, {wake_read_.get(), POLLIN, 0}};
        if (::poll(fds, 2, -1) < 0) {
            if (errno == EINTR) continue;
            throw std::system_error(errno, std::generic_category(), "poll");
        }
        if (fds[1].revents != 0) break;
        if ((fds[0].revents & POLLIN) == 0) continue;

        UniqueFd conn(::accept(listener_.get(), nullptr, nullptr));
        if (!conn.valid()) {
            if (errno == EMFILE || errno == ENFILE) {
                // Out of file descriptors: back off instead of spinning on poll().
                log_error(errno_message("accept"));
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
            continue;  // EINTR, ECONNABORTED, EAGAIN: nothing to do
        }
        configure_stream_socket(conn.get(), config_.idle_timeout_sec);
        const std::string peer = peer_address(conn.get());

        // std::function must be copyable, but UniqueFd is move-only, so the fd rides in
        // a shared_ptr. The worker moves it out, taking sole ownership.
        auto shared = std::make_shared<UniqueFd>(std::move(conn));
        const bool queued = pool_.try_submit([this, shared, peer] { handle_connection(std::move(*shared), peer); });
        if (!queued) {
            try {
                send_all(shared->get(), err_line("busy", "server at capacity, retry later"));
            } catch (const NetError&) {
            }
            log_info(peer, " rejected: all workers busy and queue full");
        }
    }
    shutdown_connections();
    pool_.shutdown();
    log_info("server stopped");
}

void Server::shutdown_connections() {
    std::lock_guard<std::mutex> lock(conns_mu_);
    // shutdown() (unlike close()) wakes a thread blocked in recv() on this fd; that
    // worker then sees EOF and returns. The worker still owns and closes the fd.
    for (int fd : conns_) ::shutdown(fd, SHUT_RDWR);
    if (!conns_.empty()) log_info("aborted ", conns_.size(), " open connection(s)");
}

void Server::handle_connection(UniqueFd conn, const std::string& peer) {
    const int fd = conn.get();
    {
        std::lock_guard<std::mutex> lock(conns_mu_);
        // Checked under the same mutex shutdown_connections() holds: either we register
        // before it runs (and it unblocks us), or we see stopping_ and leave.
        if (stopping_.load()) return;
        conns_.insert(fd);
    }
    log_info(peer, " connected");
    try {
        serve(fd, peer);
    } catch (const std::exception& e) {
        log_info(peer, " connection error: ", e.what());
    }
    {
        // Unregister BEFORE close(). Otherwise the fd number could be reused by a new
        // connection while still in conns_, and stop() would shut down the wrong socket.
        std::lock_guard<std::mutex> lock(conns_mu_);
        conns_.erase(fd);
    }
    conn.close();
    log_info(peer, " disconnected");
}

void Server::serve(int fd, const std::string& peer) {
    Reader reader(fd);
    std::string line;
    while (!stopping_.load() && reader.read_line(line, kMaxLineLength)) {
        std::string error;
        std::optional<Request> request = parse_request(line, error);
        if (!request) {
            // We can't know whether a payload follows, so the stream can't be trusted.
            reject_and_close(fd, reader, err_line("bad_request", error));
            return;
        }
        if (!dispatch(fd, reader, *request, peer)) return;
    }
}

bool Server::dispatch(int fd, Reader& reader, const Request& request, const std::string& peer) {
    switch (request.command) {
        case Command::List: do_list(fd); return true;
        case Command::Download: do_download(fd, request.name, peer); return true;
        case Command::Delete: do_delete(fd, request.name, peer); return true;
        case Command::Upload: return do_upload(fd, reader, request.name, request.size, peer);
        case Command::Quit: send_all(fd, ok_line({"bye"})); return false;
    }
    return false;
}

void Server::do_list(int fd) {
    const std::vector<FileEntry> entries = store_.list();  // snapshot; lock already released
    std::string out = ok_line({std::to_string(entries.size())});
    for (const FileEntry& e : entries) {
        out += e.name + " " + std::to_string(e.meta.size) + " " + to_hex(e.meta.hash) + " " +
               std::to_string(e.meta.version) + "\n";
    }
    send_all(fd, out);
}

bool Server::do_upload(int fd, Reader& reader, const std::string& name, uint64_t size, const std::string& peer) {
    if (size > config_.max_file_size) {
        reject_and_close(fd, reader,
                         err_line("too_large", std::to_string(size) + " bytes exceeds the limit of " +
                                                   std::to_string(config_.max_file_size)));
        return false;
    }
    try {
        FileStore::Upload upload = store_.begin_upload(name);
        std::vector<char> buf(kChunkSize);
        uint64_t remaining = size;
        while (remaining > 0) {
            const size_t want = static_cast<size_t>(std::min<uint64_t>(remaining, buf.size()));
            const size_t n = reader.read_some(buf.data(), want);
            // Throwing here destroys `upload`, whose destructor deletes the temp file.
            if (n == 0) throw NetError("client disconnected with " + std::to_string(remaining) + " bytes left");
            upload.write(buf.data(), n);
            remaining -= n;
        }
        const FileMeta meta = upload.commit();
        send_all(fd, ok_line({std::to_string(meta.version), to_hex(meta.hash)}));
        log_info(peer, " UPLOAD ", name, " ", size, " bytes -> v", meta.version);
        return true;
    } catch (const std::system_error& e) {  // disk full, permissions, rename failure...
        log_error(peer, " UPLOAD ", name, " failed: ", e.what());
        reject_and_close(fd, reader, err_line("io_error", e.what()));
        return false;
    }
}

void Server::do_download(int fd, const std::string& name, const std::string& peer) {
    std::optional<OpenedFile> file = store_.open_for_read(name);
    if (!file) {
        send_all(fd, err_line("not_found", name));
        return;
    }
    send_all(fd, ok_line({std::to_string(file->meta.size), to_hex(file->meta.hash)}));
    std::vector<char> buf(kChunkSize);
    uint64_t remaining = file->meta.size;
    while (remaining > 0) {
        const size_t want = static_cast<size_t>(std::min<uint64_t>(remaining, buf.size()));
        const ssize_t n = ::read(file->fd.get(), buf.data(), want);
        if (n < 0 && errno == EINTR) continue;
        // Can't happen for an immutable file; if it does, the stream is broken and the
        // exception closes the connection.
        if (n <= 0) throw std::runtime_error("short read on stored file " + name);
        send_all(fd, buf.data(), static_cast<size_t>(n));
        remaining -= static_cast<uint64_t>(n);
    }
    log_info(peer, " DOWNLOAD ", name, " ", file->meta.size, " bytes (v", file->meta.version, ")");
}

void Server::do_delete(int fd, const std::string& name, const std::string& peer) {
    if (store_.remove(name)) {
        send_all(fd, ok_line());
        log_info(peer, " DELETE ", name);
    } else {
        send_all(fd, err_line("not_found", name));
    }
}

void Server::reject_and_close(int fd, Reader& reader, const std::string& response) {
    send_all(fd, response);
    ::shutdown(fd, SHUT_WR);
    // "Lingering close": if we close() while unread client bytes are still in our receive
    // buffer, the kernel sends a TCP RST, and the client may lose our ERR line before
    // reading it. So read and discard the client's data until it hangs up, bounded by
    // bytes and a short timeout.
    set_receive_timeout(fd, 1);
    std::vector<char> sink(kChunkSize);
    uint64_t drained = 0;
    try {
        while (drained < kMaxDrainBytes) {
            const size_t n = reader.read_some(sink.data(), sink.size());
            if (n == 0) break;
            drained += n;
        }
    } catch (const NetError&) {
    }
}

}  // namespace tcpsync
