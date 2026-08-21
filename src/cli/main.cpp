#include <stdlib.h>

import std;
import sage;

namespace {

using std::size_t;
using std::uint32_t;

struct CliOptions {
    std::string command;
    std::vector<std::string> args;
    std::filesystem::path target_root{"/"};
    bool dry_run{false};
    bool verbose{false};
    bool force{false};        // --force, -f, --nodeps, -d
    bool cascade{false};      // --cascade, -c
    bool no_recursive{false};  // --no-recursive
    bool no_elf_check{false};  // --no-elf-check
    std::string channel_filter;  // --channel <NAME>
};

void print_banner() {
    std::println("{}🌿 Sage Package Manager v0.2.0 (Modern C++23){}", sage::util::color::green, sage::util::color::reset);
}

void print_help() {
    print_banner();
    std::println(R"(
Usage: sage [GLOBAL OPTIONS] <COMMAND> [ARGS...]

Commands:
  install <PKG...>         Install packages into target root via PubGrub SAT solver
  remove <PKG...>          Remove installed package files and unregister state
  rebuild                  Declarative reconcile (/etc/sage/system.toml vs LMDB)
  toolchain [list|use]     Manage multi-slot compiler toolchains (llvm, gcc, rust, java)
  java [list|use <slot>]   Manage OpenJDK/GraalVM/Temurin versions and JAVA_HOME
  rust [list|use <slot>]   Manage Rust stable/nightly versions and targets
  shell [--with <spec...>] Launch ephemeral sandboxed shell with custom toolchains
  channel [COMMAND]        Manage multi-layer channels (list, add, remove, sync)
  repo index <DIR> [NAME]  Generate index.toml for local repository directory
  query [COMMAND]          Query packages, files, capabilities and ownership (nanosecond LMDB)
  list [-q] [PATTERN]      List installed packages (-q: bare names for scripting)
  count [PATTERN]          Print the number of installed packages
  service [COMMAND]        Inspect and generate native init scripts (OpenRC/Runit/Systemd/Dinit/s6)
  build <RECIPE_DIR>       Build package from recipe.toml (fetch source, check sha256, build, scan ELF)
  verify [PKG...]          Check installed files against the recorded files.idx hashes
  status [--full]          Show declared providers, channels, and database state
  test-suite               Run internal engine self-test suite

Global Options:
  --root, --sysroot <DIR>  Operate on target root directory (default: /)
  --dry-run                Simulate actions without modifying filesystem
  --verbose, -v            Enable verbose diagnostics
  --no-elf-check           Skip build-time DT_NEEDED validation (bootstrap escape hatch)
  --channel <NAME>         Restrict `install` to a single channel
  --help, -h               Show this help message
  --version, -V            Show version information
)");
}

// ============================================================================
// Enhanced `sage build` Implementation
// ============================================================================

int cmd_build(const CliOptions& opts) {
    if (opts.args.empty()) {
        std::println("Usage: sage build <RECIPE_DIR>");
        return 1;
    }
    std::filesystem::path recipe_dir = opts.args[0];
    std::filesystem::path recipe_file = recipe_dir / "recipe.toml";
    if (!std::filesystem::exists(recipe_file)) {
        sage::util::log_error("recipe.toml not found in directory: {}", recipe_dir.string());
        return 1;
    }

    // Make the recipe directory absolute before anything derives from it.
    // The build phases run with the working directory changed to src/ (or the
    // recipe directory), so a relative DESTDIR such as "./foo/pkg" would be
    // resolved against *that* directory instead of sage's own -- the install
    // phase writes into a phantom nested path, packing then finds pkg/ empty,
    // and the package ships with no payload and no error anywhere.
    {
        std::error_code ec;
        auto abs = std::filesystem::canonical(recipe_dir, ec);
        if (ec) {
            sage::util::log_error("Cannot resolve recipe directory '{}': {}", recipe_dir.string(), ec.message());
            return 1;
        }
        recipe_dir = std::move(abs);
        recipe_file = recipe_dir / "recipe.toml";
    }

    std::ifstream rf(recipe_file);
    std::stringstream ss;
    ss << rf.rdbuf();
    auto recipe_res = sage::package::Recipe::parse_toml(ss.str());
    if (!recipe_res) {
        sage::util::log_error("Failed to parse recipe: {}", recipe_res.error());
        return 1;
    }
    const auto& r = *recipe_res;
    sage::util::log_info("Building package '{}' version {} (channel: {})...", r.name, r.version.to_string(), r.channel);

    std::filesystem::path dist_dir = recipe_dir / "distfiles";
    std::filesystem::path src_dir = recipe_dir / "src";
    std::filesystem::path pkg_dir = recipe_dir / "pkg";

    // 1. Source Fetch & SHA256 Verification
    if (!r.source_url.empty()) {
        std::filesystem::create_directories(dist_dir);
        std::string filename = std::filesystem::path(r.source_url).filename().string();
        if (filename.empty()) filename = "source.tar.gz";
        std::filesystem::path archive_path = dist_dir / filename;

        if (!std::filesystem::exists(archive_path)) {
            sage::util::log_info("Fetching source archive from {}...", r.source_url);
            auto dl_res = sage::vendor::curl::download_file(r.source_url, archive_path);
            if (!dl_res) {
                sage::util::log_error("Failed to download source: {}", dl_res.error());
                return 1;
            }
        }

        // Verify SHA256
        if (!r.source_sha256.empty()) {
            auto hash_res = sage::util::compute_file_sha256(archive_path);
            if (!hash_res) {
                sage::util::log_error("Failed to compute SHA256 for source: {}", hash_res.error());
                return 1;
            }
            if (*hash_res != r.source_sha256) {
                sage::util::log_error("SHA256 checksum mismatch!\n  Expected: {}\n  Actual:   {}", r.source_sha256, *hash_res);
                return 1;
            }
            sage::util::log_success("Source SHA256 checksum verified: {}", *hash_res);
        }

        // Unpack source if src_dir doesn't exist
        if (!std::filesystem::exists(src_dir)) {
            std::filesystem::create_directories(src_dir);
            sage::util::log_info("Unpacking source to {}...", src_dir.string());
            std::string cmd = std::format("tar -xf \"{}\" -C \"{}\" --strip-components=1 2>/dev/null || tar -xf \"{}\" -C \"{}\"", 
                archive_path.string(), src_dir.string(), archive_path.string(), src_dir.string());
            int ret = std::system(cmd.c_str());
            if (ret != 0) {
                sage::util::log_error("Failed to unpack source archive! Archive may be corrupted. Cleaning up...");
                std::error_code ec;
                std::filesystem::remove(archive_path, ec);
                std::filesystem::remove_all(src_dir, ec);
                return 1;
            }
        }
    }

    // 2. Prepare, Build & Install Phases
    std::error_code ec;
    std::filesystem::remove_all(pkg_dir, ec);
    std::filesystem::create_directories(pkg_dir);
    std::filesystem::path work_dir = std::filesystem::exists(src_dir) ? src_dir : recipe_dir;

    auto run_phase = [&](std::string_view phase_name, const std::vector<std::string>& cmds) -> bool {
        if (cmds.empty()) return true;
        sage::util::log_info("Executing {} phase...", phase_name);
        for (const auto& cmd_line : cmds) {
            std::string full_cmd = std::format("export DESTDIR=\"{}\" PREFIX=\"/usr\" RECIPE_DIR=\"{}\" SRCDIR=\"{}\" PKGDIR=\"{}\"; cd \"{}\" && {}", 
                pkg_dir.string(), recipe_dir.string(), src_dir.string(), pkg_dir.string(), work_dir.string(), cmd_line);
            int ret = std::system(full_cmd.c_str());
            if (ret != 0) {
                sage::util::log_error("Command failed in {} phase: {}", phase_name, cmd_line);
                return false;
            }
        }
        return true;
    };

    if (!run_phase("prepare", r.prepare_cmds)) return 1;
    if (!run_phase("build", r.build_cmds)) return 1;
    if (!run_phase("install", r.install_cmds)) return 1;

    // 3. Automated ELF Scanner for DT_SONAME & DT_NEEDED
    sage::package::PackageManifest manifest;
    manifest.name = r.name;
    manifest.version = r.version;
    manifest.description = r.description;
    manifest.license = r.license;
    manifest.channel = r.channel;
    manifest.dependencies = r.host_deps;
    manifest.provides = r.provides;
    manifest.arch = r.arch;
    manifest.capability_hooks = r.capability_hooks;
    manifest.triggers = r.triggers;

    // Every soname this package satisfies by itself, and every soname it still
    // needs from elsewhere -- remembering which file asked, so a failure can
    // name the offender rather than just the missing library.
    std::set<std::string> self_sonames;
    std::set<std::string> needed_sonames;
    std::map<std::string, std::vector<std::string>> needed_by;

    if (std::filesystem::exists(pkg_dir)) {
        for (const auto& entry : std::filesystem::recursive_directory_iterator(pkg_dir, std::filesystem::directory_options::skip_permission_denied)) {
            auto rel = entry.path().lexically_relative(pkg_dir).generic_string();

            // A soname is normally reached through the versioned symlink the
            // package installs next to the real file (libz.so.1 ->
            // libz.so.1.3.1). Skipping symlinks meant those names never made
            // it into `provides`, which is why repository indexes ended up
            // full of so: constraints nothing satisfied.
            if (entry.is_symlink()) {
                auto base = entry.path().filename().string();
                if (base.starts_with("lib") && base.find(".so") != std::string::npos) {
                    self_sonames.insert(base);
                }
                continue;
            }
            if (!entry.is_regular_file()) continue;

            auto base = entry.path().filename().string();
            if (base.starts_with("lib") && base.find(".so") != std::string::npos) {
                self_sonames.insert(base);
            }

            auto elf_res = sage::util::scan_elf(entry.path());
            if (!elf_res) continue;
            if (!elf_res->soname.empty()) {
                self_sonames.insert(elf_res->soname);
            }
            for (const auto& needed : elf_res->needed) {
                needed_sonames.insert(needed);
                needed_by[needed].push_back(rel);
            }
        }
    }

    for (const auto& soname : self_sonames) {
        manifest.provides.push_back("so:" + soname);
    }

    // A package does not depend on itself: a soname it installs is not an
    // external constraint, and emitting it as one makes the solver chase a
    // cycle through the package being built.
    std::vector<std::string> external_sonames;
    for (const auto& soname : needed_sonames) {
        if (!self_sonames.contains(soname)) external_sonames.push_back(soname);
    }
    for (const auto& soname : external_sonames) {
        manifest.dependencies.push_back(sage::package::Dependency::parse("so:" + soname));
    }

    // Deduplicate. The same soname is routinely reached from a dozen binaries
    // in one package, and a repeated provides entry makes the index unreadable.
    {
        std::unordered_set<std::string> seen;
        std::erase_if(manifest.provides, [&](const std::string& p) { return !seen.insert(p).second; });
    }
    {
        std::unordered_set<std::string> seen;
        std::erase_if(manifest.dependencies, [&](const sage::package::Dependency& d) {
            return !seen.insert(d.to_string()).second;
        });
    }

    // 3b. Validate every remaining DT_NEEDED against what is actually
    // installed. Without this the build happily links against a library that
    // only exists on the build host -- xfsprogs picking up the host's
    // libdevmapper -- and the failure surfaces at install time on a machine
    // that has no such file.
    if (!opts.no_elf_check && !external_sonames.empty()) {
        auto host_cfg = sage::config::SystemConfig::load_from_root(opts.target_root);
        auto host_db = host_cfg
            ? sage::db::Database::open(host_cfg->db_path, true)
            : std::expected<sage::db::Database, std::string>(std::unexpected("no config"));

        if (!host_db) {
            sage::util::log_warn("Cannot verify DT_NEEDED: no package database at '{}'. "
                "{} external soname(s) go unchecked -- expected while bootstrapping, a bug otherwise.",
                host_cfg ? host_cfg->db_path.string() : std::string("<unknown>"), external_sonames.size());
        } else {
            std::vector<std::string> unsatisfied;
            for (const auto& soname : external_sonames) {
                if (host_db->get_provider("so:" + soname)) continue;
                unsatisfied.push_back(soname);
            }
            if (!unsatisfied.empty()) {
                sage::util::log_error("Build linked against {} library/libraries no installed package provides:", unsatisfied.size());
                for (const auto& soname : unsatisfied) {
                    sage::util::log_error("  so:{}  needed by: {}", soname, sage::util::join(needed_by[soname], ", "));
                }
                sage::util::log_error("These came from the build host, not from the repository. Either package them, "
                    "or configure the build to not use them. Pass --no-elf-check to override.");
                return 1;
            }
        }
    }

    // 4. Archive Creation
    std::string out_name = std::format("{}-{}-{}-{}.pkg.tar.zst", r.name, r.version.ver, r.version.rel, manifest.arch);
    std::filesystem::path out_path = recipe_dir / out_name;
    auto pack_res = sage::archive::create_package(manifest, pkg_dir, out_path);
    if (!pack_res) {
        sage::util::log_error("Package packaging failed: {}", pack_res.error());
        return 1;
    }

    sage::util::log_success("Package built successfully: {}", out_path.string());
    return 0;
}

// ============================================================================
// End-to-End `sage install <PKG...>` Implementation
// ============================================================================

inline std::expected<std::optional<sage::package::PackageManifest>, std::string>
load_install_snapshot(
    sage::db::Database& db,
    auto& txn,
    std::string_view package_name,
    const std::optional<sage::package::PackageIdentity>& expected_identity)
{
    auto current = db.get_package(txn, package_name);
    if (!current) return std::unexpected(current.error());

    auto current_identity = *current
        ? std::optional{sage::package::package_identity(**current)}
        : std::nullopt;
    if (current_identity != expected_identity) {
        return std::unexpected(std::format(
            "Installed package '{}' changed after dependency resolution", package_name));
    }
    return current;
}

int cmd_install(const CliOptions& opts) {
    if (opts.args.empty()) {
        std::println("Usage: sage install <PKG...>");
        return 1;
    }

    auto cfg_res = sage::config::SystemConfig::load_from_root(opts.target_root);
    if (!cfg_res) {
        sage::util::log_error("Failed to load configuration: {}", cfg_res.error());
        return 1;
    }
    const auto& cfg = *cfg_res;

    auto db_res = sage::db::Database::open(cfg.db_path);
    if (!db_res) {
        sage::util::log_error("Failed to open database at {}: {}", cfg.db_path.string(), db_res.error());
        return 1;
    }
    auto& db = *db_res;
    auto installed_res = db.list_installed_packages();
    if (!installed_res) {
        sage::util::log_error("Installed package database is inconsistent: {}", installed_res.error());
        return 1;
    }
    auto installed_packages = std::move(*installed_res);

    // 1. Gather Available Package Pool from Channels and Local Repos
    std::vector<sage::package::PackageManifest> pool;
    std::map<sage::package::PackageIdentity, std::filesystem::path> package_archive_map;

    auto channel_configs = cfg.channels;
    std::ranges::stable_sort(channel_configs, std::greater{},
        &sage::config::ChannelConfig::priority);
    bool channel_filter_matched = opts.channel_filter.empty();
    for (const auto& ch_cfg : channel_configs) {
        if (!ch_cfg.enabled) continue;
        // --channel narrows the pool to one channel. Installed packages are
        // still added below, so a restricted install can still satisfy
        // constraints that are already met on the system.
        if (!opts.channel_filter.empty() && ch_cfg.name != opts.channel_filter) continue;
        channel_filter_matched = true;
        sage::channel::Channel ch;
        ch.name = ch_cfg.name;
        ch.url = ch_cfg.url;
        ch.scope = sage::channel::parse_scope(ch_cfg.scope);
        ch.priority = ch_cfg.priority;

        // Fetch channel index
        auto idx_res = sage::channel::ProfileManager::sync_channel(ch, cfg.cache_dir);
        if (idx_res) {
            for (const auto& pkg : idx_res->available_packages) {
                pool.push_back(pkg);
                // Map archive url / path
                std::filesystem::path dir_base;
                if (ch.url.starts_with("file://")) {
                    dir_base = std::filesystem::path(ch.url.substr(7));
                } else if (ch.url.starts_with("/")) {
                    dir_base = std::filesystem::path(ch.url);
                } else {
                    dir_base = cfg.cache_dir / "pkg";
                }

                std::filesystem::path local_p;
                if (!pkg.file.empty()) {
                    // Use the file field from index if present
                    local_p = dir_base / pkg.file;
                } else {
                    // Fallback to old naming scheme for backward compatibility
                    local_p = dir_base / std::format("{}-{}-{}-{}.pkg.tar.zst", pkg.name, pkg.version.ver, pkg.version.rel, pkg.arch);
                    if (!std::filesystem::exists(local_p)) {
                        local_p = dir_base / std::format("{}-{}-{}.pkg.tar.zst", pkg.name, pkg.version.ver, pkg.version.rel);
                    }
                    if (!std::filesystem::exists(local_p)) {
                        local_p = dir_base / std::format("{}-{}.pkg.tar.zst", pkg.name, pkg.version.ver);
                    }
                }
                // Equal-version solver ties keep the first candidate. Preserve
                // the same priority-ordered source here so metadata and payload
                // always come from one repository.
                package_archive_map.try_emplace(
                    sage::package::package_identity(pkg), std::move(local_p));
            }
        }
    }

    if (!channel_filter_matched) {
        sage::util::log_error("No enabled channel named '{}' is configured for '{}'",
            opts.channel_filter, opts.target_root.string());
        return 1;
    }

    // Include already-installed packages in the pool so that installed providers
    // (e.g. glibc providing so:libc.so.6) satisfy dependencies during bootstrap,
    // when the repository may not yet contain the providing package. Candidates
    // that are already installed are filtered out after solving.
    for (const auto& pkg : installed_packages) {
        pool.push_back(pkg);
    }

    // Check if any arguments are direct .pkg.tar.zst archive files
    std::vector<sage::package::Dependency> root_reqs;
    std::unordered_set<std::string> direct_package_names;
    for (const auto& arg : opts.args) {
        if (arg.ends_with(".pkg.tar.zst") && std::filesystem::exists(arg)) {
            auto inspect_res = sage::archive::inspect_package(arg);
            if (!inspect_res) {
                sage::util::log_error("Invalid package archive '{}': {}", arg, inspect_res.error());
                return 1;
            }
            if (!direct_package_names.insert(inspect_res->manifest.name).second) {
                sage::util::log_error(
                    "Multiple direct archives were provided for package '{}'",
                    inspect_res->manifest.name);
                return 1;
            }
            std::erase_if(pool, [&](const auto& candidate) {
                return candidate.name == inspect_res->manifest.name;
            });
            pool.push_back(inspect_res->manifest);
            package_archive_map[sage::package::package_identity(inspect_res->manifest)] =
                std::filesystem::absolute(arg);
            root_reqs.push_back(sage::package::Dependency{
                .name = inspect_res->manifest.name,
                .op = sage::package::ConstraintOp::Equal,
                .version = inspect_res->manifest.version,
            });
        } else {
            root_reqs.push_back(sage::package::Dependency::parse(arg));
        }
    }

    // 2. PubGrub SAT Dependency Solver
    sage::solver::DependencySolver solver(pool, cfg.providers);
    auto solve_res = solver.solve(root_reqs);
    if (!solve_res) {
        sage::util::log_error("Dependency resolution failed:\n{}", solve_res.error());
        return 1;
    }

    std::vector<sage::package::PackageManifest> unique_to_install;
    std::unordered_set<std::string> seen_install_names;
    for (const auto& pkg : *solve_res) {
        if (seen_install_names.insert(pkg.name).second) {
            unique_to_install.push_back(pkg);
        }
    }

    // Filter out packages already installed at a satisfying version: the pool
    // includes installed packages purely as dependency providers, and the solver
    // may have selected them as candidates. Only repo-newer versions are installed
    // (upgrades); everything else is already satisfied by the current system.
    std::unordered_map<std::string, sage::package::PackageManifest> installed_by_name;
    for (auto& p : installed_packages) {
        installed_by_name.emplace(p.name, std::move(p));
    }
    std::vector<sage::package::PackageManifest> to_install;
    for (auto& pkg : unique_to_install) {
        auto it = installed_by_name.find(pkg.name);
        const bool exact_direct_request = direct_package_names.contains(pkg.name);
        if (!exact_direct_request
            && it != installed_by_name.end() && it->second.version >= pkg.version) {
            std::vector<std::string> db_files;
            if (it->second.files.empty()) {
                auto db_files_res = db.get_package_files(pkg.name);
                if (!db_files_res) {
                    sage::util::log_error(
                        "Failed to read files owned by '{}': {}", pkg.name, db_files_res.error());
                    return 1;
                }
                db_files = std::move(*db_files_res);
            }
            bool files_present = true;
            if (!it->second.files.empty()) {
                for (const auto& f : it->second.files) {
                    if (f.type != sage::package::FileType::Directory) {
                        if (!std::filesystem::exists(opts.target_root / f.path)) {
                            files_present = false;
                            break;
                        }
                    }
                }
            } else if (!db_files.empty()) {
                for (const auto& fpath : db_files) {
                    if (!std::filesystem::exists(opts.target_root / fpath)) {
                        files_present = false;
                        break;
                    }
                }
            } else {
                files_present = false;
            }
            if (files_present) {
                sage::util::log_info("  ~ {:<20} {:<15} [already installed]", pkg.name, pkg.version.to_string());
                continue;
            } else {
                sage::util::log_warn("  ! {:<20} {:<15} [files missing on disk, reinstalling]", pkg.name, pkg.version.to_string());
            }
        }
        to_install.push_back(std::move(pkg));
    }
    unique_to_install = std::move(to_install);

    // Enforce exclusive capabilities.
    //
    // An exclusive capability -- virtual/init, virtual/udev, virtual/libc --
    // is one where two providers on the same root cannot both work. Shared
    // capabilities are exempt by construction: two initramfs builders or two
    // kernels coexisting is the normal case, and rejecting them here is
    // exactly the mistake this distinction exists to prevent.
    {
        std::map<std::string, std::string> claimed;
        for (const auto& [name, pkg] : installed_by_name) {
            for (const auto& prov : pkg.provides) {
                if (cfg.is_exclusive_capability(prov)) claimed.emplace(prov, name);
            }
        }
        for (const auto& pkg : unique_to_install) {
            for (const auto& prov : pkg.provides) {
                if (!cfg.is_exclusive_capability(prov)) continue;
                auto [it, fresh] = claimed.emplace(prov, pkg.name);
                if (!fresh && it->second != pkg.name) {
                    sage::util::log_error("Exclusive capability '{}' would have two providers: '{}' and '{}'",
                        prov, it->second, pkg.name);
                    sage::util::log_error("Swap the provider through '{}' [providers] and 'sage rebuild', "
                        "or drop the capability to \"shared\" under [capabilities] if they really do coexist.",
                        cfg.system_config_path.string());
                    return 1;
                }
            }
        }
    }

    sage::util::log_info("Resolved {} packages to install into target root '{}':", unique_to_install.size(), opts.target_root.string());
    for (const auto& pkg : unique_to_install) {
        std::println("  + {:<20} {:<15} [{}]", pkg.name, pkg.version.to_string(), pkg.channel);
    }

    if (opts.dry_run) {
        sage::util::log_info("Dry-run preview completed successfully (no changes written).");
        return 0;
    }

    std::map<sage::package::PackageIdentity, sage::archive::InspectedPackage> inspected_packages;
    for (const auto& pkg : unique_to_install) {
        const auto identity = sage::package::package_identity(pkg);
        auto archive_it = package_archive_map.find(identity);
        if (archive_it == package_archive_map.end() || !std::filesystem::exists(archive_it->second)) {
            sage::util::log_error("Package archive for '{}' not found at {}", pkg.name,
                (archive_it != package_archive_map.end()) ? archive_it->second.string() : "<unknown>");
            return 1;
        }
        auto inspect_res = sage::archive::inspect_package(archive_it->second);
        if (!inspect_res) {
            sage::util::log_error(
                "Invalid package archive for '{}': {}", pkg.name, inspect_res.error());
            return 1;
        }
        if (sage::package::package_identity(inspect_res->manifest) != identity) {
            sage::util::log_error(
                "Package archive identity does not match selected package '{} {} [{}; {}]'",
                pkg.name, pkg.version.to_string(), pkg.arch, pkg.channel);
            return 1;
        }
        inspected_packages.emplace(identity, std::move(*inspect_res));
    }

    // Self-heal: prune file registrations owned by packages that are no longer
    // installed (leftovers from previous versions with incomplete file lists),
    // so the paths can be claimed by the packages about to be installed.
    auto prune_txn = db.begin_write_txn();
    if (!prune_txn) {
        sage::util::log_error("Failed to open orphan-pruning transaction: {}", prune_txn.error());
        return 1;
    }
    auto pruned = db.prune_orphaned_files(*prune_txn);
    if (!pruned) {
        sage::util::log_error("Failed to prune orphaned file registrations: {}", pruned.error());
        return 1;
    }
    auto prune_commit = prune_txn->commit();
    if (!prune_commit) {
        sage::util::log_error("Failed to commit orphan-pruning transaction: {}", prune_commit.error());
        return 1;
    }
    if (*pruned > 0) {
        sage::util::log_info("  ~ pruned {} orphaned file registration(s)", *pruned);
    }

    std::vector<sage::channel::Channel> active_channels;
    for (const auto& ch_cfg : cfg.channels) {
        sage::channel::Channel ch;
        ch.name = ch_cfg.name;
        ch.scope = sage::channel::parse_scope(ch_cfg.scope);
        ch.enabled = ch_cfg.enabled;
        active_channels.push_back(std::move(ch));
    }
    std::vector<sage::package::FileEntry> all_installed_files;
    std::set<std::string> completed_trigger_commands;

    auto run_package_postprocessing = [&](const sage::package::PackageManifest& installed_pkg) {
        auto spec = sage::channel::SubChannelSpec::parse(installed_pkg.channel);
        if (spec.scope == sage::channel::ChannelScope::Toolchain
            && !spec.category.empty() && !spec.slot.empty()) {
            auto act_res = sage::channel::ProfileManager::switch_active_toolchain(
                opts.target_root, spec.category, spec.slot);
            if (!act_res) {
                sage::util::log_warn(
                    "Failed to activate toolchain '{}:{}': {}",
                    spec.category, spec.slot, act_res.error());
            }
        }

        (void)sage::channel::ProfileManager::regenerate_fhs_profile(
            opts.target_root, active_channels);
        auto current_packages = db.list_installed_packages();
        if (!current_packages) {
            sage::util::log_warn(
                "Skipping post-install triggers for '{}': {}",
                installed_pkg.name, current_packages.error());
            return;
        }
        sage::rebuild::TriggerContext trigger_context;
        trigger_context.sysroot = opts.target_root;
        trigger_context.touched_files = installed_pkg.files;
        trigger_context.transaction_packages = {installed_pkg};
        trigger_context.installed_packages = std::move(*current_packages);
        trigger_context.providers = cfg.providers;
        sage::rebuild::TriggerEngine::run(
            trigger_context, completed_trigger_commands);
    };

    // 3. Streaming Unpack & LMDB State Registration
    for (const auto& pkg : unique_to_install) {
        const auto identity = sage::package::package_identity(pkg);
        auto archive_it = package_archive_map.find(identity);
        auto inspected_it = inspected_packages.find(identity);
        auto package_txn = db.begin_write_txn();
        if (!package_txn) {
            sage::util::log_error("Failed to open database transaction for '{}': {}", pkg.name, package_txn.error());
            return 1;
        }

        auto expected_it = installed_by_name.find(pkg.name);
        auto expected_previous_identity = expected_it != installed_by_name.end()
            ? std::optional{sage::package::package_identity(expected_it->second)}
            : std::nullopt;
        auto previous_package = load_install_snapshot(
            db, *package_txn, pkg.name, expected_previous_identity);
        if (!previous_package) {
            sage::util::log_error(
                "Cannot install package '{}': {}", pkg.name, previous_package.error());
            return 1;
        }

        std::optional<std::string> previous_owner;
        std::vector<std::string> previous_paths;
        if (*previous_package) {
            previous_owner = std::format("{}:{}", pkg.name, (**previous_package).channel);
            auto previous_paths_res = db.get_package_files(*package_txn, pkg.name);
            if (!previous_paths_res) {
                sage::util::log_error(
                    "Failed to read previous files for '{}': {}",
                    pkg.name, previous_paths_res.error());
                return 1;
            }
            previous_paths = std::move(*previous_paths_res);
            for (const auto& path : previous_paths) {
                auto owner = db.get_file_owner(*package_txn, path);
                if (!owner) {
                    sage::util::log_error(
                        "Failed to read ownership for '{}': {}", path, owner.error());
                    return 1;
                }
                if (!*owner || **owner != *previous_owner) {
                    sage::util::log_error(
                        "Cannot migrate package '{}': file '{}' is owned by '{}' instead of '{}'",
                        pkg.name, path, *owner ? **owner : "<none>", *previous_owner);
                    return 1;
                }
            }
        }

        auto conflict_res = db.check_file_conflicts(
            *package_txn,
            previous_owner
                ? std::optional<std::string_view>{*previous_owner}
                : std::nullopt,
            inspected_it->second.data_files);
        if (!conflict_res) {
            sage::util::log_error(
                "Cannot install package '{}': {}", pkg.name, conflict_res.error());
            return 1;
        }

        // Archives always contain paths relative to sysroot (e.g. usr/bin/bash or
        // opt/channels/gcc/15/bin/gcc), so always extract to the target root directly.
        sage::util::log_info("Unpacking {} -> {}...", pkg.name, opts.target_root.string());
        auto ext_res = sage::archive::extract_package(
            archive_it->second, opts.target_root, &pkg, &inspected_it->second);
        if (!ext_res) {
            sage::util::log_error("Failed to extract package '{}': {}", pkg.name, ext_res.error());
            return 1;
        }

        auto installed_pkg = pkg;
        installed_pkg.files = ext_res->extracted_files;

        // The channel index is a solving summary: it carries names, versions,
        // dependencies and provides, but not capability hooks or triggers.
        // Those live in the archive's own manifest, and the post-transaction
        // trigger pass reads them back out of the database -- so adopt them
        // here, or an installed initramfs generator would be invisible to the
        // very trigger that has to run it.
        installed_pkg.capability_hooks = ext_res->manifest.capability_hooks;
        installed_pkg.triggers = ext_res->manifest.triggers;

        // Reinstall/upgrade cleanup: remove physical files owned by a previously
        // installed package that are not part of the new payload, so file
        // ownership can transition to other packages (e.g. split -dev/-libs
        // children claim headers/libs the old monolithic version used to own).
        if (*previous_package) {
            std::unordered_set<std::string> new_paths;
            for (const auto& f : installed_pkg.files) {
                new_paths.insert(sage::util::clean_rel_path(f.path));
            }
            std::vector<sage::package::FileEntry> stale;
            const auto& old_owner = *previous_owner;
            for (const auto& old_path : previous_paths) {
                if (new_paths.contains(old_path)) continue;
                auto cur_owner = db.get_file_owner(*package_txn, old_path);
                if (!cur_owner) {
                    sage::util::log_error(
                        "Failed to read ownership for '{}': {}", old_path, cur_owner.error());
                    return 1;
                }
                if (*cur_owner && **cur_owner != old_owner) {
                    continue;
                }
                auto remove_res = sage::archive::remove_path_anchored(
                    opts.target_root, old_path);
                if (!remove_res) {
                    sage::util::log_error(
                        "Failed to remove stale file '{}' for '{}': {}",
                        old_path, pkg.name, remove_res.error());
                    return 1;
                }
                sage::package::FileEntry fe;
                fe.path = old_path;
                stale.push_back(std::move(fe));
            }
            if (!stale.empty()) {
                auto unregister_res = db.unregister_files(*package_txn, stale, old_owner);
                if (!unregister_res) {
                    sage::util::log_error(
                        "Failed to unregister stale files for '{}': {}",
                        pkg.name, unregister_res.error());
                    return 1;
                }
                sage::util::log_info("  ~ removed {} stale file(s) from previous {} {}", 
                    stale.size(), pkg.name, (**previous_package).version.to_string());
            }
        }

        auto p_res = db.put_package(*package_txn, installed_pkg);
        if (!p_res) {
            sage::util::log_error("Failed to register package '{}' in DB: {}", installed_pkg.name, p_res.error());
            return 1;
        }
        auto f_res = db.register_files(
            *package_txn,
            installed_pkg.name,
            installed_pkg.channel,
            installed_pkg.files,
            previous_owner
                ? std::optional<std::string_view>{*previous_owner}
                : std::nullopt);
        if (!f_res) {
            sage::util::log_error("Failed to register files for '{}': {}", installed_pkg.name, f_res.error());
            return 1;
        }
        auto prov_res = db.register_provides(*package_txn, installed_pkg.name, installed_pkg.provides);
        if (!prov_res) {
            sage::util::log_error("Failed to register provides for '{}': {}", installed_pkg.name, prov_res.error());
            return 1;
        }

        auto package_commit = package_txn->commit();
        if (!package_commit) {
            sage::util::log_error("Failed to commit package '{}': {}", installed_pkg.name, package_commit.error());
            return 1;
        }

        // A later package may fail, so complete all post-processing for this
        // committed package before advancing to the next archive.
        run_package_postprocessing(installed_pkg);
        all_installed_files.insert(
            all_installed_files.end(), installed_pkg.files.begin(), installed_pkg.files.end());
    }

    // Re-run aggregate triggers after the complete install set. A trigger tool
    // may itself have arrived after an earlier package first requested it.
    // Runs AFTER toolchain activation so freshly written
    // /etc/ld.so.conf.d/sage-*.conf entries are picked up by ldconfig, and
    // after the DB commit so a capability installed in this very transaction
    // (mkinitcpio arriving alongside the kernel that needs it) is already
    // visible when the initramfs trigger looks for its provider.
    sage::rebuild::TriggerContext trig_ctx;
    trig_ctx.sysroot = opts.target_root;
    trig_ctx.touched_files = all_installed_files;
    trig_ctx.transaction_packages = unique_to_install;
    if (auto current_packages = db.list_installed_packages()) {
        trig_ctx.installed_packages = std::move(*current_packages);
    } else {
        sage::util::log_error(
            "Cannot run aggregate triggers: {}", current_packages.error());
        return 1;
    }
    trig_ctx.providers = cfg.providers;
    trig_ctx.dry_run = opts.dry_run;
    sage::rebuild::TriggerEngine::run(trig_ctx, completed_trigger_commands);

    sage::util::log_success("Successfully installed {} packages into {}", unique_to_install.size(), opts.target_root.string());
    return 0;
}

// ============================================================================
// End-to-End `sage remove <PKG...>` Implementation
// ============================================================================

int cmd_remove(const CliOptions& opts) {
    if (opts.args.empty()) {
        std::println("Usage: sage remove [--cascade|-c] [--nodeps|-d] <PKG...>");
        return 1;
    }

    auto cfg_res = sage::config::SystemConfig::load_from_root(opts.target_root);
    if (!cfg_res) {
        sage::util::log_error("Failed to load configuration: {}", cfg_res.error());
        return 1;
    }
    const auto& cfg = *cfg_res;

    auto db_res = sage::db::Database::open(cfg.db_path);
    if (!db_res) {
        sage::util::log_error("Failed to open database: {}", db_res.error());
        return 1;
    }
    auto& db = *db_res;

    auto all_installed = db.list_installed_packages();
    if (!all_installed) {
        sage::util::log_error("Installed package database is inconsistent: {}", all_installed.error());
        return 1;
    }
    std::map<std::string, sage::package::PackageManifest> installed_map;
    for (const auto& pkg : *all_installed) {
        installed_map[pkg.name] = pkg;
    }

    std::set<std::string> to_remove_set;
    for (const auto& pkg_name : opts.args) {
        if (installed_map.contains(pkg_name)) {
            to_remove_set.insert(pkg_name);
        } else {
            sage::util::log_warn("Package '{}' is not installed, skipping", pkg_name);
        }
    }

    if (to_remove_set.empty()) {
        sage::util::log_info("No matching installed packages found to remove.");
        return 0;
    }

    // 1. Core System Provider Protection (unless --nodeps/--force is specified)
    if (!opts.force) {
        for (const auto& pkg_name : to_remove_set) {
            for (const auto& [iface, prov_target] : cfg.providers) {
                if (pkg_name == prov_target) {
                    sage::util::log_error("Cannot remove core system package '{}' (active provider for interface '{}').", pkg_name, iface);
                    sage::util::log_info("Tip: update /etc/sage/system.toml and run 'sage rebuild' to swap providers, or pass '--nodeps' to bypass.");
                    return 1;
                }
            }
        }
    }

    // 2. Cascade Expansion OR Reverse Dependency Protection Check
    if (opts.cascade) {
        // Cascade: recursively add all packages that depend on anything in to_remove_set
        bool cascaded = true;
        while (cascaded) {
            cascaded = false;
            for (const auto& [inst_name, inst_pkg] : installed_map) {
                if (to_remove_set.contains(inst_name)) continue;
                for (const auto& dep : inst_pkg.dependencies) {
                    for (const auto& r_pkg_name : to_remove_set) {
                        const auto& r_pkg = installed_map.at(r_pkg_name);
                        bool match = (dep.name == r_pkg.name);
                        if (!match) {
                            for (const auto& prov : r_pkg.provides) {
                                if (prov == dep.name || prov.starts_with(dep.name + " ")) {
                                    match = true;
                                    break;
                                }
                            }
                        }
                        if (match && dep.satisfies(r_pkg.version)) {
                            to_remove_set.insert(inst_name);
                            cascaded = true;
                            break;
                        }
                    }
                    if (cascaded) break;
                }
            }
        }
    } else if (!opts.force) {
        // Reverse Dependency Protection: ensure no remaining package depends on the package(s) being removed
        std::map<std::string, std::vector<std::string>> broken_deps;

        for (const auto& r_pkg_name : to_remove_set) {
            const auto& r_pkg = installed_map.at(r_pkg_name);
            for (const auto& [inst_name, inst_pkg] : installed_map) {
                if (to_remove_set.contains(inst_name)) continue;

                for (const auto& dep : inst_pkg.dependencies) {
                    bool match = (dep.name == r_pkg.name);
                    if (!match) {
                        for (const auto& prov : r_pkg.provides) {
                            if (prov == dep.name || prov.starts_with(dep.name + " ")) {
                                match = true;
                                break;
                            }
                        }
                    }
                    if (match && dep.satisfies(r_pkg.version)) {
                        // Check if another remaining package satisfies this dependency (alternative provider)
                        bool alt_available = false;
                        for (const auto& [alt_name, alt_pkg] : installed_map) {
                            if (to_remove_set.contains(alt_name) || alt_name == r_pkg_name) continue;
                            bool alt_match = (dep.name == alt_pkg.name);
                            if (!alt_match) {
                                for (const auto& prov : alt_pkg.provides) {
                                    if (prov == dep.name || prov.starts_with(dep.name + " ")) {
                                        alt_match = true;
                                        break;
                                    }
                                }
                            }
                            if (alt_match && dep.satisfies(alt_pkg.version)) {
                                alt_available = true;
                                break;
                            }
                        }

                        if (!alt_available) {
                            broken_deps[r_pkg_name].push_back(std::format("{} (requires '{}')", inst_name, dep.to_string()));
                        }
                    }
                }
            }
        }

        if (!broken_deps.empty()) {
            sage::util::log_error("Failed to prepare transaction: breaking dependencies for installed packages");
            for (const auto& [pkg_name, requirers] : broken_deps) {
                std::println(std::cerr, "  :: Unable to remove '{}', required by:", pkg_name);
                for (const auto& req : requirers) {
                    std::println(std::cerr, "     - {}", req);
                }
            }
            std::println(std::cerr, "Tip: use 'sage remove --cascade <PKG>' to remove dependent packages as well, or '--nodeps' to force removal.");
            return 1;
        }
    }

    // 3. Iteratively discover orphaned dependencies
    if (!opts.no_recursive) {
        bool changed = true;
        while (changed) {
            changed = false;
            for (const auto& r_pkg_name : to_remove_set) {
                const auto& r_pkg = installed_map.at(r_pkg_name);
                for (const auto& dep : r_pkg.dependencies) {
                    for (const auto& [inst_name, inst_pkg] : installed_map) {
                        if (to_remove_set.contains(inst_name)) continue;

                        bool match = (inst_name == dep.name);
                        if (!match) {
                            for (const auto& prov : inst_pkg.provides) {
                                if (prov == dep.name || prov.starts_with(dep.name + " ")) {
                                    match = true;
                                    break;
                                }
                            }
                        }

                        if (match && dep.satisfies(inst_pkg.version)) {
                            // Check if any remaining installed package outside to_remove_set still needs inst_name
                            bool needed_by_others = false;
                            for (const auto& [other_name, other_pkg] : installed_map) {
                                if (to_remove_set.contains(other_name) || other_name == inst_name) continue;
                                for (const auto& other_dep : other_pkg.dependencies) {
                                    bool other_match = (other_dep.name == inst_name);
                                    if (!other_match) {
                                        for (const auto& prov : other_pkg.provides) {
                                            if (prov == other_dep.name || prov.starts_with(other_dep.name + " ")) {
                                                other_match = true;
                                                break;
                                            }
                                        }
                                    }
                                    if (other_match && other_dep.satisfies(inst_pkg.version)) {
                                        needed_by_others = true;
                                        break;
                                    }
                                }
                                if (needed_by_others) break;
                            }

                            // Also protect virtual system provider locks (e.g. virtual/init, virtual/libc)
                            for (const auto& [iface, prov_target] : cfg.providers) {
                                if (inst_name == prov_target) {
                                    needed_by_others = true;
                                    break;
                                }
                            }

                            if (!needed_by_others) {
                                to_remove_set.insert(inst_name);
                                changed = true;
                                break;
                            }
                        }
                    }
                }
            }
        }
    }

    if (opts.dry_run) {
        sage::util::log_info("Dry-run removal preview for {} packages (including orphaned dependencies):", to_remove_set.size());
        for (const auto& name : to_remove_set) {
            std::println("  - {}", name);
        }
        return 0;
    }

    auto wtxn = db.begin_write_txn();
    if (!wtxn) return 1;

    // The dependency/removal plan was built before taking the writer lock.
    // Abort if any package was installed, removed, or replaced in that window;
    // otherwise stale plans can delete a newer same-name package or break a
    // dependency introduced concurrently.
    auto current_installed = db.list_installed_packages(*wtxn);
    if (!current_installed) {
        sage::util::log_error(
            "Failed to revalidate installed packages before removal: {}",
            current_installed.error());
        return 1;
    }
    if (current_installed->size() != installed_map.size()) {
        sage::util::log_error(
            "Installed package state changed while planning removal; retry the command");
        return 1;
    }
    for (const auto& current : *current_installed) {
        auto planned = installed_map.find(current.name);
        if (planned == installed_map.end()
            || planned->second.serialize_toml() != current.serialize_toml()) {
            sage::util::log_error(
                "Installed package '{}' changed while planning removal; retry the command",
                current.name);
            return 1;
        }
    }

    std::vector<sage::package::FileEntry> removed_files;

    for (const auto& pkg_name : to_remove_set) {
        const auto& pkg = installed_map.at(pkg_name);
        if (std::find(opts.args.begin(), opts.args.end(), pkg_name) != opts.args.end()) {
            sage::util::log_info("Removing package '{}'...", pkg_name);
        } else {
            sage::util::log_info("Auto-removing orphaned dependency '{}' (version {})...", pkg_name, pkg.version.to_string());
        }

        // Delete physical files. The LMDB files table is the authoritative owner
        // registry: merge the installed manifest's file list with all files still
        // registered to this package (a previous version's leftovers may not be
        // present in the current manifest), so stale ownership records are purged.
        auto files_to_delete = pkg.files;
        std::unordered_set<std::string> seen_paths;
        for (const auto& f : files_to_delete) {
            seen_paths.insert(sage::util::clean_rel_path(f.path));
        }
        auto registered_files = db.get_package_files(*wtxn, pkg_name);
        if (!registered_files) {
            sage::util::log_error(
                "Failed to read files owned by '{}': {}", pkg_name, registered_files.error());
            return 1;
        }
        for (const auto& fp : *registered_files) {
            if (seen_paths.insert(fp).second) {
                sage::package::FileEntry fe;
                fe.path = fp;
                files_to_delete.push_back(std::move(fe));
            }
        }
        auto path_depth = [](std::string_view path) {
            const auto relative = std::filesystem::path(sage::util::clean_rel_path(path));
            return static_cast<size_t>(std::distance(relative.begin(), relative.end()));
        };
        std::ranges::stable_sort(files_to_delete, [&](const auto& lhs, const auto& rhs) {
            return path_depth(lhs.path) > path_depth(rhs.path);
        });

        std::string my_owner = std::format("{}:{}", pkg_name, pkg.channel);
        for (const auto& file_entry : files_to_delete) {
            auto cur_owner = db.get_file_owner(*wtxn, file_entry.path);
            if (!cur_owner) {
                sage::util::log_error(
                    "Failed to read ownership for '{}': {}",
                    file_entry.path, cur_owner.error());
                return 1;
            }
            if (*cur_owner && **cur_owner != my_owner) {
                continue;
            }

            auto relative_path = std::filesystem::path(
                sage::util::clean_rel_path(file_entry.path)).lexically_normal();
            bool escapes_root = relative_path.empty() || relative_path == "."
                || relative_path.is_absolute() || relative_path.has_root_name()
                || relative_path.has_root_directory();
            for (const auto& component : relative_path) {
                if (component == "..") {
                    escapes_root = true;
                    break;
                }
            }
            if (escapes_root) {
                sage::util::log_error(
                    "Refusing to remove invalid package path '{}' for '{}'",
                    file_entry.path, pkg_name);
                return 1;
            }

            auto remove_res = sage::archive::remove_path_anchored(
                opts.target_root,
                relative_path.generic_string(),
                !*cur_owner);
            if (!remove_res) {
                sage::util::log_error(
                    "Failed to remove '{}' from package '{}': {}",
                    relative_path.generic_string(), pkg_name, remove_res.error());
                return 1;
            }

            auto removed_entry = file_entry;
            removed_entry.path = relative_path.generic_string();
            removed_files.push_back(std::move(removed_entry));
        }

        auto unregister_files = db.unregister_files(*wtxn, files_to_delete, my_owner);
        if (!unregister_files) {
            sage::util::log_error(
                "Failed to unregister files for '{}': {}", pkg_name, unregister_files.error());
            return 1;
        }
        auto unregister_provides = db.unregister_provides(*wtxn, pkg.provides);
        if (!unregister_provides) {
            sage::util::log_error(
                "Failed to unregister provides for '{}': {}",
                pkg_name, unregister_provides.error());
            return 1;
        }
        auto delete_package = db.del_package(*wtxn, pkg_name);
        if (!delete_package) {
            sage::util::log_error(
                "Failed to delete package record for '{}': {}",
                pkg_name, delete_package.error());
            return 1;
        }
    }

    auto commit_res = wtxn->commit();
    if (!commit_res) return 1;

    // Removing a kernel is as much a reason to regenerate the initramfs and
    // the bootloader entries as installing one, so removal runs the same
    // triggers -- with the removed packages as the transaction set, since it
    // is their capabilities that make the kernel triggers fire.
    sage::rebuild::TriggerContext trig_ctx;
    trig_ctx.sysroot = opts.target_root;
    trig_ctx.touched_files = removed_files;
    auto remaining_packages = db.list_installed_packages();
    if (!remaining_packages) {
        sage::util::log_error(
            "Cannot run post-remove triggers: {}", remaining_packages.error());
        return 1;
    }
    trig_ctx.installed_packages = std::move(*remaining_packages);
    trig_ctx.providers = cfg.providers;
    trig_ctx.dry_run = opts.dry_run;
    for (const auto& [name, pkg] : installed_map) {
        if (to_remove_set.contains(name)) trig_ctx.transaction_packages.push_back(pkg);
    }
    sage::rebuild::TriggerEngine::run(trig_ctx);
    std::vector<sage::channel::Channel> active_channels;
    for (const auto& ch_cfg : cfg.channels) {
        sage::channel::Channel ch;
        ch.name = ch_cfg.name;
        ch.scope = sage::channel::parse_scope(ch_cfg.scope);
        ch.enabled = ch_cfg.enabled;
        active_channels.push_back(std::move(ch));
    }
    (void)sage::channel::ProfileManager::regenerate_fhs_profile(opts.target_root, active_channels);

    sage::util::log_success("Successfully removed {} packages (including orphaned dependencies) from {}", 
        to_remove_set.size(), opts.target_root.string());
    return 0;
}

// ============================================================================
// Other Commands & Test Suite
// ============================================================================

int cmd_query(const CliOptions& opts);

int cmd_test_suite() {
    sage::util::log_info("Running Sage Master Architecture & Subsystem Integration Test Suite...");

    // 1. Semantic Versioning Test
    auto v1 = sage::package::Version::parse("1.2.3-1");
    auto v2 = sage::package::Version::parse("1.2.4-1");
    if (!(v1 < v2)) {
        sage::util::log_error("Version comparator test failed");
        return 1;
    }
    auto legacy_config = sage::config::SystemConfig::parse_system_toml(
        "[providers]\ninit = \"openrc\"\n");
    auto shared_override = sage::config::SystemConfig::parse_system_toml(
        "[providers]\ninit = \"openrc\"\n\n[capabilities]\ninit = \"shared\"\n");
    if (!legacy_config
        || !legacy_config->is_exclusive_capability("virtual/init")
        || !legacy_config->is_exclusive_capability("virtual/udev")
        || !legacy_config->is_exclusive_capability("virtual/libc")
        || !shared_override
        || shared_override->is_exclusive_capability("virtual/init")) {
        sage::util::log_error("Legacy capability defaults or explicit override failed");
        return 1;
    }
    sage::util::log_success("1. Semantic & Alphanum Version Comparator OK");

    // 2. Tar+Zstd Archive Packaging & Streaming Extractor Test
    auto temp_dir = std::filesystem::temp_directory_path() / "sage_archive_test";
    std::filesystem::remove_all(temp_dir);
    const auto long_rel = std::filesystem::path(std::string(110, 'a')) / std::string(80, 'b');
    const auto boundary_rel = std::filesystem::path(std::string(100, 'e')) / std::string(49, 'f') / std::string(100, 'g');
    const auto boundary_empty_dir = std::filesystem::path(std::string(100, 'i'));
    const std::string boundary_link_target(100, 'h');
    auto populate_payload = [&](const std::filesystem::path& root, bool long_path_first) {
        auto write_long_path = [&] {
            std::filesystem::create_directories((root / long_rel).parent_path());
            std::ofstream(root / long_rel) << "long path payload\n";
            std::filesystem::create_directories((root / boundary_rel).parent_path());
            std::ofstream(root / boundary_rel) << "USTAR boundary payload\n";
        };
        auto write_dummy = [&] {
            std::filesystem::create_directories(root / "usr/bin");
            std::ofstream(root / "usr/bin/dummy") << "#!/bin/sh\necho 'hello sage'\n";
            std::filesystem::permissions(root / "usr/bin/dummy", std::filesystem::perms::owner_all | std::filesystem::perms::group_read | std::filesystem::perms::group_exec);
            std::filesystem::create_symlink(boundary_link_target, root / "usr/bin/link");
            std::filesystem::create_directories(root / boundary_empty_dir);
        };

        if (long_path_first) {
            write_long_path();
            write_dummy();
        } else {
            write_dummy();
            write_long_path();
        }
    };
    populate_payload(temp_dir / "data", true);
    populate_payload(temp_dir / "data_reordered", false);

    sage::package::PackageManifest manifest;
    manifest.name = "dummy-tool";
    manifest.version = sage::package::Version::parse("1.0.0-1");
    manifest.description = "Test \"mock\" tool with \\ path";
    manifest.license = "BSD-2-Clause";
    manifest.channel = "system";

    auto pkg_path = temp_dir / "dummy-tool-1.0.0-1-x86_64.pkg.tar.zst";
    auto pack_res = sage::archive::create_package(manifest, temp_dir / "data", pkg_path);
    if (!pack_res) {
        sage::util::log_error("Archive pack failed: {}", pack_res.error());
        return 1;
    }

    auto reordered_pkg_path = temp_dir / "dummy-tool-reordered.pkg.tar.zst";
    auto reordered_pack_res = sage::archive::create_package(manifest, temp_dir / "data_reordered", reordered_pkg_path);
    auto pkg_hash = sage::util::compute_file_sha256(pkg_path);
    auto reordered_pkg_hash = sage::util::compute_file_sha256(reordered_pkg_path);
    if (!reordered_pack_res || !pkg_hash || !reordered_pkg_hash || *pkg_hash != *reordered_pkg_hash) {
        sage::util::log_error("Archive reproducibility verification failed");
        return 1;
    }

    // The ownership preflight and extraction must refer to the same payload,
    // even when a replacement archive keeps the same package identity.
    auto preflight_package = sage::archive::inspect_package(pkg_path);
    auto replaced_payload = temp_dir / "replaced-payload";
    std::filesystem::create_directories(replaced_payload / "usr/bin");
    std::ofstream(replaced_payload / "usr/bin/injected") << "must not extract\n";
    auto replacement_archive = temp_dir / "replacement-same-identity.pkg.tar.zst";
    auto replacement_pack = sage::archive::create_package(
        manifest, replaced_payload, replacement_archive);
    auto binding_root = temp_dir / "binding-root";
    auto binding_result = preflight_package && replacement_pack
        ? sage::archive::extract_package(
            replacement_archive, binding_root, &manifest, &*preflight_package)
        : std::expected<sage::archive::ExtractedPackage, std::string>(
            std::unexpected("fixture failed"));
    if (binding_result || std::filesystem::exists(binding_root / "usr/bin/injected")) {
        sage::util::log_error("Archive replacement bypassed ownership preflight");
        return 1;
    }

    // Cleanup follows the same anchored traversal as extraction. An
    // intermediate symlink must never redirect unlink outside the sysroot.
    auto anchored_remove_root = temp_dir / "anchored-remove-root";
    auto anchored_remove_outside = temp_dir / "anchored-remove-outside";
    std::filesystem::create_directories(anchored_remove_root / "opt");
    std::filesystem::create_directories(anchored_remove_outside);
    std::ofstream(anchored_remove_outside / "keep") << "must survive\n";
    std::filesystem::create_directory_symlink(
        anchored_remove_outside, anchored_remove_root / "opt/app");
    auto anchored_remove = sage::archive::remove_path_anchored(
        anchored_remove_root, "opt/app/keep");
    if (anchored_remove || !std::filesystem::exists(anchored_remove_outside / "keep")) {
        sage::util::log_error("Anchored cleanup followed an intermediate symlink");
        return 1;
    }

    auto raw_tar_path = temp_dir / "dummy-tool.tar";
    auto decompress_cmd = std::format("zstd -dc \"{}\" > \"{}\"", pkg_path.string(), raw_tar_path.string());
    if (std::system(decompress_cmd.c_str()) != 0) {
        sage::util::log_error("Failed to decompress generated USTAR archive for inspection");
        return 1;
    }
    sage::archive::TarHeader first_header{};
    std::ifstream raw_tar(raw_tar_path, std::ios::binary);
    bool checksum_digits = true;
    if (!raw_tar.read(reinterpret_cast<char*>(&first_header), sizeof(first_header))) {
        sage::util::log_error("Failed to inspect generated USTAR header");
        return 1;
    }
    for (size_t i = 0; i < 6; ++i) {
        checksum_digits = checksum_digits && first_header.chksum[i] >= '0' && first_header.chksum[i] <= '7';
    }
    if (std::memcmp(first_header.magic, "ustar\0", 6) != 0 ||
        std::memcmp(first_header.version, "00", 2) != 0 || !checksum_digits ||
        first_header.chksum[6] != '\0' || first_header.chksum[7] != ' ' ||
        sage::archive::parse_octal(first_header.chksum, sizeof(first_header.chksum)) !=
            sage::archive::compute_tar_checksum(first_header) ||
        sage::archive::parse_octal(first_header.mtime, sizeof(first_header.mtime)) != 1700000000) {
        sage::util::log_error("Generated POSIX USTAR header fields are not canonical");
        return 1;
    }

    auto malformed_archive_dir = temp_dir / "malformed-archives";
    std::filesystem::create_directories(malformed_archive_dir);
    auto unrepresentable_root = temp_dir / "unrepresentable-path";
    std::filesystem::create_directories(unrepresentable_root);
    std::ofstream(unrepresentable_root / std::string(101, 'c')) << "must fail\n";
    auto unrepresentable_res = sage::archive::create_package(
        manifest, unrepresentable_root, malformed_archive_dir / "unrepresentable-path.pkg.tar.zst");
    if (unrepresentable_res || unrepresentable_res.error().find("Path cannot be represented") == std::string::npos) {
        sage::util::log_error("Unrepresentable USTAR path was silently accepted");
        return 1;
    }

    auto long_link_root = temp_dir / "long-link-target";
    std::filesystem::create_directories(long_link_root);
    std::error_code link_ec;
    std::filesystem::create_symlink(std::string(101, 'd'), long_link_root / "link", link_ec);
    if (link_ec) {
        sage::util::log_error("Failed to create long-link USTAR fixture: {}", link_ec.message());
        return 1;
    }
    auto long_link_res = sage::archive::create_package(
        manifest, long_link_root, malformed_archive_dir / "long-link-target.pkg.tar.zst");
    if (long_link_res || long_link_res.error().find("Link target cannot be represented") == std::string::npos) {
        sage::util::log_error("Unrepresentable USTAR link target was silently accepted");
        return 1;
    }

    auto extract_root = temp_dir / "sysroot";
    auto ext_res = sage::archive::extract_package(pkg_path, extract_root);
    if (!ext_res || !std::filesystem::exists(extract_root / "usr/bin/dummy") ||
        !std::filesystem::exists(extract_root / long_rel) || !std::filesystem::exists(extract_root / boundary_rel) ||
        !std::filesystem::is_directory(extract_root / boundary_empty_dir) ||
        !std::filesystem::is_symlink(extract_root / "usr/bin/link") ||
        std::filesystem::read_symlink(extract_root / "usr/bin/link").generic_string() != boundary_link_target) {
        sage::util::log_error("Archive extraction verification failed");
        return 1;
    }

    auto conflict_root = temp_dir / "conflict-sysroot";
    std::filesystem::create_directories(conflict_root / "usr/bin/link");
    std::ofstream(conflict_root / "usr/bin/link/keep") << "must survive\n";
    auto conflict_res = sage::archive::extract_package(pkg_path, conflict_root);
    if (conflict_res
        || !std::filesystem::exists(conflict_root / "usr/bin/link/keep")
        || std::filesystem::exists(conflict_root / "usr/bin/dummy")
        || std::filesystem::exists(conflict_root / long_rel)) {
        sage::util::log_error("Archive path-type conflict was not reported safely");
        return 1;
    }

    std::ifstream raw_tar_bytes_in(raw_tar_path, std::ios::binary);
    std::vector<std::uint8_t> raw_tar_bytes(
        std::istreambuf_iterator<char>(raw_tar_bytes_in), {});
    auto find_tar_header = [&](const std::vector<std::uint8_t>& bytes, std::string_view wanted)
        -> std::optional<size_t> {
        size_t offset = 0;
        while (offset + sizeof(sage::archive::TarHeader) <= bytes.size()) {
            sage::archive::TarHeader header{};
            std::memcpy(&header, bytes.data() + offset, sizeof(header));
            bool all_zero = std::ranges::all_of(
                std::span(bytes.data() + offset, sizeof(header)),
                [](std::uint8_t byte) { return byte == 0; });
            if (all_zero) break;

            const auto bounded_string = [](const char* data, size_t size) {
                return std::string(data, std::find(data, data + size, '\0'));
            };
            std::string name;
            if (header.prefix[0] != '\0') {
                name = bounded_string(header.prefix, sizeof(header.prefix)) + "/";
            }
            name += bounded_string(header.name, sizeof(header.name));
            if (name == wanted) return offset;

            auto size = sage::archive::parse_octal(header.size, sizeof(header.size));
            offset += sizeof(header) + static_cast<size_t>(((size + 511) / 512) * 512);
        }
        return std::nullopt;
    };
    auto write_mutated_package = [&](const std::vector<std::uint8_t>& bytes, std::string_view stem)
        -> std::optional<std::filesystem::path> {
        auto tar_path = malformed_archive_dir / (std::string(stem) + ".tar");
        auto archive_path = malformed_archive_dir / (std::string(stem) + ".pkg.tar.zst");
        std::ofstream tar_out(tar_path, std::ios::binary);
        tar_out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        tar_out.close();
        auto command = std::format("zstd -q -f \"{}\" -o \"{}\"", tar_path.string(), archive_path.string());
        if (std::system(command.c_str()) != 0) return std::nullopt;
        return archive_path;
    };
    auto rename_tar_entry = [&](std::vector<std::uint8_t>& bytes,
                                std::string_view old_name,
                                std::string_view new_name) -> bool {
        auto offset = find_tar_header(bytes, old_name);
        if (!offset || new_name.size() > 100) return false;
        sage::archive::TarHeader header{};
        std::memcpy(&header, bytes.data() + *offset, sizeof(header));
        std::memset(header.name, 0, sizeof(header.name));
        std::memset(header.prefix, 0, sizeof(header.prefix));
        std::memcpy(header.name, new_name.data(), new_name.size());
        sage::archive::write_tar_checksum(
            header.chksum, sage::archive::compute_tar_checksum(header));
        std::memcpy(bytes.data() + *offset, &header, sizeof(header));
        return true;
    };

    auto invalid_manifest_bytes = raw_tar_bytes;
    auto manifest_header = find_tar_header(invalid_manifest_bytes, ".METADATA/manifest.toml");
    if (!manifest_header) {
        sage::util::log_error("Failed to locate manifest test fixture");
        return 1;
    }
    invalid_manifest_bytes[*manifest_header + sizeof(sage::archive::TarHeader)] = '@';
    auto invalid_manifest_pkg = write_mutated_package(invalid_manifest_bytes, "invalid-manifest");
    auto invalid_manifest_root = temp_dir / "invalid-manifest-root";
    auto invalid_manifest_result = invalid_manifest_pkg
        ? sage::archive::extract_package(*invalid_manifest_pkg, invalid_manifest_root)
        : std::expected<sage::archive::ExtractedPackage, std::string>(std::unexpected("fixture failed"));
    if (invalid_manifest_result
        || std::filesystem::exists(invalid_manifest_root / "usr/bin/dummy")
        || std::filesystem::exists(invalid_manifest_root / long_rel)) {
        sage::util::log_error("Invalid manifest was rejected only after writing package files");
        return 1;
    }

    auto traversal_bytes = raw_tar_bytes;
    if (!rename_tar_entry(
            traversal_bytes, "data/usr/bin/dummy", "data/../../escaped-by-package")) {
        sage::util::log_error("Failed to create path traversal test fixture");
        return 1;
    }
    auto traversal_pkg = write_mutated_package(traversal_bytes, "path-traversal");
    auto traversal_root = temp_dir / "path-traversal-root/inner";
    auto escaped_path = temp_dir / "escaped-by-package";
    auto traversal_result = traversal_pkg
        ? sage::archive::extract_package(*traversal_pkg, traversal_root)
        : std::expected<sage::archive::ExtractedPackage, std::string>(std::unexpected("fixture failed"));
    if (traversal_result || std::filesystem::exists(escaped_path)) {
        sage::util::log_error("Package data path escaped the target root");
        return 1;
    }

    auto symlink_pivot_bytes = raw_tar_bytes;
    if (!rename_tar_entry(
            symlink_pivot_bytes, "data/usr/bin/dummy", "data/usr/bin/link/escape")) {
        sage::util::log_error("Failed to create archive symlink traversal fixture");
        return 1;
    }
    auto symlink_pivot_pkg = write_mutated_package(symlink_pivot_bytes, "symlink-pivot");
    auto symlink_pivot_root = temp_dir / "symlink-pivot-root";
    auto symlink_pivot_result = symlink_pivot_pkg
        ? sage::archive::extract_package(*symlink_pivot_pkg, symlink_pivot_root)
        : std::expected<sage::archive::ExtractedPackage, std::string>(std::unexpected("fixture failed"));
    if (symlink_pivot_result
        || std::filesystem::exists(symlink_pivot_root / long_rel)
        || std::filesystem::exists(symlink_pivot_root / "usr/bin/link")) {
        sage::util::log_error("Archive symlink parent traversal was not rejected before extraction");
        return 1;
    }

    auto existing_symlink_root = temp_dir / "existing-symlink-root";
    auto outside_symlink_target = temp_dir / "outside-symlink-target";
    std::filesystem::create_directories(existing_symlink_root);
    std::filesystem::create_directories(outside_symlink_target);
    std::filesystem::create_directory_symlink(outside_symlink_target, existing_symlink_root / "usr");
    auto outside_dummy = outside_symlink_target / "bin/dummy";
    auto existing_symlink_result = sage::archive::extract_package(pkg_path, existing_symlink_root);
    if (existing_symlink_result || std::filesystem::exists(outside_dummy)) {
        sage::util::log_error("Existing target symlink escaped the target root");
        return 1;
    }

    auto read_archive_test_file = [](const std::filesystem::path& path) {
        std::ifstream input(path, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(input), {});
    };

    // A pre-created legacy fixed-name temporary symlink must be ignored: the
    // extractor uses its own unique O_EXCL/O_NOFOLLOW temporary leaf.
    auto temp_symlink_root = temp_dir / "temp-symlink-root";
    auto temp_symlink_outside = temp_dir / "temp-symlink-outside";
    std::filesystem::create_directories(temp_symlink_root / "usr/bin");
    std::ofstream(temp_symlink_outside) << "must survive\n";
    std::filesystem::create_symlink(
        temp_symlink_outside, temp_symlink_root / "usr/bin/dummy.sage_tmp");
    auto temp_symlink_result = sage::archive::extract_package(pkg_path, temp_symlink_root);
    if (!temp_symlink_result
        || read_archive_test_file(temp_symlink_outside) != "must survive\n"
        || read_archive_test_file(temp_symlink_root / "usr/bin/dummy")
            != "#!/bin/sh\necho 'hello sage'\n"
        || !std::filesystem::is_symlink(temp_symlink_root / "usr/bin/dummy.sage_tmp")) {
        sage::util::log_error("Archive extraction followed a fixed temporary-file symlink");
        return 1;
    }

    auto reserved_temp_bytes = raw_tar_bytes;
    if (!rename_tar_entry(
            reserved_temp_bytes,
            "data/usr/bin/link",
            "data/usr/bin/.sage-tmp-controlled")) {
        sage::util::log_error("Failed to create reserved temporary-name fixture");
        return 1;
    }
    auto reserved_temp_pkg = write_mutated_package(
        reserved_temp_bytes, "reserved-temp-name");
    auto reserved_temp_root = temp_dir / "reserved-temp-root";
    auto reserved_temp_result = reserved_temp_pkg
        ? sage::archive::extract_package(*reserved_temp_pkg, reserved_temp_root)
        : std::expected<sage::archive::ExtractedPackage, std::string>(
            std::unexpected("fixture failed"));
    if (reserved_temp_result
        || reserved_temp_result.error().find("reserved temporary-file namespace")
            == std::string::npos
        || std::filesystem::exists(reserved_temp_root / "usr/bin/dummy")) {
        sage::util::log_error("Archive accepted the internal temporary-file namespace");
        return 1;
    }

    // Replacing a verified parent directory with an external symlink while the
    // second archive pass is running must fail closed at the fd-relative sink.
    auto parent_race_data = temp_dir / "parent-race-data";
    auto parent_race_target = temp_dir / "parent-race-target";
    auto parent_race_outside = temp_dir / "parent-race-outside";
    std::filesystem::create_directories(parent_race_data / "z-parent");
    std::filesystem::create_directories(parent_race_target / "z-parent");
    std::filesystem::create_directories(parent_race_outside);
    std::ofstream(parent_race_data / "a-marker") << "ready\n";
    {
        std::ofstream filler(parent_race_data / "b-filler", std::ios::binary);
        std::array<char, 1024 * 1024> zeros{};
        for (int i = 0; i < 16; ++i) filler.write(zeros.data(), zeros.size());
    }
    std::ofstream(parent_race_data / "z-parent/escaped") << "must stay inside\n";
    sage::package::PackageManifest parent_race_manifest;
    parent_race_manifest.name = "parent-race";
    parent_race_manifest.version = sage::package::Version::parse("1.0.0-1");
    auto parent_race_pkg = temp_dir / "parent-race-1.0.0-1-x86_64.pkg.tar.zst";
    if (!sage::archive::create_package(
            parent_race_manifest, parent_race_data, parent_race_pkg)) {
        sage::util::log_error("Failed to create parent replacement race fixture");
        return 1;
    }
    std::atomic<bool> parent_replaced{false};
    std::jthread parent_attacker([&](std::stop_token stop) {
        while (!stop.stop_requested()
            && !std::filesystem::exists(parent_race_target / "a-marker")) {
            std::this_thread::yield();
        }
        if (stop.stop_requested()) return;
        std::error_code race_ec;
        std::filesystem::remove_all(parent_race_target / "z-parent", race_ec);
        if (!race_ec) {
            std::filesystem::create_directory_symlink(
                parent_race_outside, parent_race_target / "z-parent", race_ec);
        }
        parent_replaced.store(!race_ec, std::memory_order_release);
    });
    auto parent_race_result = sage::archive::extract_package(
        parent_race_pkg, parent_race_target);
    parent_attacker.request_stop();
    parent_attacker.join();
    if (!parent_replaced.load(std::memory_order_acquire)
        || parent_race_result
        || std::filesystem::exists(parent_race_outside / "escaped")) {
        sage::util::log_error("Archive extraction escaped through a replaced parent directory");
        return 1;
    }

    auto regular_parent_bytes = raw_tar_bytes;
    if (!rename_tar_entry(
            regular_parent_bytes, "data/usr/bin/link", "data/usr/bin/dummy/child")) {
        sage::util::log_error("Failed to create non-directory archive ancestor fixture");
        return 1;
    }
    auto regular_parent_pkg = write_mutated_package(
        regular_parent_bytes, "regular-file-parent");
    auto regular_parent_root = temp_dir / "regular-file-parent-root";
    auto regular_parent_result = regular_parent_pkg
        ? sage::archive::extract_package(*regular_parent_pkg, regular_parent_root)
        : std::expected<sage::archive::ExtractedPackage, std::string>(
            std::unexpected("fixture failed"));
    if (regular_parent_result
        || std::filesystem::exists(regular_parent_root / "usr/bin/dummy")
        || std::filesystem::exists(regular_parent_root / long_rel)) {
        sage::util::log_error("Non-directory archive ancestor was rejected only after extraction");
        return 1;
    }

    // Raw archives cannot bypass usr-merge ownership by omitting the data/bin
    // directory entry: non-base packages must use canonical usr/bin paths.
    auto usr_merge_bytes = raw_tar_bytes;
    if (!rename_tar_entry(
            usr_merge_bytes, "data/usr/bin/dummy", "data/bin/dummy")) {
        sage::util::log_error("Failed to create usr-merge alias fixture");
        return 1;
    }
    auto usr_merge_pkg = write_mutated_package(usr_merge_bytes, "usr-merge-alias");
    auto usr_merge_root = temp_dir / "usr-merge-root";
    std::filesystem::create_directories(usr_merge_root / "usr/bin");
    std::filesystem::create_symlink("usr/bin", usr_merge_root / "bin");
    auto usr_merge_result = usr_merge_pkg
        ? sage::archive::extract_package(*usr_merge_pkg, usr_merge_root)
        : std::expected<sage::archive::ExtractedPackage, std::string>(
            std::unexpected("fixture failed"));
    if (usr_merge_result
        || usr_merge_result.error().find("must use canonical usr/ paths")
            == std::string::npos
        || std::filesystem::exists(usr_merge_root / "usr/bin/dummy")
        || std::filesystem::exists(usr_merge_root / long_rel)) {
        sage::util::log_error("Usr-merge alias payload bypassed canonical package paths");
        return 1;
    }

    auto base_files_data = temp_dir / "base-files-data";
    std::filesystem::create_directories(base_files_data);
    std::filesystem::create_symlink("usr/bin", base_files_data / "bin");
    sage::package::PackageManifest base_files_manifest;
    base_files_manifest.name = "base-files";
    base_files_manifest.version = sage::package::Version::parse("1.0.0-1");
    auto base_files_pkg = temp_dir / "base-files-1.0.0-1-x86_64.pkg.tar.zst";
    auto base_files_root = temp_dir / "base-files-root";
    auto base_files_pack = sage::archive::create_package(
        base_files_manifest, base_files_data, base_files_pkg);
    auto base_files_extract = base_files_pack
        ? sage::archive::extract_package(base_files_pkg, base_files_root)
        : std::expected<sage::archive::ExtractedPackage, std::string>(
            std::unexpected(base_files_pack.error()));
    if (!base_files_extract
        || !std::filesystem::is_symlink(base_files_root / "bin")
        || std::filesystem::read_symlink(base_files_root / "bin") != "usr/bin") {
        sage::util::log_error("Base-files could not create the canonical usr-merge alias");
        return 1;
    }

    auto invalid_base_files_data = temp_dir / "invalid-base-files-data";
    std::filesystem::create_directories(invalid_base_files_data);
    std::filesystem::create_symlink("opt/bin", invalid_base_files_data / "bin");
    auto invalid_base_files_pkg = malformed_archive_dir / "base-files-invalid.pkg.tar.zst";
    if (!sage::archive::create_package(
            base_files_manifest, invalid_base_files_data, invalid_base_files_pkg)) {
        sage::util::log_error("Failed to create invalid base-files fixture");
        return 1;
    }
    auto invalid_base_files_root = temp_dir / "invalid-base-files-root";
    auto invalid_base_files_extract = sage::archive::extract_package(
        invalid_base_files_pkg, invalid_base_files_root);
    if (invalid_base_files_extract
        || invalid_base_files_extract.error().find("must use canonical usr/ paths")
            == std::string::npos
        || std::filesystem::exists(invalid_base_files_root / "bin")) {
        sage::util::log_error("Base-files accepted a non-canonical usr-merge target");
        return 1;
    }

    auto verify_tar_reader = [&](std::string_view reader, const std::filesystem::path& root) {
        std::filesystem::create_directories(root);
        auto command = std::format("{} -xf \"{}\" -C \"{}\"", reader, pkg_path.string(), root.string());
        return std::system(command.c_str()) == 0 &&
            std::filesystem::exists(root / "data" / long_rel) &&
            std::filesystem::exists(root / "data" / boundary_rel) &&
            std::filesystem::is_directory(root / "data" / boundary_empty_dir) &&
            std::filesystem::is_symlink(root / "data/usr/bin/link") &&
            std::filesystem::read_symlink(root / "data/usr/bin/link").generic_string() == boundary_link_target;
    };
    if (!verify_tar_reader("tar", temp_dir / "tar-sysroot") ||
        !verify_tar_reader("bsdtar", temp_dir / "bsdtar-sysroot")) {
        sage::util::log_error("POSIX USTAR compatibility verification failed");
        return 1;
    }
    sage::util::log_success("2. Deterministic POSIX USTAR + Zstandard Engine OK");

    // 2b. `cmd_build` ELF scan order must be reflected deterministically in the final manifest.
    auto write_test_elf = [](const std::filesystem::path& path,
                             std::string_view soname,
                             std::string_view needed) {
        constexpr size_t phoff = 64;
        constexpr size_t dynoff = 176;
        constexpr size_t stroff = 256;
        constexpr std::uint64_t base = 0x400000;

        std::string strings(1, '\0');
        const std::uint64_t soname_offset = strings.size();
        strings.append(soname);
        strings.push_back('\0');
        const std::uint64_t needed_offset = strings.size();
        strings.append(needed);
        strings.push_back('\0');

        std::vector<std::uint8_t> elf(stroff + strings.size());
        auto put = [&](size_t offset, std::uint64_t value, size_t width) {
            for (size_t i = 0; i < width; ++i) {
                elf[offset + i] = static_cast<std::uint8_t>(value >> (8 * i));
            }
        };

        elf[0] = 0x7f;
        elf[1] = 'E';
        elf[2] = 'L';
        elf[3] = 'F';
        elf[4] = 2;
        elf[5] = 1;
        elf[6] = 1;
        put(16, 3, 2);
        put(18, 62, 2);
        put(20, 1, 4);
        put(32, phoff, 8);
        put(52, 64, 2);
        put(54, 56, 2);
        put(56, 2, 2);

        put(64, 1, 4);
        put(68, 4, 4);
        put(80, base, 8);
        put(88, base, 8);
        put(96, elf.size(), 8);
        put(104, elf.size(), 8);
        put(112, 0x1000, 8);

        put(120, 2, 4);
        put(124, 4, 4);
        put(128, dynoff, 8);
        put(136, base + dynoff, 8);
        put(144, base + dynoff, 8);
        put(152, 80, 8);
        put(160, 80, 8);
        put(168, 8, 8);

        auto put_dynamic = [&](size_t index, std::uint64_t tag, std::uint64_t value) {
            put(dynoff + index * 16, tag, 8);
            put(dynoff + index * 16 + 8, value, 8);
        };
        put_dynamic(0, 5, base + stroff);
        put_dynamic(1, 10, strings.size());
        put_dynamic(2, 1, needed_offset);
        put_dynamic(3, 14, soname_offset);
        put_dynamic(4, 0, 0);

        std::memcpy(elf.data() + stroff, strings.data(), strings.size());
        std::ofstream out(path, std::ios::binary);
        out.write(reinterpret_cast<const char*>(elf.data()), static_cast<std::streamsize>(elf.size()));
        return out.good();
    };

    auto write_elf_recipe = [&](const std::filesystem::path& recipe_dir, bool z_first) {
        std::filesystem::create_directories(recipe_dir);
        if (!write_test_elf(recipe_dir / "a.elf", "liba-provider.so.1", "liba-needed.so.1") ||
            !write_test_elf(recipe_dir / "z.elf", "libz-provider.so.1", "libz-needed.so.1")) {
            return false;
        }

        std::ofstream recipe(recipe_dir / "recipe.toml");
        recipe << R"(schema_version = 1
[package]
name = "elf-order-test"
version = "1.0.0"
release = "1"
description = "ELF manifest ordering fixture"
license = "BSD-2-Clause"
channel = "system"
dependencies = ["declared-runtime"]
provides = ["elf-order"]
install = [
    'mkdir -p "$DESTDIR/usr/lib"',
)";
        if (z_first) {
            recipe << "    'cp \"$RECIPE_DIR/z.elf\" \"$DESTDIR/usr/lib/z-provider.so\"',\n";
            recipe << "    'cp \"$RECIPE_DIR/a.elf\" \"$DESTDIR/usr/lib/a-provider.so\"',\n";
        } else {
            recipe << "    'cp \"$RECIPE_DIR/a.elf\" \"$DESTDIR/usr/lib/a-provider.so\"',\n";
            recipe << "    'cp \"$RECIPE_DIR/z.elf\" \"$DESTDIR/usr/lib/z-provider.so\"',\n";
        }
        recipe << "]\n";
        return recipe.good();
    };

    auto elf_test_root = temp_dir / "elf-order";
    auto z_first_dir = elf_test_root / "z-first";
    auto a_first_dir = elf_test_root / "a-first";
    if (!write_elf_recipe(z_first_dir, true) || !write_elf_recipe(a_first_dir, false)) {
        sage::util::log_error("Failed to create ELF manifest ordering fixtures");
        return 1;
    }

    auto build_elf_fixture = [&](const std::filesystem::path& recipe_dir,
                                 const std::filesystem::path& extract_dir)
        -> std::expected<sage::package::PackageManifest, std::string> {
        CliOptions build_opts;
        build_opts.args = {recipe_dir.string()};
        if (cmd_build(build_opts) != 0) {
            return std::unexpected("cmd_build failed for " + recipe_dir.string());
        }
        auto package_path = recipe_dir / "elf-order-test-1.0.0-1-x86_64.pkg.tar.zst";
        auto extracted = sage::archive::extract_package(package_path, extract_dir);
        if (!extracted) return std::unexpected(extracted.error());
        return std::move(extracted->manifest);
    };

    auto z_first_manifest = build_elf_fixture(z_first_dir, elf_test_root / "z-first-extracted");
    auto a_first_manifest = build_elf_fixture(a_first_dir, elf_test_root / "a-first-extracted");
    auto has_expected_elf_order = [](const sage::package::PackageManifest& built) {
        return built.dependencies.size() == 3 && built.provides.size() == 3 &&
            built.dependencies[0].to_string() == "declared-runtime" &&
            built.dependencies[1].to_string() == "so:liba-needed.so.1" &&
            built.dependencies[2].to_string() == "so:libz-needed.so.1" &&
            built.provides[0] == "elf-order" &&
            built.provides[1] == "so:liba-provider.so.1" &&
            built.provides[2] == "so:libz-provider.so.1";
    };
    auto z_first_hash = sage::util::compute_file_sha256(
        z_first_dir / "elf-order-test-1.0.0-1-x86_64.pkg.tar.zst");
    auto a_first_hash = sage::util::compute_file_sha256(
        a_first_dir / "elf-order-test-1.0.0-1-x86_64.pkg.tar.zst");
    if (!z_first_manifest || !a_first_manifest ||
        !has_expected_elf_order(*z_first_manifest) || !has_expected_elf_order(*a_first_manifest) ||
        !z_first_hash || !a_first_hash || *z_first_hash != *a_first_hash) {
        sage::util::log_error("cmd_build ELF manifest ordering is not deterministic");
        return 1;
    }
    sage::util::log_success("   Deterministic cmd_build ELF Manifest Ordering OK");

    // 3. PubGrub Dependency Solver & Toolchain MSRV Resolution Test
    std::vector<sage::package::PackageManifest> repo_pool;
    sage::package::PackageManifest libfoo;
    libfoo.name = "libfoo";
    libfoo.version = sage::package::Version::parse("2.1.0-1");
    libfoo.provides = {"libfoo", "so:libfoo.so.2"};

    sage::package::PackageManifest gcc15;
    gcc15.name = "gcc";
    gcc15.version = sage::package::Version::parse("15.3.0-1");
    gcc15.channel = "toolchain/gcc:15";
    gcc15.provides = {"cc", "c++", "gcc", "toolchain/gcc"};

    sage::package::PackageManifest app;
    app.name = "demo-app";
    app.version = sage::package::Version::parse("1.0.0-1");
    app.dependencies.push_back(sage::package::Dependency::parse("libfoo >= 2.0.0"));
    app.dependencies.push_back(sage::package::Dependency::parse("toolchain/gcc >= 14.0"));

    sage::package::PackageManifest openrc_pkg;
    openrc_pkg.name = "openrc";
    openrc_pkg.version = sage::package::Version::parse("0.54.0-1");
    openrc_pkg.provides = {"openrc", "virtual/init"};

    repo_pool.push_back(libfoo);
    repo_pool.push_back(gcc15);
    repo_pool.push_back(app);
    repo_pool.push_back(openrc_pkg);

    sage::solver::DependencySolver solver(repo_pool);
    auto solve_res = solver.solve({sage::package::Dependency::parse("demo-app")});
    if (!solve_res || solve_res->size() != 3) {
        sage::util::log_error("Dependency & Toolchain MSRV solver test failed (resolved size: {})", solve_res ? solve_res->size() : 0);
        return 1;
    }
    auto dependency_before = [&](std::string_view dependency, std::string_view dependent) {
        auto dependency_it = std::ranges::find(*solve_res, dependency, &sage::package::PackageManifest::name);
        auto dependent_it = std::ranges::find(*solve_res, dependent, &sage::package::PackageManifest::name);
        return dependency_it != solve_res->end()
            && dependent_it != solve_res->end()
            && dependency_it < dependent_it;
    };
    if (!dependency_before("libfoo", "demo-app") || !dependency_before("gcc", "demo-app")) {
        sage::util::log_error("Dependency solver did not return dependency-first install order");
        return 1;
    }
    sage::util::log_success("3. Native PubGrub / CDCL SAT Dependency & MSRV Solver OK");

    // 4. Multi-Init Universal Service Generation Test
    sage::service::ServiceSpec svc;
    svc.name = "sshd";
    svc.description = "OpenSSH Server";
    svc.exec_start = "/usr/sbin/sshd -D";
    auto gen_openrc = sage::service::generate_service(svc, sage::service::InitType::OpenRC, extract_root);
    auto gen_sysd = sage::service::generate_service(svc, sage::service::InitType::Systemd, extract_root);
    if (!gen_openrc || !gen_sysd) {
        sage::util::log_error("Service generation test failed");
        return 1;
    }
    sage::util::log_success("4. Universal Multi-Init Service Generator (OpenRC/Systemd/Runit/Dinit/s6) OK");

    // 5. LMDB Database & Rebuild Engine Test
    auto db_dir = temp_dir / "db";
    auto db_res = sage::db::Database::open(db_dir);
    if (!db_res) {
        sage::util::log_error("DB open failed: {}", db_res.error());
        return 1;
    }

    sage::package::PackageManifest escaped_manifest;
    escaped_manifest.name = "escaped-metadata";
    escaped_manifest.version = sage::package::Version::parse("7:1.0.0-1");
    escaped_manifest.description = "quoted \"value\" with \\ slash\nnext line";
    sage::package::FileEntry escaped_file;
    escaped_file.path = R"(usr/lib/systemd/system/system-systemd\x2dmute.slice)";
    escaped_manifest.files.push_back(std::move(escaped_file));
    auto escaped_round_trip = sage::package::PackageManifest::parse_toml(escaped_manifest.serialize_toml());
    if (!escaped_round_trip
        || escaped_round_trip->version != escaped_manifest.version
        || escaped_round_trip->description != escaped_manifest.description
        || escaped_round_trip->files.size() != 1
        || escaped_round_trip->files.front().path != escaped_manifest.files.front().path) {
        sage::util::log_error("Package metadata TOML escaping round-trip failed");
        return 1;
    }

    auto embedded_epoch_manifest = sage::package::PackageManifest::parse_toml(R"(
schema_version = 1
[package]
name = "embedded-epoch"
version = "1:2.0-3"
)");
    auto embedded_epoch_index = sage::channel::ChannelIndex::parse_toml(R"(
schema_version = 1
[channel]
name = "core"
[[packages]]
name = "embedded-epoch"
version = "1:2.0-3"
)");
    if (!embedded_epoch_manifest
        || embedded_epoch_manifest->version.epoch != 1
        || embedded_epoch_manifest->version.rel != "3"
        || !embedded_epoch_index
        || embedded_epoch_index->available_packages.size() != 1
        || embedded_epoch_index->available_packages.front().version.epoch != 1
        || embedded_epoch_index->available_packages.front().version.rel != "3") {
        sage::util::log_error("Embedded version epoch/release was not preserved");
        return 1;
    }

    // Per-package and aggregate trigger passes share completion state. A
    // command that is not available yet remains retryable, while a command
    // that already ran is not executed again in the aggregate pass.
    auto trigger_state_root = temp_dir / "trigger-state-root";
    sage::package::PackageManifest trigger_package;
    trigger_package.name = "trigger-state-test";
    sage::package::Trigger trigger;
    trigger.name = "trigger-state-counter";
    trigger.on_paths = {"usr/share/trigger-input"};
    trigger.exec = "/usr/bin/trigger-counter";
    trigger_package.triggers = {trigger};
    sage::package::FileEntry trigger_input;
    trigger_input.path = "usr/share/trigger-input/file";
    sage::rebuild::TriggerContext trigger_context;
    trigger_context.sysroot = trigger_state_root;
    trigger_context.touched_files = {trigger_input};
    trigger_context.transaction_packages = {trigger_package};
    trigger_context.installed_packages = {trigger_package};
    trigger_context.dry_run = true;
    std::set<std::string> completed_trigger_commands;
    if (sage::rebuild::TriggerEngine::run(
            trigger_context, completed_trigger_commands) != 0
        || !completed_trigger_commands.empty()) {
        sage::util::log_error("Unavailable trigger command was marked as completed");
        return 1;
    }
    std::filesystem::create_directories(trigger_state_root / "usr/bin");
    std::ofstream(trigger_state_root / "usr/bin/trigger-counter") << "fixture\n";
    auto first_trigger_pass = sage::rebuild::TriggerEngine::run(
        trigger_context, completed_trigger_commands);
    auto aggregate_trigger_pass = sage::rebuild::TriggerEngine::run(
        trigger_context, completed_trigger_commands);
    if (first_trigger_pass != 1
        || aggregate_trigger_pass != 0
        || completed_trigger_commands
            != std::set<std::string>{"/usr/bin/trigger-counter"}) {
        sage::util::log_error("Trigger command ran more than once across install passes");
        return 1;
    }

    sage::package::FileEntry owned_file;
    owned_file.path = "usr/bin/database-owned";
    {
        auto owner_txn = db_res->begin_write_txn();
        if (!owner_txn
            || !db_res->register_files(*owner_txn, "database-owner", "system", {owned_file})
            || !owner_txn->commit()) {
            sage::util::log_error("Failed to create database file ownership fixture");
            return 1;
        }
    }
    sage::package::FileEntry unowned_file;
    unowned_file.path = "usr/bin/database-unowned";
    bool database_conflict_rejected = false;
    {
        auto conflict_txn = db_res->begin_write_txn();
        if (!conflict_txn) {
            sage::util::log_error("Failed to open database file conflict transaction");
            return 1;
        }
        auto conflict_registration = db_res->register_files(
            *conflict_txn, "database-challenger", "system", {unowned_file, owned_file});
        database_conflict_rejected = !conflict_registration;
    }
    if (!database_conflict_rejected
        || db_res->get_file_owner(owned_file.path).value_or(std::nullopt) != "database-owner:system"
        || db_res->get_file_owner(unowned_file.path).value_or(std::nullopt)) {
        sage::util::log_error("Database file conflict registration was not atomic");
        return 1;
    }

    // A package identity captured before the writer lock must be revalidated
    // inside that write transaction, and same-name ownership is not sufficient.
    auto migration_db = sage::db::Database::open(temp_dir / "migration-race-db");
    sage::package::FileEntry migration_file;
    migration_file.path = "usr/bin/migration-race";
    sage::package::PackageManifest migration_old;
    migration_old.name = "migration-race";
    migration_old.version = sage::package::Version::parse("1.0.0-1");
    migration_old.channel = "system";
    migration_old.files = {migration_file};
    if (!migration_db) {
        sage::util::log_error("Failed to create migration race database");
        return 1;
    }
    {
        auto setup_txn = migration_db->begin_write_txn();
        if (!setup_txn
            || !migration_db->put_package(*setup_txn, migration_old)
            || !migration_db->register_files(
                *setup_txn, migration_old.name, migration_old.channel, migration_old.files)
            || !setup_txn->commit()) {
            sage::util::log_error("Failed to populate migration race database");
            return 1;
        }
    }
    const auto expected_migration_identity = std::optional{
        sage::package::package_identity(migration_old)};
    auto migration_new = migration_old;
    migration_new.version = sage::package::Version::parse("2.0.0-1");
    migration_new.channel = "runtime/python:3.12";
    {
        auto concurrent_txn = migration_db->begin_write_txn();
        if (!concurrent_txn
            || !migration_db->unregister_files(
                *concurrent_txn, migration_old.files, "migration-race:system")
            || !migration_db->put_package(*concurrent_txn, migration_new)
            || !migration_db->register_files(
                *concurrent_txn, migration_new.name, migration_new.channel, migration_new.files)
            || !concurrent_txn->commit()) {
            sage::util::log_error("Failed to simulate concurrent channel migration");
            return 1;
        }
    }
    {
        auto install_txn = migration_db->begin_write_txn();
        const std::string stale_owner = "migration-race:system";
        auto stale_snapshot = install_txn
            ? load_install_snapshot(
                *migration_db,
                *install_txn,
                migration_old.name,
                expected_migration_identity)
            : std::expected<std::optional<sage::package::PackageManifest>, std::string>(
                std::unexpected("transaction failed"));
        auto stale_owner_check = install_txn
            ? migration_db->check_file_conflicts(
                *install_txn,
                std::optional<std::string_view>{stale_owner},
                migration_new.files)
            : std::expected<void, std::string>(std::unexpected("transaction failed"));
        if (stale_snapshot || stale_owner_check) {
            sage::util::log_error("Concurrent same-name channel migration bypassed identity revalidation");
            return 1;
        }
    }
    auto preserved_migration = migration_db->get_package(migration_new.name);
    if (!preserved_migration || !*preserved_migration
        || sage::package::package_identity(**preserved_migration)
            != sage::package::package_identity(migration_new)
        || migration_db->get_file_owner(migration_file.path).value_or(std::nullopt)
            != "migration-race:runtime/python:3.12") {
        sage::util::log_error("Rejected stale migration changed the concurrent package state");
        return 1;
    }

    auto corrupt_target = temp_dir / "corrupt-target";
    auto corrupt_db_dir = corrupt_target / "var/lib/sage";
    {
        auto raw_env = sage::vendor::lmdb::MdbEnv::create(corrupt_db_dir);
        if (!raw_env) {
            sage::util::log_error("Failed to create corrupt database fixture");
            return 1;
        }
        auto raw_txn = sage::vendor::lmdb::MdbTxn::begin(*raw_env);
        if (!raw_txn) {
            sage::util::log_error("Failed to begin corrupt database fixture transaction");
            return 1;
        }
        auto raw_packages = sage::vendor::lmdb::MdbDbi::open(
            *raw_txn, "packages", sage::vendor::lmdb::flag_create);
        auto raw_files = sage::vendor::lmdb::MdbDbi::open(
            *raw_txn, "files", sage::vendor::lmdb::flag_create);
        if (!raw_packages || !raw_files
            || !raw_packages->put(
                *raw_txn, "aaa-mismatch", "[package]\nname = \"different-name\"\nversion = \"1.0.0\"\n")
            || !raw_packages->put(*raw_txn, "broken", "[package]\nname = \"broken\n")
            || !raw_files->put(*raw_txn, "usr/bin/broken", "broken:system")) {
            sage::util::log_error("Failed to populate corrupt database fixture");
            return 1;
        }
        auto raw_commit = raw_txn->commit();
        if (!raw_commit) {
            sage::util::log_error("Failed to commit corrupt database fixture");
            return 1;
        }
    }
    std::filesystem::create_directories(corrupt_target / "etc/sage");
    CliOptions corrupt_install_opts;
    corrupt_install_opts.target_root = corrupt_target;
    corrupt_install_opts.args = {"dummy-tool"};
    if (cmd_install(corrupt_install_opts) == 0) {
        sage::util::log_error("Install accepted a corrupt installed package manifest");
        return 1;
    }
    auto corrupt_db = sage::db::Database::open(corrupt_db_dir, true);
    auto corrupt_package = corrupt_db
        ? corrupt_db->get_package("broken")
        : std::expected<std::optional<sage::package::PackageManifest>, std::string>(
            std::unexpected("database open failed"));
    auto mismatched_package = corrupt_db
        ? corrupt_db->get_package("aaa-mismatch")
        : std::expected<std::optional<sage::package::PackageManifest>, std::string>(
            std::unexpected("database open failed"));
    if (!corrupt_db
        || corrupt_db->list_installed_packages()
        || corrupt_package
        || mismatched_package
        || corrupt_db->get_file_owner("usr/bin/broken").value_or(std::nullopt) != "broken:system") {
        sage::util::log_error("Corrupt manifest failure changed existing file ownership");
        return 1;
    }

    sage::config::SystemConfig sys_cfg;
    sys_cfg.providers["virtual/init"] = "openrc";
    sys_cfg.capabilities["virtual/init"] = sage::config::CapabilityKind::Exclusive;
    auto plan_res = sage::rebuild::ReconcileEngine::calculate_diff(*db_res, sys_cfg, repo_pool);
    if (!plan_res) {
        sage::util::log_error("Reconcile plan failed: {}", plan_res.error());
        return 1;
    }
    auto exec_res = sage::rebuild::ReconcileEngine::execute(*db_res, *plan_res, extract_root, false);
    if (!exec_res) {
        sage::util::log_error("Reconcile execute failed: {}", exec_res.error());
        return 1;
    }

    // A reconcile plan must not remove a same-name package that changed before
    // its write transaction began. Preserve both the newer package and the old
    // provider lock so the caller can recalculate from a fresh snapshot.
    auto reconcile_race_db = sage::db::Database::open(temp_dir / "reconcile-race-db");
    sage::package::PackageManifest old_init;
    old_init.name = "old-init";
    old_init.version = sage::package::Version::parse("1.0.0-1");
    old_init.provides = {"old-init", "virtual/init"};
    if (!reconcile_race_db) {
        sage::util::log_error("Failed to create reconcile snapshot fixture");
        return 1;
    }
    {
        auto setup_txn = reconcile_race_db->begin_write_txn();
        if (!setup_txn
            || !reconcile_race_db->put_package(*setup_txn, old_init)
            || !reconcile_race_db->set_system_provider(*setup_txn, "virtual/init", old_init.name)
            || !setup_txn->commit()) {
            sage::util::log_error("Failed to populate reconcile snapshot fixture");
            return 1;
        }
    }
    auto stale_plan = sage::rebuild::ReconcileEngine::calculate_diff(
        *reconcile_race_db, sys_cfg, repo_pool);
    auto replacement_init = old_init;
    replacement_init.version = sage::package::Version::parse("2.0.0-1");
    {
        auto update_txn = reconcile_race_db->begin_write_txn();
        if (!stale_plan || !update_txn
            || !reconcile_race_db->put_package(*update_txn, replacement_init)
            || !update_txn->commit()) {
            sage::util::log_error("Failed to update reconcile snapshot fixture");
            return 1;
        }
    }
    auto stale_execute = sage::rebuild::ReconcileEngine::execute(
        *reconcile_race_db, *stale_plan, extract_root, false);
    auto preserved_init = reconcile_race_db->get_package(old_init.name);
    auto preserved_provider = reconcile_race_db->get_system_provider("virtual/init");
    if (stale_execute
        || !preserved_init || !*preserved_init
        || (**preserved_init).version != replacement_init.version
        || !preserved_provider || !*preserved_provider
        || **preserved_provider != old_init.name) {
        sage::util::log_error("Reconcile executed against a stale package snapshot");
        return 1;
    }

    // A provider lock can also change without changing the package record.
    // Reject the stale plan before it overwrites that newer binding.
    {
        auto reset_txn = reconcile_race_db->begin_write_txn();
        if (!reset_txn
            || !reconcile_race_db->put_package(*reset_txn, old_init)
            || !reconcile_race_db->set_system_provider(*reset_txn, "virtual/init", old_init.name)
            || !reset_txn->commit()) {
            sage::util::log_error("Failed to reset reconcile provider fixture");
            return 1;
        }
    }
    auto stale_provider_plan = sage::rebuild::ReconcileEngine::calculate_diff(
        *reconcile_race_db, sys_cfg, repo_pool);
    {
        auto update_txn = reconcile_race_db->begin_write_txn();
        if (!stale_provider_plan || !update_txn
            || !reconcile_race_db->set_system_provider(
                *update_txn, "virtual/init", "concurrent-init")
            || !update_txn->commit()) {
            sage::util::log_error("Failed to update reconcile provider fixture");
            return 1;
        }
    }
    auto stale_provider_execute = sage::rebuild::ReconcileEngine::execute(
        *reconcile_race_db, *stale_provider_plan, extract_root, false);
    auto concurrent_provider = reconcile_race_db->get_system_provider("virtual/init");
    auto provider_package = reconcile_race_db->get_package(old_init.name);
    if (stale_provider_execute
        || !concurrent_provider || !*concurrent_provider
        || **concurrent_provider != "concurrent-init"
        || !provider_package || !*provider_package
        || (**provider_package).version != old_init.version) {
        sage::util::log_error("Reconcile overwrote a concurrently changed provider lock");
        return 1;
    }
    sage::util::log_success("5. Declarative System Reconcile & Triggers Engine (sage rebuild) OK");

    // 6. Sub-Channel Toolchain & Profile Swapper Test
    auto tc_dir = extract_root / "opt/channels/llvm/22/bin";
    std::filesystem::create_directories(tc_dir);
    std::ofstream clang_bin(tc_dir / "clang");
    clang_bin << "#!/bin/sh\necho 'clang version 22.1.0'\n";
    clang_bin.close();
    std::filesystem::permissions(tc_dir / "clang", std::filesystem::perms::owner_all | std::filesystem::perms::group_read | std::filesystem::perms::group_exec);

    auto sw_res = sage::channel::ProfileManager::switch_active_toolchain(extract_root, "llvm", "22");
    // Profile links are deliberately chroot-relative: they point at
    // /opt/channels/..., which is where the toolchain lives once the sysroot
    // becomes the root. On the build host that target does not exist, so
    // std::filesystem::exists() -- which follows the link -- is the wrong
    // probe. Check the link itself, and that it points where it should.
    auto cc_link = extract_root / "etc/sage/profiles/default/bin/cc";
    std::error_code cc_ec;
    if (!sw_res
        || !std::filesystem::is_symlink(cc_link, cc_ec)
        || std::filesystem::read_symlink(cc_link, cc_ec) != "/opt/channels/llvm/22/bin/clang") {
        sage::util::log_error("Toolchain profile switch verification failed");
        return 1;
    }
    sage::util::log_success("6. Sub-Channel Toolchain Slot Swapper & Profile Aggregator OK");

    // 7. Ephemeral Shell Environment Synthesis Test
    auto shell_env = sage::channel::ProfileManager::generate_shell_env(extract_root, {
        sage::channel::SubChannelSpec::parse("toolchain/llvm:22")
    });
    if (!shell_env.contains("CC") || shell_env["CC"].find("llvm/22/bin/clang") == std::string::npos) {
        sage::util::log_error("Ephemeral shell environment generation failed");
        return 1;
    }
    sage::util::log_success("7. Ephemeral Sandboxed Shell Environment Generator (sage shell) OK");

    // 8. Local Repository Indexing & Zero-Copy file:// Protocol Test
    const std::string escaped_channel_name = "core \"quoted\" \\ channel";
    auto local_repo = temp_dir / "local-repo";
    std::filesystem::create_directories(local_repo);
    std::error_code repo_copy_ec;
    std::filesystem::copy_file(
        pkg_path, local_repo / pkg_path.filename(),
        std::filesystem::copy_options::overwrite_existing, repo_copy_ec);
    auto idx_res = repo_copy_ec
        ? std::expected<void, std::string>(std::unexpected(repo_copy_ec.message()))
        : sage::archive::generate_repo_index(local_repo, escaped_channel_name);
    if (!idx_res || !std::filesystem::exists(local_repo / "index.toml")) {
        sage::util::log_error("Local repository index generation failed");
        return 1;
    }

    auto fetch_res = sage::vendor::curl::fetch_string(
        "file://" + (local_repo / "index.toml").string());
    if (!fetch_res) {
        sage::util::log_error("Local file:// protocol fetch failed");
        return 1;
    }
    auto parsed_index = sage::channel::ChannelIndex::parse_toml(*fetch_res);
    if (fetch_res->find("schema_version = 1") == std::string::npos
        || !parsed_index
        || parsed_index->channel_name != escaped_channel_name
        || parsed_index->available_packages.empty()
        || parsed_index->available_packages.front().description != manifest.description) {
        sage::util::log_error("Local file:// protocol fetch failed");
        return 1;
    }
    sage::util::log_success("8. Local Repository (file:// & /path) Indexer & Zero-Copy Fetch OK");

    // 9. End-to-End `sage install` & `sage remove` into isolated Target Root Test
    auto isolated_target = temp_dir / "target_root";
    std::filesystem::create_directories(isolated_target / "etc/sage");
    std::ofstream chan_f(isolated_target / "etc/sage/channels.toml");
    chan_f << "schema_version = 1\n\n[[channels]]\nname = \"core\"\nurl = \"file://" << local_repo.string() << "\"\nscope = \"system\"\npriority = 100\nenabled = true\n";
    chan_f.close();

    CliOptions inst_opts;
    inst_opts.target_root = isolated_target;
    inst_opts.args = {"dummy-tool"};
    int inst_ret = cmd_install(inst_opts);
    if (inst_ret != 0 || !std::filesystem::exists(isolated_target / "usr/bin/dummy")) {
        sage::util::log_error("End-to-end sage install to target root failed");
        return 1;
    }

    CliOptions rem_opts;
    rem_opts.target_root = isolated_target;
    rem_opts.args = {"dummy-tool"};
    int rem_ret = cmd_remove(rem_opts);
    if (rem_ret != 0 || std::filesystem::exists(isolated_target / "usr/bin/dummy")) {
        sage::util::log_error("End-to-end sage remove from target root failed");
        return 1;
    }

    auto write_test_channel = [](const std::filesystem::path& target,
                                 const std::filesystem::path& repo) {
        std::filesystem::create_directories(target / "etc/sage");
        std::ofstream channels(target / "etc/sage/channels.toml");
        channels
            << "schema_version = 1\n\n[[channels]]\nname = \"core\"\nurl = \"file://"
            << repo.string()
            << "\"\nscope = \"system\"\npriority = 100\nenabled = true\n";
        return channels.good();
    };
    auto read_test_file = [](const std::filesystem::path& path) {
        std::ifstream input(path, std::ios::binary);
        return std::string(
            std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
    };

    // A solver selection, direct archive request, and extracted archive must
    // all refer to the same complete package identity.
    auto version_repo = temp_dir / "version-repo";
    auto version_1_data = temp_dir / "version-1-data";
    auto version_2_data = temp_dir / "version-2-data";
    std::filesystem::create_directories(version_repo);
    std::filesystem::create_directories(version_1_data / "usr/bin");
    std::filesystem::create_directories(version_2_data / "usr/bin");
    std::ofstream(version_1_data / "usr/bin/versioned") << "version 1\n";
    std::ofstream(version_2_data / "usr/bin/versioned") << "version 2\n";

    sage::package::PackageManifest version_1;
    version_1.name = "versioned-package";
    version_1.version = sage::package::Version::parse("1.0.0-1");
    sage::package::PackageManifest version_2 = version_1;
    version_2.version = sage::package::Version::parse("2.0.0-1");
    auto version_1_pkg = version_repo / "versioned-package-1.0.0-1-x86_64.pkg.tar.zst";
    auto version_2_pkg = version_repo / "versioned-package-2.0.0-1-x86_64.pkg.tar.zst";
    if (!sage::archive::create_package(version_1, version_1_data, version_1_pkg)
        || !sage::archive::create_package(version_2, version_2_data, version_2_pkg)
        || !sage::archive::generate_repo_index(version_repo, "core")) {
        sage::util::log_error("Failed to create package identity fixtures");
        return 1;
    }

    auto version_target = temp_dir / "version-target";
    if (!write_test_channel(version_target, version_repo)) {
        sage::util::log_error("Failed to write package identity test channel");
        return 1;
    }
    CliOptions version_install;
    version_install.target_root = version_target;
    version_install.args = {"versioned-package"};
    if (cmd_install(version_install) != 0
        || read_test_file(version_target / "usr/bin/versioned") != "version 2\n") {
        sage::util::log_error("Solver selection did not install the selected archive version");
        return 1;
    }
    auto version_db = sage::db::Database::open(version_target / "var/lib/sage/data.mdb", true);
    auto selected_version = version_db
        ? version_db->get_package("versioned-package")
        : std::expected<std::optional<sage::package::PackageManifest>, std::string>(
            std::unexpected("database open failed"));
    if (!selected_version || !*selected_version
        || (**selected_version).version != version_2.version) {
        sage::util::log_error("Installed database version differs from extracted archive version");
        return 1;
    }

    // Equal identities from multiple sources must keep solver metadata and the
    // extracted archive bound to the same highest-priority source.
    auto high_priority_repo = temp_dir / "source-high";
    auto low_priority_repo = temp_dir / "source-low";
    auto high_priority_data = temp_dir / "source-high-data";
    auto low_priority_data = temp_dir / "source-low-data";
    std::filesystem::create_directories(high_priority_repo);
    std::filesystem::create_directories(low_priority_repo);
    std::filesystem::create_directories(high_priority_data / "usr/bin");
    std::filesystem::create_directories(low_priority_data / "usr/bin");
    std::ofstream(high_priority_data / "usr/bin/source-bound") << "high priority\n";
    std::ofstream(low_priority_data / "usr/bin/source-bound") << "low priority\n";
    sage::package::PackageManifest source_bound;
    source_bound.name = "source-bound";
    source_bound.version = sage::package::Version::parse("1.0.0-1");
    auto high_priority_archive =
        high_priority_repo / "source-bound-1.0.0-1-x86_64.pkg.tar.zst";
    auto low_priority_archive =
        low_priority_repo / "source-bound-1.0.0-1-x86_64.pkg.tar.zst";
    if (!sage::archive::create_package(
            source_bound, high_priority_data, high_priority_archive)
        || !sage::archive::create_package(
            source_bound, low_priority_data, low_priority_archive)
        || !sage::archive::generate_repo_index(high_priority_repo, "high")
        || !sage::archive::generate_repo_index(low_priority_repo, "low")) {
        sage::util::log_error("Failed to create multi-source identity fixtures");
        return 1;
    }
    auto source_target = temp_dir / "source-target";
    std::filesystem::create_directories(source_target / "etc/sage");
    std::ofstream source_channels(source_target / "etc/sage/channels.toml");
    source_channels
        << "schema_version = 1\n\n"
        << "[[channels]]\nname = \"low\"\nurl = \"file://"
        << low_priority_repo.string()
        << "\"\nscope = \"system\"\npriority = 10\nenabled = true\n\n"
        << "[[channels]]\nname = \"high\"\nurl = \"file://"
        << high_priority_repo.string()
        << "\"\nscope = \"system\"\npriority = 100\nenabled = true\n";
    source_channels.close();
    CliOptions source_install;
    source_install.target_root = source_target;
    source_install.args = {source_bound.name};
    if (cmd_install(source_install) != 0
        || read_test_file(source_target / "usr/bin/source-bound") != "high priority\n") {
        sage::util::log_error("Solver metadata and archive source were not bound together");
        return 1;
    }

    auto direct_target = temp_dir / "direct-version-target";
    if (!write_test_channel(direct_target, version_repo)) {
        sage::util::log_error("Failed to write direct archive test channel");
        return 1;
    }
    CliOptions direct_install;
    direct_install.target_root = direct_target;
    direct_install.args = {version_1_pkg.string()};
    if (cmd_install(direct_install) != 0
        || read_test_file(direct_target / "usr/bin/versioned") != "version 1\n") {
        sage::util::log_error("Direct archive install was not locked to its exact version");
        return 1;
    }
    {
        auto direct_db = sage::db::Database::open(
            direct_target / "var/lib/sage/data.mdb", true);
        auto direct_version = direct_db
            ? direct_db->get_package("versioned-package")
            : std::expected<std::optional<sage::package::PackageManifest>, std::string>(
                std::unexpected("database open failed"));
        if (!direct_version || !*direct_version
            || (**direct_version).version != version_1.version) {
            sage::util::log_error("Direct archive manifest identity was not preserved in the database");
            return 1;
        }
    }

    // A direct archive may intentionally rebuild the same identity with a
    // different payload. Paths dropped by that rebuild must be removed from
    // both the target root and the ownership database.
    auto same_identity_data = temp_dir / "same-identity-data";
    std::filesystem::create_directories(same_identity_data / "usr/bin");
    std::ofstream(same_identity_data / "usr/bin/replacement")
        << "same identity replacement\n";
    auto same_identity_archive =
        temp_dir / "versioned-package-same-identity.pkg.tar.zst";
    if (!sage::archive::create_package(
            version_1, same_identity_data, same_identity_archive)) {
        sage::util::log_error("Failed to create same-identity reinstall fixture");
        return 1;
    }
    direct_install.args = {same_identity_archive.string()};
    if (cmd_install(direct_install) != 0
        || std::filesystem::exists(direct_target / "usr/bin/versioned")
        || read_test_file(direct_target / "usr/bin/replacement")
            != "same identity replacement\n") {
        sage::util::log_error("Same-identity reinstall left a stale payload path");
        return 1;
    }
    auto same_identity_db = sage::db::Database::open(
        direct_target / "var/lib/sage/data.mdb", true);
    if (!same_identity_db) {
        sage::util::log_error("Failed to open same-identity reinstall database");
        return 1;
    }
    auto removed_owner = same_identity_db->get_file_owner("usr/bin/versioned");
    auto replacement_owner = same_identity_db->get_file_owner("usr/bin/replacement");
    if (!removed_owner || *removed_owner
        || !replacement_owner || !*replacement_owner
        || **replacement_owner != "versioned-package:system") {
        sage::util::log_error("Same-identity reinstall left stale file ownership");
        return 1;
    }

    // A normal same-package upgrade may replace files already owned by that
    // package, while still selecting the exact newer archive.
    direct_install.args = {"versioned-package"};
    if (cmd_install(direct_install) != 0
        || read_test_file(direct_target / "usr/bin/versioned") != "version 2\n") {
        sage::util::log_error("Same-package upgrade was blocked or extracted the wrong archive");
        return 1;
    }
    auto upgraded_db = sage::db::Database::open(
        direct_target / "var/lib/sage/data.mdb", true);
    auto upgraded_version = upgraded_db
        ? upgraded_db->get_package("versioned-package")
        : std::expected<std::optional<sage::package::PackageManifest>, std::string>(
            std::unexpected("database open failed"));
    if (!upgraded_version || !*upgraded_version
        || (**upgraded_version).version != version_2.version) {
        sage::util::log_error("Same-package upgrade did not record the extracted version");
        return 1;
    }

    // An explicit archive remains exact even when a newer same-name package is
    // installed. This is the local-package downgrade path used for rollback.
    direct_install.args = {version_1_pkg.string()};
    if (cmd_install(direct_install) != 0
        || read_test_file(direct_target / "usr/bin/versioned") != "version 1\n") {
        sage::util::log_error("Direct archive downgrade was silently skipped");
        return 1;
    }
    auto downgraded_db = sage::db::Database::open(
        direct_target / "var/lib/sage/data.mdb", true);
    auto downgraded_version = downgraded_db
        ? downgraded_db->get_package("versioned-package")
        : std::expected<std::optional<sage::package::PackageManifest>, std::string>(
            std::unexpected("database open failed"));
    if (!downgraded_version || !*downgraded_version
        || (**downgraded_version).version != version_1.version) {
        sage::util::log_error("Direct archive downgrade did not update installed metadata");
        return 1;
    }

    // The same version from a different architecture/channel is also a distinct
    // direct archive identity, and must replace stale files owned by the old one.
    auto alternate_data = temp_dir / "alternate-identity-data";
    auto alternate_binary = alternate_data / "opt/channels/llvm/42/bin/versioned";
    std::filesystem::create_directories(alternate_binary.parent_path());
    std::ofstream(alternate_binary) << "alternate identity\n";
    sage::package::PackageManifest alternate_identity = version_1;
    alternate_identity.arch = "any";
    alternate_identity.channel = "toolchain/llvm:42";
    auto alternate_archive = temp_dir / "versioned-package-alternate.pkg.tar.zst";
    if (!sage::archive::create_package(
            alternate_identity, alternate_data, alternate_archive)) {
        sage::util::log_error("Failed to create alternate direct archive identity fixture");
        return 1;
    }
    direct_install.args = {alternate_archive.string()};
    if (cmd_install(direct_install) != 0
        || std::filesystem::exists(direct_target / "usr/bin/versioned")
        || read_test_file(direct_target / "opt/channels/llvm/42/bin/versioned")
            != "alternate identity\n") {
        sage::util::log_error("Direct archive identity replacement was silently skipped");
        return 1;
    }
    auto alternate_db = sage::db::Database::open(
        direct_target / "var/lib/sage/data.mdb", true);
    auto alternate_record = alternate_db
        ? alternate_db->get_package("versioned-package")
        : std::expected<std::optional<sage::package::PackageManifest>, std::string>(
            std::unexpected("database open failed"));
    if (!alternate_record || !*alternate_record
        || sage::package::package_identity(**alternate_record)
            != sage::package::package_identity(alternate_identity)
        || alternate_db->get_file_owner("opt/channels/llvm/42/bin/versioned").value_or(std::nullopt)
            != "versioned-package:toolchain/llvm:42"
        || alternate_db->get_file_owner("usr/bin/versioned").value_or(std::nullopt)) {
        sage::util::log_error("Alternate direct archive identity was not recorded");
        return 1;
    }

    auto mismatch_repo = temp_dir / "identity-mismatch-repo";
    std::filesystem::create_directories(mismatch_repo);
    auto mismatch_v1 = mismatch_repo / version_1_pkg.filename();
    auto mismatch_v2 = mismatch_repo / version_2_pkg.filename();
    std::filesystem::copy_file(version_1_pkg, mismatch_v1);
    std::filesystem::copy_file(version_2_pkg, mismatch_v2);
    if (!sage::archive::generate_repo_index(mismatch_repo, "core")) {
        sage::util::log_error("Failed to generate archive identity mismatch index");
        return 1;
    }
    std::filesystem::copy_file(
        version_1_pkg, mismatch_v2, std::filesystem::copy_options::overwrite_existing);
    auto mismatch_target = temp_dir / "identity-mismatch-target";
    if (!write_test_channel(mismatch_target, mismatch_repo)) {
        sage::util::log_error("Failed to write archive identity mismatch channel");
        return 1;
    }
    CliOptions mismatch_install;
    mismatch_install.target_root = mismatch_target;
    mismatch_install.args = {"versioned-package"};
    if (cmd_install(mismatch_install) == 0
        || std::filesystem::exists(mismatch_target / "usr/bin/versioned")) {
        sage::util::log_error("Mismatched selected and archive identities mutated the target root");
        return 1;
    }

    // Different packages must not overwrite the same ordinary file. The first
    // package remains committed because the second fails before extraction.
    auto owner_repo = temp_dir / "owner-conflict-repo";
    auto owner_a_data = temp_dir / "owner-a-data";
    auto owner_b_data = temp_dir / "owner-b-data";
    std::filesystem::create_directories(owner_repo);
    std::filesystem::create_directories(owner_a_data / "usr/bin");
    std::filesystem::create_directories(owner_b_data / "usr/bin");
    std::ofstream(owner_a_data / "usr/bin/shared-file") << "owned by A\n";
    std::ofstream(owner_b_data / "usr/bin/shared-file") << "owned by B\n";
    sage::package::PackageManifest owner_a;
    owner_a.name = "owner-a";
    owner_a.version = sage::package::Version::parse("1.0.0-1");
    sage::package::PackageManifest owner_b;
    owner_b.name = "owner-b";
    owner_b.version = sage::package::Version::parse("1.0.0-1");
    owner_b.dependencies.push_back(sage::package::Dependency::parse("owner-a"));
    auto owner_a_pkg = owner_repo / "owner-a-1.0.0-1-x86_64.pkg.tar.zst";
    auto owner_b_pkg = owner_repo / "owner-b-1.0.0-1-x86_64.pkg.tar.zst";
    if (!sage::archive::create_package(owner_a, owner_a_data, owner_a_pkg)
        || !sage::archive::create_package(owner_b, owner_b_data, owner_b_pkg)
        || !sage::archive::generate_repo_index(owner_repo, "core")) {
        sage::util::log_error("Failed to create file ownership conflict fixtures");
        return 1;
    }
    auto owner_target = temp_dir / "owner-conflict-target";
    if (!write_test_channel(owner_target, owner_repo)) {
        sage::util::log_error("Failed to write file ownership conflict channel");
        return 1;
    }
    CliOptions owner_install;
    owner_install.target_root = owner_target;
    owner_install.args = {"owner-b"};
    if (cmd_install(owner_install) == 0) {
        sage::util::log_error("Different packages silently overwrote the same regular file");
        return 1;
    }
    auto owner_db = sage::db::Database::open(owner_target / "var/lib/sage/data.mdb", true);
    auto owner_a_record = owner_db
        ? owner_db->get_package("owner-a")
        : std::expected<std::optional<sage::package::PackageManifest>, std::string>(
            std::unexpected("database open failed"));
    auto owner_b_record = owner_db
        ? owner_db->get_package("owner-b")
        : std::expected<std::optional<sage::package::PackageManifest>, std::string>(
            std::unexpected("database open failed"));
    if (!owner_db || !owner_a_record || !*owner_a_record
        || !owner_b_record || *owner_b_record
        || owner_db->get_file_owner("usr/bin/shared-file").value_or(std::nullopt) != "owner-a:system"
        || read_test_file(owner_target / "usr/bin/shared-file") != "owned by A\n") {
        sage::util::log_error("File conflict changed the first package or its ownership record");
        return 1;
    }

    auto transaction_repo = temp_dir / "transaction-repo";
    auto transaction_a_data = temp_dir / "transaction-a-data";
    auto transaction_b_data = temp_dir / "transaction-b-data";
    std::filesystem::create_directories(transaction_repo);
    std::filesystem::create_directories(transaction_a_data / "usr/bin");
    std::ofstream(transaction_a_data / "usr/bin/transaction-a") << "committed package\n";
    auto transaction_clang = transaction_a_data / "opt/channels/llvm/99/bin/clang";
    std::filesystem::create_directories(transaction_clang.parent_path());
    std::ofstream(transaction_clang) << "#!/bin/sh\nexit 0\n";
    std::filesystem::permissions(
        transaction_clang,
        std::filesystem::perms::owner_all
            | std::filesystem::perms::group_read
            | std::filesystem::perms::group_exec
            | std::filesystem::perms::others_read
            | std::filesystem::perms::others_exec);
    std::filesystem::create_directories(transaction_b_data / "usr/share");
    std::filesystem::create_symlink("elsewhere", transaction_b_data / "usr/share/blocked");

    sage::package::PackageManifest transaction_a;
    transaction_a.name = "transaction-a";
    transaction_a.version = sage::package::Version::parse("1.0.0-1");
    transaction_a.channel = "toolchain/llvm:99";
    sage::package::PackageManifest transaction_b;
    transaction_b.name = "transaction-b";
    transaction_b.version = sage::package::Version::parse("1.0.0-1");
    transaction_b.channel = "system";
    transaction_b.dependencies.push_back(sage::package::Dependency::parse("transaction-a"));

    auto transaction_a_pkg = transaction_repo / "transaction-a-1.0.0-1-x86_64.pkg.tar.zst";
    auto transaction_b_pkg = transaction_repo / "transaction-b-1.0.0-1-x86_64.pkg.tar.zst";
    if (!sage::archive::create_package(transaction_a, transaction_a_data, transaction_a_pkg)
        || !sage::archive::create_package(transaction_b, transaction_b_data, transaction_b_pkg)
        || !sage::archive::generate_repo_index(transaction_repo, "core")) {
        sage::util::log_error("Failed to create multi-package transaction fixture");
        return 1;
    }

    auto transaction_target = temp_dir / "transaction-target";
    std::filesystem::create_directories(transaction_target / "etc/sage");
    std::filesystem::create_directories(transaction_target / "usr/share/blocked");
    std::ofstream(transaction_target / "usr/share/blocked/keep") << "must survive\n";
    std::ofstream transaction_channels(transaction_target / "etc/sage/channels.toml");
    transaction_channels
        << "schema_version = 1\n\n[[channels]]\nname = \"core\"\nurl = \"file://"
        << transaction_repo.string()
        << "\"\nscope = \"system\"\npriority = 100\nenabled = true\n";
    transaction_channels.close();

    CliOptions transaction_install;
    transaction_install.target_root = transaction_target;
    transaction_install.args = {"transaction-b"};
    if (cmd_install(transaction_install) == 0) {
        sage::util::log_error("Multi-package install accepted a later package path conflict");
        return 1;
    }
    auto transaction_db = sage::db::Database::open(
        transaction_target / "var/lib/sage/data.mdb", true);
    if (!transaction_db) {
        sage::util::log_error("Failed to inspect multi-package transaction database");
        return 1;
    }
    auto transaction_a_record = transaction_db->get_package("transaction-a");
    auto transaction_b_record = transaction_db->get_package("transaction-b");
    auto transaction_cc_link = transaction_target / "etc/sage/profiles/default/bin/cc";
    std::error_code transaction_cc_ec;
    if (!transaction_a_record || !*transaction_a_record
        || !transaction_b_record || *transaction_b_record
        || !std::filesystem::exists(transaction_target / "usr/bin/transaction-a")
        || !std::filesystem::exists(transaction_target / "usr/share/blocked/keep")
        || !std::filesystem::is_symlink(transaction_cc_link, transaction_cc_ec)
        || std::filesystem::read_symlink(transaction_cc_link, transaction_cc_ec)
            != "/opt/channels/llvm/99/bin/clang"
        || !std::filesystem::exists(transaction_target / "etc/profile.d/sage-channels.sh")) {
        sage::util::log_error(
            "A later package failure desynchronized an earlier committed package or skipped its post-processing");
        return 1;
    }

    // Split packages in one toolchain slot must refresh activation as each
    // package commits. The dependency installs libraries first; the compiler
    // package that follows is what makes the cc alias possible.
    auto split_repo = temp_dir / "split-toolchain-repo";
    auto split_libs_data = temp_dir / "split-toolchain-libs-data";
    auto split_compiler_data = temp_dir / "split-toolchain-compiler-data";
    auto split_library = split_libs_data / "opt/channels/llvm/77/lib/libsplit.so";
    auto split_clang = split_compiler_data / "opt/channels/llvm/77/bin/clang";
    std::filesystem::create_directories(split_repo);
    std::filesystem::create_directories(split_library.parent_path());
    std::filesystem::create_directories(split_clang.parent_path());
    std::ofstream(split_library) << "split toolchain library\n";
    std::ofstream(split_clang) << "#!/bin/sh\nexit 0\n";
    std::filesystem::permissions(
        split_clang,
        std::filesystem::perms::owner_all
            | std::filesystem::perms::group_read
            | std::filesystem::perms::group_exec
            | std::filesystem::perms::others_read
            | std::filesystem::perms::others_exec);

    sage::package::PackageManifest split_libs;
    split_libs.name = "split-toolchain-libs";
    split_libs.version = sage::package::Version::parse("1.0.0-1");
    split_libs.channel = "toolchain/llvm:77";
    sage::package::PackageManifest split_compiler;
    split_compiler.name = "split-toolchain-compiler";
    split_compiler.version = sage::package::Version::parse("1.0.0-1");
    split_compiler.channel = "toolchain/llvm:77";
    split_compiler.dependencies.push_back(
        sage::package::Dependency::parse("split-toolchain-libs"));

    auto split_libs_archive =
        split_repo / "split-toolchain-libs-1.0.0-1-x86_64.pkg.tar.zst";
    auto split_compiler_archive =
        split_repo / "split-toolchain-compiler-1.0.0-1-x86_64.pkg.tar.zst";
    if (!sage::archive::create_package(split_libs, split_libs_data, split_libs_archive)
        || !sage::archive::create_package(
            split_compiler, split_compiler_data, split_compiler_archive)
        || !sage::archive::generate_repo_index(split_repo, "core")) {
        sage::util::log_error("Failed to create split toolchain fixtures");
        return 1;
    }

    auto split_target = temp_dir / "split-toolchain-target";
    if (!write_test_channel(split_target, split_repo)) {
        sage::util::log_error("Failed to write split toolchain test channel");
        return 1;
    }
    CliOptions split_install;
    split_install.target_root = split_target;
    split_install.args = {"split-toolchain-compiler"};
    if (cmd_install(split_install) != 0) {
        sage::util::log_error("Failed to install split toolchain packages");
        return 1;
    }
    auto split_cc_link = split_target / "etc/sage/profiles/default/bin/cc";
    std::error_code split_cc_ec;
    if (!std::filesystem::is_symlink(split_cc_link, split_cc_ec)
        || std::filesystem::read_symlink(split_cc_link, split_cc_ec)
            != "/opt/channels/llvm/77/bin/clang") {
        sage::util::log_error("Later split toolchain package did not refresh activation");
        return 1;
    }

    auto split_owner_db = sage::db::Database::open(
        split_target / "var/lib/sage/data.mdb", true);
    if (!split_owner_db
        || split_owner_db->get_file_owner("opt/channels/llvm/77/bin/clang").value_or(std::nullopt)
            != "split-toolchain-compiler:toolchain/llvm:77") {
        sage::util::log_error("Split toolchain removal fixture has no registered file owner");
        return 1;
    }

    // Replacing an owned regular file with a non-empty directory forces a real
    // filesystem removal error. The command must fail and keep both DB records.
    auto installed_split_clang = split_target / "opt/channels/llvm/77/bin/clang";
    std::filesystem::remove(installed_split_clang);
    std::filesystem::create_directories(installed_split_clang);
    std::ofstream(installed_split_clang / "keep") << "block removal\n";
    CliOptions split_remove;
    split_remove.target_root = split_target;
    split_remove.args = {"split-toolchain-compiler"};
    if (cmd_remove(split_remove) == 0) {
        sage::util::log_error("Package removal ignored a real filesystem error");
        return 1;
    }
    {
        auto split_db = sage::db::Database::open(
            split_target / "var/lib/sage/data.mdb", true);
        auto split_compiler_record = split_db
            ? split_db->get_package("split-toolchain-compiler")
            : std::expected<std::optional<sage::package::PackageManifest>, std::string>(
                std::unexpected("database open failed"));
        auto split_libs_record = split_db
            ? split_db->get_package("split-toolchain-libs")
            : std::expected<std::optional<sage::package::PackageManifest>, std::string>(
                std::unexpected("database open failed"));
        if (!split_compiler_record || !*split_compiler_record
            || !split_libs_record || !*split_libs_record) {
            sage::util::log_error("Failed split removal discarded installed database records");
            return 1;
        }
    }

    // Restore the owned file and verify removal uses the sysroot-relative paths
    // stored in the manifest instead of prepending the toolchain root twice.
    std::error_code restore_ec;
    std::filesystem::remove_all(installed_split_clang, restore_ec);
    if (restore_ec) {
        sage::util::log_error("Failed to restore split toolchain fixture: {}", restore_ec.message());
        return 1;
    }
    std::ofstream(installed_split_clang) << "#!/bin/sh\nexit 0\n";
    if (cmd_remove(split_remove) != 0
        || std::filesystem::exists(split_target / "opt/channels/llvm/77/bin/clang")
        || std::filesystem::exists(split_target / "opt/channels/llvm/77/lib/libsplit.so")) {
        sage::util::log_error("Toolchain removal did not delete sysroot-relative package paths");
        return 1;
    }
    auto removed_split_db = sage::db::Database::open(
        split_target / "var/lib/sage/data.mdb", true);
    auto removed_split_packages = removed_split_db
        ? removed_split_db->list_installed_packages()
        : std::expected<std::vector<sage::package::PackageManifest>, std::string>(
            std::unexpected("database open failed"));
    if (!removed_split_packages || !removed_split_packages->empty()) {
        sage::util::log_error("Split toolchain database was not cleared after removal");
        return 1;
    }
    sage::util::log_success("9. End-to-End `sage install` & `sage remove` to Target Root OK");

    // 10. Complete Closed-Loop: `sage build` -> `sage repo index` -> `sage install` -> `sage remove` with Orphan Cleanup
    auto build_test_dir = temp_dir / "build_test";
    std::filesystem::create_directories(build_test_dir / "libsample");
    std::filesystem::create_directories(build_test_dir / "sample-app");
    std::filesystem::create_directories(build_test_dir / "repo");
    // Both recipes produce their payload from an `install` phase writing into
    // $DESTDIR. `cmd_build` clears <recipe>/pkg/ before running the phases, so
    // a payload staged there beforehand would be deleted and the package would
    // come out empty; going through the phase is also what exercises the
    // DESTDIR contract these packages are meant to demonstrate.

    // 1. Write libsample recipe
    std::ofstream lib_recipe(build_test_dir / "libsample/recipe.toml");
    lib_recipe << R"(schema_version = 1
[package]
name = "libsample"
version = "1.2.0"
release = "1"
description = "Sample dynamic library"
license = "MIT"
channel = "system"

provides = ["libsample", "so:libsample.so.1"]

install = [
    'mkdir -p "$DESTDIR/usr/lib"',
    'printf "/* libsample binary */\n" > "$DESTDIR/usr/lib/libsample.so.1"',
]
)";
    lib_recipe.close();

    // 2. Write sample-app recipe
    std::ofstream app_recipe(build_test_dir / "sample-app/recipe.toml");
    app_recipe << R"(schema_version = 1
[package]
name = "sample-app"
version = "2.0.0"
release = "1"
description = "Sample user application"
license = "GPL-3.0"
channel = "system"

dependencies = ["libsample >= 1.0.0"]

install = [
    'mkdir -p "$DESTDIR/usr/bin"',
    'printf "#!/bin/sh\necho running sample-app\n" > "$DESTDIR/usr/bin/sample-app"',
    'chmod 755 "$DESTDIR/usr/bin/sample-app"',
]
)";
    app_recipe.close();

