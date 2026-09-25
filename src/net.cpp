#include "tcpsync/net.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <memory>
#include <system_error>

#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

namespace tcpsync {

namespace {

// Linux suppresses SIGPIPE per call; macOS/BSD do it per socket (SO_NOSIGPIPE below).
#ifdef MSG_NOSIGNAL
constexpr int kSendFlags = MSG_NOSIGNAL;
#else
constexpr int kSendFlags = 0;
#endif

[[noreturn]] void throw_errno(const std::string& what) { throw NetError(errno_message(what)); }

using AddrInfoPtr = std::unique_ptr<addrinfo, decltype(&::freeaddrinfo)>;

AddrInfoPtr resolve(const std::string& host, uint16_t port, bool passive) {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = passive ? AI_PASSIVE : 0;
    addrinfo* res = nullptr;
    const std::string service = std::to_string(port);
    const char* node = host.empty() ? nullptr : host.c_str();
    if (int rc = ::getaddrinfo(node, service.c_str(), &hints, &res); rc != 0) {
        throw NetError("resolve " + host + ": " + ::gai_strerror(rc));
    }
    return AddrInfoPtr(res, &::freeaddrinfo);
}

}  // namespace

void UniqueFd::close() noexcept {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

std::string errno_message(const std::string& what) {
    return what + ": " + std::error_code(errno, std::generic_category()).message();
}

UniqueFd listen_tcp(const std::string& host, uint16_t port, int backlog) {
    AddrInfoPtr addrs = resolve(host, port, /*passive=*/true);
    std::string last_error = "no usable address for " + host;
    for (addrinfo* ai = addrs.get(); ai != nullptr; ai = ai->ai_next) {
        UniqueFd fd(::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol));
        if (!fd.valid()) {
            last_error = errno_message("socket");
            continue;
        }
        int one = 1;
        ::setsockopt(fd.get(), SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
        if (::bind(fd.get(), ai->ai_addr, ai->ai_addrlen) != 0) {
            last_error = errno_message("bind");
            continue;
        }
        if (::listen(fd.get(), backlog) != 0) {
            last_error = errno_message("listen");
            continue;
        }
        return fd;
    }
    throw NetError(last_error);
}

UniqueFd connect_tcp(const std::string& host, uint16_t port) {
    AddrInfoPtr addrs = resolve(host, port, /*passive=*/false);
    std::string last_error = "no usable address for " + host;
    for (addrinfo* ai = addrs.get(); ai != nullptr; ai = ai->ai_next) {
        UniqueFd fd(::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol));
        if (!fd.valid()) {
            last_error = errno_message("socket");
            continue;
        }
        int rc;
        do {
            rc = ::connect(fd.get(), ai->ai_addr, ai->ai_addrlen);
        } while (rc != 0 && errno == EINTR);
        if (rc != 0) {
            last_error = errno_message("connect " + host + ":" + std::to_string(port));
            continue;
        }
        return fd;
    }
    throw NetError(last_error);
}

uint16_t local_port(int fd) {
    sockaddr_storage addr{};
    socklen_t len = sizeof addr;
    if (::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len) != 0) throw_errno("getsockname");
    if (addr.ss_family == AF_INET) return ntohs(reinterpret_cast<sockaddr_in*>(&addr)->sin_port);
    if (addr.ss_family == AF_INET6) return ntohs(reinterpret_cast<sockaddr_in6*>(&addr)->sin6_port);
    throw NetError("getsockname: unexpected address family");
}

std::string peer_address(int fd) {
    sockaddr_storage addr{};
    socklen_t len = sizeof addr;
    if (::getpeername(fd, reinterpret_cast<sockaddr*>(&addr), &len) != 0) return "?";
    char host[NI_MAXHOST];
    char serv[NI_MAXSERV];
    if (::getnameinfo(reinterpret_cast<sockaddr*>(&addr), len, host, sizeof host, serv, sizeof serv,
                      NI_NUMERICHOST | NI_NUMERICSERV) != 0) {
        return "?";
    }
    return std::string(host) + ":" + serv;
}

void set_receive_timeout(int fd, int timeout_sec) {
    timeval tv{};
    tv.tv_sec = timeout_sec;
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
}

void configure_stream_socket(int fd, int timeout_sec) {
    timeval tv{};
    tv.tv_sec = timeout_sec;
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    int one = 1;
    // The protocol is request/response with small header lines; without this, Nagle's
    // algorithm plus delayed ACKs can stall each exchange by ~40ms.
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
#ifdef SO_NOSIGPIPE
    ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
}

bool readable_now(int fd) {
    pollfd p{};
    p.fd = fd;
    p.events = POLLIN;
    return ::poll(&p, 1, 0) > 0 && (p.revents & (POLLIN | POLLHUP | POLLERR)) != 0;
}

void send_all(int fd, const void* data, size_t len) {
    const char* p = static_cast<const char*>(data);
    while (len > 0) {
        ssize_t n = ::send(fd, p, len, kSendFlags);
        if (n < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) throw NetError("send timed out");
            throw_errno("send");
        }
        p += n;
        len -= static_cast<size_t>(n);
    }
}

size_t Reader::recv_into(void* dst, size_t len) {
    for (;;) {
        ssize_t n = ::recv(fd_, dst, len, 0);
        if (n >= 0) return static_cast<size_t>(n);
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) throw NetError("receive timed out");
        throw_errno("recv");
    }
}

bool Reader::read_line(std::string& line, size_t max_len) {
    line.clear();
    for (;;) {
        const char* begin = buf_.data() + pos_;
        const char* end = buf_.data() + end_;
        const char* newline = std::find(begin, end, '\n');
        const size_t take = static_cast<size_t>(newline - begin);
        if (line.size() + take > max_len) throw NetError("line exceeds " + std::to_string(max_len) + " bytes");
        line.append(begin, take);
        if (newline != end) {
            pos_ += take + 1;
            if (!line.empty() && line.back() == '\r') line.pop_back();
            return true;
        }
        pos_ = end_ = 0;
        const size_t n = recv_into(buf_.data(), buf_.size());
        if (n == 0) {
            if (line.empty()) return false;
            throw NetError("connection closed in the middle of a line");
        }
        end_ = n;
    }
}

size_t Reader::read_some(void* dst, size_t len) {
    if (len == 0) return 0;
    if (pos_ < end_) {
        const size_t n = std::min(len, end_ - pos_);
        std::memcpy(dst, buf_.data() + pos_, n);
        pos_ += n;
        return n;
    }
    // Buffer empty: read straight into the caller's memory to avoid an extra copy.
    return recv_into(dst, len);
}

void Reader::read_exact(void* dst, size_t len) {
    char* p = static_cast<char*>(dst);
    while (len > 0) {
        const size_t n = read_some(p, len);
        if (n == 0) throw NetError("connection closed with " + std::to_string(len) + " bytes outstanding");
        p += n;
        len -= n;
    }
}

}  // namespace tcpsync
