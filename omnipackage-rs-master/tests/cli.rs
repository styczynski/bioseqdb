use assert_cmd::Command;
use predicates::prelude::*;
use std::fs;
use tempfile::TempDir;

#[test]
fn test_help() {
    Command::cargo_bin("omnipackage").unwrap().arg("--help").assert().success().stdout(predicate::str::contains("Usage"));
}

#[test]
fn test_version() {
    Command::cargo_bin("omnipackage")
        .unwrap()
        .arg("--version")
        .assert()
        .success()
        .stdout(predicate::str::contains(env!("CARGO_PKG_VERSION")));
}

#[test]
fn test_build_help() {
    Command::cargo_bin("omnipackage")
        .unwrap()
        .arg("build")
        .arg("--help")
        .assert()
        .success()
        .stdout(predicate::str::contains("--distros"));
}

#[test]
fn test_build_dir_default() {
    Command::cargo_bin("omnipackage")
        .unwrap()
        .env_remove("OMNIPACKAGE_BUILD_DIR")
        .args(["build", "--help"])
        .assert()
        .success()
        .stdout(predicate::str::contains(std::env::temp_dir().to_string_lossy().as_ref()));
}

#[test]
fn test_build_dir_env_var() {
    Command::cargo_bin("omnipackage")
        .unwrap()
        .env("OMNIPACKAGE_BUILD_DIR", "/tmp/my-build-dir")
        .args(["build", "--help"])
        .assert()
        .success()
        .stdout(predicate::str::contains("/tmp/my-build-dir"));
}

#[test]
fn test_build_dir_env_var_empty_falls_back_to_default() {
    Command::cargo_bin("omnipackage")
        .unwrap()
        .env("OMNIPACKAGE_BUILD_DIR", "")
        .args(["build", "--help"])
        .assert()
        .success()
        .stdout(predicate::str::contains(std::env::temp_dir().to_string_lossy().as_ref()));
}

#[test]
fn test_init_help() {
    Command::cargo_bin("omnipackage")
        .unwrap()
        .args(["init", "--help"])
        .assert()
        .success()
        .stdout(predicate::str::contains("--package-name"))
        .stdout(predicate::str::contains("--maintainer"))
        .stdout(predicate::str::contains("--type"))
        .stdout(predicate::str::contains("--force"))
        .stdout(predicate::str::contains("--dry-run"))
        .stdout(predicate::str::contains("--retain-packages"));
}

#[test]
fn test_gpg_generate() {
    Command::cargo_bin("omnipackage")
        .unwrap()
        .args(["gpg", "generate", "--name", "John Doe", "--email", "john@example.com"])
        .assert()
        .success()
        .stdout(predicate::str::contains("BEGIN PGP PRIVATE KEY BLOCK"))
        .stdout(predicate::str::contains("BEGIN PGP PUBLIC KEY BLOCK").not());
}
/* TODO integration test with smaple_project in fixtures
#[test]
fn test_build_dir_custom() {
    Command::cargo_bin("omnipackage")
        .unwrap()
        .args(["build", ".", "--build-dir", "/tmp/custom"])
        .assert()
        .success();
}*/

fn run_init(dir: &std::path::Path, ty: &str) {
    Command::cargo_bin("omnipackage")
        .unwrap()
        .args(["init", dir.to_str().unwrap(), "--type", ty, "--maintainer", "T", "--email", "t@x"])
        .assert()
        .success();
}

fn assert_parses(dir: &std::path::Path) {
    Command::cargo_bin("omnipackage").unwrap().args(["info", dir.to_str().unwrap(), "--list-distros"]).assert().success();
}

fn assert_core_files(dir: &std::path::Path) {
    let omni = dir.join(".omnipackage");
    assert!(omni.join("config.yml").exists(), "config.yml");
    assert!(omni.join("specfile.spec.liquid").exists(), "specfile.spec.liquid");
    assert!(omni.join("deb/control.liquid").exists(), "deb/control.liquid");
    assert!(omni.join("deb/changelog.liquid").exists(), "deb/changelog.liquid");
    assert!(omni.join("deb/compat.liquid").exists(), "deb/compat.liquid");
    assert!(omni.join("deb/rules.liquid").exists(), "deb/rules.liquid");
    assert!(omni.join("PKGBUILD.liquid").exists(), "PKGBUILD.liquid");
}

