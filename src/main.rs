mod archive;
mod runner;
mod sources;
mod steam;

use clap::{Parser, Subcommand};
use std::process;

#[derive(Parser)]
#[command(
    name = "prm",
    version,
    about = "Proton Runner Manager — download, inspect, update, remove, and purge Steam Proton runners"
)]
struct Cli {
    #[command(subcommand)]
    command: Commands,
}

#[derive(Subcommand)]
enum Commands {
    /// List installed Proton runners (or remote sources/releases with --remote)
    List {
        /// List configured remote sources, or query upstream releases for a source (ge, cachyos, em, dw, tkg)
        #[arg(short, long, value_name = "SOURCE", num_args = 0..=1, default_missing_value = "")]
        remote: Option<String>,

        /// Maximum number of remote releases to fetch
        #[arg(short, long, default_value_t = 10)]
        limit: usize,
    },

    /// Download, verify, and install a Proton runner into compatibilitytools.d
    Install {
        /// Source and optional version tag: e.g., `ge`, `cachyos`, `em`, `ge@GE-Proton10-1`
        spec: String,

        /// Force reinstall even if already installed and verified
        #[arg(short, long)]
        force: bool,
    },

    /// Check installed runner families against upstream latest releases and install updates
    Update {
        /// Optional specific source family to update (ge, cachyos, em)
        source: Option<String>,
    },

    /// Remove specific installed Proton runner(s) by name
    Remove {
        /// Runner name(s) to remove (as shown in `prm list`)
        #[arg(required = true)]
        runners: Vec<String>,
    },

    /// Remove all unused installed Proton runners (with confirmation prompt)
    Purge,
}

fn run() -> Result<(), String> {
    let cli = Cli::parse();
    let paths = steam::SteamPaths::resolve()?;

    match cli.command {
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
