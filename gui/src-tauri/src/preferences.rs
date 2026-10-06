//! GUI preferences, saved automatically apart from server memory.
use std::path::Path;

use serde::{Deserialize, Serialize};

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
    let Some(text) = crate::ini::read(path)? else {
        return Ok(Preferences::default());
    };
    let ini = crate::ini::Ini::parse(&text);
    let preferences = Preferences {
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
    };
    Ok(preferences)
}

pub fn save(path: &Path, preferences: Preferences) -> Result<(), String> {
    if !(1..=4096).contains(&preferences.buffer_mib) {
        return Err("Buffer must be a number between 1 and 4096 MiB.".to_string());
    }
    if preferences.retries == 0 {
        return Err("Upload attempts must be at least 1.".to_string());
    }
    let units = match preferences.units {
        Units::Si => "si",
        Units::Binary => "binary",
    };
    let updates = preferences
        .check_updates
        .map_or_else(String::new, |enabled| {
            format!("\n[updates]\ncheck_on_launch={enabled}\n")
        });
    crate::ini::write_atomic(
        path,
        &format!(
            "[display]\nunits={units}\nshow_passwords={}\n\n[transfer]\nbuffer_mib={}\nretries={}\nprevent_sleep={}\n\n[notifications]\ncompletion_sound={}\n{updates}",
            preferences.show_passwords, preferences.buffer_mib, preferences.retries, preferences.prevent_sleep, preferences.completion_sound
        ),
    )
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::fs;
    use std::time::{SystemTime, UNIX_EPOCH};

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
        let path = dir.join("preferences.ini");
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
        let memory = dir.join("memory.ini");
        fs::write(&memory, "server settings").unwrap();
        save(&path, Preferences::default()).unwrap();
        assert_eq!(load(&path).unwrap().units, Units::Si);
        assert!(load(&path).unwrap().completion_sound);
        assert_eq!(fs::read_to_string(&memory).unwrap(), "server settings");
        crate::memory::clear(&memory).unwrap();
        assert_eq!(load(&path).unwrap().units, Units::Si);
        fs::write(
            &path,
            "\u{feff}; comment\n[other]\nunits=binary\n[display]\nunits=invalid\n",
        )
        .unwrap();
        assert_eq!(load(&path).unwrap(), Preferences::default());
        fs::write(&path, "[ DISPLAY ]\nunits = BINARY\nunknown=yes\n").unwrap();
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
            fs::write(&path, format!("[updates]\ncheck_on_launch = {value}\n")).unwrap();
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
            fs::write(&path, format!("[display]\nshow_passwords = {value}\n")).unwrap();
            assert_eq!(load(&path).unwrap().show_passwords, enabled);
        }
        for (value, enabled) in [("FALSE", false), ("true", true), ("invalid", true)] {
            fs::write(
                &path,
                format!("[notifications]\ncompletion_sound = {value}\n"),
            )
            .unwrap();
            assert_eq!(load(&path).unwrap().completion_sound, enabled);
        }
        fs::write(&path, "[ DISPLAY ]\nunits = SI\nunknown=yes\n").unwrap();
        assert_eq!(load(&path).unwrap().units, Units::Si);
        for value in ["0", "4097", "-1", "invalid"] {
            fs::write(&path, format!("[transfer]\nbuffer_mib={value}\n")).unwrap();
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
            fs::write(&path, format!("[transfer]\nretries={value}\n")).unwrap();
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
            fs::write(&path, format!("[transfer]\nprevent_sleep={value}\n")).unwrap();
            assert_eq!(load(&path).unwrap().prevent_sleep, enabled);
        }
        fs::remove_file(&path).unwrap();
        fs::create_dir(&path).unwrap();
        assert!(load(&path).is_err());
        assert!(save(&path, Preferences::default()).is_err());
        fs::remove_dir_all(dir).unwrap();
    }
}
