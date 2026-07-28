#![allow(dead_code)]

use clap::builder::styling::{AnsiColor, Effects, Styles};
use clap::{Args, Parser, Subcommand};
use std::path::{Path, PathBuf};

mod config;
mod distros;
mod extract_version;
mod gpg;
mod gpg_commands;
mod image_cache;
mod info;
mod init;
mod job_variables;
mod logger;
mod package;
mod portal;
mod publish;
mod release;
mod runner;
mod shell;
mod template;

use anyhow::Result;
use config::Config;
use logger::{LogOutput, Logger};

#[derive(Debug, Args)]
struct GlobalOpts {
    /// Container runtime, autodetect by default
    #[arg(long, global = true, value_parser = ["docker", "podman"])]
    container_runtime: Option<String>,
}

#[derive(Parser)]
#[command(version = version(), about)]
#[command(styles = styles())]
#[command(after_help = "Full documentation: https://docs.omnipackage.org/")]
struct Cli {
    #[command(flatten)]
    global: GlobalOpts,

    #[command(subcommand)]
    command: Commands,
}

#[derive(Args, Clone, Debug)]
pub struct ProjectArgs {
    /// Path to the project
    #[arg(default_value = ".")]
    source_dir: PathBuf,

    /// Relative path within source_dir to the omnipackage config
    #[arg(long, default_value = ".omnipackage/config.yml")]
    config_path: PathBuf,

    /// Full path to .env file containing secrets rendered in config
    #[arg(long, default_value = ".env")]
    env_file: PathBuf,
}

#[derive(Args, Clone, Debug)]
pub struct JobArgs {
    /// Distros to build/publish, e.g. opensuse_15.6, debian_12, fedora_40, by default build for all configured distros
    #[arg(short, long, num_args = 0..)]
    distros: Vec<String>,

    /// Root directory for temporary build/publish files
    #[arg(long, default_value_os_t = default_build_dir())]
    build_dir: PathBuf,

    /// Stop on first error instead of continuing with remaining distros
    #[arg(long, default_value_t = false)]
    fail_fast: bool,

    /// Image cache name from config.yml, no cache if omitted
    #[arg(long, short)]
    image_cache: Option<String>,
}

#[derive(Args, Clone, Debug)]
pub struct LoggingArgs {
    /// Where to print output from the containers (i.e. actual terminal output)
    #[arg(long, default_value = "stderr", value_parser = ["null", "stdout", "stderr"])]
    container_output: String,

    /// Disable echo (set -x) of commands inside the container
    #[arg(long)]
    disable_container_echo: bool,

    /// Number of lines to print from the log on failure (only when --container-output=null)
    #[arg(long, default_value_t = 50)]
    fail_log_lines: usize,
}

#[derive(Args, Clone, Debug)]
pub struct BuildArgs {
    #[command(flatten)]
    project: ProjectArgs,

    #[command(flatten)]
    logging: LoggingArgs,

    #[command(flatten)]
    job: JobArgs,

    /// Version extractor name, if blank the first version extractor from config will be used
    #[arg(short, long)]
    version_extractor: Option<String>,
}

#[derive(Args, Clone, Debug)]
pub struct PublishArgs {
    #[command(flatten)]
    project: ProjectArgs,

    #[command(flatten)]
    logging: LoggingArgs,

    #[command(flatten)]
    job: JobArgs,

    /// Repository name, if blank the first repository from config will be used
    #[arg(short, long)]
    repository: Option<String>,

    /// Path to custom install page template
    #[arg(long)]
    custom_install_page: Option<PathBuf>,
}

#[derive(Args, Clone, Debug)]
pub struct ReleaseArgs {
    #[command(flatten)]
    project: ProjectArgs,

    #[command(flatten)]
    logging: LoggingArgs,

    #[command(flatten)]
    job: JobArgs,

    /// Repository name to publish to, if blank the first repository from config will be used
    #[arg(short, long)]
    repository: Option<String>,

    /// Version extractor name, if blank the first version extractor from config will be used
    #[arg(short, long)]
    version_extractor: Option<String>,

    /// Path to custom install page template
    #[arg(long)]
    custom_install_page: Option<PathBuf>,
}

#[derive(Args, Clone, Debug)]
pub struct InfoArgs {
    #[command(flatten)]
    project: ProjectArgs,

    /// List all configured distros in project
    #[arg(long)]
    list_distros: bool,

    /// Show install page url
    #[arg(long)]
    show_install_page_url: bool,

    /// Repository name of the said install page, if blank the first repository from config will be used
    #[arg(short, long)]
    repository: Option<String>,

    /// Output format (list_distros only)
    #[arg(long, default_value = "plain", value_parser = ["plain", "json"])]
    format: String,
}

#[derive(Args, Clone, Debug)]
pub struct GpgGenerateArgs {
    /// Key owner name, i.e. your real name
    #[arg(short, long)]
    name: String,

    /// Key owner email, i.e. your real email
    #[arg(short, long)]
    email: String,

