# AGENTS.md — `prm` (Proton Runner Manager)

## Core Philosophy (Strict KISS & YAGNI)
- **Minimalism first**: Shortest working diff wins. Deletion over addition. Standard library or existing crates before any new dependency.
- **Atomic CLI commands**: Each command (`list`, `install`, `update`, `remove`, `purge`) does one thing and exits.
- **No unsolicited output**: Never print post-command tips, workflow suggestions, or decorative banners.

## Hard Non-Goals (Reject Immediately)
- **No GUI / TUI**: Plain stdout/stderr tables and prompts only.
- **No async runtime**: Synchronous I/O (`ureq` + `rustls`) only. Never add `tokio`, `reqwest`, or OpenSSL.
- **No Steam config mutation**: Never write to Steam's `config.vdf` or `.acf` files. `config.vdf` is read-only (used by `prm purge` to detect used runners).
- **No DLL injection / prefix patching**: `prm` manages whole Proton runners in `compatibilitytools.d` only. Component columns (`DXVK`, `VKD3D-PROTON`, `NVAPI`, `D7VK`) in `prm list` are strictly read-only.
- **No hardcoded upstream URLs**: All runner release endpoints live in `config/runners.json`.

## Codebase Layout
- `src/main.rs` — CLI definition (`clap` derive) and command dispatch.
- `src/runner.rs` — Subcommand handlers (`cmd_list`, `cmd_install`, `cmd_update`, `cmd_remove`, `cmd_purge`).
- `src/sources.rs` — `runners.json` loading and GitHub/Gitea release API resolution.
- `src/archive.rs` — HTTP download with progress, SHA-256/SHA-512 verification, `.tar.{gz,tgz,xz,zst}` extraction, read-only permission cleanup.
- `src/steam.rs` — Steam path resolution, installed runner scanning, VDF tokenizer (`compatibilitytool.vdf` and `CompatToolMapping` in `config.vdf`), table formatter.
- `config/runners.json` — Default upstream runner sources (`ge`, `cachyos`, `em`, `tkg`, `dw`).

## Rust Coding Rules
Follow the [Rust Style Guide](https://doc.rust-lang.org/nightly/style-guide/) and [Rust API Guidelines](https://rust-lang.github.io/api-guidelines/):
- **Edition & Formatting**: Rust 2024 edition (`let` chains, `if let` guards). Format exclusively with `cargo fmt`.
- **Error Handling**: Return `Result<T, String>` with actionable context (file path or URL that failed). Never use `.unwrap()`, `.expect()`, or `panic!()` in non-test code.
- **Signatures & Ownership**: Accept borrowed types (`&str`, `&Path`, `&[T]`) instead of owned (`String`, `PathBuf`, `Vec<T>`) when ownership is not required.
- **Zero Warnings**: Must pass `cargo clippy --all-targets -- -D warnings` with zero warnings or suppressions (`#[allow(...)]`).

## Verification Gate
Run before finishing any change:
```bash
cargo fmt --check
cargo clippy --all-targets -- -D warnings
cargo test
```

## Git & Security Workflow
- **Pre-Commit Secret Check**: Always inspect `git diff --cached` before committing to ensure zero secrets, tokens, passwords, private keys, or sensitive local data are staged.
- **Commit Locally Only**: Commit changes locally and present the commit/diff to the user for review.
- **Never Push**: NEVER run `git push`. The user pushes all changes manually.
