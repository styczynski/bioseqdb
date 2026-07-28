use crate::config::S3Config;
use anyhow::{Context, Result};
use aws_config::BehaviorVersion;
use aws_sdk_s3::config::retry::RetryConfig;
use aws_sdk_s3::config::{Builder, Credentials, Region, RequestChecksumCalculation, ResponseChecksumValidation};
use aws_sdk_s3::error::{DisplayErrorContext, SdkError};
use aws_sdk_s3::primitives::ByteStream;
use aws_sdk_s3::{Client, Config};
use std::collections::HashSet;
use std::path::{Path, PathBuf};
use std::sync::OnceLock;

mod probe;

pub struct S3 {
    client: Client,
    bucket: String,
    path: String,
    supports_if_match: OnceLock<bool>,
}

#[derive(Debug)]
pub enum UploadError {
    PreconditionFailed,
    Other(anyhow::Error),
}

impl std::fmt::Display for UploadError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            UploadError::PreconditionFailed => write!(f, "S3 precondition failed (412)"),
            UploadError::Other(e) => write!(f, "{:#}", e),
        }
    }
}

impl std::error::Error for UploadError {}

impl From<anyhow::Error> for UploadError {
    fn from(e: anyhow::Error) -> Self {
        UploadError::Other(e)
    }
}

fn block<F: std::future::Future>(f: F) -> F::Output {
    static RT: std::sync::OnceLock<tokio::runtime::Runtime> = std::sync::OnceLock::new();
    RT.get_or_init(|| tokio::runtime::Builder::new_current_thread().enable_all().build().expect("cannot create tokio runtime"))
        .block_on(f)
}

#[derive(Debug, PartialEq, Eq)]
struct ExplicitCredentials {
    access_key_id: String,
    secret_access_key: String,
    session_token: Option<String>,
}

fn non_empty(value: &Option<String>) -> Option<&str> {
    value.as_deref().filter(|value| !value.trim().is_empty())
}

fn explicit_credentials(config: &S3Config) -> Result<Option<ExplicitCredentials>, anyhow::Error> {
    let access_key_id = non_empty(&config.access_key_id);
    let secret_access_key = non_empty(&config.secret_access_key);
    let session_token = non_empty(&config.session_token);

    match (access_key_id, secret_access_key, session_token) {
        (None, None, None) => Ok(None),
        (Some(access_key_id), Some(secret_access_key), session_token) => Ok(Some(ExplicitCredentials {
            access_key_id: access_key_id.to_string(),
            secret_access_key: secret_access_key.to_string(),
            session_token: session_token.map(str::to_string),
        })),
        (None, None, Some(_)) => Err(anyhow::anyhow!("S3 session_token requires explicit access_key_id and secret_access_key")),
        _ => Err(anyhow::anyhow!("S3 access_key_id and secret_access_key must either both be set or both be omitted")),
    }
}

fn build_client(config: &S3Config) -> Result<Client, anyhow::Error> {
    let region = Region::new(config.region.as_deref().unwrap_or("auto").to_string());
    let mut builder = match explicit_credentials(config)? {
        Some(credentials) => Config::builder().credentials_provider(Credentials::new(
            credentials.access_key_id,
            credentials.secret_access_key,
            credentials.session_token,
            None,
            "omnipackage-config",
        )),
        None => {
            let shared_config = block(aws_config::defaults(BehaviorVersion::latest()).region(region.clone()).load());
            Builder::from(&shared_config)
        }
    };

    builder = builder
        .endpoint_url(&config.endpoint)
        .region(region)
        .force_path_style(config.force_path_style)
        .request_checksum_calculation(RequestChecksumCalculation::WhenRequired)
        .response_checksum_validation(ResponseChecksumValidation::WhenRequired)
        .behavior_version(BehaviorVersion::latest())
        .retry_config(RetryConfig::adaptive().with_max_attempts(10));

    Ok(Client::from_conf(builder.build()))
}

impl S3 {
    pub fn new(config: &S3Config, path: impl Into<String>) -> Result<Self, anyhow::Error> {
        Ok(Self {
            client: build_client(config)?,
            bucket: config.bucket.clone(),
            path: path.into(),
            supports_if_match: OnceLock::new(),
        })
    }

    pub fn supports_if_match(&self) -> bool {
        if let Some(v) = self.supports_if_match.get() {
            return *v;
        }
        let detected = block(probe::detect(&self.client, &self.bucket, &self.path));
        *self.supports_if_match.get_or_init(|| detected)
    }

