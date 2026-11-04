use crate::sources::ReleaseAsset;
use crate::steam::format_bytes;
use flate2::read::GzDecoder;
use sha2::{Digest, Sha256, Sha512};
use std::fs::{self, File};
use std::io::{self, Read, Write};
use std::path::{Path, PathBuf};
use tar::Archive;
use xz2::read::XzDecoder;
use zstd::stream::read::Decoder as ZstdDecoder;

pub fn download_file(url: &str, dest: &Path, expected_size: u64) -> Result<(), String> {
    if let Some(parent) = dest.parent() {
        fs::create_dir_all(parent)
            .map_err(|e| format!("Failed to create cache dir {}: {}", parent.display(), e))?;
    }

    if dest.exists()
        && expected_size > 0
        && let Ok(meta) = fs::metadata(dest)
        && meta.len() == expected_size
    {
        eprintln!(
            "Using cached archive: {} ({})",
            dest.file_name().unwrap_or_default().to_string_lossy(),
            format_bytes(expected_size)
        );
        return Ok(());
    }

    let tmp_dest = dest.with_extension("part");
    eprintln!(
        "Downloading {} ...",
        dest.file_name().unwrap_or_default().to_string_lossy()
    );

    let resp = ureq::get(url)
        .set("User-Agent", concat!("prm/", env!("CARGO_PKG_VERSION")))
        .call()
        .map_err(|e| format!("HTTP download failed for {}: {}", url, e))?;

    let total = resp
        .header("Content-Length")
        .and_then(|s| s.parse::<u64>().ok())
        .unwrap_or(expected_size);

    let mut reader = resp.into_reader();
    let mut file = File::create(&tmp_dest)
        .map_err(|e| format!("Failed to create {}: {}", tmp_dest.display(), e))?;

    let mut buf = [0u8; 64 * 1024];
    let mut downloaded = 0u64;
    let mut last_report = 0u64;

    loop {
        let n = reader
            .read(&mut buf)
            .map_err(|e| format!("Network read error: {}", e))?;
        if n == 0 {
            break;
        }
        file.write_all(&buf[..n])
            .map_err(|e| format!("Disk write error: {}", e))?;
        downloaded += n as u64;
        if total > 0 && downloaded - last_report >= 8 * 1024 * 1024 {
            last_report = downloaded;
            let pct = (downloaded as f64 / total as f64) * 100.0;
            eprint!(
                "\r  {:.1}% ({} / {})",
                pct,
                format_bytes(downloaded),
                format_bytes(total)
            );
            let _ = io::stderr().flush();
        }
    }
    if total > 0 {
        eprintln!(
            "\r  100.0% ({} / {})",
            format_bytes(downloaded),
            format_bytes(total)
        );
    }

    fs::rename(&tmp_dest, dest)
        .map_err(|e| format!("Failed to finalize {}: {}", dest.display(), e))?;
    Ok(())
}

pub fn fetch_expected_checksum(checksum_asset: &ReleaseAsset) -> Result<String, String> {
    let resp = ureq::get(&checksum_asset.download_url)
        .set("User-Agent", concat!("prm/", env!("CARGO_PKG_VERSION")))
        .call()
        .map_err(|e| format!("Failed to fetch checksum {}: {}", checksum_asset.name, e))?;
    let text = resp
        .into_string()
        .map_err(|e| format!("Failed to read checksum text: {}", e))?;

    let expected_hex = text
        .split_whitespace()
        .next()
        .ok_or_else(|| format!("Empty checksum file: {}", checksum_asset.name))?
        .trim()
        .to_lowercase();
    Ok(expected_hex)
}

pub fn compute_file_checksum(archive_path: &Path, use_sha256: bool) -> Result<String, String> {
    let mut file = File::open(archive_path).map_err(|e| {
        format!(
            "Failed to open {} for hashing: {}",
            archive_path.display(),
            e
        )
    })?;
    if use_sha256 {
        let mut hasher = Sha256::new();
        io::copy(&mut file, &mut hasher).map_err(|e| format!("Hashing error: {}", e))?;
        Ok(hasher
            .finalize()
            .iter()
            .map(|b| format!("{b:02x}"))
            .collect())
    } else {
        let mut hasher = Sha512::new();
        io::copy(&mut file, &mut hasher).map_err(|e| format!("Hashing error: {}", e))?;
        Ok(hasher
            .finalize()
            .iter()
            .map(|b| format!("{b:02x}"))
            .collect())
    }
}

