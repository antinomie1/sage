# AGENTS.md - Developer & AI Contributor Guide for Sage

Welcome to the **Sage** project! This document establishes the engineering rules, architectural principles, code conventions, and workflows for all human and AI agent contributors.

---

## 1. Project Overview & Philosophy

**Sage** is a modern, blazingly fast, modular, multi-layer universal Linux package manager written from scratch in **Modern C++20**.

### Key Architectural Pillars:
* **Universal Multi-Layer Channel System**: Manages system root (`/`), shared runtimes (`/usr/lib/runtimes`), toolchains (`/opt/channels`), and user-level packages (`~/.local`) with strict FHS compliance via profile symlink aggregation.
* **Minimal Core Virtual Providers**: Strictly scopes virtual interfaces to fundamental, mutually exclusive system components: `virtual/init`, `virtual/udev`, `virtual/libc`. Kernels, shells, awks, and utilities are pure, independent, coexisting packages.
* **Declarative Reconcile Engine (`sage rebuild`)**: Reads `/etc/distro/system.toml`, calculates state diffs in LMDB, performs atomic package swaps, and auto-regenerates native service scripts for the active init system.
* **Universal Service Specification (`service.toml`)**: Decouples services from any single init daemon, auto-compiling into OpenRC, Runit, Systemd, Dinit, and s6 configurations.
* **Zero-Copy ACID State Storage (LMDB)**: Ultra-fast memory-mapped B+ tree database with nanosecond reads and Copy-on-Write transaction safety.
* **Native C++20 Streaming Archive Engine**: Self-contained streaming Tar reader/writer directly compressed with `libzstd` (no `libarchive` bloat).
* **Native C++20 PubGrub / CDCL SAT Solver**: Mathematically complete dependency resolution with world-class human-readable conflict diagnostic cause trees.
* **Dynamic Linking by Default**: Dynamically links against system shared libraries (`liblmdb`, `libzstd`, `libtomlplusplus`, `libc`), with automated ELF `DT_NEEDED` and `DT_SONAME` scanning.

---

## 2. The 5 Invariable Engineering Rules

All code contributions MUST strictly adhere to these 5 rules:

### Rule 1: Strict Memory Safety & 100% RAII
* **Zero naked owning pointers**: Manual `new` and `delete` are strictly forbidden.
* **Automatic Resource Cleanup**: All LMDB environments (`MDB_env*`), transactions (`MDB_txn*`), cursors (`MDB_cursor*`), file descriptors, and Zstandard contexts (`ZSTD_DCtx*`, `ZSTD_CCtx*`) must be wrapped in RAII structures that automatically clean up upon scope exit or exceptions.
* **Boundary-safe Views**: Prefer `std::string_view` and `std::span` for zero-copy read operations. Always respect lifetime bounds.

### Rule 2: Clean, Small & Beautiful, High Performance
* **Total Project LOC < 10,000**: Aim for maximum conciseness and code density (~2,500 - 3,500 lines total).
* **Data-Oriented & Value Semantics**: Avoid deep inheritance hierarchies, abstract factory bloat, or excessive enterprise patterns. Keep structs simple, transparent, and moveable.
* **Modern Standard Library Features**: Use `std::expected<T, Error>` for zero-cost monadic error handling, `std::format` / `std::print` for type-safe fast formatting, and `std::ranges` for functional operations.

### Rule 3: DRY (Don't Repeat Yourself) with Zero-Cost Abstractions
* Common utilities (path normalization, binary serialization, ELF SONAME extraction, string manipulation, formatting) must be written once in `sage.util` and reused everywhere.
* Use `constexpr`, `inline`, and templates to ensure code reuse introduces zero runtime overhead.

### Rule 4: 100% C++20 Module System (Zero Headers in Business Logic)
* All application code, domain models, and engines must be implemented as `.cppm` C++20 module interface files.
* Third-party C/C++ library headers (`lmdb.h`, `zstd.h`, `toml++/toml.hpp`) are isolated exclusively within `src/vendor/` module partitions using Global Module Fragments (`module; #include <...>; export module ...;`).
* Application code and the CLI entry point must NEVER `#include` third-party headers; use `import sage.*;` or `import sage;`.

### Rule 5: Orthogonal Modular Architecture
* Maintain strict unidirectional, acyclic module dependencies:
  `vendor` -> `util` -> `config/package/service` -> `channel/archive/db` -> `solver` -> `rebuild` -> `sage` (root module) -> `cli`.

---

## 3. Directory & Module Structure

```
sage/
├── xmake.lua                     # xmake build script
├── AGENTS.md                     # Agent & Developer instructions (this file)
├── README.md                     # Project overview and quick start
├── docs/
│   ├── ARCHITECTURE.md           # Full architectural specification
│   ├── MODULES.md                # Module reference and dependency DAG
│   └── CLI_SPEC.md               # Complete CLI command-line reference
├── src/
│   ├── vendor/                   # 3rd-party library RAII bridge modules
│   │   ├── sage.vendor.lmdb.cppm # LMDB RAII wrapper (zero-copy B+ tree)
│   │   ├── sage.vendor.zstd.cppm # Zstandard streaming RAII wrapper
│   │   └── sage.vendor.toml.cppm # tomlplusplus C++20 module bridge
│   ├── core/                     # Core domain logic modules
│   │   ├── sage.util.cppm        # Common utilities, ELF SONAME scanner, paths, formatting
│   │   ├── sage.config.cppm      # /etc/distro/system.toml & provider configuration
│   │   ├── sage.package.cppm     # Package domain model, recipe.toml, manifest.toml, triggers
│   │   ├── sage.channel.cppm     # Multi-layer Channel runtime & FHS Profile aggregator
│   │   ├── sage.service.cppm     # Universal service generator (OpenRC/Runit/Systemd/Dinit/s6)
│   │   ├── sage.db.cppm          # LMDB package registry, file ownership & transaction engine
│   │   ├── sage.archive.cppm     # Native C++20 streaming Tar + Zstd extractor & packager
│   │   ├── sage.solver.cppm      # Native PubGrub / CDCL SAT dependency solver
│   │   └── sage.rebuild.cppm     # System reconcile & atomic rebuild orchestration
│   ├── sage.cppm                 # Primary root module aggregating and re-exporting all modules
│   └── cli/
│       └── main.cpp              # CLI entry point (pure `import sage;`)
└── tests/                        # Unit tests and end-to-end integration tests
```

---

## 4. Build & Development Workflow with xmake

```bash
# Configure debug / release build
xmake f -m debug
xmake f -m release

# Build sage package manager
xmake

# Run the compiled binary
xmake run sage --help

# Run all test suites
xmake test
```

---

## 5. Summary Checklist Before Any Contribution

- [ ] Does the code use C++20 modules (`.cppm`) without legacy `#include` leaking into business logic?
- [ ] Is all memory and OS resource management 100% RAII-compliant with zero manual memory management?
- [ ] Are third-party dependencies limited exclusively to dynamic `liblmdb`, `libzstd`, and `libtomlplusplus`?
- [ ] Is error handling monadic using `std::expected` / `std::optional` where applicable?
- [ ] Does the change keep the overall codebase clean, readable, and well under the 10,000 LOC limit?