    // 3. Execute `sage build` on both packages
    CliOptions build_lib_opts;
    build_lib_opts.args = {(build_test_dir / "libsample").string()};
    if (cmd_build(build_lib_opts) != 0) {
        sage::util::log_error("Failed to build libsample");
        return 1;
    }

    CliOptions build_app_opts;
    build_app_opts.args = {(build_test_dir / "sample-app").string()};
    if (cmd_build(build_app_opts) != 0) {
        sage::util::log_error("Failed to build sample-app");
        return 1;
    }

    // Move built packages to repo/. The names carry the arch suffix that
    // `cmd_build` emits; the recipes above declare no `arch`, so both land on
    // the PackageManifest default.
    auto stage_package = [&](const std::filesystem::path& built,
                             const std::filesystem::path& into) -> bool {
        std::error_code ec;
        std::filesystem::copy_file(built, into, std::filesystem::copy_options::overwrite_existing, ec);
        if (ec) {
            sage::util::log_error("Failed to stage {} into the test repo: {}",
                built.filename().string(), ec.message());
            return false;
        }
        return true;
    };

    if (!stage_package(build_test_dir / "libsample/libsample-1.2.0-1-x86_64.pkg.tar.zst",
                       build_test_dir / "repo/libsample-1.2.0-1-x86_64.pkg.tar.zst")) {
        return 1;
    }
    if (!stage_package(build_test_dir / "sample-app/sample-app-2.0.0-1-x86_64.pkg.tar.zst",
                       build_test_dir / "repo/sample-app-2.0.0-1-x86_64.pkg.tar.zst")) {
        return 1;
    }

