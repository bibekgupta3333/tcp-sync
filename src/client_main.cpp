#include "tcpsync/client.hpp"
#include "tcpsync/hash.hpp"

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace {

void usage(std::ostream& os) {
    os << "usage: tcpsync-client [--host H] [--port P] [--timeout SEC] <command> [args]\n"
          "commands:\n"
          "  list                          list files on the server\n"
          "  upload <local> [remote]       upload a file (remote name defaults to the basename)\n"
          "  download <remote> [local]     download a file (local path defaults to ./<remote>)\n"
          "  delete <remote>               delete a file on the server\n"
          "  sync <dir>                    two-way sync of a flat directory\n";
}

void print_names(const char* label, const std::vector<std::string>& names) {
    for (const std::string& n : names) std::cout << label << " " << n << "\n";
}

}  // namespace

int main(int argc, char** argv) {
    std::signal(SIGPIPE, SIG_IGN);
    std::string host = "127.0.0.1";
    uint16_t port = 9000;
    int timeout = 30;
    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if ((arg == "--host" || arg == "--port" || arg == "--timeout") && i + 1 < argc) {
            const std::string value = argv[++i];
            if (arg == "--host") host = value;
            else if (arg == "--port") port = static_cast<uint16_t>(std::strtoul(value.c_str(), nullptr, 10));
            else timeout = std::atoi(value.c_str());
        } else if (arg == "--help" || arg == "-h") {
            usage(std::cout);
            return 0;
        } else {
            args.push_back(arg);
        }
    }
    if (args.empty()) {
        usage(std::cerr);
        return 2;
    }
    const std::string& cmd = args[0];
    auto arg_or = [&](size_t i, const std::string& fallback) { return i < args.size() ? args[i] : fallback; };

    try {
        tcpsync::Client client(host, port, timeout);
        if (cmd == "list" && args.size() == 1) {
            for (const tcpsync::RemoteFile& f : client.list()) {
                std::printf("%-40s %12llu  %s  v%llu\n", f.name.c_str(), static_cast<unsigned long long>(f.size),
                            tcpsync::to_hex(f.hash).c_str(), static_cast<unsigned long long>(f.version));
            }
        } else if (cmd == "upload" && (args.size() == 2 || args.size() == 3)) {
            const std::string remote = arg_or(2, std::filesystem::path(args[1]).filename().string());
            const tcpsync::TransferResult r = client.upload_file(args[1], remote);
            std::cout << "uploaded " << remote << " (" << r.size << " bytes, hash " << tcpsync::to_hex(r.hash)
                      << ", v" << r.version << ")\n";
        } else if (cmd == "download" && (args.size() == 2 || args.size() == 3)) {
            const std::string local = arg_or(2, args[1]);
            const tcpsync::TransferResult r = client.download_file(args[1], local);
            std::cout << "downloaded " << args[1] << " -> " << local << " (" << r.size << " bytes, hash "
                      << tcpsync::to_hex(r.hash) << ")\n";
        } else if (cmd == "delete" && args.size() == 2) {
            client.remove(args[1]);
            std::cout << "deleted " << args[1] << "\n";
        } else if (cmd == "sync" && args.size() == 2) {
            const tcpsync::SyncReport r = tcpsync::sync_directory(client, args[1]);
            print_names("upload       ", r.uploaded);
            print_names("download     ", r.downloaded);
            print_names("delete-local ", r.deleted_local);
            print_names("delete-remote", r.deleted_remote);
            for (const std::string& n : r.conflicts) {
                std::cout << "conflict      " << n << " (server copy kept; yours saved as " << n << ".conflict)\n";
            }
            std::cout << r.unchanged << " unchanged\n";
        } else {
            usage(std::cerr);
            return 2;
        }
        client.quit();
        return 0;
    } catch (const tcpsync::ServerError& e) {
        std::cerr << "server error: " << e.what() << "\n";
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << "\n";
    }
    return 1;
}
