use crate::LoggingArgs;
use crate::config::ImageCacheProvider;
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
        let mut args = vec!["run".to_string(), "--rm".to_string(), "--entrypoint".to_string(), "/bin/bash".to_string()];

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
        let env_args: Vec<String> = env_vars.iter().flat_map(|(k, v)| ["-e".to_string(), format!("{k}={v}")]).collect();
        args.extend(env_args);

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

        let result = Command::container(args)
            .stream_output_to(self.container_logger())
            .log_to(&log_path)
            .run()
            .map(|_| self.package.artefacts().into_iter().filter(|p| !preexisting.contains(p)).collect())
            .map_err(|e| (e, log_path.clone()));

        self.package.teardown();
        result
    }

    fn container_logger(&self) -> Logger {
        self.logging.container_logger().with_secrets(self.job_variables.secrets.values().cloned().collect::<Vec<String>>())
    }
}
