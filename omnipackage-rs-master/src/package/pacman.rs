use crate::config::{Build, ImageCache, Repository};
use crate::distros::Distro;
use crate::gpg::Key;
use crate::job_variables::JobVariables;
use crate::package::{Package, SetupStage};
use crate::template::{Template, Var};
use anyhow::{Context, Result};
use std::collections::HashMap;
use std::path::PathBuf;

#[derive(Debug, Clone)]
pub struct Pacman {
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

impl Pacman {
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

    // Working dir for makepkg (PKGBUILD + extracted source). Bind-mounted to /work.
    fn work_path(&self) -> PathBuf {
        self.distro_build_dir().join("work")
    }

    // Where makepkg drops the built package (PKGDEST). Bind-mounted to /output.
    fn output_path(&self) -> PathBuf {
        self.distro_build_dir().join("output")
    }
}

impl Package for Pacman {
    fn setup_build(&mut self, config: Build) -> Result<(), anyhow::Error> {
        self.prepare_build_dir()?;
        let pkgbuild_template_path = config.pacman.clone().ok_or_else(|| anyhow::anyhow!("pacman config is missing"))?.pkgbuild_template;

        let work_path = self.work_path();
        let output_path = self.output_path();
        std::fs::create_dir_all(&work_path).with_context(|| format!("cannot create directory {}", work_path.display()))?;
        std::fs::create_dir_all(&output_path).with_context(|| format!("cannot create directory {}", output_path.display()))?;

        let source_folder_name = format!("{}-{}", config.package_name, self.job_variables.version);

        let mut template_vars: HashMap<String, Var> = self.job_variables.to_template_vars();
        template_vars.extend(config.to_template_vars());
        template_vars.insert("source_folder_name".to_string(), source_folder_name.clone().into());
        let template = Template::from_file(self.source_dir.join(&pkgbuild_template_path))?;
        template.render_to_file(template_vars, work_path.join("PKGBUILD"))?;

        self.mounts.insert(self.source_dir.to_string_lossy().to_string(), "/source".to_string());
        self.mounts.insert(work_path.to_string_lossy().to_string(), "/work".to_string());
        self.mounts.insert(output_path.to_string_lossy().to_string(), "/output".to_string());

        if self.image_cache.is_none() {
            self.commands.extend(self.distro.setup(&config.build_dependencies));
            if let Some(bbs) = self.before_build_script("/source", &config) {
                self.commands.push(bbs);
            }
        }
        let rsync_excludes: String = self.ignore_source_files.iter().map(|p| format!(" --exclude='{p}'")).collect();
        // makepkg refuses to run as root, so build as an unprivileged user. It's created here (not in
        // the distro setup) so it also exists on cached images, with a dedicated name that won't collide
        // with a `builder` user some base images already ship (e.g. manjaro, whose builder home isn't
        // even /home/builder). Everything else runs as the container's root user.
        self.commands.extend([
            format!("rsync -a{rsync_excludes} /source/ /work/{source_folder_name}/"),
            "cd /work".to_string(),
            format!("tar -cvzf {source_folder_name}.tar.gz {source_folder_name}/"),
            // !debug: don't emit a separate -debug package (matches the rpm spec's `%define debug_package %{nil}`).
            // !lto: makepkg enables LTO by default, which breaks crates that link prebuilt C/assembly objects
            // (e.g. aws-lc-sys: `undefined symbol: aws_lc_*_SHA512`); the rpm/deb paths don't LTO either.
            "echo 'OPTIONS+=(!debug !lto)' >> /etc/makepkg.conf".to_string(),
            // -m gives omnibuild a fresh /home/omnibuild it owns (cargo needs a writable ~/.cargo).
            "useradd -m -s /bin/bash omnibuild 2>/dev/null || true".to_string(),
            // NOPASSWD:SETENV lets the `sudo -E` below carry the injected build secrets through.
            "echo 'omnibuild ALL=(ALL) NOPASSWD:SETENV: ALL' > /etc/sudoers.d/omnibuild".to_string(),
            "chown -R omnibuild:omnibuild /work /output".to_string(),
            // HOME/PKGDEST are set inline so makepkg writes to omnibuild's home and drops the
            // package into the bind-mounted output dir.
            "sudo -E -u omnibuild bash -c 'cd /work && HOME=/home/omnibuild PKGDEST=/output makepkg -f --nodeps'".to_string(),
        ]);

        self.build_output_dir = output_path;
        self.setup_stages.push(SetupStage::Build);

        Ok(())
    }

