use anyhow::Result;
use serde::Deserialize;
use std::path::Path;
use std::sync::OnceLock;

#[derive(Debug, Deserialize, Clone, PartialEq)]
#[serde(rename_all = "lowercase")]
pub enum PackageType {
    Rpm,
    Deb,
    Pacman,
}

impl PackageType {
    pub fn extension(&self) -> &'static str {
        match self {
            PackageType::Rpm => "rpm",
            PackageType::Deb => "deb",
            PackageType::Pacman => "pkg.tar.zst",
        }
    }
}

impl std::fmt::Display for PackageType {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            PackageType::Rpm => write!(f, "rpm"),
            PackageType::Deb => write!(f, "deb"),
            PackageType::Pacman => write!(f, "pacman"),
        }
    }
}

#[derive(Debug, Deserialize, Clone)]
pub struct Distro {
    pub id: String,
    pub name: String,
    pub image: String,
    pub package_type: PackageType,
    #[serde(default)]
    pub setup: Vec<String>,
    #[serde(default)]
    pub setup_repo: Vec<String>,
    #[serde(default)]
    pub install_steps: Vec<String>,
    #[serde(default)]
    pub cleanup: Vec<String>,
    pub image_info_url: Option<String>,
    pub deprecated: Option<String>,
}

impl Distro {
    pub fn setup(&self, build_dependencies: &[String]) -> Vec<String> {
        let deps = build_dependencies.join(" ");
        self.setup.iter().map(|command| command.replace("%{build_dependencies}", &deps)).collect()
    }

    pub fn family(&self) -> &'static str {
        let id = self.id.to_lowercase();
        let name = self.name.to_lowercase();
        let matches = |s: &str| id.contains(s) || name.contains(s);

        if matches("opensuse") {
            return "openSUSE";
        }
        if matches("fedora") {
            return "Fedora";
        }
        if matches("debian") {
            return "Debian";
        }
        if matches("ubuntu") {
            return "Ubuntu";
        }
        if matches("alma") {
            return "AlmaLinux";
        }
        if matches("rocky") {
            return "Rocky Linux";
        }
        if matches("mageia") {
            return "Mageia";
        }
        if matches("manjaro") {
            return "Manjaro";
        }
        if matches("arch") {
            return "Arch Linux";
        }

        "Other"
    }
}

#[derive(Debug, Deserialize, Clone)]
pub struct Distros {
    distros: Vec<Distro>,
}

const DISTROS_YAML: &str = include_str!("distros.yml");
static DISTROS: OnceLock<Distros> = OnceLock::new();

impl Distros {
    pub fn get() -> Self {
        DISTROS.get_or_init(Self::load_default).clone()
    }

    pub fn load_from_file(path: &Path) -> Result<Self, anyhow::Error> {
        let content = std::fs::read_to_string(path)?;
        Ok(serde_saphyr::from_str(&content)?)
    }

    fn load_default() -> Self {
        serde_saphyr::from_str(DISTROS_YAML).expect("failed to parse embedded distros.yml")
    }

    pub fn ids(&self) -> Vec<String> {
        self.iter().map(|d| d.id.clone()).collect()
    }

    pub fn iter(&self) -> impl Iterator<Item = &Distro> {
        self.distros.iter()
    }

    pub fn by_id(&self, id: &str) -> Distro {
        self.iter().find(|d| d.id == id).unwrap_or_else(|| panic!("distro '{}' not found", id)).clone()
    }

