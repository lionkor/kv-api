#pragma once

#include <cassert>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fmt/core.h>
#include <vector>
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

        int read_from_file(std::FILE* file);

        int write_to_file(std::FILE* file) const;
    };

    KVStore() = default;

public:
    KVStore(const std::string& filename);

    ~KVStore();

    int merge();

    int index();

    int write_entry(const std::string& key, const std::vector<uint8_t>& value);

    // returns -1 on error, 0 on found and read, and 1 on not found
    int read_entry(const std::string& key, std::vector<uint8_t>& out_value);

private:
    int write_entry_impl(const KVEntry& entry);

    std::mutex m_mtx;
    std::FILE* m_file { nullptr };
    std::string m_filename;

    std::unordered_map<std::string, std::fpos_t> m_keydir;
};