    // 4. Generate local repo index
    auto build_idx_res = sage::archive::generate_repo_index(build_test_dir / "repo", "core");
    if (!build_idx_res) {
        sage::util::log_error("Failed to generate index for built packages");
        return 1;
    }

    // 5. Point isolated target to the newly built repo
    auto loop_target = build_test_dir / "target_root";
    std::filesystem::create_directories(loop_target / "etc/sage");
    std::ofstream loop_chan(loop_target / "etc/sage/channels.toml");
    loop_chan << "schema_version = 1\n\n[[channels]]\nname = \"core\"\nurl = \"file://" << (build_test_dir / "repo").string() << "\"\nscope = \"system\"\npriority = 100\nenabled = true\n";
    loop_chan.close();

    // 6. Install sample-app (which requires libsample)
    CliOptions loop_inst_opts;
    loop_inst_opts.target_root = loop_target;
    loop_inst_opts.args = {"sample-app"};
    if (cmd_install(loop_inst_opts) != 0) {
        sage::util::log_error("Failed to install built sample-app");
        return 1;
    }

    // Verify files on disk and packages in LMDB
    if (!std::filesystem::exists(loop_target / "usr/bin/sample-app") || 
        !std::filesystem::exists(loop_target / "usr/lib/libsample.so.1")) {
        sage::util::log_error("Files from sample-app and libsample were not properly installed to target root");
        return 1;
    }

