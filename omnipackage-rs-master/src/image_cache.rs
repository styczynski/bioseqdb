use crate::PrimeArgs;
use crate::config::{Build, ImageCache, ImageCacheProvider};
use crate::distros::Distros;
use crate::logger::Logger;
use crate::release;
use crate::shell::Command;
use anyhow::{Context, Result};
use std::path::PathBuf;
use std::time::Instant;

pub fn refresh(args: PrimeArgs) -> Result<(), anyhow::Error> {
    let config = args.project.load_config(false)?;
    let ic = config
        .image_caches
        .as_ref()
        .context("image_caches is missing")?
        .find_by_name_or_default(args.job.image_cache.as_deref())?;
    let mut any_failed = false;

    for build_config in release::detect_builds(args.job.clone(), config.clone()) {
        Logger::new().info(format!("starting image cache refresh for {}", build_config.distro));
        let started_at = Instant::now();
        let res = refresh_distro(args.clone(), build_config.clone(), ic.clone());
        if let Err(ref e) = res {
            Logger::new().error(format!("image cache refresh error: {:#}", e));
        } else {
            Logger::new().info(format!("finished image cache refresh for {} in {:.1}s", build_config.distro, started_at.elapsed().as_secs_f32()));
        }

        let ok = release::fail_fast_or_continue(res, args.job.fail_fast)?;

        if !ok {
            any_failed = true;
        }
    }

    if any_failed {
        Err(anyhow::anyhow!("image cache refresh for one or more distros failed"))
    } else {
        Ok(())
    }
}

pub fn login_to_registry(image_cache_config: ImageCache, logger: Logger, log_path: Option<&PathBuf>) -> Result<(), anyhow::Error> {
    let registry = image_cache_config.registry.clone().context("registry config is missing")?;

    let mut cmd = Command::container(vec![
        "login".to_string(),
        registry.url.clone(),
        "-u".to_string(),
        registry.username.clone(),
        "--password-stdin".to_string(),
    ])
    .stream_output_to(logger)
    .with_stdin(move |stdin| {
        stdin.write_all(registry.password.as_bytes()).unwrap();
    });
    if let Some(v) = log_path {
        cmd = cmd.log_to(v);
    }
    cmd.run()
}

/// Build the Dockerfile `RUN` line that bakes `commands` into the cached image.
/// Commands are joined with ` && ` and wrapped in a single-quoted `bash -c '…'`.
/// Embedded single quotes are escaped as `'\''` so a command like arch's
/// `echo 'builder ALL=(ALL)…'` doesn't close the wrapper early and break shell parsing.
fn run_command(commands: &[String]) -> String {
    let joined = commands.join(" && ").replace('\'', r"'\''");
    format!("--mount=type=bind,from=src,target=/source,readonly bash -c '{}'", joined)
}

fn refresh_distro(args: PrimeArgs, build_config: Build, image_cache_config: ImageCache) -> Result<(), anyhow::Error> {
    let distro = Distros::get().by_id(&build_config.distro);
    let temp_dir = args.job.build_dir.join(format!("{}-{}", build_config.package_name, build_config.distro));
    std::fs::create_dir_all(&temp_dir)?;

    let base_image = distro.image.clone();
    let mut commands: Vec<String> = Vec::new();
    commands.extend(distro.setup(&build_config.build_dependencies));
    commands.extend(distro.setup_repo.clone());

    if let Some(bbs) = build_config.before_build_script {
        if args.project.source_dir.join(&bbs).exists() {
            commands.push(format!("/source/{}", bbs));
        } else {
            commands.push(bbs);
        }
    }
    commands.extend(distro.cleanup.clone());

    let runcmd = run_command(&commands);

    let dockerfile = format!(
        "FROM {base_image}\n\
         RUN {runcmd}\n"
    );
    std::fs::write(temp_dir.join("Dockerfile"), &dockerfile)?;

    let output_image = image_cache_config.full_image_name(&distro.id);
    let source_dir_abs = std::fs::canonicalize(&args.project.source_dir).with_context(|| format!("cannot resolve source dir {}", args.project.source_dir.display()))?;
    let cliargs = vec![
        "build".to_string(),
        "--build-context".to_string(),
        format!("src={}", source_dir_abs.display()),
        "-t".to_string(),
        output_image.clone(),
        ".".to_string(),
    ];

    Command::container(cliargs).stream_output_to(args.logging.container_logger()).current_dir(temp_dir.clone()).run()?;

    if image_cache_config.provider == ImageCacheProvider::Registry {
        Logger::new().info(format!("pushing image {} to registry", output_image));
        login_to_registry(image_cache_config.clone(), args.logging.container_logger(), None)?;
        Command::container(vec!["push".to_string(), output_image.clone()])
            .stream_output_to(args.logging.container_logger())
            .current_dir(temp_dir.clone())
            .run()?;
    }

    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn run_command_joins_and_escapes_single_quotes() {
        // a single-quoted command (sudoers-style) with parens is what would break the wrapper.
        let commands = vec![
            "pacman -Sy --noconfirm --needed base-devel".to_string(),
            "echo 'omnibuild ALL=(ALL) NOPASSWD:SETENV: ALL' > /etc/sudoers.d/omnibuild".to_string(),
        ];

        let runcmd = run_command(&commands);

        assert!(runcmd.starts_with("--mount=type=bind,from=src,target=/source,readonly bash -c '"));
        assert!(runcmd.ends_with('\''));
        assert!(runcmd.contains("base-devel && echo"), "commands should be joined with ` && `: {runcmd}");
        // each embedded single quote becomes '\'' so it doesn't close the wrapper early
        assert!(runcmd.contains(r"echo '\''omnibuild ALL=(ALL) NOPASSWD:SETENV: ALL'\''"), "single quotes not escaped: {runcmd}");
    }

    // The wrapper is single-quoted, so its argument must parse back to the exact join under a
    // POSIX shell — single-quoted commands would otherwise break it.
    #[cfg(unix)]
    #[test]
    fn run_command_arg_round_trips_through_shell() {
        let commands = vec![
            "pacman -Sy base-devel".to_string(),
            "echo 'omnibuild ALL=(ALL) NOPASSWD:SETENV: ALL' > /etc/sudoers.d/omnibuild".to_string(),
        ];

        let runcmd = run_command(&commands);
        // drop the buildkit `--mount` flag and `bash -c `, leaving the single-quoted argument
        let arg = runcmd.strip_prefix("--mount=type=bind,from=src,target=/source,readonly bash -c ").expect("runcmd prefix");

        // `printf %s <single-quoted-arg>` is inert (no command runs) and must reproduce the join
        let out = std::process::Command::new("/bin/sh").arg("-c").arg(format!("printf %s {arg}")).output().expect("run /bin/sh");

        assert!(out.status.success(), "shell failed to parse the wrapped argument: {}", String::from_utf8_lossy(&out.stderr));
        assert_eq!(String::from_utf8_lossy(&out.stdout), commands.join(" && "));
    }
}
