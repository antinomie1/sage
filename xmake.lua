-- Sage Package Manager build configuration
-- Build system: xmake (https://xmake.io)
-- Standards: Modern C++23 with full C++23 Modules (.cppm)
-- Linking: Fully dynamically linked to system shared libraries

set_project("sage")
set_version("0.1.0")
set_license("BSD-2-Clause")
set_description("Sage: High-performance, modular, multi-layer universal Linux package manager")

set_languages("c++23")
set_warnings("all", "extra")

-- Enable optimization for release mode
if is_mode("release") then
    set_optimize("fastest")
    set_strip("all")
elseif is_mode("debug") then
    set_symbols("debug")
    set_optimize("none")
end

-- Use system shared libraries dynamically
add_requires("system::lmdb", {system = true})
add_requires("system::zstd", {system = true})
add_requires("system::tomlplusplus", {system = true})
add_requires("system::curl", {system = true})

-- Sage CLI executable target
target("sage")
    set_kind("binary")
    add_files("src/vendor/**.cppm")
    add_files("src/core/**.cppm")
    add_files("src/sage.cppm")
    add_files("src/cli/main.cpp")
    add_packages("system::lmdb", "system::zstd", "system::tomlplusplus", "system::curl")
    set_default(true)
