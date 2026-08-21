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
// Extracted Package Result
// ============================================================================

struct ExtractedPackage {
    package::PackageManifest manifest;
    std::optional<service::ServiceSpec> service;
    std::vector<package::FileEntry> extracted_files;
};

struct InspectedPackage {
    package::PackageManifest manifest;
    std::optional<service::ServiceSpec> service;
    std::vector<package::FileEntry> data_files;
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
    Handler&& handler)
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
    char typeflag{'0'};
    std::string link_target;
};

class UniqueTempFile {
public:
    UniqueTempFile(const UniqueTempFile&) = delete;
    UniqueTempFile& operator=(const UniqueTempFile&) = delete;

    UniqueTempFile(UniqueTempFile&& other) noexcept
        : directory_fd_(std::exchange(other.directory_fd_, -1)),
          fd_(std::exchange(other.fd_, -1)),
          name_(std::exchange(other.name_, {})) {}

    UniqueTempFile& operator=(UniqueTempFile&& other) noexcept {
        if (this != &other) {
            cleanup();
            directory_fd_ = std::exchange(other.directory_fd_, -1);
            fd_ = std::exchange(other.fd_, -1);
            name_ = std::exchange(other.name_, {});
        }
        return *this;
    }

    ~UniqueTempFile() noexcept { cleanup(); }

    static std::expected<UniqueTempFile, std::string> create(
        const std::filesystem::path& directory)
    {
        int directory_flags = O_RDONLY | O_DIRECTORY;
#ifdef O_CLOEXEC
        directory_flags |= O_CLOEXEC;
#endif
        int directory_fd = ::open(directory.c_str(), directory_flags);
        if (directory_fd < 0) {
            return std::unexpected(std::format(
                "Cannot open temporary-file directory: {}", std::strerror(errno)));
        }

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
            int fd = ::openat(directory_fd, name.c_str(), flags, 0600);
            if (fd >= 0) {
                return UniqueTempFile(directory_fd, fd, std::move(name));
            }
            if (errno != EEXIST) {
                auto message = std::string(std::strerror(errno));
                ::close(directory_fd);
                return std::unexpected(std::format(
                    "Cannot create temporary file: {}", message));
            }
        }
        ::close(directory_fd);
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
        const std::filesystem::path& destination,
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
        auto destination_name = destination.filename().string();
        if (::renameat(
                directory_fd_, name_.c_str(), directory_fd_, destination_name.c_str()) != 0) {
            return std::unexpected(
                "Cannot atomically install temporary file: " + std::string(std::strerror(errno)));
        }
        name_.clear();
        return {};
    }

private:
    UniqueTempFile(int directory_fd, int fd, std::string name)
        : directory_fd_(directory_fd), fd_(fd), name_(std::move(name)) {}

    void cleanup() noexcept {
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
        if (directory_fd_ >= 0 && !name_.empty()) {
            (void)::unlinkat(directory_fd_, name_.c_str(), 0);
            name_.clear();
        }
        if (directory_fd_ >= 0) {
            ::close(directory_fd_);
            directory_fd_ = -1;
        }
    }

    int directory_fd_{-1};
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