    fn setup_repository(&mut self, config: Repository) -> Result<(), anyhow::Error> {
        let gpgkey = self.prepare_gpgkey(&config)?;
        let (home_dir, repo_dir) = self.prepare_repository(&gpgkey)?;

        self.mounts.insert(home_dir.to_string_lossy().to_string(), "/omnipackage".to_string());
        self.mounts.insert(repo_dir.to_string_lossy().to_string(), "/repo".to_string());
        self.mounts.insert(self.output_path().to_string_lossy().to_string(), "/output".to_string());

        if self.image_cache.is_none() {
            self.commands.extend(self.distro.setup_repo.clone());
        }
        self.commands.extend(self.import_gpg_keys_commands());
        // public.key stays in the repo root (written by prepare_repository) — install_steps
        // fetch it from there. The repo db is named after the project slug so pacman finds it
        // at `Server/<slug>.db`.
        let db_name = format!("{}.db.tar.gz", config.project_slug());
        self.commands.extend([
            "cd /repo".to_string(),
            "cp /output/*.pkg.tar.zst /repo/".to_string(),
            r#"for f in *.pkg.tar.zst; do gpg --no-tty --batch --yes --detach-sign --no-armor "$f"; done"#.to_string(),
            format!("repo-add -s -v {db_name} *.pkg.tar.zst"),
        ]);

        self.build_output_dir = repo_dir;
        self.setup_stages.push(SetupStage::Repository);
        self.gpgkey = Some(gpgkey);

        Ok(())
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
    use crate::config::{Build, PacmanConfig, Repository, RepositoryProvider, S3Config};
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
        Distros::get().by_id("arch")
    }

    fn make_job_variables() -> JobVariables {
        JobVariables::new("1.2.3".to_string())
    }

    fn make_pacman(dir: &tempfile::TempDir) -> Pacman {
        make_pacman_with_ignores(dir, vec![])
    }

    fn make_pacman_with_ignores(dir: &tempfile::TempDir, ignore_source_files: Vec<String>) -> Pacman {
        let source_dir = dir.path().join("source");
        let build_dir = dir.path().join("build");
        std::fs::create_dir_all(&source_dir).unwrap();
        std::fs::create_dir_all(&build_dir).unwrap();
        Pacman::new(make_distro(), source_dir, make_job_variables(), build_dir, None, ignore_source_files)
    }

    fn make_build_config(dir: &tempfile::TempDir) -> Build {
        let pkgbuild_template = "PKGBUILD.liquid";
        std::fs::write(dir.path().join("source").join(pkgbuild_template), "pkgname={{ package_name }}\npkgver={{ version }}\n").unwrap();

        Build {
            distro: "arch".to_string(),
            package_name: "myapp".to_string(),
            maintainer: "Test <test@test.com>".to_string(),
            homepage: "https://example.com".to_string(),
            description: "Test package".to_string(),
            build_dependencies: vec!["gcc".to_string(), "make".to_string()],
            runtime_dependencies: vec![],
            before_build_script: None,
            rpm: None,
            deb: None,
            pacman: Some(PacmanConfig {
                pkgbuild_template: pkgbuild_template.to_string(),
            }),
            rest: HashMap::new(),
        }
    }

    fn make_repository_config(gpg_private_key: &str) -> Repository {
        use base64::{Engine, engine::general_purpose};
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
            gpg_private_key_base64: general_purpose::STANDARD.encode(gpg_private_key),
            package_name: "myapp".to_string(),
            retain_packages: 0,
            rest: HashMap::new(),
        }
    }

    // ── new() ────────────────────────────────────────────────────────────────

    #[test]
    fn test_new_initializes_fields() {
        let dir = tempfile::tempdir().unwrap();
        let pacman = make_pacman(&dir);

        assert_eq!(pacman.distro().id, "arch");
        assert_eq!(pacman.job_variables.version, "1.2.3");
        assert!(pacman.mounts().is_empty());
        assert!(pacman.commands().is_empty());
        assert!(pacman.setup_stages().is_empty());
        assert!(pacman.gpgkey().is_none());
        assert_eq!(pacman.build_output_dir(), pacman.distro_build_dir());
    }

    // ── setup_build() ────────────────────────────────────────────────────────

