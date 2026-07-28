use crate::config::{Build, Config, ImageCache, Repository};
use crate::distros::Distros;
use crate::logger::Logger;
use crate::package::{Package, make_package};
use crate::publish::{Publish, prepopulate_with_retention};
use crate::runner::Runner;
use crate::{BuildArgs, JobArgs, ProjectArgs, PublishArgs, ReleaseArgs};
use crate::{extract_version, job_variables};
use anyhow::{Context, Result};
use std::collections::HashSet;
use std::path::PathBuf;

fn apply_retention(repository_config: &Repository, pkg: &dyn Package) -> Result<HashSet<PathBuf>, anyhow::Error> {
    match prepopulate_with_retention(repository_config, pkg)? {
        Some(stats) => {
            Logger::new().info(format!("retention for {}: kept {}, removed {}", pkg.distro().id, stats.kept, stats.removed));
            Ok(stats.retained_files.into_iter().collect())
        }
        None => Ok(HashSet::new()),
    }
}

struct JobSetup {
    job_variables: job_variables::JobVariables,
    build_dir: PathBuf,
    source_dir: PathBuf,
    image_cache: Option<ImageCache>,
    ignore_source_files: Vec<String>,
}

impl JobSetup {
    fn new(project: &ProjectArgs, job: &JobArgs, config: &Config, version_extractor: &Option<String>) -> Result<Self, anyhow::Error> {
        let version_config = config.version_extractors.find_by_name_or_default(version_extractor.as_deref())?.clone();
        let version = extract_version::extract_version(&project.source_dir, &version_config)?;
        let job_variables = job_variables::JobVariables::new(version).with_secrets(config.secrets.clone().into_iter().collect());
        let image_cache = job
            .image_cache
            .as_deref()
            .map(|ic| config.image_caches.as_ref().context("image_caches is missing")?.find_by_name_or_default(Some(ic)).cloned())
            .transpose()?;

        Ok(Self {
            job_variables,
            build_dir: job.build_dir.clone(),
            source_dir: project.source_dir.clone(),
            image_cache,
            ignore_source_files: config.ignore_source_files.clone(),
        })
    }

    pub fn make_package(&self, distro_id: &str, package_name: &str) -> Result<Box<dyn Package>, anyhow::Error> {
        let distro = Distros::get().by_id(distro_id);

        make_package(
            distro.clone(),
            self.source_dir.clone(),
            self.job_variables.clone(),
            self.build_dir.join(format!("{}-{}", package_name, distro_id)),
            self.image_cache.clone(),
            self.ignore_source_files.clone(),
        )
    }
}

pub fn build(args: BuildArgs) -> Result<(), anyhow::Error> {
    let config = args.project.load_config(false)?;
    let setup = JobSetup::new(&args.project, &args.job, &config, &args.version_extractor)?;
    let mut any_failed = false;

    for build_config in detect_builds(args.job.clone(), config) {
        let mut pkg = setup.make_package(&build_config.distro, &build_config.package_name)?;
        pkg.setup_build(build_config.clone())?;

        let builder = Runner::new(pkg.clone(), args.logging.clone(), setup.job_variables.clone());
        let build_ok = fail_fast_or_continue(builder.run(), args.job.fail_fast)?;

        if !build_ok {
            any_failed = true;
        }
    }

    if any_failed { Err(anyhow::anyhow!("build one or more distros failed")) } else { Ok(()) }
}

pub fn publish(args: PublishArgs) -> Result<(), anyhow::Error> {
    let config = args.project.load_config(false)?;
    let setup = JobSetup::new(&args.project, &args.job, &config, &None)?;
    let repository_config = config.repositories.find_by_name_or_default(args.repository.as_deref())?.clone();
    let mut any_failed = false;

    for build_config in detect_builds(args.job.clone(), config) {
        let mut pkg = setup.make_package(&build_config.distro, &build_config.package_name)?;
        pkg.setup_repository(repository_config.clone())?;
        let skip_upload = apply_retention(&repository_config, pkg.as_ref())?;

        let runner = Runner::new(pkg.clone(), args.logging.clone(), setup.job_variables.clone());
        let build_ok = fail_fast_or_continue(runner.run(), args.job.fail_fast)?;
        if build_ok {
            let publisher = Publish::new(pkg.clone(), repository_config.clone(), args.custom_install_page.clone(), skip_upload);
            let publish_ok = fail_fast_or_continue(publisher.run(), args.job.fail_fast)?;
            any_failed |= !publish_ok;
        } else {
            any_failed = true;
        }
    }

    if any_failed { Err(anyhow::anyhow!("publish one or more distros failed")) } else { Ok(()) }
}

