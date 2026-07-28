use crate::config::{VersionExtractor, VersionExtractorProvider};
use crate::shell::Command;
use anyhow::{Context, Result};
use regex::Regex;
use std::path::Path;

pub fn extract_version(path: &Path, config: &VersionExtractor) -> Result<String, anyhow::Error> {
    match config.provider {
        VersionExtractorProvider::File => {
            let file_config = config.file.clone().ok_or_else(|| anyhow::anyhow!("file config is missing"))?;

            let file_path = path.join(&file_config.file);
            let content = std::fs::read_to_string(&file_path).with_context(|| format!("cannot read {}", file_path.display()))?;

            let regex = &file_config.regex;
            let re = Regex::new(regex).with_context(|| format!("invalid regex '{}'", regex))?;

            let result = re
                .captures(&content)
                .and_then(|c| c.get(1))
                .map(|m| m.as_str().to_string())
                .ok_or_else(|| anyhow::anyhow!("regex '{}' did not match in {}", regex, file_path.display()));
            Ok(result?)
        }
        VersionExtractorProvider::Shell => {
            let shell_config = config.shell.clone().ok_or_else(|| anyhow::anyhow!("shell config is missing"))?;
            let output = Command::new("sh").current_dir(path).args(["-c", &shell_config.command]).capture()?.trim_end().to_string();
            Ok(output)
        }
        VersionExtractorProvider::Constant => {
            let constant_config = config.constant.clone().ok_or_else(|| anyhow::anyhow!("constant config is missing"))?;
            Ok(constant_config.version)
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::config::{ExtractVersionConstant, ExtractVersionFile, VersionExtractor};
    use std::path::PathBuf;

    fn make_config(file: &str, regex: &str) -> VersionExtractor {
        VersionExtractor {
            provider: VersionExtractorProvider::File,
            name: "testtest".to_string(),
            file: Some(ExtractVersionFile {
                file: file.to_string(),
                regex: regex.to_string(),
            }),
            shell: None,
            constant: None,
        }
    }

    #[test]
    fn test_extract_version_from_file() {
        let dir = tempfile::tempdir().unwrap();
        std::fs::write(dir.path().join("version.rb"), "VERSION = '1.2.3'").unwrap();

        let config = make_config("version.rb", "VERSION = '(.+)'");
        let version = extract_version(&dir.path().to_path_buf(), &config).unwrap();
        assert_eq!(version, "1.2.3");
    }

    #[test]
    fn test_extract_version_constant() {
        let dir = tempfile::tempdir().unwrap();
        std::fs::write(dir.path().join("version.rb"), "VERSION = '1.2.3'").unwrap();

        let config = make_config("version.rb", "VERSION = '(.+)'");
        let version = extract_version(&dir.path().to_path_buf(), &config).unwrap();
        assert_eq!(version, "1.2.3");
    }

    #[test]
    fn test_extract_version_regex_no_match() {
        let dir = tempfile::tempdir().unwrap();
        std::fs::write(dir.path().join("version.rb"), "no version here").unwrap();

        let config = make_config("version.rb", "VERSION = '(.+)'");
        let err = extract_version(dir.path(), &config).unwrap_err();
        assert!(err.to_string().contains("did not match"));
    }

    #[test]
    fn test_extract_version_invalid_regex() {
        let dir = tempfile::tempdir().unwrap();
        std::fs::write(dir.path().join("version.rb"), "VERSION = '1'").unwrap();

        let config = make_config("version.rb", "[invalid(");
        let err = extract_version(dir.path(), &config).unwrap_err();
        assert!(err.to_string().contains("invalid regex"));
    }

    #[test]
    fn test_extract_version_file_missing() {
        let dir = tempfile::tempdir().unwrap();
        let config = make_config("nonexistent.txt", ".*");
        let err = extract_version(dir.path(), &config).unwrap_err();
        assert!(err.to_string().contains("cannot read"));
    }

    #[test]
    fn test_extract_version_shell_provider() {
        let dir = tempfile::tempdir().unwrap();
        let config = VersionExtractor {
            provider: VersionExtractorProvider::Shell,
            name: "shell".to_string(),
            file: None,
            shell: Some(crate::config::ExtractVersionShell { command: "echo 7.8.9".to_string() }),
            constant: None,
        };
        let version = extract_version(dir.path(), &config).unwrap();
        assert_eq!(version, "7.8.9");
    }

    #[test]
    fn test_extract_version_unknown_provider() {
        let config = VersionExtractor {
            provider: VersionExtractorProvider::Constant,
            name: "onono".to_string(),
            file: None,
            shell: None,
            constant: Some(ExtractVersionConstant {
                version: "some static string".to_string(),
            }),
        };
        let version = extract_version(&PathBuf::from("."), &config).unwrap();
        assert_eq!(version, "some static string");
    }
}
