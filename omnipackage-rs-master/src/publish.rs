use crate::config::{Repository, RepositoryProvider, S3Config};
use crate::logger::{Color, Logger, colorize};
use crate::package::Package;
use anyhow::{Context, Result};
use std::collections::HashSet;
use std::path::PathBuf;

mod artefacts;
mod cloudflare;
mod locking;
mod repo_files;
mod retention;
mod s3;

use cloudflare::CloudflareApi;
use s3::S3;

pub use retention::prepopulate_with_retention;

pub struct Publish {
    pub config: Repository,
    pub package: Box<dyn Package>,
    pub custom_install_page: Option<PathBuf>,
    pub skip_upload: HashSet<PathBuf>,
}

const INSTALL_PAGE_NAME: &str = "install.html";
const INSTALL_JSON_NAME: &str = "install.json";
const INSTALL_SCRIPT_NAME: &str = "install.sh";
const BADGE_NAME: &str = "badge.svg";

#[derive(Debug, Clone)]
struct InstallPageBadge {
    pub page_url: String,
    pub badge_md: String,
}

impl Publish {
    pub fn new(package: Box<dyn Package>, config: Repository, custom_install_page: Option<PathBuf>, skip_upload: HashSet<PathBuf>) -> Self {
        Self {
            package,
            config,
            custom_install_page,
            skip_upload,
        }
    }

    pub fn run(&self) -> Result<(), anyhow::Error> {
        Logger::new().info(format!("starting repository publish for {}", self.package.distro().id));

        if let Err(e) = self.sync_repo_files() {
            Logger::new().error(format!("failed publish for {} ({:#})", self.package.distro().id, e));
            return Err(e);
        }

        if let Err(e) = self.purge_cache() {
            Logger::new().warn(format!("cannot purge cache: {:#}", e));
        }

        let res = self.update_install_page().map_err(|e| {
            Logger::new().error(format!("failed deploy install page for {} ({:#})", self.package.distro().id, e));
            e
        })?;

        if !res.page_url.is_empty() {
            Logger::new().info(format!("install page: {}", colorize(Color::Green, res.page_url)));
        }
        if !res.badge_md.is_empty() {
            Logger::new().info(format!("badge markdown: {}", colorize(Color::Yellow, res.badge_md)));
        }

        Logger::new().info(format!("done repository publish for {}", self.package.distro().id));
        Ok(())
    }

    fn sync_repo_files(&self) -> Result<(), anyhow::Error> {
        let dir = self.package.repository_output_dir();
        if !dir.exists() {
            return Err(anyhow::anyhow!("repository dir does not exist: {}", self.package.repository_output_dir().display()));
        }
        if self.package.artefacts().is_empty() {
            return Err(anyhow::anyhow!("no artefacts in {}", self.package.build_output_dir().display()));
        }

        match self.config.provider {
            RepositoryProvider::S3 => {
                let s3_config = self.config.s3();
                let s3 = S3::new(s3_config, self.s3_in_bucket_distro_path(s3_config));

                if !s3.bucket_exists()? {
                    return Err(anyhow::anyhow!("bucket '{}' does not exist", s3_config.bucket));
                }
                s3.upload_all(&dir, &self.skip_upload)?;
                s3.delete_deleted_files(&dir)?;
                Ok(())
            }
            RepositoryProvider::LocalFs => {
                let localfs_config = self.config.localfs();
                let dst = localfs_config.repository_path().join(&self.package.distro().id);
                artefacts::copy_dir_recursive(&dir, &dst, &self.skip_upload)?;
                // intentional, mirrors S3: retain_packages prepopulates src, so anything in dst but not src is stale
                artefacts::delete_dst_files_not_in_src(&dir, &dst)?;
                Ok(())
            }
        }
    }

    fn s3_in_bucket_distro_path(&self, s3_config: &S3Config) -> String {
        PathBuf::from(s3_config.path_in_bucket.as_deref().unwrap_or(""))
            .join(&self.package.distro().id)
            .to_string_lossy()
            .to_string()
    }

    fn purge_cache(&self) -> Result<(), anyhow::Error> {
        if self.config.provider != RepositoryProvider::S3 {
            return Ok(());
        }
        let s3_config = self.config.s3();

        let binding = s3_config.base_bucket_url();
        let prefix = binding.trim_start_matches("https://").trim_start_matches("http://");

        if let (Some(zone_id), Some(api_token)) = (&s3_config.cloudflare_zone_id, &s3_config.cloudflare_api_token) {
            Logger::new().info(format!("purging Cloudflare CDN cache at {}", prefix));
            CloudflareApi::new(zone_id.clone(), api_token.clone()).purge_by_prefix(prefix)?;
        }
        Ok(())
    }

    fn distro_url(&self) -> String {
        self.package.distro_url(&self.config)
    }

    fn install_steps(&self) -> Vec<String> {
        self.package
            .distro()
            .install_steps
            .iter()
            .map(|command| command.replace("%{package_name}", &self.config.package_name))
            .map(|command| command.replace("%{project_slug}", &self.config.project_slug()))
            .map(|command| command.replace("%{url}", &self.distro_url()))
            .collect()
    }

