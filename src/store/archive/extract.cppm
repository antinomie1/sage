module;

#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <cerrno>
#include <cstdlib>
#include <cstring>

// Mutations on the target root: anchored cleanup and streaming extraction.
export module sage.archive:extract;

import std;
import sage.package;
import sage.service;
import sage.util;

import :detail;
import :tape;

export namespace sage::archive {

using std::size_t;
using std::uint8_t;
using std::uint32_t;
using std::uint64_t;

inline std::expected<void, std::string> remove_path_anchored(
    const std::filesystem::path&, std::string_view, bool);

// Durable, redo-only filesystem transaction.  Payloads are built below the
// target root and the operation list is fsync'd before its id is committed to
// LMDB.  Publication is idempotent, so startup recovery can finish a commit
// interrupted at any instruction (including after rename/unlink + fsync).
class FilesystemTransaction {
public:
    static std::expected<FilesystemTransaction, std::string> create(
        const std::filesystem::path& root)
    {
        auto id = std::format("{:x}-{:x}-{:x}", static_cast<uint64_t>(::getpid()),
            static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count()),
            static_cast<uint64_t>(std::random_device{}()));
        FilesystemTransaction tx(root, std::move(id));
        std::error_code ec;
        std::filesystem::create_directories(tx.payload_, ec);
        if (ec) return std::unexpected("Cannot create filesystem transaction: " + ec.message());
        // An empty journal is meaningful: metadata-only transactions still
        // need durable replay evidence after their LMDB marker is committed.
        std::ofstream journal(tx.journal_, std::ios::binary | std::ios::trunc);
        if (!journal) return std::unexpected("Cannot create filesystem transaction journal");
        journal.close();
        if (!journal) return std::unexpected("Cannot close filesystem transaction journal");
        return tx;
    }

    FilesystemTransaction(FilesystemTransaction&&) noexcept = default;
    FilesystemTransaction& operator=(FilesystemTransaction&&) noexcept = default;
    FilesystemTransaction(const FilesystemTransaction&) = delete;
    FilesystemTransaction& operator=(const FilesystemTransaction&) = delete;

    [[nodiscard]] const std::string& id() const noexcept { return id_; }
    [[nodiscard]] const std::filesystem::path& payload_root() const noexcept { return payload_; }

    std::expected<void, std::string> install(std::string_view path) {
        return append('I', path, true);
    }
    std::expected<void, std::string> remove(std::string_view path, bool ignore_nonempty = true) {
        return append(ignore_nonempty ? 'R' : 'r', path, false);
    }

    std::expected<void, std::string> prepare() {
        // Strict removals represent package-owned non-directory entries. Catch
        // type changes before the database commit; publication itself remains
        // redo-only once the transaction has been committed.
        std::ifstream operations(journal_, std::ios::binary);
        char kind;
        std::string path;
        while (operations >> kind && operations.get() == ' ' && std::getline(operations, path)) {
            if (kind != 'r') continue;
            std::error_code ec;
            const auto status = std::filesystem::symlink_status(root_ / path, ec);
            if (ec && ec != std::errc::no_such_file_or_directory)
                return std::unexpected("Cannot inspect removal path '" + path + "': " + ec.message());
            if (!ec && std::filesystem::is_directory(status))
                return std::unexpected("Cannot replace package file '" + path + "' with a directory");
        }
        if (!operations.eof())
            return std::unexpected(std::string("Cannot validate filesystem transaction journal"));

        int flags = O_RDONLY;
#ifdef O_CLOEXEC
        flags |= O_CLOEXEC;
#endif
        UniqueFd journal(::open(journal_.c_str(), flags));
        if (journal.get() < 0 || ::fsync(journal.get()) != 0)
            return std::unexpected("Cannot sync filesystem transaction journal: " + std::string(std::strerror(errno)));
        UniqueFd directory(::open(directory_.c_str(), flags | O_DIRECTORY));
        if (directory.get() < 0 || ::fsync(directory.get()) != 0)
            return std::unexpected("Cannot sync filesystem transaction directory: " + std::string(std::strerror(errno)));
        UniqueFd payload_directory(::open(payload_.c_str(), flags | O_DIRECTORY));
        if (payload_directory.get() < 0 || ::fsync(payload_directory.get()) != 0)
            return std::unexpected("Cannot sync staged payload directory: " + std::string(std::strerror(errno)));
        for (const auto& entry : std::filesystem::recursive_directory_iterator(payload_)) {
            if (!entry.is_directory()) continue;
            UniqueFd staged_directory(::open(entry.path().c_str(), flags | O_DIRECTORY | O_NOFOLLOW));
            if (staged_directory.get() < 0 || ::fsync(staged_directory.get()) != 0)
                return std::unexpected("Cannot sync staged directory: " + std::string(std::strerror(errno)));
        }
        UniqueFd transaction_root(::open(directory_.parent_path().c_str(), flags | O_DIRECTORY));
        if (transaction_root.get() < 0 || ::fsync(transaction_root.get()) != 0)
            return std::unexpected("Cannot sync filesystem transaction root: " + std::string(std::strerror(errno)));
        return {};
    }