pub fn release(args: ReleaseArgs) -> Result<(), anyhow::Error> {
    let config = args.project.load_config(false)?;
    let setup = JobSetup::new(&args.project, &args.job, &config, &args.version_extractor)?;
    let repository_config = config.repositories.find_by_name_or_default(args.repository.as_deref())?.clone();
    let mut any_failed = false;

    for build_config in detect_builds(args.job.clone(), config) {
        let mut pkg = setup.make_package(&build_config.distro, &build_config.package_name)?;
        pkg.setup_build(build_config.clone())?;
        pkg.setup_repository(repository_config.clone())?;
        let skip_upload = apply_retention(&repository_config, pkg.as_ref())?;

        let runner = Runner::new(pkg.clone(), args.logging.clone(), setup.job_variables.clone());
        let build_ok = fail_fast_or_continue(runner.run(), args.job.fail_fast)?;
        if build_ok {
            let publisher = Publish::new(pkg.clone(), repository_config.clone(), args.custom_install_page.clone(), skip_upload);
            let publish_ok = fail_fast_or_continue(publisher.run(), args.job.fail_fast)?;
            any_failed |= !publish_ok;
        } else {
            any_failed = true;
        }
    }

    if any_failed { Err(anyhow::anyhow!("release one or more distros failed")) } else { Ok(()) }
}

pub fn fail_fast_or_continue(result: Result<(), anyhow::Error>, fail_fast: bool) -> Result<bool, anyhow::Error> {
    match result {
        Ok(()) => Ok(true),
        Err(e) if fail_fast => Err(e),
        Err(_) => Ok(false),
    }
}

pub fn detect_builds(job: JobArgs, config: Config) -> impl Iterator<Item = Build> {
    config
        .builds
        .into_iter()
        .filter(move |build| Distros::get().contains(&build.distro))
        .filter(move |build| job.distros.is_empty() || job.distros.contains(&build.distro))
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::config::{Repositories, VersionExtractors};
    use std::collections::HashMap;
    use std::path::PathBuf;

    #[test]
    fn test_fail_fast_or_continue_ok_returns_true() {
        let result = fail_fast_or_continue(Ok(()), false).unwrap();
        assert!(result);
    }

    #[test]
    fn test_fail_fast_or_continue_err_with_fail_fast_propagates() {
        let result = fail_fast_or_continue(Err(anyhow::anyhow!("boom")), true);
        assert!(result.is_err());
    }

    #[test]
    fn test_fail_fast_or_continue_err_without_fail_fast_returns_false() {
        let result = fail_fast_or_continue(Err(anyhow::anyhow!("boom")), false).unwrap();
        assert!(!result);
    }

    fn make_build(distro: &str) -> Build {
        Build {
            distro: distro.to_string(),
            package_name: "p".to_string(),
            maintainer: "m".to_string(),
            homepage: "h".to_string(),
            description: "d".to_string(),
            build_dependencies: vec![],
            runtime_dependencies: vec![],
            before_build_script: None,
            rpm: None,
            deb: None,
            pacman: None,
            rest: HashMap::new(),
        }
    }

    fn make_config(builds: Vec<Build>) -> Config {
        Config {
            version_extractors: VersionExtractors::default(),
            builds,
            repositories: Repositories::default(),
            secrets: HashMap::new(),
            image_caches: None,
            ignore_source_files: vec![],
        }
    }

    fn make_job(distros: Vec<String>) -> JobArgs {
        JobArgs {
            distros,
            build_dir: PathBuf::from("/tmp/x"),
            fail_fast: false,
            image_cache: None,
        }
    }

    #[test]
    fn test_detect_builds_drops_unknown_distros() {
        let config = make_config(vec![make_build("debian_12"), make_build("nonexistent_99")]);
        let detected: Vec<_> = detect_builds(make_job(vec![]), config).collect();
        assert_eq!(detected.len(), 1);
        assert_eq!(detected[0].distro, "debian_12");
    }

    #[test]
    fn test_detect_builds_filters_by_requested_distros() {
        let config = make_config(vec![make_build("debian_12"), make_build("fedora_40")]);
        let detected: Vec<_> = detect_builds(make_job(vec!["fedora_40".to_string()]), config).collect();
        assert_eq!(detected.len(), 1);
        assert_eq!(detected[0].distro, "fedora_40");
    }

    #[test]
    fn test_detect_builds_empty_filter_keeps_all_known() {
        let config = make_config(vec![make_build("debian_12"), make_build("fedora_40")]);
        let detected: Vec<_> = detect_builds(make_job(vec![]), config).collect();
        assert_eq!(detected.len(), 2);
    }
}