    fn update_install_page(&self) -> Result<InstallPageBadge, anyhow::Error> {
        let download_url = self.package_download_url()?;

        let entry = repo_files::Repository::from([
            ("distro_id".to_string(), self.package.distro().id.clone()),
            ("distro_name".to_string(), self.package.distro().name.clone()),
            ("distro_family".to_string(), self.package.distro().family().to_string()),
            ("install_steps".to_string(), self.install_steps().join("\n")),
            ("gpg_key".to_string(), self.package.gpgkey().ok_or_else(|| anyhow::anyhow!("no gpg key"))?.pub_key),
            ("download_url".to_string(), download_url),
            ("package_type".to_string(), self.package.distro().package_type.to_string()),
            ("timestamp".to_string(), chrono::Utc::now().to_rfc3339()),
        ]);
        let arch = std::env::consts::ARCH;
        let base_entries: repo_files::Repositories = vec![entry];
        let entries = enrich_for_json(&base_entries, arch, &self.config.package_name);
        let custom_template = self.custom_install_page.as_deref().map(std::fs::read_to_string).transpose()?;

        match self.config.provider {
            RepositoryProvider::S3 => {
                let s3_config = self.config.s3();
                let path = PathBuf::from(s3_config.path_in_bucket.as_deref().unwrap_or(""));
                let s3 = S3::new(s3_config, path.to_string_lossy().to_string());
                let base_url = s3_config.base_bucket_url();

                locking::commit_anchored(&s3, INSTALL_PAGE_NAME, "text/html", |existing_html| {
                    let existing = repo_files::html::parse(&String::from_utf8_lossy(existing_html)).unwrap_or_default();
                    let merged = repo_files::upsert_from(existing, &entries);
                    let html = repo_files::html::render_page(&merged, &self.config, custom_template.clone())?;
                    Ok((html.into_bytes(), vec![]))
                })?;

                locking::commit_anchored(&s3, INSTALL_JSON_NAME, "application/json", |existing_json| {
                    let existing = repo_files::json::parse(&String::from_utf8_lossy(existing_json));
                    let merged = repo_files::upsert_from(existing, &entries);
                    let json = repo_files::json::to_json(&merged)?;
                    let sh = repo_files::sh::render(&merged, &self.config.package_name, &base_url, arch)?;
                    let badge = repo_files::badge::render(&merged, &self.config)?;
                    Ok((
                        json.into_bytes(),
                        vec![(INSTALL_SCRIPT_NAME, "text/x-shellscript", sh.into_bytes()), (BADGE_NAME, "image/svg+xml", badge.into_bytes())],
                    ))
                })?;

                let page_url = install_page_url(&self.config).ok_or_else(|| anyhow::anyhow!("install page url cannot be generated"))?;
                let badge_url = format!("{}/{}", s3_config.base_bucket_url(), BADGE_NAME);
                let badge_md = format!("[![OmniPackage repositories badge]({badge_url})]({page_url})");

                Ok(InstallPageBadge { page_url, badge_md })
            }
            RepositoryProvider::LocalFs => {
                let localfs_config = self.config.localfs();
                let repo_dir = localfs_config.repository_path();
                let base_url = repo_dir.to_string_lossy().into_owned();

                let existing_html = std::fs::read_to_string(repo_dir.join(INSTALL_PAGE_NAME)).unwrap_or_default();
                let html_merged = repo_files::upsert_from(repo_files::html::parse(&existing_html).unwrap_or_default(), &entries);
                std::fs::write(repo_dir.join(INSTALL_PAGE_NAME), repo_files::html::render_page(&html_merged, &self.config, custom_template)?)?;

                let existing_json = std::fs::read_to_string(repo_dir.join(INSTALL_JSON_NAME)).unwrap_or_default();
                let json_merged = repo_files::upsert_from(repo_files::json::parse(&existing_json), &entries);
                std::fs::write(repo_dir.join(INSTALL_JSON_NAME), repo_files::json::to_json(&json_merged)?)?;
                std::fs::write(repo_dir.join(INSTALL_SCRIPT_NAME), repo_files::sh::render(&json_merged, &self.config.package_name, &base_url, arch)?)?;
                std::fs::write(repo_dir.join(BADGE_NAME), repo_files::badge::render(&json_merged, &self.config)?)?;

                Ok(InstallPageBadge {
                    page_url: repo_dir.join(INSTALL_PAGE_NAME).to_string_lossy().to_string(),
                    badge_md: "".to_string(),
                })
            }
        }
    }

    fn package_download_url(&self) -> Result<String, anyhow::Error> {
        let dir = self.package.repository_output_dir();
        let package_file = artefacts::select_fresh_artefact(&self.package.artefacts(), &self.skip_upload, &dir)
            .with_context(|| anyhow::anyhow!("cannot find packages in {}", dir.display()))?
            .ok_or_else(|| anyhow::anyhow!("no packages found in repository dir"))?;

        Ok(format!("{}/{}", self.distro_url().trim_end_matches('/'), package_file.relative_path.display()))
    }
}

fn enrich_for_json(entries: &repo_files::Repositories, arch: &str, package_name: &str) -> repo_files::Repositories {
    entries
        .iter()
        .map(|e| {
            let mut m = e.clone();
            m.insert("arch".to_string(), arch.to_string());
            m.insert("package_name".to_string(), package_name.to_string());
            m
        })
        .collect()
}

pub fn install_page_url(repository: &Repository) -> Option<String> {
    match repository.provider {
        RepositoryProvider::S3 => {
            let s3_config = repository.s3();
            let page_url = format!("{}/{}", s3_config.base_bucket_url(), INSTALL_PAGE_NAME);
            Some(page_url)
        }
        _ => None,
    }
}
