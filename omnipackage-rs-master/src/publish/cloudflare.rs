use anyhow::Result;
use reqwest::blocking::Client;

pub struct CloudflareApi {
    pub api_token: String,
    pub zone_id: String,
}

impl CloudflareApi {
    pub fn new(zone_id: String, api_token: String) -> Self {
        Self { zone_id, api_token }
    }

    pub fn purge_by_prefix(&self, prefix: &str) -> Result<(), anyhow::Error> {
        let _ = Client::new()
            .post(format!("https://api.cloudflare.com/client/v4/zones/{}/purge_cache", self.zone_id))
            .bearer_auth(&self.api_token)
            .json(&serde_json::json!({ "prefixes": [prefix] }))
            .send()?
            .text()?;

        Ok(())
    }
}
