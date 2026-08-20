export module sage.config;

import std;
import sage.vendor.toml;
import sage.util;

export namespace sage::config {

using std::uint32_t;
using std::size_t;

struct ChannelConfig {
    std::string name;
    std::string url;
    std::string scope{"system"}; // "system", "runtime", "toolchain", "user"
    int priority{50};
    bool enabled{true};
};

struct SystemConfig {
    uint32_t schema_version{1};
    std::filesystem::path root_dir{"/"};
    std::filesystem::path db_path{"/var/lib/sage/data.mdb"};
    std::filesystem::path cache_dir{"/var/cache/sage"};
    std::filesystem::path config_dir{"/etc/sage"};
    std::filesystem::path system_config_path{"/etc/sage/system.toml"};
    std::filesystem::path channels_config_path{"/etc/sage/channels.toml"};

    // Core minimal virtual providers
    // e.g. "virtual/init" -> "openrc", "virtual/udev" -> "eudev", "virtual/libc" -> "glibc"
    std::map<std::string, std::string> providers;

    std::vector<ChannelConfig> channels;

    static SystemConfig default_config() {
        SystemConfig cfg;
        cfg.providers["virtual/init"] = "openrc";
        cfg.providers["virtual/udev"] = "eudev";
        cfg.providers["virtual/libc"] = "glibc";

        ChannelConfig core;
        core.name = "core";
        core.url = "https://pkg.sage-linux.org/core";
        core.scope = "system";
        core.priority = 100;
        core.enabled = true;
        cfg.channels.push_back(std::move(core));

        return cfg;
    }

    static std::expected<SystemConfig, std::string> parse_system_toml(std::string_view toml_str) {
        auto tbl_res = vendor::toml::parse_string(toml_str);
        if (!tbl_res) return std::unexpected(tbl_res.error());
        const auto& tbl = *tbl_res;

        SystemConfig cfg;

        if (auto* sys = tbl.get_as<vendor::toml::table>("system")) {
            cfg.root_dir = (*sys)["root_dir"].value_or("/");
            cfg.db_path = (*sys)["db_path"].value_or("/var/lib/sage/data.mdb");
            cfg.cache_dir = (*sys)["cache_dir"].value_or("/var/cache/sage");
            if (auto* cfg_d = sys->get("config_dir")) {
                cfg.config_dir = cfg_d->value_or("/etc/sage");
                cfg.system_config_path = cfg.config_dir / "system.toml";
                cfg.channels_config_path = cfg.config_dir / "channels.toml";
            }
        }

        if (auto* prov = tbl.get_as<vendor::toml::table>("providers")) {
            for (auto&& [k, v] : *prov) {
                if (auto val_str = v.value<std::string_view>()) {
                    std::string key_str(k.str());
                    if (!key_str.starts_with("virtual/")) {
                        key_str = "virtual/" + key_str;
                    }
                    cfg.providers[key_str] = std::string(*val_str);
                }
            }
        }

        // Also check if channels are defined inside system.toml
        if (auto* chs = tbl.get_as<vendor::toml::array>("channels")) {
            for (auto&& item : *chs) {
                if (auto* ctab = item.as_table()) {
                    ChannelConfig ch;
                    ch.name = (*ctab)["name"].value_or("");
                    ch.url = (*ctab)["url"].value_or("");
                    ch.scope = (*ctab)["scope"].value_or("system");
                    ch.priority = static_cast<int>((*ctab)["priority"].value_or(50LL));
                    ch.enabled = (*ctab)["enabled"].value_or(true);
                    if (!ch.name.empty()) {
                        cfg.channels.push_back(std::move(ch));
                    }
                }
            }
        }

        return cfg;
    }

