use anyhow::Result;
use std::collections::HashSet;
use std::path::{Path, PathBuf};

#[derive(Debug)]
pub struct ArtefactMatch {
    pub filename: String,
    pub relative_path: PathBuf,
}

pub fn find_artefacts_in_repository_dir(artefacts: &[PathBuf], repository_dir: &Path) -> Result<Vec<ArtefactMatch>, anyhow::Error> {
    let mut results = Vec::new();

    for artifact in artefacts {
        let filename = artifact
            .file_name()
            .ok_or_else(|| anyhow::anyhow!("artifact has no filename: {}", artifact.display()))?
            .to_string_lossy();

        let pattern = repository_dir.join("**").join(filename.as_ref()).to_string_lossy().into_owned();

        for entry in glob::glob(&pattern)? {
            let full_path = entry?;

            let relative_path = full_path.strip_prefix(repository_dir).map(PathBuf::from).unwrap_or_else(|_| full_path.clone());

            results.push(ArtefactMatch {
                filename: filename.to_string(),
                relative_path,
            });
        }
    }

    Ok(results)
}

pub fn select_fresh_artefact(artefacts: &[PathBuf], skip: &HashSet<PathBuf>, repository_dir: &Path) -> Result<Option<ArtefactMatch>, anyhow::Error> {
    let fresh: Vec<PathBuf> = artefacts.iter().filter(|p| !skip.contains(*p)).cloned().collect();
    Ok(find_artefacts_in_repository_dir(&fresh, repository_dir)?.into_iter().next())
}

pub fn copy_dir_recursive(src: &Path, dst: &Path, skip: &HashSet<PathBuf>) -> Result<()> {
    std::fs::create_dir_all(dst)?;

    for entry in std::fs::read_dir(src)? {
        let entry = entry?;
        let src_path = entry.path();
        let dst_path = dst.join(entry.file_name());

        if src_path.is_dir() {
            copy_dir_recursive(&src_path, &dst_path, skip)?;
        } else if !skip.contains(&src_path) {
            std::fs::copy(&src_path, &dst_path)?;
        }
    }

    Ok(())
}

pub fn delete_dst_files_not_in_src(src: &Path, dst: &Path) -> Result<()> {
    if !dst.exists() {
        return Ok(());
    }
    for entry in std::fs::read_dir(dst)? {
        let entry = entry?;
        let dst_path = entry.path();
        let src_path = src.join(entry.file_name());

        if dst_path.is_dir() {
            delete_dst_files_not_in_src(&src_path, &dst_path)?;
        } else if !src_path.exists() {
            std::fs::remove_file(&dst_path)?;
        }
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::fs;
    use tempfile::tempdir;

    #[test]
    fn finds_nested_artefacts() {
        let dir = tempdir().unwrap();
        let repository_dir = dir.path();

        let deep = repository_dir.join("sub").join("deep");
        fs::create_dir_all(&deep).unwrap();
        fs::write(deep.join("foo.txt"), b"").unwrap();

        fs::write(repository_dir.join("bar.txt"), b"").unwrap();

        let artefacts = vec![PathBuf::from("/some/original/path/foo.txt"), PathBuf::from("/another/path/bar.txt")];

        let matches = find_artefacts_in_repository_dir(&artefacts, repository_dir).unwrap();

        assert_eq!(matches.len(), 2);

        let foo = matches.iter().find(|m| m.filename == "foo.txt").unwrap();
        assert_eq!(foo.relative_path, PathBuf::from("sub/deep/foo.txt"));

        let bar = matches.iter().find(|m| m.filename == "bar.txt").unwrap();
        assert_eq!(bar.relative_path, PathBuf::from("bar.txt"));
    }

    #[test]
    fn no_match_returns_empty() {
        let dir = tempdir().unwrap();
        let artefacts = vec![PathBuf::from("/path/to/missing.txt")];
        let matches = find_artefacts_in_repository_dir(&artefacts, dir.path()).unwrap();
        assert!(matches.is_empty());
    }

    #[test]
    fn select_fresh_skips_retained_old_package() {
        let dir = tempdir().unwrap();
        let old = dir.path().join("omnipackage-0.1.15-1.x86_64.rpm");
        let new = dir.path().join("omnipackage-0.1.16-1.x86_64.rpm");
        fs::write(&old, b"").unwrap();
        fs::write(&new, b"").unwrap();

        let artefacts = vec![old.clone(), new.clone()];
        let skip = HashSet::from([old]);

        let selected = select_fresh_artefact(&artefacts, &skip, dir.path()).unwrap().unwrap();
        assert_eq!(selected.filename, "omnipackage-0.1.16-1.x86_64.rpm");
    }

    #[test]
    fn select_fresh_returns_only_artefact_without_retention() {
        let dir = tempdir().unwrap();
        let only = dir.path().join("omnipackage-0.1.16-1.x86_64.rpm");
        fs::write(&only, b"").unwrap();

        let selected = select_fresh_artefact(&[only], &HashSet::new(), dir.path()).unwrap().unwrap();
        assert_eq!(selected.filename, "omnipackage-0.1.16-1.x86_64.rpm");
    }

    #[test]
    fn select_fresh_returns_none_when_all_retained() {
        let dir = tempdir().unwrap();
        let old = dir.path().join("omnipackage-0.1.15-1.x86_64.rpm");
        fs::write(&old, b"").unwrap();

        let skip = HashSet::from([old.clone()]);
        assert!(select_fresh_artefact(&[old], &skip, dir.path()).unwrap().is_none());
    }
}
