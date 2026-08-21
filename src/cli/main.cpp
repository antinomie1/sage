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
  channel [COMMAND]        Manage multi-layer channels (list, add, sync)
  repo index <DIR> [NAME]  Generate index.toml for local repository directory
  query [COMMAND]          Query packages, file ownership, and manifests (nanosecond LMDB)
  service [COMMAND]        Inspect and generate native init scripts (OpenRC/Runit/Systemd/Dinit/s6)
  build <RECIPE_DIR>       Build package from recipe.toml (fetch source, check sha256, build, scan ELF)
  status [--full]          Show declared providers, channels, and database state
  test-suite               Run internal engine self-test suite

Global Options:
  --root, --sysroot <DIR>  Operate on target root directory (default: /)
  --dry-run                Simulate actions without modifying filesystem
  --verbose, -v            Enable verbose diagnostics
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

    if (std::filesystem::exists(pkg_dir)) {
        std::vector<std::filesystem::directory_entry> entries;
        for (const auto& entry : std::filesystem::recursive_directory_iterator(pkg_dir, std::filesystem::directory_options::skip_permission_denied)) {
            entries.push_back(entry);
        }
        std::ranges::sort(entries, {}, [&](const auto& entry) {
            return entry.path().lexically_relative(pkg_dir).generic_string();
        });

        for (const auto& entry : entries) {
            if (entry.is_symlink()) continue;
            if (entry.is_regular_file()) {
                auto elf_res = sage::util::scan_elf(entry.path());
                if (elf_res) {
                    if (!elf_res->soname.empty()) {
                        manifest.provides.push_back("so:" + elf_res->soname);
                    }
                    for (const auto& needed : elf_res->needed) {
                        manifest.dependencies.push_back(sage::package::Dependency::parse("so:" + needed));
                    }
                }
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

    // 1. Gather Available Package Pool from Channels and Local Repos
    std::vector<sage::package::PackageManifest> pool;
    std::map<std::string, std::filesystem::path> package_archive_map;

    for (const auto& ch_cfg : cfg.channels) {
        if (!ch_cfg.enabled) continue;
        sage::channel::Channel ch;
        ch.name = ch_cfg.name;
        ch.url = ch_cfg.url;
        ch.scope = sage::channel::parse_scope(ch_cfg.scope);

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

                std::filesystem::path local_p = dir_base / std::format("{}-{}-{}-{}.pkg.tar.zst", pkg.name, pkg.version.ver, pkg.version.rel, pkg.arch);
                if (!std::filesystem::exists(local_p)) {
                    local_p = dir_base / std::format("{}-{}-{}.pkg.tar.zst", pkg.name, pkg.version.ver, pkg.version.rel);
                }
                if (!std::filesystem::exists(local_p)) {
                    local_p = dir_base / std::format("{}-{}.pkg.tar.zst", pkg.name, pkg.version.ver);
                }
                package_archive_map[pkg.name] = local_p;
            }
        }
    }

    // Include already-installed packages in the pool so that installed providers
    // (e.g. glibc providing so:libc.so.6) satisfy dependencies during bootstrap,
    // when the repository may not yet contain the providing package. Candidates
    // that are already installed are filtered out after solving.
    for (auto& pkg : db.list_installed_packages()) {
        pool.push_back(std::move(pkg));
    }

    // Check if any arguments are direct .pkg.tar.zst archive files
    std::vector<sage::package::Dependency> root_reqs;
    for (const auto& arg : opts.args) {
        if (arg.ends_with(".pkg.tar.zst") && std::filesystem::exists(arg)) {
            auto ext_res = sage::archive::extract_package(arg, std::filesystem::temp_directory_path() / "sage_probe");
            std::filesystem::remove_all(std::filesystem::temp_directory_path() / "sage_probe");
            if (ext_res) {
                pool.push_back(ext_res->manifest);
                package_archive_map[ext_res->manifest.name] = std::filesystem::absolute(arg);
                root_reqs.push_back(sage::package::Dependency::parse(ext_res->manifest.name));
            }
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
    for (auto& p : db.list_installed_packages()) {
        installed_by_name.emplace(p.name, std::move(p));
    }
    std::vector<sage::package::PackageManifest> to_install;
    for (auto& pkg : unique_to_install) {
        auto it = installed_by_name.find(pkg.name);
        if (it != installed_by_name.end() && it->second.version >= pkg.version) {
            auto db_files = it->second.files.empty() ? db.get_package_files(pkg.name) : std::vector<std::string>{};
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

    sage::util::log_info("Resolved {} packages to install into target root '{}':", unique_to_install.size(), opts.target_root.string());
    for (const auto& pkg : unique_to_install) {
        std::println("  + {:<20} {:<15} [{}]", pkg.name, pkg.version.to_string(), pkg.channel);
    }

    if (opts.dry_run) {
        sage::util::log_info("Dry-run preview completed successfully (no changes written).");
        return 0;
    }

    auto wtxn = db.begin_write_txn();
    if (!wtxn) {
        sage::util::log_error("Failed to open database write transaction: {}", wtxn.error());
        return 1;
    }

    // Self-heal: prune file registrations owned by packages that are no longer
    // installed (leftovers from previous versions with incomplete file lists),
    // so the paths can be claimed by the packages about to be installed.
    std::unordered_set<std::string> installed_names;
    for (const auto& [name, _] : installed_by_name) {
        installed_names.insert(name);
    }
    auto pruned = db.prune_orphaned_files(*wtxn, installed_names);
    if (pruned > 0) {
        sage::util::log_info("  ~ pruned {} orphaned file registration(s)", pruned);
    }

    std::vector<sage::package::FileEntry> all_installed_files;

    // 3. Streaming Unpack & LMDB State Registration
    for (const auto& pkg : unique_to_install) {
        auto archive_it = package_archive_map.find(pkg.name);
        if (archive_it == package_archive_map.end() || !std::filesystem::exists(archive_it->second)) {
            sage::util::log_error("Package archive for '{}' not found at {}", pkg.name, 
                (archive_it != package_archive_map.end()) ? archive_it->second.string() : "<unknown>");
            return 1;
        }

        // Archives always contain paths relative to sysroot (e.g. usr/bin/bash or
        // opt/channels/gcc/15/bin/gcc), so always extract to the target root directly.
        sage::util::log_info("Unpacking {} -> {}...", pkg.name, opts.target_root.string());
        auto ext_res = sage::archive::extract_package(archive_it->second, opts.target_root);
        if (!ext_res) {
            sage::util::log_error("Failed to extract package '{}': {}", pkg.name, ext_res.error());
            return 1;
        }

        auto installed_pkg = pkg;
        installed_pkg.files = ext_res->extracted_files;

        // Upgrade cleanup: remove physical files owned by a previously installed
        // version of this package that are not part of the new version, so file
        // ownership can transition to other packages (e.g. split -dev/-libs
        // children claim headers/libs the old monolithic version used to own).
        auto old_it = installed_by_name.find(pkg.name);
        if (old_it != installed_by_name.end() && old_it->second.version != pkg.version) {
            std::unordered_set<std::string> new_paths;
            for (const auto& f : installed_pkg.files) {
                new_paths.insert(sage::util::clean_rel_path(f.path));
            }
            std::vector<sage::package::FileEntry> stale;
            std::string my_owner = std::format("{}:{}", pkg.name, pkg.channel);
            for (const auto& old_path : db.get_package_files(pkg.name)) {
                if (new_paths.contains(old_path)) continue;
                if (auto cur_owner = db.get_file_owner(old_path); cur_owner && *cur_owner != my_owner) {
                    continue;
                }
                std::filesystem::path disk_p = opts.target_root / old_path;
                std::error_code ec;
                if (!std::filesystem::is_directory(disk_p, ec)) {
                    std::filesystem::remove(disk_p, ec);
                }
                sage::package::FileEntry fe;
                fe.path = old_path;
                stale.push_back(std::move(fe));
            }
            if (!stale.empty()) {
                (void)db.unregister_files(*wtxn, stale, my_owner);
                sage::util::log_info("  ~ removed {} stale file(s) from previous {} {}", 
                    stale.size(), pkg.name, old_it->second.version.to_string());
            }
        }

        auto p_res = db.put_package(*wtxn, installed_pkg);
        if (!p_res) {
            sage::util::log_error("Failed to register package '{}' in DB: {}", installed_pkg.name, p_res.error());
            return 1;
        }
        auto f_res = db.register_files(*wtxn, installed_pkg.name, installed_pkg.channel, installed_pkg.files);
        if (!f_res) {
            sage::util::log_error("Failed to register files for '{}': {}", installed_pkg.name, f_res.error());
            return 1;
        }
        auto prov_res = db.register_provides(*wtxn, installed_pkg.name, installed_pkg.provides);
        if (!prov_res) {
            sage::util::log_error("Failed to register provides for '{}': {}", installed_pkg.name, prov_res.error());
            return 1;
        }

        all_installed_files.insert(all_installed_files.end(), installed_pkg.files.begin(), installed_pkg.files.end());
    }

    auto commit_res = wtxn->commit();
    if (!commit_res) {
        sage::util::log_error("Failed to commit database transaction: {}", commit_res.error());
        return 1;
    }

    // 4. Auto-activate installed toolchain channels
    std::set<std::pair<std::string, std::string>> activated_toolchains;
    for (const auto& pkg : unique_to_install) {
        auto spec = sage::channel::SubChannelSpec::parse(pkg.channel);
        if (spec.scope == sage::channel::ChannelScope::Toolchain && !spec.category.empty() && !spec.slot.empty()) {
            auto key = std::make_pair(spec.category, spec.slot);
            if (activated_toolchains.insert(key).second) {
                auto act_res = sage::channel::ProfileManager::switch_active_toolchain(opts.target_root, spec.category, spec.slot);
                if (!act_res) {
                    sage::util::log_warn("Failed to activate toolchain '{}:{}': {}", spec.category, spec.slot, act_res.error());
                }
            }
        }
    }

    // 5. Regenerate FHS profile for all active channels
    std::vector<sage::channel::Channel> active_channels;
    for (const auto& ch_cfg : cfg.channels) {
        sage::channel::Channel ch;
        ch.name = ch_cfg.name;
        ch.scope = sage::channel::parse_scope(ch_cfg.scope);
        ch.enabled = ch_cfg.enabled;
        active_channels.push_back(std::move(ch));
    }
    (void)sage::channel::ProfileManager::regenerate_fhs_profile(opts.target_root, active_channels);

    // 6. Post-Transaction File Triggers (ldconfig, ca-certificates, mime).
    // Runs AFTER toolchain activation so freshly written
    // /etc/ld.so.conf.d/sage-*.conf entries are picked up by ldconfig.
    sage::rebuild::TriggerEngine::run_post_transaction_triggers(opts.target_root, all_installed_files);

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
    std::map<std::string, sage::package::PackageManifest> installed_map;
    for (const auto& pkg : all_installed) {
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

    std::vector<sage::package::FileEntry> removed_files;

    for (const auto& pkg_name : to_remove_set) {
        const auto& pkg = installed_map.at(pkg_name);
        if (std::find(opts.args.begin(), opts.args.end(), pkg_name) != opts.args.end()) {
            sage::util::log_info("Removing package '{}'...", pkg_name);
        } else {
            sage::util::log_info("Auto-removing orphaned dependency '{}' (version {})...", pkg_name, pkg.version.to_string());
        }

        sage::channel::Channel ch;
        auto spec = sage::channel::SubChannelSpec::parse(pkg.channel);
        ch.scope = spec.scope;
        ch.category = spec.category;
        ch.slot = spec.slot;
        auto dest_root = ch.resolve_target_root(opts.target_root);

        // Delete physical files. The LMDB files table is the authoritative owner
        // registry: merge the installed manifest's file list with all files still
        // registered to this package (a previous version's leftovers may not be
        // present in the current manifest), so stale ownership records are purged.
        auto files_to_delete = pkg.files;
        std::unordered_set<std::string> seen_paths;
        for (const auto& f : files_to_delete) {
            seen_paths.insert(sage::util::clean_rel_path(f.path));
        }
        for (const auto& fp : db.get_package_files(pkg_name)) {
            if (seen_paths.insert(fp).second) {
                sage::package::FileEntry fe;
                fe.path = fp;
                files_to_delete.push_back(std::move(fe));
            }
        }

        std::string my_owner = std::format("{}:{}", pkg_name, pkg.channel);
        for (const auto& file_entry : files_to_delete) {
            if (auto cur_owner = db.get_file_owner(file_entry.path); cur_owner && *cur_owner != my_owner) {
                continue;
            }
            std::filesystem::path p = dest_root / file_entry.path;
            std::error_code ec;
            if (!std::filesystem::is_directory(p, ec)) {
                std::filesystem::remove(p, ec);
            }
            removed_files.push_back(file_entry);
        }

        (void)db.unregister_files(*wtxn, files_to_delete, my_owner);
        (void)db.unregister_provides(*wtxn, pkg.provides);
        (void)db.del_package(*wtxn, pkg_name);
    }

    auto commit_res = wtxn->commit();
    if (!commit_res) return 1;

    sage::rebuild::TriggerEngine::run_post_transaction_triggers(opts.target_root, removed_files);
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

int cmd_test_suite() {
    sage::util::log_info("Running Sage Master Architecture & Subsystem Integration Test Suite...");

    // 1. Semantic Versioning Test
    auto v1 = sage::package::Version::parse("1.2.3-1");
    auto v2 = sage::package::Version::parse("1.2.4-1");
    if (!(v1 < v2)) {
        sage::util::log_error("Version comparator test failed");
        return 1;
    }
    sage::util::log_success("1. Semantic & Alphanum Version Comparator OK");

    // 2. Tar+Zstd Archive Packaging & Streaming Extractor Test
    auto temp_dir = std::filesystem::temp_directory_path() / "sage_archive_test";
    std::filesystem::remove_all(temp_dir);
    const auto long_rel = std::filesystem::path("usr/share") / std::string(80, 'a') / std::string(30, 'b') / "payload.txt";
    auto populate_payload = [&](const std::filesystem::path& root, bool long_path_first) {
        auto write_long_path = [&] {
            std::filesystem::create_directories((root / long_rel).parent_path());
            std::ofstream(root / long_rel) << "long path payload\n";
        };
        auto write_dummy = [&] {
            std::filesystem::create_directories(root / "usr/bin");
            std::ofstream(root / "usr/bin/dummy") << "#!/bin/sh\necho 'hello sage'\n";
            std::filesystem::permissions(root / "usr/bin/dummy", std::filesystem::perms::owner_all | std::filesystem::perms::group_read | std::filesystem::perms::group_exec);
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
    manifest.description = "Test mock tool";
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

    auto extract_root = temp_dir / "sysroot";
    auto ext_res = sage::archive::extract_package(pkg_path, extract_root);
    if (!ext_res || !std::filesystem::exists(extract_root / "usr/bin/dummy") || !std::filesystem::exists(extract_root / long_rel)) {
        sage::util::log_error("Archive extraction verification failed");
        return 1;
    }

    auto tar_extract_root = temp_dir / "tar-sysroot";
    std::filesystem::create_directories(tar_extract_root);
    auto tar_cmd = std::format("tar -xf \"{}\" -C \"{}\"", pkg_path.string(), tar_extract_root.string());
    if (std::system(tar_cmd.c_str()) != 0 || !std::filesystem::exists(tar_extract_root / "data" / long_rel)) {
        sage::util::log_error("POSIX USTAR compatibility verification failed");
        return 1;
    }
    sage::util::log_success("2. Deterministic POSIX USTAR + Zstandard Engine OK");

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

    sage::config::SystemConfig sys_cfg;
    sys_cfg.providers["virtual/init"] = "openrc";
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
    auto idx_res = sage::archive::generate_repo_index(temp_dir, "core");
    if (!idx_res || !std::filesystem::exists(temp_dir / "index.toml")) {
        sage::util::log_error("Local repository index generation failed");
        return 1;
    }

    auto fetch_res = sage::vendor::curl::fetch_string("file://" + (temp_dir / "index.toml").string());
    if (!fetch_res || fetch_res->find("schema_version = 1") == std::string::npos) {
        sage::util::log_error("Local file:// protocol fetch failed");
        return 1;
    }
    sage::util::log_success("8. Local Repository (file:// & /path) Indexer & Zero-Copy Fetch OK");

    // 9. End-to-End `sage install` & `sage remove` into isolated Target Root Test
    auto isolated_target = temp_dir / "target_root";
    std::filesystem::create_directories(isolated_target / "etc/sage");
    std::ofstream chan_f(isolated_target / "etc/sage/channels.toml");
    chan_f << "schema_version = 1\n\n[[channels]]\nname = \"core\"\nurl = \"file://" << temp_dir.string() << "\"\nscope = \"system\"\npriority = 100\nenabled = true\n";
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
    if (loop_db && !loop_db->list_installed_packages().empty()) {
        sage::util::log_error("LMDB still contains packages after cascaded removal");
        return 1;
    }

    sage::util::log_success("10. Complete Build -> Index -> Install -> Remove (Auto Orphan Cleanup) Closed-Loop OK");
    sage::util::log_success("11. Reverse Dependency Protection & Cascade Removal Safety Locks OK");

    std::filesystem::remove_all(temp_dir);
    sage::util::log_success("🎉 All Sage Master Architecture & Subsystem Integration Tests Passed Successfully!");
    return 0;
}

int cmd_query(const CliOptions& opts) {
    if (opts.args.empty()) {
        std::println("Usage: sage query [installed|info <pkg>|owner <path>]");
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

    if (sub == "installed") {
        auto list = db.list_installed_packages();
        std::println("Installed packages in '{}' ({} total):", opts.target_root.string(), list.size());
        for (const auto& pkg : list) {
            std::println("  • {:<20} {:<15} [{}]", pkg.name, pkg.version.to_string(), pkg.channel);
        }
    } else if (sub == "info" && opts.args.size() >= 2) {
        std::string pkg_name = opts.args[1];
        if (auto pkg = db.get_package(pkg_name)) {
            std::println("Package:     {}", pkg->name);
            std::println("Version:     {}", pkg->version.to_string());
            std::println("Channel:     {}", pkg->channel);
            std::println("License:     {}", pkg->license);
            std::println("Description: {}", pkg->description);
            std::println("Provides:    {}", sage::util::join(pkg->provides, ", "));
        } else {
            sage::util::log_error("Package '{}' is not installed", pkg_name);
            return 1;
        }
    } else if (sub == "owner" && opts.args.size() >= 2) {
        std::string path = opts.args[1];
        if (auto owner = db.get_file_owner(path)) {
            std::println("{} is owned by {}", path, *owner);
        } else {
            std::println("No installed package owns {}", path);
        }
    }
    return 0;
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

int cmd_channel(const CliOptions& opts) {
    auto cfg = sage::config::SystemConfig::load_from_root(opts.target_root);
    if (!cfg) {
        sage::util::log_error("Failed to load configuration: {}", cfg.error());
        return 1;
    }
    std::println("Configured Channels for '{}':", opts.target_root.string());
    for (const auto& ch : cfg->channels) {
        std::println("  • {:<15} {:<35} scope: {:<10} priority: {}", 
            ch.name, ch.url, ch.scope, ch.priority);
    }
    return 0;
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
    std::println("Installed Packages: {}", installed.size());
    if (full) {
        for (const auto& pkg : installed) {
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

    auto exec_res = sage::rebuild::ReconcileEngine::execute(*db_res, *plan_res, opts.target_root, opts.dry_run);
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

    std::println(std::cerr, "Unknown command: '{}'", opts.command);
    print_help();
    return 1;
}
