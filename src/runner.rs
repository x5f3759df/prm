use crate::archive::{
    compute_file_checksum, download_file, extract_tar_archive, fetch_expected_checksum,
    remove_dir_force, verify_checksum,
};
use crate::sources::{
    CpuVariant, SelectedReleaseAssets, archive_stem, fetch_releases, is_supported_archive,
    load_runners_config, parse_source_spec, resolve_release, resolve_runner_endpoint,
    select_release_assets,
};
use crate::steam::{
    InstalledRunner, SteamPaths, format_bytes, parse_compatibilitytool_vdf, print_table,
    read_used_runners, scan_installed_runners,
};
use std::fs;
use std::io::{self, Write};

pub fn cmd_list(paths: &SteamPaths, remote: Option<&str>, limit: usize) -> Result<(), String> {
    if let Some(source) = remote {
        if source.trim().is_empty() {
            let cfg = load_runners_config(&paths.config_dir)?;
            let mut rows = Vec::new();
            for (key, entry) in cfg.runners {
                let aliases = if entry.aliases.is_empty() {
                    "-".to_string()
                } else {
                    entry.aliases.join(", ")
                };
                rows.push(vec![key, aliases, entry.releases_url]);
            }
            print_table(&["SOURCE", "ALIASES", "RELEASES URL"], &rows);
            return Ok(());
        }
        let endpoint = resolve_runner_endpoint(&paths.config_dir, source)?;
        let releases = fetch_releases(&endpoint, limit)?;
        let cpu = CpuVariant::detect();
        println!(
            "Remote releases for '{}' (host CPU level: {}):\n",
            endpoint.key,
            cpu.as_str()
        );
        let mut rows = Vec::new();
        for rel in releases {
            let best = select_release_assets(&rel, cpu).ok();
            let (size_str, asset_name) = match best {
                Some(b) => (format_bytes(b.archive.size), b.archive.name),
                None => rel
                    .assets
                    .iter()
                    .find(|a| is_supported_archive(&a.name))
                    .map(|a| (format_bytes(a.size), a.name.clone()))
                    .unwrap_or_else(|| ("-".into(), "(no x86_64 archive)".into())),
            };
            rows.push(vec![rel.tag_name, rel.published_at, size_str, asset_name]);
        }
        print_table(&["TAG", "PUBLISHED", "SIZE", "BEST ASSET"], &rows);
        return Ok(());
    }

    let runners = scan_installed_runners(&paths.compat_dir);

    if runners.is_empty() {
        println!(
            "No custom Proton runners installed in {}",
            paths.compat_dir.display()
        );
    } else {
        let mut total_bytes = 0u64;
        let mut rows = Vec::new();
        for r in &runners {
            total_bytes += r.size_bytes;
            rows.push(vec![
                r.dir_name.clone(),
                format_bytes(r.size_bytes),
                r.dxvk_version.clone().unwrap_or_else(|| "-".into()),
                r.vkd3d_version.clone().unwrap_or_else(|| "-".into()),
                r.nvapi_version.clone().unwrap_or_else(|| "-".into()),
                r.d7vk_version.clone().unwrap_or_else(|| "-".into()),
            ]);
        }
        print_table(
            &["RUNNER", "SIZE", "DXVK", "VKD3D-PROTON", "NVAPI", "D7VK"],
            &rows,
        );
        println!(
            "\nTotal: {} runner(s), {}",
            runners.len(),
            format_bytes(total_bytes)
        );
    }

    Ok(())
}

