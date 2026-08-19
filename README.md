# 🌿 Sage Package Manager

**Sage** is a high-performance, modular, multi-layer universal Linux package manager written from scratch in **Modern C++20**.

Designed for absolute user control, radical simplicity, and microsecond-level execution speed, Sage introduces a unified **Channel-based architecture**, full **FHS compliance**, **declarative system reconciliation**, and a **pure C++20 module-first design**.

---

## ✨ Key Features

* **⚡ Ultra-High Performance & Zero-Copy**:
  - Powered by **LMDB** (Memory-Mapped B+ Tree) for nanosecond package queries and Copy-on-Write ACID transactional safety.
  - Native **C++20 streaming archive engine** over **libzstd**, eliminating heavy legacy tar libraries.
* **🌐 Universal Multi-Layer Channel System**:
  - Manages multiple OS layers seamlessly: System root (`/`), shared runtimes (`/usr/lib/runtimes`), toolchains (`/opt/channels`), and user-level apps (`~/.local`).
  - Adheres strictly to **FHS (Filesystem Hierarchy Standard)** via declarative profile symlinks and environment hooks.
* **🎛️ Absolute System Sovereignty & Minimal Virtual Providers**:
  - Core system structural components (`virtual/init`, `virtual/udev`, `virtual/libc`) are fully swappable.
  - Natural coexisting components (Linux Kernels, Shells, Awk, Core utilities) are managed as pure, independent packages.
* **🔄 Declarative System Reconcile (`sage rebuild`)**:
  - Automatically compares `/etc/distro/system.toml` against active LMDB state.
  - Performs atomic package swaps (e.g., swapping `systemd` with `openrc` + `eudev`).
  - Automatically re-generates native service configurations for all installed daemons.
* **🔌 Universal Service Specification (`service.toml`)**:
  - Package daemons are declared with a single init-agnostic spec, auto-compiled into **OpenRC**, **Runit**, **Systemd**, **Dinit**, or **s6** scripts.
* **🧩 Native C++20 PubGrub / CDCL SAT Dependency Solver**:
  - Zero external solver dependencies.
  - Generates clear, human-readable conflict diagnostic cause trees.
* **🛡️ 100% C++20 Modules & RAII Memory Safety**:
  - Fully modular `.cppm` architecture with zero header pollution.
  - Dynamic linking against system shared libraries (`liblmdb`, `libzstd`, `libtomlplusplus`).

---

## 🏗️ Architecture at a Glance

```
sage
 ├── 状态引擎: LMDB (零拷贝 mmap B+ 树，微秒级读写，Copy-on-Write ACID 事务)
 ├── 归档引擎: libzstd + 原生 C++20 流式 Tar 解包与打包器 (无 libarchive 依赖)
 ├── 求解引擎: 自研 C++20 PubGrub / CDCL SAT 依赖求解器 (顶级因果树报错)
 ├── 服务体系: 通用 service.toml -> OpenRC / Runit / Systemd / Dinit / s6 自动生成
 ├── 抽象收敛: 精简虚拟提供者 virtual/init, virtual/udev, virtual/libc
 └── 系统重构: sage rebuild 自动基于 system.toml 执行原子大件迁移与服务重构
```

---

## 🚀 Building & Running

### Requirements
* **xmake** (>= 2.8.0)
* **GCC** (>= 11.0 with C++20 modules) or **Clang** (>= 14.0)
* Dynamic system libraries: `liblmdb`, `libzstd`, `libtomlplusplus`

### Build with xmake
```bash
# Build sage in release mode
xmake f -m release
xmake

# Run the compiled binary
xmake run sage --help
```

---

## 📖 Documentation
* [Architecture Specification](docs/ARCHITECTURE.md)
* [Module Reference & Dependency Topology](docs/MODULES.md)
* [CLI Command Specification](docs/CLI_SPEC.md)
* [Developer & AI Contributor Guide](AGENTS.md)
