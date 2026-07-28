use clap::Args;
use std::path::PathBuf;

#[derive(Args, Clone, Debug)]
pub struct InitArgs {
    /// Path to the project directory that will receive .omnipackage/
    #[arg(default_value = ".")]
    pub path: PathBuf,

    /// Override project type detection
    #[arg(long, value_parser = ["rust","go","python","ruby","crystal","c","cpp","cmake","electron","tauri","generic"])]
    pub r#type: Option<String>,

    /// Package name (default: derived from project manifest or directory name)
    #[arg(long)]
    pub package_name: Option<String>,

    /// Maintainer name (default: from `git config user.name`)
    #[arg(long)]
    pub maintainer: Option<String>,

    /// Maintainer email (default: from `git config user.email`)
    #[arg(long)]
    pub email: Option<String>,

    /// Project homepage URL
    #[arg(long)]
    pub homepage: Option<String>,

    /// Short package description
    #[arg(long)]
    pub description: Option<String>,

    /// Retain this many previously published packages per repository (0 = disabled)
    #[arg(long, default_value_t = 0)]
    pub retain_packages: u32,

    /// Overwrite existing files in .omnipackage/
    #[arg(long)]
    pub force: bool,

    /// Print what would be created without writing anything
    #[arg(long)]
    pub dry_run: bool,
}
