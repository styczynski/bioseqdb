use crate::logger::Logger;
use crate::template::Var;
use anyhow::{Context, Result};
use base64::{Engine, engine::general_purpose};
use serde::Deserialize;
use std::collections::HashMap;
use std::path::{Path, PathBuf};

#[derive(Debug, Deserialize, Clone)]
pub struct ExtractVersionFile {
    pub file: String,
    pub regex: String,
}

#[derive(Debug, Deserialize, Clone)]
pub struct ExtractVersionShell {
    pub command: String,
}

#[derive(Debug, Deserialize, Clone)]
pub struct ExtractVersionConstant {
    pub version: String,
}

#[derive(Debug, Deserialize, Clone, PartialEq)]
#[serde(rename_all = "lowercase")]
pub enum VersionExtractorProvider {
    File,
    Shell,
    Constant,
}

#[derive(Debug, Deserialize, Clone)]
pub struct VersionExtractor {
    pub name: String,
    pub provider: VersionExtractorProvider,
    pub file: Option<ExtractVersionFile>,
    pub shell: Option<ExtractVersionShell>,
    pub constant: Option<ExtractVersionConstant>,
}

#[derive(Debug, Deserialize, Clone)]
pub struct RpmConfig {
    pub spec_template: String,
}

#[derive(Debug, Deserialize, Clone)]
pub struct DebConfig {
    pub debian_templates: String,
}

#[derive(Debug, Deserialize, Clone)]
pub struct PacmanConfig {
    pub pkgbuild_template: String,
}

#[derive(Debug, Deserialize, Clone)]
#[serde(untagged)]
pub enum AnyValue {
    String(String),
    Bool(bool),
    Int(i64),
    Float(f64),
}

#[derive(Debug, Deserialize, Clone)]
pub struct Build {
    pub distro: String,
    pub package_name: String,
    pub maintainer: String,
    pub homepage: String,
    pub description: String,
    #[serde(default)]
    pub build_dependencies: Vec<String>,
    #[serde(default)]
    pub runtime_dependencies: Vec<String>,
    pub before_build_script: Option<String>,
    pub rpm: Option<RpmConfig>,
    pub deb: Option<DebConfig>,
    pub pacman: Option<PacmanConfig>,
    #[serde(flatten, default)]
    pub rest: HashMap<String, AnyValue>,
}

#[derive(Debug, Deserialize, Clone)]
pub struct LocalFsConfig {
    pub path: String,
}

#[derive(Debug, Deserialize, Clone)]
pub struct S3Config {
    pub bucket: String,
    pub path_in_bucket: Option<String>,
    pub bucket_public_url: Option<String>,
    pub endpoint: String,
    pub access_key_id: String,
    pub secret_access_key: String,
    pub region: Option<String>,
    #[serde(default)]
    pub force_path_style: bool,
    pub cloudflare_zone_id: Option<String>,
    pub cloudflare_api_token: Option<String>,
}

#[derive(Debug, Deserialize, Clone, PartialEq)]
#[serde(rename_all = "lowercase")]
pub enum RepositoryProvider {
    S3,
    LocalFs,
}

#[derive(Debug, Deserialize, Clone)]
pub struct Repository {
    pub name: String,
    pub provider: RepositoryProvider,
    pub localfs: Option<LocalFsConfig>,
    pub s3: Option<S3Config>,
    pub gpg_private_key_base64: String,
    pub package_name: String,
    #[serde(default)]
    pub retain_packages: u32,
    #[serde(flatten, default)]
    pub rest: HashMap<String, AnyValue>,
}

impl Repository {
    pub fn localfs(&self) -> &LocalFsConfig {
        self.localfs.as_ref().unwrap_or_else(|| panic!("repository '{}' has no localfs config", self.name))
    }

    pub fn s3(&self) -> &S3Config {
        self.s3.as_ref().unwrap_or_else(|| panic!("repository '{}' has no s3 config", self.name))
    }

    pub fn gpg_private_key(&self) -> Result<String, anyhow::Error> {
        let decoded = general_purpose::STANDARD.decode(self.gpg_private_key_base64.clone()).context("cannot decode GPG key")?;
        Ok(String::from_utf8(decoded)?)
    }

