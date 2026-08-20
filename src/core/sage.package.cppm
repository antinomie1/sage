export module sage.package;

import std;
import sage.vendor.toml;
import sage.util;

export namespace sage::package {

using std::uint8_t;
using std::uint32_t;
using std::uint64_t;
using std::size_t;

// ============================================================================
// Version Model with standard epoch-ver-rel ordering
// ============================================================================

struct Version {
    uint32_t epoch{0};
    std::string ver;
    std::string rel{"1"};

    static Version parse(std::string_view s) {
        Version v;
        if (s.empty()) return v;

        // Check for epoch (e.g. "1:2.0.0-1")
        if (auto colon = s.find(':'); colon != std::string_view::npos) {
            uint32_t ep = 0;
            for (char c : s.substr(0, colon)) {
                if (std::isdigit(static_cast<unsigned char>(c))) ep = ep * 10 + (c - '0');
            }
            v.epoch = ep;
            s = s.substr(colon + 1);
        }

        // Check for release (e.g. "2.0.0-1")
        if (auto dash = s.rfind('-'); dash != std::string_view::npos) {
            v.ver = std::string(s.substr(0, dash));
            v.rel = std::string(s.substr(dash + 1));
        } else {
            v.ver = std::string(s);
            v.rel = "1";
        }
        return v;
    }

    [[nodiscard]] std::string to_string() const {
        if (epoch > 0) {
            return std::format("{}:{}-{}", epoch, ver, rel);
        }
        return std::format("{}-{}", ver, rel);
    }

    // Alphanumeric segment comparator (vercmp)
    static int compare_segments(std::string_view a, std::string_view b) noexcept {
        size_t i = 0, j = 0;
        while (i < a.size() || j < b.size()) {
            while (i < a.size() && !std::isalnum(static_cast<unsigned char>(a[i]))) ++i;
            while (j < b.size() && !std::isalnum(static_cast<unsigned char>(b[j]))) ++j;
            if (i >= a.size() || j >= b.size()) {
                if (i >= a.size() && j >= b.size()) return 0;
                return (i >= a.size()) ? -1 : 1;
            }

            bool a_digit = std::isdigit(static_cast<unsigned char>(a[i]));
            bool b_digit = std::isdigit(static_cast<unsigned char>(b[j]));

            if (a_digit && b_digit) {
                // Numeric comparison
                size_t start_i = i, start_j = j;
                while (i < a.size() && std::isdigit(static_cast<unsigned char>(a[i]))) ++i;
                while (j < b.size() && std::isdigit(static_cast<unsigned char>(b[j]))) ++j;

                std::string_view sa = a.substr(start_i, i - start_i);
                std::string_view sb = b.substr(start_j, j - start_j);
                // Strip leading zeros
                while (sa.size() > 1 && sa.front() == '0') sa.remove_prefix(1);
                while (sb.size() > 1 && sb.front() == '0') sb.remove_prefix(1);

                if (sa.size() != sb.size()) {
                    return sa.size() < sb.size() ? -1 : 1;
                }
                if (sa != sb) {
                    return sa < sb ? -1 : 1;
                }
            } else if (!a_digit && !b_digit) {
                // Alpha segment comparison
                size_t start_i = i, start_j = j;
                while (i < a.size() && std::isalpha(static_cast<unsigned char>(a[i]))) ++i;
                while (j < b.size() && std::isalpha(static_cast<unsigned char>(b[j]))) ++j;

                std::string_view sa = a.substr(start_i, i - start_i);
                std::string_view sb = b.substr(start_j, j - start_j);
                if (sa != sb) {
                    return sa < sb ? -1 : 1;
                }
            } else {
                return a_digit ? 1 : -1;
            }
        }
        return 0;
    }

    std::strong_ordering operator<=>(const Version& other) const noexcept {
        if (epoch != other.epoch) {
            return epoch <=> other.epoch;
        }
        int vc = compare_segments(ver, other.ver);
        if (vc != 0) {
            return vc < 0 ? std::strong_ordering::less : std::strong_ordering::greater;
        }
        int rc = compare_segments(rel, other.rel);
        if (rc != 0) {
            return rc < 0 ? std::strong_ordering::less : std::strong_ordering::greater;
        }
        return std::strong_ordering::equal;
    }

