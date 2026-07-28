use std::sync::OnceLock;

struct Config {
    colors: bool,
}

static CONFIG: OnceLock<Config> = OnceLock::new();

fn config() -> &'static Config {
    CONFIG.get_or_init(|| Config {
        colors: std::env::var("NO_COLOR").is_err(),
    })
}

fn timestamp() -> String {
    chrono::Local::now().format("%H:%M:%S").to_string()
}

#[derive(Clone, Copy)]
pub enum Color {
    Red,
    Green,
    Yellow,
    Cyan,
    Bold,
    BoldRed,
    BoldGreen,
    BoldYellow,
    BoldCyan,
}

impl Color {
    fn code(self) -> &'static str {
        match self {
            Color::Red => "\x1b[31m",
            Color::Green => "\x1b[32m",
            Color::Yellow => "\x1b[33m",
            Color::Cyan => "\x1b[36m",
            Color::Bold => "\x1b[1m",
            Color::BoldRed => "\x1b[1;31m",
            Color::BoldGreen => "\x1b[1;32m",
            Color::BoldYellow => "\x1b[1;33m",
            Color::BoldCyan => "\x1b[1;36m",
        }
    }
}

fn colorize_with(colors: bool, color: Color, text: impl std::fmt::Display) -> String {
    if colors { format!("{}{}\x1b[0m", color.code(), text) } else { format!("{}", text) }
}

pub fn colorize(color: Color, text: impl std::fmt::Display) -> String {
    colorize_with(config().colors, color, text)
}

pub enum LogOutput {
    Silent,
    Stdout,
    Stderr,
}

pub struct Logger {
    secrets: Vec<String>,
    output: LogOutput,
}

impl Logger {
    pub fn new() -> Self {
        Self {
            secrets: vec![],
            output: LogOutput::Stdout,
        }
    }

    pub fn with_output(mut self, output: LogOutput) -> Self {
        self.output = output;
        self
    }

    pub fn with_secrets(mut self, secrets: Vec<String>) -> Self {
        self.secrets = secrets;
        self
    }

    pub fn print(&self, msg: impl std::fmt::Display) -> String {
        let msg = self.redact(msg.to_string());
        match self.output {
            LogOutput::Stdout => println!("{}", msg),
            LogOutput::Stderr => eprintln!("{}", msg),
            LogOutput::Silent => {}
        }
        msg
    }

    pub fn info(&self, msg: impl std::fmt::Display) {
        self.print(format!("{} {} {}", colorize(Color::Cyan, timestamp()), colorize(Color::Green, "[I]"), msg));
    }

    pub fn warn(&self, msg: impl std::fmt::Display) {
        self.print(format!("{} {} {}", colorize(Color::Cyan, timestamp()), colorize(Color::Yellow, "[W]"), msg));
    }

    pub fn error(&self, msg: impl std::fmt::Display) {
        self.print(format!("{} {} {}", colorize(Color::Cyan, timestamp()), colorize(Color::Red, "[E]"), msg));
    }

    pub fn cmd(&self, program: &str, args: &[String], env: &[(String, String)]) {
        let env_str = if env.is_empty() {
            String::new()
        } else {
            env.iter().map(|(k, v)| format!("{}={}", k, v)).collect::<Vec<_>>().join(" ") + " "
        };

        self.print(format!(
            "{} {} {}",
            colorize(Color::Cyan, timestamp()),
            colorize(Color::Cyan, "$"),
            colorize(Color::Bold, format!("{}{} {}", env_str, program, args.join(" ")))
        ));
    }

    pub fn redact(&self, msg: String) -> String {
        self.secrets.iter().fold(msg, |acc, s| if s.is_empty() { acc } else { acc.replace(s.as_str(), "[REDACTED]") })
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_colorize_disabled() {
        assert_eq!(colorize_with(false, Color::Red, "hello"), "hello");
    }

    #[test]
    fn test_colorize_enabled() {
        let result = colorize_with(true, Color::Red, "hello");
        assert!(result.contains("\x1b[31m"));
        assert!(result.contains("hello"));
        assert!(result.ends_with("\x1b[0m"));
    }

    #[test]
    fn test_timestamp_format() {
        let ts = timestamp();
        assert_eq!(ts.len(), 8);
        assert_eq!(ts.chars().nth(2), Some(':'));
        assert_eq!(ts.chars().nth(5), Some(':'));
    }

    #[test]
    fn test_redact_single_secret() {
        let logger = Logger::new().with_secrets(vec!["secret123".to_string()]);
        assert_eq!(logger.redact("token is secret123".to_string()), "token is [REDACTED]");
    }

    #[test]
    fn test_redact_multiple_secrets() {
        let logger = Logger::new().with_secrets(vec!["secret123".to_string(), "password".to_string()]);
        assert_eq!(logger.redact("secret123 and password".to_string()), "[REDACTED] and [REDACTED]");
    }

    #[test]
    fn test_redact_no_match() {
        let logger = Logger::new().with_secrets(vec!["secret123".to_string()]);
        assert_eq!(logger.redact("hello world".to_string()), "hello world");
    }

    #[test]
    fn test_redact_empty_secrets() {
        let logger = Logger::new();
        assert_eq!(logger.redact("hello world".to_string()), "hello world");
    }

    #[test]
    fn test_redact_empty_secret_string() {
        let logger = Logger::new().with_secrets(vec!["".to_string()]);
        assert_eq!(logger.redact("hello world".to_string()), "hello world");
    }
}