    pub fn project_slug(&self) -> String {
        self.package_name.clone() // TODO: make sure it's safe to use in S3 path
    }

    pub fn to_template_vars(&self) -> HashMap<String, Var> {
        let mut vars = HashMap::new();
        vars.insert("package_name".to_string(), self.package_name.clone().into());

        for (k, v) in &self.rest {
            let var = match v {
                AnyValue::String(s) => s.clone().into(),
                AnyValue::Bool(b) => (*b).into(),
                AnyValue::Int(i) => (*i).into(),
                AnyValue::Float(f) => f.to_string().into(),
            };
            vars.insert(k.clone(), var);
        }

        vars
    }
}

impl S3Config {
    pub fn base_url(&self) -> String {
        let url = self.bucket_public_url.as_deref().unwrap_or(&self.endpoint);
        // TODO: handle different providers' shenanigans and/or force_path_style
        url.trim_end_matches('/').to_string()
    }

    pub fn base_bucket_url(&self) -> String {
        let path_in_b = PathBuf::from(self.path_in_bucket.as_deref().unwrap_or(""));
        format!("{}/{}", self.base_url().trim_end_matches('/'), path_in_b.display())
    }
}

impl LocalFsConfig {
    pub fn repository_path(&self) -> PathBuf {
        PathBuf::from(&self.path)
    }
}

#[derive(Debug, Deserialize, Clone, Default)]
pub struct Repositories(Vec<Repository>);

impl Repositories {
    pub fn find_by_name_or_default(&self, name: Option<&str>) -> Result<&Repository, anyhow::Error> {
        match name {
            Some(name) => self.0.iter().find(|r| r.name == name).ok_or_else(|| anyhow::anyhow!("repository '{}' not found", name)),
            None => self.0.first().ok_or_else(|| anyhow::anyhow!("no repositories configured")),
        }
    }
}

impl std::ops::Deref for Repositories {
    type Target = Vec<Repository>;

    fn deref(&self) -> &Self::Target {
        &self.0
    }
}

#[derive(Debug, Deserialize, Clone, Default)]
pub struct VersionExtractors(Vec<VersionExtractor>);

impl VersionExtractors {
    pub fn find_by_name_or_default(&self, name: Option<&str>) -> Result<&VersionExtractor, anyhow::Error> {
        match name {
            Some(name) => self.0.iter().find(|r| r.name == name).ok_or_else(|| anyhow::anyhow!("version extractor '{}' not found", name)),
            None => self.0.first().ok_or_else(|| anyhow::anyhow!("no version extractors configured")),
        }
    }
}

impl std::ops::Deref for VersionExtractors {
    type Target = Vec<VersionExtractor>;

    fn deref(&self) -> &Self::Target {
        &self.0
    }
}

#[derive(Debug, Deserialize, Clone)]
pub struct ImageCacheRegistry {
    pub url: String,
    pub namespace: String,
    pub username: String,
    pub password: String,
}

#[derive(Debug, Deserialize, Clone, PartialEq)]
#[serde(rename_all = "lowercase")]
pub enum ImageCacheProvider {
    Registry,
    Local,
}

#[derive(Debug, Deserialize, Clone)]
pub struct ImageCache {
    pub name: String,
    pub provider: ImageCacheProvider,
    pub image_tag: String,
    pub registry: Option<ImageCacheRegistry>,
}

#[derive(Debug, Deserialize, Clone, Default)]
pub struct ImageCaches(Vec<ImageCache>);

impl ImageCaches {
    pub fn find_by_name_or_default(&self, name: Option<&str>) -> Result<&ImageCache, anyhow::Error> {
        match name {
            Some(name) => self.0.iter().find(|r| r.name == name).ok_or_else(|| anyhow::anyhow!("image cache '{}' not found", name)),
            None => self.0.first().ok_or_else(|| anyhow::anyhow!("no image cache configured")),
        }
    }
}

