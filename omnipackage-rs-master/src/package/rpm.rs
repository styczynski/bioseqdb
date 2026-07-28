use crate::config::{Build, ImageCache, Repository};
use crate::distros::Distro;
use crate::gpg::{Gpg, Key};
use crate::job_variables::JobVariables;
use crate::package::{Package, SetupStage};
use crate::template::{Template, Var};
use anyhow::{Context, Result};
use std::collections::HashMap;
use std::path::{Path, PathBuf};

#[derive(Debug, Clone)]
pub struct Rpm {
    pub distro: Distro,
    pub source_dir: PathBuf,
    pub job_variables: JobVariables,
    pub distro_build_dir: PathBuf,
    pub image_cache: Option<ImageCache>,
    pub ignore_source_files: Vec<String>,

    mounts: HashMap<String, String>,
    commands: Vec<String>,
    build_output_dir: PathBuf,
    setup_stages: Vec<SetupStage>,
    gpgkey: Option<Key>,
}

impl Rpm {
    pub fn new(distro: Distro, source_dir: PathBuf, job_variables: JobVariables, distro_build_dir: PathBuf, image_cache: Option<ImageCache>, ignore_source_files: Vec<String>) -> Self {
        Self {
            distro,
            source_dir,
            job_variables,
            image_cache,
            ignore_source_files,
            distro_build_dir: distro_build_dir.clone(),
            mounts: HashMap::new(),
            commands: Vec::new(),
            build_output_dir: distro_build_dir.clone(),
            setup_stages: Vec::new(),
            gpgkey: None,
        }
    }

    fn write_repo_file(&self, repo_dir: &Path, project_slug: &str, distro_name: &str, distro_url: &str) -> Result<(), anyhow::Error> {
        let content = format!(
            "[{project_slug}]\n\
             name={project_slug} ({distro_name})\n\
             type=rpm-md\n\
             baseurl={distro_url}\n\
             gpgcheck=1\n\
             gpgkey={distro_url}/repodata/repomd.xml.key\n\
             enabled=1\n"
        );

        Ok(std::fs::write(repo_dir.join(format!("{}.repo", project_slug)), content)?)
    }

    fn output_path(&self) -> PathBuf {
        self.distro_build_dir()
    }
}

impl Package for Rpm {
    fn setup_build(&mut self, config: Build) -> Result<(), anyhow::Error> {
        self.prepare_build_dir()?;
        let specfile_path_template_path = config.rpm.clone().ok_or_else(|| anyhow::anyhow!("rpm config is missing"))?.spec_template;

        let rpmbuild_path = self.output_path();
        std::fs::create_dir_all(&rpmbuild_path).with_context(|| format!("cannot create directory {}", rpmbuild_path.display()))?;

        let source_folder_name = format!("{}-{}", config.package_name, self.job_variables.version);
        let specfile_name = format!("{}-{}.spec", source_folder_name, self.distro.id);

        let mut template_vars: HashMap<String, Var> = self.job_variables.to_template_vars();
        template_vars.extend(config.to_template_vars());
        template_vars.insert("source_folder_name".to_string(), source_folder_name.clone().into());
        let template = Template::from_file(self.source_dir.join(&specfile_path_template_path))?;
        template.render_to_file(template_vars, rpmbuild_path.join(&specfile_name))?;

        self.mounts.insert(self.source_dir.to_string_lossy().to_string(), "/source".to_string());
        self.mounts.insert(self.output_path().to_string_lossy().to_string(), "/root/rpmbuild".to_string());

        if self.image_cache.is_none() {
            self.commands.extend(self.distro.setup(&config.build_dependencies));
            if let Some(bbs) = self.before_build_script("/source", &config) {
                self.commands.push(bbs);
            }
        }
        let rsync_excludes: String = self.ignore_source_files.iter().map(|p| format!(" --exclude='{p}'")).collect();
        self.commands.extend([
            "rpmdev-setuptree".to_string(),
            "rm -rf /root/rpmbuild/SOURCES/*".to_string(),
            format!("rsync -a{rsync_excludes} /source/ /root/rpmbuild/SOURCES/{source_folder_name}/"),
            "cd /root/rpmbuild/SOURCES/".to_string(),
            format!("tar -cvzf {source_folder_name}.tar.gz {source_folder_name}/"),
            format!("cd /root/rpmbuild/SOURCES/{source_folder_name}/"),
            format!("QA_RPATHS=$(( 0x0001|0x0010|0x0002|0x0004|0x0008|0x0020 )) rpmbuild --clean -bb /root/rpmbuild/{specfile_name}"),
        ]);

        self.build_output_dir = rpmbuild_path.join("RPMS");
        self.setup_stages.push(SetupStage::Build);

        Ok(())
    }

