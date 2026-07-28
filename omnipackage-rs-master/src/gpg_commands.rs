use crate::gpg::Gpg;
use crate::{GpgConvertArgs, GpgGenerateArgs};
use anyhow::{Context, Result};
use base64::{Engine, engine::general_purpose};

pub fn generate(args: GpgGenerateArgs) -> Result<(), anyhow::Error> {
    let keys = Gpg::new().generate_keys(&args.name, &args.email)?;

    let priv_content = match args.format.as_str() {
        "base64" => {
            use base64::{Engine, engine::general_purpose};
            general_purpose::STANDARD.encode(&keys.priv_key)
        }
        _ => keys.priv_key,
    };

    println!("{}", priv_content);

    Ok(())
}

pub fn convert(args: GpgConvertArgs) -> Result<(), anyhow::Error> {
    let content = match &args.input {
        Some(path) => std::fs::read(path)?,
        None => {
            use std::io::Read;
            let mut buf = Vec::new();
            std::io::stdin().read_to_end(&mut buf)?;
            buf
        }
    };

    let decoded = match args.input_format.as_str() {
        "base64" => {
            let stripped: Vec<u8> = content.iter().copied().filter(|b| !b.is_ascii_whitespace()).collect();
            general_purpose::STANDARD.decode(&stripped).context("Failed to decode base64 input")?
        }
        _ => content,
    };

    let output_content = match args.output_format.as_str() {
        "base64" => general_purpose::STANDARD.encode(&decoded),
        _ => String::from_utf8(decoded).context("Decoded key is not valid UTF-8")?,
    };

    println!("{}", output_content);

    Ok(())
}