impl ImageCache {
    pub fn full_image_name(&self, distro_id: &str) -> String {
        match self.provider {
            ImageCacheProvider::Registry => {
                let reg = self.registry.as_ref().unwrap();
                format!("{}/{}/{}:{}", reg.url.trim_end_matches('/'), reg.namespace, distro_id, self.image_tag)
            }
            ImageCacheProvider::Local => format!("{}:{}", distro_id, self.image_tag),
        }
    }
}

#[derive(Debug, Deserialize, Clone)]
pub struct Config {
    pub version_extractors: VersionExtractors,
    pub builds: Vec<Build>,
    #[serde(default)]
    pub repositories: Repositories,
    #[serde(default)]
    pub secrets: HashMap<String, String>,
    pub image_caches: Option<ImageCaches>,
    // Basename-style globs only (e.g. ".git", "node_modules", "*.log").
    // Patterns with slashes or "**" are not portable across rsync's exclude syntax.
    #[serde(default)]
    pub ignore_source_files: Vec<String>,
}

impl Config {
    pub fn load(path: &Path, silent: bool) -> Result<Self, anyhow::Error> {
        Self::load_with_env(path, Path::new(".env"), silent)
    }

    pub fn load_with_env(path: &Path, env_path: &Path, silent: bool) -> Result<Self, anyhow::Error> {
        let env_map: HashMap<String, String> = match dotenvy::from_path_iter(env_path) {
            Ok(iter) => {
                let map: HashMap<String, String> = iter.filter_map(Result::ok).collect();
                if !silent {
                    Logger::new().info(format!(
                        "env loaded from {}: {}",
                        std::env::current_dir().unwrap_or_default().join(env_path).display(),
                        map.keys().map(String::as_str).collect::<Vec<_>>().join(", ")
                    ));
                }
                map
            }
            Err(_) => {
                if !silent {
                    Logger::new().warn(format!("no env in {}", std::env::current_dir().unwrap_or_default().join(env_path).display()));
                }
                HashMap::new()
            }
        };

        let content = std::fs::read_to_string(path).with_context(|| format!("cannot read {}", path.display()))?;

        let content = Self::expand_env_vars_with(&content, |var| std::env::var(var).ok().or_else(|| env_map.get(var).cloned()).unwrap_or_default());

        serde_saphyr::from_str(&content).with_context(|| format!("cannot parse {}", path.display()))
    }

    fn expand_env_vars_with<F>(content: &str, resolver: F) -> String
    where
        F: Fn(&str) -> String,
    {
        let re = regex::Regex::new(r"\$\{([^}]+)\}").unwrap();
        re.replace_all(content, |caps: &regex::Captures| resolver(&caps[1])).to_string()
    }
}

