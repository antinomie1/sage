module;

#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <stdio.h>
#include <cerrno>
#include <cstring>
#include <zstd.h>

export module sage.archive;

import std;
import sage.vendor.zstd;
import sage.package;
import sage.service;
import sage.util;

export namespace sage::archive {

using std::size_t;
using std::uint8_t;
using std::uint32_t;
using std::uint64_t;

// ============================================================================
// POSIX USTAR Tar Header (512 bytes)
// ============================================================================

#pragma pack(push, 1)
struct TarHeader {
    char name[100];
    char mode[8];
    char uid[8];
    char gid[8];
    char size[12];
    char mtime[12];
    char chksum[8];
    char typeflag;
    char linkname[100];
    char magic[6];
    char version[2];
    char uname[32];
    char gname[32];
    char devmajor[8];
    char devminor[8];
    char prefix[155];
    char padding[12];
};
#pragma pack(pop)

static_assert(sizeof(TarHeader) == 512, "TarHeader must be exactly 512 bytes");

inline uint64_t parse_octal(const char* str, size_t len) noexcept {
    uint64_t val = 0;
    size_t i = 0;
    while (i < len && (str[i] == ' ' || str[i] == '\0')) ++i;
    while (i < len && str[i] >= '0' && str[i] <= '7') {
        val = (val << 3) | (str[i] - '0');
        ++i;
    }
    return val;
}

inline void write_octal(char* dest, size_t len, uint64_t val) noexcept {
    if (len == 0) return;
    dest[len - 1] = '\0';
    size_t i = len - 1;
    if (val == 0) {
        if (i > 0) dest[--i] = '0';
    } else {
        while (i > 0 && val > 0) {
            dest[--i] = static_cast<char>('0' + (val & 7));
            val >>= 3;
        }
    }
    while (i > 0) {
        dest[--i] = '0';
    }
}

inline void write_tar_checksum(char* dest, uint32_t val) noexcept {
    for (size_t i = 6; i > 0; --i) {
        dest[i - 1] = static_cast<char>('0' + (val & 7));
        val >>= 3;
    }
    dest[6] = '\0';
    dest[7] = ' ';
}

struct UstarPathParts {
    std::string_view prefix;
    std::string_view name;
};

inline std::expected<UstarPathParts, std::string> split_ustar_path(std::string_view path) {
    if (path.size() <= 100) {
        return UstarPathParts{{}, path};
    }

    size_t slash = path.rfind('/', std::min(path.size() - 1, size_t{155}));
    while (slash != std::string_view::npos) {
        size_t name_size = path.size() - slash - 1;
        if (slash > 0 && name_size > 0 && name_size <= 100) {
            return UstarPathParts{path.substr(0, slash), path.substr(slash + 1)};
        }
        if (slash == 0) break;
        slash = path.rfind('/', slash - 1);
    }

    return std::unexpected("Path cannot be represented in a POSIX USTAR header: " + std::string(path));
}

inline uint32_t compute_tar_checksum(const TarHeader& hdr) noexcept {
    const auto* p = reinterpret_cast<const uint8_t*>(&hdr);
    uint32_t sum = 0;
    for (size_t i = 0; i < 512; ++i) {
        // Tar chksum field (bytes 148..155) treated as ASCII spaces during calculation
        if (i >= 148 && i < 156) {
            sum += ' ';
        } else {
            sum += p[i];
        }
    }
    return sum;
}

// ============================================================================
// .METADATA/files.idx -- per-file integrity index
// ============================================================================
//
// Tab-separated, path last but one so a path containing whitespace still
// parses: type, mode (octal), size, sha256 ("-" when not applicable), path,
// symlink target ("-" when not a symlink).

inline std::string serialize_files_idx(const std::vector<package::FileEntry>& files) {
    std::ostringstream ss;
    ss << "# sage files index v1\n";
    ss << "# type\tmode\tsize\tsha256\tpath\ttarget\n";
    for (const auto& f : files) {
        ss << package::to_string(f.type) << '\t'
           << std::format("{:o}", f.mode) << '\t'
           << f.size << '\t'
           << (f.sha256.empty() ? "-" : f.sha256) << '\t'
           << f.path << '\t'
           << (f.link_target.empty() ? "-" : f.link_target) << '\n';
    }
    return ss.str();
}

inline std::vector<package::FileEntry> parse_files_idx(std::string_view content) {
    std::vector<package::FileEntry> out;
    size_t pos = 0;
    while (pos < content.size()) {
        size_t eol = content.find('\n', pos);
        if (eol == std::string_view::npos) eol = content.size();
        std::string_view line = content.substr(pos, eol - pos);
        pos = eol + 1;
        if (line.empty() || line.front() == '#') continue;

        std::array<std::string_view, 6> fields{};
        size_t fpos = 0;
        size_t idx = 0;
        for (; idx < fields.size(); ++idx) {
            if (idx + 1 == fields.size()) {
                fields[idx] = line.substr(fpos);
                break;
            }
            size_t tab = line.find('\t', fpos);
            if (tab == std::string_view::npos) break;
            fields[idx] = line.substr(fpos, tab - fpos);
            fpos = tab + 1;
        }
        if (idx + 1 != fields.size()) continue; // malformed line

        package::FileEntry fe;
        fe.type = package::parse_file_type(fields[0]);
        fe.mode = static_cast<uint32_t>(parse_octal(fields[1].data(), fields[1].size()));
        fe.size = 0;
        for (char c : fields[2]) {
            if (c >= '0' && c <= '9') fe.size = fe.size * 10 + static_cast<uint64_t>(c - '0');
        }
        if (fields[3] != "-") fe.sha256 = std::string(fields[3]);
        fe.path = std::string(fields[4]);
        if (fields[5] != "-") fe.link_target = std::string(fields[5]);
        if (!fe.path.empty()) out.push_back(std::move(fe));
    }
    return out;
}

// ============================================================================
// Extracted Package Result
// ============================================================================

struct ExtractedPackage {
    package::PackageManifest manifest;
    std::optional<service::ServiceSpec> service;
    std::vector<package::FileEntry> extracted_files;
    // What .METADATA/files.idx claimed, when the archive shipped one.
    std::vector<package::FileEntry> declared_files;
};

struct InspectedPackage {
    package::PackageManifest manifest;
    std::optional<service::ServiceSpec> service;
    std::vector<package::FileEntry> data_files;
    std::vector<package::FileEntry> declared_files;
    std::string archive_sha256;
};

struct ArchiveEntryView {
    std::string name;
    uint64_t size{0};
    uint32_t mode{0};
    char typeflag{'0'};
    std::string linkname;
    std::span<const uint8_t> payload;
};

template <typename Handler>
inline std::expected<void, std::string> walk_archive_entries(
    std::istream& file,
    std::string_view archive_name,
    Handler&& handler,
    util::Sha256* archive_hasher = nullptr)
{
    vendor::zstd::ZstdDecompressStream zstd_stream;
    if (!zstd_stream) {
        return std::unexpected("Failed to initialize ZSTD decompressor");
    }

    constexpr size_t in_buf_size = 64 * 1024;
    constexpr size_t out_buf_size = 128 * 1024;

    std::vector<uint8_t> in_buffer(in_buf_size);
    std::vector<uint8_t> out_buffer(out_buf_size);

    std::vector<uint8_t> ring;
    ring.reserve(256 * 1024);
    size_t end_zero_blocks = 0;
    bool frame_finished = false;

    auto process_decompressed_bytes = [&](std::span<const uint8_t> chunk) -> std::expected<void, std::string> {
        ring.insert(ring.end(), chunk.begin(), chunk.end());

        while (ring.size() >= 512) {
            const auto* hdr = reinterpret_cast<const TarHeader*>(ring.data());

            // End of archive marker (all zero block)
            bool all_zero = true;
            for (size_t i = 0; i < 512; ++i) {
                if (ring[i] != 0) { all_zero = false; break; }
            }
            if (all_zero) {
                ++end_zero_blocks;
                ring.erase(ring.begin(), ring.begin() + 512);
                continue;
            }
            if (end_zero_blocks > 0) {
                return std::unexpected("Tar archive contains data after its end marker");
            }

            // Verify checksum
            uint32_t expected_chk = static_cast<uint32_t>(parse_octal(hdr->chksum, sizeof(hdr->chksum)));
            uint32_t actual_chk = compute_tar_checksum(*hdr);
            if (expected_chk != actual_chk) {
                return std::unexpected("Tar header checksum mismatch");
            }

            std::string full_name;
            if (hdr->prefix[0] != '\0') {
                full_name = std::string(hdr->prefix, strnlen(hdr->prefix, sizeof(hdr->prefix))) + "/";
            }
            full_name += std::string(hdr->name, strnlen(hdr->name, sizeof(hdr->name)));

            uint64_t file_size = parse_octal(hdr->size, sizeof(hdr->size));
            uint32_t mode = static_cast<uint32_t>(parse_octal(hdr->mode, sizeof(hdr->mode)));
            char typeflag = hdr->typeflag ? hdr->typeflag : '0';
            std::string linkname(hdr->linkname, strnlen(hdr->linkname, sizeof(hdr->linkname)));

            size_t total_entry_size = 512 + ((file_size + 511) / 512) * 512;
            if (ring.size() < total_entry_size) {
                // Wait for more decompressed stream data
                return {};
            }

            ArchiveEntryView entry{
                .name = std::move(full_name),
                .size = file_size,
                .mode = mode,
                .typeflag = typeflag,
                .linkname = std::move(linkname),
                .payload = std::span<const uint8_t>(ring.data() + 512, file_size),
            };
            auto handle_res = handler(entry);
            if (!handle_res) return handle_res;

            ring.erase(ring.begin(), ring.begin() + total_entry_size);
        }
        return {};
    };

    while (file.read(reinterpret_cast<char*>(in_buffer.data()), in_buf_size) || file.gcount() > 0) {
        size_t read_bytes = static_cast<size_t>(file.gcount());
        if (archive_hasher && read_bytes > 0) {
            archive_hasher->update(in_buffer.data(), read_bytes);
        }
        ZSTD_inBuffer in = { in_buffer.data(), read_bytes, 0 };

        while (in.pos < in.size) {
            ZSTD_outBuffer out = { out_buffer.data(), out_buffer.size(), 0 };
            auto dec_res = zstd_stream.decompress_stream(in, out);
            if (!dec_res) return std::unexpected(dec_res.error());
            frame_finished = *dec_res == 0;

            if (out.pos > 0) {
                auto proc_res = process_decompressed_bytes(std::span<const uint8_t>(out_buffer.data(), out.pos));
                if (!proc_res) return std::unexpected(proc_res.error());
            }
        }
    }

    if (file.bad()) {
        return std::unexpected("Failed while reading package archive: " + std::string(archive_name));
    }
    if (!frame_finished) {
        return std::unexpected("Truncated ZSTD package archive: " + std::string(archive_name));
    }
    if (!ring.empty()) {
        return std::unexpected("Truncated tar block in package archive: " + std::string(archive_name));
    }
    if (end_zero_blocks < 2) {
        return std::unexpected("Tar archive is missing its end marker");
    }

    return {};
}

template <typename Handler>
inline std::expected<void, std::string> walk_archive_entries(
    const std::filesystem::path& archive_path,
    Handler&& handler)
{
    std::ifstream file(archive_path, std::ios::binary);
    if (!file.is_open()) {
        return std::unexpected("Cannot open package archive: " + archive_path.string());
    }
    return walk_archive_entries(file, archive_path.string(), std::forward<Handler>(handler));
}

struct PackageDataEntry {
    std::string path;
    uint64_t size{0};
    uint32_t mode{0};
    char typeflag{'0'};
    std::string link_target;
    std::string sha256;
};

class UniqueFd {
public:
    explicit UniqueFd(int fd = -1) noexcept : fd_(fd) {}
    UniqueFd(const UniqueFd&) = delete;
    UniqueFd& operator=(const UniqueFd&) = delete;