    bool operator==(const Version& other) const noexcept {
        return (*this <=> other) == std::strong_ordering::equal;
    }
};

// ============================================================================
// Dependency Model
// ============================================================================

enum class ConstraintOp {
    Any,
    Equal,
    NotEqual,
    GreaterEqual,
    LessEqual,
    Greater,
    Less
};

struct Dependency {
    std::string name;
    ConstraintOp op{ConstraintOp::Any};
    Version version;

    static Dependency parse(std::string_view s) {
        s = util::trim(s);
        Dependency d;
        if (s.empty()) return d;

        size_t op_pos = std::string_view::npos;
        std::string_view op_str;

        static const std::pair<std::string_view, ConstraintOp> ops[] = {
            {">=", ConstraintOp::GreaterEqual},
            {"<=", ConstraintOp::LessEqual},
            {"!=", ConstraintOp::NotEqual},
            {"==", ConstraintOp::Equal},
            {"=",  ConstraintOp::Equal},
            {">",  ConstraintOp::Greater},
            {"<",  ConstraintOp::Less}
        };

        for (const auto& [str, op] : ops) {
            if (auto pos = s.find(str); pos != std::string_view::npos) {
                op_pos = pos;
                op_str = str;
                d.op = op;
                break;
            }
        }

        if (op_pos == std::string_view::npos) {
            d.name = std::string(s);
            d.op = ConstraintOp::Any;
        } else {
            d.name = std::string(util::trim(s.substr(0, op_pos)));
            d.version = Version::parse(util::trim(s.substr(op_pos + op_str.size())));
        }
        return d;
    }

    [[nodiscard]] bool satisfies(const Version& target_ver) const noexcept {
        if (op == ConstraintOp::Any) return true;
        auto cmp = target_ver <=> version;
        switch (op) {
            case ConstraintOp::Equal:        return cmp == 0;
            case ConstraintOp::NotEqual:     return cmp != 0;
            case ConstraintOp::GreaterEqual: return cmp >= 0;
            case ConstraintOp::LessEqual:    return cmp <= 0;
            case ConstraintOp::Greater:      return cmp > 0;
            case ConstraintOp::Less:         return cmp < 0;
            default:                         return true;
        }
    }

    [[nodiscard]] std::string to_string() const {
        if (op == ConstraintOp::Any) return name;
        std::string_view op_sym = "=";
        switch (op) {
            case ConstraintOp::GreaterEqual: op_sym = ">="; break;
            case ConstraintOp::LessEqual:    op_sym = "<="; break;
            case ConstraintOp::NotEqual:     op_sym = "!="; break;
            case ConstraintOp::Greater:      op_sym = ">"; break;
            case ConstraintOp::Less:         op_sym = "<"; break;
            default: op_sym = "="; break;
        }
        return std::format("{} {} {}", name, op_sym, version.to_string());
    }
};

// ============================================================================
// File Entry in Package Manifest
// ============================================================================

enum class FileType {
    Regular,
    Directory,
    Symlink
};

struct FileEntry {
    std::string path; // Clean relative path (e.g. "usr/bin/rg")
    uint64_t size{0};
    uint32_t mode{0644};
    std::string sha256;
    FileType type{FileType::Regular};
    std::string link_target;
};

// ============================================================================
// Package Manifest Model
// ============================================================================

struct PackageManifest {
    uint32_t schema_version{1};
    std::string name;
    Version version;
    std::string description;
    std::string license;
    std::string channel{"system"};
    std::string arch{"x86_64"};
    uint64_t installed_size{0};

    std::vector<Dependency> dependencies;
    std::vector<std::string> provides; // e.g. "virtual/init", "so:libz.so.1"
    std::vector<Dependency> conflicts;
    std::vector<FileEntry> files;

