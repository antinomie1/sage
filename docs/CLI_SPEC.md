# Sage CLI Command Specification

The `sage` command-line interface provides clean, high-performance commands for package management, system reconciliation, channel routing, and packaging.

---

## 1. Global Options

```
sage [OPTIONS] <COMMAND> [ARGS...]

Global Options:
  --verbose, -v        Enable detailed verbose diagnostic output
  --quiet, -q          Suppress informational output
  --help, -h           Show help message
  --version, -V        Display Sage version
```

---

## 2. Command Reference

### `sage install <PKG...>`
Resolves dependencies via PubGrub SAT solver, unpacks `*.pkg.tar.zst` streams to target channel scope, writes LMDB state records, and executes triggers.
Archive writes are anchored to the target root without following parent symlinks. If the installed identity changes concurrently after dependency resolution, the command exits without applying the stale package migration.
```bash
# Install packages into system channel (root)
sage install ripgrep neovim

# Install into specific channel (e.g. isolated toolchain)
sage install --channel python312 python

# Dry run / preview transaction without modifying filesystem
sage install --dry-run waybar
```

### `sage remove <PKG...>`
Removes installed package files, unregisters LMDB records, and removes generated service scripts.
The complete installed-package snapshot is checked again under the LMDB writer lock before any files are removed; a concurrent package change aborts the stale removal plan.
```bash
sage remove nginx
```

### `sage rebuild`
**Declarative System Reconcile**: Compares `/etc/sage/system.toml` against active LMDB state. Performs guarded swaps of exclusive virtual providers (`virtual/init`, `virtual/udev`) and automatically re-generates all native daemon service scripts. Provider locks and packages scheduled for removal are revalidated inside the LMDB write transaction before the plan is applied.
```bash
# Preview what rebuild would change
sage rebuild --dry-run

# Execute system reconciliation
sage rebuild
```

### `sage channel [list|add|remove|sync]`
Manages multi-layer Channel sources, scopes, and priorities.
```bash
# List active channels and their scopes
sage channel list

# Add a remote channel repository
sage channel add core https://pkg.sage-linux.org/core --scope system --priority 100
sage channel add rust-nightly https://pkg.sage-linux.org/rust --scope toolchain --priority 50

# Sync channel metadata indexes
sage channel sync
```

### `sage build <RECIPE_DIR>`
Builds a `.pkg.tar.zst` binary package from a `recipe.toml` definition, automatically extracting ELF `DT_NEEDED` and `DT_SONAME` symbols.
```bash
# Build package archive in current directory
sage build ./recipes/ripgrep
```

### `sage query [installed|info|files|owner]`
Queries package information, files manifest, and file ownership in nanoseconds via LMDB mmap.
```bash
# List all installed packages
sage query installed

# Show details of a package
sage query info ripgrep

# List all files owned by a package
sage query files ripgrep

# Find which package owns a specific file path
sage query owner /usr/bin/rg
```

### `sage service [list|status|generate]`
Inspects daemon definitions and manually re-generates service scripts.
```bash
# List all installed services and their active init mapping
sage service list

# Re-generate service configuration for current active init
sage service generate sshd
```