    UniqueFd(UniqueFd&& other) noexcept
        : fd_(std::exchange(other.fd_, -1)) {}

    UniqueFd& operator=(UniqueFd&& other) noexcept {
        if (this != &other) {
            reset();
            fd_ = std::exchange(other.fd_, -1);
        }
        return *this;
    }

    ~UniqueFd() noexcept { reset(); }

    [[nodiscard]] int get() const noexcept { return fd_; }

private:
    void reset() noexcept {
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
    }

    int fd_{-1};
};

struct AnchoredPath {
    UniqueFd directory;
    std::string leaf;
};

inline std::expected<AnchoredPath, std::string> open_anchored_parent(
    int root_fd,
    const std::filesystem::path& relative)
{
    int duplicate = ::fcntl(root_fd, F_DUPFD_CLOEXEC, 0);
    if (duplicate < 0) {
        return std::unexpected(
            "Cannot duplicate target-root directory: " + std::string(std::strerror(errno)));
    }
    UniqueFd current(duplicate);

    for (const auto& component : relative.parent_path()) {
        if (component == ".") continue;
        const auto name = component.string();
        int flags = O_RDONLY | O_DIRECTORY | O_NOFOLLOW;
#ifdef O_CLOEXEC
        flags |= O_CLOEXEC;
#endif
        int next = ::openat(current.get(), name.c_str(), flags);
        if (next < 0 && errno == ENOENT) {
            if (::mkdirat(current.get(), name.c_str(), 0755) != 0 && errno != EEXIST) {
                return std::unexpected(std::format(
                    "Cannot create parent directory '{}': {}", name, std::strerror(errno)));
            }
            next = ::openat(current.get(), name.c_str(), flags);
        }
        if (next < 0) {
            return std::unexpected(std::format(
                "Cannot securely open parent directory '{}': {}", name, std::strerror(errno)));
        }
        current = UniqueFd(next);
    }

    return AnchoredPath{
        .directory = std::move(current),
        .leaf = relative.filename().string(),
    };
}

inline std::expected<void, std::string> ensure_anchored_directory(
    int parent_fd,
    std::string_view leaf)
{
    const auto name = std::string(leaf);
    if (::mkdirat(parent_fd, name.c_str(), 0755) == 0) return {};
    if (errno != EEXIST) {
        return std::unexpected(std::strerror(errno));
    }

    struct stat status {};
    if (::fstatat(parent_fd, name.c_str(), &status, AT_SYMLINK_NOFOLLOW) != 0) {
        return std::unexpected(std::strerror(errno));
    }
    if (!S_ISDIR(status.st_mode)) {
        return std::unexpected("existing path is not a directory");
    }
    return {};
}

inline std::expected<void, std::string> remove_anchored_leaf(
    int parent_fd,
    std::string_view leaf)
{
    const auto name = std::string(leaf);
    struct stat status {};
    if (::fstatat(parent_fd, name.c_str(), &status, AT_SYMLINK_NOFOLLOW) != 0) {
        if (errno == ENOENT) return {};
        return std::unexpected(std::strerror(errno));
    }

    const int flags = S_ISDIR(status.st_mode) ? AT_REMOVEDIR : 0;
    if (::unlinkat(parent_fd, name.c_str(), flags) != 0) {
        return std::unexpected(std::strerror(errno));
    }
    return {};
}

class UniqueTempFile {
public:
    UniqueTempFile(const UniqueTempFile&) = delete;
    UniqueTempFile& operator=(const UniqueTempFile&) = delete;

