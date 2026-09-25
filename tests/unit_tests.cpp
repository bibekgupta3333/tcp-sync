// Self-contained test runner (no framework dependency). Run: build/unit_tests [filter]
// The end-to-end tests start a real Server on an ephemeral port inside this process, so
// `make tsan` checks the server, client and store together for data races.

#include "tcpsync/client.hpp"
#include "tcpsync/file_store.hpp"
#include "tcpsync/hash.hpp"
#include "tcpsync/log.hpp"
#include "tcpsync/protocol.hpp"
#include "tcpsync/server.hpp"
#include "tcpsync/thread_pool.hpp"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <memory>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include <unistd.h>

namespace fs = std::filesystem;
using namespace tcpsync;

namespace {

struct TestCase {
    const char* name;
    void (*fn)();
};

std::vector<TestCase>& registry() {
    static std::vector<TestCase> tests;
    return tests;
}

struct Registrar {
    Registrar(const char* name, void (*fn)()) { registry().push_back({name, fn}); }
};

struct CheckFailure : std::runtime_error {
    using std::runtime_error::runtime_error;
};

#define TEST(name)                               \
    void name();                                 \
    const Registrar registrar_##name(#name, name); \
    void name()

// CHECK throws, so only use it on the test's own thread. Worker threads count failures
// in an atomic that the test thread CHECKs after join().
#define CHECK(cond)                                                                                   \
    do {                                                                                              \
        if (!(cond)) {                                                                                \
            throw CheckFailure(std::string(__FILE__) + ":" + std::to_string(__LINE__) + ": CHECK(" #cond \
                               ") failed");                                                           \
        }                                                                                             \
    } while (0)

#define CHECK_THROWS(expr)      \
    do {                        \
        bool threw_ = false;    \
        try {                   \
            (void)(expr);       \
        } catch (...) {         \
            threw_ = true;      \
        }                       \
        CHECK(threw_);          \
    } while (0)

struct TempDir {
    fs::path path;
    TempDir() {
        std::string pattern = (fs::temp_directory_path() / "tcpsync-test-XXXXXX").string();
        if (::mkdtemp(pattern.data()) == nullptr) throw std::runtime_error("mkdtemp failed");
        path = pattern;
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

std::string random_bytes(size_t n, uint64_t seed) {
    std::mt19937_64 rng(seed);
    std::string s(n, '\0');
    for (char& c : s) c = static_cast<char>(rng() & 0xff);
    return s;
}

std::string read_fd(int fd) {
    std::string out;
    char buf[8192];
    ssize_t n;
    while ((n = ::read(fd, buf, sizeof buf)) > 0) out.append(buf, static_cast<size_t>(n));
    return out;
}

void write_file(const fs::path& p, const std::string& data) {
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    out << data;
}

std::string read_file(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), {});
}

size_t count_entries(const fs::path& dir) {
    size_t n = 0;
    for ([[maybe_unused]] const auto& e : fs::directory_iterator(dir)) ++n;
    return n;
}

// Runs a real server on 127.0.0.1:<ephemeral> in a background thread.
class TestServer {
public:
    explicit TestServer(size_t workers = 8, size_t max_queue = 64, uint64_t max_file_size = 64ULL << 20) {
        ServerConfig cfg;
        cfg.host = "127.0.0.1";
        cfg.port = 0;
        cfg.root = (dir_.path / "store").string();
        cfg.workers = workers;
        cfg.max_queue = max_queue;
        cfg.max_file_size = max_file_size;
        server_ = std::make_unique<Server>(cfg);
        thread_ = std::thread([this] {
            try {
                server_->run();
            } catch (const std::exception& e) {
                std::cerr << "server thread failed: " << e.what() << "\n";
                std::abort();
            }
        });
    }
    ~TestServer() { stop(); }

    uint16_t port() const { return server_->port(); }
    fs::path root() const { return dir_.path / "store"; }

