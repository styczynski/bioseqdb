use crate::config::{Build, ImageCache, Repository};
use crate::distros::Distro;
use crate::gpg::Key;
use crate::job_variables::JobVariables;
use crate::package::{Package, SetupStage};
use crate::template::{Template, Var};
use anyhow::{Context, Result};
use std::collections::HashMap;
use std::path::{Path, PathBuf};

#[derive(Debug, Clone)]
pub struct Deb {
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

impl Deb {
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

    fn render_templates(&self, vars: HashMap<String, Var>, from: PathBuf, to: PathBuf) -> Result<(), anyhow::Error> {
        std::fs::create_dir_all(&to).with_context(|| format!("cannot create directory {}", to.display()))?;

        for entry in std::fs::read_dir(&from).with_context(|| format!("cannot read dir {}", from.display()))? {
            let path = entry?.path();
            let file_name = path
                .file_name()
                .ok_or_else(|| anyhow::anyhow!("cannot get file name for {}", path.display()))?
                .to_string_lossy()
                .into_owned();
            let dest = to.join(&file_name);

            if path.extension().and_then(|e| e.to_str()) == Some("liquid") {
                let stem = path
                    .file_stem()
                    .ok_or_else(|| anyhow::anyhow!("cannot get file stem for {}", path.display()))?
                    .to_string_lossy()
                    .into_owned();
                let dest_without_ext = to.join(stem);
                Template::from_file(&path)?.render_to_file(vars.clone(), dest_without_ext)?;
            } else {
                std::fs::copy(&path, &dest).with_context(|| format!("cannot copy {} to {}", path.display(), dest.display()))?;
            }
        }

        Ok(())
    }

    fn write_releases_script(&self, home_dir: &Path) -> Result<(), anyhow::Error> {
        // credit: https://earthly.dev/blog/creating-and-hosting-your-own-deb-packages-and-apt-repo/
        let script = r#"#!/bin/sh
set -e

do_hash() {
    HASH_NAME=$1
    HASH_CMD=$2
    echo "${HASH_NAME}:"
    for f in $(find -type f); do
        f=$(echo $f | cut -c3-) # remove ./ prefix
        if [ "$f" = "Release" ]; then
            continue
        fi
        echo " $(${HASH_CMD} ${f}  | cut -d" " -f1) $(wc -c $f)"
    done
}

cat << EOF
Origin: Omnipackage repository
Label: Example
Suite: stable
Codename: stable
Version: 1.0
Architectures: $(dpkg --print-architecture)
Components: main
Description: Omnipackage repository
Date: $(date -Ru)
EOF
do_hash "MD5Sum" "md5sum"
do_hash "SHA1" "sha1sum"
do_hash "SHA256" "sha256sum"
"#;

        Ok(std::fs::write(home_dir.join("generate_releases_script.sh"), script)?)
    }

    fn output_path(&self) -> PathBuf {
        self.distro_build_dir().join("output")
    }
}

impl Package for Deb {
    fn setup_build(&mut self, config: Build) -> Result<(), anyhow::Error> {
        self.prepare_build_dir()?;
        let debian_folder_template_path = config.deb.clone().ok_or_else(|| anyhow::anyhow!("deb config is missing"))?.debian_templates;

        let build_path = self.distro_build_dir().join("build");
        let output_path = self.output_path();
        std::fs::create_dir_all(&build_path).with_context(|| format!("cannot create directory {}", build_path.display()))?;
        std::fs::create_dir_all(&output_path).with_context(|| format!("cannot create directory {}", output_path.display()))?;

        let mut template_vars: HashMap<String, Var> = self.job_variables.to_template_vars();
        template_vars.extend(config.to_template_vars());
        self.render_templates(template_vars, self.source_dir.join(&debian_folder_template_path), build_path.join("debian"))?;

        self.mounts.insert(self.source_dir.to_string_lossy().to_string(), "/source".to_string());
        self.mounts.insert(build_path.to_string_lossy().to_string(), "/output/build".to_string());
        self.mounts.insert(self.output_path().to_string_lossy().to_string(), "/output/".to_string());

        if self.image_cache.is_none() {
            self.commands.extend(self.distro.setup(&config.build_dependencies));
            if let Some(bbs) = self.before_build_script("/source", &config) {
                self.commands.push(bbs);
            }
        }
        let rsync_excludes: String = self.ignore_source_files.iter().map(|p| format!(" --exclude='{p}'")).collect();
        self.commands.extend([
            format!("rsync -a{rsync_excludes} /source/ /output/build/"),
            "cd /output/build".to_string(),
            "DEB_BUILD_OPTIONS=noddebs dpkg-buildpackage -b -tc".to_string(),
        ]);

        self.build_output_dir = output_path;
        self.setup_stages.push(SetupStage::Build);

        Ok(())
    }

