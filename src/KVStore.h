#pragma once

#include <cassert>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fmt/core.h>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

class KVStore {
private:
    union KVSize {
        uint32_t value;
        uint8_t bytes[sizeof(uint32_t)];
    };
    // first 8 bytes are zero
    // and must be the first thing in the file
    struct KVEntry {
        KVSize key_length;
        KVSize value_length;
        KVSize mime_length;
        std::string key;
        std::vector<uint8_t> value;
        std::string mime;

        int read_from_file(std::FILE* file);

        int write_to_file(std::FILE* file) const;
    };

public:
    KVStore() = default;
    struct KVHeader {
        KVSize version = { .value = 0 };

        void set_version(uint8_t maj, uint8_t min, uint8_t pat);
        int write_to_file(std::FILE* file);
        static bool is_header(std::FILE* file);
        int parse_from_file(std::FILE* file);
        std::tuple<uint8_t, uint8_t, uint8_t> get_version() const;
    };

    KVStore(const std::string& filename);

    KVStore(KVStore&& other);

    KVStore& operator=(KVStore&& other);

    KVStore& operator=(const KVStore&) = delete;
    KVStore(const KVStore&) = delete;

    ~KVStore();

    int merge();

    int index();

    int write_entry(const std::string& key, const std::vector<uint8_t>& value, const std::string& mime);

    // returns -1 on error, 0 on found and read, and 1 on not found
    int read_entry(const std::string& key, std::vector<uint8_t>& out_value, std::string& out_mime);

    std::vector<std::string> get_all_keys() const;

    std::string getFilename();

private:
    int write_entry_impl(const KVEntry& entry);

    std::mutex m_mtx;
    std::FILE* m_file { nullptr };
    std::string m_filename;

    KVHeader m_header;

    std::unordered_map<std::string, std::fpos_t> m_keydir;
};

