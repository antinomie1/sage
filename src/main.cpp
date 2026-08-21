import std;
import sage;
import sage.cli;
import sage.cli.build;
import sage.cli.pkg;
import sage.cli.query;
import sage.cli.toolchain;

// Entry point: global option parsing lives in sage.cli; this file only maps
// the command word to its implementation module.
int main(int argc, char* argv[]) {
    if (argc < 2) {
        sage::cli::print_help();
        return 0;
    }

    auto parsed = sage::cli::parse_args(argc, argv);
    if (!parsed) return 0;
    const auto& opts = *parsed;

    using namespace sage::cli;

    // State-changing commands take the target root's write lock first, so a
    // second concurrent sage fails fast at the entrance instead of racing.
    std::optional<sage::util::RootLock> mutation_lock;
    if (opts.command == "install" || opts.command == "remove" || opts.command == "rebuild") {
        auto lock_res = acquire_root_write_lock(opts);
        if (!lock_res) return lock_res.error();
        mutation_lock = std::move(*lock_res);
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
    sage::cli::print_help();
    return 1;
}
