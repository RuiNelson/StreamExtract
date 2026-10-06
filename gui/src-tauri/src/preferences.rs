//! GUI preferences, saved automatically as YAML apart from server memory.
//! Legacy INI settings are migrated before the old file is deleted.
use std::fs;
use std::io;
use std::path::Path;
use std::sync::{Mutex, PoisonError};

use serde::{Deserialize, Serialize};

// Startup, preference commands and release checks can all trigger migration.
static FILE_LOCK: Mutex<()> = Mutex::new(());

#[derive(Debug, Default, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum Units {
    #[default]
    Si,
    Binary,
}

const DEFAULT_BUFFER_MIB: u32 = 64;

fn default_buffer_mib() -> u32 {
    DEFAULT_BUFFER_MIB
}

pub(crate) fn default_retries() -> u32 {
    3
}

pub(crate) fn default_prevent_sleep() -> bool {
    true
}

fn default_completion_sound() -> bool {
    true
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(default)]
pub struct Preferences {
    pub units: Units,
    #[serde(default = "default_buffer_mib")]
    pub buffer_mib: u32,
    #[serde(default = "default_retries")]
    pub retries: u32,
    #[serde(default = "default_prevent_sleep")]
    pub prevent_sleep: bool,
    #[serde(default = "default_completion_sound")]
    pub completion_sound: bool,
    #[serde(default)]
    pub show_passwords: bool,
    /// None until the user answers the first-launch question.
    #[serde(default)]
    pub check_updates: Option<bool>,
}

impl Default for Preferences {
    fn default() -> Self {
        Self {
            units: Units::Si,
            buffer_mib: DEFAULT_BUFFER_MIB,
            retries: default_retries(),
            prevent_sleep: default_prevent_sleep(),
            completion_sound: true,
            show_passwords: false,
            check_updates: None,
        }
    }
}

pub fn load(path: &Path) -> Result<Preferences, String> {
    let _guard = FILE_LOCK.lock().unwrap_or_else(PoisonError::into_inner);
    migrate_locked(path)?;
    Ok(read_yaml(path)?.unwrap_or_default())
}

fn read_yaml(path: &Path) -> Result<Option<Preferences>, String> {
    let text = match fs::read_to_string(path) {
        Ok(text) => text,
        Err(error) if error.kind() == io::ErrorKind::NotFound => return Ok(None),
        Err(error) => return Err(format!("Cannot read {}: {error}", path.display())),
    };
    let preferences = serde_yaml_ng::from_str(&text)
        .map_err(|error| format!("Cannot read {}: {error}", path.display()))?;
    validate(preferences).map_err(|error| format!("Cannot read {}: {error}", path.display()))?;
    Ok(Some(preferences))
}

pub fn save(path: &Path, preferences: Preferences) -> Result<(), String> {
    validate(preferences)?;
    let _guard = FILE_LOCK.lock().unwrap_or_else(PoisonError::into_inner);
    migrate_locked(path)?;
    write_yaml(path, preferences)
}

fn write_yaml(path: &Path, preferences: Preferences) -> Result<(), String> {
    let text = serde_yaml_ng::to_string(&preferences)
        .map_err(|error| format!("Cannot serialize preferences: {error}"))?;
    crate::ini::write_atomic(path, &text)
}

/// Called at startup and retried by preference reads/saves if it previously failed.
pub fn migrate(path: &Path) -> Result<(), String> {
    let _guard = FILE_LOCK.lock().unwrap_or_else(PoisonError::into_inner);
    migrate_locked(path)
}

fn migrate_locked(path: &Path) -> Result<(), String> {
    let legacy = path.with_file_name("preferences.ini");
    let Some(text) = crate::ini::read(&legacy)? else {
        return Ok(());
    };
    // A valid YAML file takes precedence, including after a failed INI deletion.
    // Validate it before deleting the old file so malformed YAML preserves both.
    if read_yaml(path)?.is_none() {
        write_yaml(path, parse_legacy(&text))?;
    }
    fs::remove_file(&legacy).map_err(|error| format!("Cannot delete {}: {error}", legacy.display()))
}