    void stop() {
        if (thread_.joinable()) {
            server_->stop();
            thread_.join();
        }
    }

private:
    TempDir dir_;
    std::unique_ptr<Server> server_;
    std::thread thread_;
};

// ------------------------------------------------------------------ protocol

TEST(name_validation) {
    CHECK(is_valid_name("report.pdf"));
    CHECK(is_valid_name("a..b"));
    CHECK(is_valid_name(std::string(255, 'x')));
    CHECK(!is_valid_name(""));
    CHECK(!is_valid_name("."));
    CHECK(!is_valid_name(".."));
    CHECK(!is_valid_name(".hidden"));
    CHECK(!is_valid_name("a/b"));
    CHECK(!is_valid_name("../etc/passwd"));
    CHECK(!is_valid_name("back\\slash"));
    CHECK(!is_valid_name("has space"));
    CHECK(!is_valid_name("tab\tname"));
    CHECK(!is_valid_name(std::string(256, 'x')));
    CHECK(!is_valid_name(std::string("nul\0byte", 8)));
}

TEST(request_parsing) {
    std::string err;
    auto up = parse_request("UPLOAD notes.txt 42", err);
    CHECK(up && up->command == Command::Upload && up->name == "notes.txt" && up->size == 42);
    CHECK(parse_request("  LIST  ", err)->command == Command::List);
    CHECK(parse_request("DOWNLOAD a\r", err) == std::nullopt);  // Reader strips \r, parser doesn't
    CHECK(parse_request("DOWNLOAD a", err)->command == Command::Download);
    CHECK(parse_request("DELETE a", err)->command == Command::Delete);
    CHECK(parse_request("QUIT", err)->command == Command::Quit);

    CHECK(!parse_request("", err));
    CHECK(!parse_request("FROB x", err));
    CHECK(!parse_request("LIST extra", err));
    CHECK(!parse_request("UPLOAD a", err));
    CHECK(!parse_request("UPLOAD a -1", err));
    CHECK(!parse_request("UPLOAD a 12x", err));
    CHECK(!parse_request("UPLOAD a 99999999999999999999999", err));  // overflows uint64
    CHECK(!parse_request("DELETE ../x", err));
    CHECK(err == "invalid file name");

    const Request r{Command::Upload, "f.bin", 7};
    auto round_trip = parse_request(format_request(r).substr(0, format_request(r).size() - 1), err);
    CHECK(round_trip && round_trip->name == "f.bin" && round_trip->size == 7);
}

TEST(response_parsing) {
    Response ok = parse_response("OK 3 00000000000000ff");
    CHECK(ok.ok && ok.fields.size() == 2 && ok.fields[0] == "3");
    Response err = parse_response("ERR not_found no such file: x");
    CHECK(!err.ok && err.error_code == "not_found" && err.error_message == "no such file: x");
    CHECK_THROWS(parse_response("HELLO world"));
    CHECK(err_line("io", "two\nlines") == "ERR io two lines\n");
    CHECK(ok_line({"a", "b"}) == "OK a b\n");
}

TEST(fnv1a_known_vectors) {
    CHECK(hash_bytes("") == 0xcbf29ce484222325ULL);
    CHECK(hash_bytes("a") == 0xaf63dc4c8601ec8cULL);
    CHECK(hash_bytes("foobar") == 0x85944171f73967e8ULL);
    Fnv1a64 incremental;
    incremental.update("foo", 3);
    incremental.update("bar", 3);
    CHECK(incremental.digest() == hash_bytes("foobar"));
    uint64_t parsed = 0;
    CHECK(from_hex(to_hex(0x0123456789abcdefULL), parsed) && parsed == 0x0123456789abcdefULL);
    CHECK(!from_hex("123", parsed));
}

// ------------------------------------------------------------------ thread pool

TEST(thread_pool_runs_every_task) {
    std::atomic<int> count{0};
    ThreadPool pool(4, 1000);
    for (int i = 0; i < 500; ++i) CHECK(pool.try_submit([&] { count.fetch_add(1); }));
    pool.shutdown();  // drains the queue before joining
    CHECK(count.load() == 500);
    CHECK(!pool.try_submit([] {}));
}

TEST(thread_pool_rejects_when_queue_full) {
    ThreadPool pool(1, 1);
    std::promise<void> started;
    std::promise<void> release;
    std::shared_future<void> release_signal = release.get_future().share();
    CHECK(pool.try_submit([&started, release_signal] {
        started.set_value();
        release_signal.wait();
    }));
    started.get_future().wait();       // the only worker is now busy
    CHECK(pool.try_submit([] {}));     // fills the single queue slot
    CHECK(!pool.try_submit([] {}));    // backpressure
    release.set_value();
    pool.shutdown();
}

TEST(thread_pool_survives_throwing_task) {
    set_log_quiet(true);
    std::atomic<bool> ran{false};
    ThreadPool pool(1, 4);
    CHECK(pool.try_submit([] { throw std::runtime_error("boom"); }));
    CHECK(pool.try_submit([&] { ran = true; }));
    pool.shutdown();
    CHECK(ran.load());
}

// ------------------------------------------------------------------ file store

TEST(file_store_upload_and_read) {
    TempDir d;
    FileStore store((d.path / "s").string());
    FileStore::Upload up = store.begin_upload("a.txt");
    up.write("hel", 3);
    up.write("lo", 2);
    const FileMeta meta = up.commit();
    CHECK(meta.size == 5 && meta.hash == hash_bytes("hello") && meta.version == 1);
    auto file = store.open_for_read("a.txt");
    CHECK(file && read_fd(file->fd.get()) == "hello");
    CHECK(store.list().size() == 1);
    CHECK(!store.open_for_read("missing"));
    CHECK(store.remove("a.txt"));
    CHECK(!store.remove("a.txt"));
    CHECK(count_entries(d.path / "s") == 0);
}

TEST(file_store_abandoned_upload_leaves_no_trace) {
    TempDir d;
    FileStore store((d.path / "s").string());
    {
        FileStore::Upload up = store.begin_upload("x.bin");
        up.write("partial", 7);
    }  // destroyed without commit, like a client that disconnected mid-upload
    CHECK(!store.stat("x.bin"));
    CHECK(count_entries(d.path / "s") == 0);
}

TEST(file_store_rejects_bad_names) {
    TempDir d;
    FileStore store((d.path / "s").string());
    CHECK_THROWS(store.begin_upload("../escape"));
    CHECK_THROWS(store.begin_upload(".hidden"));
}

TEST(file_store_rebuilds_index_and_removes_stale_temps) {
    TempDir d;
    const std::string root = (d.path / "s").string();
    {
        FileStore store(root);
        FileStore::Upload up = store.begin_upload("b.txt");
        up.write("abc", 3);
        up.commit();
    }
    const fs::path stale = fs::path(root) / (std::string(FileStore::kTempPrefix) + "999.1");
    write_file(stale, "junk");
    FileStore reopened(root);
    auto meta = reopened.stat("b.txt");
    CHECK(meta && meta->size == 3 && meta->hash == hash_bytes("abc"));
    CHECK(!fs::exists(stale));
}

TEST(file_store_concurrent_writers_and_readers) {
    // Many writers replace the same file while readers keep reading it. Every read must
    // be a consistent snapshot: the bytes must match the metadata returned with the fd.
    TempDir d;
    FileStore store((d.path / "s").string());
    constexpr int kWriters = 8;
    constexpr int kRounds = 25;
    std::atomic<int> failures{0};
    std::atomic<bool> writers_done{false};

    std::vector<std::thread> readers;
    for (int r = 0; r < 4; ++r) {
        readers.emplace_back([&] {
            while (!writers_done.load()) {
                auto file = store.open_for_read("shared.bin");
                if (!file) continue;
                const std::string data = read_fd(file->fd.get());
                if (data.size() != file->meta.size || hash_bytes(data) != file->meta.hash) failures.fetch_add(1);
            }
        });
    }
    std::vector<std::thread> writers;
    for (int w = 0; w < kWriters; ++w) {
        writers.emplace_back([&, w] {
            for (int round = 0; round < kRounds; ++round) {
                const std::string data = random_bytes(1 + (w * 7919 + round * 104729) % 65536, w * 1000 + round);
                try {
                    FileStore::Upload up = store.begin_upload("shared.bin");
                    up.write(data.data(), data.size());
                    if (up.commit().hash != hash_bytes(data)) failures.fetch_add(1);
                } catch (...) {
                    failures.fetch_add(1);
                }
            }
        });
    }
    for (std::thread& t : writers) t.join();
    writers_done = true;
    for (std::thread& t : readers) t.join();

    CHECK(failures.load() == 0);
    auto final_file = store.open_for_read("shared.bin");
    CHECK(final_file);
    CHECK(final_file->meta.version == kWriters * kRounds);  // every commit got a unique version
    CHECK(hash_bytes(read_fd(final_file->fd.get())) == final_file->meta.hash);
    CHECK(count_entries(d.path / "s") == 1);  // no temp files left behind
}

// ------------------------------------------------------------------ server end-to-end

TEST(server_concurrent_clients_round_trip) {
    TestServer ts;
    constexpr int kClients = 16;
    std::vector<std::string> payloads;
    for (int i = 0; i < kClients; ++i) payloads.push_back(random_bytes(i == 0 ? 0 : (i * 16411) % 262144, i));

    std::atomic<int> failures{0};
    std::vector<std::thread> threads;
    for (int i = 0; i < kClients; ++i) {
        threads.emplace_back([&, i] {
            try {
                Client c("127.0.0.1", ts.port());
                const std::string name = "file" + std::to_string(i) + ".bin";
                const TransferResult up = c.upload_data(name, payloads[i]);
                if (up.hash != hash_bytes(payloads[i])) failures.fetch_add(1);
                if (c.download_data(name) != payloads[i]) failures.fetch_add(1);
                c.quit();
            } catch (const std::exception& e) {
                std::cerr << "client " << i << ": " << e.what() << "\n";
                failures.fetch_add(1);
            }
        });
    }
    for (std::thread& t : threads) t.join();
    CHECK(failures.load() == 0);
    Client c("127.0.0.1", ts.port());
    CHECK(c.list().size() == kClients);
}

TEST(server_same_file_upload_race) {
    TestServer ts;
    constexpr int kClients = 10;
    std::vector<std::string> payloads;
    for (int i = 0; i < kClients; ++i) payloads.push_back(random_bytes(100000 + i, 500 + i));
    std::atomic<int> failures{0};
    std::vector<std::thread> threads;
    for (int i = 0; i < kClients; ++i) {
        threads.emplace_back([&, i] {
            try {
                Client c("127.0.0.1", ts.port());
                c.upload_data("race.bin", payloads[i]);
            } catch (...) {
                failures.fetch_add(1);
            }
        });
    }
    for (std::thread& t : threads) t.join();
    CHECK(failures.load() == 0);
    Client c("127.0.0.1", ts.port());
    const std::string winner = c.download_data("race.bin");
    int matches = 0;
    for (const std::string& p : payloads) matches += (p == winner);
    CHECK(matches == 1);  // exactly one complete upload won, no interleaving
    CHECK(count_entries(ts.root()) == 1);
}

TEST(server_missing_file_keeps_connection_open) {
    TestServer ts;
    Client c("127.0.0.1", ts.port());
    bool not_found = false;
    try {
        c.download_data("nope");
    } catch (const ServerError& e) {
        not_found = e.code() == "not_found";
    }
    CHECK(not_found);
    CHECK(c.list().empty());  // same connection still usable
}

TEST(server_rejects_path_traversal) {
    TestServer ts;
    UniqueFd sock = connect_tcp("127.0.0.1", ts.port());
    configure_stream_socket(sock.get(), 5);
    send_all(sock.get(), "DOWNLOAD ../../etc/passwd\n");
    Reader reader(sock.get());
    std::string line;
    CHECK(reader.read_line(line, kMaxLineLength));
    CHECK(line.rfind("ERR bad_request", 0) == 0);
}

TEST(server_rejects_oversized_upload) {
    TestServer ts(4, 16, /*max_file_size=*/1024);
    Client c("127.0.0.1", ts.port());
    std::string code;
    try {
        c.upload_data("big.bin", std::string(512 * 1024, 'x'));
    } catch (const ServerError& e) {
        code = e.code();
    }
    CHECK(code == "too_large");
    CHECK(count_entries(ts.root()) == 0);
    Client again("127.0.0.1", ts.port());
    CHECK(again.upload_data("small.bin", "fits").size == 4);  // server unaffected
}

TEST(server_answers_busy_when_saturated) {
    TestServer ts(/*workers=*/1, /*max_queue=*/1);
    Client holder("127.0.0.1", ts.port());
    holder.list();  // the single worker is now serving `holder`'s connection
    UniqueFd queued = connect_tcp("127.0.0.1", ts.port());    // waits in the queue slot
    UniqueFd rejected = connect_tcp("127.0.0.1", ts.port());  // nowhere to go
    configure_stream_socket(rejected.get(), 5);
    Reader reader(rejected.get());
    std::string line;
    CHECK(reader.read_line(line, kMaxLineLength));
    CHECK(line.rfind("ERR busy", 0) == 0);
}

TEST(server_stop_unblocks_idle_connections) {
    auto ts = std::make_unique<TestServer>();
    Client idle("127.0.0.1", ts->port());
    idle.list();  // a worker is now blocked in recv() on this connection (30s idle timeout)
    const auto start = std::chrono::steady_clock::now();
    ts->stop();
    const auto elapsed = std::chrono::steady_clock::now() - start;
    CHECK(elapsed < std::chrono::seconds(3));
}

// ------------------------------------------------------------------ sync

TEST(sync_two_way_with_conflicts_and_deletes) {
    TestServer ts;
    TempDir a;
    TempDir b;
    Client ca("127.0.0.1", ts.port());
    Client cb("127.0.0.1", ts.port());

    write_file(a.path / "notes.txt", "v1");
    write_file(a.path / "other.txt", "keep");
    SyncReport r = sync_directory(ca, a.path.string());
    CHECK(r.uploaded.size() == 2);
    r = sync_directory(cb, b.path.string());
    CHECK(r.downloaded.size() == 2);
    CHECK(read_file(b.path / "notes.txt") == "v1");

    // An edit on A reaches B.
    write_file(a.path / "notes.txt", "v2");
    CHECK(sync_directory(ca, a.path.string()).uploaded.size() == 1);
    CHECK(sync_directory(cb, b.path.string()).downloaded.size() == 1);
    CHECK(read_file(b.path / "notes.txt") == "v2");

    // Both sides edit the same file: the first to sync wins, the other keeps a .conflict copy.
    write_file(a.path / "notes.txt", "from-a");
    write_file(b.path / "notes.txt", "from-b");
    sync_directory(ca, a.path.string());
    r = sync_directory(cb, b.path.string());
    CHECK(r.conflicts.size() == 1);
    CHECK(read_file(b.path / "notes.txt") == "from-a");
    CHECK(read_file(b.path / "notes.txt.conflict") == "from-b");

    // A deletion propagates.
    fs::remove(a.path / "other.txt");
    CHECK(sync_directory(ca, a.path.string()).deleted_remote.size() == 1);
    CHECK(sync_directory(cb, b.path.string()).deleted_local.size() == 1);
    CHECK(!fs::exists(b.path / "other.txt"));

    // Steady state: nothing to do.
    r = sync_directory(ca, a.path.string());
    CHECK(r.uploaded.empty() && r.downloaded.empty() && r.conflicts.empty() && r.unchanged == 1);
}

}  // namespace

int main(int argc, char** argv) {
    std::signal(SIGPIPE, SIG_IGN);
    set_log_quiet(std::getenv("TCPSYNC_VERBOSE") == nullptr);
    const std::string filter = argc > 1 ? argv[1] : "";
    int passed = 0;
    int failed = 0;
    for (const TestCase& t : registry()) {
        if (!filter.empty() && std::string(t.name).find(filter) == std::string::npos) continue;
        const auto start = std::chrono::steady_clock::now();
        try {
            t.fn();
            const auto ms =
                std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);
            std::cout << "[ OK ] " << t.name << " (" << ms.count() << " ms)\n";
            ++passed;
        } catch (const std::exception& e) {
            std::cout << "[FAIL] " << t.name << ": " << e.what() << "\n";
            ++failed;
        }
    }
    std::cout << passed << " passed, " << failed << " failed\n";
    return failed == 0 ? 0 : 1;
}
