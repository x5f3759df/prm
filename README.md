# `prm` — Proton Runner Manager

I needed a tool that can manage proton runners all in one place. I don't found one which satisfy my needs, so I vibecoded it itself. Feel free to use, open issues and improve it. Tool should use KISS and YAGNI principles, any bloat will be rejected.

> **Disclaimer**: This tool is 100% AI-generated using Google DeepMind's Antigravity coding agent.

`prm` is a fast, self-contained Rust CLI utility for managing custom Steam Proton runners (`GE-Proton`, `Proton-CachyOS`, `Proton-EM`, `DW-Proton`, `Wine-TKG`, etc.).

All upstream API URLs and source definitions live in a plain-text JSON configuration file (`config/runners.json`)—no URLs are hardcoded in the binary.

---

## Features

- **Zero External Runtime Dependencies**: Pure-Rust HTTPS (`ureq` + `rustls`), checksum verification (`sha2`), and `.tar.gz` / `.tar.xz` / `.tar.zst` archive extraction.
- **CPU Micro-Architecture Auto-Detection**: Reads CPU feature flags (`avx512*`, `avx2`, `bmi2`, `fma`) to automatically pick the best matching build (`x86_64_v4`, `x86_64_v3`, `x86_64_v2`, or `x86_64`), with `--variant` override.
- **Checksum Verification & Idempotent Updates**: Automatically verifies `.sha256sum` and `.sha512sum` release assets before unpacking and persists `.prm-checksum`. If the latest runner is already installed and matches the upstream checksum, `prm install` and `prm update` do nothing.
- **Runner Inspection, Removal & Purging**: Auto-sized tables show disk usage and embedded `dxvk`/`vkd3d-proton`/`nvapi`/`d7vk` versions. Remove specific runners with `prm remove <runner>` or interactively delete all unused runners not referenced in Steam's `config.vdf` with `prm purge`.

---

## Dependencies

### Build Dependencies
- **Rust toolchain**: `rustc` >= 1.85 (Edition 2024) and `cargo`
- **C compiler & pkg-config**: `gcc` or `clang` and `make` (used at compile time by `zstd-sys` and `lzma-sys`)

On Fedora:
```bash
sudo dnf install rust cargo gcc make
```

On Arch Linux / CachyOS:
```bash
sudo pacman -S rust base-devel
```

On Ubuntu / Debian:
```bash
sudo apt install cargo rustc build-essential
```

### Runtime Dependencies
- None (single statically-linked TLS/archive Rust binary + JSON config files).

---

## Building & Installation

### 1. Build Release Binary
```bash
cargo build --release
```
The compiled binary is placed at `./target/release/prm`.

### 2. Install Binary & Configs for Your User
```bash
# Install binary to ~/.cargo/bin/prm (or copy to ~/.local/bin/prm)
cargo install --path .

# Copy default editable config to ~/.config/prm/
mkdir -p ~/.config/prm
cp ./config/runners.json ~/.config/prm/
```

### Config Directory Lookup Order
`prm` searches for `runners.json` in the following order:
1. `$PRM_CONFIG_DIR` environment variable
2. `./config/` in the current working directory
3. `<binary_dir>/config/` or `<repo_root>/config/` (when running directly from `target/release/prm`)
4. `$XDG_CONFIG_HOME/prm` (defaults to `~/.config/prm`)

If the config file is missing or invalid, `prm` exits with an error and prints the required JSON schema template.

---

## Configuration (`runners.json`)

You can add, edit, or remove any upstream source with a text editor:

### `config/runners.json`
```json
{
  "runners": {
    "ge": {
      "aliases": ["ge-proton", "proton-ge"],
      "installed_prefix": "GE-Proton",
      "releases_url": "https://api.github.com/repos/GloriousEggroll/proton-ge-custom/releases"
    }
  }
}
```

> **Tip**: Set `GITHUB_TOKEN=<your_pat>` in your environment if you hit unauthenticated GitHub API rate limits.

---

## Usage & Examples

### 1. Inspect Installed Runners & Remote Releases
```bash
# List installed runners in compatibilitytools.d (size and DXVK/VKD3D/NVAPI/D7VK versions)
prm list

# List configured remote sources from runners.json
prm list --remote

# List recent remote releases from a configured source
prm list --remote ge --limit 5
prm list --remote cachyos --limit 5
prm list --remote dw --limit 5
```

### 2. Install & Update Proton Runners
```bash
# Install latest GE-Proton
prm install ge

# Install a specific version tag
prm install ge@GE-Proton11-12

# Install latest Proton-CachyOS (auto-selects x86_64_v4 / x86_64_v3 / x86_64 based on your CPU)
prm install cachyos

# Force a specific CPU micro-architecture variant
prm install cachyos --variant v3

# Check all installed runner families against upstream and install newer releases
prm update

# Update only a specific runner family
prm update ge
```

### 3. Remove or Purge Installed Runners
```bash
# Remove specific installed runner(s) by name (as shown in `prm list`)
prm remove GE-Proton9-27

# Interactively remove all unused runners (any runner not selected as Steam default or pinned to a game in Steam's config.vdf)
prm purge
```

> **Note**: When you install or update a runner, select the new runner in Steam (`Settings -> Compatibility` or per-game `Properties -> Compatibility`) before running `prm purge` so Steam records it in `config.vdf`.

---

## Limitations

- **Native Steam Paths Only**: Looks for Steam at `~/.local/share/Steam` or `~/.steam/root`. Flatpak/Snap Steam installations and `STEAM_EXTRA_COMPAT_TOOLS_PATHS` are not supported.
- **GitHub/Gitea Release API Format**: Sources in `runners.json` must use a GitHub-compatible `/releases` JSON endpoint publishing `.tar.gz`, `.tgz`, `.tar.xz`, or `.tar.zst` archives (`x86_64`).
- **Read-Only Steam Config**: `prm` never writes to Steam's `config.vdf`. Assign runners to games inside Steam; `prm purge` only reads `config.vdf` to identify unused runners.
