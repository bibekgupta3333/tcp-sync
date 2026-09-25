#pragma once

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace tcpsync {

// Socket-level failures: connect/bind errors, timeouts, peer resets, unexpected EOF.
class NetError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// Owns a file descriptor and closes it exactly once. Move-only, like std::unique_ptr.
class UniqueFd {
public:
    UniqueFd() noexcept = default;
    explicit UniqueFd(int fd) noexcept : fd_(fd) {}
    ~UniqueFd() { close(); }

    UniqueFd(const UniqueFd&) = delete;
    UniqueFd& operator=(const UniqueFd&) = delete;
    UniqueFd(UniqueFd&& other) noexcept : fd_(other.release()) {}
    UniqueFd& operator=(UniqueFd&& other) noexcept {
        if (this != &other) {
            close();
            fd_ = other.release();
        }
        return *this;
    }

    int get() const noexcept { return fd_; }
    bool valid() const noexcept { return fd_ >= 0; }
    int release() noexcept {
        int fd = fd_;
        fd_ = -1;
        return fd;
    }
    void close() noexcept;

private:
    int fd_ = -1;
};

// "what: <strerror(errno)>", using the thread-safe std::error_code message.
std::string errno_message(const std::string& what);

UniqueFd listen_tcp(const std::string& host, uint16_t port, int backlog = 128);
UniqueFd connect_tcp(const std::string& host, uint16_t port);
uint16_t local_port(int fd);
std::string peer_address(int fd);

// Send/receive timeouts (so a stalled peer cannot pin a thread forever), TCP_NODELAY,
// and SO_NOSIGPIPE where the platform has it.
void configure_stream_socket(int fd, int timeout_sec);
void set_receive_timeout(int fd, int timeout_sec);

// True if a read on fd would not block (data, EOF or error pending).
bool readable_now(int fd);

// Loops until every byte is written; send() may accept only part of a buffer.
void send_all(int fd, const void* data, size_t len);
inline void send_all(int fd, const std::string& s) { send_all(fd, s.data(), s.size()); }

// Buffered reader for one connection. All reads go through it, so payload bytes that
// arrive in the same segment as a header line are never lost.
class Reader {
public:
    explicit Reader(int fd, size_t buffer_size = 64 * 1024) : fd_(fd), buf_(buffer_size) {}

    // Reads one '\n'-terminated line (terminator and trailing '\r' stripped).
    // Returns false on a clean EOF before any byte; throws NetError on a line longer
    // than max_len, EOF mid-line, timeout, or socket error.
    bool read_line(std::string& line, size_t max_len);

    // Reads at least 1 and at most len bytes. Returns 0 only on EOF.
    size_t read_some(void* dst, size_t len);

    // Reads exactly len bytes or throws NetError.
    void read_exact(void* dst, size_t len);

private:
    size_t recv_into(void* dst, size_t len);

    int fd_;
    std::vector<char> buf_;
    size_t pos_ = 0;
    size_t end_ = 0;
};

}  // namespace tcpsync
