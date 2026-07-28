use super::Repositories;
use crate::config::Repository as RepoConfig;
use crate::template::{Template, Var};
use std::collections::HashMap;

const BADGE_TEMPLATE_SVG: &str = include_str!("badge.svg.liquid");

pub fn render(repositories: &Repositories, config: &RepoConfig) -> Result<String, anyhow::Error> {
    let mut vars = config.to_template_vars();

    let (rpm_count, deb_count, pacman_count) = repositories.iter().fold((0, 0, 0), |(rpm, deb, pacman), r| match r.get("package_type").map(|t| t.as_str()) {
        Some("rpm") => (rpm + 1, deb, pacman),
        Some("deb") => (rpm, deb + 1, pacman),
        Some("pacman") => (rpm, deb, pacman + 1),
        _ => (rpm, deb, pacman),
    });

    let mut aux = format!("{rpm_count} RPM {deb_count} DEB");
    if pacman_count > 0 {
        aux.push_str(&format!(" {pacman_count} PAC"));
    }
    vars.extend(badge_vars(config.name.clone(), aux));

    Template::from_content(BADGE_TEMPLATE_SVG)?.render(vars)
}

fn char_width_11(c: char) -> f64 {
    match c {
        ' ' => 4.0,
        'i' | 'l' | 'I' | '.' | ',' | ':' | ';' | '|' | '!' | '\'' | '`' => 3.8,
        'f' | 'j' | 't' | 'r' => 4.8,
        'm' => 10.0,
        'w' => 9.0,
        'M' => 10.5,
        'W' => 11.5,
        c if c.is_ascii_digit() => 7.2,
        c if c.is_ascii_lowercase() => 6.8,
        c if c.is_ascii_uppercase() => 8.4,
        _ => 7.3,
    }
}

fn measure_text_width(text: &str) -> f64 {
    text.chars().map(char_width_11).sum()
}

fn badge_vars(title: String, aux: String) -> HashMap<String, Var> {
    let left_w = (25.4 + measure_text_width(&title)).ceil() as u32;
    let right_w = (14.0 + measure_text_width(&aux)).ceil() as u32;
    let total_w = left_w + right_w;
    let aux_cx = left_w as f64 + right_w as f64 / 2.0;

    let mut map = HashMap::new();
    map.insert("TITLE".to_string(), title.into());
    map.insert("AUX".to_string(), aux.into());
    map.insert("LEFT_W".to_string(), left_w.to_string().into());
    map.insert("RIGHT_W".to_string(), right_w.to_string().into());
    map.insert("TOTAL_W".to_string(), total_w.to_string().into());
    map.insert("AUX_CX".to_string(), format!("{:.1}", aux_cx).into());
    map
}

#[cfg(test)]
mod tests {
    use super::super::{Repositories, Repository};
    use super::*;
    use crate::config::RepositoryProvider;

    #[test]
    fn test_badge_counts_pacman() {
        let repo_conf = RepoConfig {
            name: "title".into(),
            provider: RepositoryProvider::S3,
            gpg_private_key_base64: "".into(),
            package_name: "pkg".into(),
            retain_packages: 0,
            rest: HashMap::new(),
            s3: None,
            localfs: None,
        };
        let repos: Repositories = vec![
            Repository::from([("distro_id".to_string(), "arch".to_string()), ("package_type".to_string(), "pacman".to_string())]),
            Repository::from([("distro_id".to_string(), "fedora_40".to_string()), ("package_type".to_string(), "rpm".to_string())]),
        ];

        let badge = render(&repos, &repo_conf).unwrap();
        assert!(badge.contains("1 RPM 0 DEB 1 PAC"), "badge missing pacman count: {badge}");
    }
}
