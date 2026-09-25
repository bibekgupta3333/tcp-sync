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

constexpr const char* kStateFileName = ".tcpsync-state";
constexpr const char* kConflictSuffix = ".conflict";

bool ends_with(const std::string& s, const std::string& suffix) {
    return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

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

// ---------------------------------------------------------------------------------------
// Directory sync

namespace {

using HashMap = std::map<std::string, uint64_t>;

bool is_syncable(const std::string& name) { return is_valid_name(name) && !ends_with(name, kConflictSuffix); }

HashMap load_state(const fs::path& path) {
    HashMap state;
    std::ifstream in(path);
    std::string line;
    while (std::getline(in, line)) {
        const auto words = split_words(line);
        uint64_t hash = 0;
        if (words.size() == 2 && from_hex(words[0], hash) && is_valid_name(words[1])) {
            state[std::string(words[1])] = hash;
        }
    }
    return state;
}

void save_state(const fs::path& path, const HashMap& state) {
    fs::path temp = path;
    temp += ".tmp";
    {
        std::ofstream out(temp, std::ios::trunc);
        for (const auto& [name, hash] : state) out << to_hex(hash) << ' ' << name << '\n';
        out.close();
        if (!out) throw std::runtime_error("cannot write " + temp.string());
    }
    fs::rename(temp, path);
}

std::optional<uint64_t> lookup(const HashMap& m, const std::string& key) {
    auto it = m.find(key);
    if (it == m.end()) return std::nullopt;
    return it->second;
}

}  // namespace

SyncReport sync_directory(Client& client, const std::string& dir_path) {
    const fs::path dir(dir_path);
    fs::create_directories(dir);
    const fs::path state_path = dir / kStateFileName;

    const HashMap base = load_state(state_path);
    HashMap local;
    for (const fs::directory_entry& entry : fs::directory_iterator(dir)) {
        const std::string name = entry.path().filename().string();
        if (entry.is_regular_file() && is_syncable(name)) local[name] = hash_file(entry.path().string());
    }
    HashMap remote;
    for (const RemoteFile& f : client.list()) {
        if (is_syncable(f.name)) remote[f.name] = f.hash;
    }

    std::set<std::string> names;
    for (const HashMap* m : std::initializer_list<const HashMap*>{&base, &local, &remote}) {
        for (const auto& kv : *m) names.insert(kv.first);
    }

    // 3-way merge. B = content at the last successful sync, L = local now, R = remote now.
    // Whichever side still equals B is unchanged, so the other side's change wins.
    SyncReport report;
    HashMap next_base;
    for (const std::string& name : names) {
        const std::optional<uint64_t> L = lookup(local, name);
        const std::optional<uint64_t> R = lookup(remote, name);
        const std::optional<uint64_t> B = lookup(base, name);
        const std::string path = (dir / name).string();

        if (L && R) {
            if (*L == *R) {
                next_base[name] = *L;
                ++report.unchanged;
            } else if (B == L) {  // only remote changed
                next_base[name] = client.download_file(name, path).hash;
                report.downloaded.push_back(name);
            } else if (B == R) {  // only local changed
                next_base[name] = client.upload_file(path, name).hash;
                report.uploaded.push_back(name);
            } else {  // both changed: the server copy wins, the local copy is kept aside
                fs::rename(path, path + kConflictSuffix);
                next_base[name] = client.download_file(name, path).hash;
                report.conflicts.push_back(name);
            }
        } else if (L) {
            if (B == L) {  // deleted remotely, unchanged locally
                fs::remove(path);
                report.deleted_local.push_back(name);
            } else {  // new locally, or edited locally after a remote delete: edit wins
                next_base[name] = client.upload_file(path, name).hash;
                report.uploaded.push_back(name);
            }
        } else if (R) {
            if (B == R) {  // deleted locally, unchanged remotely
                try {
                    client.remove(name);
                } catch (const ServerError& e) {
                    if (e.code() != "not_found") throw;
                }
                report.deleted_remote.push_back(name);
            } else {
                next_base[name] = client.download_file(name, path).hash;
                report.downloaded.push_back(name);
            }
        }
        // Gone on both sides: it just drops out of the new state.
    }
    // Saved only after every step succeeded. If we crash earlier, the next run recomputes
    // from the old state, and steps that already happened show up as L == R (no-ops).
    save_state(state_path, next_base);
    return report;
}

}  // namespace tcpsync
