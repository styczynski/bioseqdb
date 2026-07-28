use crate::init::detect::ProjectType;
use crate::init::templates::TemplateFile;
use crate::init::tokens;
use crate::logger::{Color, colorize};
use anyhow::{Context, Result};
use std::collections::HashMap;
use std::fs;
use std::path::{Path, PathBuf};

pub struct Plan {
    pub root: PathBuf,
    pub project_type: ProjectType,
    pub files: Vec<RenderedFile>,
}

pub struct RenderedFile {
    pub abs_path: PathBuf,
    pub rel_path: String,
    pub content: String,
    pub executable: bool,
}

pub struct Scaffold<'a> {
    pub project_root: &'a Path,
    pub project_type: ProjectType,
    pub vars: HashMap<&'static str, String>,
    pub templates: Vec<TemplateFile>,
    pub package_name_slug: String,
}

impl<'a> Scaffold<'a> {
    pub fn build_plan(&self) -> Plan {
        let omni_dir = self.project_root.join(".omnipackage");
        let files = self
            .templates
            .iter()
            .map(|t| {
                let rel = t.dest.replace("<PACKAGE_NAME>", &self.package_name_slug);
                let abs = omni_dir.join(&rel);
                let content = tokens::apply_tokens(t.content, &self.vars);
                RenderedFile {
                    abs_path: abs,
                    rel_path: rel,
                    content,
                    executable: t.executable,
                }
            })
            .collect();
        Plan {
            root: omni_dir,
            project_type: self.project_type,
            files,
        }
    }
}

pub fn pre_flight(plan: &Plan) -> Vec<&RenderedFile> {
    plan.files.iter().filter(|f| f.abs_path.exists()).collect()
}

pub fn write(plan: &Plan) -> Result<()> {
    for f in &plan.files {
        if let Some(parent) = f.abs_path.parent() {
            fs::create_dir_all(parent).with_context(|| format!("creating directory {}", parent.display()))?;
        }
        fs::write(&f.abs_path, &f.content).with_context(|| format!("writing {}", f.abs_path.display()))?;
        if f.executable {
            set_executable(&f.abs_path)?;
        }
    }
    Ok(())
}

#[cfg(unix)]
fn set_executable(path: &Path) -> Result<()> {
    use std::os::unix::fs::PermissionsExt;
    let mut perms = fs::metadata(path)?.permissions();
    perms.set_mode(0o755);
    fs::set_permissions(path, perms)?;
    Ok(())
}

#[cfg(not(unix))]
fn set_executable(_path: &Path) -> Result<()> {
    Ok(())
}

pub fn print_summary(plan: &Plan, dry_run: bool) {
    let header = if dry_run {
        format!("Would create {} files in {} (dry-run):", plan.files.len(), plan.root.display())
    } else {
        format!("Created {} files in {}:", plan.files.len(), plan.root.display())
    };
    println!("{}", colorize(Color::BoldGreen, header));
    for f in &plan.files {
        let marker = if f.executable { " (executable)" } else { "" };
        println!("  {} {}{}", colorize(Color::Cyan, "+"), f.rel_path, marker);
    }
    println!();
    println!("{}", colorize(Color::Bold, "Detected project type:"));
    println!("  {} (override with --type)", plan.project_type.as_str());
    println!();
    println!("{}", colorize(Color::Bold, "Next steps:"));
    println!("  1. Edit .omnipackage/config.yml — set repository details and review distros.");
    println!(
        "  2. Generate a signing key:  {}",
        colorize(Color::Cyan, "omnipackage gpg generate -n \"Your Name\" -e you@example.com --format base64")
    );
    println!("     Put the printed value into the GPG_KEY env var (or .env file).");
    println!("  3. Build packages:  {}", colorize(Color::Cyan, "omnipackage build"));
    println!();
    println!("Docs: https://docs.omnipackage.org");
}

pub fn print_conflicts(conflicts: &[&RenderedFile]) {
    eprintln!("{}", colorize(Color::BoldRed, "Refusing to overwrite existing files:"));
    for f in conflicts {
        eprintln!("  - {}", f.abs_path.display());
    }
    eprintln!("\nRe-run with --force to overwrite.");
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::init::tokens::*;
    use tempfile::TempDir;

    fn vars() -> HashMap<&'static str, String> {
        HashMap::from([
            (PACKAGE_NAME, "demo".to_string()),
            (MAINTAINER, "Tester <t@example.com>".to_string()),
            (EMAIL, "t@example.com".to_string()),
            (HOMEPAGE, "https://example.com".to_string()),
            (DESCRIPTION, "demo pkg".to_string()),
            (TODAY, "Mon Jan 02 2026".to_string()),
            (VERSION_FILE, "Cargo.toml".to_string()),
            (VERSION_REGEX, r#"version = "(.+)""#.to_string()),
        ])
    }

    #[test]
    fn plan_substitutes_package_name_in_dest() {
        let d = TempDir::new().unwrap();
        let scaffold = Scaffold {
            project_root: d.path(),
            project_type: ProjectType::Electron,
            vars: vars(),
            templates: crate::init::templates::template_set_for(ProjectType::Electron),
            package_name_slug: "demo".to_string(),
        };
        let plan = scaffold.build_plan();
        let postinst = plan.files.iter().find(|f| f.rel_path.contains("postinst")).expect("postinst in plan");
        assert_eq!(postinst.rel_path, "deb/demo.postinst");
        assert!(postinst.executable);
    }

    #[test]
    fn write_creates_files_and_substitutes_tokens() {
        let d = TempDir::new().unwrap();
        let scaffold = Scaffold {
            project_root: d.path(),
            project_type: ProjectType::C,
            vars: vars(),
            templates: crate::init::templates::template_set_for(ProjectType::C),
            package_name_slug: "demo".to_string(),
        };
        let plan = scaffold.build_plan();
        write(&plan).unwrap();
        let cfg = std::fs::read_to_string(d.path().join(".omnipackage/config.yml")).unwrap();
        assert!(cfg.contains("demo"));
        assert!(!cfg.contains("__INIT_PACKAGE_NAME__"));
        assert!(d.path().join(".omnipackage/specfile.spec.liquid").exists());
        assert!(d.path().join(".omnipackage/deb/control.liquid").exists());
        assert!(d.path().join(".omnipackage/deb/rules.liquid").exists());
    }

    #[test]
    fn pre_flight_detects_existing_files() {
        let d = TempDir::new().unwrap();
        std::fs::create_dir_all(d.path().join(".omnipackage")).unwrap();
        std::fs::write(d.path().join(".omnipackage/config.yml"), "old").unwrap();

        let scaffold = Scaffold {
            project_root: d.path(),
            project_type: ProjectType::C,
            vars: vars(),
            templates: crate::init::templates::template_set_for(ProjectType::C),
            package_name_slug: "demo".to_string(),
        };
        let plan = scaffold.build_plan();
        let conflicts = pre_flight(&plan);
        assert!(conflicts.iter().any(|f| f.rel_path == "config.yml"));
    }
}