    std::expected<void, std::string> publish() { return publish_impl(root_, id_); }
    static std::expected<void, std::string> recover(
        const std::filesystem::path& root, std::string_view id) {
        return publish_impl(root, id);
    }
    static std::expected<void, std::string> retire(
        const std::filesystem::path& root, std::string_view id) {
        auto dir = root / "var/lib/sage/transactions" / std::string(id);
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
        if (ec) return std::unexpected("Cannot retire filesystem transaction: " + ec.message());
        return {};
    }

private:
    FilesystemTransaction(std::filesystem::path root, std::string id)
        : id_(std::move(id)), root_(std::move(root)),
          directory_(root_ / "var/lib/sage/transactions" / id_),
          payload_(directory_ / "payload"), journal_(directory_ / "operations") {}

    std::expected<void, std::string> append(char kind, std::string_view raw, bool install_op) {
        auto path = normalize_data_path(raw);
        if (!path) return std::unexpected(path.error());
        if (path->contains('\n') || path->contains('\r')) return std::unexpected("Transaction path contains a newline");
        std::ofstream out(journal_, std::ios::app | std::ios::binary);
        if (!out) return std::unexpected("Cannot open filesystem transaction journal");
        out << kind << ' ' << *path << '\n';
        if (!out.flush()) return std::unexpected("Cannot write filesystem transaction journal");
        (void)install_op;
        return {};
    }

    static std::expected<void, std::string> publish_impl(
        const std::filesystem::path& root, std::string_view id)
    {
        auto dir = root / "var/lib/sage/transactions" / std::string(id);
        auto payload = dir / "payload";
        std::ifstream in(dir / "operations", std::ios::binary);
        if (!in) return std::unexpected("Missing committed filesystem transaction " + std::string(id));
        std::vector<std::pair<char, std::string>> ops;
        char kind; std::string path;
        while (in >> kind && in.get() == ' ' && std::getline(in, path)) ops.emplace_back(kind, path);
        if (!in.eof()) return std::unexpected("Corrupt filesystem transaction " + std::string(id));
        size_t published_ops = 0;
        const auto fault_after = []() -> std::optional<size_t> {
            const char* value = ::getenv("SAGE_FAULT_FS_PUBLISH_AFTER");
            if (!value) return std::nullopt;
            size_t parsed = 0;
            auto [end, error] = std::from_chars(value, value + std::strlen(value), parsed);
            if (error != std::errc{} || *end != '\0') return std::nullopt;
            return parsed;
        }();
        auto inject_fault = [&]() -> std::expected<void, std::string> {
            if (fault_after && ++published_ops >= *fault_after)
                return std::unexpected("Injected filesystem publication failure");
            return {};
        };

        // Removes precede installs, matching upgrades/provider swaps. Replays
        // tolerate already absent paths and already non-empty directories.
        for (const auto& [op, rel] : ops) if (op == 'R' || op == 'r') {
            auto removed = remove_path_anchored(root, rel, op == 'R');
            if (!removed) return removed;
            if (auto fault = inject_fault(); !fault) return fault;
        }
        for (const auto& [op, rel] : ops) if (op == 'I') {
            auto source = payload / rel;
            std::error_code ec;
            auto status = std::filesystem::symlink_status(source, ec);
            if (ec) return std::unexpected("Missing staged path '" + rel + "': " + ec.message());
            int flags = O_RDONLY | O_DIRECTORY | O_NOFOLLOW;
#ifdef O_CLOEXEC
            flags |= O_CLOEXEC;
#endif
            UniqueFd root_fd(::open(root.c_str(), flags));
            if (root_fd.get() < 0) return std::unexpected(std::strerror(errno));
            auto destination = open_anchored_parent(root_fd.get(), std::filesystem::path(rel));
            if (!destination) return std::unexpected(destination.error());
            if (std::filesystem::is_directory(status)) {
                auto made = ensure_anchored_directory(destination->directory.get(), destination->leaf);
                if (!made) return made;
            } else {
                auto removed = remove_anchored_leaf(destination->directory.get(), destination->leaf);
                if (!removed) return removed;
                if (std::filesystem::is_symlink(status)) {
                    auto target = std::filesystem::read_symlink(source, ec);
                    if (ec || ::symlinkat(target.c_str(), destination->directory.get(), destination->leaf.c_str()) != 0)
                        return std::unexpected(ec ? ec.message() : std::string(std::strerror(errno)));
                } else {
                    std::ifstream input(source, std::ios::binary);
                    std::vector<uint8_t> bytes(std::istreambuf_iterator<char>(input), {});
                    auto temp = UniqueTempFile::create(std::move(destination->directory));
                    if (!temp) return std::unexpected(temp.error());
                    auto written = temp->write_all(bytes); if (!written) return written;
                    auto installed = temp->install(destination->leaf,
                        static_cast<uint32_t>(status.permissions()) & 07777); if (!installed) return installed;
                    if (auto fault = inject_fault(); !fault) return fault;
                    continue;
                }
                if (::fsync(destination->directory.get()) != 0) return std::unexpected(std::strerror(errno));
            }
            if (auto fault = inject_fault(); !fault) return fault;
        }
        // The journal and payload are completion evidence.  The caller must
        // first durably clear the LMDB pending marker and only then retire
        // them, otherwise a crash between these steps makes replay impossible.
        return {};
    }

