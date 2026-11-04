use serde::Deserialize;
use std::collections::BTreeMap;
use std::env;
use std::fs;
use std::path::Path;

#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord)]
pub enum CpuVariant {
    X86_64,
    V2,
    V3,
    V4,
}

impl CpuVariant {
    pub fn detect() -> Self {
        let Ok(cpuinfo) = fs::read_to_string("/proc/cpuinfo") else {
            return CpuVariant::X86_64;
        };
        Self::from_cpuinfo(&cpuinfo)
    }

    pub fn from_cpuinfo(cpuinfo: &str) -> Self {
        let Some(flags_line) = cpuinfo
            .lines()
            .find(|l| l.starts_with("flags") && l.contains(':'))
        else {
            return CpuVariant::X86_64;
        };
        let flags: Vec<&str> = flags_line.split_whitespace().collect();
        let has = |f: &str| flags.contains(&f);

        let v3 = has("avx2") && has("bmi1") && has("bmi2") && has("fma") && has("movbe");
        let v4 = v3
            && has("avx512f")
            && has("avx512bw")
            && has("avx512cd")
            && has("avx512dq")
            && has("avx512vl");

        if v4 {
            CpuVariant::V4
        } else if v3 {
            CpuVariant::V3
        } else {
            CpuVariant::X86_64
        }
    }

    pub fn parse(s: &str) -> Result<Self, String> {
        match s.to_lowercase().as_str() {
            "v4" | "x86_64_v4" | "x86-64-v4" => Ok(CpuVariant::V4),
            "v3" | "x86_64_v3" | "x86-64-v3" => Ok(CpuVariant::V3),
            "v2" | "x86_64_v2" | "x86-64-v2" => Ok(CpuVariant::V2),
            "v1" | "x86_64" | "x86-64" | "generic" => Ok(CpuVariant::X86_64),
            other => Err(format!(
                "Unknown CPU variant '{}'. Valid: v4, v3, v2, x86_64",
                other
            )),
        }
    }

    pub fn as_str(&self) -> &'static str {
        match self {
            CpuVariant::V4 => "x86_64_v4",
            CpuVariant::V3 => "x86_64_v3",
            CpuVariant::V2 => "x86_64_v2",
            CpuVariant::X86_64 => "x86_64",
        }
    }
}

#[derive(Debug, Clone, Deserialize)]
pub struct RunnerSourceConfig {
    #[serde(default)]
    pub aliases: Vec<String>,
    #[serde(default)]
    pub installed_prefix: Option<String>,
    pub releases_url: String,
}

#[derive(Debug, Clone, Deserialize)]
pub struct RunnersConfigFile {
    pub runners: BTreeMap<String, RunnerSourceConfig>,
}

#[derive(Debug, Clone)]
pub struct EndpointConfig {
    pub key: String,
    pub releases_url: String,
}

const RUNNERS_FORMAT_HINT: &str = r#"{
  "runners": {
    "<short_name>": {
      "aliases": ["<optional_alias>"],
      "installed_prefix": "<optional_folder_prefix>",
      "releases_url": "<full_releases_api_url>"
    }
  }
}"#;

fn validate_url(field: &str, key: &str, url: &str, path: &Path) -> Result<(), String> {
    let trimmed = url.trim();
    if !(trimmed.starts_with("https://") || trimmed.starts_with("http://")) {
        return Err(format!(
            "Invalid `{}` for '{}' in {}: must be a full http(s) URL, got '{}'",
            field,
            key,
            path.display(),
            url
        ));
    }
    Ok(())
}

