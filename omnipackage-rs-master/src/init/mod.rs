use anyhow::{Context, Result};
use std::collections::HashMap;

mod args;
mod detect;
mod scaffold;
mod templates;
mod tokens;

pub use args::InitArgs;

pub fn init(args: InitArgs) -> Result<()> {
    let project_root = args.path.canonicalize().with_context(|| format!("resolving path {}", args.path.display()))?;

    let project_type = args
        .r#type
        .as_deref()
        .and_then(detect::ProjectType::from_str)
        .unwrap_or_else(|| detect::detect_project_type(&project_root));

    let detected_defaults = detect::extract_defaults(&project_root, project_type);

    let package_name_raw = args
        .package_name
        .clone()
        .or_else(|| detected_defaults.package_name.clone())
        .unwrap_or_else(|| detect::dir_basename(&project_root));
    let package_name_slug = detect::slugify(&package_name_raw);

    let maintainer_name = args
        .maintainer
        .clone()
        .or_else(|| detect::git_config("user.name"))
        .or_else(|| std::env::var("USER").ok())
        .unwrap_or_else(|| "Unknown".to_string());

    let email = args.email.clone().or_else(|| detect::git_config("user.email")).unwrap_or_else(|| "unknown@example.com".to_string());

    let maintainer_full = format!("{} <{}>", maintainer_name, email);

    let homepage = args.homepage.clone().or(detected_defaults.homepage).unwrap_or_else(|| "https://example.com".to_string());

    let description = args
        .description
        .clone()
        .or(detected_defaults.description)
        .unwrap_or_else(|| format!("{} packaged with omnipackage", package_name_slug));

    let (version_file, version_regex) = default_version_extractor(project_type, &package_name_slug);

    let mut vars: HashMap<&'static str, String> = HashMap::new();
    vars.insert(tokens::PACKAGE_NAME, package_name_slug.clone());
    vars.insert(tokens::MAINTAINER, maintainer_full);
    vars.insert(tokens::EMAIL, email);
    vars.insert(tokens::HOMEPAGE, homepage);
    vars.insert(tokens::DESCRIPTION, description);
    vars.insert(tokens::TODAY, tokens::today());
    vars.insert(tokens::VERSION_FILE, version_file);
    vars.insert(tokens::VERSION_REGEX, version_regex.to_string());
    vars.insert(tokens::RETAIN_PACKAGES, args.retain_packages.to_string());

    let scaffold = scaffold::Scaffold {
        project_root: &project_root,
        project_type,
        vars,
        templates: templates::template_set_for(project_type),
        package_name_slug,
    };

    let plan = scaffold.build_plan();

    let conflicts = scaffold::pre_flight(&plan);
    if !conflicts.is_empty() && !args.force {
        scaffold::print_conflicts(&conflicts);
        anyhow::bail!("aborted: {} file(s) already exist", conflicts.len());
    }

    if args.dry_run {
        scaffold::print_summary(&plan, true);
        return Ok(());
    }

    scaffold::write(&plan)?;
    scaffold::print_summary(&plan, false);
    Ok(())
}

fn default_version_extractor(t: detect::ProjectType, package_name_slug: &str) -> (String, &'static str) {
    use detect::ProjectType::*;
    match t {
        Tauri => ("src-tauri/Cargo.toml".to_string(), r#"version = "(.+)""#),
        Rust => ("Cargo.toml".to_string(), r#"version = "(.+)""#),
        Go => ("version.go".to_string(), r#"Version = "(.+)""#),
        Python => ("main.py".to_string(), r#"VERSION = "(.+)""#),
        // Ruby convention: gem foo-bar lives in lib/foo_bar/version.rb.
        Ruby => (format!("lib/{}/version.rb", package_name_slug.replace('-', "_")), r#"VERSION = "(.+)""#),
        Crystal => ("shard.yml".to_string(), r"version: (.+)"),
        Electron => ("package.json".to_string(), r#""version": "(.+)""#),
        CMake => ("CMakeLists.txt".to_string(), r"project\([^)]*VERSION ([0-9.]+)"),
        Cpp | C => ("version.h".to_string(), r#"VERSION "(.+)""#),
        Generic => ("VERSION".to_string(), r"^(\S+)$"),
    }
}