inline bool path_is_within(
    const std::filesystem::path& root,
    const std::filesystem::path& candidate)
{
    auto root_it = root.begin();
    auto candidate_it = candidate.begin();
    for (; root_it != root.end(); ++root_it, ++candidate_it) {
        if (candidate_it == candidate.end() || *candidate_it != *root_it) return false;
    }
    return true;
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
            std::error_code link_ec;
            auto link_target = std::filesystem::read_symlink(current, link_ec);
            if (link_ec) {
                return std::unexpected(std::format(
                    "Cannot inspect parent symlink for '{}': {}", entry.path, link_ec.message()));
            }
            auto resolved = link_target.is_absolute()
                ? link_target
                : current.parent_path() / link_target;
            resolved = std::filesystem::weakly_canonical(resolved, link_ec);
            if (link_ec) {
                return std::unexpected(std::format(
                    "Cannot resolve parent symlink for '{}': {}", entry.path, link_ec.message()));
            }
            if (!path_is_within(resolved_root, resolved)) {
                return std::unexpected(std::format(
                    "Parent symlink for '{}' escapes target root", entry.path));
            }
            current = std::move(resolved);
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
    bool manifest_seen = false;
    bool service_seen = false;
    std::vector<PackageDataEntry> data_entries;
    std::unordered_set<std::string> data_paths;
    std::unordered_map<std::string, char> archive_entry_types;

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
        data_entries.push_back(PackageDataEntry{
            .path = *path_res,
            .typeflag = entry.typeflag,
            .link_target = entry.typeflag == '2' ? std::string(entry.linkname) : std::string{},
        });
        archive_entry_types.emplace(*path_res, entry.typeflag);
        return {};
    });
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
        if (entry.typeflag == '5') {
            file.type = package::FileType::Directory;
        } else if (entry.typeflag == '2') {
            file.type = package::FileType::Symlink;
        }
        result.data_files.push_back(std::move(file));
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
    const package::PackageManifest* expected_manifest = nullptr)
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

    ExtractedPackage result;
    result.manifest = std::move(inspect_res->manifest);
    result.service = std::move(inspect_res->service);

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
        std::filesystem::path dest_file = target_root / rel_path;
        std::error_code parent_ec;
        std::filesystem::create_directories(dest_file.parent_path(), parent_ec);
        if (parent_ec) {
            return std::unexpected(std::format(
                "Cannot create parent directory for '{}': {}", rel_path, parent_ec.message()));
        }

        package::FileEntry entry;
        entry.path = rel_path;
        entry.mode = archive_entry.mode;
        entry.size = archive_entry.size;

        if (archive_entry.typeflag == '5') {
            entry.type = package::FileType::Directory;
            std::error_code dir_ec;
            std::filesystem::create_directories(dest_file, dir_ec);
            if (dir_ec) {
                return std::unexpected(std::format(
                    "Cannot create directory '{}': {}", rel_path, dir_ec.message()));
            }
        } else if (archive_entry.typeflag == '2') {
            entry.type = package::FileType::Symlink;
            entry.link_target = archive_entry.linkname;
            std::error_code ec;
            std::filesystem::remove(dest_file, ec);
            if (ec) {
                return std::unexpected(std::format(
                    "Cannot replace '{}' with symlink: {}", rel_path, ec.message()));
            }
            std::filesystem::create_symlink(archive_entry.linkname, dest_file, ec);
            if (ec) {
                return std::unexpected(std::format(
                    "Cannot create symlink '{}' -> '{}': {}", rel_path, archive_entry.linkname, ec.message()));
            }
        } else {
            entry.type = package::FileType::Regular;
            auto temp_res = UniqueTempFile::create(dest_file.parent_path());
            if (!temp_res) {
                return std::unexpected(std::format(
                    "Cannot create temporary file for '{}': {}", rel_path, temp_res.error()));
            }
            auto write_res = temp_res->write_all(archive_entry.payload);
            if (!write_res) {
                return std::unexpected(std::format(
                    "Cannot write '{}': {}", rel_path, write_res.error()));
            }
            auto install_res = temp_res->install(dest_file, archive_entry.mode);
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

    // 1. Append .METADATA/manifest.toml
    std::string manifest_toml = manifest.serialize_toml();
    auto m_res = append_tar_entry(".METADATA/manifest.toml", std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(manifest_toml.data()), manifest_toml.size()), 0644);
    if (!m_res) return m_res;

    // 2. Append .METADATA/service.toml if present
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

    // 4. Two 512-byte zero blocks marking end of Tar archive
    static const uint8_t end_blocks[1024] = {0};
    auto end_res = write_compressed_chunk(std::span<const uint8_t>(end_blocks, sizeof(end_blocks)));
    if (!end_res) return end_res;

    // 5. Flush and finish ZSTD stream
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

    for (const auto& entry : std::filesystem::directory_iterator(repo_dir)) {
        if (entry.is_regular_file() && entry.path().string().ends_with(".pkg.tar.zst")) {
            auto inspect_res = inspect_package(entry.path());
            if (!inspect_res) {
                return std::unexpected(std::format(
                    "Cannot index package '{}': {}", entry.path().filename().string(), inspect_res.error()));
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
            ss << "dependencies = [\n";
            for (const auto& d : m.dependencies) ss << "    \"" << quote(d.to_string()) << "\",\n";
            ss << "]\n";
            ss << "provides = [\n";
            for (const auto& p : m.provides) ss << "    \"" << quote(p) << "\",\n";
            ss << "]\n\n";
        }
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
