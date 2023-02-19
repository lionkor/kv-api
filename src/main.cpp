#include "Accept.h"
#include "KVStore.h"
#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <doctest/doctest.h>
#include <filesystem>
#include <fmt/core.h>
#include <httplib.h>
#include <mutex>
#include <nlohmann/json.hpp>
#include <string>
#include <map>
#include <unordered_map>

static httplib::Server server {};

static void sighandler(int) {
    fmt::print("Closing via SIGINT/SIGTERM\n");
    server.stop();
}

int main(int argc, const char** argv) {
    setlocale(LC_ALL, "C");

    const char* defaults[] = { argv[0], "127.0.0.1", "8080" };
    if (argc == 1) {
        argc = 3;
        argv = defaults;
    }

    fmt::print("KV API v{}.{}.{}-{}\n", PRJ_VERSION_MAJOR, PRJ_VERSION_MINOR, PRJ_VERSION_PATCH, PRJ_GIT_HASH);
    if (argc != 3) {
        fmt::print("error: not enough arguments. <host> <port> expected.\n\texample: {} 127.0.0.1 8080\n", argv[0]);
        return 1;
    }

    server.set_payload_max_length(std::numeric_limits<uint32_t>::max());

    if (!std::filesystem::exists("store")) {
        std::filesystem::create_directory("store");
    }

    std::map<std::string, KVStore> stores;
    std::filesystem::directory_iterator storePaths = std::filesystem::directory_iterator("store");
    for (const auto& storePath : storePaths) {
        KVStore store(storePath.path().string());
        stores[storePath.path().stem().string()] = std::move(store);
    }

    server.set_error_handler([&](const httplib::Request&, httplib::Response& res) {
        res.set_content(fmt::format("error {}", res.status), "text/plain");
    });

    server.Get(R"(/kv/(.+)/(.+))", [&](const httplib::Request& req, httplib::Response& res) {
        if (stores.find(req.matches[1]) == stores.end() || req.matches.length() < 3) {
            res.set_content("Not found", "text/plain");
            res.status = 404;
            return;
        }

        KVStore& store = stores[req.matches[1]];
        auto key = req.matches[2];

        std::vector<uint8_t> data;
        std::string mime;
        int ret = store.read_entry(key, data, mime);
        fmt::print("GET /{}: {}\n", key.str(), ret == 1 ? "Not found" : std::strerror(ret));
        if (ret < 0) {
            res.set_content(fmt::format("error: {}", std::strerror(ret)), "text/plain");
            res.status = 500;
        } else if (ret == 1) {
            res.set_content("Not found", "text/plain");
            res.status = 404;
        } else {
            res.set_content(reinterpret_cast<const char*>(data.data()), data.size(), mime);
        }
    });

    server.Post(R"(/kv/(.+)/(.+))", [&](const httplib::Request& req, httplib::Response& res) {
        if (req.matches.length() < 3) {
            res.set_content("Not found", "text/plain");
            res.status = 404;
            return;
        }

        if (stores.find(req.matches[1]) == stores.end()) {
            stores[req.matches[1]] = KVStore(req.matches[1]);
        }

        KVStore& store = stores[req.matches[1]];
        auto key = req.matches[2];
        std::vector<uint8_t> data;
        std::string mime = req.get_header_value("Content-Type");
        if (mime.empty()) {
            mime = "application/octet-stream";
        }
        int ret = store.write_entry(key, std::vector<uint8_t>(req.body.begin(), req.body.end()), mime);
        fmt::print("POST /{} ({}): {}\n", key.str(), mime, std::strerror(ret));
        if (ret < 0) {
            res.set_content(std::strerror(ret), "text/plain");
            res.status = 500;
        } else {
            res.set_content("OK", "text/plain");
        }
    });

    server.Get("/help", [&](const httplib::Request&, httplib::Response& res) {
        res.set_content(
#include "helptext.html"
            , "text/html");
    });

    server.Get("/merge/(.+)", [&](const httplib::Request& req, httplib::Response& res) {
        if (stores.find(req.matches[1]) == stores.end()) {
            res.set_content("Not found", "text/plain");
            res.status = 404;
            return;
        }

        KVStore& store = stores[req.matches[1]];
        auto before = std::filesystem::file_size(store.getFilename());
        int ret = store.merge();
        if (ret == 0) {
            auto after = std::filesystem::file_size(store.getFilename());
            res.set_content(fmt::format("before: {} bytes, after: {} bytes", before, after), "text/plain");
        } else {
            res.set_content(fmt::format("error: {}\n", std::strerror(ret)), "text/plain");
            res.status = 500;
        }
    });

    server.Get("/all-keys/(.+)", [&](const httplib::Request& req, httplib::Response& res) {
        if (stores.find(req.matches[1]) == stores.end()) {
            res.set_content("Not found", "text/plain");
            res.status = 404;
            return;
        }

        KVStore& store = stores[req.matches[1]];
        std::string accept = req.get_header_value("Accept");
        const std::vector<Mime> allowed_types = {
            { "application", "json" },
            { "text", "html" },
        };
        if (accept.empty()) {
            fmt::print("/all-keys requested without 'Accept' header, assuming application/json\n");
            accept = "application/json";
        } else {
            // parses and sorts
            AcceptValues values(accept);
            Mime highest = values.highest_in(allowed_types);
            if (highest.type == "*" && highest.subtype == "*") {
                fmt::print("/all-keys request has 'Accept' header, but nothing this server can provide. Sending application/json instead.\n");
                highest = allowed_types.front();
            }
            accept = highest.type + "/" + highest.subtype;
        }
        auto keys = store.get_all_keys();
        std::sort(keys.begin(), keys.end());
        if (accept == "text/html") {
            std::string html;
            std::string rows = "";

            for (const auto& key : keys) {
                rows += fmt::format(R"(<tr><td>{}</td></tr>)", key);
            }

            html = fmt::format(
                #include "all-keys.html"
            , rows);

            res.set_content(html, accept);
        } else if (accept == "application/json") {
            res.set_content(nlohmann::json(keys).dump(), accept);
        } else {
            res.status = 500;
            res.set_content("Internal server error", "text/plain");
        }
    });

    signal(SIGINT, sighandler);
    signal(SIGTERM, sighandler);

    std::string host = argv[1];
    int port = std::stoi(argv[2]);

    fmt::print("Listening on [{}]:{}\n", host, port);
    fmt::print("POST/GET to http://{}:{}/kv/<store>/<key>\n", host, port);
    fmt::print("-----------\nHow-to: http://{}:{}/help\n-----------\n", host, port);
    server.listen(host, port);
    fmt::print("Terminating gracefully\n");
}
