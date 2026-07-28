use crate::config::{Repository, RepositoryProvider};
use crate::package::Package;
use anyhow::Result;
use std::collections::HashSet;
use std::path::{Path, PathBuf};

pub struct RetentionStats {
    pub kept: usize,
    pub removed: usize,
    pub retained_files: Vec<PathBuf>,
}

pub fn prepopulate_with_retention(config: &Repository, package: &dyn Package) -> Result<Option<RetentionStats>> {
    if config.retain_packages == 0 {
        return Ok(None);
    }

    let dir = package.repository_output_dir();
    std::fs::create_dir_all(&dir)?;
    download_existing(config, package, &dir)?;

    let ext = package.distro().package_type.extension();

    let stats = prune_by_mtime(&dir, ext, config.retain_packages as usize)?;
    Ok(Some(stats))
}

fn download_existing(config: &Repository, package: &dyn Package, dir: &Path) -> Result<()> {
    match config.provider {
        RepositoryProvider::S3 => {
            let s3_config = config.s3();
            let in_bucket_path = PathBuf::from(s3_config.path_in_bucket.as_deref().unwrap_or(""))
                .join(&package.distro().id)
                .to_string_lossy()
                .to_string();
            super::s3::S3::new(s3_config, in_bucket_path).download_all(dir)?;
        }
        RepositoryProvider::LocalFs => {
            let src = config.localfs().repository_path().join(&package.distro().id);
            if src.exists() {
                super::artefacts::copy_dir_recursive(&src, dir, &HashSet::new())?;
            }
        }
    }
    Ok(())
}

fn prune_by_mtime(dir: &Path, ext: &str, keep: usize) -> Result<RetentionStats> {
    let pattern = dir.join(format!("**/*.{}", ext));
    let mut files: Vec<(PathBuf, std::time::SystemTime)> = vec![];

    for entry in glob::glob(pattern.to_str().unwrap())? {
        let path = entry?;
        let mtime = std::fs::metadata(&path)?.modified()?;
        files.push((path, mtime));
    }

    files.sort_by_key(|b| std::cmp::Reverse(b.1));

    let mut removed = 0;
    for (path, _) in files.iter().skip(keep) {
        std::fs::remove_file(path)?;
        removed += 1;
    }

    let retained_files: Vec<PathBuf> = files.into_iter().take(keep).map(|(p, _)| p).collect();
    Ok(RetentionStats {
        kept: retained_files.len(),
        removed,
        retained_files,
    })
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::time::Duration;

    fn touch(path: &Path, mtime_offset_secs: u64) {
        std::fs::write(path, b"x").unwrap();
        let file = std::fs::File::options().write(true).open(path).unwrap();
        let mtime = std::time::SystemTime::now() - Duration::from_secs(mtime_offset_secs);
        file.set_modified(mtime).unwrap();
    }

    #[test]
    fn prune_keeps_n_newest_by_mtime() {
        let dir = tempfile::tempdir().unwrap();
        touch(&dir.path().join("a.rpm"), 300);
        touch(&dir.path().join("b.rpm"), 200);
        touch(&dir.path().join("c.rpm"), 100);
        touch(&dir.path().join("d.rpm"), 0);

        let stats = prune_by_mtime(dir.path(), "rpm", 2).unwrap();
        assert_eq!(stats.kept, 2);
        assert_eq!(stats.removed, 2);
        assert_eq!(stats.retained_files.len(), 2);
        assert!(stats.retained_files.contains(&dir.path().join("c.rpm")));
        assert!(stats.retained_files.contains(&dir.path().join("d.rpm")));

        assert!(!dir.path().join("a.rpm").exists());
        assert!(!dir.path().join("b.rpm").exists());
        assert!(dir.path().join("c.rpm").exists());
        assert!(dir.path().join("d.rpm").exists());
    }

    #[test]
    fn prune_ignores_other_files() {
        let dir = tempfile::tempdir().unwrap();
        touch(&dir.path().join("a.rpm"), 100);
        touch(&dir.path().join("repomd.xml"), 200);
        touch(&dir.path().join("public.key"), 300);

        let stats = prune_by_mtime(dir.path(), "rpm", 0).unwrap();
        assert_eq!(stats.kept, 0);
        assert_eq!(stats.removed, 1);

        assert!(!dir.path().join("a.rpm").exists());
        assert!(dir.path().join("repomd.xml").exists());
        assert!(dir.path().join("public.key").exists());
    }

    #[test]
    fn prune_keep_larger_than_count_removes_nothing() {
        let dir = tempfile::tempdir().unwrap();
        touch(&dir.path().join("a.deb"), 200);
        touch(&dir.path().join("b.deb"), 100);

        let stats = prune_by_mtime(dir.path(), "deb", 5).unwrap();
        assert_eq!(stats.kept, 2);
        assert_eq!(stats.removed, 0);
    }

    #[test]
    fn prune_finds_nested() {
        let dir = tempfile::tempdir().unwrap();
        let sub = dir.path().join("stable");
        std::fs::create_dir_all(&sub).unwrap();
        touch(&sub.join("a.deb"), 200);
        touch(&sub.join("b.deb"), 100);
        touch(&sub.join("c.deb"), 0);

        let stats = prune_by_mtime(dir.path(), "deb", 1).unwrap();
        assert_eq!(stats.kept, 1);
        assert_eq!(stats.removed, 2);
        assert!(sub.join("c.deb").exists());
    }
}
