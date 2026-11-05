# AGENTS.md — `prm` (Proton Runner Manager)

## Core Philosophy (Strict KISS & YAGNI)
- **Minimalism first**: Shortest working diff wins. Deletion over addition. Standard C library or existing vendored libraries before any new dependency.
- **Atomic CLI commands**: Each command (`list`, `install`, `update`, `remove`, `purge`) does one thing and exits.
- **No unsolicited output**: Never print post-command tips, workflow suggestions, or decorative banners.

## Hard Non-Goals (Reject Immediately)
- **No GUI / TUI**: Plain stdout/stderr tables and prompts only.
- **No external tools or heavy runtimes**: 100% self-contained synchronous C11 binary using minimal vendored libraries (`yyjson`, `mbedtls`, `miniz`, `xz-embedded`, `zstddeclib`). Never shell out to `curl`/`tar`.
- **No Steam config mutation**: Never write to Steam's `config.vdf` or `.acf` files. `config.vdf` is read-only (used by `prm purge` to detect used runners).
- **No DLL injection / prefix patching**: `prm` manages whole Proton runners in `compatibilitytools.d` only. Component columns (`DXVK`, `VKD3D-PROTON`, `NVAPI`, `D7VK`) in `prm list` are strictly read-only.
- **No hardcoded upstream URLs**: All runner release endpoints live in `config/runners.json`.

## Codebase Layout
- `Makefile` — Build targets (`all`, `static`, `test`, `clean`).
- `src/prm.h` — Shared structs and function declarations.
- `src/main.c` — CLI argument parser and command dispatch.
- `src/runner.c` — Subcommand handlers (`cmd_list`, `cmd_install`, `cmd_update`, `cmd_remove`, `cmd_purge`).
- `src/sources.c` — `runners.json` loading (`yyjson`) and GitHub/Gitea release API resolution.
- `src/archive.c` — HTTPS 1.1 client (`mbedtls`), SHA-256/SHA-512 verification, `.tar.{gz,tgz,xz,zst}` streaming decompression and USTAR/PAX/GNU tar extraction, read-only permission cleanup.
- `src/steam.c` — Steam path resolution, installed runner scanning, VDF tokenizer (`compatibilitytool.vdf` and `CompatToolMapping` in `config.vdf`), table formatter.
- `tests/test_prm.c` — Unit and archive extraction test suite.
- `vendor/` — Minimal vendored C sources (`yyjson`, `mbedtls` client subset, `miniz`, `xz-embedded`, `zstddeclib`).
- `config/runners.json` — Default upstream runner sources (`ge`, `cachyos`, `em`, `tkg`, `dw`).

## C Coding Rules
- **Standard & Warnings**: C11 (`-std=c11`). Must pass `-Wall -Wextra -Werror` with zero warnings on `src/` and `tests/`.
- **Error Handling**: Return `0` on success, `-1` on failure, writing actionable context (file path or URL that failed) into `char *err, size_t err_cap`.
- **Bounds & Memory Safety**: Use `snprintf` for bounded writes, validate tar paths against directory traversal, and free heap allocations on all return paths.

## Verification Gate
Run before finishing any change:
```bash
make clean && make && make test
```

## Git & Security Workflow
- **Pre-Commit Secret Check**: Always inspect `git diff --cached` before committing to ensure zero secrets, tokens, passwords, private keys, or sensitive local data are staged.
- **Commit Locally Only**: Commit changes locally and present the commit/diff to the user for review.
- **Never Push**: NEVER run `git push`. The user pushes all changes manually.