    #[test]
    fn test_setup_build_creates_output_dir() {
        let dir = tempfile::tempdir().unwrap();
        let mut pacman = make_pacman(&dir);

        pacman.setup_build(make_build_config(&dir)).unwrap();

        assert!(pacman.build_output_dir().exists());
    }

    #[test]
    fn test_setup_build_renders_pkgbuild() {
        let dir = tempfile::tempdir().unwrap();
        let mut pacman = make_pacman(&dir);

        pacman.setup_build(make_build_config(&dir)).unwrap();

        let pkgbuild = pacman.distro_build_dir().join("work").join("PKGBUILD");
        assert!(pkgbuild.exists());
        let content = std::fs::read_to_string(pkgbuild).unwrap();
        assert!(content.contains("pkgname=myapp"));
        assert!(content.contains("pkgver=1.2.3"));
    }

    #[test]
    fn test_setup_build_adds_mounts() {
        let dir = tempfile::tempdir().unwrap();
        let mut pacman = make_pacman(&dir);

        pacman.setup_build(make_build_config(&dir)).unwrap();

        let mounts = pacman.mounts();
        assert!(mounts.values().any(|v| v == "/source"));
        assert!(mounts.values().any(|v| v == "/work"));
        assert!(mounts.values().any(|v| v == "/output"));
    }

    #[test]
    fn test_setup_build_adds_commands() {
        let dir = tempfile::tempdir().unwrap();
        let mut pacman = make_pacman(&dir);

        pacman.setup_build(make_build_config(&dir)).unwrap();

        let commands = pacman.commands();
        assert!(!commands.is_empty());
        assert!(commands.iter().any(|c| c.contains("makepkg")));
        assert!(commands.iter().any(|c| c.contains("useradd") && c.contains("omnibuild")));
        assert!(commands.iter().any(|c| c.contains("-u omnibuild")));
        assert!(commands.iter().any(|c| c.contains("rsync") && c.contains("/source/ /work/")));
    }

    #[test]
    fn test_setup_build_adds_distro_setup_commands() {
        let dir = tempfile::tempdir().unwrap();
        let mut pacman = make_pacman(&dir);
        let config = make_build_config(&dir);
        let distro_setup = make_distro().setup(&config.build_dependencies.clone());

        pacman.setup_build(config).unwrap();

        let commands = pacman.commands();
        for expected in &distro_setup {
            assert!(commands.contains(expected), "missing distro setup command: {}", expected);
        }
    }

    #[test]
    fn test_setup_build_adds_build_to_stages() {
        let dir = tempfile::tempdir().unwrap();
        let mut pacman = make_pacman(&dir);

        pacman.setup_build(make_build_config(&dir)).unwrap();

        assert!(pacman.setup_stages().contains(&SetupStage::Build));
    }

    #[test]
    fn test_setup_build_sets_build_output_dir_to_output() {
        let dir = tempfile::tempdir().unwrap();
        let mut pacman = make_pacman(&dir);

        pacman.setup_build(make_build_config(&dir)).unwrap();

        assert!(pacman.build_output_dir().ends_with("output"));
    }

    #[test]
    fn test_setup_build_fails_without_pacman_config() {
        let dir = tempfile::tempdir().unwrap();
        let mut pacman = make_pacman(&dir);
        let mut config = make_build_config(&dir);
        config.pacman = None;

        assert!(pacman.setup_build(config).is_err());
    }

    #[test]
    fn test_setup_build_passes_ignore_source_files_to_rsync() {
        let dir = tempfile::tempdir().unwrap();
        let ignores = vec![".git".to_string(), "node_modules".to_string(), "*.log".to_string()];
        let mut pacman = make_pacman_with_ignores(&dir, ignores);

        pacman.setup_build(make_build_config(&dir)).unwrap();

        let rsync_cmd = pacman.commands().into_iter().find(|c| c.starts_with("rsync ")).expect("rsync command not found");
        assert!(rsync_cmd.contains("--exclude='.git'"));
        assert!(rsync_cmd.contains("--exclude='node_modules'"));
        assert!(rsync_cmd.contains("--exclude='*.log'"));
        assert!(rsync_cmd.contains("/source/ /work/"));
    }

    #[test]
    fn test_setup_build_no_excludes_when_ignore_source_files_empty() {
        let dir = tempfile::tempdir().unwrap();
        let mut pacman = make_pacman(&dir);

        pacman.setup_build(make_build_config(&dir)).unwrap();

        let rsync_cmd = pacman.commands().into_iter().find(|c| c.starts_with("rsync ")).expect("rsync command not found");
        assert!(!rsync_cmd.contains("--exclude"));
    }