pub fn verify_checksum(
    archive_path: &Path,
    checksum_asset: &ReleaseAsset,
) -> Result<String, String> {
    let expected_hex = fetch_expected_checksum(checksum_asset)?;
    let use_sha256 = checksum_asset.name.ends_with(".sha256sum") || expected_hex.len() == 64;
    let actual_hex = compute_file_checksum(archive_path, use_sha256)?;

    if actual_hex != expected_hex {
        let _ = fs::remove_file(archive_path);
        return Err(format!(
            "Checksum mismatch for {}!\n  Expected: {}\n  Actual:   {}",
            archive_path.display(),
            expected_hex,
            actual_hex
        ));
    }

    eprintln!("Checksum verified ({}).", checksum_asset.name);
    Ok(expected_hex)
}

pub fn extract_tar_archive(
    archive_path: &Path,
    target_parent_dir: &Path,
) -> Result<PathBuf, String> {
    fs::create_dir_all(target_parent_dir).map_err(|e| {
        format!(
            "Failed to create directory {}: {}",
            target_parent_dir.display(),
            e
        )
    })?;

    let staging_dir = target_parent_dir.join(format!(".prm-extract-{}", std::process::id()));
    if staging_dir.exists() {
        let _ = fs::remove_dir_all(&staging_dir);
    }
    fs::create_dir_all(&staging_dir).map_err(|e| format!("Failed to create staging dir: {}", e))?;

    let fname = archive_path
        .file_name()
        .unwrap_or_default()
        .to_string_lossy()
        .to_lowercase();
    eprintln!("Extracting {} ...", fname);

    let file = File::open(archive_path)
        .map_err(|e| format!("Failed to open archive {}: {}", archive_path.display(), e))?;

    let reader: Box<dyn Read> = if fname.ends_with(".tar.gz") || fname.ends_with(".tgz") {
        Box::new(GzDecoder::new(file))
    } else if fname.ends_with(".tar.xz") {
        Box::new(XzDecoder::new(file))
    } else if fname.ends_with(".tar.zst") {
        Box::new(
            ZstdDecoder::new(file)
                .map_err(|e| format!("Failed to initialize zstd decoder: {}", e))?,
        )
    } else {
        let _ = fs::remove_dir_all(&staging_dir);
        return Err(format!("Unsupported archive format: {}", fname));
    };

    let mut archive = Archive::new(reader);
    archive.set_preserve_permissions(true);
    if let Err(e) = archive.unpack(&staging_dir) {
        let _ = fs::remove_dir_all(&staging_dir);
        return Err(format!("Failed to unpack archive: {}", e));
    }

    // Determine top-level extracted directory
    let entries: Vec<fs::DirEntry> = fs::read_dir(&staging_dir)
        .map_err(|e| format!("Failed to read staging dir: {}", e))?
        .flatten()
        .collect();

    let final_path = if entries.len() == 1 && entries[0].path().is_dir() {
        let top_dir = entries[0].path();
        let dir_name = entries[0].file_name();
        let dest = target_parent_dir.join(&dir_name);
        if dest.exists() {
            remove_dir_force(&dest)?;
        }
        fs::rename(&top_dir, &dest).map_err(|e| {
            format!(
                "Failed to move extracted folder to {}: {}",
                dest.display(),
                e
            )
        })?;
        let _ = fs::remove_dir_all(&staging_dir);
        dest
    } else {
        let stem = crate::sources::archive_stem(&fname);
        let dest = target_parent_dir.join(stem);
        if dest.exists() {
            remove_dir_force(&dest)?;
        }
        fs::rename(&staging_dir, &dest)
            .map_err(|e| format!("Failed to move staging dir to {}: {}", dest.display(), e))?;
        dest
    };

    Ok(final_path)
}

/// Removes a directory tree even if some files/directories inside have read-only permissions (e.g., 0555 in GE-Proton).
pub fn remove_dir_force(path: &Path) -> Result<(), String> {
    if !path.exists() {
        return Ok(());
    }
    make_writable_recursive(path);
    fs::remove_dir_all(path)
        .map_err(|e| format!("Failed to remove directory {}: {}", path.display(), e))
}

fn make_writable_recursive(path: &Path) {
    use std::os::unix::fs::PermissionsExt;
    let Ok(meta) = fs::symlink_metadata(path) else {
        return;
    };
    if meta.is_symlink() {
        return;
    }
    if meta.is_dir() {
        let _ = fs::set_permissions(path, fs::Permissions::from_mode(0o755));
        if let Ok(entries) = fs::read_dir(path) {
            for entry in entries.flatten() {
                make_writable_recursive(&entry.path());
            }
        }
    } else {
        let _ = fs::set_permissions(path, fs::Permissions::from_mode(0o644));
    }
}
