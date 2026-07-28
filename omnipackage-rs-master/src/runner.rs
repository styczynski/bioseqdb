use crate::LoggingArgs;
use crate::config::ImageCacheProvider;
use crate::distros::ContainerCapability;
use crate::image_cache::login_to_registry;
use crate::job_variables::JobVariables;
use crate::logger::{Color, Logger, colorize};
use crate::package::Package;
use crate::shell::Command;
use anyhow::Result;
use std::collections::{HashMap, HashSet};
use std::path::PathBuf;
use std::time::Instant;

pub struct Runner {
    pub logging: LoggingArgs,
    pub package: Box<dyn Package>,
    pub job_variables: JobVariables,
}

type ExecuteError = (anyhow::Error, PathBuf);

impl Runner {
    pub fn new(package: Box<dyn Package>, logging: LoggingArgs, job_variables: JobVariables) -> Self {
        Self { package, logging, job_variables }
    }

    pub fn run(&self) -> Result<(), anyhow::Error> {
        Logger::new().info(format!(
            "starting {} for {}, variables: {}",
            self.package.setup_stage_name(),
            self.package.distro().id,
            self.job_variables
        ));
        let started_at = Instant::now();
        let result = self.execute();
        let finished_at = started_at.elapsed().as_secs_f32();

        match result {
            Ok(artefacts) => {
                Logger::new().info(format!(
                    "successfully finished {} for {} in {:.1}s, artefacts: {}",
                    self.package.setup_stage_name(),
                    self.package.distro().id,
                    finished_at,
                    colorize(Color::Green, artefacts.iter().map(|p| p.display().to_string()).collect::<Vec<_>>().join(", ")),
                ));

                Ok(())
            }
            Err((err, build_log)) => {
                Logger::new().error(format!(
                    "failed {} for {} in {:.1}s ({}), log: {}{}",
                    self.package.setup_stage_name(),
                    self.package.distro().id,
                    finished_at,
                    err,
                    colorize(Color::Red, build_log.display()),
                    self.logging.tail_log(&build_log),
                ));

                Err(err)
            }
        }
    }

    fn execute(&self) -> Result<Vec<PathBuf>, ExecuteError> {
        let result = self.execute_inner();
        // setup_repository writes key.priv under home_dir. Cleanup must run
        // even when registry login or container startup fails before the old
        // end-of-function teardown point.
        match self.package.teardown() {
            Ok(()) => result,
            Err(cleanup_error) => {
                let log_path = self.package.distro_build_dir().join("runner.log");
                match result {
                    Ok(_) => Err((cleanup_error, log_path)),
                    Err((execution_error, existing_log_path)) => Err((
                        anyhow::anyhow!("container execution failed ({execution_error:#}) and private-key cleanup failed ({cleanup_error:#})"),
                        existing_log_path,
                    )),
                }
            }
        }
    }

    fn execute_inner(&self) -> Result<Vec<PathBuf>, ExecuteError> {
        let mut args = vec!["run".to_string(), "--rm".to_string(), "--entrypoint".to_string(), "/bin/bash".to_string()];
        args.extend(container_capability_args(&self.package.container_capabilities()));

        let mut commands = self.package.commands().clone();
        if self.logging.disable_container_echo {
            commands.insert(0, "set -euo pipefail".to_string());
        } else {
            commands.insert(0, "set -euxo pipefail".to_string());
        }

        let mount_args: Vec<String> = self
            .package
            .mounts()
            .iter()
            .flat_map(|(from, to)| ["--mount".to_string(), format!("type=bind,source={from},target={to}")])
            .collect();
        args.extend(mount_args);

        let mut env_vars: HashMap<String, String> = [("GPG_TTY".to_string(), "".to_string())].into_iter().collect();
        env_vars.extend(self.job_variables.secrets.clone());
        args.extend(container_environment_args(&env_vars));

        let log_path = self.package.distro_build_dir().join("runner.log");
        let _ = std::fs::remove_file(&log_path);

        let preexisting: HashSet<PathBuf> = self.package.artefacts().into_iter().collect();

        if let Some(image_cache) = self.package.image_cache() {
            match image_cache.provider {
                ImageCacheProvider::Registry => login_to_registry(image_cache.clone(), self.container_logger(), Some(&log_path)).map_err(|e| (e, log_path.clone()))?,
                ImageCacheProvider::Local => args.push("--pull=never".to_string()),
            };
            args.push(image_cache.full_image_name(&self.package.distro().id));
        } else {
            args.push(self.package.distro().image.clone());
        }
        args.push("-c".to_string());
        args.push(commands.join(" && "));

        let mut container_command = Command::container(args).stream_output_to(self.container_logger()).log_to(&log_path);
        for (key, value) in env_vars {
            // `docker run -e KEY` inherits KEY from the Docker CLI process.
            // Keeping VALUE out of argv prevents exposure through ps/procfs.
            container_command = container_command.with_env(key, value);
        }
        container_command
            .run()
            .map(|_| self.package.artefacts().into_iter().filter(|p| !preexisting.contains(p)).collect())
            .map_err(|e| (e, log_path))
    }

    fn container_logger(&self) -> Logger {
        self.logging.container_logger().with_secrets(self.job_variables.secrets.values().cloned().collect::<Vec<String>>())
    }
}

fn container_capability_args(capabilities: &[ContainerCapability]) -> Vec<String> {
    capabilities.iter().flat_map(|capability| ["--cap-add".to_string(), capability.to_string()]).collect()
}

fn container_environment_args(environment: &HashMap<String, String>) -> Vec<String> {
    let mut names = environment.keys().cloned().collect::<Vec<_>>();
    names.sort();
    names.into_iter().flat_map(|name| ["-e".to_string(), name]).collect()
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn container_capability_args_adds_only_typed_capabilities() {
        assert_eq!(container_capability_args(&[ContainerCapability::SysNice]), vec!["--cap-add".to_string(), "SYS_NICE".to_string()]);
    }

    #[test]
    fn container_capability_args_is_empty_by_default() {
        assert!(container_capability_args(&[]).is_empty());
    }

    #[test]
    fn container_environment_args_never_contain_secret_values() {
        let environment = HashMap::from([("GPG_PRIVATE_KEY".to_string(), "super-secret-value".to_string()), ("GPG_TTY".to_string(), "".to_string())]);
        let args = container_environment_args(&environment);
        assert_eq!(args, vec!["-e", "GPG_PRIVATE_KEY", "-e", "GPG_TTY"].into_iter().map(str::to_string).collect::<Vec<_>>());
        assert!(!args.iter().any(|argument| argument.contains("super-secret-value")));
        assert!(!args.iter().any(|argument| argument.contains('=')));
    }
}