    fn setup_repository(&mut self, config: Repository) -> Result<(), anyhow::Error> {
        let gpgkey = self.prepare_gpgkey(&config)?;
        let (home_dir, repo_dir) = self.prepare_repository(&gpgkey)?;

        self.write_releases_script(&home_dir)?;

        self.mounts.insert(home_dir.to_string_lossy().to_string(), "/omnipackage".to_string());
        self.mounts.insert(repo_dir.to_string_lossy().to_string(), "/repo".to_string());
        self.mounts.insert(self.output_path().to_string_lossy().to_string(), "/output/".to_string());

        if self.image_cache.is_none() {
            self.commands.extend(self.distro.setup_repo.clone());
        }
        self.commands.extend(self.import_gpg_keys_commands());
        self.commands.extend([
            "cd /repo".to_string(),
            "cp /output/*.deb /repo/".to_string(),
            "chmod +x /omnipackage/generate_releases_script.sh".to_string(),
            "mkdir -p stable".to_string(),
            "mv *.deb stable/".to_string(),
            "dpkg-scanpackages stable/ > stable/Packages".to_string(),
            "cat stable/Packages | gzip -1 > stable/Packages.gz".to_string(),
            "cd stable/".to_string(),
            "/omnipackage/generate_releases_script.sh > Release".to_string(),
            "gpg --no-tty --batch --yes --armor --detach-sign -o Release.gpg Release".to_string(),
            "gpg --no-tty --batch --yes --armor --detach-sign --clearsign -o InRelease Release".to_string(),
            "mv ../public.key Release.key".to_string(),
        ]);

        self.build_output_dir = repo_dir.clone();
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
    use crate::config::{Build, DebConfig, Repository, RepositoryProvider, S3Config};
    use crate::distros::Distros;
    use crate::gpg::Gpg;
    use crate::job_variables::JobVariables;
    use crate::package::Package;
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
        Distros::get().by_id("debian_12")
    }

    fn make_job_variables() -> JobVariables {
        JobVariables::new("1.2.3".to_string())
    }

    fn make_deb(dir: &tempfile::TempDir) -> Deb {
        make_deb_with_ignores(dir, vec![])
    }

    fn make_deb_with_ignores(dir: &tempfile::TempDir, ignore_source_files: Vec<String>) -> Deb {
        let source_dir = dir.path().join("source");
        let build_dir = dir.path().join("build");
        std::fs::create_dir_all(&source_dir).unwrap();
        std::fs::create_dir_all(&build_dir).unwrap();
        Deb::new(make_distro(), source_dir, make_job_variables(), build_dir, None, ignore_source_files)
    }

    fn make_debian_templates(dir: &tempfile::TempDir) -> String {
        let templates_dir = dir.path().join("source").join("debian-templates");
        std::fs::create_dir_all(&templates_dir).unwrap();

        std::fs::write(
            templates_dir.join("control.liquid"),
            "Package: {{ package_name }}\nVersion: {{ version }}\nMaintainer: {{ maintainer }}\n",
        )
        .unwrap();
        std::fs::write(templates_dir.join("changelog"), "myapp (1.0) stable; urgency=low\n  * Initial release\n").unwrap();

        "debian-templates".to_string()
    }

