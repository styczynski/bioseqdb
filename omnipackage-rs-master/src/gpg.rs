use crate::logger::{LogOutput, Logger};
use crate::shell::Command;
use anyhow::Result;
use std::path::PathBuf;

#[derive(Debug, Clone)]
pub struct Key {
    pub priv_key: String,
    pub pub_key: String,
}

pub struct Gpg {
    exe: String,
    env: Vec<(String, String)>,
}

impl Gpg {
    pub fn new() -> Self {
        Self {
            exe: "gpg".to_string(),
            env: vec![("GNUPGHOME".to_string(), "/dev/null".to_string()), ("GPG_AGENT_INFO".to_string(), "".to_string())],
        }
    }

    pub fn generate_keys(&self, name: &str, email: &str) -> Result<Key, anyhow::Error> {
        self.within_tmp_dir(|gpg, dir| {
            let batchfile_path = dir.join("genkey.batch");
            std::fs::write(&batchfile_path, self.batch_generate_keys(name, email))?;

            gpg.cmd(["--no-tty", "--batch", "--gen-key", batchfile_path.to_str().unwrap()]).run()?;

            let pub_key = gpg.export_key(name, false)?;
            let priv_key = gpg.export_key(name, true)?;
            Ok(Key { priv_key, pub_key })
        })
    }

    pub fn key_id(&self, key_string: &str) -> Result<String, anyhow::Error> {
        self.within_tmp_dir(|gpg, _dir| {
            let key = key_string.to_string();
            let output = gpg
                .cmd(["--quiet", "--no-tty", "--show-keys"])
                .with_stdin(move |stdin| {
                    stdin.write_all(key.as_bytes()).unwrap();
                })
                .capture()?;

            Ok(output.lines().nth(1).unwrap_or("").trim().to_string())
        })
    }

    pub fn key_info(&self, key_string: &str) -> Result<String, anyhow::Error> {
        self.within_tmp_dir(|gpg, _dir| {
            let key = key_string.to_string();
            gpg.cmd(["--quiet", "--no-tty", "--show-keys", "--with-fingerprint"])
                .with_stdin(move |stdin| {
                    stdin.write_all(key.as_bytes()).unwrap();
                })
                .capture()
        })
    }

    pub fn test_private_key(&self, key_string: &str) -> Result<(), anyhow::Error> {
        self.within_tmp_dir(|gpg, _dir| {
            let key = key_string.to_string();
            gpg.cmd(["--quiet", "--no-tty", "--import"])
                .with_stdin(move |stdin| {
                    stdin.write_all(key.as_bytes()).unwrap();
                })
                .run()?;

            let data = "random string to encrypt".to_string();
            gpg.cmd(["-o", "/dev/null", "-as", "-"])
                .with_stdin(move |stdin| {
                    stdin.write_all(data.as_bytes()).unwrap();
                })
                .run()
        })
    }

    pub fn key_from_private(&self, priv_key: &str) -> Result<Key, anyhow::Error> {
        self.within_tmp_dir(|gpg, _dir| {
            let key = priv_key.to_string();
            gpg.cmd(["--quiet", "--no-tty", "--import"])
                .with_stdin(move |stdin| {
                    stdin.write_all(key.as_bytes()).unwrap();
                })
                .run()?;

            let pub_key = gpg.cmd(["--armor", "--export"]).capture()?;

            Ok(Key {
                priv_key: priv_key.to_string(),
                pub_key,
            })
        })
    }

    fn export_key(&self, name: &str, secret: bool) -> Result<String, anyhow::Error> {
        let mut args = vec!["--armor".to_string()];
        if secret {
            args.push("--export-secret-keys".to_string())
        } else {
            args.push("--export".to_string())
        }
        args.push(name.to_string());

        self.cmd(args).capture()
    }

    fn cmd(&self, args: impl IntoIterator<Item = impl Into<String>>) -> Command {
        let mut cmd = Command::new(&self.exe).stream_output_to(Logger::new().with_output(LogOutput::Silent));
        for (k, v) in &self.env {
            cmd = cmd.with_env(k, v);
        }
        cmd.args(args)
    }