    // 7. Verify Reverse Dependency Protection: Attempting to remove libsample directly while sample-app depends on it must fail!
    CliOptions direct_lib_rem;
    direct_lib_rem.target_root = loop_target;
    direct_lib_rem.args = {"libsample"};
    if (cmd_remove(direct_lib_rem) == 0) {
        sage::util::log_error("Direct removal of libsample should have been blocked by reverse dependency protection!");
        return 1;
    }

    // 8. Remove with --cascade: should remove both libsample and sample-app!
    direct_lib_rem.cascade = true;
    if (cmd_remove(direct_lib_rem) != 0) {
        sage::util::log_error("Failed to cascaded-remove libsample and sample-app");
        return 1;
    }

    // Verify all files from both sample-app and libsample are gone from disk!
    if (std::filesystem::exists(loop_target / "usr/bin/sample-app") || 
        std::filesystem::exists(loop_target / "usr/lib/libsample.so.1")) {
        sage::util::log_error("Files still exist on disk after cascade removal");
        return 1;
    }

    // Verify LMDB is clean
    auto loop_db = sage::db::Database::open(loop_target / "var/lib/sage/data.mdb");
    if (!loop_db) {
        sage::util::log_error("LMDB unavailable after cascaded removal");
        return 1;
    }
    auto loop_packages = loop_db->list_installed_packages();
    if (!loop_packages || !loop_packages->empty()) {
        sage::util::log_error("LMDB still contains packages after cascaded removal");
        return 1;
    }