pub fn load_runners_config(config_dir: &Path) -> Result<RunnersConfigFile, String> {
    let path = config_dir.join("runners.json");
    if !path.exists() {
        return Err(format!(
            "Missing runner configuration file at: {}\n\
             Create `{}` with the following JSON format:\n{}",
            path.display(),
            path.display(),
            RUNNERS_FORMAT_HINT
        ));
    }
    let raw = fs::read_to_string(&path)
        .map_err(|e| format!("Failed to read {}: {}", path.display(), e))?;
    let parsed: RunnersConfigFile = serde_json::from_str(&raw).map_err(|e| {
        format!(
            "Invalid JSON in {}: {}\n\
             Expected format:\n{}",
            path.display(),
            e,
            RUNNERS_FORMAT_HINT
        )
    })?;
    if parsed.runners.is_empty() {
        return Err(format!(
            "Configuration file {} has an empty `runners` map.\n\
             Expected format:\n{}",
            path.display(),
            RUNNERS_FORMAT_HINT
        ));
    }
    for (k, v) in &parsed.runners {
        validate_url("releases_url", k, &v.releases_url, &path)?;
    }
    Ok(parsed)
}

pub fn resolve_runner_endpoint(
    config_dir: &Path,
    source_query: &str,
) -> Result<EndpointConfig, String> {
    let cfg = load_runners_config(config_dir)?;
    let q = source_query.to_lowercase();

    for (key, entry) in &cfg.runners {
        if key.to_lowercase() == q || entry.aliases.iter().any(|a| a.to_lowercase() == q) {
            return Ok(EndpointConfig {
                key: key.clone(),
                releases_url: entry.releases_url.clone(),
            });
        }
    }

    let available: Vec<String> = cfg.runners.keys().cloned().collect();
    Err(format!(
        "Runner source '{}' not found in {}/runners.json. Configured sources: {}",
        source_query,
        config_dir.display(),
        available.join(", ")
    ))
}

#[derive(Debug, Clone)]
pub struct ReleaseInfo {
    pub tag_name: String,
    pub published_at: String,
    pub assets: Vec<ReleaseAsset>,
}

#[derive(Debug, Clone)]
pub struct ReleaseAsset {
    pub name: String,
    pub download_url: String,
    pub size: u64,
}

#[derive(Debug, Clone)]
pub struct SelectedReleaseAssets {
    pub tag_name: String,
    pub archive: ReleaseAsset,
    pub checksum: Option<ReleaseAsset>,
}

pub fn parse_source_spec(spec: &str) -> (String, Option<String>) {
    if let Some((src, ver)) = spec.split_once('@') {
        (src.to_string(), Some(ver.to_string()))
    } else {
        (spec.to_string(), None)
    }
}

#[derive(Deserialize)]
struct GhRelease {
    tag_name: String,
    #[serde(default)]
    published_at: Option<String>,
    assets: Vec<GhAsset>,
}

#[derive(Deserialize)]
struct GhAsset {
    name: String,
    browser_download_url: String,
    #[serde(default)]
    size: u64,
}

fn build_request(url: &str) -> ureq::Request {
    let mut req = ureq::get(url)
        .set("User-Agent", concat!("prm/", env!("CARGO_PKG_VERSION")))
        .set("Accept", "application/vnd.github+json");
    if let Ok(token) = env::var("GITHUB_TOKEN")
        && !token.trim().is_empty()
    {
        req = req.set("Authorization", &format!("Bearer {}", token.trim()));
    }
    req
}

fn append_per_page(url: &str, per_page: usize) -> String {
    if url.contains('?') {
        format!("{}&per_page={}", url, per_page)
    } else {
        format!("{}?per_page={}", url, per_page)
    }
}

pub fn fetch_releases(endpoint: &EndpointConfig, limit: usize) -> Result<Vec<ReleaseInfo>, String> {
    let per_page = limit.clamp(1, 50);
    let url = append_per_page(&endpoint.releases_url, per_page);
    let resp = build_request(&url)
        .call()
        .map_err(|e| format!("API request failed for '{}' ({}): {}", endpoint.key, url, e))?;

    let gh_releases: Vec<GhRelease> = resp
        .into_json()
        .map_err(|e| format!("Failed to parse releases JSON: {}", e))?;
    Ok(gh_releases
        .into_iter()
        .take(limit)
        .map(map_gh_release)
        .collect())
}

