use super::{Repositories, sorted_by_distro_order};
use crate::config::Repository as RepoConfig;
use crate::template::Template;

const PAGE_TEMPLATE_HTML: &str = include_str!("install.html.liquid");

pub(crate) fn render_page(repositories: &Repositories, config: &RepoConfig, custom_template: Option<String>) -> Result<String, anyhow::Error> {
    let template_html = render(repositories, custom_template)?;
    Template::from_content(&template_html)?.render(config.to_template_vars())
}

pub(crate) fn parse(html: &str) -> Result<Repositories, anyhow::Error> {
    let (start, end) = extract_json_bounds(html)?;
    let json = html[start..end].trim();
    Ok(serde_json::from_str(json)?)
}

fn render(repositories: &Repositories, custom_template: Option<String>) -> Result<String, anyhow::Error> {
    let html: &str = custom_template.as_deref().unwrap_or(PAGE_TEMPLATE_HTML);
    let (start_pos, end_pos) = extract_json_bounds(html)?;

    let sorted = sorted_by_distro_order(repositories);
    let json = serde_json::to_string_pretty(&sorted)?;

    let mut rendered = String::with_capacity(html.len() + json.len());
    rendered.push_str(&html[..start_pos]);
    rendered.push_str(&json);
    rendered.push_str(&html[end_pos..]);

    Ok(rendered)
}

fn extract_json_bounds(html: &str) -> Result<(usize, usize), anyhow::Error> {
    let start_tag = r#"<script type="application/json" id="data">"#;
    let start = html.find(start_tag).ok_or_else(|| anyhow::anyhow!("cannot find data script tag"))? + start_tag.len();
    let end = html[start..].find("</script>").ok_or_else(|| anyhow::anyhow!("cannot find closing script tag"))? + start;
    Ok((start, end))
}

#[cfg(test)]
mod tests {
    use super::super::{Repositories, Repository, upsert_repository};
    use super::*;
    use crate::config::{AnyValue, RepositoryProvider};
    use std::collections::HashMap;

    fn fixture() -> String {
        std::fs::read_to_string("tests/fixtures/install.html").expect("cannot read tests/fixtures/install.html")
    }

    #[test]
    fn test_parse() {
        let html = fixture();

        let entries = parse(&html).expect("cannot extract data");

        assert_eq!(entries.len(), 23);

        let first = &entries[0];
        assert_eq!(first["distro_id"], "opensuse_15.5");
        assert_eq!(first["distro_name"], "openSUSE 15.5");
        assert_eq!(first["download_url"], "https://repositories.omnipackage.org/oleg/mpz/opensuse-15-5/mpz-2.0.3-1.x86_64.rpm");
        assert_eq!(
            first["install_steps"],
            "zypper addrepo --refresh https://repositories.omnipackage.org/oleg/mpz/opensuse-15-5/mpz.repo\nzypper refresh\nzypper install mpz"
        );

        let mageia = &entries[21];
        assert_eq!(mageia["distro_id"], "mageia_cauldron");
        assert_eq!(mageia["distro_name"], "Mageia Cauldron");
        assert_eq!(mageia["download_url"], "https://repositories.omnipackage.org/oleg/mpz/mageia-cauldron/mpz-2.0.3-1.mga10.x86_64.rpm");

        let arch = &entries[22];
        assert_eq!(arch["distro_id"], "arch");
        assert_eq!(arch["distro_name"], "Arch Linux");
        assert!(arch["download_url"].ends_with(".pkg.tar.zst"), "arch download_url: {}", arch["download_url"]);
        assert!(arch["install_steps"].contains("pacman-key"), "arch install_steps: {}", arch["install_steps"]);

        let required_keys = ["distro_id", "distro_name", "install_steps", "gpg_key", "download_url"];
        for entry in &entries {
            for key in &required_keys {
                assert!(entry.contains_key(*key), "entry '{}' missing key '{key}'", entry["distro_id"]);
                assert!(!entry[*key].is_empty(), "entry '{}' has empty key '{key}'", entry["distro_id"]);
            }
        }

        let mut ids: Vec<&str> = entries.iter().map(|e| e["distro_id"].as_str()).collect();
        ids.sort();
        ids.dedup();
        assert_eq!(ids.len(), entries.len(), "distro_ids are not unique");

        for entry in &entries {
            let url = &entry["download_url"];
            assert!(
                url.ends_with(".rpm") || url.ends_with(".deb") || url.ends_with(".pkg.tar.zst"),
                "entry '{}' has unexpected download_url: {url}",
                entry["distro_id"]
            );
        }
    }

