//! Public GitHub release lookup. Called on a blocking worker, independently of transfer jobs.
use github_release_check::{GitHub, GitHubReleaseItem};
use semver::Version;

const REPOSITORY: &str = "RuiNelson/StreamExtract";
pub const RELEASES_URL: &str = "https://github.com/RuiNelson/StreamExtract/releases";

fn newer_release(current: &Version, releases: &[GitHubReleaseItem]) -> Option<String> {
    releases
        .iter()
        .filter(|release| !release.draft && !release.prerelease)
        .filter_map(|release| {
            Version::parse(
                release
                    .tag_name
                    .strip_prefix('v')
                    .unwrap_or(&release.tag_name),
            )
            .ok()
        })
        .filter(|version| version.pre.is_empty() && version.cmp_precedence(current).is_gt())
        .max_by(Version::cmp_precedence)
        .map(|version| version.to_string())
}

pub fn check() -> Result<Option<String>, String> {
    let current = Version::parse(env!("CARGO_PKG_VERSION")).map_err(|error| error.to_string())?;
    let releases = GitHub::new()
        .and_then(|github| github.query(REPOSITORY))
        .map_err(|error| error.to_string())?;
    Ok(newer_release(&current, &releases))
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;

    fn release(tag: &str, draft: bool, prerelease: bool) -> GitHubReleaseItem {
        serde_json::from_value(json!({
            "url": "", "html_url": "", "assets_url": "", "upload_url": "", "id": 1,
            "tag_name": tag, "name": null, "draft": draft, "prerelease": prerelease,
            "created_at": "", "published_at": "", "body": null,
        }))
        .unwrap()
    }

    #[test]
    fn selects_newest_published_stable_semantic_version() {
        let current = Version::parse("2.4.1").unwrap();
        let releases = [
            release("v2.9.0", false, false),
            release("v99.0.0", true, false),
            release("v98.0.0", false, true),
            release("v97.0.0-beta.1", false, false),
            release("nightly", false, false),
            release("2.10.0", false, false),
            release("v2.4.1", false, false),
        ];
        assert_eq!(
            newer_release(&current, &releases).as_deref(),
            Some("2.10.0")
        );
    }

    #[test]
    fn no_prompt_for_empty_equal_older_or_build_only_releases() {
        let current = Version::parse("2.4.1").unwrap();
        assert_eq!(newer_release(&current, &[]), None);
        for tag in ["v2.4.1", "v2.4.0", "v1.99.0", "v2.4.1+newbuild"] {
            assert_eq!(newer_release(&current, &[release(tag, false, false)]), None);
        }
        let development = Version::parse("2.7.0-beta.1").unwrap();
        assert_eq!(
            newer_release(&development, &[release("v2.7.0", false, false)]).as_deref(),
            Some("2.7.0")
        );
    }
}
