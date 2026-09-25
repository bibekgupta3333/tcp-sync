#include "tcpsync/log.hpp"
#include "tcpsync/server.hpp"

#include <csignal>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>

#include <pthread.h>

namespace {

void usage(std::ostream& os) {
    os << "usage: tcpsync-server [options]\n"
          "  --host ADDR       bind address (default 0.0.0.0)\n"
          "  --port N          TCP port, 0 = any free port (default 9000)\n"
          "  --root DIR        storage directory (default ./storage)\n"
          "  --workers N       worker threads (default 8)\n"
          "  --queue N         max connections waiting for a worker (default 64)\n"
          "  --timeout SEC     idle/stall timeout per connection (default 30)\n"
          "  --max-size BYTES  largest accepted upload (default 1073741824)\n"
          "  --quiet           only log errors\n";
}

unsigned long long parse_number(const std::string& flag, const char* value) {
    char* end = nullptr;
    const unsigned long long n = std::strtoull(value, &end, 10);
    if (*value == '\0' || *end != '\0') throw std::invalid_argument(flag + " needs a number, got '" + value + "'");
    return n;
}

}  // namespace

int main(int argc, char** argv) {
    tcpsync::ServerConfig config;
    try {
        for (int i = 1; i < argc; ++i) {
            const std::string arg = argv[i];
            auto value = [&]() -> const char* {
                if (i + 1 >= argc) throw std::invalid_argument(arg + " needs a value");
                return argv[++i];
            };
            if (arg == "--host") config.host = value();
            else if (arg == "--port") config.port = static_cast<uint16_t>(parse_number(arg, value()));
            else if (arg == "--root") config.root = value();
            else if (arg == "--workers") config.workers = parse_number(arg, value());
            else if (arg == "--queue") config.max_queue = parse_number(arg, value());
            else if (arg == "--timeout") config.idle_timeout_sec = static_cast<int>(parse_number(arg, value()));
            else if (arg == "--max-size") config.max_file_size = parse_number(arg, value());
            else if (arg == "--quiet") tcpsync::set_log_quiet(true);
            else if (arg == "--help" || arg == "-h") {
                usage(std::cout);
                return 0;
            } else {
                throw std::invalid_argument("unknown option " + arg);
            }
        }
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << "\n";
        usage(std::cerr);
        return 2;
    }

    // Signals: block SIGINT/SIGTERM in this thread BEFORE any other thread exists (new
    // threads inherit the mask), then receive them synchronously in one dedicated thread
    // with sigwait(). That thread may call anything, unlike an async signal handler.
    sigset_t signals;
    sigemptyset(&signals);
    sigaddset(&signals, SIGINT);
    sigaddset(&signals, SIGTERM);
    pthread_sigmask(SIG_BLOCK, &signals, nullptr);
    std::signal(SIGPIPE, SIG_IGN);  // writing to a dead peer must be an error, not death

    try {
        tcpsync::Server server(config);
        std::cout << "tcpsync-server listening on " << config.host << ":" << server.port() << std::endl;

        std::thread signal_thread([&] {
            int sig = 0;
            sigwait(&signals, &sig);
            tcpsync::log_info("received signal ", sig, ", shutting down");
            server.stop();
        });

        int rc = 0;
        try {
            server.run();
        } catch (const std::exception& e) {
            tcpsync::log_error("server failed: ", e.what());
            rc = 1;
        }
        // If run() ended on its own, the signal thread is still in sigwait(); wake it so
        // it can be joined.
        pthread_kill(signal_thread.native_handle(), SIGTERM);
        signal_thread.join();
        return rc;
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }
}
