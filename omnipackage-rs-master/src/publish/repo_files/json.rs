use super::Repositories;
use serde_json::{Map, Value};

pub(crate) fn parse(existing_json: &str) -> Repositories {
    let trimmed = existing_json.trim();
    if trimmed.is_empty() {
        return Vec::new();
    }
    let objects: Vec<Map<String, Value>> = serde_json::from_str(trimmed).unwrap_or_default();
    objects
        .into_iter()
        .map(|obj| {
            obj.into_iter()
                .map(|(k, v)| {
                    let s = match v {
                        Value::Array(items) => items.iter().filter_map(Value::as_str).collect::<Vec<_>>().join("\n"),
                        Value::String(s) => s,
                        other => other.to_string(),
                    };
                    (k, s)
                })
                .collect()
        })
        .collect()
}

pub(crate) fn to_json(repos: &Repositories) -> Result<String, anyhow::Error> {
    let objects: Vec<Map<String, Value>> = repos
        .iter()
        .map(|entry| {
            entry
                .iter()
                .map(|(k, v)| {
                    let value = if k == "install_steps" {
                        Value::Array(v.split('\n').map(|line| Value::String(line.to_string())).collect())
                    } else {
                        Value::String(v.clone())
                    };
                    (k.clone(), value)
                })
                .collect()
        })
        .collect();
    Ok(serde_json::to_string_pretty(&objects)?)
}

#[cfg(test)]
mod tests {
    use super::super::Repository;
    use super::*;

    fn entry(id: &str, extra: &[(&str, &str)]) -> Repository {
        let mut m = Repository::from([("distro_id".to_string(), id.to_string())]);
        for (k, v) in extra {
            m.insert((*k).to_string(), (*v).to_string());
        }
        m
    }

    #[test]
    fn test_to_json_produces_bare_array() {
        let json = to_json(&vec![entry("ubuntu_24.04", &[("arch", "x86_64"), ("package_name", "omnipackage")])]).unwrap();
        assert!(json.trim_start().starts_with('['), "not a bare array: {json}");
        let parsed: Vec<Value> = serde_json::from_str(&json).unwrap();
        assert_eq!(parsed[0]["arch"], "x86_64");
        assert_eq!(parsed[0]["package_name"], "omnipackage");
    }

    #[test]
    fn test_install_steps_serialized_as_array() {
        let json = to_json(&vec![entry("ubuntu_24.04", &[("install_steps", "line one\nline two\nline three")])]).unwrap();
        let parsed: Vec<Value> = serde_json::from_str(&json).unwrap();
        let steps = &parsed[0]["install_steps"];
        assert!(steps.is_array(), "install_steps not an array: {steps}");
        assert_eq!(steps.as_array().unwrap().len(), 3);
        assert_eq!(steps[0], "line one");
        assert_eq!(steps[2], "line three");
    }

    #[test]
    fn test_parse_reads_array_steps_back_to_joined_string() {
        let json = to_json(&vec![entry("arch", &[("install_steps", "a\nb")])]).unwrap();
        let repos = parse(&json);
        assert_eq!(repos.len(), 1);
        assert_eq!(repos[0]["install_steps"], "a\nb", "array steps must read back as a joined string");
    }

    #[test]
    fn test_parse_tolerates_string_steps() {
        let repos = parse(r#"[{"distro_id":"arch","install_steps":"a\nb"}]"#);
        assert_eq!(repos[0]["install_steps"], "a\nb");
    }

    #[test]
    fn test_parse_empty_is_empty() {
        assert!(parse("").is_empty());
        assert!(parse("   ").is_empty());
    }
}
