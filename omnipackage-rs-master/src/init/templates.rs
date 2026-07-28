use crate::init::detect::ProjectType;

#[derive(Debug, Clone, Copy)]
pub struct TemplateFile {
    /// Path under the target `.omnipackage/` directory. May contain
    /// `<PACKAGE_NAME>` which is substituted at write time.
    pub dest: &'static str,
    pub content: &'static str,
    pub executable: bool,
}

const SHARED_DEB_CONTROL: &str = include_str!("templates/_shared/deb/control.liquid");
const SHARED_DEB_CHANGELOG: &str = include_str!("templates/_shared/deb/changelog.liquid");
const SHARED_DEB_COMPAT: &str = include_str!("templates/_shared/deb/compat.liquid");

fn shared_deb() -> [TemplateFile; 3] {
    [
        TemplateFile {
            dest: "deb/control.liquid",
            content: SHARED_DEB_CONTROL,
            executable: false,
        },
        TemplateFile {
            dest: "deb/changelog.liquid",
            content: SHARED_DEB_CHANGELOG,
            executable: false,
        },
        TemplateFile {
            dest: "deb/compat.liquid",
            content: SHARED_DEB_COMPAT,
            executable: false,
        },
    ]
}

pub fn template_set_for(t: ProjectType) -> Vec<TemplateFile> {
    let mut files: Vec<TemplateFile> = shared_deb().to_vec();
    files.extend(per_type(t));
    files
}

