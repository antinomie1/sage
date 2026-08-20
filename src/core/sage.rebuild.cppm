export module sage.rebuild;

import std;
import sage.config;
import sage.package;
import sage.service;
import sage.db;
import sage.archive;
import sage.solver;
import sage.util;

export namespace sage::rebuild {

using std::size_t;

struct ProviderSwap {
    std::string iface;
    std::string current_provider;
    std::string target_provider;
};

struct ReconcilePlan {
    std::vector<ProviderSwap> swaps;
    std::vector<package::PackageManifest> packages_to_install;
    std::vector<std::string> packages_to_remove;
    service::InitType target_init{service::InitType::OpenRC};
    bool has_changes{false};
};

class TriggerEngine {
public:
    static void run_post_transaction_triggers(
        const std::filesystem::path& sysroot, 
        const std::vector<package::FileEntry>& touched_files) 
    {
        bool run_ldconfig = false;
        bool run_ca = false;
        bool run_mime = false;
        bool run_initramfs = false;

        for (const auto& f : touched_files) {
            if (f.path.starts_with("usr/lib/") || f.path.starts_with("lib/")) {
                if (f.path.find(".so") != std::string::npos) run_ldconfig = true;
            }
            if (f.path.starts_with("usr/lib/modules/") || f.path.starts_with("boot/vmlinuz")) {
                run_initramfs = true;
            }
            if (f.path.starts_with("etc/ssl/certs/") || f.path.starts_with("usr/share/ca-certificates/")) {
                run_ca = true;
            }
            if (f.path.starts_with("usr/share/mime/")) {
                run_mime = true;
            }
        }

        // Post-transaction tools are provided BY the target, and resolve their
        // own paths against "/". For a sysroot other than "/" they must run
        // inside the chroot: host-side they would rebuild the HOST's caches and
        // leave the sysroot's untouched, and the host loader may not even match
        // the target's glibc. `abs_path` and `args` are therefore always
        // expressed relative to the target root.
        auto run_target_tool = [&](std::string_view abs_path, std::string_view args = "") {
            if (!std::filesystem::exists(sysroot / std::filesystem::path(abs_path).relative_path())) {
                return;
            }
            std::string suffix = args.empty() ? std::string{} : std::format(" {}", args);
            std::string cmd = (sysroot == "/")
                ? std::format("{}{}", abs_path, suffix)
                : std::format("chroot {} {}{}", sysroot.string(), abs_path, suffix);

            util::log_info("Running post-transaction trigger: {}", abs_path);
            int ret = std::system(cmd.c_str());
            if (ret != 0) {
                util::log_warn("Trigger failed (exit {}): {}", ret, cmd);
            }
        };

        if (run_ldconfig)  run_target_tool("/sbin/ldconfig");
        if (run_initramfs) run_target_tool("/usr/bin/sage-initramfs-generate");
        if (run_ca)        run_target_tool("/usr/sbin/update-ca-certificates");
        if (run_mime)      run_target_tool("/usr/bin/update-mime-database", "/usr/share/mime");
    }
};

class ReconcileEngine {
public:
    static std::expected<ReconcilePlan, std::string> calculate_diff(
        db::Database& db,
        const config::SystemConfig& desired_config,
        const std::vector<package::PackageManifest>& repo_pool) 
    {
        ReconcilePlan plan;
        auto current_providers = db.get_all_system_providers();

        // 1. Calculate provider diffs (virtual/init, virtual/udev, virtual/libc)
        for (const auto& [iface, target_prov] : desired_config.providers) {
            std::string cur = current_providers.contains(iface) ? current_providers.at(iface) : "";
            if (cur != target_prov) {
                plan.swaps.push_back(ProviderSwap{
                    .iface = iface,
                    .current_provider = cur,
                    .target_provider = target_prov
                });
                plan.has_changes = true;
            }
        }

        // Determine active init system
        std::string init_prov = desired_config.providers.contains("virtual/init") ? 
                                desired_config.providers.at("virtual/init") : "openrc";
        plan.target_init = service::parse_init_type(init_prov);

        if (!plan.has_changes) {
            return plan;
        }

        // 2. Solve dependencies for new providers
        std::vector<package::Dependency> root_reqs;
        for (const auto& swap : plan.swaps) {
            root_reqs.push_back(package::Dependency{
                .name = swap.target_provider,
                .op = package::ConstraintOp::Any,
                .version = {}
            });
            if (!swap.current_provider.empty() && swap.current_provider != swap.target_provider) {
                plan.packages_to_remove.push_back(swap.current_provider);
            }
        }

        solver::DependencySolver solver(repo_pool, desired_config.providers);
        auto solve_res = solver.solve(root_reqs);
        if (!solve_res) {
            return std::unexpected("Reconcile dependency resolution failed: " + solve_res.error());
        }

        plan.packages_to_install = *solve_res;
        return plan;
    }