#[test]
fn init_rust_generates_parseable_config() {
    let d = TempDir::new().unwrap();
    fs::write(d.path().join("Cargo.toml"), "[package]\nname = \"demo\"\nversion = \"0.1.0\"\n").unwrap();
    run_init(d.path(), "rust");
    assert_core_files(d.path());
    assert!(d.path().join(".omnipackage/install_rust.sh").exists());
    // pacman support: config carries the anchor + arch/manjaro builds.
    let config = fs::read_to_string(d.path().join(".omnipackage/config.yml")).unwrap();
    assert!(config.contains("pkgbuild_template"), "config should reference pkgbuild_template, got:\n{config}");
    assert!(config.contains("distro: \"arch\""), "config should add arch build, got:\n{config}");
    assert!(config.contains("distro: \"manjaro\""), "config should add manjaro build, got:\n{config}");
    assert_parses(d.path());
}

#[test]
fn init_retain_packages_defaults_to_zero() {
    let d = TempDir::new().unwrap();
    fs::write(d.path().join("Cargo.toml"), "[package]\nname = \"demo\"\nversion = \"0.1.0\"\n").unwrap();
    run_init(d.path(), "rust");
    let config = fs::read_to_string(d.path().join(".omnipackage/config.yml")).unwrap();
    assert!(config.contains("retain_packages: 0"), "expected disabled default, got:\n{config}");
    assert_parses(d.path());
}

#[test]
fn init_retain_packages_flag_sets_value() {
    let d = TempDir::new().unwrap();
    fs::write(d.path().join("Cargo.toml"), "[package]\nname = \"demo\"\nversion = \"0.1.0\"\n").unwrap();
    Command::cargo_bin("omnipackage")
        .unwrap()
        .args(["init", d.path().to_str().unwrap(), "--type", "rust", "--maintainer", "T", "--email", "t@x", "--retain-packages", "3"])
        .assert()
        .success();
    let config = fs::read_to_string(d.path().join(".omnipackage/config.yml")).unwrap();
    assert!(config.contains("retain_packages: 3"), "expected retain_packages: 3, got:\n{config}");
    assert_parses(d.path());
}

#[test]
fn init_go_generates_parseable_config() {
    let d = TempDir::new().unwrap();
    fs::write(d.path().join("go.mod"), "module example.com/demo\n").unwrap();
    run_init(d.path(), "go");
    assert_core_files(d.path());
    assert!(d.path().join(".omnipackage/install_go.sh").exists());
    assert_parses(d.path());
}

#[test]
fn init_python_generates_parseable_config() {
    let d = TempDir::new().unwrap();
    fs::write(d.path().join("main.py"), "VERSION = \"0.1.0\"\n").unwrap();
    run_init(d.path(), "python");
    assert_core_files(d.path());
    assert!(d.path().join(".omnipackage/install.sh").exists());
    assert_parses(d.path());
}

#[test]
fn init_ruby_generates_parseable_config() {
    let d = TempDir::new().unwrap();
    fs::write(d.path().join("Gemfile"), "source 'https://rubygems.org'\n").unwrap();
    run_init(d.path(), "ruby");
    assert_core_files(d.path());
    assert!(d.path().join(".omnipackage/install.sh").exists());
    assert_parses(d.path());
}

#[test]
fn init_crystal_generates_parseable_config() {
    let d = TempDir::new().unwrap();
    fs::write(d.path().join("shard.yml"), "name: demo\nversion: 0.1.0\n").unwrap();
    run_init(d.path(), "crystal");
    assert_core_files(d.path());
    assert!(d.path().join(".omnipackage/install_crystal.sh").exists());
    assert_parses(d.path());
}

#[test]
fn init_c_generates_parseable_config() {
    let d = TempDir::new().unwrap();
    fs::write(d.path().join("Makefile"), "all:\n").unwrap();
    fs::write(d.path().join("main.c"), "int main(){return 0;}\n").unwrap();
    run_init(d.path(), "c");
    assert_core_files(d.path());
    assert_parses(d.path());
}

