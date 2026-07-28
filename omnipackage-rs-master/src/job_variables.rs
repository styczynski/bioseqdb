use crate::template::Var;
use chrono::Utc;
use std::collections::HashMap;

#[derive(Debug, Clone)]
pub struct JobVariables {
    pub version: String,
    pub current_time_rfc2822: String,
    pub secrets: HashMap<String, String>,
}

impl JobVariables {
    pub fn new(version: String) -> Self {
        JobVariables {
            version,
            current_time_rfc2822: Utc::now().to_rfc2822(),
            secrets: HashMap::new(),
        }
    }

    pub fn with_secrets(mut self, secrets: HashMap<String, String>) -> Self {
        self.secrets = secrets;
        self
    }

    pub fn to_template_vars(&self) -> HashMap<String, Var> {
        let mut vars = HashMap::new();
        vars.insert("version".to_string(), self.version.clone().into());
        vars.insert("current_time_rfc2822".to_string(), self.current_time_rfc2822.clone().into());
        vars.insert("secrets".to_string(), self.secrets.clone().into());
        vars
    }
}

impl std::fmt::Display for JobVariables {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        let secret_keys = self.secrets.keys().map(String::as_str).collect::<Vec<_>>().join(", ");
        write!(f, "version={} current_time_rfc2822={} secrets=[{}]", self.version, self.current_time_rfc2822, secret_keys)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_job_variables_build() {
        let vars = JobVariables::new("1.2.3".to_string());

        assert_eq!(vars.version, "1.2.3");
        assert!(!vars.current_time_rfc2822.is_empty());

        // verify it's valid RFC 2822 by parsing it back
        assert!(chrono::DateTime::parse_from_rfc2822(&vars.current_time_rfc2822).is_ok());

        let re = regex::Regex::new(r"^\w{3}, \d{1,2} \w{3} \d{4} \d{2}:\d{2}:\d{2} [+-]\d{4}$").unwrap();
        assert!(re.is_match(&vars.current_time_rfc2822), "unexpected format: {}", vars.current_time_rfc2822);
    }

    #[test]
    fn test_to_template_vars() {
        let mut secrets = HashMap::new();
        secrets.insert("api_key".to_string(), "abc123".to_string());

        let vars = JobVariables {
            version: "1.2.3".to_string(),
            current_time_rfc2822: "Mon, 1 Jan 2024 12:00:00 +0000".to_string(),
            secrets,
        };

        let template_vars = vars.to_template_vars();

        assert!(template_vars.contains_key("version"));
        assert!(template_vars.contains_key("current_time_rfc2822"));
        assert!(template_vars.contains_key("secrets"));
    }

    #[test]
    fn test_to_template_vars_rendered() {
        let mut secrets = HashMap::new();
        secrets.insert("api_key".to_string(), "abc123".to_string());

        let vars = JobVariables {
            version: "1.2.3".to_string(),
            current_time_rfc2822: "Mon, 1 Jan 2024 12:00:00 +0000".to_string(),
            secrets,
        };

        let dir = tempfile::tempdir().unwrap();
        let path = dir.path().join("template.liquid");
        std::fs::write(&path, "{{ version }} {{ secrets.api_key }}").unwrap();

        let template = crate::template::Template::from_file(path).unwrap();
        let output = template.render(vars.to_template_vars()).unwrap();
        assert_eq!(output, "1.2.3 abc123");
    }

    #[test]
    fn test_display() {
        let vars = JobVariables {
            version: "3.2.1".to_string(),
            current_time_rfc2822: "Mon, 1 Jan 2024 12:00:00 +0000".to_string(),
            secrets: HashMap::new(),
        };
        assert_eq!(vars.to_string(), "version=3.2.1 current_time_rfc2822=Mon, 1 Jan 2024 12:00:00 +0000 secrets=[]");
    }

    #[test]
    fn test_display_with_secrets() {
        let mut secrets = HashMap::new();
        secrets.insert("api_key".to_string(), "abc123".to_string());
        secrets.insert("token".to_string(), "xyz789".to_string());

        let vars = JobVariables {
            version: "3.2.1".to_string(),
            current_time_rfc2822: "Mon, 1 Jan 2024 12:00:00 +0000".to_string(),
            secrets,
        };

        let display = vars.to_string();
        assert!(display.contains("version=3.2.1"));
        assert!(display.contains("api_key"));
        assert!(display.contains("token"));
        assert!(!display.contains("abc123"));
        assert!(!display.contains("xyz789"));
    }
}
