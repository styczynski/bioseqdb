use std::fs;
use std::path::{Path, PathBuf};

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ProjectType {
    Tauri,
    Rust,
    Go,
    Python,
    Ruby,
    Crystal,
    Electron,
    CMake,
    Cpp,
    C,
    Generic,
}

impl ProjectType {
    pub fn as_str(self) -> &'static str {
        match self {
            ProjectType::Tauri => "tauri",
            ProjectType::Rust => "rust",
            ProjectType::Go => "go",
            ProjectType::Python => "python",
            ProjectType::Ruby => "ruby",
            ProjectType::Crystal => "crystal",
            ProjectType::Electron => "electron",
            ProjectType::CMake => "cmake",
            ProjectType::Cpp => "cpp",
            ProjectType::C => "c",
            ProjectType::Generic => "generic",
        }
    }

    pub fn from_str(s: &str) -> Option<Self> {
        Some(match s {
            "tauri" => ProjectType::Tauri,
            "rust" => ProjectType::Rust,
            "go" => ProjectType::Go,
            "python" => ProjectType::Python,
            "ruby" => ProjectType::Ruby,
            "crystal" => ProjectType::Crystal,
            "electron" => ProjectType::Electron,
            "cmake" => ProjectType::CMake,
            "cpp" => ProjectType::Cpp,
            "c" => ProjectType::C,
            "generic" => ProjectType::Generic,
            _ => return None,
        })
    }
}

pub fn detect_project_type(root: &Path) -> ProjectType {
    if root.join("src-tauri/Cargo.toml").exists() {
        return ProjectType::Tauri;
    }
    if root.join("Cargo.toml").exists() {
        return ProjectType::Rust;
    }
    if root.join("shard.yml").exists() {
        return ProjectType::Crystal;
    }
    if root.join("go.mod").exists() {
        return ProjectType::Go;
    }
    if root.join("Gemfile").exists() || has_file_with_ext(root, "gemspec") {
        return ProjectType::Ruby;
    }
    // Tauri is checked above, so a bare package.json is Electron (or generic JS, treated as Electron).
    if root.join("package.json").exists() {
        return ProjectType::Electron;
    }
    if root.join("pyproject.toml").exists() || root.join("requirements.txt").exists() || has_file_with_ext(root, "py") {
        return ProjectType::Python;
    }
    // CMake before c/cpp because CMake projects sometimes ship a Makefile alongside CMakeLists.txt.
    if root.join("CMakeLists.txt").exists() {
        return ProjectType::CMake;
    }
    if root.join("Makefile").exists() {
        if has_cpp_source(root) {
            return ProjectType::Cpp;
        }
        if has_file_with_ext(root, "c") {
            return ProjectType::C;
        }
    }
    ProjectType::Generic
}

fn has_file_with_ext(root: &Path, ext: &str) -> bool {
    read_dir_filter(root, |name| {
        std::path::Path::new(name).extension().and_then(|e| e.to_str()).map(|e| e.eq_ignore_ascii_case(ext)).unwrap_or(false)
    })
}

fn has_cpp_source(root: &Path) -> bool {
    read_dir_filter(root, |name| {
        let ext = std::path::Path::new(name).extension().and_then(|e| e.to_str()).map(|e| e.to_ascii_lowercase());
        matches!(ext.as_deref(), Some("cpp") | Some("cxx") | Some("cc"))
    })
}

fn read_dir_filter<F: Fn(&str) -> bool>(root: &Path, pred: F) -> bool {
    let Ok(entries) = fs::read_dir(root) else {
        return false;
    };
    for entry in entries.flatten() {
        if let Some(name) = entry.file_name().to_str()
            && pred(name)
        {
            return true;
        }
    }
    false
}

#[derive(Debug, Default, Clone)]
pub struct Defaults {
    pub package_name: Option<String>,
    pub homepage: Option<String>,
    pub description: Option<String>,
}

