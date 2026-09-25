#include "tcpsync/protocol.hpp"

#include <charconv>

namespace tcpsync {

namespace {

bool is_space(char c) { return c == ' ' || c == '\t'; }

}  // namespace

std::vector<std::string_view> split_words(std::string_view line) {
    std::vector<std::string_view> words;
    size_t i = 0;
    while (i < line.size()) {
        while (i < line.size() && is_space(line[i])) ++i;
        const size_t start = i;
        while (i < line.size() && !is_space(line[i])) ++i;
        if (i > start) words.push_back(line.substr(start, i - start));
    }
    return words;
}

bool parse_u64(std::string_view text, uint64_t& out) {
    if (text.empty()) return false;
    const char* end = text.data() + text.size();
    auto [ptr, ec] = std::from_chars(text.data(), end, out);
    return ec == std::errc() && ptr == end;
}

bool is_valid_name(std::string_view name) {
    if (name.empty() || name.size() > kMaxNameLength || name.front() == '.') return false;
    for (unsigned char c : name) {
        if (c <= 0x20 || c >= 0x7f || c == '/' || c == '\\') return false;
    }
    return true;
}

std::optional<Request> parse_request(std::string_view line, std::string& error) {
    const std::vector<std::string_view> words = split_words(line);
    if (words.empty()) {
        error = "empty request";
        return std::nullopt;
    }
    const std::string_view verb = words[0];
    auto arity = [&](size_t expected) {
        if (words.size() == expected) return true;
        error = std::string(verb) + " expects " + std::to_string(expected - 1) + " argument(s)";
        return false;
    };
    auto name_ok = [&](std::string_view name) {
        if (is_valid_name(name)) return true;
        error = "invalid file name";
        return false;
    };

    Request req;
    if (verb == "LIST" || verb == "QUIT") {
        if (!arity(1)) return std::nullopt;
        req.command = verb == "LIST" ? Command::List : Command::Quit;
        return req;
    }
    if (verb == "UPLOAD") {
        if (!arity(3) || !name_ok(words[1])) return std::nullopt;
        if (!parse_u64(words[2], req.size)) {
            error = "invalid size";
            return std::nullopt;
        }
        req.command = Command::Upload;
        req.name = std::string(words[1]);
        return req;
    }
    if (verb == "DOWNLOAD" || verb == "DELETE") {
        if (!arity(2) || !name_ok(words[1])) return std::nullopt;
        req.command = verb == "DOWNLOAD" ? Command::Download : Command::Delete;
        req.name = std::string(words[1]);
        return req;
    }
    error = "unknown command " + std::string(verb);
    return std::nullopt;
}

std::string format_request(const Request& request) {
    switch (request.command) {
        case Command::List: return "LIST\n";
        case Command::Quit: return "QUIT\n";
        case Command::Upload: return "UPLOAD " + request.name + " " + std::to_string(request.size) + "\n";
        case Command::Download: return "DOWNLOAD " + request.name + "\n";
        case Command::Delete: return "DELETE " + request.name + "\n";
    }
    return {};
}

Response parse_response(std::string_view line) {
    const std::vector<std::string_view> words = split_words(line);
    Response r;
    if (!words.empty() && words[0] == "OK") {
        r.ok = true;
        for (size_t i = 1; i < words.size(); ++i) r.fields.emplace_back(words[i]);
        return r;
    }
    if (!words.empty() && words[0] == "ERR") {
        r.error_code = words.size() > 1 ? std::string(words[1]) : "unknown";
        if (words.size() > 2) {
            const size_t start = static_cast<size_t>(words[2].data() - line.data());
            r.error_message = std::string(line.substr(start));
        }
        return r;
    }
    throw ProtocolError("malformed response: " + std::string(line.substr(0, 80)));
}

std::string ok_line(const std::vector<std::string>& fields) {
    std::string line = "OK";
    for (const std::string& f : fields) {
        line += ' ';
        line += f;
    }
    line += '\n';
    return line;
}

std::string err_line(std::string_view code, std::string_view message) {
    std::string line = "ERR ";
    line += code;
    line += ' ';
    for (char c : message) line += (c == '\n' || c == '\r') ? ' ' : c;
    line += '\n';
    return line;
}

}  // namespace tcpsync
