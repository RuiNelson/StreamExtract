//! Server configurations stored as a YAML array in
//! `<home>/.config/streamextract/memory.yaml`. The archive password is never stored.
//! Legacy INI settings are migrated before the old file is deleted.

use std::collections::HashSet;
use std::fs;
use std::io;
use std::path::{Path, PathBuf};

use serde::{Deserialize, Serialize};
use uuid::Uuid;

use crate::{Mode, Protocol, ServerSettings};

pub const CONSENT_REQUIRED: &str = "credentials consent required";

pub fn memory_path(home: &Path) -> PathBuf {
    home.join(".config")
        .join("streamextract")
        .join("memory.yaml")
}

#[derive(Clone, PartialEq, Eq, Serialize, Deserialize)]
struct Config {
    id: Uuid,
    #[serde(flatten)]
    server: ServerSettings,
    /// Consent belongs to this slot, and is forgotten when the slot is deleted.
    store_credentials: Option<bool>,
}

/// Picker information deliberately excludes passwords and key passphrases.
#[derive(Serialize)]
pub struct Summary {
    id: Uuid,
    host: String,
    port: u32,
    protocol: Protocol,
    user: Option<String>,
    directory: String,
    store_credentials: Option<bool>,
}

#[derive(Serialize)]
pub struct Status {
    pub saved: bool,
    pub configs: Vec<Summary>,
}

fn read(path: &Path) -> Result<Vec<Config>, String> {
    let text = match fs::read_to_string(path) {
        Ok(text) => text,
        Err(error) if error.kind() == io::ErrorKind::NotFound => return Ok(Vec::new()),
        Err(error) => return Err(format!("Cannot read {}: {error}", path.display())),
    };
    let configs: Vec<Config> = serde_yaml_ng::from_str(&text)
        .map_err(|_| format!("Cannot read {}: invalid memory YAML", path.display()))?;
    let mut ids = HashSet::new();
    for config in &configs {
        if config.id.is_nil()
            || !ids.insert(config.id)
            || !(1..=65535).contains(&config.server.port)
        {
            return Err(format!(
                "Cannot read {}: invalid memory slot",
                path.display()
            ));
        }
    }
    Ok(configs)
}

fn write(path: &Path, configs: &[Config]) -> Result<(), String> {
    let text = serde_yaml_ng::to_string(configs)
        .map_err(|_| "Cannot serialize the saved server settings".to_string())?;
    crate::ini::write_atomic(path, &text)
}

pub fn status(path: &Path) -> Result<Status, String> {
    let configs = read(path)?
        .into_iter()
        .map(|config| Summary {
            id: config.id,
            host: config.server.host,
            port: config.server.port,
            protocol: config.server.protocol,
            user: config.server.user,
            directory: config.server.directory,
            store_credentials: config.store_credentials,
        })
        .collect::<Vec<_>>();
    Ok(Status {
        saved: !configs.is_empty(),
        configs,
    })
}

pub fn recall(path: &Path, id: Uuid) -> Result<ServerSettings, String> {
    read(path)?
        .into_iter()
        .find(|config| config.id == id)
        .map(|config| config.server)
        .ok_or_else(|| "The saved configuration no longer exists".to_string())
}

/// `id = None` creates a new slot; an existing ID replaces only that slot.
pub fn save(
    path: &Path,
    id: Option<Uuid>,
    server: &ServerSettings,
    store_credentials: Option<bool>,
) -> Result<(), String> {
    let mut configs = read(path)?;
    let index = match id {
        Some(id) => Some(
            configs
                .iter()
                .position(|config| config.id == id)
                .ok_or_else(|| "The saved configuration no longer exists".to_string())?,
        ),
        None => None,
    };
    if !(1..=65535).contains(&server.port) {
        return Err("The port must be between 1 and 65535".into());
    }
    let stored_answer = index.and_then(|index| configs[index].store_credentials);
    let user = server.user.as_deref().unwrap_or("");
    let anonymous = user.is_empty()
        && (server.protocol != Protocol::Sftp
            || (server.password.as_deref().unwrap_or("").is_empty()
                && server
                    .private_key_passphrase
                    .as_deref()
                    .unwrap_or("")
                    .is_empty()));
    let answer = if anonymous {
        stored_answer
    } else {
        store_credentials.or(stored_answer)
    };
    let store_login = if anonymous {
        false
    } else {
        answer.ok_or_else(|| CONSENT_REQUIRED.to_string())?
    };
    let mut saved = server.clone();
    saved.user = if anonymous {
        Some(String::new())
    } else if store_login {
        Some(user.to_string())
    } else {
        None
    };
    saved.password = if store_login {
        Some(server.password.clone().unwrap_or_default())
    } else {
        None
    };
    if !store_login {
        saved.private_key_passphrase = None;
    }
    let config = Config {
        id: match id {
            Some(id) => id,
            None => Uuid::new_v4(),
        },
        server: saved,
        store_credentials: answer,
    };
    if let Some(index) = index {
        configs[index] = config;
    } else {
        configs.push(config);
    }
    write(path, &configs)
}

