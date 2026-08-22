module;

#include <sys/stat.h>

export module sage.service;

import std;
import sage.vendor.toml;
import sage.util;

export namespace sage::service {

using std::uint32_t;
using std::size_t;

enum class InitType {
    OpenRC,
    Runit,
    Systemd,
    Dinit,
    S6,
    Unknown
};

inline InitType parse_init_type(std::string_view s) noexcept {
    if (s == "openrc" || s == "OpenRC") return InitType::OpenRC;
    if (s == "runit" || s == "Runit") return InitType::Runit;
    if (s == "systemd" || s == "Systemd") return InitType::Systemd;
    if (s == "dinit" || s == "Dinit") return InitType::Dinit;
    if (s == "s6" || s == "S6") return InitType::S6;
    return InitType::Unknown;
}

inline std::string_view to_string(InitType t) noexcept {
    switch (t) {
        case InitType::OpenRC:  return "openrc";
        case InitType::Runit:   return "runit";
        case InitType::Systemd: return "systemd";
        case InitType::Dinit:   return "dinit";
        case InitType::S6:      return "s6";
        default:                return "unknown";
    }
}

struct ServiceSpec {
    uint32_t schema_version{1};
    std::string name;
    std::string description;
    std::string exec_start;
    std::string exec_stop;
    std::string exec_reload;
    std::string user{"root"};
    std::string group{"root"};
    std::string working_dir{"/"};
    std::string pid_file;
    std::string restart{"always"}; // "always", "on-failure", "no"
    std::string type{"simple"};     // "simple", "forking"
    std::vector<std::string> after;
    std::vector<std::string> before;

    static std::expected<ServiceSpec, std::string> parse_toml(std::string_view toml_str) {
        auto tbl_res = vendor::toml::parse_string(toml_str);
        if (!tbl_res) return std::unexpected(tbl_res.error());
        const auto& tbl = *tbl_res;

        ServiceSpec s;
        s.schema_version = static_cast<uint32_t>(tbl["schema_version"].value_or(1LL));
        if (auto* svc = tbl.get_as<vendor::toml::table>("service")) {
            s.name = (*svc)["name"].value_or("");
            s.description = (*svc)["description"].value_or("");
            s.exec_start = (*svc)["exec_start"].value_or("");
            s.exec_stop = (*svc)["exec_stop"].value_or("");
            s.exec_reload = (*svc)["exec_reload"].value_or("");
            s.user = (*svc)["user"].value_or("root");
            s.group = (*svc)["group"].value_or("root");
            s.working_dir = (*svc)["working_dir"].value_or("/");
            s.pid_file = (*svc)["pid_file"].value_or("");
            s.restart = (*svc)["restart"].value_or("always");
            s.type = (*svc)["type"].value_or("simple");

            if (auto* aft = svc->get_as<vendor::toml::array>("after")) {
                for (auto&& a : *aft) {
                    if (auto str = a.value<std::string_view>()) {
                        s.after.emplace_back(*str);
                    }
                }
            }
            if (auto* bef = svc->get_as<vendor::toml::array>("before")) {
                for (auto&& b : *bef) {
                    if (auto str = b.value<std::string_view>()) {
                        s.before.emplace_back(*str);
                    }
                }
            }
        } else {
            return std::unexpected("Missing [service] section in service.toml");
        }

        if (s.name.empty() || s.exec_start.empty()) {
            return std::unexpected("Service 'name' and 'exec_start' are required");
        }
        return s;
    }

    [[nodiscard]] std::string render_openrc() const {
        std::ostringstream ss;
        ss << "#!/usr/bin/openrc-run\n";
        ss << "# Generated automatically by Sage Package Manager\n";
        ss << "description=\"" << (description.empty() ? name : description) << "\"\n\n";

        ss << "depend() {\n";
        if (!after.empty()) {
            ss << "    need " << util::join(after, " ") << "\n";
        } else {
            ss << "    need net\n";
        }
        if (!before.empty()) {
            ss << "    before " << util::join(before, " ") << "\n";
        }
        ss << "}\n\n";

        ss << "command=\"" << exec_start << "\"\n";
        if (!pid_file.empty()) {
            ss << "pidfile=\"" << pid_file << "\"\n";
        }
        if (user != "root") {
            ss << "command_user=\"" << user << ":" << group << "\"\n";
        }
        if (type == "simple") {
            ss << "command_background=\"yes\"\n";
            if (pid_file.empty()) {
                ss << "pidfile=\"/run/" << name << ".pid\"\n";
            }
        }
        return ss.str();
    }

    [[nodiscard]] std::string render_systemd() const {
        std::ostringstream ss;
        ss << "# Generated automatically by Sage Package Manager\n";
        ss << "[Unit]\n";
        ss << "Description=" << (description.empty() ? name : description) << "\n";
        if (!after.empty()) {
            ss << "After=" << util::join(after, " ") << "\n";
        }
        if (!before.empty()) {
            ss << "Before=" << util::join(before, " ") << "\n";
        }
        ss << "\n[Service]\n";
        ss << "Type=" << (type.empty() ? "simple" : type) << "\n";
        ss << "ExecStart=" << exec_start << "\n";
        if (!exec_stop.empty()) ss << "ExecStop=" << exec_stop << "\n";
        if (!exec_reload.empty()) ss << "ExecReload=" << exec_reload << "\n";
        if (user != "root") ss << "User=" << user << "\n";
        if (group != "root") ss << "Group=" << group << "\n";
        if (working_dir != "/") ss << "WorkingDirectory=" << working_dir << "\n";
        if (!pid_file.empty()) ss << "PIDFile=" << pid_file << "\n";
        if (restart != "no") ss << "Restart=" << (restart == "always" ? "always" : "on-failure") << "\n";

        ss << "\n[Install]\n";
        ss << "WantedBy=multi-user.target\n";
        return ss.str();
    }

