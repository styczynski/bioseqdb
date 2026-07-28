use crate::logger::Logger;
use aws_sdk_s3::Client;
use aws_sdk_s3::error::{DisplayErrorContext, SdkError};
use aws_sdk_s3::primitives::ByteStream;
use std::time::{SystemTime, UNIX_EPOCH};

const PROBE_PREFIX: &str = ".omnipackage-precondition-probe-";
const ORPHAN_AGE_SECS: u64 = 3600;
const SWEEP_PROBABILITY_DENOM: u64 = 10;

pub(super) async fn detect(client: &Client, bucket: &str, path: &str) -> bool {
    if should_sweep() {
        sweep_orphans(client, bucket, path).await;
    }

    let probe_key = full_key(path, &probe_object_name());

    if let Err(e) = client.put_object().bucket(bucket).key(&probe_key).body(ByteStream::from_static(b"probe")).send().await {
        Logger::new().warn(format!("If-Match probe step 1 failed: {}; assuming unsupported", DisplayErrorContext(&e)));
        return false;
    }

    let put_with_wrong_if_match = client
        .put_object()
        .bucket(bucket)
        .key(&probe_key)
        .body(ByteStream::from_static(b"probe2"))
        .if_match("\"00000000-omnipackage-deliberately-wrong\"")
        .send()
        .await;

    let supported = match &put_with_wrong_if_match {
        Ok(_) => false,
        Err(e) => classify_probe_status(http_status_from_put_err(e)),
    };

    if let Err(e) = client.delete_object().bucket(bucket).key(&probe_key).send().await {
        Logger::new().warn(format!("If-Match probe cleanup of {} failed: {}", probe_key, DisplayErrorContext(&e)));
    }

    supported
}

fn http_status_from_put_err(e: &SdkError<aws_sdk_s3::operation::put_object::PutObjectError>) -> Option<u16> {
    if let SdkError::ServiceError(svc) = e {
        return Some(svc.raw().status().as_u16());
    }
    None
}

async fn sweep_orphans(client: &Client, bucket: &str, path: &str) {
    let prefix = full_key(path, PROBE_PREFIX);

    let cutoff = match SystemTime::now().duration_since(UNIX_EPOCH) {
        Ok(d) => d.as_secs().saturating_sub(ORPHAN_AGE_SECS),
        Err(_) => return,
    };

    let mut continuation_token: Option<String> = None;
    loop {
        let mut req = client.list_objects_v2().bucket(bucket).prefix(&prefix);
        if let Some(token) = continuation_token.take() {
            req = req.continuation_token(token);
        }

        let response = match req.send().await {
            Ok(r) => r,
            Err(e) => {
                Logger::new().warn(format!("If-Match probe orphan sweep list failed: {}", DisplayErrorContext(&e)));
                return;
            }
        };

        for obj in response.contents() {
            let Some(key) = obj.key() else { continue };
            let last_modified_secs = obj.last_modified().and_then(|t| u64::try_from(t.secs()).ok()).unwrap_or(0);
            if last_modified_secs == 0 || last_modified_secs > cutoff {
                continue;
            }
            if let Err(e) = client.delete_object().bucket(bucket).key(key).send().await {
                Logger::new().warn(format!("If-Match probe orphan sweep delete of {} failed: {}", key, DisplayErrorContext(&e)));
            }
        }

        if response.is_truncated().unwrap_or(false) {
            continuation_token = response.next_continuation_token().map(|s| s.to_string());
            if continuation_token.is_none() {
                return;
            }
        } else {
            return;
        }
    }
}

fn probe_object_name() -> String {
    let nanos = SystemTime::now().duration_since(UNIX_EPOCH).map(|d| d.subsec_nanos()).unwrap_or(0);
    let pid = std::process::id();
    format!("{}{}-{:x}", PROBE_PREFIX, pid, nanos)
}

fn full_key(path: &str, name: &str) -> String {
    let path = path.trim_end_matches('/');
    if path.is_empty() { name.to_string() } else { format!("{}/{}", path, name) }
}

fn should_sweep() -> bool {
    let nanos = SystemTime::now().duration_since(UNIX_EPOCH).map(|d| u64::from(d.subsec_nanos())).unwrap_or(0);
    nanos.is_multiple_of(SWEEP_PROBABILITY_DENOM)
}

fn classify_probe_status(status: Option<u16>) -> bool {
    matches!(status, Some(412) | Some(404))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_classify_412_supported() {
        assert!(classify_probe_status(Some(412)));
    }

    #[test]
    fn test_classify_404_supported() {
        assert!(classify_probe_status(Some(404)));
    }

    #[test]
    fn test_classify_200_unsupported() {
        assert!(!classify_probe_status(Some(200)));
    }

    #[test]
    fn test_classify_500_unsupported() {
        assert!(!classify_probe_status(Some(500)));
    }

    #[test]
    fn test_classify_403_unsupported() {
        assert!(!classify_probe_status(Some(403)));
    }

    #[test]
    fn test_classify_none_unsupported() {
        assert!(!classify_probe_status(None));
    }

    #[test]
    fn test_probe_object_name_has_prefix() {
        let name = probe_object_name();
        assert!(name.starts_with(PROBE_PREFIX));
    }

    #[test]
    fn test_probe_object_name_contains_pid() {
        let name = probe_object_name();
        let pid = std::process::id().to_string();
        assert!(name.contains(&pid));
    }

    #[test]
    fn test_full_key_with_path() {
        assert_eq!(full_key("repo", "name"), "repo/name");
        assert_eq!(full_key("repo/", "name"), "repo/name");
    }

    #[test]
    fn test_full_key_empty_path() {
        assert_eq!(full_key("", "name"), "name");
    }
}
