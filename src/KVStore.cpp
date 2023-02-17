#include "KVStore.h"

#include <doctest/doctest.h>

// error checked version of fwrite
// returns negative value on error, otherwise 0
static int file_write(const void* buffer, size_t size, std::FILE* file) {
    size_t n = std::fwrite(buffer, 1, size, file);
    if (n != size) {
        // error
        return errno;
    }
    return 0;
}

// error checked version of fread
// returns negative value on error, 1 if less than size was read,
// and otherwise 0
static int file_read(void* buffer, size_t size, std::FILE* file) {
    size_t n = std::fread(buffer, 1, size, file);
    if (n != size) {
        // error?
        if (std::feof(file)) {
            return 1;
        } else if (std::ferror(file)) {
            return errno;
        } else {
            return -1; // should never happen :)
        }
    }
    return 0;
}

int KVStore::write_entry_impl(const KVEntry& entry) {
    std::fseek(m_file, 0, SEEK_END);
    std::fpos_t pos;
    if (std::fgetpos(m_file, &pos) < 0) {
        return errno;
    }
    m_keydir[entry.key] = pos;
    int ret = entry.write_to_file(m_file);
    if (ret < 0) {
        return ret;
    }
    return 0;
}
int KVStore::read_entry(const std::string& key, std::vector<uint8_t>& out_value) {
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
    int ret = entry.read_from_file(m_file);
    if (ret < 0) {
        // error
        return ret;
    } else if (ret > 0) {
        return -EIO;
    }
    std::swap(out_value, entry.value);
    return 0;
}
int KVStore::write_entry(const std::string& key, const std::vector<uint8_t>& value) {
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
int KVStore::index() {
    std::unique_lock lock(m_mtx);
    std::fseek(m_file, 0, SEEK_SET);
    KVEntry entry;
    // TODO: handle errors
    fmt::print("index: collecting kv entries...\n");
    int ret = 0;
    for (;;) {
        std::fpos_t pos;
        ret = std::fgetpos(m_file, &pos);
        if (ret < 0) {
            return errno;
        }
        ret = entry.read_from_file(m_file);
        if (ret < 0) {
            // error
            fmt::print("index: error reading from file: {}\n", std::strerror(ret));
            return ret;
        } else if (ret > 0) {
            fmt::print("index: end of file\n");
            break;
        }
        m_keydir[entry.key] = pos;
    }
    fmt::print("index: collected {} kv entries\n", m_keydir.size());
    return 0;
}
int KVStore::merge() {
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
        KVStore tmp_store(temp_file.string());
        KVEntry entry;
        for (const auto& [key, pos] : m_keydir) {
            (void)key; // ignore
            ret = std::fsetpos(m_file, &pos);
            if (ret < 0) {
                return ret;
            }
            ret = entry.read_from_file(m_file);
            if (ret < 0) {
                // error
                fmt::print("merge: failed due to error reading file: {}\n", ret);
                return ret;
            } else if (ret > 0) {
                fmt::print("merge: end of file\n");
                break;
            }
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

    lock.unlock();

    ret = index();
    if (ret < 0) {
        return ret;
    }

    fmt::print("merge: merged {} entries, reduced store size from {} to {} bytes\n", entries, old_size, std::filesystem::file_size(m_filename));
    return 0;
}
KVStore::~KVStore() {
    std::unique_lock lock(m_mtx);
    std::fclose(m_file);
    m_file = nullptr;
}
KVStore::KVStore(const std::string& filename)
    : m_file(std::fopen(filename.c_str(), "a+b"))
    , m_filename(filename) {
    if (!m_file) {
        throw std::runtime_error(fmt::format("could not open file '{}': {}", filename, std::strerror(errno)));
    }
}
int KVStore::KVEntry::write_to_file(std::FILE* file) const {
    assert(key_length.value == key.size());
    assert(value_length.value == value.size());
    int ret = file_write(key_length.bytes, sizeof(key_length.bytes), file);
    if (ret != 0) {
        return ret;
    }
    ret = file_write(value_length.bytes, sizeof(value_length.bytes), file);
    if (ret != 0) {
        return ret;
    }
    ret = file_write(key.data(), key.size(), file);
    if (ret != 0) {
        return ret;
    }
    ret = file_write(value.data(), value.size(), file);
    if (ret != 0) {
        return errno;
    }
    return 0;
}
int KVStore::KVEntry::read_from_file(std::FILE* file) {
    int ret = file_read(key_length.bytes, sizeof(key_length.bytes), file);
    if (ret != 0) {
        return ret;
    }
    ret = file_read(value_length.bytes, sizeof(value_length.bytes), file);
    if (ret != 0) {
        return ret;
    }
    key.resize(key_length.value, ' ');
    ret = file_read(key.data(), key.size(), file);
    if (ret != 0) {
        return ret;
    }
    value.resize(value_length.value);
    ret = file_read(value.data(), value.size(), file);
    if (ret != 0) {
        return errno;
    }
    return 0;
}

TEST_CASE("KVStore store / load") {
    auto file = "./test-store.kvstore";
    {
        KVStore store(file);

        SUBCASE("normal string") {
            std::string msg = "hello, world";
            std::string key = "my-key";
            std::vector<uint8_t> value(msg.begin(), msg.end());

            int ret = store.write_entry(key, value);

            CHECK_EQ(ret, 0);

            std::vector<uint8_t> r_value;
            ret = store.read_entry(key, r_value);
            CHECK_EQ(ret, 0);
            CHECK(std::equal(value.begin(), value.end(), r_value.begin(), r_value.end()));
        }
        SUBCASE("normal binary") {
            std::string key = "my-key";
            std::vector<uint8_t> value = { 0, 5, 3, 134, 5, 0, 1, 0, 0 };

            int ret = store.write_entry(key, value);

            CHECK_EQ(ret, 0);

            std::vector<uint8_t> r_value;
            ret = store.read_entry(key, r_value);
            CHECK_EQ(ret, 0);
            CHECK(std::equal(value.begin(), value.end(), r_value.begin(), r_value.end()));
        }
        SUBCASE("multiple same key same value") {
            std::string key = "my-key";
            std::vector<uint8_t> value = { 0, 5, 3, 134, 5, 0, 1, 0, 0 };

            for (size_t i = 0; i < 10; ++i) {
                int ret = store.write_entry(key, value);
                CHECK_EQ(ret, 0);
            }

            std::vector<uint8_t> r_value;
            int ret = store.read_entry(key, r_value);
            CHECK_EQ(ret, 0);
            CHECK(std::equal(value.begin(), value.end(), r_value.begin(), r_value.end()));
        }
        SUBCASE("multiple same key different value") {
            std::string key = "my-key";
            std::vector<uint8_t> value = { 0, 5, 3, 134, 5, 0, 1, 0, 0 };

            for (size_t i = 0; i < 10; ++i) {
                std::vector<uint8_t> temp_value = {
                    static_cast<unsigned char>(i),
                    static_cast<unsigned char>(i * 2),
                    static_cast<unsigned char>(i * 3)
                };
                int ret = store.write_entry(key, temp_value);
                CHECK_EQ(ret, 0);
            }

            // finally write the last value, which is the one we then expect to read
            int ret = store.write_entry(key, value);
            CHECK_EQ(ret, 0);

            std::vector<uint8_t> r_value;
            ret = store.read_entry(key, r_value);
            CHECK_EQ(ret, 0);
            CHECK(std::equal(value.begin(), value.end(), r_value.begin(), r_value.end()));
        }
        SUBCASE("multiple writes, merge, read") {
            std::string key = "my-key";
            std::vector<uint8_t> value = { 0, 5, 3, 134, 5, 0, 1, 0, 0 };

            for (size_t i = 0; i < 10; ++i) {
                std::vector<uint8_t> temp_value = {
                    static_cast<unsigned char>(i),
                    static_cast<unsigned char>(i * 2),
                    static_cast<unsigned char>(i * 3)
                };
                int ret = store.write_entry(key, temp_value);
                CHECK_EQ(ret, 0);
            }

            // finally write the last value, which is the one we then expect to read
            int ret = store.write_entry(key, value);
            CHECK_EQ(ret, 0);

            store.merge();

            std::vector<uint8_t> r_value;
            ret = store.read_entry(key, r_value);
            CHECK_EQ(ret, 0);
            CHECK(std::equal(value.begin(), value.end(), r_value.begin(), r_value.end()));
        }
    }
    std::filesystem::remove(file);
}