    static std::expected<void, std::string> execute(
        db::Database& db,
        const ReconcilePlan& plan,
        const std::filesystem::path& sysroot = "/",
        bool dry_run = false) 
    {
        if (!plan.has_changes) {
            util::log_info("System state matches desired configuration. No reconcile needed.");
            return {};
        }

        util::log_info("Executing Declarative System Reconcile (Target Init: {})...", service::to_string(plan.target_init));

        for (const auto& swap : plan.swaps) {
            util::log_info("  • Swapping interface '{}': [{}] -> [{}]", 
                swap.iface, 
                swap.current_provider.empty() ? "none" : swap.current_provider, 
                swap.target_provider);
        }

        if (dry_run) {
            util::log_info("Dry-run preview completed successfully (no changes applied).");
            return {};
        }

        auto wtxn = db.begin_write_txn();
        if (!wtxn) return std::unexpected("Failed to open database write transaction");

        // 1. Update system provider locks in LMDB
        for (const auto& swap : plan.swaps) {
            auto set_res = db.set_system_provider(*wtxn, swap.iface, swap.target_provider);
            if (!set_res) return std::unexpected(set_res.error());
        }

        // 2. Unregister and remove obsolete packages
        for (const auto& pkg_name : plan.packages_to_remove) {
            if (auto old_pkg = db.get_package(pkg_name)) {
                (void)db.unregister_files(*wtxn, old_pkg->files);
                (void)db.unregister_provides(*wtxn, old_pkg->provides);
                (void)db.del_package(*wtxn, pkg_name);
                // Remove legacy service scripts
                service::remove_service(pkg_name, service::InitType::OpenRC, sysroot);
                service::remove_service(pkg_name, service::InitType::Systemd, sysroot);
                service::remove_service(pkg_name, service::InitType::Runit, sysroot);
                service::remove_service(pkg_name, service::InitType::Dinit, sysroot);
                service::remove_service(pkg_name, service::InitType::S6, sysroot);
            }
        }

        // 3. Register newly installed packages in LMDB
        for (const auto& new_pkg : plan.packages_to_install) {
            auto p_res = db.put_package(*wtxn, new_pkg);
            if (!p_res) return p_res;
            auto f_res = db.register_files(*wtxn, new_pkg.name, new_pkg.channel, new_pkg.files);
            if (!f_res) return f_res;
            auto prov_res = db.register_provides(*wtxn, new_pkg.name, new_pkg.provides);
            if (!prov_res) return prov_res;
        }

        auto commit_res = wtxn->commit();
        if (!commit_res) return std::unexpected("Database commit failed: " + commit_res.error());

        // 4. Automatically re-generate native service configurations for ALL installed daemons
        auto installed = db.list_installed_packages();
        size_t gen_count = 0;
        for (const auto& pkg : installed) {
            // Check if package has service definition in database or package files
            service::ServiceSpec spec;
            spec.name = pkg.name;
            spec.description = pkg.description;
            spec.exec_start = "/usr/bin/" + pkg.name; // default convention
            auto gen_res = service::generate_service(spec, plan.target_init, sysroot);
            if (gen_res) {
                gen_count++;
            }
        }

        // 5. Execute post-transaction file triggers (ldconfig, ca-certificates, mime)
        std::vector<package::FileEntry> touched_files;
        for (const auto& pkg : plan.packages_to_install) {
            touched_files.insert(touched_files.end(), pkg.files.begin(), pkg.files.end());
        }
        TriggerEngine::run_post_transaction_triggers(sysroot, touched_files);

        util::log_success("Reconcile completed! Regenerated {} native service scripts for {}", 
            gen_count, service::to_string(plan.target_init));
        return {};
    }
};

} // namespace sage::rebuild
