use std::collections::HashMap;

pub const PACKAGE_NAME: &str = "__INIT_PACKAGE_NAME__";
pub const MAINTAINER: &str = "__INIT_MAINTAINER__";
pub const EMAIL: &str = "__INIT_EMAIL__";
pub const HOMEPAGE: &str = "__INIT_HOMEPAGE__";
pub const DESCRIPTION: &str = "__INIT_DESCRIPTION__";
pub const TODAY: &str = "__INIT_TODAY__";
pub const VERSION_FILE: &str = "__INIT_VERSION_FILE__";
pub const VERSION_REGEX: &str = "__INIT_VERSION_REGEX__";
pub const RETAIN_PACKAGES: &str = "__INIT_RETAIN_PACKAGES__";

pub fn apply_tokens(content: &str, vars: &HashMap<&'static str, String>) -> String {
    let mut out = content.to_string();
    for (token, value) in vars {
        out = out.replace(token, value);
    }
    out
}

pub fn today() -> String {
    chrono::Local::now().format("%a %b %d %Y").to_string()
}

#[cfg(test)]
mod tests {
    use super::*;

    fn vars() -> HashMap<&'static str, String> {
        HashMap::from([
            (PACKAGE_NAME, "my-pkg".to_string()),
            (MAINTAINER, "Jane Doe <jane@example.com>".to_string()),
            (EMAIL, "jane@example.com".to_string()),
            (HOMEPAGE, "https://example.com".to_string()),
            (DESCRIPTION, "A nice package".to_string()),
            (TODAY, "* Mon Jan 02 2026 Jane - 1".to_string()),
        ])
    }

    #[test]
    fn replaces_known_tokens() {
        let s = "name: __INIT_PACKAGE_NAME__\nmaintainer: __INIT_MAINTAINER__\n";
        let r = apply_tokens(s, &vars());
        assert!(r.contains("name: my-pkg"));
        assert!(r.contains("maintainer: Jane Doe <jane@example.com>"));
    }

    #[test]
    fn does_not_mangle_liquid_tags() {
        let s = "Source: {{ package_name }}\n{% if foo %}bar{% endif %}\nMaintainer: __INIT_MAINTAINER__\n";
        let r = apply_tokens(s, &vars());
        assert!(r.contains("{{ package_name }}"));
        assert!(r.contains("{% if foo %}bar{% endif %}"));
        assert!(r.contains("Maintainer: Jane Doe <jane@example.com>"));
    }

    #[test]
    fn leaves_unknown_init_tokens_alone() {
        let s = "__INIT_NEW_THING__ __INIT_PACKAGE_NAME__";
        let r = apply_tokens(s, &vars());
        assert!(r.starts_with("__INIT_NEW_THING__ "));
        assert!(r.ends_with("my-pkg"));
    }

    #[test]
    fn today_format() {
        let s = today();
        assert_eq!(s.len(), 15); // "Mon Jan 02 2026"
        assert!(s.contains(' '));
    }
}
