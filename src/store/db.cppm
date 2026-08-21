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
        std::filesystem::path actual_dir = (db_path.extension() == ".mdb") ? db_path.parent_path() : db_path;
        auto env_res = vendor::lmdb::MdbEnv::create(actual_dir, map_size, 32, read_only ? vendor::lmdb::flag_rdonly : 0);
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

        auto commit_res = txn.commit();
        if (!commit_res) return std::unexpected(commit_res.error());

        return db;
    }

    std::expected<vendor::lmdb::MdbTxn, std::string> begin_read_txn() {
        return vendor::lmdb::MdbTxn::begin(env_, true);
    }

    std::expected<vendor::lmdb::MdbTxn, std::string> begin_write_txn() {
        return vendor::lmdb::MdbTxn::begin(env_, false);
    }

    // A filesystem publication is deliberately recorded in the same LMDB
    // transaction as the package metadata it belongs to.  Consequently a
    // crash can leave either no marker (the staged tree is disposable), or a
    // committed marker which is safe to replay idempotently.  There is never
    // an ambiguous "did LMDB commit?" window.
    std::expected<void, std::string> add_pending_filesystem_transaction(
        vendor::lmdb::MdbTxn& txn, std::string_view id)
    {
        return dbi_system_.put(txn, std::string("fs-txn/") + std::string(id), "pending");
    }

    std::expected<std::vector<std::string>, std::string> pending_filesystem_transactions()
    {
        auto txn = begin_read_txn();
        if (!txn) return std::unexpected(txn.error());
        std::vector<std::string> result;
        auto cursor = vendor::lmdb::MdbCursor::open(*txn, dbi_system_);
        if (!cursor) return std::unexpected(cursor.error());
        std::string_view key, value;
        auto entry = cursor->first(key, value);
        if (!entry) return std::unexpected(entry.error());
        while (*entry) {
            if (key.starts_with("fs-txn/") && value == "pending") {
                result.emplace_back(key.substr(7));
            }
            entry = cursor->next(key, value);
            if (!entry) return std::unexpected(entry.error());
        }
        return result;
    }

    std::expected<void, std::string> finish_filesystem_transaction(std::string_view id)
    {
        auto txn = begin_write_txn();
        if (!txn) return std::unexpected(txn.error());
        auto removed = dbi_system_.del(*txn, std::string("fs-txn/") + std::string(id));
        if (!removed) return removed;
        return txn->commit();
    }

    // ========================================================================
    // Packages Table
    // ========================================================================

    std::expected<std::optional<package::PackageManifest>, std::string> get_package(
        std::string_view name)
    {
        auto txn = begin_read_txn();
        if (!txn) {
            return std::unexpected("Failed to open installed package read transaction: " + txn.error());
        }
        return get_package(*txn, name);
    }

    std::expected<std::optional<package::PackageManifest>, std::string> get_package(
        vendor::lmdb::MdbTxn& txn,
        std::string_view name)
    {
        auto val = dbi_packages_.get_checked(txn, name);
        if (!val) {
            return std::unexpected("Failed to read installed package metadata: " + val.error());
        }
        if (!*val) return std::optional<package::PackageManifest>{};
        auto parsed = package::PackageManifest::parse_toml(**val);
        if (!parsed) {
            return std::unexpected(std::format(
                "Failed to parse installed package '{}' metadata: {}", name, parsed.error()));
        }
        if (parsed->name != name) {
            return std::unexpected(std::format(
                "Installed package key '{}' contains metadata for '{}'", name, parsed->name));
        }
        return std::optional<package::PackageManifest>{std::move(*parsed)};
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

    std::expected<std::vector<package::PackageManifest>, std::string> list_installed_packages() {
        auto txn = begin_read_txn();
        if (!txn) return std::unexpected("Failed to open installed package read transaction: " + txn.error());
        return list_installed_packages(*txn);
    }

    std::expected<std::vector<package::PackageManifest>, std::string> list_installed_packages(
        vendor::lmdb::MdbTxn& txn)
    {
        std::vector<package::PackageManifest> list;
        auto cur_res = vendor::lmdb::MdbCursor::open(txn, dbi_packages_);
        if (!cur_res) return std::unexpected("Failed to open installed package cursor: " + cur_res.error());
        auto& cursor = *cur_res;

        std::string_view k, v;
        auto entry = cursor.first(k, v);
        if (!entry) {
            return std::unexpected("Failed to read installed package cursor: " + entry.error());
        }
        while (*entry) {
            auto pkg = package::PackageManifest::parse_toml(v);
            if (!pkg) {
                return std::unexpected(std::format(
                    "Failed to parse installed package '{}' metadata: {}", k, pkg.error()));
            }
            if (pkg->name != k) {
                return std::unexpected(std::format(
                    "Installed package key '{}' contains metadata for '{}'", k, pkg->name));
            }
            list.push_back(std::move(*pkg));

            entry = cursor.next(k, v);
            if (!entry) {
                return std::unexpected("Failed to advance installed package cursor: " + entry.error());
            }
        }
        return list;
    }

    // ========================================================================
    // Files Table & Conflict Detection
    // ========================================================================

    std::expected<std::optional<std::string>, std::string> get_file_owner(
        std::string_view rel_path)
    {
        auto txn = begin_read_txn();
        if (!txn) {
            return std::unexpected("Failed to open file ownership read transaction: " + txn.error());
        }
        return get_file_owner(*txn, rel_path);
    }

    std::expected<std::optional<std::string>, std::string> get_file_owner(
        vendor::lmdb::MdbTxn& txn,
        std::string_view rel_path)
    {
        auto cleaned = util::clean_rel_path(rel_path);
        auto val = dbi_files_.get_checked(txn, cleaned);
        if (!val) {
            return std::unexpected("Failed to read file ownership: " + val.error());
        }
        if (!*val) return std::optional<std::string>{};
        return std::optional<std::string>{std::string(**val)};
    }

    std::expected<std::vector<std::string>, std::string> get_package_files(
        std::string_view package_name)
    {
        auto txn = begin_read_txn();
        if (!txn) return std::unexpected("Failed to open file ownership read transaction: " + txn.error());
        return get_package_files(*txn, package_name);
    }

    std::expected<std::vector<std::string>, std::string> get_package_files(
        vendor::lmdb::MdbTxn& txn,
        std::string_view package_name)
    {
        std::vector<std::string> files;
        auto cur_res = vendor::lmdb::MdbCursor::open(txn, dbi_files_);
        if (!cur_res) return std::unexpected("Failed to open file ownership cursor: " + cur_res.error());
        auto& cursor = *cur_res;
        std::string_view k, v;
        auto entry = cursor.first(k, v);
        if (!entry) return std::unexpected("Failed to read file ownership cursor: " + entry.error());
        while (*entry) {
            if (v == package_name || v.starts_with(std::string(package_name) + ":")) {
                files.emplace_back(k);
            }
            entry = cursor.next(k, v);
            if (!entry) return std::unexpected("Failed to advance file ownership cursor: " + entry.error());
        }
        return files;
    }

    // Register package files into LMDB with atomic conflict checking
    std::expected<void, std::string> check_file_conflicts(
        vendor::lmdb::MdbTxn& txn,
        std::optional<std::string_view> allowed_owner,
        const std::vector<package::FileEntry>& files)
    {
        for (const auto& f : files) {
            if (f.type == package::FileType::Directory) continue;
            auto cleaned = util::clean_rel_path(f.path);
            if (cleaned == "usr/share/info/dir" || cleaned.ends_with("/info/dir")) continue;

            auto existing = dbi_files_.get_checked(txn, cleaned);
            if (!existing) {
                return std::unexpected(std::format(
                    "Failed to check ownership for '{}': {}", cleaned, existing.error()));
            }
            if (*existing && (!allowed_owner || **existing != *allowed_owner)) {
                return std::unexpected(std::format(
                    "File conflict: '{}' is already owned by '{}'",
                    cleaned, **existing));
            }
        }
        return {};
    }

    std::expected<void, std::string> register_files(
        vendor::lmdb::MdbTxn& txn,
        std::string_view pkg_name,
        std::string_view channel,
        const std::vector<package::FileEntry>& files,
        std::optional<std::string_view> allowed_owner = std::nullopt)
    {
        std::string owner_val = std::format("{}:{}", pkg_name, channel);

        auto conflict_res = check_file_conflicts(txn, allowed_owner, files);
        if (!conflict_res) return conflict_res;

        // Insert file records (new owner claims file)
        for (const auto& f : files) {
            if (f.type == package::FileType::Directory) continue;
            auto cleaned = util::clean_rel_path(f.path);
            if (cleaned == "usr/share/info/dir" || cleaned.ends_with("/info/dir")) continue;
            auto res = dbi_files_.put(txn, cleaned, owner_val);
            if (!res) return res;
        }

        return {};
    }

    std::expected<void, std::string> unregister_files(
        vendor::lmdb::MdbTxn& txn,
        const std::vector<package::FileEntry>& files,
        std::string_view expected_owner = "") 
    {
        for (const auto& f : files) {
            if (f.type == package::FileType::Directory) continue;
            auto cleaned = util::clean_rel_path(f.path);
            if (cleaned == "usr/share/info/dir" || cleaned.ends_with("/info/dir")) continue;
            if (!expected_owner.empty()) {
                auto existing = dbi_files_.get_checked(txn, cleaned);
                if (!existing) {
                    return std::unexpected(std::format(
                        "Failed to read ownership for '{}': {}", cleaned, existing.error()));
                }
                if (*existing && **existing != expected_owner) {
                    continue;
                }
            }
            auto res = dbi_files_.del(txn, cleaned);
            if (!res) return res;
        }
        return {};
    }

    // Prune file registrations whose owning package is no longer installed.
    // The files table is the authoritative ownership registry: entries left
    // behind by upgrades/removes (e.g. when a previous version's manifest file
    // list was incomplete) would otherwise block other packages from claiming
    // the same paths. Returns the number of pruned entries.
    std::expected<std::size_t, std::string> prune_orphaned_files(
        vendor::lmdb::MdbTxn& txn)
    {
        std::vector<std::string> orphaned;
        std::unordered_map<std::string, bool> package_exists;
        auto cur_res = vendor::lmdb::MdbCursor::open(txn, dbi_files_);
        if (!cur_res) return std::unexpected("Failed to open orphaned-file cursor: " + cur_res.error());
        auto& cursor = *cur_res;
        std::string_view k, v;
        auto entry = cursor.first(k, v);
        if (!entry) return std::unexpected("Failed to read orphaned-file cursor: " + entry.error());
        while (*entry) {
            std::string_view owner = v;
            auto colon = owner.find(':');
            std::string package_name(
                colon == std::string_view::npos ? owner : owner.substr(0, colon));
            auto known = package_exists.find(package_name);
            if (known == package_exists.end()) {
                auto package = dbi_packages_.get_checked(txn, package_name);
                if (!package) {
                    return std::unexpected(
                        "Failed to check package while pruning ownership: " + package.error());
                }
                known = package_exists.emplace(package_name, package->has_value()).first;
            }
            if (!known->second) {
                orphaned.emplace_back(k);
            }
            entry = cursor.next(k, v);
            if (!entry) return std::unexpected("Failed to advance orphaned-file cursor: " + entry.error());
        }
        for (const auto& key : orphaned) {
            auto deleted = dbi_files_.del(txn, key);
            if (!deleted) return std::unexpected("Failed to prune orphaned file ownership: " + deleted.error());
        }
        return orphaned.size();
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

    std::expected<std::optional<std::string>, std::string> get_system_provider(
        std::string_view iface)
    {
        auto txn = begin_read_txn();
        if (!txn) {
            return std::unexpected("Failed to open system provider read transaction: " + txn.error());
        }
        return get_system_provider(*txn, iface);
    }

    std::expected<std::optional<std::string>, std::string> get_system_provider(
        vendor::lmdb::MdbTxn& txn,
        std::string_view iface)
    {
        auto val = dbi_system_.get_checked(txn, iface);
        if (!val) {
            return std::unexpected("Failed to read system provider: " + val.error());
        }
        if (!*val) return std::optional<std::string>{};
        return std::optional<std::string>{std::string(**val)};
    }

    std::expected<void, std::string> set_system_provider(
        vendor::lmdb::MdbTxn& txn,
        std::string_view iface,
        std::string_view provider) 
    {
        return dbi_system_.put(txn, iface, provider);
    }

    std::expected<std::map<std::string, std::string>, std::string> get_all_system_providers() {
        std::map<std::string, std::string> res;
        auto txn = begin_read_txn();
        if (!txn) return std::unexpected("Failed to open system provider read transaction: " + txn.error());

        auto cur_res = vendor::lmdb::MdbCursor::open(*txn, dbi_system_);
        if (!cur_res) return std::unexpected("Failed to open system provider cursor: " + cur_res.error());
        auto& cursor = *cur_res;

        std::string_view k, v;
        auto entry = cursor.first(k, v);
        if (!entry) return std::unexpected("Failed to read system provider cursor: " + entry.error());
        while (*entry) {
            res[std::string(k)] = std::string(v);
            entry = cursor.next(k, v);
            if (!entry) return std::unexpected("Failed to advance system provider cursor: " + entry.error());
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