/// Checks if `selected` release is already installed in `compatibilitytools.d` and its checksum matches.
/// Returns `Ok(Some(internal_name))` if already installed and verified.
fn check_already_installed_and_verified(
    paths: &SteamPaths,
    selected: &SelectedReleaseAssets,
) -> Result<Option<String>, String> {
    let runners = scan_installed_runners(&paths.compat_dir);
    let stem = archive_stem(&selected.archive.name);

    let matched = runners.iter().find(|r| {
        r.dir_name == selected.tag_name
            || r.internal_name == selected.tag_name
            || r.dir_name == stem
            || r.internal_name == stem
            || r.version_tag.as_deref() == Some(selected.tag_name.as_str())
    });

    let Some(installed) = matched else {
        return Ok(None);
    };

    // Ensure key runner files exist on disk
    if !installed.path.join("proton").exists()
        && !installed.path.join("compatibilitytool.vdf").exists()
    {
        return Ok(None);
    }

    let Some(cs_asset) = &selected.checksum else {
        // No upstream checksum published; directory + version match is sufficient
        return Ok(Some(installed.internal_name.clone()));
    };

    let expected_hex = fetch_expected_checksum(cs_asset)?;
    let marker_path = installed.path.join(".prm-checksum");

    // 1. Check stored .prm-checksum inside the installed runner directory
    if let Ok(saved) = fs::read_to_string(&marker_path)
        && let Some(saved_hex) = saved.split_whitespace().next()
        && saved_hex.eq_ignore_ascii_case(&expected_hex)
    {
        return Ok(Some(installed.internal_name.clone()));
    }

    // 2. Otherwise check cached archive in ~/.cache/prm/downloads/ if present
    let cached_archive = paths
        .cache_dir
        .join("downloads")
        .join(&selected.archive.name);
    if cached_archive.exists() {
        let use_sha256 = cs_asset.name.ends_with(".sha256sum") || expected_hex.len() == 64;
        if let Ok(actual_hex) = compute_file_checksum(&cached_archive, use_sha256)
            && actual_hex == expected_hex
        {
            // Persist .prm-checksum so future checks don't even need to re-hash the tarball
            let _ = fs::write(
                &marker_path,
                format!("{}  {}\n", expected_hex, selected.archive.name),
            );
            return Ok(Some(installed.internal_name.clone()));
        }
    }

    Ok(None)
}

pub fn cmd_install(
    paths: &SteamPaths,
    spec: &str,
    variant_override: Option<&str>,
    force: bool,
) -> Result<String, String> {
    let (source, version) = parse_source_spec(spec);
    let endpoint = resolve_runner_endpoint(&paths.config_dir, &source)?;
    let cpu = match variant_override {
        Some(v) => CpuVariant::parse(v)?,
        None => CpuVariant::detect(),
    };

    let release = resolve_release(&endpoint, version.as_deref())?;
    let selected = select_release_assets(&release, cpu)?;

    if !force && let Some(existing_name) = check_already_installed_and_verified(paths, &selected)? {
        println!(
            "'{}' ({}) is already installed, up to date, and checksum-verified. Nothing to do.",
            existing_name, selected.tag_name
        );
        return Ok(existing_name);
    }

    eprintln!(
        "Resolved {} -> {} (asset: {}, CPU target: {})",
        spec,
        selected.tag_name,
        selected.archive.name,
        cpu.as_str()
    );

    let cache_downloads = paths.cache_dir.join("downloads");
    let archive_path = cache_downloads.join(&selected.archive.name);
    download_file(
        &selected.archive.download_url,
        &archive_path,
        selected.archive.size,
    )?;

    let verified_hex = if let Some(cs) = &selected.checksum {
        Some(verify_checksum(&archive_path, cs)?)
    } else {
        None
    };

    let extracted_dir = extract_tar_archive(&archive_path, &paths.compat_dir)?;
    if let Some(hex) = verified_hex {
        let _ = fs::write(
            extracted_dir.join(".prm-checksum"),
            format!("{}  {}\n", hex, selected.archive.name),
        );
    }

    let dir_name = extracted_dir
        .file_name()
        .unwrap_or_default()
        .to_string_lossy()
        .to_string();

    let internal_name = fs::read_to_string(extracted_dir.join("compatibilitytool.vdf"))
        .map(|vdf| parse_compatibilitytool_vdf(&vdf, &dir_name))
        .unwrap_or_else(|_| dir_name.clone());

    println!(
        "Installed '{}' to {}",
        internal_name,
        extracted_dir.display()
    );

    Ok(internal_name)
}