    UniqueTempFile(UniqueTempFile&& other) noexcept
        : directory_(std::move(other.directory_)),
          fd_(std::exchange(other.fd_, -1)),
          name_(std::exchange(other.name_, {})) {}

    UniqueTempFile& operator=(UniqueTempFile&& other) noexcept {
        if (this != &other) {
            cleanup();
            directory_ = std::move(other.directory_);
            fd_ = std::exchange(other.fd_, -1);
            name_ = std::exchange(other.name_, {});
        }
        return *this;
    }

    ~UniqueTempFile() noexcept { cleanup(); }

    static std::expected<UniqueTempFile, std::string> create(UniqueFd directory)
    {
        static std::atomic<uint64_t> sequence{0};
        for (size_t attempt = 0; attempt < 128; ++attempt) {
            auto name = std::format(
                ".sage-tmp-{:x}-{:x}-{:x}",
                static_cast<uint64_t>(::getpid()),
                sequence.fetch_add(1, std::memory_order_relaxed),
                static_cast<uint64_t>(std::random_device{}()));
            int flags = O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW;
#ifdef O_CLOEXEC
            flags |= O_CLOEXEC;
#endif
            int fd = ::openat(directory.get(), name.c_str(), flags, 0600);
            if (fd >= 0) {
                return UniqueTempFile(std::move(directory), fd, std::move(name));
            }
            if (errno != EEXIST) {
                return std::unexpected(std::format(
                    "Cannot create temporary file: {}", std::strerror(errno)));
            }
        }
        return std::unexpected("Cannot create a unique temporary file after repeated collisions");
    }

    std::expected<void, std::string> write_all(std::span<const uint8_t> payload) {
        size_t written = 0;
        while (written < payload.size()) {
            ssize_t count = ::write(fd_, payload.data() + written, payload.size() - written);
            if (count < 0 && errno == EINTR) continue;
            if (count <= 0) {
                return std::unexpected(std::strerror(errno));
            }
            written += static_cast<size_t>(count);
        }
        return {};
    }

    std::expected<void, std::string> install(
        std::string_view destination,
        uint32_t mode)
    {
        if (::fchmod(fd_, mode ? mode : 0644) != 0) {
            return std::unexpected(std::format(
                "Cannot set mode: {}", std::strerror(errno)));
        }
        if (::close(fd_) != 0) {
            auto message = std::string(std::strerror(errno));
            fd_ = -1;
            return std::unexpected("Cannot close temporary file: " + message);
        }
        fd_ = -1;
        auto destination_name = std::string(destination);
        if (::renameat(
                directory_.get(), name_.c_str(), directory_.get(), destination_name.c_str()) != 0) {
            return std::unexpected(
                "Cannot atomically install temporary file: " + std::string(std::strerror(errno)));
        }
        name_.clear();
        return {};
    }

private:
    UniqueTempFile(UniqueFd directory, int fd, std::string name)
        : directory_(std::move(directory)), fd_(fd), name_(std::move(name)) {}

    void cleanup() noexcept {
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
        if (directory_.get() >= 0 && !name_.empty()) {
            (void)::unlinkat(directory_.get(), name_.c_str(), 0);
            name_.clear();
        }
    }