    /// Output format
    #[arg(long, default_value = "pem", value_parser = ["pem", "base64"])]
    format: String,
}

#[derive(Args, Clone, Debug)]
pub struct GpgConvertArgs {
    /// Input file (reads from stdin if omitted)
    #[arg()]
    input: Option<PathBuf>,

    /// Format of the input key file
    #[arg(short, long, default_value = "pem")]
    input_format: String,

    /// Format of the output key file
    #[arg(short, long, default_value = "base64")]
    output_format: String,
}

#[derive(Subcommand)]
enum GpgCommands {
    /// Generate a new GPG key
    Generate(GpgGenerateArgs),

    /// Convert keys between pem and base64 formats
    Convert(GpgConvertArgs),
}

#[derive(Args, Clone, Debug)]
pub struct PortalArgs {
    /// Distro id to spawn in a container
    #[arg()]
    distro: String,

    /// Root directory for temporary build/publish files, will be mounted in the container under the same basename
    #[arg(long, default_value_os_t = default_build_dir())]
    build_dir: PathBuf,
}

#[derive(Args, Clone, Debug)]
pub struct PrimeArgs {
    #[command(flatten)]
    project: ProjectArgs,

    #[command(flatten)]
    job: JobArgs,

    #[command(flatten)]
    logging: LoggingArgs,
}

#[derive(Subcommand)]
enum Commands {
    /// Build the project with omnipackage
    Build(BuildArgs),

    /// Publish built artefacts to a repository
    Publish(PublishArgs),

    /// Build and publish in one go
    Release(ReleaseArgs),

    /// GPG key management
    Gpg {
        #[command(subcommand)]
        command: GpgCommands,
    },

    /// Query various info about the project
    Info(InfoArgs),

    /// Shortcut to spawn a distro interactively in a container
    Portal(PortalArgs),

    /// Create or update cached images for a project
    Prime(PrimeArgs),

    /// Scaffold .omnipackage/ in a project directory
    Init(init::InitArgs),
}

fn main() -> Result<(), anyhow::Error> {
    let cli = Cli::parse();

    if let Some(runtime) = cli.global.container_runtime {
        shell::set_container_runtime(runtime);
    }

    match cli.command {
        Commands::Build(args) => release::build(args)?,
        Commands::Publish(args) => release::publish(args)?,
        Commands::Release(args) => release::release(args)?,
        Commands::Gpg { command } => match command {
            GpgCommands::Generate(args) => gpg_commands::generate(args)?,
            GpgCommands::Convert(args) => gpg_commands::convert(args)?,
        },
        Commands::Info(args) => info::info(args)?,
        Commands::Portal(args) => portal::run(args)?,
        Commands::Prime(args) => image_cache::refresh(args)?,
        Commands::Init(args) => init::init(args)?,
    }
    Ok(())
}

fn default_build_dir() -> PathBuf {
    match std::env::var("OMNIPACKAGE_BUILD_DIR") {
        Ok(dir) if !dir.is_empty() => PathBuf::from(dir),
        _ => std::env::temp_dir().join("omnipackage-build"),
    }
}

fn styles() -> Styles {
    Styles::styled()
        .header(AnsiColor::Green.on_default() | Effects::BOLD)
        .usage(AnsiColor::Green.on_default() | Effects::BOLD)
        .literal(AnsiColor::Cyan.on_default() | Effects::BOLD)
        .placeholder(AnsiColor::Cyan.on_default())
        .error(AnsiColor::Red.on_default() | Effects::BOLD)
        .valid(AnsiColor::Green.on_default() | Effects::BOLD)
        .invalid(AnsiColor::Yellow.on_default() | Effects::BOLD)
}

fn version() -> &'static str {
    let v = env!("CARGO_PKG_VERSION");
    match option_env!("PACKAGE_VERSION") {
        Some(pkg) if pkg != v => Box::leak(format!("{v} [{pkg}]").into_boxed_str()),
        _ => v,
    }
}

impl LoggingArgs {
    pub fn container_logger(&self) -> Logger {
        let output = match self.container_output.as_str() {
            "stderr" => LogOutput::Stderr,
            "stdout" => LogOutput::Stdout,
            _ => LogOutput::Silent,
        };
        Logger::new().with_output(output)
    }

    pub fn tail_log(&self, log_path: &Path) -> String {
        if self.container_output == "null" {
            std::fs::read_to_string(log_path)
                .map(|contents| {
                    let lines: Vec<&str> = contents.lines().collect();
                    let last = &lines[lines.len().saturating_sub(self.fail_log_lines)..];
                    format!("\n{}", last.join("\n"))
                })
                .unwrap_or_default()
        } else {
            String::new()
        }
    }
}

impl ProjectArgs {
    pub fn load_config(&self, silent: bool) -> Result<Config, anyhow::Error> {
        Config::load_with_env(&self.source_dir.join(&self.config_path), &self.env_file, silent)
    }
}