fn map_gh_release(r: GhRelease) -> ReleaseInfo {
    ReleaseInfo {
        tag_name: r.tag_name,
        published_at: r
            .published_at
            .unwrap_or_default()
            .chars()
            .take(10)
            .collect(),
        assets: r
            .assets
            .into_iter()
            .map(|a| ReleaseAsset {
                name: a.name,
                download_url: a.browser_download_url,
                size: a.size,
            })
            .collect(),
    }
}

pub fn resolve_release(
    endpoint: &EndpointConfig,
    version: Option<&str>,
) -> Result<ReleaseInfo, String> {
    let base = endpoint.releases_url.trim_end_matches('/');
    match version {
        None | Some("latest") => {
            let latest_url = format!("{}/latest", base);
            if let Ok(resp) = build_request(&latest_url).call()
                && let Ok(r) = resp.into_json::<GhRelease>()
            {
                return Ok(map_gh_release(r));
            }
            let list = fetch_releases(endpoint, 10)?;
            list.into_iter()
                .next()
                .ok_or_else(|| format!("No releases found for '{}'", endpoint.key))
        }
        Some(tag) => {
            let tag_url = format!("{}/tags/{}", base, tag);
            if let Ok(resp) = build_request(&tag_url).call()
                && let Ok(r) = resp.into_json::<GhRelease>()
            {
                return Ok(map_gh_release(r));
            }
            let list = fetch_releases(endpoint, 30)?;
            let q = tag.to_lowercase();
            list.into_iter()
                .find(|r| {
                    let t = r.tag_name.to_lowercase();
                    t == q
                        || t.trim_start_matches('v') == q.trim_start_matches('v')
                        || t.contains(&q)
                })
                .ok_or_else(|| format!("Release '{}' not found for '{}'", tag, endpoint.key))
        }
    }
}

pub fn archive_stem(name: &str) -> &str {
    name.strip_suffix(".tar.gz")
        .or_else(|| name.strip_suffix(".tgz"))
        .or_else(|| name.strip_suffix(".tar.xz"))
        .or_else(|| name.strip_suffix(".tar.zst"))
        .unwrap_or(name)
}

pub fn is_supported_archive(name: &str) -> bool {
    let lower = name.to_lowercase();
    (lower.ends_with(".tar.gz")
        || lower.ends_with(".tgz")
        || lower.ends_with(".tar.xz")
        || lower.ends_with(".tar.zst"))
        && !lower.contains("arm64")
        && !lower.contains("aarch64")
}

fn asset_cpu_variant(name: &str) -> CpuVariant {
    let lower = name.to_lowercase();
    if lower.contains("x86_64_v4") || lower.contains("x86-64-v4") || lower.contains("-v4.") {
        CpuVariant::V4
    } else if lower.contains("x86_64_v3") || lower.contains("x86-64-v3") || lower.contains("-v3.") {
        CpuVariant::V3
    } else if lower.contains("x86_64_v2") || lower.contains("x86-64-v2") || lower.contains("-v2.") {
        CpuVariant::V2
    } else {
        CpuVariant::X86_64
    }
}

