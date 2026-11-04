# `prm` — Proton Runner Manager

I needed a tool to manage Proton runners all in one place. I couldn't find one that satisfied my needs, so I vibecoded it myself. Feel free to use it, open issues, and improve it. This tool strictly follows KISS and YAGNI principles—any bloat will be rejected.

## Install

```bash
curl -fsSL https://raw.githubusercontent.com/x5f3759df/prm/main/install.sh | sh
```
Installs `prm` to `~/.local/bin/prm` and default config to `~/.config/prm/runners.json`.

<details>
<summary>Build from source (requires Rust >= 1.85)</summary>

```bash
cargo install --path .
mkdir -p ~/.config/prm && cp config/runners.json ~/.config/prm/
```
</details>

## Usage

```bash
prm list                        # Installed runners, sizes, DXVK/VKD3D/NVAPI/D7VK versions
prm list --remote               # Configured sources (ge, cachyos, em, dw, tkg)
prm list --remote ge --limit 5  # Latest upstream releases for a source
prm install ge                  # Install latest (or prm install ge@GE-Proton11-12)
prm update                      # Update all installed runner families (or prm update ge)
prm remove GE-Proton9-27        # Remove specific runner
prm purge                       # Remove unused runners not in Steam's config.vdf
```

> **Note**: Select a newly installed runner in Steam (`Settings -> Compatibility` or per-game `Properties -> Compatibility`) before running `prm purge` so Steam records it in `config.vdf`.

## Configuration (`~/.config/prm/runners.json`)

Default sources: `ge` (GE-Proton), `cachyos` (Proton-CachyOS), `em` (Proton-EM), `dw` (DW-Proton), `tkg` (Wine-TKG). Add or edit sources in `~/.config/prm/runners.json`:

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

Set `GITHUB_TOKEN=<pat>` if you hit unauthenticated GitHub API rate limits.

## Limitations

- **Native Steam paths only**: Checks `~/.local/share/Steam` and `~/.steam/root` (no Flatpak/Snap or `STEAM_EXTRA_COMPAT_TOOLS_PATHS`).
- **GitHub/Gitea release API**: Sources must expose a `/releases` JSON endpoint with `x86_64` `.tar.{gz,tgz,xz,zst}` archives.
- **Read-only Steam config**: Never writes to `config.vdf`; assign runners inside Steam.
