#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <fmt/core.h>
#include <httplib.h>
#include <mutex>
#include <string>
#include <unordered_map>

class KVStore {
private:
    union KVSize {
        uint32_t value;
        uint8_t bytes[sizeof(uint32_t)];
    };
    struct KVEntry {
        KVSize key_length;
        std::string key;
        KVSize value_length;
        std::vector<uint8_t> value;
    };

    KVStore() = default;

public:
    KVStore(const std::string& filename)
        : m_file(std::fopen(filename.c_str(), "a+b"))
        , m_filename(filename) {
        if (!m_file) {
            throw std::runtime_error(fmt::format("could not open file '{}': {}", filename, std::strerror(errno)));
        }
    }

    ~KVStore() {
        std::unique_lock lock(m_mtx);
        std::fclose(m_file);
        m_file = nullptr;
    }

    int merge() {
        int ret = index();
        if (ret < 0) {
            return ret;
        }

        std::unique_lock lock(m_mtx);

        auto temp_path = std::filesystem::temp_directory_path();
        auto temp_file = temp_path / (std::filesystem::path(m_filename).filename().string() + ".kv_temporary");

        size_t n = 1;
        auto name = temp_file;
        while (std::filesystem::exists(name)) {
            name = fmt::format("{}.{}", temp_file.string(), n);
            ++n;
        }
        temp_file = name;

        fmt::print("merge: creating temporary file \"{}\"\n", temp_file.string());
        size_t entries = 0;
        {
            // temporary kv store will handle closing the file again
            KVStore tmp_store(temp_file);
            KVEntry entry;
            for (const auto& [key, pos] : m_keydir) {
                (void)key; // ignore
                std::fsetpos(m_file, &pos);
                std::fread(entry.key_length.bytes, 1, sizeof(entry.key_length.bytes), m_file);
                std::fread(entry.value_length.bytes, 1, sizeof(entry.value_length.bytes), m_file);
                entry.key.resize(entry.key_length.value, ' ');
                std::fread(entry.key.data(), 1, entry.key_length.value, m_file);
                entry.value.resize(entry.value_length.value);
                std::fread(entry.value.data(), 1, entry.value_length.value, m_file);
                tmp_store.write_entry_impl(entry);
                ++entries;
            }
            std::fflush(tmp_store.m_file);
        }
        // close the old file, move it, move the new file, remove the old one
        fmt::print("merge: closing file \"{}\"\n", m_filename);
        std::fclose(m_file);

        // get old file size
        auto old_size = std::filesystem::file_size(m_filename);

        std::string bak_file = temp_file.string() + ".bak";
        fmt::print("merge: moving file \"{}\" -> \"{}\"\n", m_filename, bak_file);
        std::filesystem::copy(m_filename, bak_file, std::filesystem::copy_options::overwrite_existing);

        fmt::print("merge: moving new file \"{}\" -> \"{}\"\n", temp_file.string(), m_filename);
        std::filesystem::copy(temp_file, m_filename, std::filesystem::copy_options::overwrite_existing);

        fmt::print("merge: opening \"{}\" as new kv store\n", m_filename);

        m_file = std::fopen(m_filename.data(), "a+b");

        // TODO: Restore old file?
        if (!m_file) {
            return errno;
        }

        if (entries != m_keydir.size()) {
            fmt::print("merge: something went wrong, maybe entries were lost. keeping the old file in the temporary directory.\n");
        } else {
            fmt::print("merge: removing old file \"{}\"\n", bak_file);
            std::filesystem::remove(bak_file);
            fmt::print("merge: removing old file \"{}\"\n", temp_file.string());
            std::filesystem::remove(temp_file);
        }

        fmt::print("merge: merged {} entries, reduced store size from {} to {} bytes\n", entries, old_size, std::filesystem::file_size(m_filename));
        return 0;
    }

    int index() {
        std::unique_lock lock(m_mtx);
        std::fseek(m_file, 0, SEEK_SET);
        KVEntry entry;
        // TODO: handle errors
        fmt::print("index: collecting kv entries...\n");
        int ret = 0;
        size_t n = 0;
        for (;;) {
            std::fpos_t pos;
            ret = std::fgetpos(m_file, &pos);
            if (ret < 0) {
                return errno;
            }
            n = std::fread(entry.key_length.bytes, 1, sizeof(entry.key_length.bytes), m_file);
            if (n == 0) {
                break;
            }
            n = std::fread(entry.value_length.bytes, 1, sizeof(entry.value_length.bytes), m_file);
            if (n == 0) {
                break;
            }
            entry.key.resize(entry.key_length.value, ' ');
            n = std::fread(entry.key.data(), 1, entry.key_length.value, m_file);
            if (n == 0) {
                break;
            }
            // skip value
            ret = std::fseek(m_file, entry.value_length.value, SEEK_CUR);
            if (ret != 0) {
                break;
            }
            m_keydir[entry.key] = pos;
        }
        fmt::print("index: collected {} kv entries\n", m_keydir.size());
        return 0;
    }

