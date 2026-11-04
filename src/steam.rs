use std::collections::BTreeSet;
use std::env;
use std::fs;
use std::path::{Path, PathBuf};

#[derive(Debug, Clone)]
pub struct SteamPaths {
    pub steam_dir: PathBuf,
    pub compat_dir: PathBuf,
    pub cache_dir: PathBuf,
    pub config_dir: PathBuf,
}

impl SteamPaths {
    pub fn resolve() -> Result<Self, String> {
        let home = env::var("HOME").map_err(|_| "HOME environment variable not set".to_string())?;
        let home_path = PathBuf::from(&home);

        let local_steam = home_path.join(".local/share/Steam");
        let steam_dir = if local_steam.exists() {
            local_steam
        } else {
            home_path.join(".steam/root")
        };

        let compat_dir = steam_dir.join("compatibilitytools.d");
        let cache_dir = env::var("XDG_CACHE_HOME")
            .map(PathBuf::from)
            .unwrap_or_else(|_| home_path.join(".cache"))
            .join("prm");

        let config_dir = resolve_config_dir(&home_path);

        Ok(Self {
            steam_dir,
            compat_dir,
            cache_dir,
            config_dir,
        })
    }

    pub fn config_vdf(&self) -> PathBuf {
        self.steam_dir.join("config/config.vdf")
    }
}

fn resolve_config_dir(home_path: &Path) -> PathBuf {
    if let Ok(env_dir) = env::var("PRM_CONFIG_DIR")
        && !env_dir.trim().is_empty()
    {
        return PathBuf::from(env_dir);
    }

    let cwd_config = PathBuf::from("config");
    if cwd_config.is_dir() {
        return cwd_config;
    }

    if let Ok(exe) = env::current_exe()
        && let Some(exe_dir) = exe.parent()
    {
        let sibling_cfg = exe_dir.join("config");
        if sibling_cfg.is_dir() {
            return sibling_cfg;
        }
        // When running from target/release/prm or target/debug/prm inside repo
        if let Some(repo_root) = exe_dir.parent().and_then(|p| p.parent()) {
            let repo_cfg = repo_root.join("config");
            if repo_cfg.is_dir() {
                return repo_cfg;
            }
        }
    }

    env::var("XDG_CONFIG_HOME")
        .map(PathBuf::from)
        .unwrap_or_else(|_| home_path.join(".config"))
        .join("prm")
}

#[derive(Debug, Clone)]
pub struct InstalledRunner {
    pub dir_name: String,
    pub internal_name: String,
    pub version_tag: Option<String>,
    pub path: PathBuf,
    pub size_bytes: u64,
    pub dxvk_version: Option<String>,
    pub vkd3d_version: Option<String>,
    pub nvapi_version: Option<String>,
    pub d7vk_version: Option<String>,
}

pub fn dir_size(path: &Path) -> u64 {
    let Ok(meta) = fs::symlink_metadata(path) else {
        return 0;
    };
    if meta.is_symlink() {
        return 0;
    }
    if meta.is_file() {
        return meta.len();
    }
    let Ok(entries) = fs::read_dir(path) else {
        return 0;
    };
    entries.flatten().map(|e| dir_size(&e.path())).sum()
}

fn read_dll_version(runner_path: &Path, comp: &str) -> Option<String> {
    let candidates = [
        runner_path
            .join("files/lib/wine")
            .join(comp)
            .join("version"),
        runner_path
            .join("files/lib64/wine")
            .join(comp)
            .join("version"),
        runner_path.join("dist/lib/wine").join(comp).join("version"),
        runner_path
            .join("dist/lib64/wine")
            .join(comp)
            .join("version"),
    ];
    for c in &candidates {
        if let Ok(raw) = fs::read_to_string(c) {
            let line = raw.lines().next().unwrap_or("").trim();
            if line.is_empty() {
                continue;
            }
            // Extract parenthesized version if present: e.g. "hash dxvk (v3.1.1)" -> "v3.1.1"
            if let (Some(start), Some(end)) = (line.find('('), line.rfind(')'))
                && start < end
            {
                return Some(line[start + 1..end].trim().to_string());
            }
            // Otherwise shorten raw git hash
            if line.len() == 40 && line.chars().all(|ch| ch.is_ascii_hexdigit()) {
                return Some(line[..10].to_string());
            }
            return Some(line.to_string());
        }
    }
    None
}

