#include "KVStore.h"

#include <array>
#include <doctest/doctest.h>

// error checked version of fwrite
// returns negative value on error, otherwise 0
[[nodiscard]] static int file_write(const void* buffer, size_t size, std::FILE* file) {
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
[[nodiscard]] static int file_read(void* buffer, size_t size, std::FILE* file) {
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
int KVStore::read_entry(const std::string& key, std::vector<uint8_t>& out_value, std::string& out_mime) {
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
    std::swap(out_mime, entry.mime);
    return 0;
}
int KVStore::write_entry(const std::string& key, const std::vector<uint8_t>& value, const std::string& mime) {
    KVEntry entry {
        .key_length = { .value = static_cast<uint32_t>(key.size()) },
        .value_length = { .value = static_cast<uint32_t>(value.size()) },
        .mime_length = { .value = static_cast<uint32_t>(mime.size()) },
        .key = key,
        .value = value,
        .mime = mime,
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
    int ret = std::fseek(m_file, 12, SEEK_SET);
    if (ret < 0) {
        return errno;
    }
    KVEntry entry;
    // TODO: handle errors
    fmt::print("index: collecting kv entries...\n");
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
                return errno;
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
    if (m_file) {
        std::fclose(m_file);
        m_file = nullptr;
    }
}
KVStore::KVStore(const std::string& filename) {
    bool exists = std::filesystem::exists(filename);

    std::string path;
    if (!exists) {
        path = fmt::format("store/{}", filename);
    }
    else {
        path = filename;
    }
    m_filename = path;

    if (!exists || std::filesystem::file_size(path) == 0) {
        m_file = std::fopen(fmt::format("{}.kvs",path).c_str(), "w+b");
        if (!m_file) {
            throw std::runtime_error(fmt::format("could not create file '{}': {}", filename, std::strerror(errno)));
        }
        KVHeader hdr;
        hdr.set_version(PRJ_VERSION_MAJOR, PRJ_VERSION_MINOR, PRJ_VERSION_PATCH);
        int ret = hdr.write_to_file(m_file);
        if (ret < 0) {
            throw std::runtime_error(fmt::format("could not write header into new file '{}': {}", filename, std::strerror(errno)));
        }
    } else {
        m_file = std::fopen(path.c_str(), "a+b");
        if (!m_file) {
            throw std::runtime_error(fmt::format("could not create file '{}': {}", filename, std::strerror(errno)));
        }
    }
    if (!KVHeader::is_header(m_file)) {
        fmt::print("file has no header, must be a kvstore from before v2.0.0.\n");
        // TODO: convert from old to new format
        assert(!"not implemented");
    }
    int ret = m_header.parse_from_file(m_file);
    if (ret < 0) {
        fmt::print("error: failed to parse header of kvstore: {}\n", std::strerror(ret));
        throw std::runtime_error("failed to parse header");
    }
    auto [maj, min, pat] = m_header.get_version();
    if (PRJ_VERSION_MAJOR != maj) {
        fmt::print("error: header version mismatch: {} (ours) != {} (file)\n", PRJ_VERSION_MAJOR, maj);
        // TODO: Implement porting to newer versions
        throw std::runtime_error("invalid kvstore version");
    }
    index();
}
int KVStore::KVEntry::write_to_file(std::FILE* file) const {
    assert(key_length.value == key.size());
    assert(value_length.value == value.size());
    assert(mime_length.value == mime.size());
    int ret = file_write(key_length.bytes, sizeof(key_length.bytes), file);
    if (ret != 0) {
        return ret;
    }
    ret = file_write(value_length.bytes, sizeof(value_length.bytes), file);
    if (ret != 0) {
        return ret;
    }
    ret = file_write(mime_length.bytes, sizeof(mime_length.bytes), file);
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
    ret = file_write(mime.data(), mime.size(), file);
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
    ret = file_read(mime_length.bytes, sizeof(mime_length.bytes), file);
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
    mime.resize(mime_length.value, ' ');
    ret = file_read(mime.data(), mime.size(), file);
    if (ret != 0) {
        return ret;
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
            std::string mime = "text/plain";
            std::vector<uint8_t> value(msg.begin(), msg.end());

            int ret = store.write_entry(key, value, mime);

            CHECK_EQ(ret, 0);

            std::vector<uint8_t> r_value;
            std::string r_mime;
            ret = store.read_entry(key, r_value, r_mime);
            CHECK_EQ(ret, 0);
            CHECK_EQ(mime, r_mime);
            CHECK(std::equal(value.begin(), value.end(), r_value.begin(), r_value.end()));
        }
        SUBCASE("normal binary") {
            std::string key = "my-key";
            std::string mime = "application/octet-stream";
            std::vector<uint8_t> value = { 0, 5, 3, 134, 5, 0, 1, 0, 0 };

            int ret = store.write_entry(key, value, mime);

            CHECK_EQ(ret, 0);

            std::vector<uint8_t> r_value;
            std::string r_mime;
            ret = store.read_entry(key, r_value, r_mime);
            CHECK_EQ(ret, 0);
            CHECK_EQ(mime, r_mime);
            CHECK(std::equal(value.begin(), value.end(), r_value.begin(), r_value.end()));
        }
        SUBCASE("multiple same key same value") {
            std::string key = "my-key";
            std::string mime = "application/octet-stream";
            std::vector<uint8_t> value = { 0, 5, 3, 134, 5, 0, 1, 0, 0 };

            for (size_t i = 0; i < 10; ++i) {
                int ret = store.write_entry(key, value, mime);
                CHECK_EQ(ret, 0);
            }

            std::vector<uint8_t> r_value;
            std::string r_mime;
            int ret = store.read_entry(key, r_value, r_mime);
            CHECK_EQ(ret, 0);
            CHECK_EQ(mime, r_mime);
            CHECK(std::equal(value.begin(), value.end(), r_value.begin(), r_value.end()));
        }
        SUBCASE("multiple same key different value") {
            std::string key = "my-key";
            std::string mime = "application/octet-stream";
            std::vector<uint8_t> value = { 0, 5, 3, 134, 5, 0, 1, 0, 0 };

            for (size_t i = 0; i < 10; ++i) {
                std::vector<uint8_t> temp_value = {
                    static_cast<unsigned char>(i),
                    static_cast<unsigned char>(i * 2),
                    static_cast<unsigned char>(i * 3)
                };
                int ret = store.write_entry(key, temp_value, mime);
                CHECK_EQ(ret, 0);
            }

            // finally write the last value, which is the one we then expect to read
            int ret = store.write_entry(key, value, mime);
            CHECK_EQ(ret, 0);

            std::vector<uint8_t> r_value;
            std::string r_mime;
            ret = store.read_entry(key, r_value, r_mime);
            CHECK_EQ(ret, 0);
            CHECK_EQ(mime, r_mime);
            CHECK(std::equal(value.begin(), value.end(), r_value.begin(), r_value.end()));
        }
        SUBCASE("multiple writes, merge, read") {
            std::string key = "my-key";
            std::string mime = "application/octet-stream";
            std::vector<uint8_t> value = { 0, 5, 3, 134, 5, 0, 1, 0, 0 };

            for (size_t i = 0; i < 10; ++i) {
                std::vector<uint8_t> temp_value = {
                    static_cast<unsigned char>(i),
                    static_cast<unsigned char>(i * 2),
                    static_cast<unsigned char>(i * 3)
                };
                int ret = store.write_entry(key, temp_value, mime);
                CHECK_EQ(ret, 0);
            }

            // finally write the last value, which is the one we then expect to read
            int ret = store.write_entry(key, value, mime);
            CHECK_EQ(ret, 0);

            store.merge();

            std::vector<uint8_t> r_value;
            std::string r_mime;
            ret = store.read_entry(key, r_value, r_mime);
            CHECK_EQ(ret, 0);
            CHECK_EQ(mime, r_mime);
            CHECK(std::equal(value.begin(), value.end(), r_value.begin(), r_value.end()));
        }
    }
    std::filesystem::remove(file);
}
bool KVStore::KVHeader::is_header(std::FILE* file) {
    int ret = std::fseek(file, 0, SEEK_SET);
    if (ret < 0) {
        return false;
    }
    // to read first 8 bytes into
    uint64_t zeros;
    ret = file_read(reinterpret_cast<uint8_t*>(&zeros), sizeof(zeros), file);
    if (ret != 0) {
        return false;
    }
    // check first 8 bytes are zero
    // this would mean its an invalid entry, and also the start of the kvheader
    if (zeros != 0) {
        return false;
    }
    KVSize size; // 32 bit value to read into
    ret = file_read(size.bytes, sizeof(size.bytes), file);
    if (ret != 0) {
        return false;
    }
    return true;
}

std::tuple<uint8_t, uint8_t, uint8_t> KVStore::KVHeader::get_version() const {
    return {
        version.value & 0xff,
        (version.value & 0xff00) >> 8,
        (version.value & 0xff0000) >> 16,
    };
}
int KVStore::KVHeader::parse_from_file(std::FILE* file) {
    if (!is_header(file)) {
        // shouldn't happen, as the user can / should check with is_header before trying to parse
        fmt::print("error: supplied file's header is not a kv header!\n");
        return -1;
    }
    // skip 8 bytes of zero (verified by is_header before)
    int ret = std::fseek(file, 8, SEEK_SET);
    if (ret < 0) {
        return errno;
    }
    ret = file_read(version.bytes, sizeof(version.bytes), file);
    if (ret < 0) {
        return ret;
    } else if (ret > 0) {
        fmt::print("error: supplied file is not large enough to contain a header!\n");
        return -1;
    }
    return 0;
}
void KVStore::KVHeader::set_version(uint8_t maj, uint8_t min, uint8_t pat) {
    version.value |= maj;
    version.value |= uint32_t(min << 8);
    version.value |= uint32_t(pat << 16);
}

TEST_CASE("KVHeader version") {
    KVStore::KVHeader hdr;
    hdr.set_version(120, 24, 53);
    auto [maj, min, pat] = hdr.get_version();
    CHECK_EQ(maj, 120);
    CHECK_EQ(min, 24);
    CHECK_EQ(pat, 53);
}

int KVStore::KVHeader::write_to_file(std::FILE* file) {
    int ret = std::fseek(file, 0, SEEK_SET);
    if (ret < 0) {
        return errno;
    }
    std::array<uint8_t, 8> zeroes;
    std::fill(zeroes.begin(), zeroes.end(), 0);
    ret = file_write(zeroes.data(), zeroes.size(), file);
    if (ret < 0) {
        return ret;
    }
    ret = file_write(version.bytes, sizeof(version.bytes), file);
    if (ret < 0) {
        return ret;
    }
    return 0;
}

std::vector<std::string> KVStore::get_all_keys() const {
    std::vector<std::string> result;
    for (const auto& [key, pos] : m_keydir) {
        (void)pos;
        result.push_back(key);
    }
    return result;
}

std::string KVStore::getFilename() {
    return m_filename;
}

KVStore& KVStore::operator=(KVStore&& other) {
    if (m_file) {
        std::fclose(m_file);
    }
    m_file = std::move(other.m_file);
    other.m_file = nullptr;
    m_filename = std::move(other.m_filename);
    m_header = std::move(other.m_header);
    m_keydir = std::move(other.m_keydir);
    return *this;
}
KVStore::KVStore(KVStore&& other)
    : m_file(std::move(other.m_file))
    , m_filename(std::move(other.m_filename))
    , m_header(std::move(other.m_header))
    , m_keydir(std::move(other.m_keydir)) {
    other.m_file = nullptr;
}