    pub fn contains(&self, id: &str) -> bool {
        self.iter().any(|d| d.id == id)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_loads_successfully() {
        let distros = Distros::get();
        assert!(!distros.distros.is_empty());
    }

    #[test]
    fn test_distro_fields() {
        let distros = Distros::get();
        let opensuse = distros.iter().find(|d| d.id == "opensuse_15.6").unwrap();

        assert_eq!(opensuse.image, "opensuse/leap:15.6");
        assert_eq!(opensuse.package_type, PackageType::Rpm);
        assert!(!opensuse.setup.is_empty());
        assert!(!opensuse.setup_repo.is_empty());
        assert!(!opensuse.install_steps.is_empty());
        assert!(opensuse.deprecated.is_none());
    }

    #[test]
    fn test_deprecated_field() {
        let distros = Distros::get();
        let deprecated = distros.iter().find(|d| d.id == "debian_10").unwrap();
        assert!(deprecated.deprecated.is_some());

        let active = distros.iter().find(|d| d.id == "debian_12").unwrap();
        assert!(active.deprecated.is_none());
    }

    #[test]
    fn test_merge_keys_resolved() {
        let distros = Distros::get();

        // these distros have no explicit setup, relies on anchor merge
        let fedora = distros.iter().find(|d| d.id == "fedora_38").unwrap();
        assert!(!fedora.setup.is_empty());
        assert_eq!(fedora.package_type, PackageType::Rpm);

        let ubuntu = distros.iter().find(|d| d.id == "ubuntu_22.04").unwrap();
        assert!(!ubuntu.setup.is_empty());
        assert_eq!(ubuntu.package_type, PackageType::Deb);
    }

    #[test]
    fn test_all_distros_have_required_fields() {
        let distros = Distros::get();
        for distro in distros.iter() {
            assert!(!distro.id.is_empty(), "distro {} has empty id", distro.id);
            assert!(!distro.name.is_empty(), "distro {} has empty name", distro.id);
            assert!(!distro.image.is_empty(), "distro {} has empty image", distro.id);
            assert!(!distro.setup.is_empty(), "distro {} has empty setup", distro.id);
        }
    }

    #[test]
    fn test_by_id_found() {
        let distros = Distros::get();
        let distro = distros.by_id("opensuse_15.6");
        assert_eq!(distro.id, "opensuse_15.6");
    }

    #[test]
    fn test_by_id_not_found() {
        let distros = Distros::get();
        let result = std::panic::catch_unwind(|| distros.by_id("nonexistent"));
        assert!(result.is_err());
    }

    fn make_distro(setup: Vec<String>) -> Distro {
        Distro {
            id: "test".to_string(),
            name: "Test".to_string(),
            image: "test:latest".to_string(),
            package_type: PackageType::Rpm,
            setup,
            setup_repo: vec![],
            install_steps: vec![],
            image_info_url: None,
            deprecated: None,
            cleanup: Vec::new(),
        }
    }

    #[test]
    fn test_setup_replaces_build_dependencies() {
        let distro = make_distro(vec!["zypper install -y %{build_dependencies}".to_string(), "echo done".to_string()]);

        let result = distro.setup(&["gcc".to_string(), "make".to_string()]);

        assert_eq!(result[0], "zypper install -y gcc make");
        assert_eq!(result[1], "echo done");
    }

    #[test]
    fn test_setup_empty_dependencies() {
        let distro = make_distro(vec!["zypper install -y %{build_dependencies}".to_string()]);

        let result = distro.setup(&[]);
        assert_eq!(result[0], "zypper install -y ");
    }

    #[test]
    fn test_setup_no_placeholder() {
        let distro = make_distro(vec!["echo hello".to_string()]);

        let result = distro.setup(&["gcc".to_string()]);
        assert_eq!(result[0], "echo hello");
    }

    fn distro_with_id_name(id: &str, name: &str) -> Distro {
        let mut d = make_distro(vec![]);
        d.id = id.to_string();
        d.name = name.to_string();
        d
    }

    #[test]
    fn test_family_matches_by_id() {
        assert_eq!(distro_with_id_name("opensuse_15.6", "x").family(), "openSUSE");
        assert_eq!(distro_with_id_name("fedora_40", "x").family(), "Fedora");
        assert_eq!(distro_with_id_name("debian_12", "x").family(), "Debian");
        assert_eq!(distro_with_id_name("ubuntu_22.04", "x").family(), "Ubuntu");
        assert_eq!(distro_with_id_name("alma_9", "x").family(), "AlmaLinux");
        assert_eq!(distro_with_id_name("rocky_9", "x").family(), "Rocky Linux");
        assert_eq!(distro_with_id_name("mageia_9", "x").family(), "Mageia");
        assert_eq!(distro_with_id_name("arch", "Arch Linux").family(), "Arch Linux");
        assert_eq!(distro_with_id_name("manjaro", "Manjaro").family(), "Manjaro");
    }

    #[test]
    fn test_family_matches_by_name() {
        assert_eq!(distro_with_id_name("custom", "openSUSE Leap").family(), "openSUSE");
        assert_eq!(distro_with_id_name("custom", "Fedora Workstation").family(), "Fedora");
    }

    #[test]
    fn test_family_unknown_returns_other() {
        assert_eq!(distro_with_id_name("gentoo", "Gentoo").family(), "Other");
    }

    #[test]
    fn test_package_type_extension() {
        assert_eq!(PackageType::Rpm.extension(), "rpm");
        assert_eq!(PackageType::Deb.extension(), "deb");
        assert_eq!(PackageType::Pacman.extension(), "pkg.tar.zst");
    }

    #[test]
    fn test_arch_distro_is_pacman() {
        let distros = Distros::get();
        let arch = distros.iter().find(|d| d.id == "arch").unwrap();
        assert_eq!(arch.package_type, PackageType::Pacman);
        assert!(!arch.setup.is_empty());
        assert!(!arch.setup_repo.is_empty());
        assert!(!arch.install_steps.is_empty());

        let manjaro = distros.iter().find(|d| d.id == "manjaro").unwrap();
        assert_eq!(manjaro.package_type, PackageType::Pacman);
    }
}