    UniqueFd directory_;
    int fd_{-1};
    std::string name_;
};

inline constexpr std::string_view temp_file_prefix = ".sage-tmp-";

inline std::expected<std::string, std::string> normalize_data_path(std::string_view raw_path) {
    std::filesystem::path path(raw_path);
    if (path.is_absolute() || path.has_root_name() || path.has_root_directory()) {
        return std::unexpected("Package data path must be relative: " + std::string(raw_path));
    }
    for (const auto& component : path) {
        if (component == "..") {
            return std::unexpected("Package data path escapes the target root: " + std::string(raw_path));
        }
    }

    auto normalized = path.lexically_normal();
    if (normalized.empty() || normalized == ".") {
        return std::unexpected("Package data path is empty");
    }
    return normalized.generic_string();
}

inline std::expected<void, std::string> remove_path_anchored(
    const std::filesystem::path& target_root,
    std::string_view raw_path,
    bool ignore_nonempty_directory = false)
{
    auto normalized = normalize_data_path(raw_path);
    if (!normalized) return std::unexpected(normalized.error());

    int root_flags = O_RDONLY | O_DIRECTORY | O_NOFOLLOW;
#ifdef O_CLOEXEC
    root_flags |= O_CLOEXEC;
#endif
    int root_fd = ::open(target_root.c_str(), root_flags);
    if (root_fd < 0) {
        return std::unexpected(
            "Cannot securely open target root: " + std::string(std::strerror(errno)));
    }
    UniqueFd current(root_fd);
    const auto relative = std::filesystem::path(*normalized);
    for (const auto& component : relative.parent_path()) {
        if (component == ".") continue;
        const auto name = component.string();
        int flags = O_RDONLY | O_DIRECTORY | O_NOFOLLOW;
#ifdef O_CLOEXEC
        flags |= O_CLOEXEC;
#endif
        int next = ::openat(current.get(), name.c_str(), flags);
        if (next < 0 && errno == ENOENT) return {};
        if (next < 0) {
            return std::unexpected(std::format(
                "Cannot securely open parent directory '{}': {}",
                name, std::strerror(errno)));
        }
        current = UniqueFd(next);
    }

    const auto leaf = relative.filename().string();
    struct stat status {};
    if (::fstatat(current.get(), leaf.c_str(), &status, AT_SYMLINK_NOFOLLOW) != 0) {
        if (errno == ENOENT) return {};
        return std::unexpected(std::strerror(errno));
    }
    const bool is_directory = S_ISDIR(status.st_mode);
    if (::unlinkat(current.get(), leaf.c_str(), is_directory ? AT_REMOVEDIR : 0) != 0) {
        if (ignore_nonempty_directory && is_directory
            && (errno == ENOTEMPTY || errno == EEXIST)) {
            return {};
        }
        return std::unexpected(std::strerror(errno));
    }
    return {};
}

inline std::expected<void, std::string> validate_target_path(
    const PackageDataEntry& entry,
    const std::filesystem::path& target_root,
    const std::filesystem::path& resolved_root)
{
    const auto relative = std::filesystem::path(entry.path);
    auto current = resolved_root;
    for (const auto& component : relative.parent_path()) {
        if (component == ".") continue;
        current /= component;

        std::error_code status_ec;
        auto status = std::filesystem::symlink_status(current, status_ec);
        if (status_ec == std::errc::no_such_file_or_directory) {
            status_ec.clear();
            continue;
        }
        if (status_ec) {
            return std::unexpected(std::format(
                "Cannot inspect parent directory for '{}': {}", entry.path, status_ec.message()));
        }
        if (std::filesystem::is_symlink(status)) {
            return std::unexpected(std::format(
                "Parent symlink for '{}' is not allowed", entry.path));
        } else if (!std::filesystem::is_directory(status)) {
            return std::unexpected(std::format(
                "Parent path for '{}' is not a directory", entry.path));
        }
    }

    const auto destination = target_root / relative;
    std::error_code status_ec;
    auto status = std::filesystem::symlink_status(destination, status_ec);
    if (status_ec == std::errc::no_such_file_or_directory) return {};
    if (status_ec) {
        return std::unexpected(std::format(
            "Cannot inspect '{}': {}", entry.path, status_ec.message()));
    }

    if (entry.typeflag == '5') {
        if (!std::filesystem::is_directory(status)) {
            return std::unexpected(std::format(
                "Cannot replace '{}' with directory", entry.path));
        }
    } else if (entry.typeflag == '2' && std::filesystem::is_directory(status)) {
        std::error_code empty_ec;
        if (!std::filesystem::is_empty(destination, empty_ec) || empty_ec) {
            return std::unexpected(std::format(
                "Cannot replace non-empty directory '{}' with symlink", entry.path));
        }
    } else if (entry.typeflag != '2' && std::filesystem::is_directory(status)) {
        return std::unexpected(std::format(
            "Cannot replace directory '{}' with regular file", entry.path));
    }
    return {};
}

inline std::expected<InspectedPackage, std::string> inspect_package_stream(
    std::istream& archive_file,
    std::string_view archive_name,
    const std::filesystem::path* target_root)
{
    std::string manifest_content;
    std::string service_content;
    std::string triggers_content;
    std::string files_idx_content;
    bool manifest_seen = false;
    bool service_seen = false;
    bool triggers_seen = false;
    bool files_idx_seen = false;
    std::vector<PackageDataEntry> data_entries;
    std::unordered_set<std::string> data_paths;
    std::unordered_map<std::string, char> archive_entry_types;
    util::Sha256 archive_hasher;

    auto walk_res = walk_archive_entries(archive_file, archive_name, [&](const ArchiveEntryView& entry)
        -> std::expected<void, std::string> {
        if (entry.name == ".METADATA/manifest.toml") {
            if (manifest_seen) return std::unexpected("Package archive contains multiple manifests");
            if (entry.typeflag != '0') return std::unexpected("Package manifest must be a regular file");
            manifest_seen = true;
            manifest_content.assign(
                reinterpret_cast<const char*>(entry.payload.data()), entry.payload.size());
            return {};
        }
        if (entry.name == ".METADATA/service.toml") {
            if (service_seen) return std::unexpected("Package archive contains multiple service manifests");
            if (entry.typeflag != '0') return std::unexpected("Package service manifest must be a regular file");
            service_seen = true;
            service_content.assign(
                reinterpret_cast<const char*>(entry.payload.data()), entry.payload.size());
            return {};
        }
        if (entry.name == ".METADATA/triggers.toml") {
            if (triggers_seen) return std::unexpected("Package archive contains multiple trigger manifests");
            if (entry.typeflag != '0') return std::unexpected("Package trigger manifest must be a regular file");
            triggers_seen = true;
            triggers_content.assign(
                reinterpret_cast<const char*>(entry.payload.data()), entry.payload.size());
            return {};
        }
        if (entry.name == ".METADATA/files.idx") {
            if (files_idx_seen) return std::unexpected("Package archive contains multiple file indexes");
            if (entry.typeflag != '0') return std::unexpected("Package file index must be a regular file");
            files_idx_seen = true;
            files_idx_content.assign(
                reinterpret_cast<const char*>(entry.payload.data()), entry.payload.size());
            return {};
        }
        if (!entry.name.starts_with("data/")) return {};
        if (entry.name.size() == 5) return {};
        if (entry.typeflag != '0' && entry.typeflag != '5' && entry.typeflag != '2') {
            return std::unexpected(std::format(
                "Unsupported tar entry type '{}' for '{}'", entry.typeflag, entry.name));
        }
        auto path_res = normalize_data_path(std::string_view(entry.name).substr(5));
        if (!path_res) return std::unexpected(path_res.error());
        for (const auto& component : std::filesystem::path(*path_res)) {
            if (component.string().starts_with(temp_file_prefix)) {
                return std::unexpected(
                    "Package data path uses reserved temporary-file namespace: " + *path_res);
            }
        }
        if (!data_paths.insert(*path_res).second) {
            return std::unexpected("Package archive contains duplicate data path: " + *path_res);
        }
        std::string sha256;
        if (entry.typeflag == '0') {
            util::Sha256 hasher;
            if (!entry.payload.empty()) {
                hasher.update(entry.payload.data(), entry.payload.size());
            }
            sha256 = hasher.finalize();
        }
        data_entries.push_back(PackageDataEntry{
            .path = *path_res,
            .size = entry.size,
            .mode = entry.mode,
            .typeflag = entry.typeflag,
            .link_target = entry.typeflag == '2' ? std::string(entry.linkname) : std::string{},
            .sha256 = std::move(sha256),
        });
        archive_entry_types.emplace(*path_res, entry.typeflag);
        return {};
    }, &archive_hasher);
    if (!walk_res) return std::unexpected(walk_res.error());

    if (!manifest_seen) {
        return std::unexpected("Package archive is missing .METADATA/manifest.toml");
    }
    auto manifest_res = package::PackageManifest::parse_toml(manifest_content);
    if (!manifest_res) {
        return std::unexpected("Failed to parse manifest.toml: " + manifest_res.error());
    }

    InspectedPackage result;
    result.manifest = std::move(*manifest_res);
    result.archive_sha256 = archive_hasher.finalize();

    // A standalone trigger manifest is the package-authoring source of truth
    // and overrides trigger tables embedded in manifest.toml.
    if (triggers_seen) {
        auto trigger_res = package::parse_triggers_toml(triggers_content);
        if (!trigger_res) {
            return std::unexpected("Failed to parse triggers.toml: " + trigger_res.error());
        }
        result.manifest.triggers = std::move(*trigger_res);
    }

    constexpr std::array<std::pair<std::string_view, std::string_view>, 4> usr_merge_aliases{
        std::pair{"bin", "usr/bin"},
        std::pair{"sbin", "usr/sbin"},
        std::pair{"lib", "usr/lib"},
        std::pair{"lib64", "usr/lib64"},
    };
    for (const auto& entry : data_entries) {
        auto top = std::string_view(entry.path);
        if (auto slash = top.find('/'); slash != std::string_view::npos) {
            top = top.substr(0, slash);
        }
        auto alias = std::ranges::find_if(
            usr_merge_aliases, [&](const auto& candidate) { return candidate.first == top; });
        if (alias == usr_merge_aliases.end()) continue;

        const bool is_base_merge_link = result.manifest.name == "base-files"
            && entry.path == top
            && entry.typeflag == '2'
            && entry.link_target == alias->second;
        if (!is_base_merge_link) {
            return std::unexpected(std::format(
                "Package '{}' must use canonical usr/ paths instead of '{}'",
                result.manifest.name, entry.path));
        }
    }

    if (service_seen) {
        auto service_res = service::ServiceSpec::parse_toml(service_content);
        if (!service_res) {
            return std::unexpected("Failed to parse service.toml: " + service_res.error());
        }
        result.service = std::move(*service_res);
    }

    for (const auto& entry : data_entries) {
        package::FileEntry file;
        file.path = entry.path;
        file.size = entry.size;
        file.mode = entry.mode;
        file.sha256 = entry.sha256;
        if (entry.typeflag == '5') {
            file.type = package::FileType::Directory;
        } else if (entry.typeflag == '2') {
            file.type = package::FileType::Symlink;
            file.link_target = entry.link_target;
        }
        result.data_files.push_back(std::move(file));
    }

    // Validate the archive payload against its integrity index before any
    // target-root mutation occurs.
    if (files_idx_seen) {
        result.declared_files = parse_files_idx(files_idx_content);
        std::unordered_map<std::string, const package::FileEntry*> actual;
        for (const auto& file : result.data_files) actual.emplace(file.path, &file);
        for (const auto& declared : result.declared_files) {
            if (declared.type != package::FileType::Regular || declared.sha256.empty()) continue;
            auto actual_it = actual.find(declared.path);
            if (actual_it == actual.end()) {
                return std::unexpected(std::format(
                    "Package integrity failure: files.idx lists '{}' but the archive does not contain it",
                    declared.path));
            }
            if (actual_it->second->sha256 != declared.sha256) {
                return std::unexpected(std::format(
                    "Package integrity failure for '{}': files.idx {}, archive {}",
                    declared.path, declared.sha256, actual_it->second->sha256));
            }
        }
    }

    for (const auto& entry : data_entries) {
        for (auto parent = std::filesystem::path(entry.path).parent_path();
             !parent.empty() && parent != ".";
             parent = parent.parent_path()) {
            auto parent_entry = archive_entry_types.find(parent.generic_string());
            if (parent_entry != archive_entry_types.end() && parent_entry->second != '5') {
                return std::unexpected(std::format(
                    "Package data path '{}' traverses non-directory archive entry '{}'",
                    entry.path, parent.generic_string()));
            }
        }
    }

    if (target_root) {
        std::error_code root_ec;
        auto absolute_root = std::filesystem::absolute(*target_root, root_ec).lexically_normal();
        if (root_ec) {
            return std::unexpected("Cannot resolve target root: " + root_ec.message());
        }
        auto resolved_root = std::filesystem::weakly_canonical(absolute_root, root_ec);
        if (root_ec) {
            return std::unexpected("Cannot resolve target root: " + root_ec.message());
        }
        for (const auto& entry : data_entries) {
            auto path_res = validate_target_path(entry, *target_root, resolved_root);
            if (!path_res) return std::unexpected(path_res.error());
        }
    }

    return result;
}

inline std::expected<InspectedPackage, std::string> inspect_package_impl(
    const std::filesystem::path& archive_path,
    const std::filesystem::path* target_root)
{
    std::ifstream archive_file(archive_path, std::ios::binary);
    if (!archive_file.is_open()) {
        return std::unexpected("Cannot open package archive: " + archive_path.string());
    }
    return inspect_package_stream(archive_file, archive_path.string(), target_root);
}

inline std::expected<InspectedPackage, std::string> inspect_package(
    const std::filesystem::path& archive_path)
{
    return inspect_package_impl(archive_path, nullptr);
}

inline std::expected<InspectedPackage, std::string> inspect_package(
    const std::filesystem::path& archive_path,
    const std::filesystem::path& target_root)
{
    return inspect_package_impl(archive_path, &target_root);
}

// ============================================================================
// Streaming Tar + Zstd Extractor (Extracts directly to target root)
// ============================================================================

inline std::expected<ExtractedPackage, std::string> extract_package(
    const std::filesystem::path& archive_path,
    const std::filesystem::path& target_root,
    const package::PackageManifest* expected_manifest = nullptr,
    const InspectedPackage* expected_inspection = nullptr)
{
    std::ifstream archive_file(archive_path, std::ios::binary);
    if (!archive_file.is_open()) {
        return std::unexpected("Cannot open package archive: " + archive_path.string());
    }
    const auto archive_name = archive_path.string();
    auto inspect_res = inspect_package_stream(archive_file, archive_name, &target_root);
    if (!inspect_res) return std::unexpected(inspect_res.error());
    if (expected_manifest
        && package::package_identity(inspect_res->manifest)
            != package::package_identity(*expected_manifest)) {
        return std::unexpected(std::format(
            "Package archive identity mismatch: selected {} {} [{}; {}], archive contains {} {} [{}; {}]",
            expected_manifest->name, expected_manifest->version.to_string(),
            expected_manifest->arch, expected_manifest->channel,
            inspect_res->manifest.name, inspect_res->manifest.version.to_string(),
            inspect_res->manifest.arch, inspect_res->manifest.channel));
    }
    if (expected_inspection
        && inspect_res->archive_sha256 != expected_inspection->archive_sha256) {
        return std::unexpected(
            "Package archive changed after ownership preflight");
    }

    ExtractedPackage result;
    result.manifest = std::move(inspect_res->manifest);
    result.service = std::move(inspect_res->service);
    result.declared_files = std::move(inspect_res->declared_files);

    std::error_code root_ec;
    std::filesystem::create_directories(target_root, root_ec);
    if (root_ec) {
        return std::unexpected(
            "Cannot create target root: " + root_ec.message());
    }
    int root_flags = O_RDONLY | O_DIRECTORY | O_NOFOLLOW;
#ifdef O_CLOEXEC
    root_flags |= O_CLOEXEC;
#endif
    int root_fd = ::open(target_root.c_str(), root_flags);
    if (root_fd < 0) {
        return std::unexpected(
            "Cannot securely open target root: " + std::string(std::strerror(errno)));
    }
    UniqueFd extraction_root(root_fd);

    archive_file.clear();
    archive_file.seekg(0);
    if (!archive_file) {
        return std::unexpected("Cannot rewind package archive: " + archive_name);
    }
    auto extract_res = walk_archive_entries(archive_file, archive_name, [&](const ArchiveEntryView& archive_entry)
        -> std::expected<void, std::string> {
        if (!archive_entry.name.starts_with("data/")) return {};

        if (archive_entry.name.size() == 5) return {};
        auto path_res = normalize_data_path(std::string_view(archive_entry.name).substr(5));
        if (!path_res) return std::unexpected(path_res.error());
        std::string rel_path = std::move(*path_res);
        auto destination = open_anchored_parent(
            extraction_root.get(), std::filesystem::path(rel_path));
        if (!destination) {
            return std::unexpected(std::format(
                "Cannot open parent directory for '{}': {}", rel_path, destination.error()));
        }

        package::FileEntry entry;
        entry.path = rel_path;
        entry.mode = archive_entry.mode;
        entry.size = archive_entry.size;

        if (archive_entry.typeflag == '5') {
            entry.type = package::FileType::Directory;
            auto directory_res = ensure_anchored_directory(
                destination->directory.get(), destination->leaf);
            if (!directory_res) {
                return std::unexpected(std::format(
                    "Cannot create directory '{}': {}", rel_path, directory_res.error()));
            }
        } else if (archive_entry.typeflag == '2') {
            entry.type = package::FileType::Symlink;
            entry.link_target = archive_entry.linkname;
            auto remove_res = remove_anchored_leaf(
                destination->directory.get(), destination->leaf);
            if (!remove_res) {
                return std::unexpected(std::format(
                    "Cannot replace '{}' with symlink: {}", rel_path, remove_res.error()));
            }
            const auto link_target = std::string(archive_entry.linkname);
            if (::symlinkat(
                    link_target.c_str(), destination->directory.get(), destination->leaf.c_str()) != 0) {
                return std::unexpected(std::format(
                    "Cannot create symlink '{}' -> '{}': {}",
                    rel_path, archive_entry.linkname, std::strerror(errno)));
            }
        } else {
            entry.type = package::FileType::Regular;
            const auto destination_name = destination->leaf;
            auto temp_res = UniqueTempFile::create(std::move(destination->directory));
            if (!temp_res) {
                return std::unexpected(std::format(
                    "Cannot create temporary file for '{}': {}", rel_path, temp_res.error()));
            }
            auto write_res = temp_res->write_all(archive_entry.payload);
            if (!write_res) {
                return std::unexpected(std::format(
                    "Cannot write '{}': {}", rel_path, write_res.error()));
            }
            auto install_res = temp_res->install(destination_name, archive_entry.mode);
            if (!install_res) {
                return std::unexpected(std::format(
                    "Cannot install '{}': {}", rel_path, install_res.error()));
            }

            util::Sha256 hasher;
            if (!archive_entry.payload.empty()) {
                hasher.update(archive_entry.payload.data(), archive_entry.payload.size());
            }
            entry.sha256 = hasher.finalize();
        }

        result.extracted_files.push_back(std::move(entry));
        return {};
    });
    if (!extract_res) return std::unexpected(extract_res.error());

    result.manifest.files = result.extracted_files;

    return result;
}

// ============================================================================
// Streaming Tar + Zstd Package Builder (`*.pkg.tar.zst`)
// ============================================================================

inline std::expected<void, std::string> create_package(
    const package::PackageManifest& manifest,
    const std::filesystem::path& data_dir,
    const std::filesystem::path& output_path,
    const std::optional<service::ServiceSpec>& service_spec = std::nullopt) 
{
    if (auto parent = output_path.parent_path(); !parent.empty()) {
        std::filesystem::create_directories(parent);
    }

    std::ofstream out_file(output_path, std::ios::binary);
    if (!out_file.is_open()) {
        return std::unexpected("Cannot create output package: " + output_path.string());
    }

    vendor::zstd::ZstdCompressStream zstd_stream(9); // Level 9 compression
    if (!zstd_stream) {
        return std::unexpected(std::string("Failed to initialize ZSTD compressor"));
    }

    constexpr size_t out_chunk_size = 128 * 1024;
    std::vector<uint8_t> out_chunk(out_chunk_size);

    auto write_compressed_chunk = [&](std::span<const uint8_t> raw, bool is_end = false) -> std::expected<void, std::string> {
        ZSTD_inBuffer in = { raw.data(), raw.size(), 0 };
        while (in.pos < in.size || is_end) {
            ZSTD_outBuffer out = { out_chunk.data(), out_chunk.size(), 0 };
            auto comp_res = zstd_stream.compress_stream(in, out, is_end ? ZSTD_e_end : ZSTD_e_continue);
            if (!comp_res) return std::unexpected(comp_res.error());

            if (out.pos > 0) {
                out_file.write(reinterpret_cast<const char*>(out_chunk.data()), static_cast<std::streamsize>(out.pos));
            }
            if (is_end && *comp_res == 0) break;
        }
        return {};
    };

    auto append_tar_entry = [&](std::string_view name, std::span<const uint8_t> content, uint32_t mode = 0644, char typeflag = '0', std::string_view linkname = {}) -> std::expected<void, std::string> {
        TarHeader hdr{};
        std::memset(&hdr, 0, sizeof(hdr));

        auto path_parts = split_ustar_path(name);
        if (!path_parts) return std::unexpected(path_parts.error());
        if (!path_parts->prefix.empty()) {
            std::memcpy(hdr.prefix, path_parts->prefix.data(), path_parts->prefix.size());
        }
        if (!path_parts->name.empty()) {
            std::memcpy(hdr.name, path_parts->name.data(), path_parts->name.size());
        }

        write_octal(hdr.mode, sizeof(hdr.mode), mode);
        write_octal(hdr.uid, sizeof(hdr.uid), 0);
        write_octal(hdr.gid, sizeof(hdr.gid), 0);
        write_octal(hdr.size, sizeof(hdr.size), content.size());
        write_octal(hdr.mtime, sizeof(hdr.mtime), 1700000000);
        hdr.typeflag = typeflag;

        if (linkname.size() > sizeof(hdr.linkname)) {
            return std::unexpected("Link target cannot be represented in a POSIX USTAR header: " + std::string(linkname));
        }
        if (!linkname.empty()) {
            std::memcpy(hdr.linkname, linkname.data(), linkname.size());
        }

        std::memcpy(hdr.magic, "ustar", 5);
        std::memcpy(hdr.version, "00", 2);
        std::memcpy(hdr.uname, "root", 4);
        std::memcpy(hdr.gname, "root", 4);

        uint32_t chk = compute_tar_checksum(hdr);
        write_tar_checksum(hdr.chksum, chk);

        // Write header
        auto res = write_compressed_chunk(std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(&hdr), 512));
        if (!res) return res;

        // Write payload
        if (!content.empty()) {
            res = write_compressed_chunk(content);
            if (!res) return res;

            // Pad to 512-byte boundary
            size_t pad = (512 - (content.size() % 512)) % 512;
            if (pad > 0) {
                static const uint8_t zeros[512] = {0};
                res = write_compressed_chunk(std::span<const uint8_t>(zeros, pad));
                if (!res) return res;
            }
        }
        return {};
    };