    fn setup_repository(&mut self, config: Repository) -> Result<(), anyhow::Error> {
        let gpgkey = self.prepare_gpgkey(&config)?;
        let (home_dir, repo_dir) = self.prepare_repository(&gpgkey)?;

        let key_id = Gpg::new().key_id(&gpgkey.priv_key)?;

        self.mounts.insert(home_dir.to_string_lossy().to_string(), "/omnipackage".to_string());
        self.mounts.insert(repo_dir.to_string_lossy().to_string(), "/repo".to_string());
        self.mounts.insert(self.output_path().to_string_lossy().to_string(), "/root/rpmbuild".to_string());

        if self.image_cache.is_none() {
            self.commands.extend(self.distro.setup_repo.clone());
        }
        self.commands.extend(self.import_gpg_keys_commands());
        self.commands.extend([
            "cd /repo".to_string(),
            "cp /root/rpmbuild/RPMS/**/*.rpm /repo/".to_string(),
            "rpm --import public.key".to_string(),
            format!("rpm --define '_signature gpg' --define '_gpg_name {key_id}' --addsign *.rpm"),
            "createrepo --retain-old-md=0 --compatibility .".to_string(),
            "gpg --no-tty --batch --detach-sign --armor --verbose --yes --always-trust repodata/repomd.xml".to_string(),
            "mv public.key repodata/repomd.xml.key".to_string(),
        ]);

        self.build_output_dir = repo_dir.clone();
        self.setup_stages.push(SetupStage::Repository);
        self.gpgkey = Some(gpgkey);

        self.write_repo_file(&repo_dir, &config.project_slug(), &self.distro.name, &self.distro_url(&config))
    }

    fn clone_box(&self) -> Box<dyn Package> {
        Box::new(self.clone())
    }

    fn source_dir(&self) -> PathBuf {
        self.source_dir.clone()
    }

    fn distro_build_dir(&self) -> PathBuf {
        self.distro_build_dir.clone()
    }

    fn distro(&self) -> Distro {
        self.distro.clone()
    }

    fn mounts(&self) -> HashMap<String, String> {
        self.mounts.clone()
    }

    fn commands(&self) -> Vec<String> {
        self.commands.clone()
    }

    fn build_output_dir(&self) -> PathBuf {
        self.build_output_dir.clone()
    }

    fn setup_stages(&self) -> Vec<SetupStage> {
        self.setup_stages.clone()
    }

    fn gpgkey(&self) -> Option<Key> {
        self.gpgkey.clone()
    }

