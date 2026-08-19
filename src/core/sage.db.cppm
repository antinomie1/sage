export module sage.db;

import std;
import sage.vendor.lmdb;
import sage.package;
import sage.util;

export namespace sage::db {

using std::size_t;

class Database {
public:
    Database() = default;
    ~Database() = default;

    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;

    Database(Database&&) noexcept = default;
    Database& operator=(Database&&) noexcept = default;

    static std::expected<Database, std::string> open(
        const std::filesystem::path& db_path, 
        bool read_only = false,
        size_t map_size = 10ULL * 1024 * 1024 * 1024) 
    {
        auto env_res = vendor::lmdb::MdbEnv::create(db_path, map_size, 32, read_only ? vendor::lmdb::flag_rdonly : 0);
        if (!env_res) return std::unexpected(env_res.error());

        Database db;
        db.env_ = std::move(*env_res);

        // Open/Create tables in initial transaction
        auto txn_res = vendor::lmdb::MdbTxn::begin(db.env_, read_only);
        if (!txn_res) return std::unexpected(txn_res.error());
        auto& txn = *txn_res;

        unsigned int dbi_flags = read_only ? 0 : vendor::lmdb::flag_create;

        auto dbi_pkg = vendor::lmdb::MdbDbi::open(txn, "packages", dbi_flags);
        if (!dbi_pkg) return std::unexpected("Failed to open packages table: " + dbi_pkg.error());
        db.dbi_packages_ = *dbi_pkg;

        auto dbi_files = vendor::lmdb::MdbDbi::open(txn, "files", dbi_flags);
        if (!dbi_files) return std::unexpected("Failed to open files table: " + dbi_files.error());
        db.dbi_files_ = *dbi_files;

        auto dbi_prov = vendor::lmdb::MdbDbi::open(txn, "provides", dbi_flags);
        if (!dbi_prov) return std::unexpected("Failed to open provides table: " + dbi_prov.error());
        db.dbi_provides_ = *dbi_prov;

        auto dbi_chan = vendor::lmdb::MdbDbi::open(txn, "channels", dbi_flags);
        if (!dbi_chan) return std::unexpected("Failed to open channels table: " + dbi_chan.error());
        db.dbi_channels_ = *dbi_chan;

        auto dbi_sys = vendor::lmdb::MdbDbi::open(txn, "system", dbi_flags);
        if (!dbi_sys) return std::unexpected("Failed to open system table: " + dbi_sys.error());
        db.dbi_system_ = *dbi_sys;

        if (!read_only) {
            auto commit_res = txn.commit();
            if (!commit_res) return std::unexpected(commit_res.error());
        }

        return db;
    }

    std::expected<vendor::lmdb::MdbTxn, std::string> begin_read_txn() {
        return vendor::lmdb::MdbTxn::begin(env_, true);
    }

    std::expected<vendor::lmdb::MdbTxn, std::string> begin_write_txn() {
        return vendor::lmdb::MdbTxn::begin(env_, false);
    }

    // ========================================================================
    // Packages Table
    // ========================================================================

    std::optional<package::PackageManifest> get_package(std::string_view name) {
        auto txn = begin_read_txn();
        if (!txn) return std::nullopt;
        auto val = dbi_packages_.get(*txn, name);
        if (!val) return std::nullopt;
        auto parsed = package::PackageManifest::parse_toml(*val);
        if (!parsed) return std::nullopt;
        return *parsed;
    }

    std::expected<void, std::string> put_package(
        vendor::lmdb::MdbTxn& txn, 
        const package::PackageManifest& manifest) 
    {
        std::string serialized = manifest.serialize_toml();
        return dbi_packages_.put(txn, manifest.name, serialized);
    }

    std::expected<void, std::string> del_package(
        vendor::lmdb::MdbTxn& txn, 
        std::string_view name) 
    {
        return dbi_packages_.del(txn, name);
    }

    std::vector<package::PackageManifest> list_installed_packages() {
        std::vector<package::PackageManifest> list;
        auto txn = begin_read_txn();
        if (!txn) return list;

        auto cur_res = vendor::lmdb::MdbCursor::open(*txn, dbi_packages_);
        if (!cur_res) return list;
        auto& cursor = *cur_res;

        std::string_view k, v;
        if (cursor.first(k, v)) {
            do {
                if (auto pkg = package::PackageManifest::parse_toml(v)) {
                    list.push_back(std::move(*pkg));
                }
            } while (cursor.next(k, v));
        }
        return list;
    }

    // ========================================================================
    // Files Table & Conflict Detection
    // ========================================================================