    fn within_tmp_dir<F, R>(&self, f: F) -> R
    where
        F: FnOnce(&Self, PathBuf) -> R,
    {
        let dir = tempfile::tempdir().expect("cannot create tmp dir - aborting to prevent ~/.gnupg access");
        std::fs::set_permissions(dir.path(), std::os::unix::fs::PermissionsExt::from_mode(0o700)).expect("cannot set permissions on tmp dir - aborting to prevent ~/.gnupg access");

        let scoped = Self {
            exe: self.exe.clone(),
            env: vec![("GNUPGHOME".to_string(), dir.path().to_string_lossy().to_string()), ("GPG_AGENT_INFO".to_string(), "".to_string())],
        };

        std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| f(&scoped, dir.path().to_path_buf()))).unwrap_or_else(|e| {
            eprintln!("panic inside within_tmp_dir - aborting");
            std::panic::resume_unwind(e)
        })
    }

    fn batch_generate_keys(&self, name: &str, email: &str) -> String {
        format!(
            "Key-Type: RSA\n\
             Key-Length: 4096\n\
             Name-Real: {name}\n\
             Name-Email: {email}\n\
             Expire-Date: 0\n\
             %no-ask-passphrase\n\
             %no-protection\n\
             %commit\n"
        )
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn gpg_available() -> bool {
        std::process::Command::new("gpg")
            .arg("--version")
            .stdout(std::process::Stdio::null())
            .stderr(std::process::Stdio::null())
            .status()
            .map(|s| s.success())
            .unwrap_or(false)
    }

    #[test]
    fn test_generate_keys() {
        if !gpg_available() {
            return;
        }
        let key = Gpg::new().generate_keys("Test User", "test@example.com").unwrap();

        assert!(key.priv_key.contains("-----BEGIN PGP PRIVATE KEY BLOCK-----"));
        assert!(key.priv_key.contains("-----END PGP PRIVATE KEY BLOCK-----"));
        assert!(key.pub_key.contains("-----BEGIN PGP PUBLIC KEY BLOCK-----"));
        assert!(key.pub_key.contains("-----END PGP PUBLIC KEY BLOCK-----"));
    }

    #[test]
    fn test_key_id() {
        if !gpg_available() {
            return;
        }
        let gpg = Gpg::new();
        let key = gpg.generate_keys("Test User", "test@example.com").unwrap();
        let id = gpg.key_id(&key.pub_key).unwrap();
        assert!(!id.is_empty());
    }

    #[test]
    fn test_key_info() {
        if !gpg_available() {
            return;
        }
        let gpg = Gpg::new();
        let key = gpg.generate_keys("Test User", "test@example.com").unwrap();
        let info = gpg.key_info(&key.pub_key).unwrap();
        assert!(info.contains("Test User"));
        assert!(info.contains("test@example.com"));
    }

    #[test]
    fn test_test_private_key_valid() {
        if !gpg_available() {
            return;
        }
        let gpg = Gpg::new();
        let key = gpg.generate_keys("Test User", "test@example.com").unwrap().priv_key;
        assert!(gpg.test_private_key(&key).is_ok());
    }

    #[test]
    fn test_test_private_key_invalid() {
        if !gpg_available() {
            return;
        }
        assert!(Gpg::new().test_private_key("invalid key").is_err());
    }

    #[test]
    fn test_key_from_private() {
        if !gpg_available() {
            return;
        }
        let gpg = Gpg::new();

        // generate a key first to have a valid private key
        let generated = gpg.generate_keys("Test User", "test@example.com").unwrap();

        // derive public key from private
        let derived = gpg.key_from_private(&generated.priv_key).unwrap();

        assert_eq!(derived.priv_key, generated.priv_key);
        assert!(derived.pub_key.contains("-----BEGIN PGP PUBLIC KEY BLOCK-----"));
        assert!(derived.pub_key.contains("-----END PGP PUBLIC KEY BLOCK-----"));
        assert!(!derived.pub_key.is_empty());
    }
}