    std::string id_;
    std::filesystem::path root_, directory_, payload_, journal_;
};

inline std::expected<void, std::string> remove_path_anchored(
    const std::filesystem::path& target_root,
    std::string_view raw_path,
    bool ignore_nonempty_directory = true)
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
    if (::fsync(current.get()) != 0) return std::unexpected(std::strerror(errno));
    return {};
}
// ============================================================================
// Streaming Tar + Zstd Extractor (Extracts directly to target root)
// ============================================================================

inline std::expected<ExtractedPackage, std::string> extract_package(
    const std::filesystem::path& archive_path,
    const std::filesystem::path& target_root,
    const package::PackageManifest* expected_manifest = nullptr,
    const InspectedPackage* expected_inspection = nullptr,
    FilesystemTransaction* transaction = nullptr)
{
    auto snapshot_res = PrivateArchiveSnapshot::create(
        archive_path, target_root / "var/lib/sage/tmp");
    if (!snapshot_res) return std::unexpected(snapshot_res.error());
    auto snapshot = std::move(*snapshot_res);

    std::ifstream archive_file(snapshot.path(), std::ios::binary);
    if (!archive_file.is_open()) {
        return std::unexpected(
            "Cannot open private archive snapshot for: " + archive_path.string());
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

    const auto& extraction_path = transaction ? transaction->payload_root() : target_root;
    std::error_code root_ec;
    std::filesystem::create_directories(extraction_path, root_ec);
    if (root_ec) {
        return std::unexpected(
            "Cannot create target root: " + root_ec.message());
    }
    int root_flags = O_RDONLY | O_DIRECTORY | O_NOFOLLOW;
#ifdef O_CLOEXEC
    root_flags |= O_CLOEXEC;
#endif
    int root_fd = ::open(extraction_path.c_str(), root_flags);
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
            if (::fsync(destination->directory.get()) != 0) {
                return std::unexpected(std::format(
                    "Cannot sync symlink directory '{}': {}",
                    rel_path, std::strerror(errno)));
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

        if (transaction) {
            auto staged = transaction->install(rel_path);
            if (!staged) return staged;
        }
        result.extracted_files.push_back(std::move(entry));
        return {};
    });
    if (!extract_res) return std::unexpected(extract_res.error());

    result.manifest.files = result.extracted_files;

    return result;
}
} // namespace sage::archive