    std::optional<std::string> get_file_owner(std::string_view rel_path) {
        auto txn = begin_read_txn();
        if (!txn) return std::nullopt;
        auto cleaned = util::clean_rel_path(rel_path);
        auto val = dbi_files_.get(*txn, cleaned);
        if (val) return std::string(*val);
        return std::nullopt;
    }

    std::vector<std::string> get_package_files(std::string_view package_name) {
        std::vector<std::string> files;
        auto txn = begin_read_txn();
        if (!txn) return files;
        auto cur_res = vendor::lmdb::MdbCursor::open(*txn, dbi_files_);
        if (!cur_res) return files;
        auto& cursor = *cur_res;
        std::string_view k, v;
        if (cursor.first(k, v)) {
            do {
                if (v == package_name || v.starts_with(std::string(package_name) + ":")) {
                    files.emplace_back(k);
                }
            } while (cursor.next(k, v));
        }
        return files;
    }

    // Register package files into LMDB with atomic conflict checking
    std::expected<void, std::string> register_files(
        vendor::lmdb::MdbTxn& txn,
        std::string_view pkg_name,
        std::string_view channel,
        const std::vector<package::FileEntry>& files) 
    {
        std::string owner_val = std::format("{}:{}", pkg_name, channel);

        // First pass: check for conflicts
        for (const auto& f : files) {
            if (f.type == package::FileType::Directory) continue;
            auto cleaned = util::clean_rel_path(f.path);
            if (auto existing = dbi_files_.get(txn, cleaned)) {
                if (*existing != owner_val) {
                    return std::unexpected(std::format("File conflict: '{}' is already owned by '{}'", cleaned, *existing));
                }
            }
        }

        // Second pass: insert file records
        for (const auto& f : files) {
            if (f.type == package::FileType::Directory) continue;
            auto cleaned = util::clean_rel_path(f.path);
            auto res = dbi_files_.put(txn, cleaned, owner_val);
            if (!res) return res;
        }

        return {};
    }

    std::expected<void, std::string> unregister_files(
        vendor::lmdb::MdbTxn& txn,
        const std::vector<package::FileEntry>& files) 
    {
        for (const auto& f : files) {
            if (f.type == package::FileType::Directory) continue;
            auto cleaned = util::clean_rel_path(f.path);
            auto res = dbi_files_.del(txn, cleaned);
            if (!res) return res;
        }
        return {};
    }

    // ========================================================================
    // Provides Table
    // ========================================================================

    std::optional<std::string> get_provider(std::string_view symbol) {
        auto txn = begin_read_txn();
        if (!txn) return std::nullopt;
        auto val = dbi_provides_.get(*txn, symbol);
        if (val) return std::string(*val);
        return std::nullopt;
    }

    std::expected<void, std::string> register_provides(
        vendor::lmdb::MdbTxn& txn,
        std::string_view pkg_name,
        const std::vector<std::string>& provides) 
    {
        for (const auto& p : provides) {
            auto res = dbi_provides_.put(txn, p, pkg_name);
            if (!res) return res;
        }
        return {};
    }

    std::expected<void, std::string> unregister_provides(
        vendor::lmdb::MdbTxn& txn,
        const std::vector<std::string>& provides) 
    {
        for (const auto& p : provides) {
            auto res = dbi_provides_.del(txn, p);
            if (!res) return res;
        }
        return {};
    }

    // ========================================================================
    // System Providers Table (virtual/init, virtual/udev, virtual/libc)
    // ========================================================================

    std::optional<std::string> get_system_provider(std::string_view iface) {
        auto txn = begin_read_txn();
        if (!txn) return std::nullopt;
        auto val = dbi_system_.get(*txn, iface);
        if (val) return std::string(*val);
        return std::nullopt;
    }

    std::expected<void, std::string> set_system_provider(
        vendor::lmdb::MdbTxn& txn,
        std::string_view iface,
        std::string_view provider) 
    {
        return dbi_system_.put(txn, iface, provider);
    }

    std::map<std::string, std::string> get_all_system_providers() {
        std::map<std::string, std::string> res;
        auto txn = begin_read_txn();
        if (!txn) return res;

        auto cur_res = vendor::lmdb::MdbCursor::open(*txn, dbi_system_);
        if (!cur_res) return res;
        auto& cursor = *cur_res;

        std::string_view k, v;
        if (cursor.first(k, v)) {
            do {
                res[std::string(k)] = std::string(v);
            } while (cursor.next(k, v));
        }
        return res;
    }

private:
    vendor::lmdb::MdbEnv env_;
    vendor::lmdb::MdbDbi dbi_packages_;
    vendor::lmdb::MdbDbi dbi_files_;
    vendor::lmdb::MdbDbi dbi_provides_;
    vendor::lmdb::MdbDbi dbi_channels_;
    vendor::lmdb::MdbDbi dbi_system_;
};

} // namespace sage::db