    sage::util::log_success("10. Complete Build -> Index -> Install -> Remove (Auto Orphan Cleanup) Closed-Loop OK");
    sage::util::log_success("11. Reverse Dependency Protection & Cascade Removal Safety Locks OK");

    std::filesystem::remove_all(temp_dir);
    sage::util::log_success("🎉 All Sage Master Architecture & Subsystem Integration Tests Passed Successfully!");
    return 0;
}

// ============================================================================
// `sage list` / `sage count` -- installed package inventory
// ============================================================================

namespace {

// Substring match, or glob when the pattern carries a wildcard. A bare name is
// far more often meant as "anything containing this" than as an exact match,
// but `sage count 'python-*'` should still mean what it looks like.
bool inventory_matches(std::string_view name, std::string_view pattern) {
    if (pattern.empty()) return true;
    if (pattern.find_first_of("*?[") == std::string_view::npos) {
        return name.find(pattern) != std::string_view::npos;
    }
    return sage::util::glob_match(pattern, name);
}

std::expected<std::vector<sage::package::PackageManifest>, std::string>
collect_installed(const CliOptions& opts, std::string_view pattern) {
    auto cfg_res = sage::config::SystemConfig::load_from_root(opts.target_root);
    auto db_res = sage::db::Database::open(
        cfg_res ? cfg_res->db_path : std::filesystem::path("/var/lib/sage/data.mdb"), true);
    if (!db_res) return std::unexpected(db_res.error());

    auto list = db_res->list_installed_packages();
    if (!list) return std::unexpected(list.error());
    std::erase_if(*list, [&](const auto& pkg) { return !inventory_matches(pkg.name, pattern); });
    std::ranges::sort(*list, {}, &sage::package::PackageManifest::name);
    return std::move(*list);
}

} // namespace

