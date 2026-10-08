//! Bindings to `libstreamextractcore` (`src/capi/streamextract.h`) and a safe wrapper around a transfer job.

use std::ffi::{c_char, c_int, c_uint, CStr, CString};
use std::marker::{PhantomData, PhantomPinned};
use std::ptr::{self, NonNull};

use serde_json::Value;

use crate::{Mode, Protocol, TransferConfig};

/// Opaque `streamextract_job`.
#[repr(C)]
pub struct StreamExtractJob {
    _data: [u8; 0],
    _marker: PhantomData<(*mut u8, PhantomPinned)>,
}

/// Mirror of `streamextract_job_config`; the field order and types must match the header exactly.
#[repr(C)]
pub struct StreamExtractJobConfig {
    pub archive: *const c_char,
    pub archive_password: *const c_char,
    pub host: *const c_char,
    pub port: c_int,
    pub active_mode: c_int,
    pub user: *const c_char,
    pub password: *const c_char,
    pub directory: *const c_char,
    pub mkdir: c_int,
    pub verbose: c_int,
    pub buffer_mib: c_uint,
}

#[repr(C)]
pub struct StreamExtractSshOptions {
    pub private_key: *const c_char,
    pub private_key_passphrase: *const c_char,
    pub known_hosts: *const c_char,
}

// The library is linked by build.rs.
extern "C" {
    pub fn streamextract_version() -> *const c_char;
    pub fn streamextract_job_start_with_staging(
        config: *const StreamExtractJobConfig,
        si_units: c_int,
        retries: c_uint,
        protocol: c_int,
        ca_certificate: *const c_char,
        ssh: *const StreamExtractSshOptions,
        extraction_root: *const c_char,
        staging: *const c_char,
    ) -> *mut StreamExtractJob;
    pub fn streamextract_archive_directories(
        archive: *const c_char,
        password: *const c_char,
    ) -> *mut c_char;
    pub fn streamextract_job_poll(job: *mut StreamExtractJob, log_cursor: u64) -> *mut c_char;
    pub fn streamextract_job_answer_password(job: *mut StreamExtractJob, password: *const c_char);
    pub fn streamextract_job_cancel(job: *mut StreamExtractJob);
    pub fn streamextract_job_free(job: *mut StreamExtractJob);
    pub fn streamextract_free(string: *mut c_char);
}

/// `"StreamExtract 2.6.0 (UnRAR 7.31, LZMA SDK 26.03, libarchive 3.8.9, libcurl 8.22.0)"`.
pub fn version() -> String {
    // SAFETY: returns a NUL-terminated string in static storage that is never freed.
    unsafe { CStr::from_ptr(streamextract_version()) }
        .to_string_lossy()
        .into_owned()
}

fn c_string(what: &str, value: &str) -> Result<CString, String> {
    CString::new(value).map_err(|_| format!("{what} must not contain NUL characters"))
}

// SAFETY: callers supply an owned library string (or NULL); always free it after parsing.
unsafe fn parse_library_json(text: *mut c_char) -> Result<Value, String> {
    if text.is_null() {
        return Err("The library returned no state".to_string());
    }
    let parsed = serde_json::from_str(&CStr::from_ptr(text).to_string_lossy());
    streamextract_free(text);
    parsed.map_err(|error| format!("The library returned invalid state: {error}"))
}

pub fn archive_directories(archive: &str, password: Option<&str>) -> Result<Value, String> {
    let archive = c_string("The archive path", archive)?;
    let password = password
        .map(|value| c_string("The archive password", value))
        .transpose()?;
    // SAFETY: the strings live through the synchronous call; the returned string is owned.
    unsafe {
        parse_library_json(streamextract_archive_directories(
            archive.as_ptr(),
            password
                .as_ref()
                .map_or(ptr::null(), |value| value.as_ptr()),
        ))
    }
}

/// A running (or finished) transfer. Dropping it cancels the job if needed, waits for it
/// (so the partial remote file is deleted) and frees it.
pub struct Job {
    raw: NonNull<StreamExtractJob>,
}

// SAFETY: the C API documents that every function is thread-safe for a given job, and the
// handle is only freed in `Drop`, which needs exclusive ownership.
unsafe impl Send for Job {}
unsafe impl Sync for Job {}