#[test]
fn init_cpp_generates_parseable_config() {
    let d = TempDir::new().unwrap();
    fs::write(d.path().join("Makefile"), "all:\n").unwrap();
    fs::write(d.path().join("main.cpp"), "int main(){return 0;}\n").unwrap();
    run_init(d.path(), "cpp");
    assert_core_files(d.path());
    assert_parses(d.path());
}

#[test]
fn init_cmake_generates_parseable_config() {
    let d = TempDir::new().unwrap();
    fs::write(d.path().join("CMakeLists.txt"), "project(demo VERSION 0.1.0)\n").unwrap();
    run_init(d.path(), "cmake");
    assert_core_files(d.path());
    assert_parses(d.path());
}

#[test]
fn init_electron_generates_parseable_config_with_postinst() {
    let d = TempDir::new().unwrap();
    fs::write(d.path().join("package.json"), "{\"name\":\"demo-electron\",\"version\":\"1.0.0\"}").unwrap();
    run_init(d.path(), "electron");
    assert_core_files(d.path());
    assert!(d.path().join(".omnipackage/install.sh").exists());
    // postinst filename should be substituted with the slug
    assert!(d.path().join(".omnipackage/deb/demo-electron.postinst").exists(), "postinst with substituted name");
    assert_parses(d.path());
}

#[test]
fn init_tauri_generates_parseable_config() {
    let d = TempDir::new().unwrap();
    fs::create_dir(d.path().join("src-tauri")).unwrap();
    fs::write(d.path().join("src-tauri/Cargo.toml"), "[package]\nname = \"demo-tauri\"\nversion = \"0.1.0\"\n").unwrap();
    run_init(d.path(), "tauri");
    assert_core_files(d.path());
    assert!(d.path().join(".omnipackage/install_rust.sh").exists());
    assert_parses(d.path());
}

#[test]
fn init_generic_generates_parseable_config() {
    let d = TempDir::new().unwrap();
    run_init(d.path(), "generic");
    assert_core_files(d.path());
    assert_parses(d.path());
}

#[test]
fn init_refuses_to_overwrite_without_force() {
    let d = TempDir::new().unwrap();
    fs::write(d.path().join("Cargo.toml"), "[package]\nname = \"demo\"\nversion = \"0.1.0\"\n").unwrap();
    run_init(d.path(), "rust");
    Command::cargo_bin("omnipackage")
        .unwrap()
        .args(["init", d.path().to_str().unwrap(), "--type", "rust", "--maintainer", "T", "--email", "t@x"])
        .assert()
        .failure()
        .stderr(predicate::str::contains("Refusing to overwrite"));
}

#[test]
fn init_force_overwrites() {
    let d = TempDir::new().unwrap();
    fs::write(d.path().join("Cargo.toml"), "[package]\nname = \"demo\"\nversion = \"0.1.0\"\n").unwrap();
    run_init(d.path(), "rust");
    Command::cargo_bin("omnipackage")
        .unwrap()
        .args(["init", d.path().to_str().unwrap(), "--type", "rust", "--maintainer", "T", "--email", "t@x", "--force"])
        .assert()
        .success();
}

#[test]
fn init_dry_run_does_not_write() {
    let d = TempDir::new().unwrap();
    Command::cargo_bin("omnipackage")
        .unwrap()
        .args(["init", d.path().to_str().unwrap(), "--type", "generic", "--maintainer", "T", "--email", "t@x", "--dry-run"])
        .assert()
        .success()
        .stdout(predicate::str::contains("Would create"));
    assert!(!d.path().join(".omnipackage").exists());
}

#[test]
fn init_empty_dir_falls_back_to_generic() {
    let d = TempDir::new().unwrap();
    Command::cargo_bin("omnipackage")
        .unwrap()
        .args(["init", d.path().to_str().unwrap(), "--maintainer", "T", "--email", "t@x"])
        .assert()
        .success()
        .stdout(predicate::str::contains("generic"));
}