pub fn parse_compatibilitytool_vdf(content: &str, fallback: &str) -> String {
    let tokens = tokenize_vdf(content);
    for i in 0..tokens.len().saturating_sub(2) {
        if tokens[i] == "compat_tools" && tokens[i + 1] == "{" {
            return tokens[i + 2].clone();
        }
    }
    fallback.to_string()
}

pub fn scan_installed_runners(compat_dir: &Path) -> Vec<InstalledRunner> {
    let Ok(entries) = fs::read_dir(compat_dir) else {
        return Vec::new();
    };
    let mut runners = Vec::new();
    for entry in entries.flatten() {
        let path = entry.path();
        if !path.is_dir() {
            continue;
        }
        let dir_name = entry.file_name().to_string_lossy().to_string();
        if dir_name.starts_with('.') {
            continue;
        }
        let vdf_path = path.join("compatibilitytool.vdf");
        let internal_name = if let Ok(vdf) = fs::read_to_string(&vdf_path) {
            parse_compatibilitytool_vdf(&vdf, &dir_name)
        } else {
            dir_name.clone()
        };
        let version_tag = fs::read_to_string(path.join("version"))
            .ok()
            .and_then(|s| s.split_whitespace().nth(1).map(|v| v.to_string()));

        runners.push(InstalledRunner {
            dir_name,
            internal_name,
            version_tag,
            size_bytes: dir_size(&path),
            dxvk_version: read_dll_version(&path, "dxvk"),
            vkd3d_version: read_dll_version(&path, "vkd3d-proton"),
            nvapi_version: read_dll_version(&path, "nvapi"),
            d7vk_version: read_dll_version(&path, "d7vk"),
            path,
        });
    }
    runners.sort_by(|a, b| a.dir_name.cmp(&b.dir_name));
    runners
}

pub fn tokenize_vdf(input: &str) -> Vec<String> {
    let mut tokens = Vec::new();
    let bytes = input.as_bytes();
    let mut i = 0;
    while i < bytes.len() {
        let b = bytes[i];
        if b.is_ascii_whitespace() {
            i += 1;
            continue;
        }
        if b == b'/' && i + 1 < bytes.len() && bytes[i + 1] == b'/' {
            while i < bytes.len() && bytes[i] != b'\n' {
                i += 1;
            }
            continue;
        }
        if b == b'{' || b == b'}' {
            tokens.push((b as char).to_string());
            i += 1;
            continue;
        }
        if b == b'"' {
            i += 1;
            let mut s = String::new();
            while i < bytes.len() {
                if bytes[i] == b'\\' && i + 1 < bytes.len() {
                    i += 1;
                    s.push(bytes[i] as char);
                } else if bytes[i] == b'"' {
                    i += 1;
                    break;
                } else if let Some(ch) = input[i..].chars().next() {
                    s.push(ch);
                    i += ch.len_utf8() - 1;
                }
                i += 1;
            }
            tokens.push(s);
            continue;
        }
        // Unquoted token
        let start = i;
        while i < bytes.len()
            && !bytes[i].is_ascii_whitespace()
            && bytes[i] != b'{'
            && bytes[i] != b'}'
            && bytes[i] != b'"'
        {
            i += 1;
        }
        tokens.push(input[start..i].to_string());
    }
    tokens
}

pub fn read_used_runners(config_vdf: &Path) -> BTreeSet<String> {
    let Ok(content) = fs::read_to_string(config_vdf) else {
        return BTreeSet::new();
    };
    parse_used_runners_str(&content)
}