    pub fn bucket_exists(&self) -> Result<bool, anyhow::Error> {
        block(async {
            match self.client.head_bucket().bucket(&self.bucket).send().await {
                Ok(_) => Ok(true),
                Err(e) => {
                    let ee = e.into_service_error();
                    if ee.is_not_found() {
                        Ok(false)
                    } else {
                        Err(anyhow::anyhow!("error checking bucket '{}': {}", self.bucket, DisplayErrorContext(&ee)))
                    }
                }
            }
        })
    }

    pub fn download_all(&self, to: &Path) -> Result<(), anyhow::Error> {
        block(async {
            let objects = self.list_objects().await?;

            for key in objects {
                let relative = key.strip_prefix(&self.path).unwrap_or(&key).trim_start_matches('/');
                let local_path = to.join(relative);

                if let Some(parent) = local_path.parent() {
                    std::fs::create_dir_all(parent).with_context(|| format!("cannot create dir {}", parent.display()))?;
                }

                let response = self
                    .client
                    .get_object()
                    .bucket(&self.bucket)
                    .key(&key)
                    .send()
                    .await
                    .with_context(|| format!("cannot download {}", key))?;

                let bytes = response.body.collect().await.with_context(|| format!("cannot read body of {}", key))?.into_bytes();

                std::fs::write(&local_path, bytes).with_context(|| format!("cannot write {}", local_path.display()))?;
            }

            Ok(())
        })
    }

    pub fn upload_all(&self, from: &Path, skip: &HashSet<PathBuf>) -> Result<(), anyhow::Error> {
        block(async {
            let pattern = format!("{}/**/*", from.to_string_lossy());
            let entries = glob::glob(&pattern).with_context(|| format!("invalid glob pattern: {}", pattern))?;

            for entry in entries {
                let path = entry.context("glob error")?;
                if !path.is_file() {
                    continue;
                }
                if skip.contains(&path) {
                    continue;
                }

                let relative = path.strip_prefix(from).context("cannot strip prefix")?;
                let key = format!("{}/{}", self.path.trim_end_matches('/'), relative.to_string_lossy());

                let body = ByteStream::from_path(&path).await.with_context(|| format!("cannot read {}", path.display()))?;

                self.client
                    .put_object()
                    .bucket(&self.bucket)
                    .key(&key)
                    .body(body)
                    .send()
                    .await
                    .with_context(|| format!("cannot upload {}", key))?;
            }

            Ok(())
        })
    }

    pub fn delete_deleted_files(&self, from: &Path) -> Result<(), anyhow::Error> {
        block(async {
            let objects = self.list_objects().await?;

            for key in objects {
                let relative = key.strip_prefix(&self.path).unwrap_or(&key).trim_start_matches('/');
                let local_path = from.join(relative);

                if !local_path.exists() {
                    self.client
                        .delete_object()
                        .bucket(&self.bucket)
                        .key(&key)
                        .send()
                        .await
                        .with_context(|| format!("cannot delete {}", key))?;
                }
            }

            Ok(())
        })
    }

    pub fn download_file(&self, key: &str) -> Result<Vec<u8>, anyhow::Error> {
        block(async {
            let full_key = format!("{}/{}", self.path.trim_end_matches('/'), key.trim_start_matches('/'));

            let response = self
                .client
                .get_object()
                .bucket(&self.bucket)
                .key(&full_key)
                .send()
                .await
                .with_context(|| format!("cannot download {}", full_key))?;

            let bytes = response.body.collect().await.with_context(|| format!("cannot read body of {}", full_key))?.into_bytes().to_vec();
            Ok(bytes)
        })
    }

    pub fn upload_file(&self, key: &str, data: Vec<u8>, content_type: Option<&str>) -> Result<(), anyhow::Error> {
        block(async {
            let full_key = format!("{}/{}", self.path.trim_end_matches('/'), key.trim_start_matches('/'));

            let body = ByteStream::from(data);
            let mut req = self.client.put_object().bucket(&self.bucket).key(&full_key).body(body);
            if let Some(ct) = content_type {
                req = req.content_type(ct);
            }
            req.send().await.with_context(|| format!("cannot upload {}", full_key))?;
            Ok(())
        })
    }

    pub fn download_file_with_etag(&self, key: &str) -> Result<(Vec<u8>, Option<String>), anyhow::Error> {
        block(async {
            let full_key = format!("{}/{}", self.path.trim_end_matches('/'), key.trim_start_matches('/'));

            let response = match self.client.get_object().bucket(&self.bucket).key(&full_key).send().await {
                Ok(r) => r,
                Err(e) => {
                    if matches!(http_status_from_get_err(&e), Some(404)) {
                        return Ok((vec![], None));
                    }
                    let svc = e.into_service_error();
                    if svc.is_no_such_key() {
                        return Ok((vec![], None));
                    }
                    return Err(anyhow::anyhow!("cannot download {}: {}", full_key, svc));
                }
            };

            let etag = response.e_tag().map(|s| s.to_string());
            let bytes = response.body.collect().await.with_context(|| format!("cannot read body of {}", full_key))?.into_bytes().to_vec();
            Ok((bytes, etag))
        })
    }

