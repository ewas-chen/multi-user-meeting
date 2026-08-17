#include "InMemoryMeetingStore.h"
#include "MeetingServiceImpl.h"
#include "UserServiceImpl.h"

#include <arpa/inet.h>
#include <grpcpp/grpcpp.h>
#include <pthread.h>
#include <signal.h>

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

namespace {

struct ServerOptions final {
    std::string listen_address{"0.0.0.0:50051"};
    std::string media_server_ip{"82.156.137.234"};
};

enum class ParseResult {
    kSuccess,
    kShowHelp,
    kFailure
};

void PrintUsage(const char* executable_name)
{
    std::cout
        << "VCE local gRPC test server\n\n"
        << "Usage:\n  "
        << executable_name
        << " [--listen ADDRESS] [--media-ip IPV4]\n\n"
        << "Options:\n"
        << "  --listen ADDRESS    gRPC listening address\n"
        << "                      Default: 0.0.0.0:50051\n"
        << "  --media-ip IPV4     SRS media server IPv4 address\n"
        << "                      Default: 82.156.137.234\n"
        << "  -h, --help          Show this help message\n\n"
        << "Example:\n  "
        << executable_name
        << " --listen 0.0.0.0:50051"
        << " --media-ip 82.156.137.234\n";
}

bool StartsWith(const std::string& value,
                const std::string& prefix)
{
    return value.size() >= prefix.size() &&
           value.compare(0, prefix.size(), prefix) == 0;
}

ParseResult ParseArguments(int argc,
                           char* argv[],
                           ServerOptions& options)
{
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];

        if (argument == "-h" || argument == "--help") {
            return ParseResult::kShowHelp;
        }

        if (argument == "--listen") {
            if (index + 1 >= argc) {
                std::cerr
                    << "Missing value after --listen"
                    << std::endl;
                return ParseResult::kFailure;
            }

            options.listen_address = argv[++index];
            continue;
        }

        if (StartsWith(argument, "--listen=")) {
            options.listen_address =
                argument.substr(
                    std::string("--listen=").size());
            continue;
        }

        if (argument == "--media-ip") {
            if (index + 1 >= argc) {
                std::cerr
                    << "Missing value after --media-ip"
                    << std::endl;
                return ParseResult::kFailure;
            }

            options.media_server_ip = argv[++index];
            continue;
        }

        if (StartsWith(argument, "--media-ip=")) {
            options.media_server_ip =
                argument.substr(
                    std::string("--media-ip=").size());
            continue;
        }

        std::cerr
            << "Unknown argument: "
            << argument
            << std::endl;

        return ParseResult::kFailure;
    }

    if (options.listen_address.empty()) {
        std::cerr
            << "The listening address cannot be empty"
            << std::endl;
        return ParseResult::kFailure;
    }

    if (options.media_server_ip.empty()) {
        std::cerr
            << "The media server IP cannot be empty"
            << std::endl;
        return ParseResult::kFailure;
    }

    return ParseResult::kSuccess;
}

bool ParseNetworkIPv4(const std::string& text,
                      std::uint32_t& network_ip)
{
    network_ip = 0;

    in_addr address{};

    if (inet_pton(
            AF_INET,
            text.c_str(),
            &address) != 1) {
        return false;
    }

    if (address.s_addr == 0 ||
        address.s_addr == INADDR_NONE) {
        return false;
    }

    /*
     * meeting_service.proto中的IP字段使用网络字节序整数，
     * 因此直接保留inet_pton()生成的address.s_addr。
     */
    network_ip = address.s_addr;
    return true;
}

bool BlockShutdownSignals(sigset_t& signal_set)
{
    sigemptyset(&signal_set);
    sigaddset(&signal_set, SIGINT);
    sigaddset(&signal_set, SIGTERM);

    /*
     * 在gRPC创建工作线程前屏蔽信号。
     * 随后由独立线程使用sigwait()同步等待，
     * 避免在异步信号处理函数中调用复杂的gRPC接口。
     */
    return pthread_sigmask(
               SIG_BLOCK,
               &signal_set,
               nullptr) == 0;
}

} // namespace

int main(int argc, char* argv[])
{
    ServerOptions options;

    const ParseResult parse_result =
        ParseArguments(argc, argv, options);

    if (parse_result == ParseResult::kShowHelp) {
        PrintUsage(argv[0]);
        return EXIT_SUCCESS;
    }

    if (parse_result == ParseResult::kFailure) {
        PrintUsage(argv[0]);
        return EXIT_FAILURE;
    }

    std::uint32_t media_server_ip = 0;

    if (!ParseNetworkIPv4(
            options.media_server_ip,
            media_server_ip)) {
        std::cerr
            << "Invalid media server IPv4 address: "
            << options.media_server_ip
            << std::endl;

        return EXIT_FAILURE;
    }

    sigset_t shutdown_signals;

    if (!BlockShutdownSignals(shutdown_signals)) {
        std::cerr
            << "Failed to configure shutdown signal handling"
            << std::endl;

        return EXIT_FAILURE;
    }

    auto store =
        std::make_shared<
            VCE::TEST_SERVER::InMemoryMeetingStore>();

    VCE::TEST_SERVER::UserServiceImpl
        user_service(store);

    VCE::TEST_SERVER::MeetingServiceImpl
        meeting_service(store, media_server_ip);

    grpc::ServerBuilder builder;

    int selected_port = 0;

    builder.AddListeningPort(
        options.listen_address,
        grpc::InsecureServerCredentials(),
        &selected_port);

    builder.RegisterService(&user_service);
    builder.RegisterService(&meeting_service);

    std::unique_ptr<grpc::Server> server =
        builder.BuildAndStart();

    if (!server || selected_port == 0) {
        std::cerr
            << "Failed to start gRPC server at "
            << options.listen_address
            << std::endl;

        return EXIT_FAILURE;
    }

    std::cout
        << "========== VCE Local Test Server =========="
        << std::endl
        << "gRPC listen address: "
        << options.listen_address
        << std::endl
        << "Selected port: "
        << selected_port
        << std::endl
        << "SRS media server: "
        << options.media_server_ip
        << std::endl
        << "Storage: in-memory"
        << std::endl
        << "Press Ctrl+C to stop the server."
        << std::endl;

    /*
     * sigwait()在普通线程上下文中处理SIGINT和SIGTERM，
     * 可以安全调用grpc::Server::Shutdown()。
     */
    std::thread shutdown_thread(
        [&server, &shutdown_signals]() {
            int received_signal = 0;

            if (sigwait(
                    &shutdown_signals,
                    &received_signal) == 0) {
                std::cout
                    << "\n[TestServer] Shutdown signal received: "
                    << received_signal
                    << std::endl;

                server->Shutdown();
            }
        });

    server->Wait();

    if (shutdown_thread.joinable()) {
        shutdown_thread.join();
    }

    std::cout
        << "[TestServer] Server stopped"
        << std::endl;

    return EXIT_SUCCESS;
}