pub fn parse_used_runners_str(content: &str) -> BTreeSet<String> {
    let tokens = tokenize_vdf(content);
    let mut used = BTreeSet::new();
    let mut i = 0;
    while i < tokens.len() {
        if tokens[i] == "CompatToolMapping" && i + 1 < tokens.len() && tokens[i + 1] == "{" {
            i += 2;
            let mut depth = 1usize;
            while i < tokens.len() && depth > 0 {
                match tokens[i].as_str() {
                    "{" => {
                        depth += 1;
                        i += 1;
                    }
                    "}" => {
                        depth -= 1;
                        i += 1;
                    }
                    "name" if depth == 2 && i + 1 < tokens.len() => {
                        let name = tokens[i + 1].trim();
                        if !name.is_empty() {
                            used.insert(name.to_string());
                        }
                        i += 2;
                    }
                    _ => i += 1,
                }
            }
            break;
        }
        i += 1;
    }
    used
}

pub fn format_bytes(bytes: u64) -> String {
    const KB: f64 = 1024.0;
    const MB: f64 = KB * 1024.0;
    const GB: f64 = MB * 1024.0;
    let b = bytes as f64;
    if b >= GB {
        format!("{:.2} GiB", b / GB)
    } else if b >= MB {
        format!("{:.1} MiB", b / MB)
    } else if b >= KB {
        format!("{:.1} KiB", b / KB)
    } else {
        format!("{} B", bytes)
    }
}

pub fn print_table(headers: &[&str], rows: &[Vec<String>]) {
    let cols = headers.len();
    let mut widths: Vec<usize> = headers.iter().map(|h| h.chars().count()).collect();
    for row in rows {
        for (i, cell) in row.iter().enumerate().take(cols) {
            let w = cell.chars().count();
            if w > widths[i] {
                widths[i] = w;
            }
        }
    }

    // Print header row
    let mut header_line = String::new();
    let mut sep_line = String::new();
    for (i, h) in headers.iter().enumerate() {
        let w = widths[i];
        let pad = w.saturating_sub(h.chars().count());
        header_line.push_str(h);
        sep_line.push_str(&"-".repeat(w));
        if i + 1 < cols {
            header_line.push_str(&" ".repeat(pad + 2));
            sep_line.push_str("  ");
        }
    }
    println!("{}", header_line);
    println!("{}", sep_line);

    // Print data rows
    for row in rows {
        let mut line = String::new();
        for (i, cell) in row.iter().enumerate().take(cols) {
            let w = widths[i];
            let pad = w.saturating_sub(cell.chars().count());
            line.push_str(cell);
            if i + 1 < cols {
                line.push_str(&" ".repeat(pad + 2));
            }
        }
        println!("{}", line);
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_parse_compatibilitytool_vdf() {
        let vdf = r#"
"compatibilitytools"
{
  "compat_tools"
  {
    "GE-Proton11-12" // Internal name
    {
      "install_path" "."
      "display_name" "GE-Proton11-12"
    }
  }
}
"#;
        let internal = parse_compatibilitytool_vdf(vdf, "fallback");
        assert_eq!(internal, "GE-Proton11-12");
    }

    #[test]
    fn test_parse_used_runners_str() {
        let vdf = r#""InstallConfigStore"
{
	"Software"
	{
		"Valve"
		{
			"Steam"
			{
				"CompatToolMapping"
				{
					"0"
					{
						"name"		"GE-Proton11-12"
						"config"		""
						"priority"		"75"
					}
					"2231380"
					{
						"name"		"GE-Proton9-27"
						"config"		""
						"priority"		"250"
					}
				}
			}
		}
	}
}"#;
        let used = parse_used_runners_str(vdf);
        assert!(used.contains("GE-Proton11-12"));
        assert!(used.contains("GE-Proton9-27"));
        assert_eq!(used.len(), 2);
    }
}
