export module sage.cli;

// Shared CLI vocabulary: option state, banner/help output, argument parsing.
// Command groups live in sibling modules (sage.cli.pkg, sage.cli.query, ...)
// and the entry point in src/main.cpp dispatches between them.
import std;
import sage;

export namespace sage::cli {

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
    int wait_seconds{0};      // --wait[=SECONDS]: wait for a concurrent sage
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
  toolchain [list|use]     Manage multi-slot toolchains (e.g. use java:21, rust:nightly)
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

Global Options:
  --root, --sysroot <DIR>  Operate on target root directory (default: /)
  --dry-run                Simulate actions without modifying filesystem
  --verbose, -v            Enable verbose diagnostics
  --no-elf-check           Skip build-time DT_NEEDED validation (bootstrap escape hatch)
  --channel <NAME>         Restrict `install` to a single channel
  --wait[=SECONDS]         Wait for a concurrent sage on the same root (default: fail fast)
  --help, -h               Show this help message
  --version, -V            Show version information
)");
}

// Parse global options. Returns nullopt when help/version was printed and the
// caller should exit successfully.
inline std::optional<CliOptions> parse_args(int argc, char* argv[]) {
    CliOptions opts;
    for (int i = 1; i < argc; ++i) {
        std::string_view arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            print_help();
            return std::nullopt;
        }
        if (arg == "--version" || arg == "-V") {
            print_banner();
            return std::nullopt;
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
        } else if (arg == "--wait") {
            opts.wait_seconds = 30;
        } else if (arg.starts_with("--wait=")) {
            opts.wait_seconds = std::atoi(std::string(arg.substr(7)).c_str());
        } else if ((arg == "--root" || arg == "--sysroot") && i + 1 < argc) {
            opts.target_root = argv[++i];
        } else if (opts.command.empty()) {
            opts.command = std::string(arg);
        } else {
            opts.args.push_back(std::string(arg));
        }
    }
    return opts;
}

// Serialize state-changing commands (install/remove/rebuild) against a second
// sage instance on the same target root: flock on <db_dir>/lock, held by the
// returned RootLock for the command's lifetime. Read-only commands never take
// it and are never blocked.
inline std::expected<util::RootLock, int> acquire_root_write_lock(const CliOptions& opts) {
    auto cfg_res = sage::config::SystemConfig::load_from_root(opts.target_root);
    if (!cfg_res) {
        sage::util::log_error("Failed to load configuration: {}", cfg_res.error());
        return std::unexpected(1);
    }
    const auto lock_path = cfg_res->db_path.parent_path() / "lock";
    auto lock = sage::util::RootLock::acquire(lock_path, opts.wait_seconds);
    if (!lock) {
        int holder = 0;
        if (std::ifstream f(lock_path); f.is_open()) (void)(f >> holder);
        sage::util::log_error("another sage instance (pid {}) is operating on '{}'; retry once it finishes",
            holder > 0 ? std::format("{}", holder) : std::string("unknown"),
            cfg_res->root_dir.string());
        return std::unexpected(1);
    }
    return std::expected<util::RootLock, int>(std::move(*lock));
}

inline void warn_pending_filesystem_transactions(sage::db::Database& db) {
    auto pending = db.pending_filesystem_transactions();
    if (pending && !pending->empty()) {
        sage::util::log_warn(
            "Filesystem state has {} pending transaction(s); run any write command to recover them",
            pending->size());
    }
}

inline std::expected<void, std::string> run_full_postprocessing(
    sage::db::Database& db, const std::filesystem::path& root,
    const sage::config::SystemConfig& cfg,
    const std::optional<std::filesystem::path>& trigger_sysroot = std::nullopt)
{
    auto installed = db.list_installed_packages();
    if (!installed) return std::unexpected(installed.error());
    std::vector<sage::channel::Channel> channels;
    for (const auto& configured : cfg.channels) {
        sage::channel::Channel channel;
        channel.name = configured.name;
        channel.scope = sage::channel::parse_scope(configured.scope);
        channel.enabled = configured.enabled;
        channels.push_back(std::move(channel));
    }
    for (const auto& pkg : *installed) {
        const auto spec = sage::channel::SubChannelSpec::parse(pkg.channel);
        if (spec.scope == sage::channel::ChannelScope::Toolchain
            && !spec.category.empty() && !spec.slot.empty()) {
            auto activated = sage::channel::ProfileManager::switch_active_toolchain(
                root, spec.category, spec.slot);
            if (!activated) sage::util::log_warn(
                "Failed to reactivate toolchain '{}:{}': {}",
                spec.category, spec.slot, activated.error());
        }
    }
    if (auto profile = sage::channel::ProfileManager::regenerate_fhs_profile(root, channels); !profile)
        sage::util::log_warn("Failed to regenerate FHS profile after recovery: {}", profile.error());

    sage::rebuild::TriggerContext context;
    context.sysroot = trigger_sysroot.value_or(root);
    context.installed_packages = *installed;
    context.transaction_packages = *installed;
    context.providers = cfg.providers;
    std::unordered_set<std::string> touched_paths;
    for (const auto& pkg : *installed) {
        context.touched_files.insert(context.touched_files.end(), pkg.files.begin(), pkg.files.end());
        for (const auto& file : pkg.files) touched_paths.insert(file.path);
        auto registered = db.get_package_files(pkg.name);
        if (!registered) return std::unexpected(registered.error());
        for (const auto& path : *registered) {
            if (touched_paths.insert(path).second)
                context.touched_files.push_back({.path = path});
        }
    }
    sage::rebuild::TriggerEngine::run(context);
    return {};
}

inline std::expected<sage::db::Database, std::string> open_write_database(
    const sage::config::SystemConfig& cfg, const std::filesystem::path& root,
    const std::optional<std::filesystem::path>& trigger_sysroot = std::nullopt)
{
    auto database = sage::db::Database::open(cfg.db_path);
    if (!database) return std::unexpected(database.error());
    auto pending = database->pending_filesystem_transactions();
    if (!pending) return std::unexpected(pending.error());
    for (const auto& id : *pending) {
        auto replayed = sage::archive::FilesystemTransaction::recover(root, id);
        if (!replayed) return std::unexpected(
            "Cannot recover filesystem transaction '" + id + "': " + replayed.error());
        auto finished = database->finish_filesystem_transaction(id);
        if (!finished) return std::unexpected(finished.error());
        if (auto retired = sage::archive::FilesystemTransaction::retire(root, id); !retired)
            sage::util::log_warn("{}", retired.error());
    }
    if (!pending->empty()) {
        auto processed = run_full_postprocessing(*database, root, cfg, trigger_sysroot);
        if (!processed) return std::unexpected(processed.error());
    }
    return database;
}

} // namespace sage::cli
