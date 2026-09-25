#include "tcpsync/file_store.hpp"

#include "tcpsync/log.hpp"
#include "tcpsync/protocol.hpp"

#include <algorithm>
#include <cerrno>
#include <filesystem>
#include <mutex>
#include <stdexcept>
#include <system_error>

#include <fcntl.h>
#include <unistd.h>

namespace fs = std::filesystem;

namespace tcpsync {

namespace {

[[noreturn]] void throw_system(const std::string& what) {
    throw std::system_error(errno, std::generic_category(), what);
}

bool starts_with(std::string_view s, std::string_view prefix) { return s.substr(0, prefix.size()) == prefix; }

}  // namespace

FileStore::FileStore(std::string root) : root_(std::move(root)) {
    fs::create_directories(root_);
    rebuild_index();
}

void FileStore::rebuild_index() {
    // Runs inside the constructor, before any other thread can see this object: no lock.
    std::vector<std::string> names;
    for (const fs::directory_entry& entry : fs::directory_iterator(root_)) {
        const std::string name = entry.path().filename().string();
        if (starts_with(name, kTempPrefix)) {
            // Left behind by a crash mid-upload. Assumes one server per storage root.
            std::error_code ec;
            fs::remove(entry.path(), ec);
            log_info("removed stale temp file ", name);
            continue;
        }
        if (entry.is_regular_file() && is_valid_name(name)) names.push_back(name);
    }
    std::sort(names.begin(), names.end());
    for (const std::string& name : names) {
        FileMeta meta;
        meta.size = fs::file_size(path_for(name));
        meta.hash = hash_file(path_for(name));
        meta.version = next_version_++;
        index_.emplace(name, meta);
    }
}

std::string FileStore::path_for(const std::string& name) const { return root_ + "/" + name; }

std::vector<FileEntry> FileStore::list() const {
    std::vector<FileEntry> entries;
    {
        std::shared_lock lock(mu_);
        entries.reserve(index_.size());
        for (const auto& [name, meta] : index_) entries.push_back({name, meta});
    }
    // Sort after releasing the lock: writers wait only for the copy.
    std::sort(entries.begin(), entries.end(),
              [](const FileEntry& a, const FileEntry& b) { return a.name < b.name; });
    return entries;
}

std::optional<FileMeta> FileStore::stat(const std::string& name) const {
    std::shared_lock lock(mu_);
    auto it = index_.find(name);
    if (it == index_.end()) return std::nullopt;
    return it->second;
}

FileStore::Upload FileStore::begin_upload(const std::string& name) {
    if (!is_valid_name(name)) throw std::invalid_argument("invalid file name: " + name);
    std::string temp = root_ + "/" + kTempPrefix + std::to_string(::getpid()) + "." +
                       std::to_string(temp_counter_.fetch_add(1));
    UniqueFd fd(::open(temp.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644));
    if (!fd.valid()) throw_system("open " + temp);
    return Upload(*this, name, std::move(temp), std::move(fd));
}

FileMeta FileStore::commit(const std::string& name, const std::string& temp_path, uint64_t size, uint64_t hash) {
    std::unique_lock lock(mu_);
    // rename() is atomic within a filesystem: a reader sees either the old file or the
    // new one, never a mix. Holding the lock makes rename + index update one step.
    if (::rename(temp_path.c_str(), path_for(name).c_str()) != 0) throw_system("rename " + temp_path);
    FileMeta meta;
    meta.size = size;
    meta.hash = hash;
    meta.version = next_version_++;
    index_[name] = meta;
    return meta;
}

std::optional<OpenedFile> FileStore::open_for_read(const std::string& name) const {
    std::shared_lock lock(mu_);
    auto it = index_.find(name);
    if (it == index_.end()) return std::nullopt;
    UniqueFd fd(::open(path_for(name).c_str(), O_RDONLY | O_CLOEXEC));
    if (!fd.valid()) throw_system("open " + name);
    return OpenedFile{std::move(fd), it->second};
}

bool FileStore::remove(const std::string& name) {
    std::unique_lock lock(mu_);
    auto it = index_.find(name);
    if (it == index_.end()) return false;
    if (::unlink(path_for(name).c_str()) != 0 && errno != ENOENT) throw_system("unlink " + name);
    index_.erase(it);
    return true;
}

FileStore::Upload::Upload(FileStore& store, std::string name, std::string temp_path, UniqueFd fd)
    : store_(&store), name_(std::move(name)), temp_path_(std::move(temp_path)), fd_(std::move(fd)) {}

FileStore::Upload::Upload(Upload&& other) noexcept
    : store_(other.store_),
      name_(std::move(other.name_)),
      temp_path_(std::move(other.temp_path_)),
      fd_(std::move(other.fd_)),
      size_(other.size_),
      hasher_(other.hasher_),
      done_(other.done_) {
    other.done_ = true;  // the moved-from object must not delete our temp file
}

FileStore::Upload::~Upload() {
    if (!done_) {
        fd_.close();
        ::unlink(temp_path_.c_str());
    }
}

void FileStore::Upload::write(const void* data, size_t len) {
    if (done_) throw std::logic_error("write after upload finished");
    const char* p = static_cast<const char*>(data);
    size_t left = len;
    while (left > 0) {
        ssize_t n = ::write(fd_.get(), p, left);
        if (n < 0) {
            if (errno == EINTR) continue;
            throw_system("write " + temp_path_);
        }
        p += n;
        left -= static_cast<size_t>(n);
    }
    hasher_.update(data, len);
    size_ += len;
}

FileMeta FileStore::Upload::commit() {
    if (done_) throw std::logic_error("upload already finished");
    if (::close(fd_.release()) != 0) throw_system("close " + temp_path_);
    FileMeta meta = store_->commit(name_, temp_path_, size_, hasher_.digest());
    done_ = true;
    return meta;
}

}  // namespace tcpsync