int cmd_list(const CliOptions& opts) {
    bool quiet = false;
    std::string pattern;
    for (const auto& a : opts.args) {
        if (a == "--quiet" || a == "-q") quiet = true;
        else if (a == "--help" || a == "-h") {
            std::println("Usage: sage list [-q|--quiet] [PATTERN]");
            return 0;
        } else if (pattern.empty()) pattern = a;
    }

    auto list_res = collect_installed(opts, pattern);
    if (!list_res) {
        // Nothing is installed on a root that has no database yet. That is a
        // legitimate answer to "what is installed", not a failure, so -q stays
        // silent and scripts reading it see an empty list rather than an error.
        if (!quiet) sage::util::log_warn("Database not yet initialized or inaccessible: {}", list_res.error());
        return quiet ? 0 : 0;
    }
    const auto& list = *list_res;

    // -q prints bare names, one per line, so the output can be piped straight
    // into xargs without anything having to strip decoration off it.
    if (quiet) {
        for (const auto& pkg : list) std::println("{}", pkg.name);
        return 0;
    }

    if (list.empty()) {
        std::println("No installed packages in '{}'{}", opts.target_root.string(),
            pattern.empty() ? "" : std::format(" matching '{}'", pattern));
        return 0;
    }

    std::uintmax_t total_size = 0;
    for (const auto& pkg : list) total_size += pkg.installed_size;

    std::println("Installed packages in '{}'{}:", opts.target_root.string(),
        pattern.empty() ? "" : std::format(" matching '{}'", pattern));
    for (const auto& pkg : list) {
        std::println("  {:<28} {:<16} [{}]", pkg.name, pkg.version.to_string(), pkg.channel);
    }
    std::println("");
    std::println("{} package(s), {} installed", list.size(), sage::util::format_size(total_size));
    return 0;
}

