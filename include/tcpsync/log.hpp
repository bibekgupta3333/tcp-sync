#pragma once

// Tiny thread-safe logger. Each message is formatted into a local buffer first and then
// written under one mutex, so lines from different worker threads never interleave.

#include <atomic>
#include <iostream>
#include <mutex>
#include <sstream>
#include <thread>

namespace tcpsync {

namespace detail {
inline std::atomic<bool>& log_quiet_flag() {
    static std::atomic<bool> quiet{false};
    return quiet;
}
inline std::mutex& log_mutex() {
    static std::mutex mu;
    return mu;
}
template <typename... Args>
void log_write(const char* level, const Args&... args) {
    std::ostringstream os;
    os << level << " [tid " << std::this_thread::get_id() << "] ";
    (os << ... << args);
    os << '\n';
    const std::string line = os.str();
    std::lock_guard<std::mutex> lock(log_mutex());
    std::cerr << line;
}
}  // namespace detail

inline void set_log_quiet(bool quiet) { detail::log_quiet_flag() = quiet; }

template <typename... Args>
void log_info(const Args&... args) {
    if (!detail::log_quiet_flag()) detail::log_write("INFO ", args...);
}

template <typename... Args>
void log_error(const Args&... args) {
    detail::log_write("ERROR", args...);
}

}  // namespace tcpsync
