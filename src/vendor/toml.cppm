module;

#define TOML_EXCEPTIONS 0
#include <toml++/toml.hpp>

export module sage.vendor.toml;

import std;

export namespace sage::vendor::toml {

using table = ::toml::table;
using array = ::toml::array;
using value = ::toml::value<int64_t>;
using node = ::toml::node;
using node_view = ::toml::node_view<::toml::node>;

inline std::expected<::toml::table, std::string> parse_string(std::string_view source, std::string_view source_path = {}) {
    auto res = ::toml::parse(source, source_path);
    if (!res) {
        return std::unexpected(std::string(res.error().description()));
    }
    return std::move(res).table();
}

inline std::expected<::toml::table, std::string> parse_file(const std::filesystem::path& path) {
    auto res = ::toml::parse_file(path.string());
    if (!res) {
        return std::unexpected(std::string(res.error().description()));
    }
    return std::move(res).table();
}

} // namespace sage::vendor::toml