    [[nodiscard]] std::string render_runit() const {
        std::ostringstream ss;
        ss << "#!/bin/sh\n";
        ss << "# Generated automatically by Sage Package Manager\n";
        ss << "exec 2>&1\n";
        if (working_dir != "/") {
            ss << "cd " << working_dir << " || exit 1\n";
        }
        if (user != "root") {
            ss << "exec chpst -u " << user << ":" << group << " " << exec_start << "\n";
        } else {
            ss << "exec " << exec_start << "\n";
        }
        return ss.str();
    }

    [[nodiscard]] std::string render_dinit() const {
        std::ostringstream ss;
        ss << "# Generated automatically by Sage Package Manager\n";
        ss << "type = process\n";
        ss << "command = " << exec_start << "\n";
        if (working_dir != "/") ss << "working-dir = " << working_dir << "\n";
        if (!after.empty()) {
            for (const auto& a : after) {
                ss << "depends-on = " << a << "\n";
            }
        }
        if (restart == "always") {
            ss << "restart = true\n";
        }
        return ss.str();
    }

    [[nodiscard]] std::string render_s6() const {
        std::ostringstream ss;
        ss << "#!/bin/sh\n";
        ss << "# Generated automatically by Sage Package Manager\n";
        if (user != "root") {
            ss << "exec s6-setuidgid " << user << " " << exec_start << "\n";
        } else {
            ss << "exec " << exec_start << "\n";
        }
        return ss.str();
    }
};

// Where this init system's script for `name` lives under the sysroot.
// Callers use it to decide whether the path is already owned by a package
// (a shipped native unit wins over the generated form).
inline std::expected<std::filesystem::path, std::string> service_destination(
    std::string_view name,
    InitType init_type,
    const std::filesystem::path& sysroot = "/")
{
    switch (init_type) {
        case InitType::OpenRC:  return sysroot / "etc/init.d" / name;
        case InitType::Systemd: return sysroot / "usr/lib/systemd/system" / (std::string(name) + ".service");
        case InitType::Runit:   return sysroot / "etc/sv" / name / "run";
        case InitType::Dinit:   return sysroot / "etc/dinit.d" / name;
        case InitType::S6:      return sysroot / "etc/s6/services" / name / "run";
        default:                return std::unexpected("Unsupported init system");
    }
}

inline std::expected<std::filesystem::path, std::string> generate_service(
    const ServiceSpec& spec,
    InitType init_type,
    const std::filesystem::path& sysroot = "/")
{
    auto dest_res = service_destination(spec.name, init_type, sysroot);
    if (!dest_res) return std::unexpected(dest_res.error());
    std::filesystem::path dest = std::move(*dest_res);
    std::string content;
    bool is_executable = false;

    switch (init_type) {
        case InitType::OpenRC:
            content = spec.render_openrc();
            is_executable = true;
            break;
        case InitType::Systemd:
            content = spec.render_systemd();
            is_executable = false;
            break;
        case InitType::Runit:
            content = spec.render_runit();
            is_executable = true;
            break;
        case InitType::Dinit:
            content = spec.render_dinit();
            is_executable = false;
            break;
        case InitType::S6:
            content = spec.render_s6();
            is_executable = true;
            break;
        default:
            return std::unexpected("Unsupported init system");
    }

    if (auto parent = dest.parent_path(); !parent.empty()) {
        std::filesystem::create_directories(parent);
    }

    std::ofstream out(dest);
    if (!out.is_open()) {
        return std::unexpected("Failed to write service file: " + dest.string());
    }
    out << content;
    out.close();

    if (is_executable) {
        ::chmod(dest.c_str(), 0755);
    } else {
        ::chmod(dest.c_str(), 0644);
    }

    return dest;
}

inline bool remove_service(
    std::string_view name,
    InitType init_type,
    const std::filesystem::path& sysroot = "/") 
{
    std::filesystem::path dest;
    switch (init_type) {
        case InitType::OpenRC:
            dest = sysroot / "etc/init.d" / name;
            break;
        case InitType::Systemd:
            dest = sysroot / "usr/lib/systemd/system" / (std::string(name) + ".service");
            break;
        case InitType::Runit: {
            std::filesystem::path sv_dir = sysroot / "etc/sv" / name;
            std::error_code ec;
            return std::filesystem::remove_all(sv_dir, ec) > 0;
        }
        case InitType::Dinit:
            dest = sysroot / "etc/dinit.d" / name;
            break;
        case InitType::S6: {
            std::filesystem::path s6_dir = sysroot / "etc/s6/services" / name;
            std::error_code ec;
            return std::filesystem::remove_all(s6_dir, ec) > 0;
        }
        default:
            return false;
    }

    std::error_code ec;
    return std::filesystem::remove(dest, ec);
}

} // namespace sage::service
