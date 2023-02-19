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
#include <chrono>
#include <cstdlib>
#include <fmt/chrono.h>
#include <fmt/core.h>
#include <httplib.h>

static void flood(const std::string& host, uint16_t port, const std::string& prefix, size_t max_stores, size_t max_keys) {
    fmt::print("{}: connecting to [{}]:{}\n", prefix, host, port);

    httplib::Client client(host, port);
    client.set_write_timeout(5, 0);
    client.set_read_timeout(5, 0);

    constexpr auto kv_path = "/kv/{}-{}/{}";

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

    size_t thread_count = 2; // cli arg 't'
    size_t store_count = 10; // cli arg 's'
    size_t key_count = 1000; // cli arg 'k'

    std::string host = "127.0.0.1";
    uint16_t port = 8080;

    for (int argi = 1; argi < argc;) {
        std::string arg = argv[argi];
        if (arg.size() == 2 && argc > argi + 1) {
            switch (arg[0]) {
            case '-':
                switch (arg[1]) {
                case 't': {
                    thread_count = std::strtoull(argv[argi + 1], nullptr, 10);
                    argi += 2;
                } break;
                case 's': {
                    store_count = std::strtoull(argv[argi + 1], nullptr, 10);
                    argi += 2;
                } break;
                case 'k': {
                    key_count = std::strtoull(argv[argi + 1], nullptr, 10);
                    argi += 2;
                } break;
                case 'h': {
                    host = argv[argi + 1];
                    argi += 2;
                } break;
                case 'p': {
                    port = uint16_t(std::stoi(argv[argi + 1]));
                    argi += 2;
                } break;
                default:
                    fmt::print("invalid flag: -{}, ignoring\n", arg[1]);
                    ++argi;
                }
                break;
            default:
                ++argi;
            }
        } else {
            ++argi;
        }
    }

    fmt::print("spawning {} thread(s), each querying {} store(s), {} key(s) each\n", thread_count, store_count, key_count);
    fmt::print("for a total of \n- {} per thread, or\n- {} in total\n", store_count * key_count, thread_count * store_count * key_count);

    std::vector<std::thread> threads;

    auto start = std::chrono::high_resolution_clock::now();

    for (size_t i = 0; i < thread_count; ++i) {
        threads.emplace_back(std::thread(flood, host, port, fmt::format("thread-{}", i), store_count, key_count));
    }

    for (auto& thread : threads) {
        thread.join();
    }

    auto end = std::chrono::high_resolution_clock::now();

    fmt::print("took {:%H:%M:%S}\n", end - start);
}

