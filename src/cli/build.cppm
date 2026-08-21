export module sage.cli.build;

// Package authoring: recipe builds and local repository indexing.
import std;
import sage;

import sage.cli;

namespace sage::cli {

export int cmd_build(const CliOptions& opts) {
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
export int cmd_repo(const CliOptions& opts) {
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
} // namespace sage::cli
