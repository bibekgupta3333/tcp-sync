#pragma once

// Wire protocol: one text header line per request/response, then an optional raw payload.
//
//   LIST                    -> OK <n>\n followed by n lines "<name> <size> <hash> <version>"
//   UPLOAD <name> <size>    -> (client sends <size> raw bytes) -> OK <version> <hash>
//   DOWNLOAD <name>         -> OK <size> <hash>\n followed by <size> raw bytes
//   DELETE <name>           -> OK
//   QUIT                    -> OK bye (server closes)
//   any failure             -> ERR <code> <message>
//
// <hash> is 16 lowercase hex digits of FNV-1a 64 over the file contents.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace tcpsync {

constexpr size_t kMaxLineLength = 1024;
constexpr size_t kMaxNameLength = 255;
constexpr size_t kChunkSize = 64 * 1024;

// A response line that is neither OK nor ERR, or an OK with the wrong fields.
class ProtocolError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

enum class Command { List, Upload, Download, Delete, Quit };

struct Request {
    Command command = Command::List;
    std::string name;
    uint64_t size = 0;
};

struct Response {
    bool ok = false;
    std::vector<std::string> fields;  // words after "OK"
    std::string error_code;           // word after "ERR"
    std::string error_message;        // rest of the ERR line
};

std::vector<std::string_view> split_words(std::string_view line);
bool parse_u64(std::string_view text, uint64_t& out);

// Flat namespace only: printable ASCII, no '/', '\\' or whitespace, no leading '.'
// (which also rules out "." and ".." and keeps server temp files out of the namespace).
bool is_valid_name(std::string_view name);

// Returns std::nullopt and sets `error` when the line is malformed.
std::optional<Request> parse_request(std::string_view line, std::string& error);
std::string format_request(const Request& request);

// Throws ProtocolError when the line starts with neither OK nor ERR.
Response parse_response(std::string_view line);
std::string ok_line(const std::vector<std::string>& fields = {});
std::string err_line(std::string_view code, std::string_view message);

}  // namespace tcpsync
