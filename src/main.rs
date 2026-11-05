mod archive;
mod runner;
mod sources;
mod steam;

use std::env;
use std::process;

const HELP: &str = "\
prm — Proton Runner Manager

Usage: prm <COMMAND> [OPTIONS]

Commands:
  list [-r|--remote [SOURCE]] [-l|--limit <N>]   List installed runners or remote releases
  install [-f|--force] <SPEC>                    Install runner (e.g. ge, cachyos, ge@GE-Proton10-1)
  update [SOURCE]                                Update installed runner families
  remove <RUNNER>...                             Remove installed runner(s)
  purge                                          Remove all unused installed runners

Options:
  -h, --help      Print help
  -V, --version   Print version";

#[derive(Debug, PartialEq, Eq)]
enum Commands {
    List {
        remote: Option<String>,
        limit: usize,
    },
    Install {
        spec: String,
        force: bool,
    },
    Update {
        source: Option<String>,
    },
    Remove {
        runners: Vec<String>,
    },
    Purge,
}

fn parse_args(args: &[String]) -> Result<Option<Commands>, String> {
    let Some(cmd) = args.first().map(String::as_str) else {
        println!("{HELP}");
        return Ok(None);
    };

    if matches!(cmd, "-h" | "--help" | "help") {
        println!("{HELP}");
        return Ok(None);
    }
    if matches!(cmd, "-V" | "--version") {
        println!("prm {}", env!("CARGO_PKG_VERSION"));
        return Ok(None);
    }

    let rest = &args[1..];
    if rest.iter().any(|a| matches!(a.as_str(), "-h" | "--help")) {
        println!("{HELP}");
        return Ok(None);
    }

    match cmd {
        "list" => {
            let mut remote = None;
            let mut limit = 10usize;
            let mut i = 0;
            while i < rest.len() {
                let arg = rest[i].as_str();
                if matches!(arg, "-r" | "--remote") {
                    if let Some(next) = rest.get(i + 1)
                        && !next.starts_with('-')
                    {
                        remote = Some(next.clone());
                        i += 2;
                    } else {
                        remote = Some(String::new());
                        i += 1;
                    }
                } else if let Some(val) = arg.strip_prefix("--remote=") {
                    remote = Some(val.to_string());
                    i += 1;
                } else if matches!(arg, "-l" | "--limit") {
                    let val = rest
                        .get(i + 1)
                        .ok_or_else(|| "Missing value for --limit".to_string())?;
                    limit = val.parse().map_err(|_| format!("Invalid limit: '{val}'"))?;
                    i += 2;
                } else if let Some(val) = arg.strip_prefix("--limit=") {
                    limit = val.parse().map_err(|_| format!("Invalid limit: '{val}'"))?;
                    i += 1;
                } else {
                    return Err(format!("Unexpected argument for list: '{arg}'"));
                }
            }
            Ok(Some(Commands::List { remote, limit }))
        }
        "install" => {
            let mut force = false;
            let mut spec = None;
            for arg in rest {
                if matches!(arg.as_str(), "-f" | "--force") {
                    force = true;
                } else if arg.starts_with('-') {
                    return Err(format!("Unknown option for install: '{arg}'"));
                } else if spec.is_none() {
                    spec = Some(arg.clone());
                } else {
                    return Err(format!("Unexpected argument for install: '{arg}'"));
                }
            }
            let spec = spec.ok_or_else(|| "Usage: prm install [-f|--force] <SPEC>".to_string())?;
            Ok(Some(Commands::Install { spec, force }))
        }
        "update" => {
            if rest.len() > 1 {
                return Err("Usage: prm update [SOURCE]".to_string());
            }
            if let Some(arg) = rest.first()
                && arg.starts_with('-')
            {
                return Err(format!("Unknown option for update: '{arg}'"));
            }
            Ok(Some(Commands::Update {
                source: rest.first().cloned(),
            }))
        }
        "remove" => {
            if rest.is_empty() {
                return Err("Usage: prm remove <RUNNER>...".to_string());
            }
            if let Some(flag) = rest.iter().find(|a| a.starts_with('-')) {
                return Err(format!("Unknown option for remove: '{flag}'"));
            }
            Ok(Some(Commands::Remove {
                runners: rest.to_vec(),
            }))
        }
        "purge" => {
            if let Some(arg) = rest.first() {
                return Err(format!("Unexpected argument for purge: '{arg}'"));
            }
            Ok(Some(Commands::Purge))
        }
        other => Err(format!(
            "Unknown command '{other}'. Run 'prm --help' for usage."
        )),
    }
}

fn run() -> Result<(), String> {
    let args: Vec<String> = env::args().skip(1).collect();
    let Some(command) = parse_args(&args)? else {
        return Ok(());
    };
    let paths = steam::SteamPaths::resolve()?;

    match command {
        Commands::List { remote, limit } => runner::cmd_list(&paths, remote.as_deref(), limit),
        Commands::Install { spec, force } => runner::cmd_install(&paths, &spec, force).map(|_| ()),
        Commands::Update { source } => runner::cmd_update(&paths, source.as_deref()),
        Commands::Remove { runners } => runner::cmd_remove(&paths, &runners),
        Commands::Purge => runner::cmd_purge(&paths),
    }
}

fn main() {
    if let Err(err) = run() {
        eprintln!("Error: {}", err);
        process::exit(1);
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn vec_s(items: &[&str]) -> Vec<String> {
        items.iter().map(|s| (*s).to_string()).collect()
    }

    #[test]
    fn test_parse_list_variants() {
        assert_eq!(
            parse_args(&vec_s(&["list"])).unwrap(),
            Some(Commands::List {
                remote: None,
                limit: 10
            })
        );
        assert_eq!(
            parse_args(&vec_s(&["list", "-r"])).unwrap(),
            Some(Commands::List {
                remote: Some(String::new()),
                limit: 10
            })
        );
        assert_eq!(
            parse_args(&vec_s(&["list", "--remote", "ge", "-l", "5"])).unwrap(),
            Some(Commands::List {
                remote: Some("ge".into()),
                limit: 5
            })
        );
    }

    #[test]
    fn test_parse_install_update_remove_purge() {
        assert_eq!(
            parse_args(&vec_s(&["install", "-f", "ge@GE-Proton10-1"])).unwrap(),
            Some(Commands::Install {
                spec: "ge@GE-Proton10-1".into(),
                force: true
            })
        );
        assert_eq!(
            parse_args(&vec_s(&["update", "cachyos"])).unwrap(),
            Some(Commands::Update {
                source: Some("cachyos".into())
            })
        );
        assert_eq!(
            parse_args(&vec_s(&["remove", "GE-Proton10-1", "GE-Proton9-27"])).unwrap(),
            Some(Commands::Remove {
                runners: vec!["GE-Proton10-1".into(), "GE-Proton9-27".into()]
            })
        );
        assert_eq!(
            parse_args(&vec_s(&["purge"])).unwrap(),
            Some(Commands::Purge)
        );
    }
}