int cmd_count(const CliOptions& opts) {
    std::string pattern;
    for (const auto& a : opts.args) {
        if (a == "--help" || a == "-h") {
            std::println("Usage: sage count [PATTERN]");
            return 0;
        }
        if (pattern.empty()) pattern = a;
    }

    auto list_res = collect_installed(opts, pattern);
    // A bare number and nothing else: this exists to be captured in a shell
    // substitution, so it must not print a word even when the root is empty.
    std::println("{}", list_res ? list_res->size() : 0u);
    return 0;
}

int cmd_query(const CliOptions& opts) {
    if (opts.args.empty()) {
        std::println("Usage: sage query [installed|count|info <pkg>|owner <path>|files <pkg>|capabilities]");
        return 1;
    }

    std::string sub = opts.args[0];
    auto cfg_res = sage::config::SystemConfig::load_from_root(opts.target_root);
    auto db_res = sage::db::Database::open(cfg_res ? cfg_res->db_path : std::filesystem::path("/var/lib/sage/data.mdb"), true);
    if (!db_res) {
        sage::util::log_warn("Database not yet initialized or inaccessible: {}", db_res.error());
        return 0;
    }
    auto& db = *db_res;

    if (sub == "files" && opts.args.size() >= 2) {
        std::string pkg_name = opts.args[1];
        auto pkg_res = db.get_package(pkg_name);
        if (!pkg_res) {
            sage::util::log_error(
                "Failed to read package '{}': {}", pkg_name, pkg_res.error());
            return 1;
        }
        if (!*pkg_res) {
            sage::util::log_error("Package '{}' is not installed", pkg_name);
            return 1;
        }
        const auto& pkg = **pkg_res;
        if (pkg.files.empty()) {
            // Packages registered before files.idx existed have paths in the
            // files table but no per-file metadata; fall back to those rather
            // than claiming the package owns nothing.
            auto paths = db.get_package_files(pkg_name);
            if (!paths) {
                sage::util::log_error(
                    "Failed to read files for '{}': {}", pkg_name, paths.error());
                return 1;
            }
            if (paths->empty()) {
                std::println("{} owns no files", pkg_name);
                return 0;
            }
            sage::util::log_warn("'{}' predates files.idx: listing paths without hashes", pkg_name);
            for (const auto& path : *paths) std::println("/{}", path);
            return 0;
        }
        for (const auto& f : pkg.files) {
            std::println("{:<4} {:>6o} {:>10}  {:<64}  /{}",
                sage::package::to_string(f.type), f.mode, f.size,
                f.sha256.empty() ? "-" : f.sha256, f.path);
        }
        return 0;
    }

    if (sub == "capabilities") {
        auto list = db.list_installed_packages();
        if (!list) {
            sage::util::log_error("Failed to list installed packages: {}", list.error());
            return 1;
        }
        std::map<std::string, std::vector<std::string>> by_cap;
        for (const auto& pkg : *list) {
            for (const auto& prov : pkg.provides) {
                if (!prov.starts_with("virtual/")) continue;
                by_cap[prov].push_back(pkg.name);
            }
        }
        auto cfg = cfg_res ? *cfg_res : sage::config::SystemConfig::default_config();
        std::println("Virtual capabilities in '{}':", opts.target_root.string());
        for (const auto& [cap, providers] : by_cap) {
            auto bound = cfg.provider_for(cap);
            std::println("  • {:<34} {:<10} providers: {}{}",
                cap,
                sage::config::to_string(cfg.capability_kind(cap)),
                sage::util::join(providers, ", "),
                bound ? std::format("  (bound: {})", *bound) : std::string{});
        }
        if (by_cap.empty()) std::println("  (none)");
        return 0;
    }

    if (sub == "count") {
        auto list = db.list_installed_packages();
        if (!list) {
            sage::util::log_error("Failed to count installed packages: {}", list.error());
            return 1;
        }
        std::println("{}", list->size());
        return 0;
    }

    if (sub == "installed") {
        auto list = db.list_installed_packages();
        if (!list) {
            sage::util::log_error("Installed package database is inconsistent: {}", list.error());
            return 1;
        }
        // LMDB hands these back in key order already, but sorting explicitly
        // keeps the listing stable if the key layout ever changes.
        std::ranges::sort(*list, {}, &sage::package::PackageManifest::name);
        std::println("Installed packages in '{}' ({} total):", opts.target_root.string(), list->size());
        for (const auto& pkg : *list) {
            std::println("  • {:<20} {:<15} [{}]", pkg.name, pkg.version.to_string(), pkg.channel);
        }
    } else if (sub == "info" && opts.args.size() >= 2) {
        std::string pkg_name = opts.args[1];
        auto pkg_res = db.get_package(pkg_name);
        if (!pkg_res) {
            sage::util::log_error("Installed package database is inconsistent: {}", pkg_res.error());
            return 1;
        }
        if (*pkg_res) {
            const auto& pkg = **pkg_res;
            std::println("Package:     {}", pkg.name);
            std::println("Version:     {}", pkg.version.to_string());
            std::println("Channel:     {}", pkg.channel);
            std::println("License:     {}", pkg.license);
            std::println("Description: {}", pkg.description);
            std::println("Provides:    {}", sage::util::join(pkg.provides, ", "));
        } else {
            sage::util::log_error("Package '{}' is not installed", pkg_name);
            return 1;
        }
    } else if (sub == "owner" && opts.args.size() >= 2) {
        std::string path = opts.args[1];
        auto owner = db.get_file_owner(path);
        if (!owner) {
            sage::util::log_error("Failed to read file ownership: {}", owner.error());
            return 1;
        }
        if (*owner) {
            std::println("{} is owned by {}", path, **owner);
        } else {
            std::println("No installed package owns {}", path);
        }
    }
    return 0;
}

