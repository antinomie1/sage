module;

#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
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
    if (len > 1) dest[len - 2] = ' ';
    size_t i = len - 2;
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

// ============================================================================
// Streaming Tar + Zstd Extractor (Extracts directly to target root)
// ============================================================================

inline std::expected<ExtractedPackage, std::string> extract_package(
    const std::filesystem::path& archive_path,
    const std::filesystem::path& target_root) 
{
    std::ifstream file(archive_path, std::ios::binary);
    if (!file.is_open()) {
        return std::unexpected("Cannot open package archive: " + archive_path.string());
    }

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

    ExtractedPackage result;
    std::string manifest_content;
    std::string service_content;

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
                ring.erase(ring.begin(), ring.begin() + 512);
                continue;
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

            const uint8_t* payload_data = ring.data() + 512;

            if (full_name == ".METADATA/manifest.toml") {
                manifest_content.assign(reinterpret_cast<const char*>(payload_data), file_size);
            } else if (full_name == ".METADATA/service.toml") {
                service_content.assign(reinterpret_cast<const char*>(payload_data), file_size);
            } else if (full_name.starts_with("data/")) {
                std::string rel_path = full_name.substr(5); // strip "data/"
                if (!rel_path.empty()) {
                    std::filesystem::path dest_file = target_root / rel_path;
                    std::filesystem::create_directories(dest_file.parent_path());

                    package::FileEntry entry;
                    entry.path = rel_path;
                    entry.mode = mode;
                    entry.size = file_size;

                    if (typeflag == '5') {
                        // Directory
                        entry.type = package::FileType::Directory;
                        std::filesystem::create_directories(dest_file);
                    } else if (typeflag == '2') {
                        // Symlink
                        entry.type = package::FileType::Symlink;
                        entry.link_target = linkname;
                        std::error_code ec;
                        std::filesystem::remove(dest_file, ec);
                        std::filesystem::create_symlink(linkname, dest_file, ec);
                    } else {
                        // Regular File
                        entry.type = package::FileType::Regular;
                        std::string tmp_file = dest_file.string() + ".sage_tmp";
                        int fd = ::open(tmp_file.c_str(), O_WRONLY | O_CREAT | O_TRUNC, mode ? mode : 0644);
                        if (fd >= 0) {
                            if (file_size > 0) {
                                ssize_t w = ::write(fd, payload_data, file_size);
                                (void)w;
                            }
                            ::close(fd);
                            ::chmod(tmp_file.c_str(), mode ? mode : 0644);
                            std::error_code ren_ec;
                            std::filesystem::rename(tmp_file, dest_file, ren_ec);
                        }

                        // Compute file SHA256
                        util::Sha256 hasher;
                        if (file_size > 0) {
                            hasher.update(payload_data, file_size);
                        }
                        entry.sha256 = hasher.finalize();
                    }

                    result.extracted_files.push_back(std::move(entry));
                }
            }

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

            if (out.pos > 0) {
                auto proc_res = process_decompressed_bytes(std::span<const uint8_t>(out_buffer.data(), out.pos));
                if (!proc_res) return std::unexpected(proc_res.error());
            }
        }
    }

    if (manifest_content.empty()) {
        return std::unexpected("Package archive is missing .METADATA/manifest.toml");
    }

    auto manifest_res = package::PackageManifest::parse_toml(manifest_content);
    if (!manifest_res) return std::unexpected("Failed to parse manifest.toml: " + manifest_res.error());
    result.manifest = std::move(*manifest_res);
    result.manifest.files = result.extracted_files;

    if (!service_content.empty()) {
        auto svc_res = service::ServiceSpec::parse_toml(service_content);
        if (svc_res) {
            result.service = std::move(*svc_res);
        }
    }

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

        if (name.size() > sizeof(hdr.name)) {
            size_t slash = name.rfind('/', sizeof(hdr.name));
            if (slash != std::string_view::npos && slash < sizeof(hdr.prefix)) {
                std::memcpy(hdr.prefix, name.data(), std::min(slash, sizeof(hdr.prefix)));
                size_t remainder = name.size() - slash - 1;
                std::memcpy(hdr.name, name.data() + slash + 1, std::min(remainder, sizeof(hdr.name)));
            } else {
                std::memcpy(hdr.name, name.data(), std::min(name.size(), sizeof(hdr.name)));
            }
        } else {
            std::memcpy(hdr.name, name.data(), std::min(name.size(), sizeof(hdr.name)));
        }

        write_octal(hdr.mode, sizeof(hdr.mode), mode);
        write_octal(hdr.uid, sizeof(hdr.uid), 0);
        write_octal(hdr.gid, sizeof(hdr.gid), 0);
        write_octal(hdr.size, sizeof(hdr.size), content.size());
        write_octal(hdr.mtime, sizeof(hdr.mtime), 1700000000);
        hdr.typeflag = typeflag;

        if (!linkname.empty()) {
            std::memcpy(hdr.linkname, linkname.data(), std::min(linkname.size(), sizeof(hdr.linkname)));
        }

        std::memcpy(hdr.magic, "ustar", 5);
        std::memcpy(hdr.version, "00", 2);
        std::memcpy(hdr.uname, "root", 4);
        std::memcpy(hdr.gname, "root", 4);

        uint32_t chk = compute_tar_checksum(hdr);
        write_octal(hdr.chksum, sizeof(hdr.chksum), chk);

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
                auto r = append_tar_entry(tar_name + "/", {}, 0755, '5');
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
    ss << "schema_version = 1\n\n";
    ss << "[channel]\n";
    ss << "name = \"" << channel_name << "\"\n";
    ss << "updated_at = \"" << "2026-08-20T00:00:00Z" << "\"\n\n";

    size_t count = 0;
    for (const auto& entry : std::filesystem::directory_iterator(repo_dir)) {
        if (entry.is_regular_file() && entry.path().string().ends_with(".pkg.tar.zst")) {
            auto temp_sysroot = std::filesystem::temp_directory_path() / ("sage_idx_" + std::to_string(count));
            std::filesystem::remove_all(temp_sysroot);
            auto ext_res = extract_package(entry.path(), temp_sysroot);
            std::filesystem::remove_all(temp_sysroot);
            if (ext_res) {
                const auto& m = ext_res->manifest;
                ss << "[[packages]]\n";
                ss << "name = \"" << m.name << "\"\n";
                ss << "version = \"" << m.version.ver << "\"\n";
                ss << "release = \"" << m.version.rel << "\"\n";
                ss << "description = \"" << m.description << "\"\n";
                ss << "license = \"" << m.license << "\"\n";
                ss << "channel = \"" << m.channel << "\"\n";
                ss << "arch = \"" << m.arch << "\"\n";
                ss << "installed_size = " << m.installed_size << "\n";
                ss << "dependencies = [\n";
                for (const auto& d : m.dependencies) ss << "    \"" << d.to_string() << "\",\n";
                ss << "]\n";
                ss << "provides = [\n";
                for (const auto& p : m.provides) ss << "    \"" << p << "\",\n";
                ss << "]\n\n";
                count++;
            }
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