    pub fn upload_file_if_match(&self, key: &str, data: Vec<u8>, content_type: Option<&str>, etag: Option<String>) -> Result<(), UploadError> {
        block(async {
            let full_key = format!("{}/{}", self.path.trim_end_matches('/'), key.trim_start_matches('/'));

            let body = ByteStream::from(data);
            let mut req = self.client.put_object().bucket(&self.bucket).key(&full_key).body(body);
            if let Some(ct) = content_type {
                req = req.content_type(ct);
            }
            req = match &etag {
                Some(e) => req.if_match(e.clone()),
                None => req.if_none_match("*"),
            };

            match req.send().await {
                Ok(_) => Ok(()),
                Err(e) => {
                    if matches!(http_status_from_put_err(&e), Some(412)) {
                        Err(UploadError::PreconditionFailed)
                    } else {
                        Err(UploadError::Other(anyhow::anyhow!("cannot upload {}: {}", full_key, DisplayErrorContext(&e))))
                    }
                }
            }
        })
    }

    async fn list_objects(&self) -> Result<Vec<String>, anyhow::Error> {
        let mut keys = vec![];
        let mut continuation_token: Option<String> = None;

        loop {
            let mut req = self.client.list_objects_v2().bucket(&self.bucket).prefix(&self.path);

            if let Some(token) = continuation_token {
                req = req.continuation_token(token);
            }

            let response = req.send().await.context("cannot list objects")?;

            for obj in response.contents() {
                if let Some(key) = obj.key() {
                    keys.push(key.to_string());
                }
            }

            if response.is_truncated().unwrap_or(false) {
                continuation_token = response.next_continuation_token().map(|s| s.to_string());
            } else {
                break;
            }
        }

        Ok(keys)
    }
}

fn http_status_from_put_err(e: &SdkError<aws_sdk_s3::operation::put_object::PutObjectError>) -> Option<u16> {
    if let SdkError::ServiceError(svc) = e {
        return Some(svc.raw().status().as_u16());
    }
    None
}

fn http_status_from_get_err(e: &SdkError<aws_sdk_s3::operation::get_object::GetObjectError>) -> Option<u16> {
    if let SdkError::ServiceError(svc) = e {
        return Some(svc.raw().status().as_u16());
    }
    None
}

#[cfg(test)]
mod tests {
    use super::*;

    fn s3_config(access_key_id: Option<&str>, secret_access_key: Option<&str>, session_token: Option<&str>) -> S3Config {
        S3Config {
            bucket: "bucket".to_string(),
            path_in_bucket: None,
            bucket_public_url: None,
            endpoint: "https://s3.example.com".to_string(),
            access_key_id: access_key_id.map(str::to_string),
            secret_access_key: secret_access_key.map(str::to_string),
            session_token: session_token.map(str::to_string),
            region: Some("us-east-1".to_string()),
            force_path_style: false,
            cloudflare_zone_id: None,
            cloudflare_api_token: None,
        }
    }

    #[test]
    fn omitted_credentials_select_default_chain() {
        assert_eq!(explicit_credentials(&s3_config(None, None, None)).unwrap(), None);
        assert_eq!(explicit_credentials(&s3_config(Some(""), Some(""), Some(""))).unwrap(), None);
        assert_eq!(explicit_credentials(&s3_config(Some("  "), Some("\t"), Some("\n"))).unwrap(), None);
    }

    #[test]
    fn explicit_credentials_accept_optional_session_token() {
        assert_eq!(
            explicit_credentials(&s3_config(Some("access"), Some("secret"), Some("token"))).unwrap(),
            Some(ExplicitCredentials {
                access_key_id: "access".to_string(),
                secret_access_key: "secret".to_string(),
                session_token: Some("token".to_string()),
            })
        );
    }

    #[test]
    fn partial_explicit_credentials_are_rejected() {
        assert!(explicit_credentials(&s3_config(Some("access"), None, None)).is_err());
        assert!(explicit_credentials(&s3_config(None, Some("secret"), None)).is_err());
        assert!(explicit_credentials(&s3_config(None, None, Some("token"))).is_err());
    }
}
