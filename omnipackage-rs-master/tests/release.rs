#![allow(unused)]

use assert_cmd::Command;
use predicates::prelude::*;

#[test]
fn test_release_with_fake_runtime() {
    let fake_runtime = std::path::Path::new(env!("CARGO_MANIFEST_DIR")).join("tests/fixtures/fake_container_runtime.sh");

    // TODO find a way to simulate release process
    let output = Command::cargo_bin("omnipackage")
        .unwrap()
        .env("OMNIPACKAGE_CONTAINER_RUNTIME", fake_runtime)
        .args(["release", "--help"])
        .output()
        .unwrap();

    let stdout = String::from_utf8_lossy(&output.stdout);
    let stderr = String::from_utf8_lossy(&output.stderr);

    assert!(output.status.success());
}