    static std::expected<PackageManifest, std::string> parse_toml(std::string_view toml_content) {
        auto tbl_res = vendor::toml::parse_string(toml_content);
        if (!tbl_res) return std::unexpected(tbl_res.error());
        const auto& tbl = *tbl_res;

        PackageManifest m;
        m.schema_version = static_cast<uint32_t>(tbl.get("schema_version")->value_or(1LL));

        if (auto* pkg = tbl.get_as<vendor::toml::table>("package")) {
            m.name = pkg->get("name")->value_or("");
            std::string ver_str = pkg->get("version")->value_or("");
            m.version = Version::parse(ver_str);
            if (auto* rel = pkg->get("release")) {
                m.version.rel = rel->value_or("1");
            }
            m.description = pkg->get("description")->value_or("");
            m.license = pkg->get("license")->value_or("");
            m.channel = pkg->get("channel")->value_or("system");
            m.arch = pkg->get("arch")->value_or("x86_64");
            m.installed_size = pkg->get("installed_size")->value_or(0ULL);
        } else {
            return std::unexpected("Missing [package] section in manifest");
        }

        if (m.name.empty()) return std::unexpected("Package name cannot be empty");

        auto parse_deps = [&](const vendor::toml::table& t, const char* key, std::vector<Dependency>& target) {
            if (auto* arr = t.get_as<vendor::toml::array>(key)) {
                for (auto&& d : *arr) {
                    if (auto str = d.value<std::string_view>()) {
                        target.push_back(Dependency::parse(*str));
                    }
                }
            }
        };

        auto parse_strings = [&](const vendor::toml::table& t, const char* key, std::vector<std::string>& target) {
            if (auto* arr = t.get_as<vendor::toml::array>(key)) {
                for (auto&& p : *arr) {
                    if (auto str = p.value<std::string_view>()) {
                        target.emplace_back(*str);
                    }
                }
            }
        };

        // Parse dependencies (root or package section)
        parse_deps(tbl, "dependencies", m.dependencies);
        if (auto* pkg = tbl.get_as<vendor::toml::table>("package")) {
            parse_deps(*pkg, "dependencies", m.dependencies);
        }

        // Parse provides
        parse_strings(tbl, "provides", m.provides);
        if (auto* pkg = tbl.get_as<vendor::toml::table>("package")) {
            parse_strings(*pkg, "provides", m.provides);
        }
        if (auto* src = tbl.get_as<vendor::toml::table>("source")) {
            parse_strings(*src, "provides", m.provides);
        }

        // Parse conflicts
        parse_deps(tbl, "conflicts", m.conflicts);
        if (auto* pkg = tbl.get_as<vendor::toml::table>("package")) {
            parse_deps(*pkg, "conflicts", m.conflicts);
        }
        if (auto* src = tbl.get_as<vendor::toml::table>("source")) {
            parse_deps(*src, "conflicts", m.conflicts);
        }

        // Parse files
        if (auto* fls = tbl.get_as<vendor::toml::array>("files")) {
            for (auto&& f : *fls) {
                if (auto str = f.value<std::string_view>()) {
                    FileEntry fe;
                    fe.path = std::string(*str);
                    m.files.push_back(std::move(fe));
                }
            }
        }
        if (auto* pkg = tbl.get_as<vendor::toml::table>("package")) {
            if (auto* fls = pkg->get_as<vendor::toml::array>("files")) {
                for (auto&& f : *fls) {
                    if (auto str = f.value<std::string_view>()) {
                        FileEntry fe;
                        fe.path = std::string(*str);
                        m.files.push_back(std::move(fe));
                    }
                }
            }
        }

        return m;
    }

    [[nodiscard]] std::string serialize_toml() const {
        std::ostringstream ss;
        ss << "schema_version = " << schema_version << "\n\n";
        ss << "[package]\n";
        ss << "name = \"" << name << "\"\n";
        ss << "version = \"" << version.ver << "\"\n";
        ss << "release = \"" << version.rel << "\"\n";
        if (version.epoch > 0) ss << "epoch = " << version.epoch << "\n";
        ss << "description = \"" << description << "\"\n";
        ss << "license = \"" << license << "\"\n";
        ss << "channel = \"" << channel << "\"\n";
        ss << "arch = \"" << arch << "\"\n";
        ss << "installed_size = " << installed_size << "\n\n";

        ss << "dependencies = [\n";
        for (const auto& d : dependencies) {
            ss << "    \"" << d.to_string() << "\",\n";
        }
        ss << "]\n\n";

        ss << "provides = [\n";
        for (const auto& p : provides) {
            ss << "    \"" << p << "\",\n";
        }
        ss << "]\n\n";

        ss << "conflicts = [\n";
        for (const auto& c : conflicts) {
            ss << "    \"" << c.to_string() << "\",\n";
        }
        ss << "]\n\n";

        if (!files.empty()) {
            ss << "files = [\n";
            for (const auto& f : files) {
                ss << "    \"" << f.path << "\",\n";
            }
            ss << "]\n";
        }

        return ss.str();
    }
};

// ============================================================================
// Recipe Model for Package Building (`recipe.toml`)
// ============================================================================

struct Recipe {
    uint32_t schema_version{1};
    std::string name;
    Version version;
    std::string description;
    std::string license;
    std::string channel{"system"};
    std::string source_url;
    std::string source_sha256;
    std::vector<std::string> build_deps;
    std::vector<Dependency> host_deps;
    std::vector<std::string> provides;
    std::vector<std::string> prepare_cmds;
    std::vector<std::string> build_cmds;
    std::vector<std::string> install_cmds;