    static std::expected<std::vector<ChannelConfig>, std::string> parse_channels_toml(std::string_view toml_str) {
        auto tbl_res = vendor::toml::parse_string(toml_str);
        if (!tbl_res) return std::unexpected(tbl_res.error());
        const auto& tbl = *tbl_res;

        std::vector<ChannelConfig> list;
        if (auto* chs = tbl.get_as<vendor::toml::array>("channels")) {
            for (auto&& item : *chs) {
                if (auto* ctab = item.as_table()) {
                    ChannelConfig ch;
                    ch.name = (*ctab)["name"].value_or("");
                    ch.url = (*ctab)["url"].value_or("");
                    ch.scope = (*ctab)["scope"].value_or("system");
                    ch.priority = static_cast<int>((*ctab)["priority"].value_or(50LL));
                    ch.enabled = (*ctab)["enabled"].value_or(true);
                    if (!ch.name.empty()) {
                        list.push_back(std::move(ch));
                    }
                }
            }
        }
        return list;
    }

    static std::expected<SystemConfig, std::string> load_or_default(const std::filesystem::path& config_dir = "/etc/sage") {
        std::filesystem::path sys_path = config_dir / "system.toml";
        std::filesystem::path chan_path = config_dir / "channels.toml";

        SystemConfig cfg = default_config();
        cfg.config_dir = config_dir;
        cfg.system_config_path = sys_path;
        cfg.channels_config_path = chan_path;

        if (std::filesystem::exists(sys_path)) {
            std::ifstream f(sys_path);
            std::stringstream ss;
            ss << f.rdbuf();
            auto sys_res = parse_system_toml(ss.str());
            if (!sys_res) return sys_res;
            cfg = std::move(*sys_res);
            cfg.config_dir = config_dir;
            cfg.system_config_path = sys_path;
            cfg.channels_config_path = chan_path;
        }

        if (std::filesystem::exists(chan_path)) {
            std::ifstream f(chan_path);
            std::stringstream ss;
            ss << f.rdbuf();
            auto ch_res = parse_channels_toml(ss.str());
            if (ch_res && !ch_res->empty()) {
                cfg.channels = std::move(*ch_res);
            }
        } else if (config_dir != "/etc/sage" && std::filesystem::exists("/etc/sage/channels.toml")) {
            std::ifstream f("/etc/sage/channels.toml");
            std::stringstream ss;
            ss << f.rdbuf();
            auto ch_res = parse_channels_toml(ss.str());
            if (ch_res && !ch_res->empty()) {
                cfg.channels = std::move(*ch_res);
            }
        }

        return cfg;
    }

    static std::expected<SystemConfig, std::string> load_from_root(const std::filesystem::path& target_root = "/") {
        std::filesystem::path norm_root = target_root.empty() ? "/" : target_root;
        std::filesystem::path config_dir = (norm_root == "/") ? "/etc/sage" : (norm_root / "etc/sage");
        auto res = load_or_default(config_dir);
        if (!res) return res;

        res->root_dir = norm_root;
        if (norm_root != "/") {
            res->db_path = norm_root / "var/lib/sage/data.mdb";
            res->cache_dir = norm_root / "var/cache/sage";
            res->config_dir = config_dir;
            res->system_config_path = config_dir / "system.toml";
            res->channels_config_path = config_dir / "channels.toml";
        }
        return res;
    }

    [[nodiscard]] std::string serialize_system_toml() const {
        std::ostringstream ss;
        ss << "schema_version = " << schema_version << "\n\n";
        ss << "[system]\n";
        ss << "root_dir = \"" << root_dir.string() << "\"\n";
        ss << "db_path = \"" << db_path.string() << "\"\n";
        ss << "cache_dir = \"" << cache_dir.string() << "\"\n";
        ss << "config_dir = \"" << config_dir.string() << "\"\n\n";

        ss << "[providers]\n";
        for (const auto& [k, v] : providers) {
            std::string short_key = k.starts_with("virtual/") ? k.substr(8) : k;
            ss << short_key << " = \"" << v << "\"\n";
        }
        return ss.str();
    }

    [[nodiscard]] std::string serialize_channels_toml() const {
        std::ostringstream ss;
        ss << "schema_version = 1\n\n";
        for (const auto& ch : channels) {
            ss << "[[channels]]\n";
            ss << "name = \"" << ch.name << "\"\n";
            ss << "url = \"" << ch.url << "\"\n";
            ss << "scope = \"" << ch.scope << "\"\n";
            ss << "priority = " << ch.priority << "\n";
            ss << "enabled = " << (ch.enabled ? "true" : "false") << "\n\n";
        }
        return ss.str();
    }
};

} // namespace sage::config
