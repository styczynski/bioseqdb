use super::{Repositories, sorted_by_distro_order};
use crate::template::{Template, Var};
use std::collections::HashMap;

const SCRIPT_TEMPLATE_SH: &str = include_str!("install.sh.liquid");

pub fn render(distros: &Repositories, package_name: &str, repo_base_url: &str, repo_arch: &str) -> Result<String, anyhow::Error> {
    let sorted = sorted_by_distro_order(distros);

    let mut steps_case = String::new();
    let mut ids: Vec<String> = Vec::new();
    for entry in &sorted {
        let (Some(id), Some(steps)) = (entry.get("distro_id"), entry.get("install_steps")) else {
            continue;
        };
        ids.push(id.clone());
        let quoted = steps.replace('\'', "'\\''");
        steps_case.push_str(&format!("  {id})\n    STEPS='{quoted}'\n    ;;\n"));
    }

    let vars: HashMap<String, Var> = HashMap::from([
        ("package_name".to_string(), package_name.into()),
        ("repo_base_url".to_string(), repo_base_url.into()),
        ("repo_arch".to_string(), repo_arch.into()),
        ("steps_case".to_string(), steps_case.trim_end().to_string().into()),
        ("supported_ids".to_string(), ids.join(" ").into()),
    ]);
    Template::from_content(SCRIPT_TEMPLATE_SH)?.render(vars)
}

#[cfg(test)]
mod tests {
    use super::super::{Repositories, Repository};
    use super::*;

    fn script_fixture_distros() -> Repositories {
        vec![
            Repository::from([
                ("distro_id".to_string(), "ubuntu_24.04".to_string()),
                ("install_steps".to_string(), "echo 'it works' from ubuntu".to_string()),
            ]),
            Repository::from([("distro_id".to_string(), "arch".to_string()), ("install_steps".to_string(), "printf 'arch %s\\n' ok".to_string())]),
        ]
    }

    fn path_with_stub_sudo(dir: &std::path::Path) -> std::ffi::OsString {
        use std::os::unix::fs::PermissionsExt;
        let sudo = dir.join("sudo");
        std::fs::write(&sudo, "#!/bin/sh\nexec \"$@\"\n").unwrap();
        std::fs::set_permissions(&sudo, std::fs::Permissions::from_mode(0o755)).unwrap();
        let mut paths = vec![dir.to_path_buf()];
        paths.extend(std::env::split_paths(&std::env::var_os("PATH").unwrap_or_default()));
        std::env::join_paths(paths).unwrap()
    }

    fn running_as_root() -> bool {
        std::process::Command::new("id").arg("-u").output().map(|o| o.stdout.starts_with(b"0")).unwrap_or(false)
    }

    #[test]
    fn test_render_structure() {
        let script = render(&script_fixture_distros(), "omnipackage", "https://example.test/stable", "x86_64").unwrap();

        assert!(script.contains("PKG='omnipackage'"), "missing package name");
        assert!(script.contains("BASE='https://example.test/stable'"), "missing base url");
        assert!(script.contains("REPO_ARCH='x86_64'"), "missing arch");
        assert!(script.contains("  ubuntu_24.04)"), "missing ubuntu case arm");
        assert!(script.contains("  arch)"), "missing arch case arm");
        assert!(script.contains(r"echo '\''it works'\'' from ubuntu"), "single quotes not escaped: {script}");
        assert!(script.contains("Supported: ubuntu_24.04 arch"), "missing supported list");
    }

    #[test]
    fn test_render_is_valid_posix() {
        let script = render(&script_fixture_distros(), "omnipackage", "https://example.test/stable", "x86_64").unwrap();
        let dir = tempfile::tempdir().unwrap();
        let path = dir.path().join("install.sh");
        std::fs::write(&path, &script).unwrap();

        let syntax = std::process::Command::new("sh").arg("-n").arg(&path).output().unwrap();
        assert!(syntax.status.success(), "sh -n failed: {}", String::from_utf8_lossy(&syntax.stderr));
    }

    #[test]
    fn test_render_runs_selected_steps() {
        let script = render(&script_fixture_distros(), "omnipackage", "https://example.test/stable", "x86_64").unwrap();
        let dir = tempfile::tempdir().unwrap();
        let path = dir.path().join("install.sh");
        std::fs::write(&path, &script).unwrap();

        let out = std::process::Command::new("sh")
            .arg(&path)
            .arg("-y")
            .arg("--distro")
            .arg("ubuntu_24.04")
            .env("PATH", path_with_stub_sudo(dir.path()))
            .output()
            .unwrap();
        assert!(out.status.success(), "script failed: {}", String::from_utf8_lossy(&out.stderr));
        let stdout = String::from_utf8_lossy(&out.stdout);
        assert!(stdout.contains("it works from ubuntu"), "steps not executed: {stdout}");
    }

    #[test]
    fn test_reports_root_required_when_non_root() {
        if running_as_root() {
            return;
        }
        let script = render(&script_fixture_distros(), "omnipackage", "https://example.test/stable", "x86_64").unwrap();
        let dir = tempfile::tempdir().unwrap();
        let path = dir.path().join("install.sh");
        std::fs::write(&path, &script).unwrap();

        // setsid: run without a controlling tty so the script's /dev/tty confirm prompt fails instead of blocking
        let out = std::process::Command::new("setsid")
            .arg("-w")
            .arg("sh")
            .arg(&path)
            .arg("--distro")
            .arg("ubuntu_24.04")
            .stdin(std::process::Stdio::null())
            .output()
            .unwrap();
        assert!(!out.status.success(), "non-root without a tty should not proceed");
        assert!(
            String::from_utf8_lossy(&out.stdout).contains("requires root"),
            "missing root notice: {}",
            String::from_utf8_lossy(&out.stdout)
        );
    }

    #[test]
    fn test_render_handles_pacman_quoting() {
        let steps = "curl -fsSL https://example.test/stable/arch/public.key | pacman-key --add -\n\
             pacman-key --lsign-key $(curl -fsSL https://example.test/stable/arch/public.key | gpg --show-keys --with-colons | awk -F: '/^fpr/{print $10; exit}')\n\
             printf '\\n[omnipackage]\\nSigLevel = Required DatabaseOptional\\nServer = https://example.test/stable/arch\\n' | tee -a /etc/pacman.conf\n\
             pacman -Sy omnipackage";
        let repos: Repositories = vec![Repository::from([("distro_id".to_string(), "arch".to_string()), ("install_steps".to_string(), steps.to_string())])];

        let script = render(&repos, "omnipackage", "https://example.test/stable", "x86_64").unwrap();
        let dir = tempfile::tempdir().unwrap();
        let path = dir.path().join("install.sh");
        std::fs::write(&path, &script).unwrap();

        let syntax = std::process::Command::new("sh").arg("-n").arg(&path).output().unwrap();
        assert!(syntax.status.success(), "sh -n failed on pacman steps: {}", String::from_utf8_lossy(&syntax.stderr));
    }

    #[test]
    fn test_render_unsupported_distro_fails() {
        let script = render(&script_fixture_distros(), "omnipackage", "https://example.test/stable", "x86_64").unwrap();
        let dir = tempfile::tempdir().unwrap();
        let path = dir.path().join("install.sh");
        std::fs::write(&path, &script).unwrap();

        let out = std::process::Command::new("sh").arg(&path).arg("-y").arg("--distro").arg("nope_1.0").output().unwrap();
        assert!(!out.status.success(), "unsupported distro should fail");
        assert!(String::from_utf8_lossy(&out.stderr).contains("not available"), "missing unsupported message");
    }
}