pub fn extract_defaults(root: &Path, project_type: ProjectType) -> Defaults {
    match project_type {
        ProjectType::Rust => from_cargo_toml(&root.join("Cargo.toml")),
        ProjectType::Tauri => from_cargo_toml(&root.join("src-tauri/Cargo.toml")),
        ProjectType::Go => from_go_mod(&root.join("go.mod")),
        ProjectType::Crystal => from_shard_yml(&root.join("shard.yml")),
        ProjectType::Ruby => from_gemspec(root),
        ProjectType::Electron => from_package_json(&root.join("package.json")),
        ProjectType::Python => from_pyproject(&root.join("pyproject.toml")),
        ProjectType::CMake => from_cmake(&root.join("CMakeLists.txt")),
        ProjectType::Cpp | ProjectType::C | ProjectType::Generic => Defaults::default(),
    }
}

fn read_to_string_or_default(path: &Path) -> String {
    fs::read_to_string(path).unwrap_or_default()
}

fn capture(re: &str, hay: &str) -> Option<String> {
    regex::Regex::new(re).ok()?.captures(hay)?.get(1).map(|m| m.as_str().trim().trim_matches('"').to_string())
}

fn from_cargo_toml(path: &Path) -> Defaults {
    let content = read_to_string_or_default(path);
    let in_package_section = content.split("\n[").find(|s| s.starts_with("package]") || s == &content.trim_start_matches('[')).unwrap_or(&content);
    Defaults {
        package_name: capture(r#"(?m)^\s*name\s*=\s*"([^"]+)""#, in_package_section),
        homepage: capture(r#"(?m)^\s*homepage\s*=\s*"([^"]+)""#, in_package_section),
        description: capture(r#"(?m)^\s*description\s*=\s*"([^"]+)""#, in_package_section),
    }
}

fn from_go_mod(path: &Path) -> Defaults {
    let content = read_to_string_or_default(path);
    let module = capture(r"(?m)^module\s+(\S+)", &content);
    let package_name = module.and_then(|m| m.rsplit('/').next().map(|s| s.to_string()));
    Defaults { package_name, ..Defaults::default() }
}

fn from_shard_yml(path: &Path) -> Defaults {
    let content = read_to_string_or_default(path);
    Defaults {
        package_name: capture(r"(?m)^name:\s*(\S+)", &content),
        description: capture(r"(?m)^description:\s*(.+)$", &content),
        homepage: capture(r"(?m)^homepage:\s*(\S+)", &content),
    }
}

fn from_gemspec(root: &Path) -> Defaults {
    let Ok(entries) = fs::read_dir(root) else {
        return Defaults::default();
    };
    let mut name = None;
    let mut homepage = None;
    let mut description = None;
    for entry in entries.flatten() {
        let path = entry.path();
        if path.extension().and_then(|e| e.to_str()) != Some("gemspec") {
            continue;
        }
        let content = read_to_string_or_default(&path);
        name = name.or_else(|| capture(r#"\.name\s*=\s*["']([^"']+)["']"#, &content));
        homepage = homepage.or_else(|| capture(r#"\.homepage\s*=\s*["']([^"']+)["']"#, &content));
        description = description.or_else(|| capture(r#"\.summary\s*=\s*["']([^"']+)["']"#, &content));
    }
    Defaults {
        package_name: name,
        homepage,
        description,
    }
}

fn from_package_json(path: &Path) -> Defaults {
    let content = read_to_string_or_default(path);
    Defaults {
        package_name: capture(r#""name"\s*:\s*"([^"]+)""#, &content),
        homepage: capture(r#""homepage"\s*:\s*"([^"]+)""#, &content),
        description: capture(r#""description"\s*:\s*"([^"]+)""#, &content),
    }
}

fn from_pyproject(path: &Path) -> Defaults {
    let content = read_to_string_or_default(path);
    Defaults {
        package_name: capture(r#"(?m)^\s*name\s*=\s*"([^"]+)""#, &content),
        homepage: capture(r#"(?m)^\s*homepage\s*=\s*"([^"]+)""#, &content),
        description: capture(r#"(?m)^\s*description\s*=\s*"([^"]+)""#, &content),
    }
}

fn from_cmake(path: &Path) -> Defaults {
    let content = read_to_string_or_default(path);
    Defaults {
        package_name: capture(r"(?m)^\s*project\s*\(\s*([A-Za-z0-9_\-]+)", &content),
        ..Defaults::default()
    }
}

pub fn slugify(s: &str) -> String {
    let lower: String = s.trim().to_ascii_lowercase().chars().map(|c| if c.is_ascii_alphanumeric() { c } else { '-' }).collect();
    let mut out = String::with_capacity(lower.len());
    let mut last_dash = true;
    for c in lower.chars() {
        if c == '-' {
            if !last_dash {
                out.push('-');
                last_dash = true;
            }
        } else {
            out.push(c);
            last_dash = false;
        }
    }
    out.trim_matches('-').to_string()
}

pub fn git_config(key: &str) -> Option<String> {
    use subprocess::{Exec, Redirection};
    let out = Exec::cmd("git").args(["config", "--get", key]).stdout(Redirection::Pipe).stderr(Redirection::None).capture().ok()?;
    if !out.success() {
        return None;
    }
    let s = out.stdout_str().trim().to_string();
    if s.is_empty() { None } else { Some(s) }
}

pub fn dir_basename(path: &Path) -> String {
    let abs = if path.is_absolute() {
        path.to_path_buf()
    } else {
        std::env::current_dir().unwrap_or_else(|_| PathBuf::from(".")).join(path)
    };
    abs.file_name().and_then(|n| n.to_str()).map(|s| s.to_string()).unwrap_or_else(|| "package".to_string())
}

#[cfg(test)]
mod tests {
    use super::*;
    use tempfile::TempDir;

    fn touch(dir: &Path, name: &str) {
        std::fs::write(dir.join(name), "").unwrap();
    }

    fn touch_in(dir: &Path, sub: &str, name: &str) {
        std::fs::create_dir_all(dir.join(sub)).unwrap();
        std::fs::write(dir.join(sub).join(name), "").unwrap();
    }

    #[test]
    fn detects_rust() {
        let d = TempDir::new().unwrap();
        touch(d.path(), "Cargo.toml");
        assert_eq!(detect_project_type(d.path()), ProjectType::Rust);
    }

    #[test]
    fn detects_tauri_before_rust() {
        let d = TempDir::new().unwrap();
        touch(d.path(), "Cargo.toml");
        touch_in(d.path(), "src-tauri", "Cargo.toml");
        assert_eq!(detect_project_type(d.path()), ProjectType::Tauri);
    }

    #[test]
    fn detects_go() {
        let d = TempDir::new().unwrap();
        touch(d.path(), "go.mod");
        assert_eq!(detect_project_type(d.path()), ProjectType::Go);
    }

    #[test]
    fn detects_crystal() {
        let d = TempDir::new().unwrap();
        touch(d.path(), "shard.yml");
        assert_eq!(detect_project_type(d.path()), ProjectType::Crystal);
    }

    #[test]
    fn detects_ruby_via_gemfile() {
        let d = TempDir::new().unwrap();
        touch(d.path(), "Gemfile");
        assert_eq!(detect_project_type(d.path()), ProjectType::Ruby);
    }

    #[test]
    fn detects_ruby_via_gemspec() {
        let d = TempDir::new().unwrap();
        touch(d.path(), "myproj.gemspec");
        assert_eq!(detect_project_type(d.path()), ProjectType::Ruby);
    }

    #[test]
    fn detects_electron() {
        let d = TempDir::new().unwrap();
        touch(d.path(), "package.json");
        assert_eq!(detect_project_type(d.path()), ProjectType::Electron);
    }

    #[test]
    fn detects_python_via_main_py() {
        let d = TempDir::new().unwrap();
        touch(d.path(), "main.py");
        assert_eq!(detect_project_type(d.path()), ProjectType::Python);
    }

    #[test]
    fn detects_cmake() {
        let d = TempDir::new().unwrap();
        touch(d.path(), "CMakeLists.txt");
        assert_eq!(detect_project_type(d.path()), ProjectType::CMake);
    }

    #[test]
    fn detects_cmake_before_make() {
        let d = TempDir::new().unwrap();
        touch(d.path(), "CMakeLists.txt");
        touch(d.path(), "Makefile");
        touch(d.path(), "main.c");
        assert_eq!(detect_project_type(d.path()), ProjectType::CMake);
    }

    #[test]
    fn detects_cpp() {
        let d = TempDir::new().unwrap();
        touch(d.path(), "Makefile");
        touch(d.path(), "main.cpp");
        assert_eq!(detect_project_type(d.path()), ProjectType::Cpp);
    }

    #[test]
    fn detects_cpp_with_cxx() {
        let d = TempDir::new().unwrap();
        touch(d.path(), "Makefile");
        touch(d.path(), "main.cxx");
        assert_eq!(detect_project_type(d.path()), ProjectType::Cpp);
    }

    #[test]
    fn detects_c() {
        let d = TempDir::new().unwrap();
        touch(d.path(), "Makefile");
        touch(d.path(), "main.c");
        assert_eq!(detect_project_type(d.path()), ProjectType::C);
    }

    #[test]
    fn detects_generic_when_empty() {
        let d = TempDir::new().unwrap();
        assert_eq!(detect_project_type(d.path()), ProjectType::Generic);
    }

    #[test]
    fn slugify_kebabs_and_lowers() {
        assert_eq!(slugify("My Cool App"), "my-cool-app");
        assert_eq!(slugify("foo_bar"), "foo-bar");
        assert_eq!(slugify("--Foo--Bar--"), "foo-bar");
        assert_eq!(slugify("MixedCASE"), "mixedcase");
    }

    #[test]
    fn extracts_cargo_name_and_meta() {
        let d = TempDir::new().unwrap();
        std::fs::write(
            d.path().join("Cargo.toml"),
            "[package]\nname = \"my-crate\"\nversion = \"0.1.0\"\nhomepage = \"https://example.com\"\ndescription = \"hi there\"\n",
        )
        .unwrap();
        let dd = extract_defaults(d.path(), ProjectType::Rust);
        assert_eq!(dd.package_name.as_deref(), Some("my-crate"));
        assert_eq!(dd.homepage.as_deref(), Some("https://example.com"));
        assert_eq!(dd.description.as_deref(), Some("hi there"));
    }

    #[test]
    fn extracts_go_module_basename() {
        let d = TempDir::new().unwrap();
        std::fs::write(d.path().join("go.mod"), "module github.com/me/myapp\n\ngo 1.22\n").unwrap();
        let dd = extract_defaults(d.path(), ProjectType::Go);
        assert_eq!(dd.package_name.as_deref(), Some("myapp"));
    }

    #[test]
    fn extracts_package_json_name() {
        let d = TempDir::new().unwrap();
        std::fs::write(d.path().join("package.json"), r#"{"name":"hello","version":"1.0.0","homepage":"https://x"}"#).unwrap();
        let dd = extract_defaults(d.path(), ProjectType::Electron);
        assert_eq!(dd.package_name.as_deref(), Some("hello"));
        assert_eq!(dd.homepage.as_deref(), Some("https://x"));
    }

    #[test]
    fn extracts_cmake_project_name() {
        let d = TempDir::new().unwrap();
        std::fs::write(d.path().join("CMakeLists.txt"), "cmake_minimum_required(VERSION 3.10)\nproject(mpz VERSION 2.0.4 LANGUAGES CXX C)\n").unwrap();
        let dd = extract_defaults(d.path(), ProjectType::CMake);
        assert_eq!(dd.package_name.as_deref(), Some("mpz"));
    }
}