    int write_entry(const std::string& key, const std::vector<uint8_t>& value) {
        KVEntry entry {
            .key_length = { .value = static_cast<uint32_t>(key.size()) },
            .key = key,
            .value_length = { .value = static_cast<uint32_t>(value.size()) },
            .value = value,
        };
        std::unique_lock lock(m_mtx);
        int ret = write_entry_impl(entry);
        if (ret < 0) {
            return ret;
        }
        // flush to make sure it's saved
        std::fflush(m_file);
        return 0;
    }

    // returns -1 on error, 0 on found and read, and 1 on not found
    int read_entry(const std::string& key, std::vector<uint8_t>& out_value) {
        KVEntry entry;
        std::unique_lock lock(m_mtx);
        if (m_keydir.contains(key)) {
            std::fpos_t pos = m_keydir.at(key);
            if (std::fsetpos(m_file, &pos) < 0) {
                return errno;
            }
        } else {
            return 1;
        }
        if (std::fread(entry.key_length.bytes, 1, sizeof(entry.key_length.bytes), m_file) < 0) {
            return errno;
        }
        if (std::fread(entry.value_length.bytes, 1, sizeof(entry.value_length.bytes), m_file) < 0) {
            return errno;
        }
        // skip reading key
        if (std::fseek(m_file, entry.key_length.value, SEEK_CUR) < 0) {
            return errno;
        }
        out_value.resize(entry.value_length.value);
        if (std::fread(out_value.data(), 1, out_value.size(), m_file) < 0) {
            return errno;
        }
        return 0;
    }

private:
    int write_entry_impl(const KVEntry& entry) {
        std::fseek(m_file, 0, SEEK_END);
        std::fpos_t pos;
        if (std::fgetpos(m_file, &pos) < 0) {
            return errno;
        }
        m_keydir[entry.key] = pos;
        // TODO: Consolidate this into one fwrite call
        if (std::fwrite(entry.key_length.bytes, 1, sizeof(entry.key_length.bytes), m_file) < 0) {
            return errno;
        }
        if (std::fwrite(entry.value_length.bytes, 1, sizeof(entry.value_length.bytes), m_file) < 0) {
            return errno;
        }
        if (std::fwrite(entry.key.data(), 1, entry.key.size(), m_file) < 0) {
            return errno;
        }
        if (std::fwrite(entry.value.data(), 1, entry.value.size(), m_file) < 0) {
            return errno;
        }
        return 0;
    }

    std::mutex m_mtx;
    std::FILE* m_file { nullptr };
    std::string m_filename;

    std::unordered_map<std::string, std::fpos_t> m_keydir;
};

static httplib::Server server {};

static void sighandler(int) {
    fmt::print("Closing via SIGINT/SIGTERM\n");
    server.stop();
}

int main(int argc, char** argv) {
    setlocale(LC_ALL, "C");

    fmt::print("KV API v{}.{}.{}-{}\n", PRJ_VERSION_MAJOR, PRJ_VERSION_MINOR, PRJ_VERSION_PATCH, PRJ_GIT_HASH);
    if (argc != 4) {
        fmt::print("error: not enough arguments. <host> <port> <storefile> expected.\n");
        return 1;
    }

    std::string storefile = argv[3];

    server.set_payload_max_length(std::numeric_limits<uint32_t>::max());

    bool is_new = true;

    if (std::filesystem::exists(storefile)) {
        fmt::print("Storefile \"{}\" already exists. KV will index existing content.\n", storefile);
        is_new = false;
    }

    KVStore kv(storefile);

    if (!is_new) {
        fmt::print("Merging storefile to save space...\n");
        kv.merge();
        fmt::print("Done\n");
    }

    server.set_error_handler([&](const httplib::Request&, httplib::Response& res) {
        res.set_content(fmt::format("error {}", res.status), "text/plain");
    });

    server.Get(R"(/kv/(([a-zA-Z\d\-_])+))", [&](const httplib::Request& req, httplib::Response& res) {
        auto path = req.matches[1];
        std::vector<uint8_t> data;
        int ret = kv.read_entry(path, data);
        fmt::print("GET /{}: {}\n", path.str(), ret == 1 ? "Not found" : std::strerror(ret));
        if (ret < 0) {
            res.set_content(fmt::format("error: {}", std::strerror(ret)), "text/plain");
            res.status = 500;
        } else if (ret == 1) {
            res.set_content("Not found", "text/plain");
            res.status = 404;
        } else {
            res.set_content(reinterpret_cast<const char*>(data.data()), data.size(), "application/octet-stream");
        }
    });

    server.Post(R"(/kv/(([a-zA-Z\d\-_])+))", [&](const httplib::Request& req, httplib::Response& res) {
        auto path = req.matches[1];
        std::vector<uint8_t> data;
        int ret = kv.write_entry(path, std::vector<uint8_t>(req.body.begin(), req.body.end()));
        fmt::print("POST /{}: {}\n", path.str(), std::strerror(ret));
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

