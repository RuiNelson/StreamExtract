//! "Memory": the server settings the user chose to keep, in a plain-text INI file at
//! `<home>/.config/streamextract/memory.ini` (on every OS). The archive password is never stored.
//!
//! Everything here takes the file path as an argument so it can be tested without touching
//! the real home directory. Format (contract section B):
//!
//! ```ini
//! # StreamExtract memory: plain text, written by Memory Save
//! [server]
//! host=ftp.example.com
//! port=21
//! protocol=ftp
//! mode=passive
//! user=alice
//! password=s3cret;#=x
//! directory=/uploads
//! mkdir=false
//!
//! [memory]
//! store_credentials=yes
//! ```
//!
//! Values are literal after the first `=` (no trimming, no quoting, no inline comments), keys are
//! trimmed, lines starting with `#` or `;` are comments and unknown keys are ignored.

use std::fs;
use std::io;
use std::path::{Path, PathBuf};

use serde::Serialize;

use crate::{Mode, Protocol, ServerSettings};

/// Error returned when a non-anonymous login is saved before the user answered the consent question.
pub const CONSENT_REQUIRED: &str = "credentials consent required";

/// `<home>/.config/streamextract/memory.ini`
pub fn memory_path(home: &Path) -> PathBuf {
    home.join(".config")
        .join("streamextract")
        .join("memory.ini")
}

/// Result of `memory_status`.
#[derive(Debug, Clone, PartialEq, Eq, Serialize)]
pub struct Status {
    /// The memory file exists.
    pub saved: bool,
    /// The stored answer to "store the login unencrypted?"; `None` = never asked.
    pub store_credentials: Option<bool>,
}