    fn image_cache(&self) -> Option<ImageCache> {
        self.image_cache.clone()
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::config::{Build, Repository, RepositoryProvider, RpmConfig, S3Config};
    use crate::distros::Distros;
    use crate::gpg::Gpg;
    use crate::job_variables::JobVariables;
    use std::collections::HashMap;

    fn gpg_available() -> bool {
        std::process::Command::new("gpg")
            .arg("--version")
            .stdout(std::process::Stdio::null())
            .stderr(std::process::Stdio::null())
            .status()
            .map(|s| s.success())
            .unwrap_or(false)
    }

    fn make_distro() -> Distro {
        Distros::get().by_id("fedora_38")
    }

    fn make_job_variables() -> JobVariables {
        JobVariables::new("1.2.3".to_string())
    }

    fn make_rpm(dir: &tempfile::TempDir) -> Rpm {
        make_rpm_with_ignores(dir, vec![])
    }

    fn make_rpm_with_ignores(dir: &tempfile::TempDir, ignore_source_files: Vec<String>) -> Rpm {
        let source_dir = dir.path().join("source");
        let build_dir = dir.path().join("build");
        std::fs::create_dir_all(&source_dir).unwrap();
        std::fs::create_dir_all(&build_dir).unwrap();
        Rpm::new(make_distro(), source_dir, make_job_variables(), build_dir, None, ignore_source_files)
    }

    fn make_build_config(dir: &tempfile::TempDir) -> Build {
        let spec_template = "myapp.spec.liquid";
        std::fs::write(dir.path().join("source").join(spec_template), "Name: {{ package_name }}\nVersion: {{ version }}\n").unwrap();

        Build {
            distro: "fedora_38".to_string(),
            package_name: "myapp".to_string(),
            maintainer: "Test <test@test.com>".to_string(),
            homepage: "https://example.com".to_string(),
            description: "Test package".to_string(),
            build_dependencies: vec!["gcc".to_string(), "make".to_string()],
            runtime_dependencies: vec![],
            before_build_script: None,
            rpm: Some(RpmConfig {
                spec_template: spec_template.to_string(),
            }),
            deb: None,
            pacman: None,
            rest: HashMap::new(),
        }
    }

    fn make_repository_config(gpg_private_key: &str) -> Repository {
        use base64::{Engine, engine::general_purpose};
        let encoded = general_purpose::STANDARD.encode(gpg_private_key);
        Repository {
            name: "test-repo".to_string(),
            provider: RepositoryProvider::S3,
            localfs: None,
            s3: Some(S3Config {
                bucket: "test-bucket".to_string(),
                path_in_bucket: Some("packages".to_string()),
                bucket_public_url: Some("https://cdn.example.com".to_string()),
                endpoint: "https://s3.example.com".to_string(),
                access_key_id: "keyid".to_string(),
                secret_access_key: "secret".to_string(),
                region: Some("us-east-1".to_string()),
                force_path_style: false,
                cloudflare_zone_id: None,
                cloudflare_api_token: None,
            }),
            gpg_private_key_base64: encoded,
            package_name: "myapp".to_string(),
            retain_packages: 0,
            rest: HashMap::new(),
        }
    }

    // ── new() ────────────────────────────────────────────────────────────────

    #[test]
    fn test_new_initializes_fields() {
        let dir = tempfile::tempdir().unwrap();
        let rpm = make_rpm(&dir);

        assert_eq!(rpm.distro().id, "fedora_38");
        assert_eq!(rpm.job_variables.version, "1.2.3");
        assert!(rpm.mounts().is_empty());
        assert!(rpm.commands().is_empty());
        assert!(rpm.setup_stages().is_empty());
        assert!(rpm.gpgkey().is_none());
        assert_eq!(rpm.build_output_dir(), rpm.distro_build_dir());
    }

    // ── setup_build() ────────────────────────────────────────────────────────

    #[test]
    fn test_setup_build_creates_output_dir() {
        let dir = tempfile::tempdir().unwrap();
        let mut rpm = make_rpm(&dir);
        let config = make_build_config(&dir);

        rpm.setup_build(config).unwrap();

        // build_output_dir points to RPMS/ which is created by rpmbuild at runtime,
        // but the parent rpmbuild dir is created by setup_build
        assert!(rpm.build_output_dir().parent().unwrap().exists());
    }

    #[test]
    fn test_setup_build_renders_spec_file() {
        let dir = tempfile::tempdir().unwrap();
        let mut rpm = make_rpm(&dir);
        let config = make_build_config(&dir);

        rpm.setup_build(config).unwrap();

        let spec_files: Vec<_> = std::fs::read_dir(rpm.distro_build_dir())
            .unwrap()
            .filter_map(Result::ok)
            .filter(|e| e.path().extension().map(|x| x == "spec").unwrap_or(false))
            .collect();

        assert_eq!(spec_files.len(), 1);
        let content = std::fs::read_to_string(spec_files[0].path()).unwrap();
        assert!(content.contains("Name: myapp"));
        assert!(content.contains("Version: 1.2.3"));
    }

    #[test]
    fn test_setup_build_adds_mounts() {
        let dir = tempfile::tempdir().unwrap();
        let mut rpm = make_rpm(&dir);
        let config = make_build_config(&dir);

        rpm.setup_build(config).unwrap();

        let mounts = rpm.mounts();
        assert!(mounts.values().any(|v| v == "/source"));
        assert!(mounts.values().any(|v| v == "/root/rpmbuild"));
    }

    #[test]
    fn test_setup_build_adds_commands() {
        let dir = tempfile::tempdir().unwrap();
        let mut rpm = make_rpm(&dir);
        let config = make_build_config(&dir);

        rpm.setup_build(config).unwrap();

        let commands = rpm.commands();
        assert!(!commands.is_empty());
        assert!(commands.iter().any(|c| c.contains("rpmbuild")));
        assert!(commands.iter().any(|c| c.contains("rpmdev-setuptree")));
    }

    #[test]
    fn test_setup_build_adds_distro_setup_commands() {
        let dir = tempfile::tempdir().unwrap();
        let mut rpm = make_rpm(&dir);
        let config = make_build_config(&dir);
        let distro_setup = make_distro().setup(&config.build_dependencies.clone());

        rpm.setup_build(config).unwrap();

        let commands = rpm.commands();
        for expected in &distro_setup {
            assert!(commands.contains(expected), "missing distro setup command: {}", expected);
        }
    }

    #[test]
    fn test_setup_build_adds_build_to_stages() {
        let dir = tempfile::tempdir().unwrap();
        let mut rpm = make_rpm(&dir);

        rpm.setup_build(make_build_config(&dir)).unwrap();

        assert!(rpm.setup_stages().contains(&SetupStage::Build));
    }

    #[test]
    fn test_setup_build_sets_build_output_dir_to_rpms() {
        let dir = tempfile::tempdir().unwrap();
        let mut rpm = make_rpm(&dir);

        rpm.setup_build(make_build_config(&dir)).unwrap();

        assert!(rpm.build_output_dir().ends_with("RPMS"));
    }

    #[test]
    fn test_setup_build_fails_without_rpm_config() {
        let dir = tempfile::tempdir().unwrap();
        let mut rpm = make_rpm(&dir);
        let mut config = make_build_config(&dir);
        config.rpm = None;

        assert!(rpm.setup_build(config).is_err());
    }

    #[test]
    fn test_setup_build_passes_ignore_source_files_to_rsync() {
        let dir = tempfile::tempdir().unwrap();
        let ignores = vec![".git".to_string(), "node_modules".to_string(), "*.log".to_string()];
        let mut rpm = make_rpm_with_ignores(&dir, ignores);

        rpm.setup_build(make_build_config(&dir)).unwrap();

        let rsync_cmd = rpm.commands().into_iter().find(|c| c.starts_with("rsync ")).expect("rsync command not found");
        assert!(rsync_cmd.contains("--exclude='.git'"));
        assert!(rsync_cmd.contains("--exclude='node_modules'"));
        assert!(rsync_cmd.contains("--exclude='*.log'"));
        assert!(rsync_cmd.contains("/source/ /root/rpmbuild/SOURCES/"));
    }

    #[test]
    fn test_setup_build_no_excludes_when_ignore_source_files_empty() {
        let dir = tempfile::tempdir().unwrap();
        let mut rpm = make_rpm(&dir);

        rpm.setup_build(make_build_config(&dir)).unwrap();

        let rsync_cmd = rpm.commands().into_iter().find(|c| c.starts_with("rsync ")).expect("rsync command not found");
        assert!(!rsync_cmd.contains("--exclude"));
        let tar_cmd = rpm.commands().into_iter().find(|c| c.contains("tar -cvzf")).expect("tar command not found");
        assert!(!tar_cmd.contains("--exclude"));
    }

    #[test]
    fn test_setup_build_with_before_build_script() {
        let dir = tempfile::tempdir().unwrap();
        let mut rpm = make_rpm(&dir);
        let mut config = make_build_config(&dir);
        config.before_build_script = Some("build.sh".to_string());

        rpm.setup_build(config).unwrap();

        assert!(rpm.commands().iter().any(|c| c.contains("build.sh")));
    }

    // ── setup_repository() ───────────────────────────────────────────────────

    #[test]
    fn test_setup_repository_adds_repository_to_stages() {
        if !gpg_available() {
            return;
        }
        let dir = tempfile::tempdir().unwrap();
        let mut rpm = make_rpm(&dir);
        let gpg_key = Gpg::new().generate_keys("Test", "test@example.com").unwrap();

        rpm.setup_repository(make_repository_config(&gpg_key.priv_key)).unwrap();

        assert!(rpm.setup_stages().contains(&SetupStage::Repository));
    }

    #[test]
    fn test_setup_repository_stores_gpg_key() {
        if !gpg_available() {
            return;
        }
        let dir = tempfile::tempdir().unwrap();
        let mut rpm = make_rpm(&dir);
        let gpg_key = Gpg::new().generate_keys("Test", "test@example.com").unwrap();

        rpm.setup_repository(make_repository_config(&gpg_key.priv_key)).unwrap();

        assert!(rpm.gpgkey().is_some());
    }

    #[test]
    fn test_setup_repository_adds_mounts() {
        if !gpg_available() {
            return;
        }
        let dir = tempfile::tempdir().unwrap();
        let mut rpm = make_rpm(&dir);
        let gpg_key = Gpg::new().generate_keys("Test", "test@example.com").unwrap();

        rpm.setup_repository(make_repository_config(&gpg_key.priv_key)).unwrap();

        let mounts = rpm.mounts();
        assert!(mounts.values().any(|v| v == "/omnipackage"));
        assert!(mounts.values().any(|v| v == "/repo"));
    }

    #[test]
    fn test_setup_repository_adds_commands() {
        if !gpg_available() {
            return;
        }
        let dir = tempfile::tempdir().unwrap();
        let mut rpm = make_rpm(&dir);
        let gpg_key = Gpg::new().generate_keys("Test", "test@example.com").unwrap();

        rpm.setup_repository(make_repository_config(&gpg_key.priv_key)).unwrap();

        let commands = rpm.commands();
        assert!(commands.iter().any(|c| c.contains("createrepo")));
        assert!(commands.iter().any(|c| c.contains("--addsign")));
        assert!(commands.iter().any(|c| c.contains("gpg")));
    }

    #[test]
    fn test_setup_repository_writes_repo_file() {
        if !gpg_available() {
            return;
        }
        let dir = tempfile::tempdir().unwrap();
        let mut rpm = make_rpm(&dir);
        let gpg_key = Gpg::new().generate_keys("Test", "test@example.com").unwrap();

        rpm.setup_repository(make_repository_config(&gpg_key.priv_key)).unwrap();

        let repo_file = rpm.build_output_dir().join("myapp.repo");
        assert!(repo_file.exists());
        let content = std::fs::read_to_string(repo_file).unwrap();
        assert!(content.contains("[myapp]"));
        assert!(content.contains("gpgcheck=1"));
    }

    #[test]
    fn test_setup_repository_writes_gpg_keys_to_disk() {
        if !gpg_available() {
            return;
        }
        let dir = tempfile::tempdir().unwrap();
        let mut rpm = make_rpm(&dir);
        let gpg_key = Gpg::new().generate_keys("Test", "test@example.com").unwrap();

        rpm.setup_repository(make_repository_config(&gpg_key.priv_key)).unwrap();

        let home_dir = rpm.distro_build_dir().join("home");
        let repo_dir = rpm.build_output_dir();
        assert!(home_dir.join("key.priv").exists());
        assert!(repo_dir.join("public.key").exists());
    }

    #[test]
    fn test_setup_repository_fails_with_invalid_gpg_key() {
        let dir = tempfile::tempdir().unwrap();
        let mut rpm = make_rpm(&dir);

        use base64::{Engine, engine::general_purpose};
        let mut config = make_repository_config("dummy");
        config.gpg_private_key_base64 = general_purpose::STANDARD.encode("not a real key");

        assert!(rpm.setup_repository(config).is_err());
    }

    // ── clone_box() ──────────────────────────────────────────────────────────

    #[test]
    fn test_clone_box() {
        let dir = tempfile::tempdir().unwrap();
        let rpm = make_rpm(&dir);
        let boxed: Box<dyn Package> = Box::new(rpm);
        let cloned = boxed.clone();

        assert_eq!(cloned.distro().id, boxed.distro().id);
        assert_eq!(cloned.source_dir(), boxed.source_dir());
        assert_eq!(cloned.build_output_dir(), boxed.build_output_dir());
    }

    // ── write_repo_file() ────────────────────────────────────────────────────

    #[test]
    fn test_write_repo_file_content() {
        let dir = tempfile::tempdir().unwrap();
        let rpm = make_rpm(&dir);
        let repo_dir = dir.path().join("repo");
        std::fs::create_dir_all(&repo_dir).unwrap();

        rpm.write_repo_file(&repo_dir, "myapp", "Fedora 38", "https://cdn.example.com/fedora_38").unwrap();

        let content = std::fs::read_to_string(repo_dir.join("myapp.repo")).unwrap();
        assert!(content.contains("[myapp]"));
        assert!(content.contains("name=myapp (Fedora 38)"));
        assert!(content.contains("baseurl=https://cdn.example.com/fedora_38"));
        assert!(content.contains("gpgcheck=1"));
        assert!(content.contains("enabled=1"));
        assert!(content.contains("type=rpm-md"));
    }
}
