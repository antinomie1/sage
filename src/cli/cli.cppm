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

} // namespace sage::cli