fn per_type(t: ProjectType) -> Vec<TemplateFile> {
    match t {
        ProjectType::C => vec![
            TemplateFile {
                dest: "config.yml",
                content: include_str!("templates/c/config.yml"),
                executable: false,
            },
            TemplateFile {
                dest: "PKGBUILD.liquid",
                content: include_str!("templates/c/PKGBUILD.liquid"),
                executable: false,
            },
            TemplateFile {
                dest: "specfile.spec.liquid",
                content: include_str!("templates/c/specfile.spec.liquid"),
                executable: false,
            },
            TemplateFile {
                dest: "deb/rules.liquid",
                content: include_str!("templates/c/deb/rules.liquid"),
                executable: false,
            },
        ],
        ProjectType::Cpp => vec![
            TemplateFile {
                dest: "config.yml",
                content: include_str!("templates/cpp/config.yml"),
                executable: false,
            },
            // Build commands identical to plain C — reuse that file.
            TemplateFile {
                dest: "PKGBUILD.liquid",
                content: include_str!("templates/c/PKGBUILD.liquid"),
                executable: false,
            },
            // Build commands identical to plain C — reuse those files.
            TemplateFile {
                dest: "specfile.spec.liquid",
                content: include_str!("templates/c/specfile.spec.liquid"),
                executable: false,
            },
            TemplateFile {
                dest: "deb/rules.liquid",
                content: include_str!("templates/c/deb/rules.liquid"),
                executable: false,
            },
        ],
        ProjectType::CMake => vec![
            TemplateFile {
                dest: "config.yml",
                content: include_str!("templates/cmake/config.yml"),
                executable: false,
            },
            TemplateFile {
                dest: "PKGBUILD.liquid",
                content: include_str!("templates/cmake/PKGBUILD.liquid"),
                executable: false,
            },
            TemplateFile {
                dest: "specfile.spec.liquid",
                content: include_str!("templates/cmake/specfile.spec.liquid"),
                executable: false,
            },
            TemplateFile {
                dest: "deb/rules.liquid",
                content: include_str!("templates/cmake/deb/rules.liquid"),
                executable: false,
            },
        ],
        ProjectType::Rust => vec![
            TemplateFile {
                dest: "config.yml",
                content: include_str!("templates/rust/config.yml"),
                executable: false,
            },
            TemplateFile {
                dest: "PKGBUILD.liquid",
                content: include_str!("templates/rust/PKGBUILD.liquid"),
                executable: false,
            },
            TemplateFile {
                dest: "specfile.spec.liquid",
                content: include_str!("templates/rust/specfile.spec.liquid"),
                executable: false,
            },
            TemplateFile {
                dest: "deb/rules.liquid",
                content: include_str!("templates/rust/deb/rules.liquid"),
                executable: false,
            },
            TemplateFile {
                dest: "install_rust.sh",
                content: include_str!("templates/rust/install_rust.sh"),
                executable: true,
            },
        ],
        ProjectType::Go => vec![
            TemplateFile {
                dest: "config.yml",
                content: include_str!("templates/go/config.yml"),
                executable: false,
            },
            TemplateFile {
                dest: "PKGBUILD.liquid",
                content: include_str!("templates/go/PKGBUILD.liquid"),
                executable: false,
            },
            TemplateFile {
                dest: "specfile.spec.liquid",
                content: include_str!("templates/go/specfile.spec.liquid"),
                executable: false,
            },
            TemplateFile {
                dest: "deb/rules.liquid",
                content: include_str!("templates/go/deb/rules.liquid"),
                executable: false,
            },
            TemplateFile {
                dest: "install_go.sh",
                content: include_str!("templates/go/install_go.sh"),
                executable: true,
            },
        ],
        ProjectType::Python => vec![
            TemplateFile {
                dest: "config.yml",
                content: include_str!("templates/python/config.yml"),
                executable: false,
            },
            TemplateFile {
                dest: "PKGBUILD.liquid",
                content: include_str!("templates/python/PKGBUILD.liquid"),
                executable: false,
            },
            TemplateFile {
                dest: "specfile.spec.liquid",
                content: include_str!("templates/python/specfile.spec.liquid"),
                executable: false,
            },
            TemplateFile {
                dest: "deb/rules.liquid",
                content: include_str!("templates/python/deb/rules.liquid"),
                executable: false,
            },
            TemplateFile {
                dest: "install.sh",
                content: include_str!("templates/python/install.sh"),
                executable: true,
            },
        ],
        ProjectType::Ruby => vec![
            TemplateFile {
                dest: "config.yml",
                content: include_str!("templates/ruby/config.yml"),
                executable: false,
            },
            // install.sh-based build identical to Python — reuse that file.
            TemplateFile {
                dest: "PKGBUILD.liquid",
                content: include_str!("templates/python/PKGBUILD.liquid"),
                executable: false,
            },
            TemplateFile {
                dest: "specfile.spec.liquid",
                content: include_str!("templates/ruby/specfile.spec.liquid"),
                executable: false,
            },
            TemplateFile {
                dest: "deb/rules.liquid",
                content: include_str!("templates/ruby/deb/rules.liquid"),
                executable: false,
            },
            TemplateFile {
                dest: "install.sh",
                content: include_str!("templates/ruby/install.sh"),
                executable: true,
            },
        ],
        ProjectType::Crystal => vec![
            TemplateFile {
                dest: "config.yml",
                content: include_str!("templates/crystal/config.yml"),
                executable: false,
            },
            TemplateFile {
                dest: "PKGBUILD.liquid",
                content: include_str!("templates/crystal/PKGBUILD.liquid"),
                executable: false,
            },
            TemplateFile {
                dest: "specfile.spec.liquid",
                content: include_str!("templates/crystal/specfile.spec.liquid"),
                executable: false,
            },
            TemplateFile {
                dest: "deb/rules.liquid",
                content: include_str!("templates/crystal/deb/rules.liquid"),
                executable: false,
            },
            TemplateFile {
                dest: "install_crystal.sh",
                content: include_str!("templates/crystal/install_crystal.sh"),
                executable: true,
            },
        ],
        ProjectType::Electron => vec![
            TemplateFile {
                dest: "config.yml",
                content: include_str!("templates/electron/config.yml"),
                executable: false,
            },
            // install.sh-based build identical to Python — reuse that file.
            TemplateFile {
                dest: "PKGBUILD.liquid",
                content: include_str!("templates/python/PKGBUILD.liquid"),
                executable: false,
            },
            TemplateFile {
                dest: "specfile.spec.liquid",
                content: include_str!("templates/electron/specfile.spec.liquid"),
                executable: false,
            },
            TemplateFile {
                dest: "deb/rules.liquid",
                content: include_str!("templates/electron/deb/rules.liquid"),
                executable: false,
            },
            TemplateFile {
                dest: "install.sh",
                content: include_str!("templates/electron/install.sh"),
                executable: true,
            },
            TemplateFile {
                dest: "deb/<PACKAGE_NAME>.postinst",
                content: include_str!("templates/electron/deb/postinst"),
                executable: true,
            },
        ],
        ProjectType::Tauri => vec![
            TemplateFile {
                dest: "config.yml",
                content: include_str!("templates/tauri/config.yml"),
                executable: false,
            },
            TemplateFile {
                dest: "PKGBUILD.liquid",
                content: include_str!("templates/tauri/PKGBUILD.liquid"),
                executable: false,
            },
            TemplateFile {
                dest: "specfile.spec.liquid",
                content: include_str!("templates/tauri/specfile.spec.liquid"),
                executable: false,
            },
            TemplateFile {
                dest: "deb/rules.liquid",
                content: include_str!("templates/tauri/deb/rules.liquid"),
                executable: false,
            },
            TemplateFile {
                dest: "install_rust.sh",
                content: include_str!("templates/tauri/install_rust.sh"),
                executable: true,
            },
        ],
        ProjectType::Generic => vec![
            TemplateFile {
                dest: "config.yml",
                content: include_str!("templates/generic/config.yml"),
                executable: false,
            },
            TemplateFile {
                dest: "PKGBUILD.liquid",
                content: include_str!("templates/generic/PKGBUILD.liquid"),
                executable: false,
            },
            TemplateFile {
                dest: "specfile.spec.liquid",
                content: include_str!("templates/generic/specfile.spec.liquid"),
                executable: false,
            },
            TemplateFile {
                dest: "deb/rules.liquid",
                content: include_str!("templates/generic/deb/rules.liquid"),
                executable: false,
            },
        ],
    }
}
