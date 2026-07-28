use anyhow::{Context, Result};
use liquid::ParserBuilder;
use liquid::model::{KString, Value};
use regex::Regex;
use std::collections::HashMap;
use std::path::{Path, PathBuf};

#[derive(Clone)]
pub struct Var(Value);

impl From<String> for Var {
    fn from(s: String) -> Self {
        Var(Value::scalar(s))
    }
}

impl From<&str> for Var {
    fn from(s: &str) -> Self {
        Var(Value::scalar(s.to_string()))
    }
}

impl From<i64> for Var {
    fn from(i: i64) -> Self {
        Var(Value::scalar(i))
    }
}

impl From<bool> for Var {
    fn from(b: bool) -> Self {
        Var(Value::scalar(b))
    }
}

impl From<Vec<String>> for Var {
    fn from(v: Vec<String>) -> Self {
        Var(Value::Array(v.into_iter().map(Value::scalar).collect()))
    }
}

impl From<HashMap<String, String>> for Var {
    fn from(map: HashMap<String, String>) -> Self {
        let obj: liquid::Object = map.into_iter().map(|(k, v)| (k.into(), Value::scalar(v))).collect();
        Var(Value::Object(obj))
    }
}

pub struct Template {
    template: liquid::Template,
    source_content: String,
}

impl Template {
    pub fn from_content(content: impl Into<String>) -> Result<Self, anyhow::Error> {
        let source_content = content.into();
        let template = ParserBuilder::with_stdlib().build()?.parse(&source_content)?;
        Ok(Self { source_content, template })
    }

    pub fn from_file(path: impl AsRef<Path>) -> Result<Self, anyhow::Error> {
        let content = std::fs::read_to_string(&path).with_context(|| format!("cannot read template {}", path.as_ref().display()))?;
        Self::from_content(content)
    }

    pub fn render(&self, vars: impl IntoIterator<Item = (String, Var)>) -> Result<String, anyhow::Error> {
        let mut globals: liquid::Object = vars.into_iter().map(|(k, v)| (k.into(), v.0)).collect();

        // Hack for liquid to not fail on missing variable (https://github.com/cobalt-org/liquid-rust/issues/570)
        for cap in Regex::new(r"\{\{-?\s*(\w+)").unwrap().captures_iter(&self.source_content) {
            globals.entry(KString::from_ref(&cap[1])).or_insert(Value::Nil);
        }

        Ok(self.template.render(&globals)?)
    }

    pub fn render_to_file(&self, vars: impl IntoIterator<Item = (String, Var)>, output_path: PathBuf) -> Result<(), anyhow::Error> {
        if let Some(parent) = output_path.parent() {
            std::fs::create_dir_all(parent).with_context(|| format!("cannot create directory {}", parent.display()))?;
        }
        let output = self.render(vars)?;
        std::fs::write(&output_path, &output).with_context(|| format!("cannot write to {}", output_path.display()))?;
        Ok(())
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn make_template(content: &str) -> (Template, tempfile::TempDir) {
        let dir = tempfile::tempdir().unwrap();
        let path = dir.path().join("template.liquid");
        std::fs::write(&path, content).unwrap();
        (Template::from_file(path).unwrap(), dir)
    }

    #[test]
    fn test_render_strings() {
        let (template, _dir) = make_template("Hello, {{ name }}! Version: {{ version }}");
        let output = template.render([("name".to_string(), "world".into()), ("version".to_string(), "1.2.3".into())]).unwrap();
        assert_eq!(output, "Hello, world! Version: 1.2.3");
    }

    #[test]
    fn test_render_integer() {
        let (template, _dir) = make_template("Port: {{ port }}");
        let output = template.render([("port".to_string(), 8080i64.into())]).unwrap();
        assert_eq!(output, "Port: 8080");
    }

    #[test]
    fn test_render_bool() {
        let (template, _dir) = make_template("{% if debug %}DEBUG{% endif %}");
        let output = template.render([("debug".to_string(), true.into())]).unwrap();
        assert_eq!(output, "DEBUG");
    }

    #[test]
    fn test_render_vec() {
        let (template, _dir) = make_template("{% for item in items %}{{ item }} {% endfor %}");
        let output = template.render([("items".to_string(), vec!["foo".to_string(), "bar".to_string(), "baz".to_string()].into())]).unwrap();
        assert_eq!(output, "foo bar baz ");
    }

    #[test]
    fn test_render_to_file() {
        let (template, dir) = make_template("Hello, {{ name }}!");
        let output_path = dir.path().join("output.txt");
        template.render_to_file([("name".to_string(), "world".into())], output_path.clone()).unwrap();
        let content = std::fs::read_to_string(&output_path).unwrap();
        assert_eq!(content, "Hello, world!");
    }

    #[test]
    fn test_render_to_file_creates_directories() {
        let (template, _dir) = make_template("Hello, {{ name }}!");
        let dir = tempfile::tempdir().unwrap();
        let output_path = dir.path().join("nested/dirs/output.txt");
        template.render_to_file([("name".to_string(), "world".into())], output_path.clone()).unwrap();
        let content = std::fs::read_to_string(&output_path).unwrap();
        assert_eq!(content, "Hello, world!");
    }

    #[test]
    fn test_render_unknown_variable_falls_back_to_empty_string() {
        let (template, _dir) = make_template("Hello, {{ name }}! Extra: {{ CMAKE_EXTRA_CLI }}");
        let output = template.render([("name".to_string(), "world".into())]).unwrap();
        assert_eq!(output, "Hello, world! Extra: ");
    }

    #[test]
    fn test_render_undefined_variable_in_if() {
        let (template, _dir) = make_template("{% if LDFLAGS %}{{ LDFLAGS }}{% endif %}");
        let output = template.render([]).unwrap();
        assert_eq!(output, "");
    }
}
