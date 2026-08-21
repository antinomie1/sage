export module sage.rebuild;

import std;
import sage.config;
import sage.package;
import sage.service;
import sage.db;
import sage.solver;
import sage.util;

export namespace sage::rebuild {

using std::size_t;

struct ProviderSwap {
    std::string iface;
    std::string current_provider;
    std::string target_provider;
};

struct PlannedPackageRemoval {
    std::string name;
    std::optional<package::PackageIdentity> expected_identity;
};

struct ReconcilePlan {
    std::vector<ProviderSwap> swaps;
    std::vector<package::PackageManifest> packages_to_install;
    std::vector<PlannedPackageRemoval> packages_to_remove;
    service::InitType target_init{service::InitType::OpenRC};
    bool has_changes{false};
};

// ============================================================================
// Post-transaction Triggers
// ============================================================================
//
// A transaction reports what it touched; triggers decide what has to be
// regenerated. Two things can fire one:
//
//   on_paths      a file under the prefix was written or removed
//   on_capability a package taking part in the transaction provides it
//
// The capability form is what makes the kernel case work. Installing a kernel
// must rebuild the initramfs -- but *which* tool does that is the admin's
// choice, so the trigger names the capability virtual/initramfs-generator and
// sage resolves it at fire time through the active provider's
// [[capability_hooks]] entry. A recipe never writes "mkinitcpio -P".

struct TriggerContext {
    std::filesystem::path sysroot{"/"};
    // Files added, replaced or deleted by this transaction.
    std::vector<package::FileEntry> touched_files;
    // Packages installed or upgraded by this transaction.
    std::vector<package::PackageManifest> transaction_packages;
    // Every package present after the transaction. Both the source of declared
    // triggers and the pool capability hooks are resolved against.
    std::vector<package::PackageManifest> installed_packages;
    // Capability -> provider bindings from system.toml.
    std::map<std::string, std::string> providers;
    bool dry_run{false};
};

class TriggerEngine {
public:
    // Built-in triggers. These are the ones that cannot be package-declared
    // without a bootstrap problem -- ldconfig has to run before the package
    // that would have declared it is usable -- plus the two capability-driven
    // ones, which are kept here so a kernel package that forgot to declare
    // them still gets a working boot.
    static std::vector<package::Trigger> builtin_triggers() {
        std::vector<package::Trigger> t;

        t.push_back(package::Trigger{
            .name = "ldconfig",
            .on_paths = {"usr/lib/", "lib/"},
            .exec = "/sbin/ldconfig",
            .priority = 10,
        });
        t.push_back(package::Trigger{
            .name = "ca-certificates",
            .on_paths = {"etc/ssl/certs/", "usr/share/ca-certificates/"},
            .exec = "/usr/sbin/update-ca-certificates",
            .priority = 20,
        });
        t.push_back(package::Trigger{
            .name = "mime-database",
            .on_paths = {"usr/share/mime/"},
            .exec = "/usr/bin/update-mime-database",
            .args = {"/usr/share/mime"},
            .priority = 20,
        });
        // Kernel installed -> rebuild the initramfs, then point the bootloader
        // at it. Both resolve through whichever package currently provides the
        // capability; if nothing does, they silently do not fire.
        t.push_back(package::Trigger{
            .name = "initramfs",
            .on_paths = {"usr/lib/modules/", "boot/vmlinuz"},
            .on_capability = {"virtual/kernel"},
            .run_capability = "virtual/initramfs-generator",
            .priority = 60,
        });
        t.push_back(package::Trigger{
            .name = "bootloader",
            .on_paths = {"boot/vmlinuz"},
            .on_capability = {"virtual/kernel"},
            .run_capability = "virtual/bootloader",
            .priority = 70,
        });

        return t;
    }