impl Build {
    pub fn to_template_vars(&self) -> HashMap<String, Var> {
        let mut vars = HashMap::new();
        vars.insert("package_name".to_string(), self.package_name.clone().into());
        vars.insert("maintainer".to_string(), self.maintainer.clone().into());
        vars.insert("homepage".to_string(), self.homepage.clone().into());
        vars.insert("description".to_string(), self.description.clone().into());
        vars.insert("build_dependencies".to_string(), self.build_dependencies.clone().into());
        vars.insert("runtime_dependencies".to_string(), self.runtime_dependencies.clone().into());

        for (k, v) in &self.rest {
            let var = match v {
                AnyValue::String(s) => s.clone().into(),
                AnyValue::Bool(b) => (*b).into(),
                AnyValue::Int(i) => (*i).into(),
                AnyValue::Float(f) => f.to_string().into(),
            };
            vars.insert(k.clone(), var);
        }

        vars
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_load_config() {
        let path = Path::new("tests/fixtures/sample_project/.omnipackage/config.yml");
        let config = Config::load_with_env(path, &Path::new("tests/fixtures/sample_project/.omnipackage/.env"), false).unwrap();

        assert!(!config.version_extractors.is_empty());
        let ve = &config.version_extractors[1];
        assert_eq!(ve.provider, VersionExtractorProvider::File);
        let file = ve.file.as_ref().unwrap();
        assert_eq!(file.file, "version.h");

        assert!(!config.builds.is_empty());

        let first = &config.builds[0];
        assert_eq!(first.distro, "opensuse_15.3");
        assert_eq!(first.package_name, "sample-project");
        assert!(!first.build_dependencies.is_empty());
        assert!(first.rpm.is_some());
        assert!(first.deb.is_none());

        let deb_build = config.builds.iter().find(|b| b.distro == "debian_12").unwrap();
        assert!(deb_build.deb.is_some());
        assert!(deb_build.rpm.is_none());

        let pacman_build = config.builds.iter().find(|b| b.distro == "arch").unwrap();
        assert!(pacman_build.pacman.is_some());
        assert!(pacman_build.rpm.is_none());
        assert!(pacman_build.deb.is_none());
        assert_eq!(pacman_build.pacman.as_ref().unwrap().pkgbuild_template, ".omnipackage/PKGBUILD.liquid");

        // verify merge key resolution — fields from anchors are present
        let simple_rpm = config.builds.iter().find(|b| b.distro == "fedora_38").unwrap();
        assert_eq!(simple_rpm.package_name, "sample-project");
        assert_eq!(simple_rpm.homepage, "https://omnipackage.org/");

        assert_eq!(config.repositories.len(), 4);

        let s3 = &config.repositories[0];
        assert_eq!(s3.name, "test repo on Cloudflare R2");
        assert_eq!(s3.provider, RepositoryProvider::S3);
        assert_eq!(s3.s3().bucket, "repositories-test");
        assert_eq!(<std::option::Option<std::string::String> as Clone>::clone(&s3.s3().region).unwrap(), "auto");
        assert!(s3.localfs.is_none());
        assert!(s3.gpg_private_key_base64.len() > 1);
        assert_eq!(s3.s3().access_key_id, "testkeyid");
        assert_eq!(s3.s3().secret_access_key, "testsecretkey");

        let localfs = &config.repositories[1];
        assert_eq!(localfs.name, "Local test");
        assert_eq!(localfs.provider, RepositoryProvider::LocalFs);
        assert_eq!(localfs.localfs().path, "/tmp/omnipackage-repos");
        assert!(localfs.s3.is_none());
    }

    #[test]
    fn test_extra_fields_in_template_vars() {
        let yaml = r#"
    distro: test
    package_name: myapp
    maintainer: Test <test@test.com>
    homepage: https://example.com
    description: Test
    custom_string: hello
    custom_bool: true
    "#;

        let build: Build = serde_saphyr::from_str(yaml).unwrap();
        let vars = build.to_template_vars();

        let dir = tempfile::tempdir().unwrap();
        let path = dir.path().join("template.liquid");
        std::fs::write(&path, "{{ description }} {{ custom_string }} {{ custom_bool }}").unwrap();

        let template = crate::template::Template::from_file(path).unwrap();
        let output = template.render(vars).unwrap();
        assert_eq!(output, "Test hello true");
    }

    #[test]
    fn test_expand_env_vars_basic() {
        let result = Config::expand_env_vars_with("value: ${FOO} and ${BAR}", |var| match var {
            "FOO" => "hello".to_string(),
            "BAR" => "world".to_string(),
            _ => String::new(),
        });
        assert_eq!(result, "value: hello and world");
    }

    #[test]
    fn test_expand_env_vars_missing() {
        let result = Config::expand_env_vars_with("value: ${MISSING}", |_| String::new());
        assert_eq!(result, "value: ");
    }

    #[test]
    fn test_expand_env_vars_no_placeholders() {
        let result = Config::expand_env_vars_with("plain: value", |_| String::new());
        assert_eq!(result, "plain: value");
    }

    #[test]
    fn test_expand_env_vars_multiple_same() {
        let result = Config::expand_env_vars_with("${FOO} and ${FOO}", |_| "bar".to_string());
        assert_eq!(result, "bar and bar");
    }

    #[test]
    fn test_load_expands_env_vars() {
        let dir = tempfile::tempdir().unwrap();
        let config_path = dir.path().join("config.yml");
        let env_path = dir.path().join(".env");

        std::fs::write(&env_path, "MY_VAR=expanded_value").unwrap();
        std::fs::write(
            &config_path,
            "
        version_extractors:
          - name: default
            provider: file
            file:
              file: ${MY_VAR}
              regex: VERSION
        builds: []
        ",
        )
        .unwrap();

        let config = Config::load_with_env(&config_path, &env_path, false).unwrap();
        assert_eq!(config.version_extractors[0].file.as_ref().unwrap().file, "expanded_value");
    }

    #[test]
    fn test_load_no_repositories() {
        let dir = tempfile::tempdir().unwrap();
        let config_path = dir.path().join("config.yml");

        std::fs::write(
            &config_path,
            r#"
        version_extractors:
          - name: default
            provider: file
            file:
              file: version.rb
              regex: VERSION
        builds: []
        "#,
        )
        .unwrap();

        let config = Config::load(&config_path, false).unwrap();
        assert!(config.repositories.is_empty());
    }

    #[test]
    fn test_version_extractors_find_by_name() {
        let yaml = r#"
version_extractors:
  - name: from-file
    provider: file
    file:
      file: version.h
      regex: 'VERSION\s+"([^"]+)"'
  - name: from-shell
    provider: shell
    shell:
      command: ./get-version.sh
builds: []
"#;
        let dir = tempfile::tempdir().unwrap();
        let path = dir.path().join("config.yml");
        std::fs::write(&path, yaml).unwrap();

        let config = Config::load(&path, true).unwrap();
        assert_eq!(config.version_extractors.len(), 2);

        let found = config.version_extractors.find_by_name_or_default(Some("from-shell")).unwrap();
        assert_eq!(found.provider, VersionExtractorProvider::Shell);
        assert!(found.shell.is_some());
        assert_eq!(found.shell.as_ref().unwrap().command, "./get-version.sh");
    }

    #[test]
    fn test_version_extractors_find_default_is_first() {
        let yaml = r#"
version_extractors:
  - name: primary
    provider: file
    file:
      file: version.txt
      regex: '.*'
  - name: secondary
    provider: shell
    shell:
      command: echo 1.0.0
builds: []
"#;
        let dir = tempfile::tempdir().unwrap();
        let path = dir.path().join("config.yml");
        std::fs::write(&path, yaml).unwrap();

        let config = Config::load(&path, true).unwrap();
        let default = config.version_extractors.find_by_name_or_default(None).unwrap();
        assert_eq!(default.name, "primary");
    }

    #[test]
    fn test_version_extractors_find_unknown_name_errors() {
        let yaml = r#"
version_extractors:
  - name: only-one
    provider: file
    file:
      file: version.h
      regex: '.*'
builds: []
"#;
        let dir = tempfile::tempdir().unwrap();
        let path = dir.path().join("config.yml");
        std::fs::write(&path, yaml).unwrap();

        let config = Config::load(&path, true).unwrap();
        let result = config.version_extractors.find_by_name_or_default(Some("does-not-exist"));
        assert!(result.is_err());
    }

    #[test]
    fn test_s3_base_url_uses_public_url_when_set() {
        let s3 = S3Config {
            bucket: "b".to_string(),
            path_in_bucket: None,
            bucket_public_url: Some("https://cdn.example.com/".to_string()),
            endpoint: "https://s3.example.com".to_string(),
            access_key_id: "k".to_string(),
            secret_access_key: "s".to_string(),
            region: None,
            force_path_style: false,
            cloudflare_zone_id: None,
            cloudflare_api_token: None,
        };
        assert_eq!(s3.base_url(), "https://cdn.example.com");
    }

    #[test]
    fn test_s3_base_url_falls_back_to_endpoint() {
        let s3 = S3Config {
            bucket: "b".to_string(),
            path_in_bucket: None,
            bucket_public_url: None,
            endpoint: "https://s3.example.com/".to_string(),
            access_key_id: "k".to_string(),
            secret_access_key: "s".to_string(),
            region: None,
            force_path_style: false,
            cloudflare_zone_id: None,
            cloudflare_api_token: None,
        };
        assert_eq!(s3.base_url(), "https://s3.example.com");
    }

    #[test]
    fn test_s3_base_bucket_url_with_path() {
        let s3 = S3Config {
            bucket: "b".to_string(),
            path_in_bucket: Some("packages/sub".to_string()),
            bucket_public_url: Some("https://cdn.example.com".to_string()),
            endpoint: "https://s3.example.com".to_string(),
            access_key_id: "k".to_string(),
            secret_access_key: "s".to_string(),
            region: None,
            force_path_style: false,
            cloudflare_zone_id: None,
            cloudflare_api_token: None,
        };
        assert_eq!(s3.base_bucket_url(), "https://cdn.example.com/packages/sub");
    }

    #[test]
    fn test_s3_base_bucket_url_without_path() {
        let s3 = S3Config {
            bucket: "b".to_string(),
            path_in_bucket: None,
            bucket_public_url: Some("https://cdn.example.com".to_string()),
            endpoint: "https://s3.example.com".to_string(),
            access_key_id: "k".to_string(),
            secret_access_key: "s".to_string(),
            region: None,
            force_path_style: false,
            cloudflare_zone_id: None,
            cloudflare_api_token: None,
        };
        assert_eq!(s3.base_bucket_url(), "https://cdn.example.com/");
    }

    #[test]
    fn test_image_cache_full_image_name_registry() {
        let ic = ImageCache {
            name: "ic".to_string(),
            provider: ImageCacheProvider::Registry,
            image_tag: "v1".to_string(),
            registry: Some(ImageCacheRegistry {
                url: "registry.example.com/".to_string(),
                namespace: "myteam".to_string(),
                username: "u".to_string(),
                password: "p".to_string(),
            }),
        };
        assert_eq!(ic.full_image_name("debian_12"), "registry.example.com/myteam/debian_12:v1");
    }

    #[test]
    fn test_image_cache_full_image_name_local() {
        let ic = ImageCache {
            name: "ic".to_string(),
            provider: ImageCacheProvider::Local,
            image_tag: "v1".to_string(),
            registry: None,
        };
        assert_eq!(ic.full_image_name("debian_12"), "debian_12:v1");
    }

    #[test]
    fn test_repositories_find_by_name_or_default() {
        let yaml = r#"
version_extractors: []
builds: []
repositories:
  - name: primary
    provider: localfs
    localfs:
      path: /tmp/a
    gpg_private_key_base64: ""
    package_name: pkg
  - name: secondary
    provider: localfs
    localfs:
      path: /tmp/b
    gpg_private_key_base64: ""
    package_name: pkg
"#;
        let dir = tempfile::tempdir().unwrap();
        let path = dir.path().join("config.yml");
        std::fs::write(&path, yaml).unwrap();
        let config = Config::load(&path, true).unwrap();

        assert_eq!(config.repositories.find_by_name_or_default(None).unwrap().name, "primary");
        assert_eq!(config.repositories.find_by_name_or_default(Some("secondary")).unwrap().name, "secondary");
        assert!(config.repositories.find_by_name_or_default(Some("missing")).is_err());
    }

    #[test]
    fn test_repositories_find_by_name_empty_errors_on_default() {
        let repos = Repositories::default();
        assert!(repos.find_by_name_or_default(None).is_err());
    }

    #[test]
    fn test_image_caches_find_by_name_or_default() {
        let caches = vec![
            ImageCache {
                name: "first".to_string(),
                provider: ImageCacheProvider::Local,
                image_tag: "v1".to_string(),
                registry: None,
            },
            ImageCache {
                name: "second".to_string(),
                provider: ImageCacheProvider::Local,
                image_tag: "v2".to_string(),
                registry: None,
            },
        ];
        let ics = ImageCaches(caches);

        assert_eq!(ics.find_by_name_or_default(None).unwrap().name, "first");
        assert_eq!(ics.find_by_name_or_default(Some("second")).unwrap().name, "second");
        assert!(ics.find_by_name_or_default(Some("missing")).is_err());
    }

    #[test]
    fn test_version_extractors_empty_errors_on_default() {
        let yaml = r#"
version_extractors: []
builds: []
"#;
        let dir = tempfile::tempdir().unwrap();
        let path = dir.path().join("config.yml");
        std::fs::write(&path, yaml).unwrap();

        let config = Config::load(&path, true).unwrap();
        let result = config.version_extractors.find_by_name_or_default(None);
        assert!(result.is_err());
    }
}