    #[test]
    fn test_setup_build_with_before_build_script() {
        let dir = tempfile::tempdir().unwrap();
        let mut pacman = make_pacman(&dir);
        let mut config = make_build_config(&dir);
        config.before_build_script = Some("build.sh".to_string());

        pacman.setup_build(config).unwrap();

        assert!(pacman.commands().iter().any(|c| c.contains("build.sh")));
    }

    // ── setup_repository() ───────────────────────────────────────────────────

    #[test]
    fn test_setup_repository_adds_repository_to_stages() {
        if !gpg_available() {
            return;
        }
        let dir = tempfile::tempdir().unwrap();
        let mut pacman = make_pacman(&dir);
        let gpg_key = Gpg::new().generate_keys("Test", "test@example.com").unwrap();

        pacman.setup_repository(make_repository_config(&gpg_key.priv_key)).unwrap();

        assert!(pacman.setup_stages().contains(&SetupStage::Repository));
    }

    #[test]
    fn test_setup_repository_stores_gpg_key() {
        if !gpg_available() {
            return;
        }
        let dir = tempfile::tempdir().unwrap();
        let mut pacman = make_pacman(&dir);
        let gpg_key = Gpg::new().generate_keys("Test", "test@example.com").unwrap();

        pacman.setup_repository(make_repository_config(&gpg_key.priv_key)).unwrap();

        assert!(pacman.gpgkey().is_some());
    }

    #[test]
    fn test_setup_repository_adds_mounts() {
        if !gpg_available() {
            return;
        }
        let dir = tempfile::tempdir().unwrap();
        let mut pacman = make_pacman(&dir);
        let gpg_key = Gpg::new().generate_keys("Test", "test@example.com").unwrap();

        pacman.setup_repository(make_repository_config(&gpg_key.priv_key)).unwrap();

        let mounts = pacman.mounts();
        assert!(mounts.values().any(|v| v == "/omnipackage"));
        assert!(mounts.values().any(|v| v == "/repo"));
        assert!(mounts.values().any(|v| v == "/output"));
    }

    #[test]
    fn test_setup_repository_adds_commands() {
        if !gpg_available() {
            return;
        }
        let dir = tempfile::tempdir().unwrap();
        let mut pacman = make_pacman(&dir);
        let gpg_key = Gpg::new().generate_keys("Test", "test@example.com").unwrap();

        pacman.setup_repository(make_repository_config(&gpg_key.priv_key)).unwrap();

        let commands = pacman.commands();
        assert!(commands.iter().any(|c| c.contains("repo-add") && c.contains("myapp.db.tar.gz")));
        assert!(commands.iter().any(|c| c.contains("--detach-sign")));
    }

    #[test]
    fn test_setup_repository_writes_gpg_keys_to_disk() {
        if !gpg_available() {
            return;
        }
        let dir = tempfile::tempdir().unwrap();
        let mut pacman = make_pacman(&dir);
        let gpg_key = Gpg::new().generate_keys("Test", "test@example.com").unwrap();

        pacman.setup_repository(make_repository_config(&gpg_key.priv_key)).unwrap();

        let home_dir = pacman.distro_build_dir().join("home");
        let repo_dir = pacman.build_output_dir();
        assert!(home_dir.join("key.priv").exists());
        assert!(repo_dir.join("public.key").exists());
    }

    #[test]
    fn test_setup_repository_fails_with_invalid_gpg_key() {
        let dir = tempfile::tempdir().unwrap();
        let mut pacman = make_pacman(&dir);

        use base64::{Engine, engine::general_purpose};
        let mut config = make_repository_config("dummy");
        config.gpg_private_key_base64 = general_purpose::STANDARD.encode("not a real key");

        assert!(pacman.setup_repository(config).is_err());
    }

    // ── clone_box() ──────────────────────────────────────────────────────────

    #[test]
    fn test_clone_box() {
        let dir = tempfile::tempdir().unwrap();
        let pacman = make_pacman(&dir);
        let boxed: Box<dyn Package> = Box::new(pacman);
        let cloned = boxed.clone();

        assert_eq!(cloned.distro().id, boxed.distro().id);
        assert_eq!(cloned.source_dir(), boxed.source_dir());
        assert_eq!(cloned.build_output_dir(), boxed.build_output_dir());
    }
}