    // 1. Inventory the payload before writing anything.
    //
    // The metadata blocks lead the tar stream, and files.idx plus
    // installed_size can only be filled in once every payload file has been
    // walked and hashed -- hence a pass over data/ first. It costs one extra
    // traversal; the file contents are read once either way.
    package::PackageManifest final_manifest = manifest;
    final_manifest.files.clear();
    uint64_t total_size = 0;

    if (std::filesystem::exists(data_dir)) {
        std::vector<std::filesystem::path> payload;
        for (const auto& entry : std::filesystem::recursive_directory_iterator(data_dir, std::filesystem::directory_options::none)) {
            payload.push_back(entry.path());
        }
        // Deterministic ordering: two builds of the same tree must produce the
        // same files.idx, otherwise nothing downstream can compare them.
        std::ranges::sort(payload);

        for (const auto& path : payload) {
            package::FileEntry fe;
            fe.path = path.lexically_relative(data_dir).generic_string();

            if (std::filesystem::is_symlink(path)) {
                fe.type = package::FileType::Symlink;
                fe.mode = 0777;
                std::error_code ec;
                fe.link_target = std::filesystem::read_symlink(path, ec).generic_string();
            } else if (std::filesystem::is_directory(path)) {
                fe.type = package::FileType::Directory;
                fe.mode = 0755;
            } else if (std::filesystem::is_regular_file(path)) {
                fe.type = package::FileType::Regular;
                auto perms = std::filesystem::status(path).permissions();
                fe.mode = ((perms & std::filesystem::perms::owner_exec) != std::filesystem::perms::none) ? 0755 : 0644;
                std::error_code ec;
                fe.size = std::filesystem::file_size(path, ec);
                if (ec) fe.size = 0;
                total_size += fe.size;
                if (auto h = util::compute_file_sha256(path)) {
                    fe.sha256 = *h;
                }
            } else {
                continue;
            }
            final_manifest.files.push_back(std::move(fe));
        }
    }
    final_manifest.installed_size = total_size;