    #[test]
    fn test_parse_missing_script_tag() {
        let html = "<html><body><p>no data here</p></body></html>";
        let err = parse(html).unwrap_err();
        assert_eq!(err.to_string(), "cannot find data script tag");
    }

    #[test]
    fn test_parse_missing_closing_tag() {
        let html = r#"<script type="application/json" id="data">[{"key": "value"}]"#;
        let err = parse(html).unwrap_err();
        assert_eq!(err.to_string(), "cannot find closing script tag");
    }

    #[test]
    fn test_parse_invalid_json() {
        let html = r#"<script type="application/json" id="data">not json</script>"#;
        let err = parse(html).unwrap_err();
        assert_eq!(err.to_string(), "expected ident at line 1 column 2");
    }

    #[test]
    fn test_parse_empty_array() {
        let html = r#"<script type="application/json" id="data">[]</script>"#;
        let entries = parse(html).expect("should parse empty array");
        assert!(entries.is_empty());
    }

    #[test]
    fn test_render_round_trip() {
        let html = fixture();
        let mut repos = parse(&html).expect("cannot parse");
        repos.sort_by_key(|r| r.get("distro_id").cloned().unwrap_or_default());
        let injected = render(&repos, None).expect("cannot render");
        let mut repos2 = parse(&injected).expect("cannot parse after render");
        repos2.sort_by_key(|r| r.get("distro_id").cloned().unwrap_or_default());
        assert_eq!(repos, repos2);
    }

    #[test]
    fn test_upsert_repository() {
        let html = fixture();
        let mut repos = parse(&html).expect("cannot parse");

        let new_repo = Repository::from([
            ("distro_id".to_string(), "debian_14".to_string()),
            ("distro_name".to_string(), "Debian 14".to_string()),
            ("install_steps".to_string(), "apt install mpz".to_string()),
            ("gpg_key".to_string(), "pub rsa4096 2024-05-10 [SCEA]".to_string()),
            (
                "download_url".to_string(),
                "https://repositories.omnipackage.org/oleg/mpz/debian-14/stable/mpz_2.0.3-0_amd64.deb".to_string(),
            ),
        ]);
        upsert_repository(&mut repos, new_repo);

        let injected = render(&repos, None).expect("cannot render");
        let repos2 = parse(&injected).expect("cannot parse after render");

        assert_eq!(repos2.len(), 24);
        assert_eq!(
            repos2.iter().find(|repo| repo.get("distro_id").is_some_and(|v| v == "debian_14")).unwrap().get("distro_name").unwrap(),
            &"Debian 14".to_string()
        );
    }

    #[test]
    fn test_render_page_merged() {
        let html = fixture();

        let new_repos: Repositories = vec![
            Repository::from([
                ("distro_id".to_string(), "debian_14".to_string()),
                ("distro_name".to_string(), "Debian 14".to_string()),
                ("install_steps".to_string(), "apt install mpz".to_string()),
                ("gpg_key".to_string(), "pub rsa4096 2024-05-10 [SCEA]".to_string()),
                (
                    "download_url".to_string(),
                    "https://repositories.omnipackage.org/oleg/mpz/debian-14/stable/mpz_2.0.3-0_amd64.deb".to_string(),
                ),
                ("package_type".to_string(), "deb".into()),
            ]),
            Repository::from([
                ("distro_id".to_string(), "debian_15".to_string()),
                ("distro_name".to_string(), "Debian 15 LTS".to_string()),
                ("install_steps".to_string(), "apt install mpz".to_string()),
                ("gpg_key".to_string(), "pub rsa4096 2024-05-10 [SCEA]".to_string()),
                (
                    "download_url".to_string(),
                    "https://repositories.omnipackage.org/oleg/mpz/debian-15/stable/mpz_2.0.3-0_amd64.deb".to_string(),
                ),
                ("package_type".to_string(), "deb".into()),
            ]),
        ];

        let rest = HashMap::from([("homepage".to_string(), AnyValue::String("http://testpacka.ge".to_string()))]);
        let repo_conf = RepoConfig {
            name: "this is badge title".into(),
            provider: RepositoryProvider::S3,
            gpg_private_key_base64: "".into(),
            package_name: "test123".into(),
            retain_packages: 0,
            rest,
            s3: None,
            localfs: None,
        };

        let mut repos = parse(&html).expect("cannot parse");
        for repo in new_repos {
            upsert_repository(&mut repos, repo);
        }
        let page = render_page(&repos, &repo_conf, None).unwrap();

        assert!(page.contains("Debian 14"));
        assert!(page.contains("Debian 15 LTS"));
        assert!(page.contains("http://testpacka.ge"));

        assert_eq!(parse(&page).unwrap().len(), 25);
    }
}