fn parse_legacy(text: &str) -> Preferences {
    let ini = crate::ini::Ini::parse(text);
    Preferences {
        units: match ini.get("display", "units").map(str::trim) {
            Some(value) if value.eq_ignore_ascii_case("binary") => Units::Binary,
            _ => Units::Si,
        },
        buffer_mib: ini
            .get("transfer", "buffer_mib")
            .and_then(|value| value.trim().parse::<u32>().ok())
            .filter(|value| (1..=4096).contains(value))
            .unwrap_or(DEFAULT_BUFFER_MIB),
        retries: ini
            .get("transfer", "retries")
            .and_then(|value| value.trim().parse::<u32>().ok())
            .filter(|value| *value > 0)
            .unwrap_or_else(default_retries),
        prevent_sleep: !ini
            .get("transfer", "prevent_sleep")
            .is_some_and(|value| value.trim().eq_ignore_ascii_case("false")),
        show_passwords: ini
            .get("display", "show_passwords")
            .is_some_and(|value| value.trim().eq_ignore_ascii_case("true")),
        completion_sound: !ini
            .get("notifications", "completion_sound")
            .is_some_and(|value| value.trim().eq_ignore_ascii_case("false")),
        check_updates: ini.get("updates", "check_on_launch").and_then(|value| {
            match value.trim().to_ascii_lowercase().as_str() {
                "true" => Some(true),
                "false" => Some(false),
                _ => None,
            }
        }),
    }
}

