#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
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

public:
    KVStore(const std::string& filename)
        : m_file(std::fopen(filename.c_str(), "a+b")) {
        if (!m_file) {
            throw std::runtime_error(fmt::format("could not open file '{}': {}", filename, std::strerror(errno)));
        }
    }

    ~KVStore() {
        std::fclose(m_file);
    }

    int write_entry(const std::string& key, const std::vector<uint8_t>& value) {
        KVEntry entry {
            .key_length = { .value = static_cast<uint32_t>(key.size()) },
            .key = key,
            .value_length = { .value = static_cast<uint32_t>(value.size()) },
            .value = value,
        };
        std::unique_lock lock(m_mtx);
        std::fseek(m_file, 0, SEEK_END);
        std::fpos_t pos;
        if (std::fgetpos(m_file, &pos) < 0) {
            return errno;
        }
        m_keydir[key] = pos;
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
    std::mutex m_mtx;
    std::FILE* m_file { nullptr };

    std::unordered_map<std::string, std::fpos_t> m_keydir;
};

int main(int argc, char** argv) {
    if (argc != 3) {
        fmt::print("error: not enough arguments. <host> <port> expected.\n");
        return 1;
    }

    httplib::Server server;

    KVStore db("./server.kvstore");

    server.Get(R"(/(([a-zA-Z\d\-_])+))", [&](const httplib::Request& req, httplib::Response& res) {
        auto path = req.matches[1];
        std::vector<uint8_t> data;
        int ret = db.read_entry(path, data);
        if (ret < 0) {
            res.set_content(std::strerror(ret), "text/plain");
            res.status = 500;
        } else if (ret == 1) {
            res.set_content("Not found", "text/plain");
            res.status = 404;
        } else {
            res.set_content(reinterpret_cast<const char*>(data.data()), data.size(), "application/octet-stream");
        }
    });
    
    server.Post(R"(/(([a-zA-Z\d\-_])+))", [&](const httplib::Request& req, httplib::Response& res) {
        auto path = req.matches[1];
        std::vector<uint8_t> data;
        int ret = db.write_entry(path, std::vector<uint8_t>(req.body.begin(), req.body.end()));
        if (ret < 0) {
            res.set_content(std::strerror(ret), "text/plain");
            res.status = 500;
        } else {
            res.set_content("OK", "text/plain");
        }
    });


    server.listen(argv[1], std::stoi(argv[2]));
}

