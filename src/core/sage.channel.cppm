export module sage.channel;

import std;
import sage.vendor.toml;
import sage.vendor.curl;
import sage.package;
import sage.util;

export namespace sage::channel {

using std::size_t;

enum class ChannelScope {
    System,
    Runtime,
    Toolchain,
    User
};

inline ChannelScope parse_scope(std::string_view s) noexcept {
    if (s == "runtime" || s == "Runtime") return ChannelScope::Runtime;
    if (s == "toolchain" || s == "Toolchain") return ChannelScope::Toolchain;
    if (s == "user" || s == "User") return ChannelScope::User;
    return ChannelScope::System;
}

inline std::string_view to_string(ChannelScope sc) noexcept {
    switch (sc) {
        case ChannelScope::Runtime:   return "runtime";
        case ChannelScope::Toolchain: return "toolchain";
        case ChannelScope::User:      return "user";
        default:                      return "system";
    }
}

struct Channel {
    std::string name;
    std::string url;
    ChannelScope scope{ChannelScope::System};
    int priority{50};
    bool enabled{true};
    std::string triplet{"x86_64-linux-gnu"};

    [[nodiscard]] std::filesystem::path resolve_target_root(
        const std::filesystem::path& sysroot = "/",
        const std::filesystem::path& user_home = {}) const 
    {
        switch (scope) {
            case ChannelScope::System:
                return sysroot;
            case ChannelScope::Runtime:
                return sysroot / "usr/lib/runtimes" / name;
            case ChannelScope::Toolchain:
                return sysroot / "opt/channels" / name;
            case ChannelScope::User:
                return user_home.empty() ? (sysroot / "root/.local") : (user_home / ".local");
        }
        return sysroot;
    }
};

// ============================================================================
// Channel Remote Index Representation
// ============================================================================

struct ChannelIndex {
    std::string channel_name;
    std::string updated_at;
    std::vector<package::PackageManifest> available_packages;

    static std::expected<ChannelIndex, std::string> parse_toml(std::string_view toml_str) {
        auto tbl_res = vendor::toml::parse_string(toml_str);
        if (!tbl_res) return std::unexpected(tbl_res.error());
        const auto& tbl = *tbl_res;

        ChannelIndex idx;
        if (auto* meta = tbl.get_as<vendor::toml::table>("channel")) {
            idx.channel_name = meta->get("name")->value_or("");
            idx.updated_at = meta->get("updated_at")->value_or("");
        }

        if (auto* pkgs = tbl.get_as<vendor::toml::array>("packages")) {
            for (auto&& item : *pkgs) {
                if (auto* ptab = item.as_table()) {
                    std::ostringstream ss;
                    ss << *ptab;
                    auto p_res = package::PackageManifest::parse_toml(ss.str());
                    if (p_res) {
                        idx.available_packages.push_back(std::move(*p_res));
                    }
                }
            }
        }
        return idx;
    }
};

// ============================================================================
// FHS Profile Symlink Aggregator & Environment Hook Generator
// ============================================================================

class ProfileManager {
public:
    static std::expected<void, std::string> regenerate_fhs_profile(
        const std::filesystem::path& sysroot = "/",
        const std::vector<Channel>& active_channels = {}) 
    {
        std::filesystem::path profile_dir = sysroot / "etc/sage/profiles/default";
        std::filesystem::path bin_dir = profile_dir / "bin";
        std::filesystem::path lib_dir = profile_dir / "lib";
        std::filesystem::path include_dir = profile_dir / "include";

        std::filesystem::create_directories(bin_dir);
        std::filesystem::create_directories(lib_dir);
        std::filesystem::create_directories(include_dir);

        // Aggregate symlinks from toolchains & runtimes in priority order
        for (const auto& ch : active_channels) {
            if (!ch.enabled || ch.scope == ChannelScope::System) continue;

            auto ch_root = ch.resolve_target_root(sysroot);
            auto ch_bin = ch_root / "bin";
            if (std::filesystem::exists(ch_bin)) {
                for (const auto& entry : std::filesystem::directory_iterator(ch_bin)) {
                    if (entry.is_regular_file() || entry.is_symlink()) {
                        auto dest_link = bin_dir / entry.path().filename();
                        std::error_code ec;
                        std::filesystem::remove(dest_link, ec);
                        std::filesystem::create_symlink(entry.path(), dest_link, ec);
                    }
                }
            }

            auto ch_lib = ch_root / "lib";
            if (std::filesystem::exists(ch_lib)) {
                for (const auto& entry : std::filesystem::directory_iterator(ch_lib)) {
                    if (entry.is_regular_file() || entry.is_symlink()) {
                        auto dest_link = lib_dir / entry.path().filename();
                        std::error_code ec;
                        std::filesystem::remove(dest_link, ec);
                        std::filesystem::create_symlink(entry.path(), dest_link, ec);
                    }
                }
            }
        }

        // Generate /etc/profile.d/sage-channels.sh
        std::filesystem::path profile_d = sysroot / "etc/profile.d";
        std::filesystem::create_directories(profile_d);

        std::filesystem::path sh_path = profile_d / "sage-channels.sh";
        std::ofstream sh(sh_path);
        if (!sh.is_open()) {
            return std::unexpected("Cannot write /etc/profile.d/sage-channels.sh");
        }

        sh << "#!/bin/sh\n";
        sh << "# Auto-generated by Sage Package Manager Channel Runtime\n\n";
        sh << "if [ -d \"/etc/sage/profiles/default/bin\" ]; then\n";
        sh << "    case \":$PATH:\" in\n";
        sh << "        *:/etc/sage/profiles/default/bin:*);;\n";
        sh << "        *) export PATH=\"/etc/sage/profiles/default/bin:$PATH\";;\n";
        sh << "    esac\n";
        sh << "fi\n\n";

        sh << "if [ -d \"/etc/sage/profiles/default/lib\" ]; then\n";
        sh << "    case \":$LD_LIBRARY_PATH:\" in\n";
        sh << "        *:/etc/sage/profiles/default/lib:*);;\n";
        sh << "        *) export LD_LIBRARY_PATH=\"/etc/sage/profiles/default/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}\";;\n";
        sh << "    esac\n";
        sh << "fi\n";

        sh.close();
        std::error_code ec;
        std::filesystem::permissions(sh_path, 
            std::filesystem::perms::owner_all | 
            std::filesystem::perms::group_read | std::filesystem::perms::group_exec | 
            std::filesystem::perms::others_read | std::filesystem::perms::others_exec, 
            ec);

        return {};
    }

    static std::expected<ChannelIndex, std::string> sync_channel(
        const Channel& ch,
        const std::filesystem::path& cache_dir) 
    {
        std::string index_url = ch.url;
        if (!index_url.ends_with('/')) index_url += '/';
        index_url += "index.toml";

        auto content = vendor::curl::fetch_string(index_url);
        if (!content) {
            return std::unexpected("Failed to fetch channel index from " + index_url + ": " + content.error());
        }

        auto index_res = ChannelIndex::parse_toml(*content);
        if (!index_res) return index_res;

        // Cache index locally
        std::filesystem::path local_idx = cache_dir / "channels" / (ch.name + ".toml");
        std::filesystem::create_directories(local_idx.parent_path());
        std::ofstream f(local_idx);
        if (f.is_open()) {
            f << *content;
        }

        return index_res;
    }
};

} // namespace sage::channel
