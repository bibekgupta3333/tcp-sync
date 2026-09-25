#include "tcpsync/client.hpp"

#include "tcpsync/hash.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>

namespace fs = std::filesystem;

namespace tcpsync {

namespace {

// UPLOAD answers "OK <version> <hash>", DOWNLOAD answers "OK <size> <hash>".
void parse_number_and_hash(const Response& r, const char* what, uint64_t& number, uint64_t& hash) {
    if (r.fields.size() != 2 || !parse_u64(r.fields[0], number) || !from_hex(r.fields[1], hash)) {
        throw ProtocolError(std::string("malformed ") + what + " response");
    }
}

}  // namespace

Client::Client(const std::string& host, uint16_t port, int timeout_sec)
    : sock_(connect_tcp(host, port)), reader_(sock_.get()) {
    configure_stream_socket(sock_.get(), timeout_sec);
}

void Client::send_text(const std::string& text) {
    if (!sock_.valid()) throw NetError("connection is closed");
    send_all(sock_.get(), text);
}

Response Client::expect_ok() {
    std::string line;
    if (!reader_.read_line(line, kMaxLineLength)) throw NetError("server closed the connection");
    Response r = parse_response(line);
    if (!r.ok) throw ServerError(r.error_code, r.error_message);
    return r;
}

std::optional<ServerError> Client::read_rejection() noexcept {
    try {
        std::string line;
        if (reader_.read_line(line, kMaxLineLength)) {
            Response r = parse_response(line);
            if (!r.ok) return ServerError(r.error_code, r.error_message);
        }
    } catch (...) {
    }
    return std::nullopt;
}

std::vector<RemoteFile> Client::list() {
    send_text(format_request(Request{Command::List, {}, 0}));
    const Response r = expect_ok();
    uint64_t count = 0;
    if (r.fields.size() != 1 || !parse_u64(r.fields[0], count)) throw ProtocolError("malformed LIST response");
    std::vector<RemoteFile> files;
    files.reserve(static_cast<size_t>(count));
    std::string line;
    for (uint64_t i = 0; i < count; ++i) {
        if (!reader_.read_line(line, kMaxLineLength)) throw NetError("server closed during LIST");
        const auto words = split_words(line);
        RemoteFile f;
        if (words.size() != 4 || !parse_u64(words[1], f.size) || !from_hex(words[2], f.hash) ||
            !parse_u64(words[3], f.version)) {
            throw ProtocolError("malformed LIST entry: " + line);
        }
        f.name = std::string(words[0]);
        files.push_back(std::move(f));
    }
    return files;
}

TransferResult Client::upload(const std::string& remote_name, uint64_t size, const ChunkSource& source) {
    send_text(format_request(Request{Command::Upload, remote_name, size}));
    Fnv1a64 hasher;
    std::vector<char> buf(kChunkSize);
    try {
        uint64_t remaining = size;
        // If the server already replied, it can only be a rejection (e.g. too_large):
        // stop streaming and go read it.
        while (remaining > 0 && !readable_now(sock_.get())) {
            const size_t want = static_cast<size_t>(std::min<uint64_t>(remaining, buf.size()));
            const size_t n = source(buf.data(), want);
            if (n == 0) throw std::runtime_error("local data shrank while uploading " + remote_name);
            hasher.update(buf.data(), n);
            send_all(sock_.get(), buf.data(), n);
            remaining -= n;
        }
        TransferResult res;
        parse_number_and_hash(expect_ok(), "UPLOAD", res.version, res.hash);
        res.size = size;
        if (res.hash != hasher.digest()) throw ProtocolError("checksum mismatch after uploading " + remote_name);
        return res;
    } catch (const NetError&) {
        // e.g. EPIPE because the server rejected us and closed; prefer its explanation.
        std::optional<ServerError> rejection = read_rejection();
        sock_.close();
        if (rejection) throw *rejection;
        throw;
    } catch (...) {
        sock_.close();  // the byte stream is out of sync; this connection is unusable
        throw;
    }
}

TransferResult Client::upload_data(const std::string& remote_name, const std::string& data) {
    size_t offset = 0;
    return upload(remote_name, data.size(), [&](char* buf, size_t cap) {
        const size_t n = std::min(cap, data.size() - offset);
        std::copy_n(data.data() + offset, n, buf);
        offset += n;
        return n;
    });
}

TransferResult Client::upload_file(const std::string& local_path, const std::string& remote_name) {
    std::ifstream in(local_path, std::ios::binary);
    if (!in) throw std::runtime_error("cannot open " + local_path);
    const uint64_t size = fs::file_size(local_path);
    return upload(remote_name, size, [&](char* buf, size_t cap) {
        in.read(buf, static_cast<std::streamsize>(cap));
        return static_cast<size_t>(in.gcount());
    });
}

TransferResult Client::download(const std::string& remote_name, const ChunkSink& sink) {
    send_text(format_request(Request{Command::Download, remote_name, 0}));
    TransferResult res;
    parse_number_and_hash(expect_ok(), "DOWNLOAD", res.size, res.hash);
    Fnv1a64 hasher;
    std::vector<char> buf(kChunkSize);
    try {
        uint64_t remaining = res.size;
        while (remaining > 0) {
            const size_t want = static_cast<size_t>(std::min<uint64_t>(remaining, buf.size()));
            const size_t n = reader_.read_some(buf.data(), want);
            if (n == 0) throw NetError("server closed during download of " + remote_name);
            hasher.update(buf.data(), n);
            sink(buf.data(), n);
            remaining -= n;
        }
    } catch (...) {
        sock_.close();
        throw;
    }
    if (hasher.digest() != res.hash) throw ProtocolError("checksum mismatch downloading " + remote_name);
    return res;
}

std::string Client::download_data(const std::string& remote_name) {
    std::string out;
    download(remote_name, [&](const char* data, size_t len) { out.append(data, len); });
    return out;
}

TransferResult Client::download_file(const std::string& remote_name, const std::string& local_path) {
    const fs::path target(local_path);
    fs::path temp = target.parent_path() / (".tcpsync-part-" + target.filename().string());
    try {
        TransferResult res;
        {
            std::ofstream out(temp, std::ios::binary | std::ios::trunc);
            if (!out) throw std::runtime_error("cannot write " + temp.string());
            res = download(remote_name, [&](const char* data, size_t len) {
                out.write(data, static_cast<std::streamsize>(len));
                if (!out) throw std::runtime_error("write failed: " + temp.string());
            });
            out.close();
            if (!out) throw std::runtime_error("write failed: " + temp.string());
        }
        fs::rename(temp, target);  // never leaves a half-written file under the real name
        return res;
    } catch (...) {
        std::error_code ec;
        fs::remove(temp, ec);
        throw;
    }
}

void Client::remove(const std::string& remote_name) {
    send_text(format_request(Request{Command::Delete, remote_name, 0}));
    expect_ok();
}

void Client::quit() {
    if (!sock_.valid()) return;
    send_text(format_request(Request{Command::Quit, {}, 0}));
    expect_ok();
    sock_.close();
}

}  // namespace tcpsync
