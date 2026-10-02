//! GUI preferences, saved automatically apart from server memory.
use std::path::Path;

use serde::{Deserialize, Serialize};

#[derive(Debug, Default, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum Units {
    Si,
    #[default]
    Binary,
}

const DEFAULT_BUFFER_MIB: u32 = 64;

fn default_buffer_mib() -> u32 {
    DEFAULT_BUFFER_MIB
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
pub struct Preferences {
    pub units: Units,
    #[serde(default = "default_buffer_mib")]
    pub buffer_mib: u32,
}

impl Default for Preferences {
    fn default() -> Self {
        Self {
            units: Units::Binary,
            buffer_mib: DEFAULT_BUFFER_MIB,
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
            Some(value) if value.eq_ignore_ascii_case("si") => Units::Si,
            _ => Units::Binary,
        },
        buffer_mib: ini
            .get("transfer", "buffer_mib")
            .and_then(|value| value.trim().parse::<u32>().ok())
            .filter(|value| (1..=4096).contains(value))
            .unwrap_or(DEFAULT_BUFFER_MIB),
    };
    Ok(preferences)
}

pub fn save(path: &Path, preferences: Preferences) -> Result<(), String> {
    if !(1..=4096).contains(&preferences.buffer_mib) {
        return Err("Buffer must be a number between 1 and 4096 MiB.".to_string());
    }
    let units = match preferences.units {
        Units::Si => "si",
        Units::Binary => "binary",
    };
    crate::ini::write_atomic(
        path,
        &format!(
            "[display]\nunits={units}\n\n[transfer]\nbuffer_mib={}\n",
            preferences.buffer_mib
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
            "rarftp-preferences-{}-{}",
            std::process::id(),
            SystemTime::now()
                .duration_since(UNIX_EPOCH)
                .unwrap()
                .as_nanos()
        ));
        let path = dir.join("preferences.ini");
        assert_eq!(load(&path).unwrap(), Preferences::default());
        save(
            &path,
            Preferences {
                units: Units::Si,
                buffer_mib: 128,
            },
        )
        .unwrap();
        assert_eq!(load(&path).unwrap().buffer_mib, 128);
        assert_eq!(load(&path).unwrap().units, Units::Si);
        let memory = dir.join("memory.ini");
        fs::write(&memory, "server settings").unwrap();
        save(&path, Preferences::default()).unwrap();
        assert_eq!(load(&path).unwrap().units, Units::Binary);
        assert_eq!(fs::read_to_string(&memory).unwrap(), "server settings");
        crate::memory::clear(&memory).unwrap();
        assert_eq!(load(&path).unwrap().units, Units::Binary);
        fs::write(
            &path,
            "\u{feff}; comment\n[other]\nunits=si\n[display]\nunits=invalid\n",
        )
        .unwrap();
        assert_eq!(load(&path).unwrap(), Preferences::default());
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
                },
            )
            .unwrap();
            assert_eq!(load(&path).unwrap().buffer_mib, value);
        }
        for value in [0, 4097] {
            assert!(save(
                &path,
                Preferences {
                    units: Units::Binary,
                    buffer_mib: value
                }
            )
            .is_err());
            assert_eq!(load(&path).unwrap().buffer_mib, 4096);
        }
        fs::remove_file(&path).unwrap();
        fs::create_dir(&path).unwrap();
        assert!(load(&path).is_err());
        assert!(save(&path, Preferences::default()).is_err());
        fs::remove_dir_all(dir).unwrap();
    }
}