    // 2. Append .METADATA/manifest.toml
    std::string manifest_toml = final_manifest.serialize_toml();
    auto m_res = append_tar_entry(".METADATA/manifest.toml", std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(manifest_toml.data()), manifest_toml.size()), 0644);
    if (!m_res) return m_res;

    // 3. Append .METADATA/files.idx
    std::string files_idx = serialize_files_idx(final_manifest.files);
    auto fi_res = append_tar_entry(".METADATA/files.idx", std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(files_idx.data()), files_idx.size()), 0644);
    if (!fi_res) return fi_res;

    // 4. Append .METADATA/triggers.toml if the package declares any
    if (!final_manifest.triggers.empty()) {
        std::string trig_toml = package::serialize_triggers_toml(final_manifest.triggers);
        auto t_res = append_tar_entry(".METADATA/triggers.toml", std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(trig_toml.data()), trig_toml.size()), 0644);
        if (!t_res) return t_res;
    }

    // 5. Append .METADATA/service.toml if present
    if (service_spec) {
        std::ostringstream ss;
        ss << "[service]\n";
        ss << "name = \"" << service_spec->name << "\"\n";
        ss << "description = \"" << service_spec->description << "\"\n";
        ss << "exec_start = \"" << service_spec->exec_start << "\"\n";
        if (!service_spec->after.empty()) {
            ss << "after = [\n";
            for (const auto& a : service_spec->after) ss << "    \"" << a << "\",\n";
            ss << "]\n";
        }
        std::string svc_toml = ss.str();
        auto s_res = append_tar_entry(".METADATA/service.toml", std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(svc_toml.data()), svc_toml.size()), 0644);
        if (!s_res) return s_res;
    }

    // 3. Append data/... filesystem payload
    if (std::filesystem::exists(data_dir)) {
        std::map<std::string, std::filesystem::directory_entry> entries;
        for (const auto& entry : std::filesystem::recursive_directory_iterator(data_dir, std::filesystem::directory_options::none)) {
            auto rel = entry.path().lexically_relative(data_dir).generic_string();
            entries.emplace(rel, entry);
        }

        for (const auto& [rel, entry] : entries) {
            std::string tar_name = "data/" + rel;

            if (entry.is_symlink()) {
                auto target = std::filesystem::read_symlink(entry.path()).generic_string();
                auto r = append_tar_entry(tar_name, {}, 0777, '2', target);
                if (!r) return r;
            } else if (entry.is_directory()) {
                auto directory_name = tar_name;
                auto path_parts = split_ustar_path(directory_name);
                std::error_code empty_ec;
                if (!path_parts && !std::filesystem::is_empty(entry.path(), empty_ec) && !empty_ec) {
                    continue;
                }
                auto r = append_tar_entry(directory_name, {}, 0755, '5');
                if (!r) return r;
            } else if (entry.is_regular_file()) {
                std::ifstream f(entry.path(), std::ios::binary);
                std::vector<uint8_t> data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
                auto perms = entry.status().permissions();
                uint32_t mode = 0644;
                if ((perms & std::filesystem::perms::owner_exec) != std::filesystem::perms::none) {
                    mode = 0755;
                }
                auto r = append_tar_entry(tar_name, data, mode, '0');
                if (!r) return r;
            }
        }
    }

    // 7. Two 512-byte zero blocks marking end of Tar archive
    static const uint8_t end_blocks[1024] = {0};
    auto end_res = write_compressed_chunk(std::span<const uint8_t>(end_blocks, sizeof(end_blocks)));
    if (!end_res) return end_res;

    // 8. Flush and finish ZSTD stream
    return write_compressed_chunk({}, true);
}