    static void run(const TriggerContext& ctx) {
        // Capabilities brought in by this transaction, for on_capability.
        std::set<std::string> txn_capabilities;
        for (const auto& pkg : ctx.transaction_packages) {
            for (const auto& prov : pkg.provides) {
                txn_capabilities.insert(prov);
            }
        }

        std::vector<package::Trigger> candidates = builtin_triggers();
        for (const auto& pkg : ctx.installed_packages) {
            candidates.insert(candidates.end(), pkg.triggers.begin(), pkg.triggers.end());
        }

        std::ranges::stable_sort(candidates, {}, &package::Trigger::priority);

        // One resolved command runs at most once per transaction, however many
        // triggers and however many touched files ask for it.
        std::set<std::string> already_run;

        for (const auto& trig : candidates) {
            if (!fires(trig, ctx, txn_capabilities)) continue;

            auto cmd = resolve_command(trig, ctx);
            if (!cmd) continue;

            if (!already_run.insert(*cmd).second) continue;
            execute(*cmd, trig.name, ctx);
        }
    }

private:
    static bool fires(const package::Trigger& trig,
                      const TriggerContext& ctx,
                      const std::set<std::string>& txn_capabilities)
    {
        for (const auto& cap : trig.on_capability) {
            if (txn_capabilities.contains(cap)) return true;
        }
        for (const auto& f : ctx.touched_files) {
            if (!trig.matches_path(f.path)) continue;
            // The historical ldconfig rule only cared about shared objects,
            // not about every file that happens to live under usr/lib.
            if (trig.name == "ldconfig" && f.path.find(".so") == std::string::npos) continue;
            return true;
        }
        return false;
    }

    // Resolve a trigger to a concrete command line inside the target root, or
    // nothing when the capability has no provider / no hook.
    static std::optional<std::string> resolve_command(const package::Trigger& trig,
                                                      const TriggerContext& ctx)
    {
        if (trig.run_capability.empty()) {
            if (trig.exec.empty()) return std::nullopt;
            std::string cmd = trig.exec;
            for (const auto& a : trig.args) cmd += " " + a;
            return cmd;
        }

        const package::CapabilityHook* hook = nullptr;

        // The admin's binding wins, whether the capability is exclusive or
        // just has a declared default.
        if (auto it = ctx.providers.find(trig.run_capability); it != ctx.providers.end()) {
            for (const auto& pkg : ctx.installed_packages) {
                if (pkg.name != it->second) continue;
                hook = pkg.hook_for(trig.run_capability);
                break;
            }
        }

        // Otherwise: any installed provider that publishes a hook.
        if (!hook) {
            for (const auto& pkg : ctx.installed_packages) {
                if (!pkg.provides_capability(trig.run_capability)) continue;
                if (auto* h = pkg.hook_for(trig.run_capability)) {
                    hook = h;
                    break;
                }
            }
        }

        if (!hook) {
            // A capability nobody provides is the ordinary case -- most roots
            // never install a bootloader -- and warning about it on every
            // transaction trains people to ignore trigger warnings. Only a
            // provider that is installed but ships no hook is worth reporting:
            // that one is a packaging mistake.
            bool provided = std::ranges::any_of(ctx.installed_packages,
                [&](const auto& pkg) { return pkg.provides_capability(trig.run_capability); });
            if (provided) {
                util::log_warn("Trigger '{}' wants capability '{}': it is provided, but no provider declares a capability hook -- skipping",
                    trig.name, trig.run_capability);
            }
            return std::nullopt;
        }

        std::string cmd = hook->command_line();
        for (const auto& a : trig.args) cmd += " " + a;
        return cmd;
    }

