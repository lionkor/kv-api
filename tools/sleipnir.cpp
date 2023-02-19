/**
 * Sleipnir is the testing tool to benchmark speed and throughput of
 * kv-api. The name is from norse mythology, so please don't ask me
 * how to pronounce it - I don't know either.
 *
 * How to use:
 *
 * TODO
 */

#include <algorithm>
#include <fmt/core.h>
#include <httplib.h>

static void flood(const std::string& host, uint16_t port, const std::string& prefix) {
    fmt::print("{}: connecting to [{}]:{}\n", prefix, host, port);

    httplib::Client client(host, port);
    client.set_write_timeout(5, 0);
    client.set_read_timeout(5, 0);

    constexpr auto kv_path = "/kv/{}-{}/{}";

    size_t max_stores = 10;
    size_t max_keys = 10000;

    std::string body = "HELLO WORLD";

    size_t ok = 0;
    size_t error = 0;
    for (size_t i = 0; i < max_stores; ++i) {
        for (size_t k = 0; k < max_keys; ++k) {
            auto result = client.Post(fmt::format(kv_path, prefix, i, k), body, "text/plain");
            if (result.error() != httplib::Error::Success) {
                fmt::print("{}: error in /kv/{}-{}/{}: {}\n", prefix, prefix, i, k, httplib::to_string(result.error()));
                ++error;
            } else {
                ++ok;
            }
        }
    }

    fmt::print("{}: {} requests, {} were ok and {} errored\n", prefix, ok + error, ok, error);
}

int main(int argc, const char** argv) {
    setlocale(LC_ALL, "C");

    const char* defaults[] = { argv[0], "127.0.0.1", "8080" };
    if (argc == 1) {
        argc = 3;
        argv = defaults;
    }

    fmt::print("SLEIPNIR for KV API v{}.{}.{}-{}\n", PRJ_VERSION_MAJOR, PRJ_VERSION_MINOR, PRJ_VERSION_PATCH, PRJ_GIT_HASH);
    if (argc != 3) {
        fmt::print("error: not enough arguments. <host> <port> expected.\n\texample: {} 127.0.0.1 8080\n", argv[0]);
        return 1;
    }

    std::string host = argv[1];
    int port = std::stoi(argv[2]);

    std::vector<std::thread> threads;

    size_t thread_count = 2;

    for (size_t i = 0; i < thread_count; ++i) {
        threads.emplace_back(std::thread(flood, host, port, fmt::format("thread-{}", i)));
    }

    for (auto& thread : threads) {
        thread.join();
    }
}

