use crate::distros::Distros;
use std::collections::HashMap;

pub mod badge;
pub mod html;
pub mod json;
pub mod sh;

pub type Repository = HashMap<String, String>;
pub type Repositories = Vec<Repository>;

pub(crate) fn upsert_from(existing: Repositories, entries: &Repositories) -> Repositories {
    let mut repos = existing;
    for entry in entries {
        upsert_repository(&mut repos, entry.clone());
    }
    sorted_by_distro_order(&repos)
}

fn upsert_repository(repositories: &mut Repositories, data: Repository) {
    let distro_id = data.get("distro_id").unwrap();

    if let Some(repo) = repositories.iter_mut().find(|repo| repo.get("distro_id").is_some_and(|value| value == distro_id)) {
        repo.extend(data);
    } else {
        repositories.push(data);
    }
}

fn sorted_by_distro_order(repositories: &Repositories) -> Repositories {
    let ids = Distros::get().ids();
    let mut sorted = repositories.clone();
    sorted.sort_by_key(|repo| repo.get("distro_id").and_then(|id| ids.iter().position(|d| d == id)).unwrap_or(usize::MAX));
    sorted
}

#[cfg(test)]
mod tests {
    use super::*;

    fn entry(id: &str, url: &str) -> Repository {
        Repository::from([("distro_id".to_string(), id.to_string()), ("download_url".to_string(), url.to_string())])
    }

    #[test]
    fn test_upsert_from_blank_adds_entries() {
        let merged = upsert_from(Vec::new(), &vec![entry("ubuntu_24.04", "u"), entry("fedora_42", "f")]);
        assert_eq!(merged.len(), 2);
    }

    #[test]
    fn test_upsert_from_merges_into_existing() {
        let existing = upsert_from(Vec::new(), &vec![entry("fedora_42", "f")]);
        let merged = upsert_from(existing, &vec![entry("ubuntu_24.04", "u")]);
        let ids: Vec<&str> = merged.iter().map(|r| r["distro_id"].as_str()).collect();
        assert_eq!(merged.len(), 2);
        assert!(ids.contains(&"fedora_42") && ids.contains(&"ubuntu_24.04"));
    }

    #[test]
    fn test_upsert_from_updates_same_distro_in_place() {
        let existing = upsert_from(Vec::new(), &vec![entry("arch", "old")]);
        let merged = upsert_from(existing, &vec![entry("arch", "new")]);
        assert_eq!(merged.len(), 1, "re-upserting the same distro must not duplicate");
        assert_eq!(merged[0]["download_url"], "new");
    }
}
