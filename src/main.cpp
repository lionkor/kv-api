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
#include <unordered_map>

static httplib::Server server {};

static void sighandler(int) {
    fmt::print("Closing via SIGINT/SIGTERM\n");
    server.stop();
}

int main(int argc, const char** argv) {
    setlocale(LC_ALL, "C");

    const char* defaults[] = { argv[0], "127.0.0.1", "8080", "default.kvs" };
    if (argc == 1) {
        argc = 4;
        argv = defaults;
    }

    fmt::print("KV API v{}.{}.{}-{}\n", PRJ_VERSION_MAJOR, PRJ_VERSION_MINOR, PRJ_VERSION_PATCH, PRJ_GIT_HASH);
    if (argc != 4) {
        fmt::print("error: not enough arguments. <host> <port> <storefile> expected.\n\texample: {} 127.0.0.1 8080 mystore.kvs\n", argv[0]);
        return 1;
    }

    std::string storefile = argv[3];

    server.set_payload_max_length(std::numeric_limits<uint32_t>::max());

    bool is_new = true;

    if (std::filesystem::exists("store/"+storefile)) {
        fmt::print("Storefile \"{}\" already exists. KV will index existing content.\n", storefile);
        is_new = false;
    }

    KVStore kv(storefile);

    if (!is_new) {
        fmt::print("Merging storefile to save space...\n");
        int ret = kv.merge();
        if (ret < 0) {
            fmt::print("Merge failed: {}\n", std::strerror(ret));
        } else {
            fmt::print("Done\n");
        }
    }

    server.set_error_handler([&](const httplib::Request&, httplib::Response& res) {
        res.set_content(fmt::format("error {}", res.status), "text/plain");
    });

    server.Get(R"(/kv/(.+))", [&](const httplib::Request& req, httplib::Response& res) {
        auto path = req.matches[1];
        std::vector<uint8_t> data;
        std::string mime;
        int ret = kv.read_entry(path, data, mime);
        fmt::print("GET /{}: {}\n", path.str(), ret == 1 ? "Not found" : std::strerror(ret));
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

    server.Post(R"(/kv/(.+))", [&](const httplib::Request& req, httplib::Response& res) {
        auto path = req.matches[1];
        std::vector<uint8_t> data;
        std::string mime = req.get_header_value("Content-Type");
        if (mime.empty()) {
            mime = "application/octet-stream";
        }
        int ret = kv.write_entry(path, std::vector<uint8_t>(req.body.begin(), req.body.end()), mime);
        fmt::print("POST /{} ({}): {}\n", path.str(), mime, std::strerror(ret));
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

    server.Get("/merge", [&](const httplib::Request&, httplib::Response& res) {
        auto before = std::filesystem::file_size(storefile);
        int ret = kv.merge();
        if (ret == 0) {
            auto after = std::filesystem::file_size(storefile);
            res.set_content(fmt::format("before: {} bytes, after: {} bytes", before, after), "text/plain");
        } else {
            res.set_content(fmt::format("error: {}\n", std::strerror(ret)), "text/plain");
            res.status = 500;
        }
    });

    server.Get("/all-keys", [&](const httplib::Request& req, httplib::Response& res) {
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
        auto keys = kv.get_all_keys();
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
    fmt::print("POST/GET to http://{}:{}/kv/<key>\n", host, port);
    fmt::print("-----------\nHow-to: http://{}:{}/help\n-----------\n", host, port);
    server.listen(host, port);
    fmt::print("Terminating gracefully\n");
}
