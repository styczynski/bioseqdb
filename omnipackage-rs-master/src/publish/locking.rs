use crate::logger::Logger;
use crate::publish::s3::{S3, UploadError};
use anyhow::Result;
use std::time::{Duration, SystemTime, UNIX_EPOCH};

const MAX_RETRY_ATTEMPTS: u32 = 10;

pub type DerivedFile = (&'static str, &'static str, Vec<u8>);

pub fn commit_anchored<F>(s3: &S3, anchor_key: &str, anchor_content_type: &str, mut build: F) -> Result<()>
where
    F: FnMut(&[u8]) -> Result<(Vec<u8>, Vec<DerivedFile>)>,
{
    if !s3.supports_if_match() {
        Logger::new().warn(format!(
            "S3 endpoint does not honor If-Match on PUT; {} and its derived files may race under parallel publishes (last writer wins)",
            anchor_key
        ));
        let existing = s3.download_file(anchor_key).unwrap_or_default();
        let (anchor_body, derived) = build(&existing)?;
        write_derived(s3, &derived)?;
        s3.upload_file(anchor_key, anchor_body, Some(anchor_content_type))?;
        return Ok(());
    }

    for attempt in 0..MAX_RETRY_ATTEMPTS {
        let (existing, etag) = s3.download_file_with_etag(anchor_key)?;
        let (anchor_body, derived) = build(&existing)?;
        // derived files before the anchor CAS: the winner of the last anchor commit is then also the last writer of every derived file
        write_derived(s3, &derived)?;
        match s3.upload_file_if_match(anchor_key, anchor_body, Some(anchor_content_type), etag) {
            Ok(()) => return Ok(()),
            Err(UploadError::PreconditionFailed) => {
                std::thread::sleep(retry_backoff(attempt));
                continue;
            }
            Err(UploadError::Other(e)) => return Err(e),
        }
    }

    Err(anyhow::anyhow!(
        "failed to update {} after {} attempts due to repeated precondition failures",
        anchor_key,
        MAX_RETRY_ATTEMPTS
    ))
}

fn write_derived(s3: &S3, derived: &[DerivedFile]) -> Result<()> {
    for (key, content_type, body) in derived {
        s3.upload_file(key, body.clone(), Some(content_type))?;
    }
    Ok(())
}

fn retry_backoff(attempt: u32) -> Duration {
    let base_ms: u64 = 100u64 << attempt.min(10);
    let jitter_ms = SystemTime::now().duration_since(UNIX_EPOCH).map(|d| u64::from(d.subsec_nanos()) % 50).unwrap_or(0);
    Duration::from_millis(base_ms + jitter_ms)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_retry_backoff_grows_with_attempt() {
        let a0 = retry_backoff(0);
        let a3 = retry_backoff(3);
        assert!(a3 >= a0);
        assert!(a0.as_millis() >= 100 && a0.as_millis() < 200);
        assert!(a3.as_millis() >= 800 && a3.as_millis() < 900);
    }

    #[test]
    fn test_retry_backoff_clamps_high_attempts() {
        let a = retry_backoff(50);
        assert!(a.as_millis() <= 200_000);
    }
}