// Check installed files against the hashes recorded at install time.
int cmd_verify(const CliOptions& opts) {
    auto cfg_res = sage::config::SystemConfig::load_from_root(opts.target_root);
    if (!cfg_res) {
        sage::util::log_error("Failed to load configuration: {}", cfg_res.error());
        return 1;
    }
    auto db_res = sage::db::Database::open(cfg_res->db_path, true);
    if (!db_res) {
        sage::util::log_error("Failed to open database at {}: {}", cfg_res->db_path.string(), db_res.error());
        return 1;
    }

    std::vector<sage::package::PackageManifest> targets;
    if (opts.args.empty()) {
        auto installed = db_res->list_installed_packages();
        if (!installed) {
            sage::util::log_error("Failed to list installed packages: {}", installed.error());
            return 1;
        }
        targets = std::move(*installed);
    } else {
        for (const auto& name : opts.args) {
            auto pkg_res = db_res->get_package(name);
            if (!pkg_res) {
                sage::util::log_error("Failed to read package '{}': {}", name, pkg_res.error());
                return 1;
            }
            if (!*pkg_res) {
                sage::util::log_error("Package '{}' is not installed", name);
                return 1;
            }
            targets.push_back(std::move(**pkg_res));
        }
    }

    size_t checked = 0;
    size_t modified = 0;
    size_t missing = 0;
    size_t unhashed = 0;

    for (const auto& pkg : targets) {
        for (const auto& f : pkg.files) {
            if (f.type != sage::package::FileType::Regular) continue;
            if (f.sha256.empty()) { unhashed++; continue; }

            std::filesystem::path on_disk = opts.target_root / f.path;
            if (!std::filesystem::exists(on_disk)) {
                std::println("missing   {:<20} /{}", pkg.name, f.path);
                missing++;
                continue;
            }
            auto hash = sage::util::compute_file_sha256(on_disk);
            checked++;
            if (!hash) {
                std::println("unreadable {:<20} /{}", pkg.name, f.path);
                missing++;
            } else if (*hash != f.sha256) {
                std::println("modified  {:<20} /{}", pkg.name, f.path);
                modified++;
            }
        }
    }

    if (unhashed > 0) {
        sage::util::log_warn("{} file(s) carry no recorded hash (installed before files.idx existed)", unhashed);
    }
    if (modified == 0 && missing == 0) {
        sage::util::log_success("Verified {} file(s) across {} package(s): all match", checked, targets.size());
        return 0;
    }
    sage::util::log_error("{} modified, {} missing out of {} file(s) checked", modified, missing, checked);
    return 1;
}

int cmd_toolchain(const CliOptions& opts) {
    if (opts.args.empty() || opts.args[0] == "list") {
        auto list = sage::channel::ProfileManager::list_installed_subchannels(opts.target_root);
        std::println("Installed Toolchains & Runtimes in '{}':", opts.target_root.string());
        if (list.empty()) {
            std::println("  (No sub-channels currently installed in /opt/channels or /usr/lib/runtimes)");
            return 0;
        }
        for (const auto& sc : list) {
            std::println("  • {:<12} slot: {:<10} scope: {:<10} path: {}", 
                sc.category, sc.slot, sage::channel::to_string(sc.scope), sc.path.string());
        }
        return 0;
    }

    if (opts.args[0] == "use" && opts.args.size() >= 2) {
        auto spec = sage::channel::SubChannelSpec::parse(opts.args[1]);
        auto res = sage::channel::ProfileManager::switch_active_toolchain(opts.target_root, spec.category, spec.slot);
        if (!res) {
            sage::util::log_error("{}", res.error());
            return 1;
        }
        return 0;
    }

    std::println("Usage: sage toolchain [list|use <category:slot>]");
    return 1;
}

int cmd_java(const CliOptions& opts) {
    if (opts.args.empty() || opts.args[0] == "list") {
        auto list = sage::channel::ProfileManager::list_installed_subchannels(opts.target_root);
        std::println("Installed Java Environments in '{}':", opts.target_root.string());
        size_t count = 0;
        for (const auto& sc : list) {
            if (sc.category == "java" || sc.category.starts_with("openjdk") || sc.category.starts_with("graalvm")) {
                std::println("  • {:<15} slot: {:<10} path: {}", sc.category, sc.slot, sc.path.string());
                count++;
            }
        }
        if (count == 0) {
            std::println("  (No Java sub-channels installed in /opt/channels/java)");
        }
        return 0;
    }

    if (opts.args[0] == "use" && opts.args.size() >= 2) {
        std::string slot = opts.args[1];
        auto res = sage::channel::ProfileManager::switch_active_toolchain(opts.target_root, "java", slot);
        if (!res) {
            sage::util::log_error("{}", res.error());
            return 1;
        }
        return 0;
    }

    std::println("Usage: sage java [list|use <slot>]");
    return 1;
}

int cmd_rust(const CliOptions& opts) {
    if (opts.args.empty() || opts.args[0] == "list") {
        auto list = sage::channel::ProfileManager::list_installed_subchannels(opts.target_root);
        std::println("Installed Rust Toolchains in '{}':", opts.target_root.string());
        size_t count = 0;
        for (const auto& sc : list) {
            if (sc.category == "rust") {
                std::println("  • {:<15} slot: {:<10} path: {}", sc.category, sc.slot, sc.path.string());
                count++;
            }
        }
        if (count == 0) {
            std::println("  (No Rust sub-channels installed in /opt/channels/rust)");
        }
        return 0;
    }

    if (opts.args[0] == "use" && opts.args.size() >= 2) {
        std::string slot = opts.args[1];
        auto res = sage::channel::ProfileManager::switch_active_toolchain(opts.target_root, "rust", slot);
        if (!res) {
            sage::util::log_error("{}", res.error());
            return 1;
        }
        return 0;
    }

    std::println("Usage: sage rust [list|use <slot>]");
    return 1;
}

int cmd_shell(const CliOptions& opts) {
    std::vector<sage::channel::SubChannelSpec> specs;
    for (size_t i = 0; i < opts.args.size(); ++i) {
        if (opts.args[i] == "--with" && i + 1 < opts.args.size()) {
            specs.push_back(sage::channel::SubChannelSpec::parse(opts.args[++i]));
        }
    }

    if (specs.empty()) {
        std::println("Usage: sage shell --with <sub-channel...> (e.g. sage shell --with toolchain/llvm:22 --with runtime/python:3.12)");
        return 1;
    }

    auto env = sage::channel::ProfileManager::generate_shell_env(opts.target_root, specs);
    sage::util::log_info("Entering Ephemeral Sandboxed Shell with {} sub-channels...", specs.size());

    // Export generated environment variables
    for (const auto& [k, v] : env) {
        if (const char* old_val = std::getenv(k.c_str())) {
            ::setenv(k.c_str(), (v + ":" + old_val).c_str(), 1);
        } else {
            ::setenv(k.c_str(), v.c_str(), 1);
        }
    }
    ::setenv("PS1", "(sage-env) \\u@\\h:\\w\\$ ", 1);

    const char* user_shell = std::getenv("SHELL");
    if (!user_shell) user_shell = "/bin/sh";
    int ret = ::system(user_shell);
    (void)ret;
    return 0;
}

int cmd_service(const CliOptions& opts) {
    if (opts.args.empty()) {
        std::println("Usage: sage service [list|generate <name>]");
        return 1;
    }
    std::string sub = opts.args[0];
    if (sub == "list") {
        sage::util::log_info("Available native init targets: OpenRC, Runit, Systemd, Dinit, s6");
    } else if (sub == "generate" && opts.args.size() >= 2) {
        std::string name = opts.args[1];
        sage::service::ServiceSpec spec;
        spec.name = name;
        spec.exec_start = "/usr/bin/" + name;
        auto res = sage::service::generate_service(spec, sage::service::InitType::OpenRC, opts.target_root);
        if (res) {
            sage::util::log_success("Generated OpenRC service script at {}", res->string());
        }
    }
    return 0;
}

// Persist the channel list to the *target root's* channels.toml.
//
// This must always write the target's own file. SystemConfig falls back to the
// host's /etc/sage/channels.toml when the target root has none, so a target
// that is merely borrowing the host's channels would otherwise have an edit
// silently applied to the host instead.
int write_channels(const sage::config::SystemConfig& cfg) {
    std::error_code ec;
    std::filesystem::create_directories(cfg.channels_config_path.parent_path(), ec);
    std::ofstream out(cfg.channels_config_path);
    if (!out.is_open()) {
        sage::util::log_error("Cannot write {}", cfg.channels_config_path.string());
        return 1;
    }
    out << cfg.serialize_channels_toml();
    return 0;
}

int cmd_channel(const CliOptions& opts) {
    auto cfg_res = sage::config::SystemConfig::load_from_root(opts.target_root);
    if (!cfg_res) {
        sage::util::log_error("Failed to load configuration: {}", cfg_res.error());
        return 1;
    }
    auto& cfg = *cfg_res;

    std::string sub = opts.args.empty() ? "list" : opts.args[0];

    if (sub == "list") {
        std::println("Configured Channels for '{}':", opts.target_root.string());
        if (cfg.channels.empty()) {
            std::println("  (none configured)");
        }
        for (const auto& ch : cfg.channels) {
            std::println("  • {:<15} {:<45} scope: {:<10} priority: {:<5} {}",
                ch.name, ch.url, ch.scope, ch.priority, ch.enabled ? "enabled" : "disabled");
        }
        return 0;
    }

    if (sub == "add") {
        if (opts.args.size() < 3) {
            std::println("Usage: sage channel add <NAME> <URL> [SCOPE] [PRIORITY]");
            std::println("  SCOPE: system (default) | runtime | toolchain | user");
            return 1;
        }
        sage::config::ChannelConfig ch;
        ch.name = opts.args[1];
        ch.url = opts.args[2];
        ch.scope = opts.args.size() >= 4 ? opts.args[3] : "system";
        ch.priority = opts.args.size() >= 5 ? std::atoi(opts.args[4].c_str()) : 50;
        ch.enabled = true;

        auto existing = std::ranges::find(cfg.channels, ch.name, &sage::config::ChannelConfig::name);
        if (existing != cfg.channels.end()) {
            sage::util::log_info("Updating existing channel '{}'", ch.name);
            *existing = std::move(ch);
        } else {
            cfg.channels.push_back(std::move(ch));
        }

        if (int rc = write_channels(cfg); rc != 0) return rc;
        sage::util::log_success("Channel '{}' written to {}", opts.args[1], cfg.channels_config_path.string());
        return 0;
    }

    if (sub == "remove" || sub == "rm") {
        if (opts.args.size() < 2) {
            std::println("Usage: sage channel remove <NAME>");
            return 1;
        }
        const std::string& name = opts.args[1];
        auto removed = std::erase_if(cfg.channels, [&](const sage::config::ChannelConfig& c) { return c.name == name; });
        if (removed == 0) {
            sage::util::log_error("No channel named '{}' is configured", name);
            return 1;
        }
        if (int rc = write_channels(cfg); rc != 0) return rc;
        sage::util::log_success("Channel '{}' removed from {}", name, cfg.channels_config_path.string());
        return 0;
    }

    if (sub == "sync") {
        // Optional second argument narrows the sync to a single channel;
        // --channel does the same, so accept whichever the user reached for.
        std::string only = opts.args.size() >= 2 ? opts.args[1] : opts.channel_filter;

        size_t synced = 0;
        size_t failed = 0;
        for (const auto& ch_cfg : cfg.channels) {
            if (!only.empty() && ch_cfg.name != only) continue;
            if (!ch_cfg.enabled) {
                sage::util::log_info("Skipping disabled channel '{}'", ch_cfg.name);
                continue;
            }
            sage::channel::Channel ch;
            ch.name = ch_cfg.name;
            ch.url = ch_cfg.url;
            ch.scope = sage::channel::parse_scope(ch_cfg.scope);

            auto idx = sage::channel::ProfileManager::sync_channel(ch, cfg.cache_dir);
            if (!idx) {
                sage::util::log_error("Channel '{}': {}", ch_cfg.name, idx.error());
                failed++;
                continue;
            }
            sage::util::log_success("Channel '{}': {} packages", ch_cfg.name, idx->available_packages.size());
            synced++;
        }

        if (synced == 0 && failed == 0) {
            sage::util::log_warn("Nothing to sync{}", only.empty() ? "" : std::format(": no enabled channel named '{}'", only));
            return only.empty() ? 0 : 1;
        }
        // A partial sync is still a failure: the pool is now inconsistent with
        // what the caller asked for, and installing from it would pick stale
        // versions without saying so.
        return failed == 0 ? 0 : 1;
    }

    std::println("Usage: sage channel [list|add <NAME> <URL> [SCOPE] [PRIORITY]|remove <NAME>|sync [NAME]]");
    return 1;
}

int cmd_status(const CliOptions& opts) {
    auto cfg_res = sage::config::SystemConfig::load_from_root(opts.target_root);
    if (!cfg_res) {
        sage::util::log_error("Failed to load configuration: {}", cfg_res.error());
        return 1;
    }
    const auto& cfg = *cfg_res;
    const bool full = std::ranges::find(opts.args, "--full") != opts.args.end();

    print_banner();
    std::println("");
    std::println("Target Root:   {}", opts.target_root.string());
    std::println("Config Dir:    {}", cfg.config_dir.string());
    std::println("Database:      {}{}", cfg.db_path.string(),
        std::filesystem::exists(cfg.db_path) ? "" : "  (not initialized)");
    std::println("Schema:        v{}", cfg.schema_version);

    std::println("");
    std::println("Active Providers:");
    if (cfg.providers.empty()) {
        std::println("  (none declared)");
    } else {
        for (const auto& [interface_name, provider] : cfg.providers) {
            std::println("  • {:<16} {}", interface_name, provider);
        }
    }

    std::println("");
    std::println("Channels ({}):", cfg.channels.size());
    for (const auto& ch : cfg.channels) {
        std::println("  • {:<15} scope: {:<10} priority: {:<5} {}",
            ch.name, ch.scope, ch.priority, ch.enabled ? "enabled" : "disabled");
    }

    std::println("");
    auto db_res = sage::db::Database::open(cfg.db_path, true);
    if (!db_res) {
        std::println("Installed Packages: (database unavailable)");
        return 0;
    }
    auto installed = db_res->list_installed_packages();
    if (!installed) {
        sage::util::log_error("Installed package database is inconsistent: {}", installed.error());
        return 1;
    }
    std::println("Installed Packages: {}", installed->size());
    if (full) {
        for (const auto& pkg : *installed) {
            std::println("  • {:<20} {:<15} [{}]", pkg.name, pkg.version.to_string(), pkg.channel);
        }
    }
    return 0;
}

int cmd_rebuild(const CliOptions& opts) {
    auto cfg_res = sage::config::SystemConfig::load_from_root(opts.target_root);
    if (!cfg_res) {
        sage::util::log_error("Failed to load configuration: {}", cfg_res.error());
        return 1;
    }

    auto db_res = sage::db::Database::open(cfg_res->db_path);
    if (!db_res) {
        sage::util::log_error("Failed to open database: {}", db_res.error());
        return 1;
    }

    std::vector<sage::package::PackageManifest> pool;
    auto plan_res = sage::rebuild::ReconcileEngine::calculate_diff(*db_res, *cfg_res, pool);
    if (!plan_res) {
        sage::util::log_error("Failed to calculate reconcile plan: {}", plan_res.error());
        return 1;
    }

    auto exec_res = sage::rebuild::ReconcileEngine::execute(*db_res, *plan_res, opts.target_root, opts.dry_run, cfg_res->providers);
    if (!exec_res) {
        sage::util::log_error("Reconcile execution failed: {}", exec_res.error());
        return 1;
    }
    return 0;
}

int cmd_repo(const CliOptions& opts) {
    if (opts.args.empty() || opts.args[0] != "index" || opts.args.size() < 2) {
        std::println("Usage: sage repo index <REPO_DIR> [CHANNEL_NAME]");
        return 1;
    }
    std::filesystem::path repo_dir = opts.args[1];
    std::string ch_name = (opts.args.size() >= 3) ? opts.args[2] : "core";
    sage::util::log_info("Generating index.toml for local repository at {}...", repo_dir.string());
    auto res = sage::archive::generate_repo_index(repo_dir, ch_name);
    if (!res) {
        sage::util::log_error("Failed to generate repository index: {}", res.error());
        return 1;
    }
    sage::util::log_success("Repository index successfully created: {}", (repo_dir / "index.toml").string());
    return 0;
}

} // anonymous namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        print_help();
        return 0;
    }

    CliOptions opts;
    for (int i = 1; i < argc; ++i) {
        std::string_view arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            print_help();
            return 0;
        }
        if (arg == "--version" || arg == "-V") {
            print_banner();
            return 0;
        }
        if (arg == "--dry-run") {
            opts.dry_run = true;
        } else if (arg == "--verbose" || arg == "-v") {
            opts.verbose = true;
        } else if (arg == "--force" || arg == "-f" || arg == "--nodeps" || arg == "-d") {
            opts.force = true;
        } else if (arg == "--cascade" || arg == "-c") {
            opts.cascade = true;
        } else if (arg == "--no-recursive") {
            opts.no_recursive = true;
        } else if (arg == "--no-elf-check") {
            opts.no_elf_check = true;
        } else if (arg == "--channel" && i + 1 < argc) {
            opts.channel_filter = argv[++i];
        } else if ((arg == "--root" || arg == "--sysroot") && i + 1 < argc) {
            opts.target_root = argv[++i];
        } else if (opts.command.empty()) {
            opts.command = std::string(arg);
        } else {
            opts.args.push_back(std::string(arg));
        }
    }

    if (opts.command == "test-suite" || opts.command == "test") {
        return cmd_test_suite();
    }
    if (opts.command == "install") {
        return cmd_install(opts);
    }
    if (opts.command == "remove") {
        return cmd_remove(opts);
    }
    if (opts.command == "build") {
        return cmd_build(opts);
    }
    if (opts.command == "rebuild") {
        return cmd_rebuild(opts);
    }
    if (opts.command == "status") {
        return cmd_status(opts);
    }
    if (opts.command == "query") {
        return cmd_query(opts);
    }
    if (opts.command == "list") {
        return cmd_list(opts);
    }
    if (opts.command == "count") {
        return cmd_count(opts);
    }
    if (opts.command == "toolchain") {
        return cmd_toolchain(opts);
    }
    if (opts.command == "java") {
        return cmd_java(opts);
    }
    if (opts.command == "rust") {
        return cmd_rust(opts);
    }
    if (opts.command == "shell") {
        return cmd_shell(opts);
    }
    if (opts.command == "service") {
        return cmd_service(opts);
    }
    if (opts.command == "channel") {
        return cmd_channel(opts);
    }
    if (opts.command == "repo") {
        return cmd_repo(opts);
    }
    if (opts.command == "verify") {
        return cmd_verify(opts);
    }

    std::println(std::cerr, "Unknown command: '{}'", opts.command);
    print_help();
    return 1;
}
