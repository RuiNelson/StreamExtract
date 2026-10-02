//! Small INI reader shared by server memory and preferences.
//! Section/key names are case-insensitive. Values remain literal after the first `=`:
//! no trimming, quoting, escaping or inline comments (passwords may contain these characters).
//! Duplicate keys use the last value; malformed lines are ignored.
use std::collections::BTreeMap;
use std::fs::{self, File, OpenOptions};
use std::io::{self, Write};
use std::path::Path;
use std::process;
use std::time::{SystemTime, UNIX_EPOCH};

#[derive(Default)]
pub struct Ini {
    sections: BTreeMap<String, BTreeMap<String, String>>,
}

impl Ini {
    pub fn parse(text: &str) -> Self {
        let mut ini = Self::default();
        let mut section = None;
        for line in text.strip_prefix('\u{feff}').unwrap_or(text).lines() {
            let trimmed = line.trim();
            if trimmed.is_empty() || trimmed.starts_with(['#', ';']) {
                continue;
            }
            if trimmed.starts_with('[') && trimmed.ends_with(']') {
                let name = trimmed[1..trimmed.len() - 1].trim().to_ascii_lowercase();
                ini.sections.entry(name.clone()).or_default();
                section = Some(name);
            } else if let (Some(section), Some((key, value))) = (&section, line.split_once('=')) {
                let key = key.trim().to_ascii_lowercase();
                if !key.is_empty() {
                    ini.sections
                        .get_mut(section)
                        .unwrap()
                        .insert(key, value.to_owned());
                }
            }
        }
        ini
    }

    pub fn has_section(&self, section: &str) -> bool {
        self.sections.contains_key(&section.to_ascii_lowercase())
    }

    pub fn get(&self, section: &str, key: &str) -> Option<&str> {
        self.sections
            .get(&section.to_ascii_lowercase())?
            .get(&key.to_ascii_lowercase())
            .map(String::as_str)
    }
}

/// Missing files have no settings. Preserve the memory reader's handling of non-UTF-8 bytes.
pub fn read(path: &Path) -> Result<Option<String>, String> {
    match fs::read(path) {
        Ok(bytes) => Ok(Some(String::from_utf8_lossy(&bytes).into_owned())),
        Err(error) if error.kind() == io::ErrorKind::NotFound => Ok(None),
        Err(error) => Err(format!("Cannot read {}: {error}", path.display())),
    }
}

/// Creates the private directory (0700 on Unix) and any missing parents.
fn create_private_dir(dir: &Path) -> io::Result<()> {
    if dir.is_dir() {
        return Ok(());
    }
    if let Some(parent) = dir.parent() {
        fs::create_dir_all(parent)?;
    }
    let mut builder = fs::DirBuilder::new();
    #[cfg(unix)]
    std::os::unix::fs::DirBuilderExt::mode(&mut builder, 0o700);
    match builder.create(dir) {
        Err(error) if error.kind() == io::ErrorKind::AlreadyExists => Ok(()),
        other => other,
    }
}

/// Creates a new file readable and writable by the owner only (0600 on Unix).
fn create_private_file(path: &Path) -> io::Result<File> {
    let mut options = OpenOptions::new();
    options.write(true).create_new(true);
    #[cfg(unix)]
    std::os::unix::fs::OpenOptionsExt::mode(&mut options, 0o600);
    options.open(path)
}

/// Writes `contents` to a temporary file in the same directory, then renames it over `path`.
pub(crate) fn write_atomic(path: &Path, contents: &str) -> Result<(), String> {
    let fail = |error: io::Error| format!("Cannot write {}: {error}", path.display());
    let dir = path
        .parent()
        .ok_or_else(|| format!("Invalid settings path {}", path.display()))?;
    create_private_dir(dir).map_err(fail)?;

    let nanos = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map_or(0, |elapsed| elapsed.as_nanos());
    let name = path.file_name().unwrap_or_default().to_string_lossy();
    let temp = dir.join(format!(".{name}.{}.{nanos}.tmp", process::id()));
    let result = (|| {
        let mut file = create_private_file(&temp)?;
        file.write_all(contents.as_bytes())?;
        file.sync_all()?;
        drop(file);
        fs::rename(&temp, path)
    })();
    if result.is_err() {
        let _ = fs::remove_file(&temp);
    }
    result.map_err(fail)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn sections_keys_and_literal_values() {
        let ini = Ini::parse("\u{feff}orphan=ignored\r\n ; comment\r\n[ SERVER ]\r\n Host =first\r\nhost=last\r\npassword=  secret;#=x  \r\nmalformed\r\n[empty]\r\n[server]\r\nuser=\r\n[other]\r\nhost=other\r\n");
        assert!(ini.has_section("EMPTY"));
        assert_eq!(ini.get("Server", "HOST"), Some("last"));
        assert_eq!(ini.get("server", "password"), Some("  secret;#=x  "));
        assert_eq!(ini.get("server", "user"), Some(""));
        assert_eq!(ini.get("other", "host"), Some("other"));
        assert_eq!(ini.get("server", "missing"), None);
        assert!(!ini.has_section("missing"));
    }
}