// ============================================================================
// Local Repository Index Generator (index.toml)
// ============================================================================

inline std::expected<void, std::string> generate_repo_index(
    const std::filesystem::path& repo_dir,
    std::string_view channel_name = "core")
{
    if (!std::filesystem::exists(repo_dir)) {
        return std::unexpected("Repository directory does not exist: " + repo_dir.string());
    }

    std::ostringstream ss;
    const auto quote = [](std::string_view value) {
        return package::escape_toml_basic_string(value);
    };
    ss << "schema_version = 1\n\n";
    ss << "[channel]\n";
    ss << "name = \"" << quote(channel_name) << "\"\n";
    ss << "updated_at = \"" << "2026-08-20T00:00:00Z" << "\"\n\n";

    // Collect all .pkg.tar.zst files recursively, building relative paths
    std::vector<std::pair<std::filesystem::path, std::string>> packages;

    for (const auto& entry : std::filesystem::recursive_directory_iterator(
             repo_dir,
             std::filesystem::directory_options::skip_permission_denied)) {
        if (entry.is_regular_file() && entry.path().string().ends_with(".pkg.tar.zst")) {
            auto rel_path = entry.path().lexically_relative(repo_dir).generic_string();
            packages.emplace_back(entry.path(), rel_path);
        }
    }
    std::ranges::sort(packages, {}, [](const auto& package) -> const std::string& {
        return package.second;
    });

    for (const auto& [abs_path, rel_path] : packages) {
        auto inspect_res = inspect_package(abs_path);
        if (!inspect_res) {
            return std::unexpected(std::format(
                "Cannot index package '{}': {}", rel_path, inspect_res.error()));
        }
        const auto& m = inspect_res->manifest;
        ss << "[[packages]]\n";
        ss << "name = \"" << quote(m.name) << "\"\n";
        ss << "version = \"" << quote(m.version.ver) << "\"\n";
        ss << "release = \"" << quote(m.version.rel) << "\"\n";
        if (m.version.epoch > 0) ss << "epoch = " << m.version.epoch << "\n";
        ss << "description = \"" << quote(m.description) << "\"\n";
        ss << "license = \"" << quote(m.license) << "\"\n";
        ss << "channel = \"" << quote(m.channel) << "\"\n";
        ss << "arch = \"" << quote(m.arch) << "\"\n";
        ss << "installed_size = " << m.installed_size << "\n";
        ss << "file = \"" << quote(rel_path) << "\"\n";
        ss << "dependencies = [\n";
        for (const auto& d : m.dependencies) ss << "    \"" << quote(d.to_string()) << "\",\n";
        ss << "]\n";
        ss << "provides = [\n";
        for (const auto& p : m.provides) ss << "    \"" << quote(p) << "\",\n";
        ss << "]\n\n";
    }

    std::ofstream out(repo_dir / "index.toml");
    if (!out.is_open()) {
        return std::unexpected("Cannot write " + (repo_dir / "index.toml").string());
    }
    out << ss.str();
    out.close();

    return {};
}

} // namespace sage::archive