/// Remove just the selected slot, including its consent answer.
pub fn clear(path: &Path, id: Uuid) -> Result<(), String> {
    let mut configs = read(path)?;
    let index = configs
        .iter()
        .position(|config| config.id == id)
        .ok_or_else(|| "The saved configuration no longer exists".to_string())?;
    configs.remove(index);
    write(path, &configs)
}

/// Called at startup, and retried by Memory commands if migration previously failed.
/// Keep both files on failure; an identical imported slot makes deletion retries idempotent.
pub fn migrate(path: &Path) -> Result<(), String> {
    let legacy = path.with_file_name("memory.ini");
    let Some(text) = crate::ini::read(&legacy)? else {
        return Ok(());
    };
    let parsed = parse(&text);
    let consent = parsed.store_credentials.as_deref().and_then(parse_bool);
    let server = legacy_server(parsed)?;
    let mut configs = read(path)?;
    if !configs
        .iter()
        .any(|config| config.server == server && config.store_credentials == consent)
    {
        configs.push(Config {
            id: Uuid::new_v4(),
            server,
            store_credentials: consent,
        });
        write(path, &configs)?;
    }
    fs::remove_file(&legacy).map_err(|error| format!("Cannot delete {}: {error}", legacy.display()))
}

/// What the file says, field by field (`None` = key absent).
#[derive(Debug, Default)]
struct Parsed {
    has_server_section: bool,
    host: Option<String>,
    port: Option<String>,
    mode: Option<String>,
    protocol: Option<String>,
    private_key: Option<String>,
    private_key_passphrase: Option<String>,
    known_hosts: Option<String>,
    user: Option<String>,
    password: Option<String>,
    directory: Option<String>,
    mkdir: Option<String>,
    store_credentials: Option<String>,
}

fn parse(text: &str) -> Parsed {
    let ini = crate::ini::Ini::parse(text);
    let server = |key| ini.get("server", key).map(str::to_owned);
    Parsed {
        has_server_section: ini.has_section("server"),
        host: server("host"),
        port: server("port"),
        mode: server("mode"),
        protocol: server("protocol"),
        private_key: server("private_key"),
        private_key_passphrase: server("private_key_passphrase"),
        known_hosts: server("known_hosts"),
        user: server("user"),
        password: server("password"),
        directory: server("directory"),
        mkdir: server("mkdir"),
        store_credentials: ini.get("memory", "store_credentials").map(str::to_owned),
    }
}

fn parse_bool(value: &str) -> Option<bool> {
    match value.trim().to_ascii_lowercase().as_str() {
        "yes" | "true" | "1" | "on" => Some(true),
        "no" | "false" | "0" | "off" => Some(false),
        _ => None,
    }
}

