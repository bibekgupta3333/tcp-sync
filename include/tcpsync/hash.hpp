#pragma once

// FNV-1a 64: a fast, incremental, NON-cryptographic hash. It is used only to detect
// changes and transfer corruption, never for security.

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace tcpsync {

class Fnv1a64 {
public:
    void update(const void* data, size_t len) noexcept {
        const auto* p = static_cast<const unsigned char*>(data);
        for (size_t i = 0; i < len; ++i) {
            state_ ^= p[i];
            state_ *= kPrime;
        }
    }
    uint64_t digest() const noexcept { return state_; }

private:
    static constexpr uint64_t kOffsetBasis = 14695981039346656037ULL;
    static constexpr uint64_t kPrime = 1099511628211ULL;
    uint64_t state_ = kOffsetBasis;
};

inline uint64_t hash_bytes(std::string_view data) {
    Fnv1a64 h;
    h.update(data.data(), data.size());
    return h.digest();
}

inline std::string to_hex(uint64_t value) {
    char buf[17];
    std::snprintf(buf, sizeof buf, "%016llx", static_cast<unsigned long long>(value));
    return buf;
}

inline bool from_hex(std::string_view text, uint64_t& out) {
    if (text.size() != 16) return false;
    const char* end = text.data() + text.size();
    auto [ptr, ec] = std::from_chars(text.data(), end, out, 16);
    return ec == std::errc() && ptr == end;
}

inline uint64_t hash_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("cannot open " + path);
    Fnv1a64 h;
    std::vector<char> buf(64 * 1024);
    while (in) {
        in.read(buf.data(), static_cast<std::streamsize>(buf.size()));
        h.update(buf.data(), static_cast<size_t>(in.gcount()));
    }
    if (in.bad()) throw std::runtime_error("read error on " + path);
    return h.digest();
}

}  // namespace tcpsync