fn validate(preferences: Preferences) -> Result<(), String> {
    if !(1..=4096).contains(&preferences.buffer_mib) {
        return Err("Buffer must be a number between 1 and 4096 MiB.".to_string());
    }
    if preferences.retries == 0 {
        return Err("Upload attempts must be at least 1.".to_string());
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::fs;
    use std::time::{SystemTime, UNIX_EPOCH};

    fn write_legacy(path: &Path, text: impl AsRef<[u8]>) -> io::Result<()> {
        match fs::remove_file(path) {
            Ok(()) => {}
            Err(error) if error.kind() == io::ErrorKind::NotFound => {}
            Err(error) => return Err(error),
        }
        fs::write(path.with_file_name("preferences.ini"), text)
    }

    #[test]
    fn preferences_persist_separately_from_memory() {
        let dir = std::env::temp_dir().join(format!(
            "streamextract-preferences-{}-{}",
            std::process::id(),
            SystemTime::now()
                .duration_since(UNIX_EPOCH)
                .unwrap()
                .as_nanos()
        ));
        let path = dir.join("preferences.yaml");
        assert_eq!(load(&path).unwrap(), Preferences::default());
        assert_eq!(Preferences::default().units, Units::Si);
        save(
            &path,
            Preferences {
                units: Units::Binary,
                buffer_mib: 128,
                retries: 5,
                prevent_sleep: false,
                completion_sound: false,
                show_passwords: true,
                check_updates: Some(false),
            },
        )
        .unwrap();
        assert_eq!(load(&path).unwrap().buffer_mib, 128);
        assert_eq!(load(&path).unwrap().retries, 5);
        assert!(!load(&path).unwrap().prevent_sleep);
        assert_eq!(load(&path).unwrap().units, Units::Binary);
        assert!(!load(&path).unwrap().completion_sound);
        assert!(load(&path).unwrap().show_passwords);
        assert_eq!(load(&path).unwrap().check_updates, Some(false));
        let memory = dir.join("memory.yaml");
        fs::write(&memory, "server settings").unwrap();
        save(&path, Preferences::default()).unwrap();
        assert_eq!(load(&path).unwrap().units, Units::Si);
        assert!(load(&path).unwrap().completion_sound);
        assert_eq!(fs::read_to_string(&memory).unwrap(), "server settings");
        fs::remove_file(&memory).unwrap();
        assert_eq!(load(&path).unwrap().units, Units::Si);
        write_legacy(
            &path,
            "\u{feff}; comment\n[other]\nunits=binary\n[display]\nunits=invalid\n",
        )
        .unwrap();
        assert_eq!(load(&path).unwrap(), Preferences::default());
        write_legacy(&path, "[ DISPLAY ]\nunits = BINARY\nunknown=yes\n").unwrap();
        assert_eq!(load(&path).unwrap().units, Units::Binary);
        assert!(load(&path).unwrap().completion_sound);
        let legacy: Preferences =
            serde_json::from_str(r#"{"units":"si","buffer_mib":64}"#).unwrap();
        assert!(legacy.completion_sound);
        assert_eq!(legacy.retries, 3);
        assert!(legacy.prevent_sleep);
        assert!(!legacy.show_passwords);
        assert_eq!(legacy.check_updates, None);
        for (value, expected) in [
            ("TRUE", Some(true)),
            ("false", Some(false)),
            ("invalid", None),
        ] {
            write_legacy(&path, format!("[updates]\ncheck_on_launch = {value}\n")).unwrap();
            assert_eq!(load(&path).unwrap().check_updates, expected);
        }
        save(
            &path,
            Preferences {
                check_updates: Some(true),
                ..Preferences::default()
            },
        )
        .unwrap();
        assert_eq!(load(&path).unwrap().check_updates, Some(true));
        for (value, enabled) in [("TRUE", true), ("false", false), ("invalid", false)] {
            write_legacy(&path, format!("[display]\nshow_passwords = {value}\n")).unwrap();
            assert_eq!(load(&path).unwrap().show_passwords, enabled);
        }
        for (value, enabled) in [("FALSE", false), ("true", true), ("invalid", true)] {
            write_legacy(
                &path,
                format!("[notifications]\ncompletion_sound = {value}\n"),
            )
            .unwrap();
            assert_eq!(load(&path).unwrap().completion_sound, enabled);
        }
        write_legacy(&path, "[ DISPLAY ]\nunits = SI\nunknown=yes\n").unwrap();
        assert_eq!(load(&path).unwrap().units, Units::Si);
        for value in ["0", "4097", "-1", "invalid"] {
            write_legacy(&path, format!("[transfer]\nbuffer_mib={value}\n")).unwrap();
            assert_eq!(load(&path).unwrap().buffer_mib, DEFAULT_BUFFER_MIB);
        }
        for value in [1, 4096] {
            save(
                &path,
                Preferences {
                    units: Units::Si,
                    buffer_mib: value,
                    ..Preferences::default()
                },
            )
            .unwrap();
            assert_eq!(load(&path).unwrap().buffer_mib, value);
        }
        for value in [0, 4097] {
            assert!(save(
                &path,
                Preferences {
                    units: Units::Si,
                    buffer_mib: value,
                    ..Preferences::default()
                }
            )
            .is_err());
            assert_eq!(load(&path).unwrap().buffer_mib, 4096);
        }
        for value in ["0", "-1", "1.5", "4294967296", "invalid"] {
            write_legacy(&path, format!("[transfer]\nretries={value}\n")).unwrap();
            assert_eq!(load(&path).unwrap().retries, 3);
        }
        for value in [1, 5, u32::MAX] {
            save(
                &path,
                Preferences {
                    retries: value,
                    ..Preferences::default()
                },
            )
            .unwrap();
            assert_eq!(load(&path).unwrap().retries, value);
        }
        assert!(save(
            &path,
            Preferences {
                retries: 0,
                ..Preferences::default()
            }
        )
        .is_err());
        assert_eq!(load(&path).unwrap().retries, u32::MAX);
        for (value, enabled) in [("FALSE", false), ("true", true), ("invalid", true)] {
            write_legacy(&path, format!("[transfer]\nprevent_sleep={value}\n")).unwrap();
            assert_eq!(load(&path).unwrap().prevent_sleep, enabled);
        }
        fs::remove_file(&path).unwrap();
        fs::create_dir(&path).unwrap();
        assert!(load(&path).is_err());
        assert!(save(&path, Preferences::default()).is_err());
        fs::remove_dir_all(dir).unwrap();
    }

    struct TempPreferences(std::path::PathBuf);

    impl TempPreferences {
        fn new() -> Self {
            static COUNTER: std::sync::atomic::AtomicU32 = std::sync::atomic::AtomicU32::new(0);
            let dir = std::env::temp_dir().join(format!(
                "streamextract-preferences-migration-{}-{}-{}",
                std::process::id(),
                SystemTime::now()
                    .duration_since(UNIX_EPOCH)
                    .unwrap()
                    .as_nanos(),
                COUNTER.fetch_add(1, std::sync::atomic::Ordering::Relaxed)
            ));
            fs::create_dir_all(&dir).unwrap();
            Self(dir)
        }
        fn yaml(&self) -> std::path::PathBuf {
            self.0.join("preferences.yaml")
        }
        fn ini(&self) -> std::path::PathBuf {
            self.0.join("preferences.ini")
        }
    }

    impl Drop for TempPreferences {
        fn drop(&mut self) {
            let _ = fs::remove_dir_all(&self.0);
        }
    }

    #[test]
    fn startup_migration_preserves_every_option_and_deletes_the_ini() {
        let fixture = TempPreferences::new();
        let text = "\u{feff}; comment\r\n[ DISPLAY ]\r\nunits = BINARY\r\nshow_passwords=TRUE\r\n[transfer]\r\nbuffer_mib=128\r\nretries=5\r\nprevent_sleep=FALSE\r\n[notifications]\r\ncompletion_sound=FALSE\r\n[updates]\r\ncheck_on_launch=false\r\n";
        fs::write(fixture.ini(), text).unwrap();
        migrate(&fixture.yaml()).unwrap();
        assert!(!fixture.ini().exists());
        let expected = Preferences {
            units: Units::Binary,
            buffer_mib: 128,
            retries: 5,
            prevent_sleep: false,
            completion_sound: false,
            show_passwords: true,
            check_updates: Some(false),
        };
        assert_eq!(load(&fixture.yaml()).unwrap(), expected);
        let yaml = fs::read_to_string(fixture.yaml()).unwrap();
        assert!(serde_yaml_ng::from_str::<serde_yaml_ng::Value>(&yaml)
            .unwrap()
            .is_mapping());
        assert!(yaml.contains("check_updates: false"));
        migrate(&fixture.yaml()).unwrap();
        assert_eq!(fs::read_to_string(fixture.yaml()).unwrap(), yaml);
    }

    #[test]
    fn existing_yaml_takes_precedence_and_a_deletion_retry_preserves_it() {
        let fixture = TempPreferences::new();
        let expected = Preferences {
            check_updates: Some(false),
            buffer_mib: 256,
            ..Preferences::default()
        };
        save(&fixture.yaml(), expected).unwrap();
        let yaml = fs::read(fixture.yaml()).unwrap();
        for _ in 0..2 {
            fs::write(
                fixture.ini(),
                "[updates]\ncheck_on_launch=true\n[transfer]\nbuffer_mib=128\n",
            )
            .unwrap();
            migrate(&fixture.yaml()).unwrap();
            assert!(!fixture.ini().exists());
            assert_eq!(fs::read(fixture.yaml()).unwrap(), yaml);
            assert_eq!(load(&fixture.yaml()).unwrap(), expected);
        }
    }

    #[test]
    fn failed_migration_keeps_both_files_and_can_be_retried() {
        let fixture = TempPreferences::new();
        let text = "[display]\nunits=binary\n[updates]\ncheck_on_launch=false\n";
        fs::write(fixture.ini(), text).unwrap();
        fs::write(fixture.yaml(), "broken: [yaml").unwrap();
        assert!(migrate(&fixture.yaml()).is_err());
        assert!(load(&fixture.yaml()).is_err());
        assert!(save(&fixture.yaml(), Preferences::default()).is_err());
        assert_eq!(fs::read_to_string(fixture.ini()).unwrap(), text);
        assert_eq!(fs::read_to_string(fixture.yaml()).unwrap(), "broken: [yaml");
        fs::remove_file(fixture.yaml()).unwrap();
        fs::create_dir(fixture.yaml()).unwrap();
        assert!(migrate(&fixture.yaml()).is_err());
        assert_eq!(fs::read_to_string(fixture.ini()).unwrap(), text);
        fs::remove_dir(fixture.yaml()).unwrap();
        migrate(&fixture.yaml()).unwrap();
        assert!(!fixture.ini().exists());
        assert_eq!(load(&fixture.yaml()).unwrap().units, Units::Binary);
        assert_eq!(load(&fixture.yaml()).unwrap().check_updates, Some(false));
    }

    #[test]
    fn concurrent_loads_migrate_once_and_keep_update_consent() {
        let fixture = TempPreferences::new();
        fs::write(
            fixture.ini(),
            "[display]\nunits=binary\n[updates]\ncheck_on_launch=false\n",
        )
        .unwrap();
        let barrier = std::sync::Arc::new(std::sync::Barrier::new(8));
        let threads: Vec<_> = (0..8)
            .map(|_| {
                let path = fixture.yaml();
                let barrier = barrier.clone();
                std::thread::spawn(move || {
                    barrier.wait();
                    load(&path).unwrap()
                })
            })
            .collect();
        for thread in threads {
            let preferences = thread.join().unwrap();
            assert_eq!(preferences.units, Units::Binary);
            assert_eq!(preferences.check_updates, Some(false));
        }
        assert!(!fixture.ini().exists());
    }

    #[test]
    fn yaml_defaults_and_invalid_values_are_handled_without_rewriting_files() {
        let fixture = TempPreferences::new();
        migrate(&fixture.yaml()).unwrap();
        assert_eq!(load(&fixture.yaml()).unwrap(), Preferences::default());
        assert!(!fixture.yaml().exists());
        fs::write(fixture.yaml(), "units: binary\n").unwrap();
        assert_eq!(
            load(&fixture.yaml()).unwrap(),
            Preferences {
                units: Units::Binary,
                ..Preferences::default()
            }
        );
        fs::write(fixture.yaml(), "{}\n").unwrap();
        assert_eq!(load(&fixture.yaml()).unwrap(), Preferences::default());
        for text in [
            "units: invalid\n",
            "buffer_mib: 0\n",
            "buffer_mib: 4097\n",
            "retries: 0\n",
            "check_updates: invalid\n",
        ] {
            fs::write(fixture.yaml(), text).unwrap();
            assert!(load(&fixture.yaml()).is_err());
            assert_eq!(fs::read_to_string(fixture.yaml()).unwrap(), text);
        }
    }

    #[cfg(unix)]
    #[test]
    fn migrated_yaml_and_subsequent_saves_are_private() {
        use std::os::unix::fs::PermissionsExt;
        let fixture = TempPreferences::new();
        fs::write(fixture.ini(), "[display]\nshow_passwords=true\n").unwrap();
        migrate(&fixture.yaml()).unwrap();
        let mode = || fs::metadata(fixture.yaml()).unwrap().permissions().mode() & 0o777;
        assert_eq!(mode(), 0o600);
        save(&fixture.yaml(), Preferences::default()).unwrap();
        assert_eq!(mode(), 0o600);
    }
}