fn legacy_server(parsed: Parsed) -> Result<ServerSettings, String> {
    if !parsed.has_server_section {
        return Err("The old memory file has no server settings".into());
    }
    let protocol = match parsed
        .protocol
        .as_deref()
        .map(|value| value.trim().to_ascii_lowercase())
        .as_deref()
    {
        Some("ftps_explicit") => Protocol::FtpsExplicit,
        Some("ftps_implicit") => Protocol::FtpsImplicit,
        Some("sftp") => Protocol::Sftp,
        _ => Protocol::Ftp,
    };
    Ok(ServerSettings {
        host: parsed.host.unwrap_or_default(),
        port: parsed
            .port
            .and_then(|value| value.trim().parse().ok())
            .filter(|port| (1..=65535).contains(port))
            .unwrap_or(protocol.default_port()),
        protocol,
        private_key: parsed.private_key,
        private_key_passphrase: parsed.private_key_passphrase,
        known_hosts: parsed.known_hosts,
        mode: match parsed
            .mode
            .as_deref()
            .map(|mode| mode.trim().to_ascii_lowercase())
            .as_deref()
        {
            Some("active") => Mode::Active,
            _ => Mode::Passive,
        },
        user: parsed.user,
        password: parsed.password,
        directory: parsed.directory.unwrap_or_default(),
        mkdir: parsed
            .mkdir
            .and_then(|value| parse_bool(&value))
            .unwrap_or(false),
    })
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::sync::atomic::{AtomicU32, Ordering};

    struct TempHome(PathBuf);
    impl TempHome {
        fn new() -> Self {
            static COUNTER: AtomicU32 = AtomicU32::new(0);
            let path = std::env::temp_dir().join(format!(
                "streamextract-memory-{}-{}-{}",
                std::process::id(),
                std::time::SystemTime::now()
                    .duration_since(std::time::UNIX_EPOCH)
                    .unwrap()
                    .as_nanos(),
                COUNTER.fetch_add(1, Ordering::Relaxed)
            ));
            fs::create_dir_all(&path).unwrap();
            Self(path)
        }
        fn memory(&self) -> PathBuf {
            memory_path(&self.0)
        }
        fn legacy(&self, text: &str) -> PathBuf {
            let path = self.memory().with_file_name("memory.ini");
            fs::create_dir_all(path.parent().unwrap()).unwrap();
            fs::write(&path, text).unwrap();
            path
        }
    }
    impl Drop for TempHome {
        fn drop(&mut self) {
            let _ = fs::remove_dir_all(&self.0);
        }
    }
    fn server(user: Option<&str>, password: Option<&str>) -> ServerSettings {
        ServerSettings {
            host: "ftp.example.com".into(),
            port: 2121,
            protocol: Protocol::Ftp,
            mode: Mode::Active,
            private_key: None,
            private_key_passphrase: None,
            known_hosts: None,
            user: user.map(str::to_string),
            password: password.map(str::to_string),
            directory: "/up loads/dir;#=x".into(),
            mkdir: true,
        }
    }

    #[test]
    fn memory_is_a_yaml_array_under_dot_config() {
        assert_eq!(
            memory_path(Path::new("/Users/x")),
            Path::new("/Users/x/.config/streamextract/memory.yaml")
        );
        let home = TempHome::new();
        let path = home.memory();
        assert!(!status(&path).unwrap().saved);
        assert!(status(&path).unwrap().configs.is_empty());
        migrate(&path).unwrap();
        assert!(!path.exists());
        save(&path, None, &server(None, None), None).unwrap();
        assert!(serde_yaml_ng::from_str::<serde_yaml_ng::Value>(
            &fs::read_to_string(path).unwrap()
        )
        .unwrap()
        .is_sequence());
    }

    fn slot(path: &Path, index: usize) -> Uuid {
        status(path).unwrap().configs[index].id
    }

    #[test]
    fn append_overwrite_and_delete_affect_only_the_selected_slot() {
        let home = TempHome::new();
        let path = home.memory();
        let first = server(Some("alice"), Some("secret"));
        let mut second = server(Some("bob"), Some("other secret"));
        second.host = "other.example.com".into();
        save(&path, None, &first, Some(true)).unwrap();
        save(&path, None, &second, Some(true)).unwrap();
        let one = slot(&path, 0);
        let two = slot(&path, 1);
        assert_ne!(one, two);
        assert_eq!(one.get_version_num(), 4);
        second.port = 2222;
        save(&path, Some(two), &second, None).unwrap();
        assert_eq!(slot(&path, 1), two);
        assert_eq!(recall(&path, one).unwrap(), first);
        assert_eq!(recall(&path, two).unwrap(), second);
        clear(&path, one).unwrap();
        assert_eq!(slot(&path, 0), two);
        assert_eq!(recall(&path, two).unwrap(), second);
        assert!(recall(&path, one).is_err());
        save(&path, None, &first, Some(true)).unwrap();
        let three = slot(&path, 1);
        assert_ne!(three, one);
        assert_ne!(three, two);
        clear(&path, two).unwrap();
        clear(&path, three).unwrap();
        assert!(!status(&path).unwrap().saved);
        assert!(status(&path).unwrap().configs.is_empty());
    }

    #[test]
    fn uuid_identity_survives_array_reordering_and_invalid_ids_are_rejected() {
        let home = TempHome::new();
        let path = home.memory();
        let mut second = server(None, None);
        second.host = "second.example".into();
        save(&path, None, &server(None, None), None).unwrap();
        save(&path, None, &second, None).unwrap();
        let one = slot(&path, 0);
        let two = slot(&path, 1);
        let mut configs = read(&path).unwrap();
        configs.reverse();
        write(&path, &configs).unwrap();
        assert_eq!(recall(&path, one).unwrap().host, "ftp.example.com");
        assert_eq!(recall(&path, two).unwrap().host, "second.example");
        let summary = serde_json::to_value(status(&path).unwrap()).unwrap();
        assert_eq!(summary["configs"][0]["id"], two.to_string());
        configs[0].id = one;
        write(&path, &configs).unwrap();
        assert!(status(&path).is_err());
        configs[0].id = Uuid::nil();
        write(&path, &configs).unwrap();
        assert!(status(&path).is_err());
    }

    #[test]
    fn credentials_consent_is_per_slot_and_forgotten_on_delete() {
        let home = TempHome::new();
        let path = home.memory();
        let settings = server(Some("alice"), Some("secret"));
        assert_eq!(
            save(&path, None, &settings, None).unwrap_err(),
            CONSENT_REQUIRED
        );
        assert!(!path.exists());
        save(&path, None, &settings, Some(false)).unwrap();
        save(&path, Some(slot(&path, 0)), &settings, None).unwrap();
        assert_eq!(recall(&path, slot(&path, 0)).unwrap().user, None);
        assert_eq!(recall(&path, slot(&path, 0)).unwrap().password, None);
        assert_eq!(
            status(&path).unwrap().configs[0].store_credentials,
            Some(false)
        );
        // An answer in one slot cannot authorize saving credentials in a new slot.
        assert_eq!(
            save(&path, None, &settings, None).unwrap_err(),
            CONSENT_REQUIRED
        );
        save(&path, None, &settings, Some(true)).unwrap();
        assert_eq!(recall(&path, slot(&path, 1)).unwrap(), settings);
        save(&path, Some(slot(&path, 0)), &settings, Some(true)).unwrap();
        assert_eq!(recall(&path, slot(&path, 0)).unwrap(), settings);
        clear(&path, slot(&path, 0)).unwrap();
        clear(&path, slot(&path, 0)).unwrap();
        assert_eq!(
            save(&path, None, &settings, None).unwrap_err(),
            CONSENT_REQUIRED
        );
    }

    #[test]
    fn yaml_round_trips_literal_strings_including_multiline_passwords() {
        let home = TempHome::new();
        let path = home.memory();
        let mut settings = server(Some(" al;ice#=1 "), Some("  secret;#= x:y\nnext line  "));
        settings.host = "yes".into();
        settings.directory = "[null] : # /日本語".into();
        save(&path, None, &settings, Some(true)).unwrap();
        assert_eq!(recall(&path, slot(&path, 0)).unwrap(), settings);
        let summary = serde_json::to_value(status(&path).unwrap()).unwrap();
        assert!(summary["configs"][0].get("password").is_none());
        assert!(summary["configs"][0]
            .get("private_key_passphrase")
            .is_none());
    }

    #[test]
    fn protocols_and_ssh_settings_round_trip_with_consent() {
        let home = TempHome::new();
        let path = home.memory();
        for protocol in [
            Protocol::Ftp,
            Protocol::FtpsExplicit,
            Protocol::FtpsImplicit,
            Protocol::Sftp,
        ] {
            let mut settings = server(Some(""), Some("ssh password"));
            settings.protocol = protocol;
            settings.port = protocol.default_port();
            settings.private_key = Some("/keys/custom".into());
            settings.known_hosts = Some("".into());
            settings.private_key_passphrase = Some("key passphrase".into());
            save(&path, None, &settings, Some(false)).unwrap();
            let id = status(&path).unwrap().configs.last().unwrap().id;
            let recalled = recall(&path, id).unwrap();
            assert_eq!(recalled.protocol, protocol);
            assert_eq!(recalled.port, settings.port);
            assert_eq!(recalled.private_key, settings.private_key);
            assert_eq!(recalled.known_hosts, Some("".into()));
            assert_eq!(recalled.private_key_passphrase, None);
            assert_eq!(recalled.password, None);
            if protocol == Protocol::Sftp {
                assert_eq!(
                    status(&path)
                        .unwrap()
                        .configs
                        .last()
                        .unwrap()
                        .store_credentials,
                    Some(false)
                );
                save(&path, Some(id), &settings, Some(true)).unwrap();
                assert_eq!(recall(&path, id).unwrap(), settings);
            }
        }
    }

    #[test]
    fn anonymous_login_preserves_consent_and_omits_password() {
        let home = TempHome::new();
        let path = home.memory();
        save(&path, None, &server(None, Some("ignored")), None).unwrap();
        assert_eq!(recall(&path, slot(&path, 0)).unwrap().user, Some("".into()));
        assert_eq!(recall(&path, slot(&path, 0)).unwrap().password, None);
        assert_eq!(status(&path).unwrap().configs[0].store_credentials, None);
        save(
            &path,
            Some(slot(&path, 0)),
            &server(Some("alice"), None),
            Some(true),
        )
        .unwrap();
        assert_eq!(
            recall(&path, slot(&path, 0)).unwrap().password,
            Some("".into())
        );
        save(
            &path,
            Some(slot(&path, 0)),
            &server(Some(""), None),
            Some(false),
        )
        .unwrap();
        assert_eq!(
            status(&path).unwrap().configs[0].store_credentials,
            Some(true)
        );
    }

    #[test]
    fn invalid_ports_and_unknown_slots_do_not_modify_memory() {
        let home = TempHome::new();
        let path = home.memory();
        save(&path, None, &server(None, None), None).unwrap();
        let original = fs::read(&path).unwrap();
        let mut settings = server(None, None);
        for port in [0, 70000] {
            settings.port = port;
            assert!(save(&path, None, &settings, None).is_err());
        }
        assert!(save(&path, Some(Uuid::new_v4()), &server(None, None), None).is_err());
        assert!(clear(&path, Uuid::new_v4()).is_err());
        assert_eq!(fs::read(path).unwrap(), original);
    }

    #[test]
    fn legacy_migration_preserves_literals_ssh_fields_and_consent() {
        let home = TempHome::new();
        let path = home.memory();
        let legacy = home.legacy("\u{feff}; comment\r\n[ SERVER ]\r\nhost= example.org \r\nport=2222\r\nprotocol=SFTP\r\nmode=ACTIVE\r\nuser=a=b\r\npassword= x ;y #z \r\nprivate_key=/keys/custom\r\nprivate_key_passphrase= secret \r\nknown_hosts=\r\ndirectory=/uploads\r\nmkdir=yes\r\n[memory]\r\nstore_credentials= Yes \r\n");
        migrate(&path).unwrap();
        assert!(!legacy.exists());
        let recalled = recall(&path, slot(&path, 0)).unwrap();
        assert_eq!(recalled.host, " example.org ");
        assert_eq!(recalled.port, 2222);
        assert_eq!(recalled.protocol, Protocol::Sftp);
        assert_eq!(recalled.mode, Mode::Active);
        assert_eq!(recalled.user.as_deref(), Some("a=b"));
        assert_eq!(recalled.password.as_deref(), Some(" x ;y #z "));
        assert_eq!(recalled.private_key.as_deref(), Some("/keys/custom"));
        assert_eq!(recalled.private_key_passphrase.as_deref(), Some(" secret "));
        assert_eq!(recalled.known_hosts, Some("".into()));
        assert_eq!(recalled.directory, "/uploads");
        assert!(recalled.mkdir);
        assert_eq!(
            status(&path).unwrap().configs[0].store_credentials,
            Some(true)
        );
        migrate(&path).unwrap();
        assert_eq!(status(&path).unwrap().configs.len(), 1);
    }

    #[test]
    fn legacy_defaults_and_missing_credentials_are_preserved() {
        let home = TempHome::new();
        let path = home.memory();
        home.legacy("[server]\nhost=old.example\nport=invalid\n[memory]\nstore_credentials=no\n");
        migrate(&path).unwrap();
        let recalled = recall(&path, slot(&path, 0)).unwrap();
        assert_eq!(recalled.protocol, Protocol::Ftp);
        assert_eq!(recalled.port, 21);
        assert_eq!(recalled.user, None);
        assert_eq!(recalled.password, None);
        assert_eq!(
            status(&path).unwrap().configs[0].store_credentials,
            Some(false)
        );
        home.legacy("[server]\nprotocol=ftps_implicit\n");
        migrate(&path).unwrap();
        assert_eq!(recall(&path, slot(&path, 1)).unwrap().port, 990);
    }

    #[test]
    fn migration_keeps_existing_slots_and_does_not_duplicate_a_retried_import() {
        let home = TempHome::new();
        let path = home.memory();
        save(&path, None, &server(None, None), None).unwrap();
        let text = "[server]\nhost=imported.example\nuser=\n";
        home.legacy(text);
        migrate(&path).unwrap();
        assert_eq!(status(&path).unwrap().configs.len(), 2);
        assert_eq!(
            recall(&path, slot(&path, 0)).unwrap().host,
            "ftp.example.com"
        );
        home.legacy(text);
        migrate(&path).unwrap();
        assert_eq!(status(&path).unwrap().configs.len(), 2);
    }

    #[test]
    fn failed_migration_preserves_the_ini_and_existing_yaml() {
        let home = TempHome::new();
        let path = home.memory();
        let legacy = home.legacy("[server]\nhost=imported.example\n");
        fs::write(&path, "broken: [yaml").unwrap();
        assert!(migrate(&path).is_err());
        assert!(legacy.exists());
        assert_eq!(fs::read_to_string(&path).unwrap(), "broken: [yaml");
        fs::remove_file(&path).unwrap();
        fs::create_dir(&path).unwrap();
        assert!(migrate(&path).is_err());
        assert!(legacy.exists());
        fs::remove_dir(&path).unwrap();
        fs::write(&legacy, "[other]\nvalue=1\n").unwrap();
        assert!(migrate(&path).is_err());
        assert!(legacy.exists());
        assert!(!path.exists());
    }

    #[test]
    fn malformed_yaml_is_never_overwritten_and_diagnostics_exclude_secrets() {
        let home = TempHome::new();
        let path = home.memory();
        fs::create_dir_all(path.parent().unwrap()).unwrap();
        for text in ["password: TOP_SECRET", "- id: TOP_SECRET"] {
            fs::write(&path, text).unwrap();
            let error = save(&path, None, &server(None, None), None).unwrap_err();
            assert!(!error.contains("TOP_SECRET"));
            assert_eq!(fs::read_to_string(&path).unwrap(), text);
        }
    }

    #[cfg(unix)]
    #[test]
    fn yaml_remains_private_after_migration_and_overwrite() {
        use std::os::unix::fs::PermissionsExt;
        let home = TempHome::new();
        let path = home.memory();
        home.legacy(
            "[server]\nhost=example\nuser=alice\npassword=pw\n[memory]\nstore_credentials=yes\n",
        );
        migrate(&path).unwrap();
        let mode = |p: &Path| fs::metadata(p).unwrap().permissions().mode() & 0o777;
        assert_eq!(mode(&path), 0o600);
        // The directory already existed for the legacy file; a fresh save creates it privately.
        let fresh = TempHome::new();
        let fresh_path = fresh.memory();
        save(&fresh_path, None, &server(None, None), None).unwrap();
        assert_eq!(mode(fresh_path.parent().unwrap()), 0o700);
        save(
            &path,
            Some(slot(&path, 0)),
            &server(Some("alice"), Some("pw2")),
            None,
        )
        .unwrap();
        assert_eq!(mode(&path), 0o600);
    }
}
