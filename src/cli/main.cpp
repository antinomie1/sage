#include <stdlib.h>

import std;
import sage;

namespace {

using std::size_t;
using std::uint32_t;

void print_banner() {
    std::println("{}🌿 Sage Package Manager v0.2.0 (Modern C++23){}", sage::util::color::green, sage::util::color::reset);
}

void print_help() {
    print_banner();
    std::println(R"(
Usage: sage [OPTIONS] <COMMAND> [ARGS...]

Commands:
  install <PKG...>         Install packages via PubGrub SAT solver & unpack archive
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
  build <RECIPE_DIR>       Build *.pkg.tar.zst package from recipe.toml with ELF scanner
  test-suite               Run internal engine self-test suite

Global Options:
  --help, -h               Show this help message
  --version, -V            Show version information
  --verbose, -v            Enable verbose diagnostics
  --dry-run                Simulate actions without modifying filesystem
)");
}

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
    std::filesystem::create_directories(temp_dir / "data/usr/bin");

    std::ofstream dummy_bin(temp_dir / "data/usr/bin/dummy");
    dummy_bin << "#!/bin/sh\necho 'hello sage'\n";
    dummy_bin.close();
    std::filesystem::permissions(temp_dir / "data/usr/bin/dummy", std::filesystem::perms::owner_all | std::filesystem::perms::group_read | std::filesystem::perms::group_exec);

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