    fn make_build_config(dir: &tempfile::TempDir) -> Build {
        let debian_templates = make_debian_templates(dir);
        Build {
            distro: "debian_12".to_string(),
            package_name: "myapp".to_string(),
            maintainer: "Test <test@test.com>".to_string(),
            homepage: "https://example.com".to_string(),
            description: "Test package".to_string(),
            build_dependencies: vec!["gcc".to_string(), "make".to_string()],
            runtime_dependencies: vec![],
            before_build_script: None,
            rpm: None,
            deb: Some(DebConfig { debian_templates }),
            pacman: None,
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
        let deb = make_deb(&dir);

        assert_eq!(deb.distro().id, "debian_12");
        assert_eq!(deb.job_variables.version, "1.2.3");
        assert!(deb.mounts().is_empty());
        assert!(deb.commands().is_empty());
        assert!(deb.setup_stages().is_empty());
        assert!(deb.gpgkey().is_none());
        assert_eq!(deb.build_output_dir(), deb.distro_build_dir());
    }

    // ── setup_build() ────────────────────────────────────────────────────────

    #[test]
    fn test_setup_build_creates_output_dir() {
        let dir = tempfile::tempdir().unwrap();
        let mut deb = make_deb(&dir);

        deb.setup_build(make_build_config(&dir)).unwrap();

        assert!(deb.build_output_dir().exists());
    }

    #[test]
    fn test_setup_build_renders_liquid_templates() {
        let dir = tempfile::tempdir().unwrap();
        let mut deb = make_deb(&dir);

        deb.setup_build(make_build_config(&dir)).unwrap();

        let control = deb.distro_build_dir().join("build").join("debian").join("control");
        assert!(control.exists());
        let content = std::fs::read_to_string(control).unwrap();
        assert!(content.contains("Package: myapp"));
        assert!(content.contains("Version: 1.2.3"));
        assert!(content.contains("Maintainer: Test <test@test.com>"));
    }

    #[test]
    fn test_setup_build_copies_non_liquid_files() {
        let dir = tempfile::tempdir().unwrap();
        let mut deb = make_deb(&dir);

        deb.setup_build(make_build_config(&dir)).unwrap();

        let changelog = deb.distro_build_dir().join("build").join("debian").join("changelog");
        assert!(changelog.exists());
        let content = std::fs::read_to_string(changelog).unwrap();
        assert!(content.contains("Initial release"));
    }

    #[test]
    fn test_setup_build_liquid_extension_stripped_from_output() {
        let dir = tempfile::tempdir().unwrap();
        let mut deb = make_deb(&dir);

        deb.setup_build(make_build_config(&dir)).unwrap();

        let with_ext = deb.distro_build_dir().join("build").join("debian").join("control.liquid");
        let without_ext = deb.distro_build_dir().join("build").join("debian").join("control");
        assert!(!with_ext.exists());
        assert!(without_ext.exists());
    }

    #[test]
    fn test_setup_build_adds_mounts() {
        let dir = tempfile::tempdir().unwrap();
        let mut deb = make_deb(&dir);

        deb.setup_build(make_build_config(&dir)).unwrap();

        let mounts = deb.mounts();
        assert!(mounts.values().any(|v| v == "/source"));
        assert!(mounts.values().any(|v| v == "/output/build"));
        assert!(mounts.values().any(|v| v == "/output/"));
    }

    #[test]
    fn test_setup_build_adds_commands() {
        let dir = tempfile::tempdir().unwrap();
        let mut deb = make_deb(&dir);

        deb.setup_build(make_build_config(&dir)).unwrap();

        let commands = deb.commands();
        assert!(!commands.is_empty());
        assert!(commands.iter().any(|c| c.contains("dpkg-buildpackage")));
        assert!(commands.iter().any(|c| c.contains("rsync") && c.contains("/source/ /output/build/")));
    }

    #[test]
    fn test_setup_build_adds_distro_setup_commands() {
        let dir = tempfile::tempdir().unwrap();
        let mut deb = make_deb(&dir);
        let config = make_build_config(&dir);
        let distro_setup = make_distro().setup(&config.build_dependencies.clone());

        deb.setup_build(config).unwrap();

        let commands = deb.commands();
        for expected in &distro_setup {
            assert!(commands.contains(expected), "missing distro setup command: {}", expected);
        }
    }

    #[test]
    fn test_setup_build_adds_build_to_stages() {
        let dir = tempfile::tempdir().unwrap();
        let mut deb = make_deb(&dir);

        deb.setup_build(make_build_config(&dir)).unwrap();

        assert!(deb.setup_stages().contains(&SetupStage::Build));
    }

    #[test]
    fn test_setup_build_sets_build_output_dir_to_output() {
        let dir = tempfile::tempdir().unwrap();
        let mut deb = make_deb(&dir);

        deb.setup_build(make_build_config(&dir)).unwrap();

        assert!(deb.build_output_dir().ends_with("output"));
    }

    #[test]
    fn test_setup_build_fails_without_deb_config() {
        let dir = tempfile::tempdir().unwrap();
        let mut deb = make_deb(&dir);
        let mut config = make_build_config(&dir);
        config.deb = None;

        assert!(deb.setup_build(config).is_err());
    }

    #[test]
    fn test_setup_build_passes_ignore_source_files_to_rsync() {
        let dir = tempfile::tempdir().unwrap();
        let ignores = vec![".git".to_string(), "node_modules".to_string(), "*.log".to_string()];
        let mut deb = make_deb_with_ignores(&dir, ignores);

        deb.setup_build(make_build_config(&dir)).unwrap();

        let rsync_cmd = deb.commands().into_iter().find(|c| c.starts_with("rsync ")).expect("rsync command not found");
        assert!(rsync_cmd.contains("--exclude='.git'"));
        assert!(rsync_cmd.contains("--exclude='node_modules'"));
        assert!(rsync_cmd.contains("--exclude='*.log'"));
        assert!(rsync_cmd.contains("/source/ /output/build/"));
    }

    #[test]
    fn test_setup_build_no_excludes_when_ignore_source_files_empty() {
        let dir = tempfile::tempdir().unwrap();
        let mut deb = make_deb(&dir);

        deb.setup_build(make_build_config(&dir)).unwrap();

        let rsync_cmd = deb.commands().into_iter().find(|c| c.starts_with("rsync ")).expect("rsync command not found");
        assert!(!rsync_cmd.contains("--exclude"));
    }

    #[test]
    fn test_setup_build_with_before_build_script() {
        let dir = tempfile::tempdir().unwrap();
        let mut deb = make_deb(&dir);
        let mut config = make_build_config(&dir);
        config.before_build_script = Some("build.sh".to_string());

        deb.setup_build(config).unwrap();

        assert!(deb.commands().iter().any(|c| c.contains("build.sh")));
    }

    // ── setup_repository() ───────────────────────────────────────────────────

    #[test]
    fn test_setup_repository_adds_repository_to_stages() {
        if !gpg_available() {
            return;
        }
        let dir = tempfile::tempdir().unwrap();
        let mut deb = make_deb(&dir);
        let gpg_key = Gpg::new().generate_keys("Test", "test@example.com").unwrap();

        deb.setup_repository(make_repository_config(&gpg_key.priv_key)).unwrap();

        assert!(deb.setup_stages().contains(&SetupStage::Repository));
    }

    #[test]
    fn test_setup_repository_stores_gpg_key() {
        if !gpg_available() {
            return;
        }
        let dir = tempfile::tempdir().unwrap();
        let mut deb = make_deb(&dir);
        let gpg_key = Gpg::new().generate_keys("Test", "test@example.com").unwrap();

        deb.setup_repository(make_repository_config(&gpg_key.priv_key)).unwrap();

        assert!(deb.gpgkey().is_some());
    }

    #[test]
    fn test_setup_repository_adds_mounts() {
        if !gpg_available() {
            return;
        }
        let dir = tempfile::tempdir().unwrap();
        let mut deb = make_deb(&dir);
        let gpg_key = Gpg::new().generate_keys("Test", "test@example.com").unwrap();

        deb.setup_repository(make_repository_config(&gpg_key.priv_key)).unwrap();

        let mounts = deb.mounts();
        assert!(mounts.values().any(|v| v == "/omnipackage"));
        assert!(mounts.values().any(|v| v == "/repo"));
        assert!(mounts.values().any(|v| v == "/output/"));
    }

    #[test]
    fn test_setup_repository_adds_commands() {
        if !gpg_available() {
            return;
        }
        let dir = tempfile::tempdir().unwrap();
        let mut deb = make_deb(&dir);
        let gpg_key = Gpg::new().generate_keys("Test", "test@example.com").unwrap();

        deb.setup_repository(make_repository_config(&gpg_key.priv_key)).unwrap();

        let commands = deb.commands();
        assert!(commands.iter().any(|c| c.contains("dpkg-scanpackages")));
        assert!(commands.iter().any(|c| c.contains("gpg")));
        assert!(commands.iter().any(|c| c.contains("Release")));
    }

    #[test]
    fn test_setup_repository_writes_releases_script() {
        if !gpg_available() {
            return;
        }
        let dir = tempfile::tempdir().unwrap();
        let mut deb = make_deb(&dir);
        let gpg_key = Gpg::new().generate_keys("Test", "test@example.com").unwrap();

        deb.setup_repository(make_repository_config(&gpg_key.priv_key)).unwrap();

        let home_dir = deb.distro_build_dir().join("home");
        let script = home_dir.join("generate_releases_script.sh");
        assert!(script.exists());
        let content = std::fs::read_to_string(script).unwrap();
        assert!(content.contains("SHA256"));
        assert!(content.contains("do_hash"));
    }

    #[test]
    fn test_setup_repository_writes_gpg_keys_to_disk() {
        if !gpg_available() {
            return;
        }
        let dir = tempfile::tempdir().unwrap();
        let mut deb = make_deb(&dir);
        let gpg_key = Gpg::new().generate_keys("Test", "test@example.com").unwrap();

        deb.setup_repository(make_repository_config(&gpg_key.priv_key)).unwrap();

        let home_dir = deb.distro_build_dir().join("home");
        let repo_dir = deb.build_output_dir();
        assert!(home_dir.join("key.priv").exists());
        assert!(repo_dir.join("public.key").exists());
    }

    #[test]
    fn test_setup_repository_fails_with_invalid_gpg_key() {
        let dir = tempfile::tempdir().unwrap();
        let mut deb = make_deb(&dir);

        use base64::{Engine, engine::general_purpose};
        let mut config = make_repository_config("dummy");
        config.gpg_private_key_base64 = general_purpose::STANDARD.encode("not a real key");

        assert!(deb.setup_repository(config).is_err());
    }

    // ── clone_box() ──────────────────────────────────────────────────────────

    #[test]
    fn test_clone_box() {
        let dir = tempfile::tempdir().unwrap();
        let deb = make_deb(&dir);
        let boxed: Box<dyn Package> = Box::new(deb);
        let cloned = boxed.clone();

        assert_eq!(cloned.distro().id, boxed.distro().id);
        assert_eq!(cloned.source_dir(), boxed.source_dir());
        assert_eq!(cloned.build_output_dir(), boxed.build_output_dir());
    }

    // ── write_releases_script() ──────────────────────────────────────────────

    #[test]
    fn test_write_releases_script_content() {
        let dir = tempfile::tempdir().unwrap();
        let deb = make_deb(&dir);

        deb.write_releases_script(dir.path()).unwrap();

        let content = std::fs::read_to_string(dir.path().join("generate_releases_script.sh")).unwrap();
        assert!(content.contains("#!/bin/sh"));
        assert!(content.contains("SHA256"));
        assert!(content.contains("MD5Sum"));
        assert!(content.contains("SHA1"));
        assert!(content.contains("do_hash"));
        assert!(content.contains("Architectures: $(dpkg --print-architecture)"));
    }

    // ── render_templates() ───────────────────────────────────────────────────

    #[test]
    fn test_render_templates_renders_liquid_files() {
        let dir = tempfile::tempdir().unwrap();
        let deb = make_deb(&dir);

        let from = dir.path().join("templates");
        let to = dir.path().join("output");
        std::fs::create_dir_all(&from).unwrap();
        std::fs::write(from.join("control.liquid"), "Package: {{ package_name }}").unwrap();

        let mut vars = HashMap::new();
        vars.insert("package_name".to_string(), crate::template::Var::from("myapp"));
        deb.render_templates(vars, from, to.clone()).unwrap();

        let content = std::fs::read_to_string(to.join("control")).unwrap();
        assert_eq!(content, "Package: myapp");
    }

    #[test]
    fn test_render_templates_copies_plain_files() {
        let dir = tempfile::tempdir().unwrap();
        let deb = make_deb(&dir);

        let from = dir.path().join("templates");
        let to = dir.path().join("output");
        std::fs::create_dir_all(&from).unwrap();
        std::fs::write(from.join("changelog"), "my changelog").unwrap();

        deb.render_templates(HashMap::new(), from, to.clone()).unwrap();

        let content = std::fs::read_to_string(to.join("changelog")).unwrap();
        assert_eq!(content, "my changelog");
    }

    #[test]
    fn test_render_templates_creates_output_dir() {
        let dir = tempfile::tempdir().unwrap();
        let deb = make_deb(&dir);

        let from = dir.path().join("templates");
        let to = dir.path().join("nested").join("output");
        std::fs::create_dir_all(&from).unwrap();

        deb.render_templates(HashMap::new(), from, to.clone()).unwrap();

        assert!(to.exists());
    }
}