    // Post-transaction tools are provided BY the target and resolve their own
    // paths against "/". For a sysroot other than "/" they must run inside the
    // chroot: host-side they would rebuild the HOST's caches and leave the
    // sysroot's untouched, and the host loader may not even match the target's
    // glibc. Every path here is therefore relative to the target root.
    static void execute(const std::string& cmd, std::string_view trigger_name, const TriggerContext& ctx) {
        std::string exec_path = cmd.substr(0, cmd.find(' '));
        if (!std::filesystem::exists(ctx.sysroot / std::filesystem::path(exec_path).relative_path())) {
            return;
        }

        std::string full = (ctx.sysroot == "/")
            ? cmd
            : std::format("chroot {} {}", ctx.sysroot.string(), cmd);

        if (ctx.dry_run) {
            util::log_info("Would run trigger '{}': {}", trigger_name, full);
            return;
        }

        util::log_info("Running trigger '{}': {}", trigger_name, full);
        int ret = std::system(full.c_str());
        if (ret != 0) {
            util::log_warn("Trigger '{}' failed (exit {}): {}", trigger_name, ret, full);
        }
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
        if (!current_providers) {
            return std::unexpected(
                "Failed to read current system providers: " + current_providers.error());
        }

        // 1. Calculate provider diffs.
        //
        // Only *exclusive* capabilities take part: they are the ones where at
        // most one provider may exist, so a changed binding means packages
        // must actually be swapped. Retargeting a shared default such as
        // virtual/initramfs-generator changes which tool later transactions
        // call, not what is installed -- reconciling on it would uninstall a
        // perfectly valid coexisting provider.
        for (const auto& [iface, target_prov] : desired_config.exclusive_providers()) {
            std::string cur = current_providers->contains(iface) ? current_providers->at(iface) : "";
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
                auto current_package = db.get_package(swap.current_provider);
                if (!current_package) {
                    return std::unexpected(std::format(
                        "Failed to read current provider package '{}': {}",
                        swap.current_provider, current_package.error()));
                }
                plan.packages_to_remove.push_back(PlannedPackageRemoval{
                    .name = swap.current_provider,
                    .expected_identity = *current_package
                        ? std::optional{package::package_identity(**current_package)}
                        : std::nullopt,
                });
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
        bool dry_run = false,
        const std::map<std::string, std::string>& providers = {})
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

        // The plan was computed before taking the writer lock. Validate every
        // provider binding before changing any of them so a stale reconcile
        // cannot overwrite a concurrently committed provider choice.
        for (const auto& swap : plan.swaps) {
            auto current = db.get_system_provider(*wtxn, swap.iface);
            if (!current) {
                return std::unexpected(std::format(
                    "Failed to revalidate provider '{}': {}", swap.iface, current.error()));
            }
            const std::string current_name = *current ? **current : std::string{};
            if (current_name != swap.current_provider) {
                return std::unexpected(std::format(
                    "System provider '{}' changed after the reconcile plan was created",
                    swap.iface));
            }
        }

        // 1. Update system provider locks in LMDB
        for (const auto& swap : plan.swaps) {
            auto set_res = db.set_system_provider(*wtxn, swap.iface, swap.target_provider);
            if (!set_res) return std::unexpected(set_res.error());
        }

        // 2. Unregister and remove obsolete packages
        for (const auto& removal : plan.packages_to_remove) {
            auto old_pkg = db.get_package(*wtxn, removal.name);
            if (!old_pkg) {
                return std::unexpected(std::format(
                    "Failed to read package '{}' in reconcile transaction: {}",
                    removal.name, old_pkg.error()));
            }
            auto current_identity = *old_pkg
                ? std::optional{package::package_identity(**old_pkg)}
                : std::nullopt;
            if (current_identity != removal.expected_identity) {
                return std::unexpected(std::format(
                    "Installed package '{}' changed after the reconcile plan was created",
                    removal.name));
            }
            if (*old_pkg) {
                auto file_res = db.unregister_files(*wtxn, (**old_pkg).files);
                if (!file_res) return std::unexpected(file_res.error());
                auto provide_res = db.unregister_provides(*wtxn, (**old_pkg).provides);
                if (!provide_res) return std::unexpected(provide_res.error());
                auto delete_res = db.del_package(*wtxn, removal.name);
                if (!delete_res) return std::unexpected(delete_res.error());
                // Remove legacy service scripts
                service::remove_service(removal.name, service::InitType::OpenRC, sysroot);
                service::remove_service(removal.name, service::InitType::Systemd, sysroot);
                service::remove_service(removal.name, service::InitType::Runit, sysroot);
                service::remove_service(removal.name, service::InitType::Dinit, sysroot);
                service::remove_service(removal.name, service::InitType::S6, sysroot);
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
        if (!installed) {
            return std::unexpected("Installed package database is inconsistent after reconcile: " + installed.error());
        }
        size_t gen_count = 0;
        for (const auto& pkg : *installed) {
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

        // 5. Execute post-transaction triggers
        TriggerContext trig_ctx;
        trig_ctx.sysroot = sysroot;
        trig_ctx.transaction_packages = plan.packages_to_install;
        trig_ctx.installed_packages = std::move(*installed);
        trig_ctx.providers = providers;
        for (const auto& pkg : plan.packages_to_install) {
            trig_ctx.touched_files.insert(trig_ctx.touched_files.end(), pkg.files.begin(), pkg.files.end());
        }
        TriggerEngine::run(trig_ctx);

        util::log_success("Reconcile completed! Regenerated {} native service scripts for {}", 
            gen_count, service::to_string(plan.target_init));
        return {};
    }
};

} // namespace sage::rebuild