impl Job {
    /// Starts a job. The library copies the strings, so the `CString`s only live for this call.
    pub fn start(config: &TransferConfig) -> Result<Job, String> {
        let archive = c_string("The archive path", &config.archive)?;
        let extraction_root = c_string("The extraction root", &config.extraction_root)?;
        let archive_password = config
            .archive_password
            .as_deref()
            .map(|password| c_string("The archive password", password))
            .transpose()?;
        let host = c_string("The host", &config.host)?;
        let user = c_string("The user name", &config.user)?;
        let password = c_string("The password", &config.password)?;
        let private_key = config
            .private_key
            .as_deref()
            .map(|value| c_string("The private key path", value))
            .transpose()?;
        let passphrase = config
            .private_key_passphrase
            .as_deref()
            .map(|value| c_string("The private key passphrase", value))
            .transpose()?;
        let known_hosts = config
            .known_hosts
            .as_deref()
            .map(|value| c_string("The known hosts path", value))
            .transpose()?;
        let ssh = StreamExtractSshOptions {
            private_key: private_key
                .as_ref()
                .map_or(ptr::null(), |value| value.as_ptr()),
            private_key_passphrase: passphrase
                .as_ref()
                .map_or(ptr::null(), |value| value.as_ptr()),
            known_hosts: known_hosts
                .as_ref()
                .map_or(ptr::null(), |value| value.as_ptr()),
        };
        let directory = c_string("The directory", &config.directory)?;
        let staging = config
            .staging
            .as_deref()
            .map(str::trim)
            .filter(|value| !value.is_empty())
            .map(|value| c_string("The staging directory", value))
            .transpose()?;
        let port =
            c_int::try_from(config.port).map_err(|_| "The port is out of range".to_string())?;
        let buffer_mib = c_uint::try_from(config.buffer_mib)
            .map_err(|_| "The buffer size is out of range".to_string())?;
        if config.retries == 0 {
            return Err("Upload attempts must be at least 1.".to_string());
        }
        let retries = c_uint::try_from(config.retries)
            .map_err(|_| "The upload attempt count is out of range".to_string())?;

        let raw_config = StreamExtractJobConfig {
            archive: archive.as_ptr(),
            archive_password: archive_password
                .as_ref()
                .map_or(ptr::null(), |password| password.as_ptr()),
            host: host.as_ptr(),
            port,
            active_mode: c_int::from(config.mode == Mode::Active),
            user: user.as_ptr(),
            password: if config.protocol == Protocol::Sftp && config.password.is_empty() {
                ptr::null()
            } else {
                password.as_ptr()
            },
            directory: directory.as_ptr(),
            mkdir: c_int::from(config.mkdir),
            verbose: c_int::from(config.verbose),
            buffer_mib,
        };
        // SAFETY: `raw_config` and the strings it points to outlive the call; the library copies them.
        let job = unsafe {
            streamextract_job_start_with_staging(
                &raw_config,
                c_int::from(config.units == crate::preferences::Units::Si),
                retries,
                match config.protocol {
                    Protocol::Ftp => 0,
                    Protocol::FtpsExplicit => 1,
                    Protocol::FtpsImplicit => 2,
                    Protocol::Sftp => 3,
                },
                ptr::null(), // Use the system certificate trust store.
                &ssh,
                extraction_root.as_ptr(),
                staging.as_ref().map_or(ptr::null(), |value| value.as_ptr()),
            )
        };
        NonNull::new(job)
            .map(|raw| Job { raw })
            .ok_or_else(|| "The library could not start the transfer".to_string())
    }

    /// Current state (contract section A), with the log lines numbered `cursor` and later.
    pub fn poll(&self, cursor: u64) -> Result<Value, String> {
        // SAFETY: `self.raw` is a live job.
        let text = unsafe { streamextract_job_poll(self.raw.as_ptr(), cursor) };
        // SAFETY: `text` is an owned library string or NULL.
        unsafe { parse_library_json(text) }
    }

    /// Answers a pending archive password prompt; `None` declines it.
    pub fn answer_password(&self, password: Option<&str>) -> Result<(), String> {
        let password = password
            .map(|password| c_string("The archive password", password))
            .transpose()?;
        let password = password
            .as_ref()
            .map_or(ptr::null(), |password| password.as_ptr());
        // SAFETY: `self.raw` is live and `password` is NULL or valid for the call.
        unsafe { streamextract_job_answer_password(self.raw.as_ptr(), password) };
        Ok(())
    }

    /// Asks the job to stop; returns immediately.
    pub fn cancel(&self) {
        // SAFETY: `self.raw` is a live job.
        unsafe { streamextract_job_cancel(self.raw.as_ptr()) };
    }
}

impl Drop for Job {
    fn drop(&mut self) {
        // SAFETY: `self.raw` is live and never used again; this cancels, waits and frees.
        unsafe { streamextract_job_free(self.raw.as_ptr()) };
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn config() -> TransferConfig {
        TransferConfig {
            archive: "/nonexistent/streamextract-test.rar".to_string(),
            extraction_root: String::new(),
            archive_password: None,
            host: "127.0.0.1".to_string(),
            port: 21,
            protocol: crate::Protocol::Ftp,
            private_key: None,
            private_key_passphrase: None,
            known_hosts: None,
            mode: Mode::Passive,
            user: String::new(),
            password: String::new(),
            directory: String::new(),
            staging: None,
            mkdir: false,
            verbose: false,
            buffer_mib: 0,
            retries: 3,
            prevent_sleep: true,
            units: crate::preferences::Units::Binary,
        }
    }

    #[test]
    fn library_reports_its_version() {
        assert!(version().starts_with("StreamExtract "), "{}", version());
    }

    #[test]
    fn strings_with_nul_are_rejected() {
        let mut bad = config();
        bad.host = "ftp\0.example.com".to_string();
        assert!(Job::start(&bad).is_err());
        let mut bad = config();
        bad.archive_password = Some("pw\0".to_string());
        assert!(Job::start(&bad).is_err());
    }

    #[test]
    fn out_of_range_numbers_are_rejected() {
        let mut bad = config();
        bad.port = u32::MAX;
        assert!(Job::start(&bad).is_err());
        let mut bad = config();
        bad.retries = 0;
        assert!(Job::start(&bad).is_err());
    }

    #[test]
    fn empty_staging_is_disabled_and_nul_is_rejected() {
        for value in ["", "  "] {
            let mut empty = config();
            empty.staging = Some(value.to_string());
            assert!(Job::start(&empty).is_ok());
        }
        let mut bad = config();
        bad.staging = Some("/stage\0".to_string());
        assert!(Job::start(&bad).is_err());
    }

    #[test]
    fn password_with_nul_is_rejected() {
        let job = Job::start(&config()).unwrap();
        assert!(job.answer_password(Some("a\0b")).is_err());
        assert!(job.answer_password(None).is_ok());
    }
}