    static std::expected<Recipe, std::string> parse_toml(std::string_view toml_content) {
        auto tbl_res = vendor::toml::parse_string(toml_content);
        if (!tbl_res) return std::unexpected(tbl_res.error());
        const auto& tbl = *tbl_res;

        Recipe r;
        r.schema_version = static_cast<uint32_t>(tbl.get("schema_version")->value_or(1LL));

        if (auto* pkg = tbl.get_as<vendor::toml::table>("package")) {
            r.name = pkg->get("name")->value_or("");
            r.version = Version::parse(pkg->get("version")->value_or(""));
            if (auto* rel = pkg->get("release")) {
                r.version.rel = rel->value_or("1");
            }
            r.description = pkg->get("description")->value_or("");
            r.license = pkg->get("license")->value_or("");
            r.channel = pkg->get("channel")->value_or("system");
        } else {
            return std::unexpected("Missing [package] section in recipe");
        }

        if (auto* src = tbl.get_as<vendor::toml::table>("source")) {
            r.source_url = src->get("url")->value_or("");
            r.source_sha256 = src->get("sha256")->value_or("");
        }

        auto parse_deps = [&](const vendor::toml::table& t, const char* key, std::vector<Dependency>& target) {
            if (auto* arr = t.get_as<vendor::toml::array>(key)) {
                for (auto&& d : *arr) {
                    if (auto str = d.value<std::string_view>()) {
                        target.push_back(Dependency::parse(*str));
                    }
                }
            }
        };

        auto parse_strings = [&](const vendor::toml::table& t, const char* key, std::vector<std::string>& target) {
            if (auto* arr = t.get_as<vendor::toml::array>(key)) {
                for (auto&& p : *arr) {
                    if (auto str = p.value<std::string_view>()) {
                        target.emplace_back(*str);
                    }
                }
            }
        };

        parse_deps(tbl, "dependencies", r.host_deps);
        parse_strings(tbl, "build_dependencies", r.build_deps);
        parse_strings(tbl, "provides", r.provides);

        if (auto* pkg = tbl.get_as<vendor::toml::table>("package")) {
            parse_deps(*pkg, "dependencies", r.host_deps);
            parse_strings(*pkg, "build_dependencies", r.build_deps);
            parse_strings(*pkg, "provides", r.provides);
        }

        if (auto* src = tbl.get_as<vendor::toml::table>("source")) {
            parse_deps(*src, "dependencies", r.host_deps);
            parse_strings(*src, "build_dependencies", r.build_deps);
            parse_strings(*src, "provides", r.provides);
        }

        auto extract_cmds = [&](const char* key, std::vector<std::string>& dest) {
            if (auto* arr = tbl.get_as<vendor::toml::array>(key)) {
                for (auto&& c : *arr) {
                    if (auto str = c.value<std::string_view>()) {
                        dest.emplace_back(*str);
                    }
                }
            }
            if (auto* pkg = tbl.get_as<vendor::toml::table>("package")) {
                if (auto* arr = pkg->get_as<vendor::toml::array>(key)) {
                    for (auto&& c : *arr) {
                        if (auto str = c.value<std::string_view>()) {
                            dest.emplace_back(*str);
                        }
                    }
                }
            }
            if (auto* src = tbl.get_as<vendor::toml::table>("source")) {
                if (auto* arr = src->get_as<vendor::toml::array>(key)) {
                    for (auto&& c : *arr) {
                        if (auto str = c.value<std::string_view>()) {
                            dest.emplace_back(*str);
                        }
                    }
                }
            }
        };

        extract_cmds("prepare", r.prepare_cmds);
        extract_cmds("build", r.build_cmds);
        extract_cmds("install", r.install_cmds);

        return r;
    }
};

} // namespace sage::package