/// What the file says, field by field (`None` = key absent).
#[derive(Debug, Default)]
struct Parsed {
    has_server_section: bool,
    host: Option<String>,
    port: Option<String>,
    mode: Option<String>,
    protocol: Option<String>,
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

/// `None` when the file does not exist.
fn read(path: &Path) -> Result<Option<Parsed>, String> {
    Ok(crate::ini::read(path)?.map(|text| parse(&text)))
}

/// Whether a memory file exists and the stored consent answer.
pub fn status(path: &Path) -> Result<Status, String> {
    let parsed = read(path)?;
    Ok(Status {
        saved: parsed.is_some(),
        store_credentials: parsed
            .and_then(|p| p.store_credentials)
            .and_then(|value| parse_bool(&value)),
    })
}

/// The saved settings, or `None` when nothing is saved. `user`/`password` are `None` when the file has
/// no such key (the user chose not to store the login).
pub fn recall(path: &Path) -> Result<Option<ServerSettings>, String> {
    let Some(parsed) = read(path)? else {
        return Ok(None);
    };
    if !parsed.has_server_section {
        return Ok(None);
    }
    let protocol = match parsed
        .protocol
        .as_deref()
        .map(|value| value.trim().to_ascii_lowercase())
        .as_deref()
    {
        Some("ftps_explicit") => Protocol::FtpsExplicit,
        Some("ftps_implicit") => Protocol::FtpsImplicit,
        _ => Protocol::Ftp,
    };
    Ok(Some(ServerSettings {
        host: parsed.host.unwrap_or_default(),
        port: parsed
            .port
            .and_then(|value| value.trim().parse().ok())
            .filter(|port| (1..=65535).contains(port))
            .unwrap_or(protocol.default_port()),
        protocol,
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
    }))
}

/// Saves `server`.
///
/// * `store_credentials` (when not `None`) is persisted as the user's answer; otherwise the stored
///   answer is used.
/// * A non-anonymous login (non-empty `user`) without any answer fails with [`CONSENT_REQUIRED`].
/// * Answer yes: user and password are written. Answer no: neither is written.
/// * Anonymous login: `user=` is written empty, no password, and the answer is left untouched.
pub fn save(
    path: &Path,
    server: &ServerSettings,
    store_credentials: Option<bool>,
) -> Result<(), String> {
    let stored_answer = read(path)?
        .and_then(|parsed| parsed.store_credentials)
        .and_then(|value| parse_bool(&value));
    let user = server.user.as_deref().unwrap_or("");
    let anonymous = user.is_empty();

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

    if !(1..=65535).contains(&server.port) {
        return Err("The port must be between 1 and 65535".to_string());
    }
    let mut text = String::new();
    text.push_str("# StreamExtract memory: plain text, written by Memory Save\n[server]\n");
    push_value(&mut text, "host", &server.host)?;
    text.push_str(&format!(
        "port={}\nprotocol={}\nmode={}\n",
        server.port,
        server.protocol.as_str(),
        server.mode.as_str()
    ));
    if anonymous {
        text.push_str("user=\n");
    } else if store_login {
        push_value(&mut text, "user", user)?;
        push_value(
            &mut text,
            "password",
            server.password.as_deref().unwrap_or(""),
        )?;
    }
    push_value(&mut text, "directory", &server.directory)?;
    text.push_str(&format!("mkdir={}\n", server.mkdir));
    if let Some(answer) = answer {
        text.push_str(&format!(
            "\n[memory]\nstore_credentials={}\n",
            if answer { "yes" } else { "no" }
        ));
    }
    crate::ini::write_atomic(path, &text)
}

/// Deletes the memory file (and with it the stored answer). Fine if it does not exist.
pub fn clear(path: &Path) -> Result<(), String> {
    match fs::remove_file(path) {
        Ok(()) => Ok(()),
        Err(error) if error.kind() == io::ErrorKind::NotFound => Ok(()),
        Err(error) => Err(format!("Cannot delete {}: {error}", path.display())),
    }
}

fn push_value(text: &mut String, key: &str, value: &str) -> Result<(), String> {
    if value.contains(['\r', '\n']) {
        return Err(format!("The {key} must not contain line breaks"));
    }
    text.push_str(key);
    text.push('=');
    text.push_str(value);
    text.push('\n');
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::process;
    use std::sync::atomic::{AtomicU32, Ordering};
    use std::time::{SystemTime, UNIX_EPOCH};

    /// A unique directory under the system temp dir, removed on drop.
    struct TempHome(PathBuf);

    impl TempHome {
        fn new() -> TempHome {
            static COUNTER: AtomicU32 = AtomicU32::new(0);
            let nanos = SystemTime::now()
                .duration_since(UNIX_EPOCH)
                .unwrap()
                .as_nanos();
            let dir = std::env::temp_dir().join(format!(
                "streamextract-test-{}-{}-{nanos}",
                process::id(),
                COUNTER.fetch_add(1, Ordering::Relaxed)
            ));
            fs::create_dir_all(&dir).unwrap();
            TempHome(dir)
        }

        fn memory(&self) -> PathBuf {
            memory_path(&self.0)
        }
    }

    impl Drop for TempHome {
        fn drop(&mut self) {
            let _ = fs::remove_dir_all(&self.0);
        }
    }

    fn server(user: Option<&str>, password: Option<&str>) -> ServerSettings {
        ServerSettings {
            host: "ftp.example.com".to_string(),
            port: 2121,
            protocol: crate::Protocol::Ftp,
            mode: Mode::Active,
            user: user.map(str::to_string),
            password: password.map(str::to_string),
            directory: "/up loads/dir;#=x".to_string(),
            mkdir: true,
        }
    }

    #[test]
    fn protocols_round_trip_and_old_memory_defaults_to_ftp() {
        let home = TempHome::new();
        let path = home.memory();
        for protocol in [
            Protocol::Ftp,
            Protocol::FtpsExplicit,
            Protocol::FtpsImplicit,
        ] {
            let mut settings = server(None, None);
            settings.protocol = protocol;
            settings.port = protocol.default_port();
            save(&path, &settings, None).unwrap();
            let recalled = recall(&path).unwrap().unwrap();
            assert_eq!(recalled.protocol, protocol);
            assert_eq!(recalled.port, settings.port);
        }
        fs::write(&path, "[server]\nhost=old.example.com\nport=2121\n").unwrap();
        let recalled = recall(&path).unwrap().unwrap();
        assert_eq!(recalled.protocol, Protocol::Ftp);
        assert_eq!(recalled.port, 2121);
        fs::write(&path, "[server]\nprotocol=ftps_implicit\n").unwrap();
        assert_eq!(recall(&path).unwrap().unwrap().port, 990);
    }

    #[test]
    fn path_is_under_dot_config_of_home() {
        let path = memory_path(Path::new("/Users/x"));
        assert_eq!(path, Path::new("/Users/x/.config/streamextract/memory.ini"));
    }

    #[test]
    fn nothing_saved() {
        let home = TempHome::new();
        let path = home.memory();
        assert_eq!(
            status(&path).unwrap(),
            Status {
                saved: false,
                store_credentials: None
            }
        );
        assert_eq!(recall(&path).unwrap(), None);
        clear(&path).unwrap(); // fine without a file
    }

    #[test]
    fn round_trip_with_special_characters() {
        let home = TempHome::new();
        let path = home.memory();
        let settings = server(Some(" al;ice#=1 "), Some("  s3cret;#= x=y  "));
        save(&path, &settings, Some(true)).unwrap();

        assert_eq!(recall(&path).unwrap(), Some(settings));
        assert_eq!(
            status(&path).unwrap(),
            Status {
                saved: true,
                store_credentials: Some(true)
            }
        );

        let text = fs::read_to_string(&path).unwrap();
        assert!(text.contains("\npassword=  s3cret;#= x=y  \n"), "{text}");
        assert!(text.contains("\nuser= al;ice#=1 \n"), "{text}");
        assert!(text.contains("\nstore_credentials=yes\n"), "{text}");
    }

    #[test]
    fn empty_password_and_missing_password_are_stored_empty() {
        let home = TempHome::new();
        let path = home.memory();
        save(&path, &server(Some("alice"), None), Some(true)).unwrap();
        let recalled = recall(&path).unwrap().unwrap();
        assert_eq!(recalled.user.as_deref(), Some("alice"));
        assert_eq!(recalled.password.as_deref(), Some(""));
    }

    #[test]
    fn consent_is_required_for_a_login() {
        let home = TempHome::new();
        let path = home.memory();
        let error = save(&path, &server(Some("alice"), Some("pw")), None).unwrap_err();
        assert_eq!(error, "credentials consent required");
        assert!(!path.exists());
    }

    #[test]
    fn answer_no_stores_neither_user_nor_password() {
        let home = TempHome::new();
        let path = home.memory();
        save(&path, &server(Some("alice"), Some("pw")), Some(false)).unwrap();

        let text = fs::read_to_string(&path).unwrap();
        assert!(!text.contains("alice") && !text.contains("pw"), "{text}");
        assert!(
            !text.contains("user=") && !text.contains("password="),
            "{text}"
        );
        assert!(text.contains("store_credentials=no"), "{text}");

        let recalled = recall(&path).unwrap().unwrap();
        assert_eq!((recalled.user, recalled.password), (None, None));
        assert_eq!(recalled.host, "ftp.example.com");
        assert_eq!(recalled.port, 2121);
        assert_eq!(recalled.mode, Mode::Active);
        assert_eq!(recalled.directory, "/up loads/dir;#=x");
        assert!(recalled.mkdir);
        assert_eq!(status(&path).unwrap().store_credentials, Some(false));
    }

    #[test]
    fn the_stored_answer_is_used_when_none_is_given() {
        let home = TempHome::new();
        let path = home.memory();
        save(&path, &server(Some("alice"), Some("pw")), Some(false)).unwrap();
        // The answer is still "no": nothing is asked again and nothing is stored.
        save(&path, &server(Some("bob"), Some("pw2")), None).unwrap();
        assert_eq!(recall(&path).unwrap().unwrap().user, None);

        // A new explicit answer replaces the stored one.
        save(&path, &server(Some("bob"), Some("pw2")), Some(true)).unwrap();
        let recalled = recall(&path).unwrap().unwrap();
        assert_eq!(
            (recalled.user.as_deref(), recalled.password.as_deref()),
            (Some("bob"), Some("pw2"))
        );

        // And it is used from then on.
        save(&path, &server(Some("carol"), Some("pw3")), None).unwrap();
        let recalled = recall(&path).unwrap().unwrap();
        assert_eq!(
            (recalled.user.as_deref(), recalled.password.as_deref()),
            (Some("carol"), Some("pw3"))
        );
        assert_eq!(status(&path).unwrap().store_credentials, Some(true));
    }

    #[test]
    fn anonymous_writes_an_empty_user_and_leaves_the_answer_untouched() {
        let home = TempHome::new();
        let path = home.memory();

        // No answer needed, none recorded, no password written.
        save(&path, &server(Some(""), Some("ignored")), None).unwrap();
        let text = fs::read_to_string(&path).unwrap();
        assert!(text.contains("\nuser=\n"), "{text}");
        assert!(
            !text.contains("password=") && !text.contains("ignored"),
            "{text}"
        );
        assert!(!text.contains("[memory]"), "{text}");
        let recalled = recall(&path).unwrap().unwrap();
        assert_eq!(
            (recalled.user.as_deref(), recalled.password),
            (Some(""), None)
        );
        assert_eq!(status(&path).unwrap().store_credentials, None);

        // `user` null is anonymous as well.
        save(&path, &server(None, None), None).unwrap();
        assert_eq!(recall(&path).unwrap().unwrap().user.as_deref(), Some(""));

        // An existing answer survives an anonymous save, even if another one is passed.
        save(&path, &server(Some("alice"), Some("pw")), Some(true)).unwrap();
        save(&path, &server(Some(""), None), Some(false)).unwrap();
        assert_eq!(status(&path).unwrap().store_credentials, Some(true));
    }

    #[test]
    fn clear_forgets_the_consent() {
        let home = TempHome::new();
        let path = home.memory();
        save(&path, &server(Some("alice"), Some("pw")), Some(true)).unwrap();
        assert!(path.exists());

        clear(&path).unwrap();
        assert!(!path.exists());
        assert_eq!(
            status(&path).unwrap(),
            Status {
                saved: false,
                store_credentials: None
            }
        );
        assert_eq!(recall(&path).unwrap(), None);
        assert_eq!(
            save(&path, &server(Some("alice"), Some("pw")), None).unwrap_err(),
            CONSENT_REQUIRED
        );
        clear(&path).unwrap(); // twice is fine
    }

    #[test]
    fn values_with_line_breaks_are_rejected() {
        let home = TempHome::new();
        let path = home.memory();
        assert!(save(&path, &server(Some("alice"), Some("a\nb")), Some(true)).is_err());
        assert!(save(&path, &server(Some("ali\rce"), Some("pw")), Some(true)).is_err());
        let mut bad_host = server(Some("alice"), Some("pw"));
        bad_host.host = "ftp\r\nevil=1".to_string();
        assert!(save(&path, &bad_host, Some(true)).is_err());
        let mut bad_directory = server(None, None);
        bad_directory.directory = "/a\n[memory]\nstore_credentials=yes".to_string();
        assert!(save(&path, &bad_directory, None).is_err());
        assert!(!path.exists());
    }

    #[test]
    fn invalid_port_is_rejected() {
        let home = TempHome::new();
        let path = home.memory();
        let mut settings = server(None, None);
        settings.port = 0;
        assert!(save(&path, &settings, None).is_err());
        settings.port = 70000;
        assert!(save(&path, &settings, None).is_err());
    }

    #[test]
    fn parser_follows_the_format_rules() {
        let home = TempHome::new();
        let path = home.memory();
        fs::create_dir_all(path.parent().unwrap()).unwrap();
        let text = "# comment\r\n; another\r\n\r\nhost=ignored (before any section)\r\n[Other]\r\nhost=nope\r\n\
                    [ server ]\r\n  host = example.org \r\nport=2222\r\nmode=ACTIVE\r\nuser=a=b\r\n\
                    password= x ;y #z \r\nunknown=1\r\nno equals sign\r\n#directory=/commented\r\nmkdir=yes\r\n\
                    [memory]\r\nhost=nope\r\nstore_credentials= No \r\n";
        fs::write(&path, text).unwrap();

        let recalled = recall(&path).unwrap().unwrap();
        assert_eq!(recalled.host, " example.org "); // values are literal, only keys are trimmed
        assert_eq!(recalled.port, 2222);
        assert_eq!(recalled.mode, Mode::Active);
        assert_eq!(recalled.user.as_deref(), Some("a=b")); // literal after the first '='
        assert_eq!(recalled.password.as_deref(), Some(" x ;y #z "));
        assert_eq!(recalled.directory, "");
        assert!(recalled.mkdir);
        assert_eq!(status(&path).unwrap().store_credentials, Some(false));
    }

    #[test]
    fn defaults_for_missing_or_invalid_fields() {
        let home = TempHome::new();
        let path = home.memory();
        fs::create_dir_all(path.parent().unwrap()).unwrap();
        fs::write(&path, "[server]\nport=abc\nmode=weird\nmkdir=maybe\n").unwrap();
        let recalled = recall(&path).unwrap().unwrap();
        assert_eq!(recalled.host, "");
        assert_eq!(recalled.port, 21);
        assert_eq!(recalled.mode, Mode::Passive);
        assert!(!recalled.mkdir);
        assert_eq!((recalled.user, recalled.password), (None, None));

        // A file without a [server] section holds no settings.
        fs::write(&path, "[memory]\nstore_credentials=yes\n").unwrap();
        assert_eq!(recall(&path).unwrap(), None);
        assert_eq!(
            status(&path).unwrap(),
            Status {
                saved: true,
                store_credentials: Some(true)
            }
        );
    }

    #[test]
    fn saving_replaces_the_previous_content_and_leaves_no_temporary_files() {
        let home = TempHome::new();
        let path = home.memory();
        save(&path, &server(Some("alice"), Some("pw")), Some(true)).unwrap();
        let mut other = server(Some("bob"), Some("pw2"));
        other.host = "other.example.com".to_string();
        save(&path, &other, None).unwrap();
        assert_eq!(recall(&path).unwrap(), Some(other));

        let names: Vec<_> = fs::read_dir(path.parent().unwrap())
            .unwrap()
            .map(|entry| entry.unwrap().file_name())
            .collect();
        assert_eq!(names, vec![std::ffi::OsString::from("memory.ini")]);
    }

    #[cfg(unix)]
    #[test]
    fn file_and_directory_are_private_on_unix() {
        use std::os::unix::fs::PermissionsExt;

        let home = TempHome::new();
        let path = home.memory();
        save(&path, &server(Some("alice"), Some("pw")), Some(true)).unwrap();
        let mode = |p: &Path| fs::metadata(p).unwrap().permissions().mode() & 0o777;
        assert_eq!(mode(&path), 0o600);
        assert_eq!(mode(path.parent().unwrap()), 0o700);

        // Stays private when overwritten.
        save(&path, &server(Some("alice"), Some("pw2")), None).unwrap();
        assert_eq!(mode(&path), 0o600);
    }
}