pub fn cmd_update(paths: &SteamPaths, source_filter: Option<&str>) -> Result<(), String> {
    let cfg = load_runners_config(&paths.config_dir)?;
    let runners = scan_installed_runners(&paths.compat_dir);

    let mut families = Vec::new();
    if let Some(src) = source_filter {
        let ep = resolve_runner_endpoint(&paths.config_dir, src)?;
        families.push(ep.key);
    } else {
        for (key, entry) in &cfg.runners {
            let prefix = entry.installed_prefix.as_deref().unwrap_or(key.as_str());
            let prefix_lower = prefix.to_lowercase();
            if runners
                .iter()
                .any(|r| r.dir_name.to_lowercase().starts_with(&prefix_lower))
            {
                families.push(key.clone());
            }
        }
    }

    if families.is_empty() {
        println!(
            "No matching installed runner families detected from {}/runners.json.",
            paths.config_dir.display()
        );
        return Ok(());
    }

    let cpu = CpuVariant::detect();
    for family in families {
        let endpoint = resolve_runner_endpoint(&paths.config_dir, &family)?;
        let latest = resolve_release(&endpoint, None)?;
        let selected = select_release_assets(&latest, cpu)?;

        if let Some(existing_name) = check_already_installed_and_verified(paths, &selected)? {
            println!(
                "{}: '{}' ({}) is already up to date and checksum-verified. Nothing to do.",
                family, existing_name, selected.tag_name
            );
        } else {
            println!(
                "{}: new or unverified release {} -> installing...",
                family, latest.tag_name
            );
            cmd_install(
                paths,
                &format!("{}@{}", family, latest.tag_name),
                None,
                false,
            )?;
        }
    }

    Ok(())
}

pub fn cmd_remove(paths: &SteamPaths, targets: &[String]) -> Result<(), String> {
    let runners = scan_installed_runners(&paths.compat_dir);
    let mut to_remove: Vec<&InstalledRunner> = Vec::new();

    for t in targets {
        let matched = runners
            .iter()
            .find(|r| r.dir_name == *t || r.internal_name == *t)
            .ok_or_else(|| format!("Installed runner '{}' not found.", t))?;
        if !to_remove.iter().any(|r| r.dir_name == matched.dir_name) {
            to_remove.push(matched);
        }
    }

    for r in to_remove {
        remove_dir_force(&r.path)?;
        println!(
            "Removed {} (freed {})",
            r.dir_name,
            format_bytes(r.size_bytes)
        );
    }

    Ok(())
}

pub fn cmd_purge(paths: &SteamPaths) -> Result<(), String> {
    let runners = scan_installed_runners(&paths.compat_dir);
    if runners.is_empty() {
        println!("No installed runners to purge.");
        return Ok(());
    }

    let used = read_used_runners(&paths.config_vdf());
    let unused: Vec<&InstalledRunner> = runners
        .iter()
        .filter(|r| !used.contains(&r.internal_name) && !used.contains(&r.dir_name))
        .collect();

    if unused.is_empty() {
        println!("No unused runners found.");
        return Ok(());
    }

    let mut total_bytes = 0u64;
    let mut rows = Vec::new();
    for r in &unused {
        total_bytes += r.size_bytes;
        rows.push(vec![r.dir_name.clone(), format_bytes(r.size_bytes)]);
    }
    print_table(&["UNUSED RUNNER", "SIZE"], &rows);

    print!(
        "\nRemove {} unused runner(s) ({})? [y/N]: ",
        unused.len(),
        format_bytes(total_bytes)
    );
    let _ = io::stdout().flush();

    let mut input = String::new();
    io::stdin()
        .read_line(&mut input)
        .map_err(|e| format!("Failed to read confirmation: {}", e))?;
    let answer = input.trim();
    if !answer.eq_ignore_ascii_case("y") && !answer.eq_ignore_ascii_case("yes") {
        println!("Aborted.");
        return Ok(());
    }

    for r in unused {
        remove_dir_force(&r.path)?;
        println!(
            "Removed {} (freed {})",
            r.dir_name,
            format_bytes(r.size_bytes)
        );
    }

    Ok(())
}