pub fn select_release_assets(
    release: &ReleaseInfo,
    target_variant: CpuVariant,
) -> Result<SelectedReleaseAssets, String> {
    let mut archives: Vec<&ReleaseAsset> = release
        .assets
        .iter()
        .filter(|a| is_supported_archive(&a.name))
        .collect();

    if archives.is_empty() {
        return Err(format!(
            "Release '{}' has no supported x86_64 archive (.tar.gz, .tar.xz, .tar.zst)",
            release.tag_name
        ));
    }

    archives.sort_by(|a, b| {
        let va = asset_cpu_variant(&a.name);
        let vb = asset_cpu_variant(&b.name);
        let score = |v: CpuVariant| -> i32 {
            if v <= target_variant {
                100 + (v as i32) * 10
            } else {
                -(v as i32)
            }
        };
        score(vb).cmp(&score(va))
    });

    let chosen = archives[0].clone();
    let stem = archive_stem(&chosen.name);

    let checksum = release
        .assets
        .iter()
        .find(|a| {
            let n = a.name.as_str();
            (n.ends_with(".sha512sum") || n.ends_with(".sha256sum"))
                && (n.starts_with(stem) || n.starts_with(&chosen.name))
        })
        .cloned();

    Ok(SelectedReleaseAssets {
        tag_name: release.tag_name.clone(),
        archive: chosen,
        checksum,
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_cpuinfo_detection() {
        let v4_cpu =
            "flags\t\t: fpu avx2 bmi1 bmi2 fma movbe avx512f avx512bw avx512cd avx512dq avx512vl\n";
        assert_eq!(CpuVariant::from_cpuinfo(v4_cpu), CpuVariant::V4);

        let v3_cpu = "flags\t\t: fpu avx2 bmi1 bmi2 fma movbe\n";
        assert_eq!(CpuVariant::from_cpuinfo(v3_cpu), CpuVariant::V3);

        let v1_cpu = "flags\t\t: fpu sse2\n";
        assert_eq!(CpuVariant::from_cpuinfo(v1_cpu), CpuVariant::X86_64);
    }

    #[test]
    fn test_missing_configs_throw_error_with_format_suggestion() {
        let missing_dir = Path::new("/tmp/prm-definitely-missing-config-dir-99999");
        let err_r = load_runners_config(missing_dir).unwrap_err();
        assert!(err_r.contains("Missing runner configuration file"));
        assert!(err_r.contains("\"releases_url\""));
    }

    #[test]
    fn test_repo_configs_are_valid() {
        let repo_cfg = Path::new(env!("CARGO_MANIFEST_DIR")).join("config");
        let r = load_runners_config(&repo_cfg).unwrap();
        assert!(r.runners.contains_key("ge"));
        assert!(r.runners.contains_key("cachyos"));
    }

    #[test]
    fn test_select_release_assets_prefers_best_variant() {
        let rel = ReleaseInfo {
            tag_name: "cachyos-10.0".into(),
            published_at: "2026-10-01".into(),
            assets: vec![
                ReleaseAsset {
                    name: "proton-cachyos-10.0-x86_64.tar.xz".into(),
                    download_url: "http://local/v1.tar.xz".into(),
                    size: 100,
                },
                ReleaseAsset {
                    name: "proton-cachyos-10.0-x86_64_v3.tar.xz".into(),
                    download_url: "http://local/v3.tar.xz".into(),
                    size: 100,
                },
                ReleaseAsset {
                    name: "proton-cachyos-10.0-x86_64_v3.tar.xz.sha512sum".into(),
                    download_url: "http://local/v3.sha512sum".into(),
                    size: 128,
                },
                ReleaseAsset {
                    name: "proton-cachyos-10.0-x86_64_v4.tar.xz".into(),
                    download_url: "http://local/v4.tar.xz".into(),
                    size: 100,
                },
            ],
        };

        let sel_v3 = select_release_assets(&rel, CpuVariant::V3).unwrap();
        assert_eq!(sel_v3.archive.name, "proton-cachyos-10.0-x86_64_v3.tar.xz");
        assert!(sel_v3.checksum.is_some());

        let sel_v4 = select_release_assets(&rel, CpuVariant::V4).unwrap();
        assert_eq!(sel_v4.archive.name, "proton-cachyos-10.0-x86_64_v4.tar.xz");

        let sel_v1 = select_release_assets(&rel, CpuVariant::X86_64).unwrap();
        assert_eq!(sel_v1.archive.name, "proton-cachyos-10.0-x86_64.tar.xz");
    }
}
