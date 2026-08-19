export module sage.config;

import std;
import sage.vendor.toml;
import sage.util;

export namespace sage::config {

struct ChannelConfig {
    std::string name;
    std::string url;
    std::string scope{"system"}; // "system", "runtime", "toolchain", "user"
    int priority{50};
    bool enabled{true};
};

struct SystemConfig {
    std::filesystem::path root_dir{"/"};
    std::filesystem::path db_path{"/var/lib/distro/data.mdb"};
    std::filesystem::path cache_dir{"/var/cache/distro"};
    std::filesystem::path system_config_path{"/etc/distro/system.toml"};

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
        core.url = "https://pkg.distro.org/core";
        core.scope = "system";
        core.priority = 100;
        core.enabled = true;
        cfg.channels.push_back(std::move(core));

        return cfg;
    }

    static std::expected<SystemConfig, std::string> parse_toml(std::string_view toml_str) {
        auto tbl_res = vendor::toml::parse_string(toml_str);
        if (!tbl_res) return std::unexpected(tbl_res.error());
        const auto& tbl = *tbl_res;

        SystemConfig cfg;

        if (auto* sys = tbl.get_as<vendor::toml::table>("system")) {
            if (auto r = sys->get("root_dir")) cfg.root_dir = r->value_or("/");
            if (auto d = sys->get("db_path")) cfg.db_path = d->value_or("/var/lib/distro/data.mdb");
            if (auto c = sys->get("cache_dir")) cfg.cache_dir = c->value_or("/var/cache/distro");
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

        if (auto* chs = tbl.get_as<vendor::toml::array>("channels")) {
            for (auto&& item : *chs) {
                if (auto* ctab = item.as_table()) {
                    ChannelConfig ch;
                    ch.name = ctab->get("name")->value_or("");
                    ch.url = ctab->get("url")->value_or("");
                    ch.scope = ctab->get("scope")->value_or("system");
                    ch.priority = static_cast<int>(ctab->get("priority")->value_or(50LL));
                    ch.enabled = ctab->get("enabled")->value_or(true);
                    if (!ch.name.empty()) {
                        cfg.channels.push_back(std::move(ch));
                    }
                }
            }
        }

        return cfg;
    }

    static std::expected<SystemConfig, std::string> load_or_default(const std::filesystem::path& path) {
        if (!std::filesystem::exists(path)) {
            return default_config();
        }
        auto tbl_res = vendor::toml::parse_file(path);
        if (!tbl_res) return std::unexpected(tbl_res.error());
        std::ifstream f(path);
        std::stringstream ss;
        ss << f.rdbuf();
        return parse_toml(ss.str());
    }

    [[nodiscard]] std::string serialize_toml() const {
        std::ostringstream ss;
        ss << "[system]\n";
        ss << "root_dir = \"" << root_dir.string() << "\"\n";
        ss << "db_path = \"" << db_path.string() << "\"\n";
        ss << "cache_dir = \"" << cache_dir.string() << "\"\n\n";

        ss << "[providers]\n";
        for (const auto& [k, v] : providers) {
            std::string short_key = k.starts_with("virtual/") ? k.substr(8) : k;
            ss << short_key << " = \"" << v << "\"\n";
        }
        ss << "\n";

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