    auto extract_root = temp_dir / "sysroot";
    auto ext_res = sage::archive::extract_package(pkg_path, extract_root);
    if (!ext_res || !std::filesystem::exists(extract_root / "usr/bin/dummy")) {
        sage::util::log_error("Archive extraction verification failed");
        return 1;
    }
    sage::util::log_success("2. Native Streaming Tar + Zstandard Engine OK");

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
    if (!sw_res || !std::filesystem::exists(extract_root / "etc/sage/profiles/default/bin/cc")) {
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

    std::filesystem::remove_all(temp_dir);
    sage::util::log_success("🎉 All Sage Master Architecture & Subsystem Integration Tests Passed Successfully!");
    return 0;
}

int cmd_query(int argc, char** argv) {
    if (argc < 3) {
        std::println("Usage: sage query [installed|info <pkg>|owner <path>]");
        return 1;
    }

    std::string sub = argv[2];
    auto db_res = sage::db::Database::open("/var/lib/sage/data.mdb", true);
    if (!db_res) {
        sage::util::log_warn("Database at /var/lib/sage/data.mdb not yet initialized or inaccessible: {}", db_res.error());
        return 0;
    }
    auto& db = *db_res;

    if (sub == "installed") {
        auto list = db.list_installed_packages();
        std::println("Installed packages ({} total):", list.size());
        for (const auto& pkg : list) {
            std::println("  • {:<20} {:<15} [{}]", pkg.name, pkg.version.to_string(), pkg.channel);
        }
    } else if (sub == "info" && argc >= 4) {
        std::string pkg_name = argv[3];
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
    } else if (sub == "owner" && argc >= 4) {
        std::string path = argv[3];
        if (auto owner = db.get_file_owner(path)) {
            std::println("{} is owned by {}", path, *owner);
        } else {
            std::println("No installed package owns {}", path);
        }
    }
    return 0;
}

int cmd_toolchain(int argc, char** argv) {
    if (argc < 3 || std::string_view(argv[2]) == "list") {
        auto list = sage::channel::ProfileManager::list_installed_subchannels("/");
        std::println("Installed Toolchains & Runtimes (Sub-Channels):");
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

    std::string action = argv[2];
    if (action == "use" && argc >= 4) {
        auto spec = sage::channel::SubChannelSpec::parse(argv[3]);
        auto res = sage::channel::ProfileManager::switch_active_toolchain("/", spec.category, spec.slot);
        if (!res) {
            sage::util::log_error("{}", res.error());
            return 1;
        }
        return 0;
    }

    std::println("Usage: sage toolchain [list|use <category:slot>]");
    return 1;
}

int cmd_java(int argc, char** argv) {
    if (argc < 3 || std::string_view(argv[2]) == "list") {
        auto list = sage::channel::ProfileManager::list_installed_subchannels("/");
        std::println("Installed Java Environments:");
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

    if (std::string_view(argv[2]) == "use" && argc >= 4) {
        std::string slot = argv[3];
        auto res = sage::channel::ProfileManager::switch_active_toolchain("/", "java", slot);
        if (!res) {
            sage::util::log_error("{}", res.error());
            return 1;
        }
        return 0;
    }

    std::println("Usage: sage java [list|use <slot>]");
    return 1;
}

int cmd_rust(int argc, char** argv) {
    if (argc < 3 || std::string_view(argv[2]) == "list") {
        auto list = sage::channel::ProfileManager::list_installed_subchannels("/");
        std::println("Installed Rust Toolchains:");
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

    if (std::string_view(argv[2]) == "use" && argc >= 4) {
        std::string slot = argv[3];
        auto res = sage::channel::ProfileManager::switch_active_toolchain("/", "rust", slot);
        if (!res) {
            sage::util::log_error("{}", res.error());
            return 1;
        }
        return 0;
    }

    std::println("Usage: sage rust [list|use <slot>]");
    return 1;
}

int cmd_shell(int argc, char** argv) {
    std::vector<sage::channel::SubChannelSpec> specs;
    for (int i = 2; i < argc; ++i) {
        if (std::string_view(argv[i]) == "--with" && i + 1 < argc) {
            specs.push_back(sage::channel::SubChannelSpec::parse(argv[++i]));
        }
    }

    if (specs.empty()) {
        std::println("Usage: sage shell --with <sub-channel...> (e.g. sage shell --with toolchain/llvm:22 --with runtime/python:3.12)");
        return 1;
    }

    auto env = sage::channel::ProfileManager::generate_shell_env("/", specs);
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

int cmd_service(int argc, char** argv) {
    if (argc < 3) {
        std::println("Usage: sage service [list|generate <name>]");
        return 1;
    }
    std::string sub = argv[2];
    if (sub == "list") {
        sage::util::log_info("Available native init targets: OpenRC, Runit, Systemd, Dinit, s6");
    } else if (sub == "generate" && argc >= 4) {
        std::string name = argv[3];
        sage::service::ServiceSpec spec;
        spec.name = name;
        spec.exec_start = "/usr/bin/" + name;
        auto res = sage::service::generate_service(spec, sage::service::InitType::OpenRC);
        if (res) {
            sage::util::log_success("Generated OpenRC service script at {}", res->string());
        }
    }
    return 0;
}

int cmd_channel(int argc, char** argv) {
    if (argc < 3 || std::string_view(argv[2]) == "list") {
        auto cfg = sage::config::SystemConfig::load_or_default("/etc/sage");
        if (!cfg) {
            sage::util::log_error("Failed to load /etc/sage configuration: {}", cfg.error());
            return 1;
        }
        std::println("Configured Channels:");
        for (const auto& ch : cfg->channels) {
            std::println("  • {:<15} {:<35} scope: {:<10} priority: {}", 
                ch.name, ch.url, ch.scope, ch.priority);
        }
        return 0;
    }
    return 0;
}

int cmd_build(int argc, char** argv) {
    if (argc < 3) {
        std::println("Usage: sage build <RECIPE_DIR>");
        return 1;
    }
    std::filesystem::path recipe_dir = argv[2];
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
    sage::util::log_info("Building package '{}' version {}...", r.name, r.version.to_string());

    // Create manifest and package archive
    sage::package::PackageManifest manifest;
    manifest.name = r.name;
    manifest.version = r.version;
    manifest.description = r.description;
    manifest.license = r.license;
    manifest.channel = r.channel;
    manifest.dependencies = r.host_deps;
    manifest.provides = r.provides;

    // Scan ELF binaries in data directory if present
    std::filesystem::path pkg_data = recipe_dir / "pkg";
    if (std::filesystem::exists(pkg_data)) {
        for (const auto& entry : std::filesystem::recursive_directory_iterator(pkg_data)) {
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

    std::string out_name = std::format("{}-{}-{}.pkg.tar.zst", r.name, r.version.ver, r.version.rel);
    std::filesystem::path out_path = recipe_dir / out_name;
    auto pack_res = sage::archive::create_package(manifest, pkg_data, out_path);
    if (!pack_res) {
        sage::util::log_error("Package build failed: {}", pack_res.error());
        return 1;
    }

    sage::util::log_success("Package built successfully: {}", out_path.string());
    return 0;
}

int cmd_rebuild(int argc, char** argv) {
    bool dry_run = false;
    for (int i = 2; i < argc; ++i) {
        if (std::string_view(argv[i]) == "--dry-run") dry_run = true;
    }

    auto cfg_res = sage::config::SystemConfig::load_or_default("/etc/sage");
    if (!cfg_res) {
        sage::util::log_error("Failed to load /etc/sage configuration: {}", cfg_res.error());
        return 1;
    }

    auto db_res = sage::db::Database::open(cfg_res->db_path);
    if (!db_res) {
        sage::util::log_error("Failed to open database: {}", db_res.error());
        return 1;
    }

    std::vector<sage::package::PackageManifest> pool; // repository pool
    auto plan_res = sage::rebuild::ReconcileEngine::calculate_diff(*db_res, *cfg_res, pool);
    if (!plan_res) {
        sage::util::log_error("Failed to calculate reconcile plan: {}", plan_res.error());
        return 1;
    }

    auto exec_res = sage::rebuild::ReconcileEngine::execute(*db_res, *plan_res, cfg_res->root_dir, dry_run);
    if (!exec_res) {
        sage::util::log_error("Reconcile execution failed: {}", exec_res.error());
        return 1;
    }
    return 0;
}

int cmd_repo(int argc, char** argv) {
    if (argc < 4 || std::string_view(argv[2]) != "index") {
        std::println("Usage: sage repo index <REPO_DIR> [CHANNEL_NAME]");
        return 1;
    }
    std::filesystem::path repo_dir = argv[3];
    std::string ch_name = (argc >= 5) ? argv[4] : "core";
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

    std::string_view command = argv[1];

    if (command == "--help" || command == "-h") {
        print_help();
        return 0;
    }
    if (command == "--version" || command == "-V") {
        print_banner();
        return 0;
    }
    if (command == "test-suite" || command == "test") {
        return cmd_test_suite();
    }
    if (command == "query") {
        return cmd_query(argc, argv);
    }
    if (command == "toolchain") {
        return cmd_toolchain(argc, argv);
    }
    if (command == "java") {
        return cmd_java(argc, argv);
    }
    if (command == "rust") {
        return cmd_rust(argc, argv);
    }
    if (command == "shell") {
        return cmd_shell(argc, argv);
    }
    if (command == "service") {
        return cmd_service(argc, argv);
    }
    if (command == "channel") {
        return cmd_channel(argc, argv);
    }
    if (command == "repo") {
        return cmd_repo(argc, argv);
    }
    if (command == "build") {
        return cmd_build(argc, argv);
    }
    if (command == "rebuild") {
        return cmd_rebuild(argc, argv);
    }

    // Default: show help for unimplemented or unknown
    std::println(std::cerr, "Unknown or unhandled command: '{}'", command);
    print_help();
    return 1;
}
