import std;
import sage;

int main(int argc, char** argv) {
    (void)argc; (void)argv;
    sage::util::log_info("Running Sage Phase 0 & 1 Verification Tests (Pure C++23 std module)...");

    // Test 1: Version comparison
    sage::package::Version v1 = sage::package::Version::parse("1.2.3-1");
    sage::package::Version v2 = sage::package::Version::parse("1.2.4-1");
    sage::package::Version v3 = sage::package::Version::parse("1:1.0.0-1");
    if (!(v1 < v2)) {
        sage::util::log_error("Version comparison failed: v1 < v2");
        return 1;
    }
    if (!(v2 < v3)) {
        sage::util::log_error("Version epoch comparison failed: v2 < v3");
        return 1;
    }
    sage::util::log_success("Test 1: Version comparator (vercmp) OK");

    // Test 2: Service rendering
    sage::service::ServiceSpec svc;
    svc.name = "sshd";
    svc.description = "OpenSSH Daemon";
    svc.exec_start = "/usr/sbin/sshd -D";
    svc.after = {"net", "syslog"};
    std::string openrc_out = svc.render_openrc();
    std::string systemd_out = svc.render_systemd();
    if (openrc_out.find("#!/sbin/openrc-run") == std::string::npos ||
        systemd_out.find("[Service]") == std::string::npos) {
        sage::util::log_error("Service rendering failed");
        return 1;
    }
    sage::util::log_success("Test 2: Universal service generator OK");

    // Test 3: LMDB Database operations
    auto temp_db_dir = std::filesystem::temp_directory_path() / "sage_test_db";
    std::filesystem::remove_all(temp_db_dir);

    auto db_res = sage::db::Database::open(temp_db_dir);
    if (!db_res) {
        sage::util::log_error("Database creation failed: {}", db_res.error());
        return 1;
    }
    auto& db = *db_res;

    // Insert package
    sage::package::PackageManifest pkg;
    pkg.name = "ripgrep";
    pkg.version = sage::package::Version::parse("14.1.0-1");
    pkg.description = "Fast line-oriented search tool";
    pkg.license = "MIT";
    pkg.provides = {"ripgrep", "so:librg.so.1"};
    pkg.files.push_back({
        .path = "usr/bin/rg",
        .size = 5000000,
        .mode = 0755,
        .sha256 = "dummy",
        .type = sage::package::FileType::Regular,
        .link_target = ""
    });

    auto wtxn = db.begin_write_txn();
    if (!wtxn) {
        sage::util::log_error("Failed to begin write txn");
        return 1;
    }

    auto put_pkg_res = db.put_package(*wtxn, pkg);
    if (!put_pkg_res) {
        sage::util::log_error("Put package failed: {}", put_pkg_res.error());
        return 1;
    }

    auto reg_files_res = db.register_files(*wtxn, pkg.name, pkg.channel, pkg.files);
    if (!reg_files_res) {
        sage::util::log_error("Register files failed: {}", reg_files_res.error());
        return 1;
    }

    auto reg_prov_res = db.register_provides(*wtxn, pkg.name, pkg.provides);
    if (!reg_prov_res) {
        sage::util::log_error("Register provides failed: {}", reg_prov_res.error());
        return 1;
    }

    auto commit_res = wtxn->commit();
    if (!commit_res) {
        sage::util::log_error("Commit failed: {}", commit_res.error());
        return 1;
    }

    // Query back
    auto queried_pkg = db.get_package("ripgrep");
    if (!queried_pkg || queried_pkg->version.ver != "14.1.0") {
        sage::util::log_error("Query package failed");
        return 1;
    }

    auto owner = db.get_file_owner("usr/bin/rg");
    if (!owner || *owner != "ripgrep:system") {
        sage::util::log_error("Query file owner failed");
        return 1;
    }

    auto provider = db.get_provider("so:librg.so.1");
    if (!provider || *provider != "ripgrep") {
        sage::util::log_error("Query provider failed");
        return 1;
    }

    std::filesystem::remove_all(temp_db_dir);
    sage::util::log_success("Test 3: LMDB ACID Database & Zero-Copy Queries OK");

    sage::util::log_success("All Phase 0 & Phase 1 module tests passed successfully with pure import std!");
    return 0;
}
