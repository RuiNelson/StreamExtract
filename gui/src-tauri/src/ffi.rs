//! Bindings to `librarftpcore` (`src/capi/rarftp.h`) and a safe wrapper around a transfer job.

use std::ffi::{c_char, c_int, c_uint, CStr, CString};
use std::marker::{PhantomData, PhantomPinned};
use std::ptr::{self, NonNull};

use serde_json::Value;

use crate::{Mode, TransferConfig};

/// Opaque `rarftp_job`.
#[repr(C)]
pub struct RarftpJob {
    _data: [u8; 0],
    _marker: PhantomData<(*mut u8, PhantomPinned)>,
}

/// Mirror of `rarftp_job_config`; the field order and types must match the header exactly.
#[repr(C)]
pub struct RarftpJobConfig {
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

// The library is linked by build.rs.
extern "C" {
    pub fn rarftp_version() -> *const c_char;
    pub fn rarftp_job_start(config: *const RarftpJobConfig) -> *mut RarftpJob;
    pub fn rarftp_job_poll(job: *mut RarftpJob, log_cursor: u64) -> *mut c_char;
    pub fn rarftp_job_answer_password(job: *mut RarftpJob, password: *const c_char);
    pub fn rarftp_job_cancel(job: *mut RarftpJob);
    pub fn rarftp_job_free(job: *mut RarftpJob);
    pub fn rarftp_free(string: *mut c_char);
}

/// `"rarftp 2.3.0 (UnRAR 7.31, LZMA SDK 26.03, libarchive 3.8.9, libcurl 8.22.0)"`.
pub fn version() -> String {
    // SAFETY: returns a NUL-terminated string in static storage that is never freed.
    unsafe { CStr::from_ptr(rarftp_version()) }
        .to_string_lossy()
        .into_owned()
}

fn c_string(what: &str, value: &str) -> Result<CString, String> {
    CString::new(value).map_err(|_| format!("{what} must not contain NUL characters"))
}

/// A running (or finished) transfer. Dropping it cancels the job if needed, waits for it
/// (so the partial remote file is deleted) and frees it.
pub struct Job {
    raw: NonNull<RarftpJob>,
}

// SAFETY: the C API documents that every function is thread-safe for a given job, and the
// handle is only freed in `Drop`, which needs exclusive ownership.
unsafe impl Send for Job {}
unsafe impl Sync for Job {}

impl Job {
    /// Starts a job. The library copies the strings, so the `CString`s only live for this call.
    pub fn start(config: &TransferConfig) -> Result<Job, String> {
        let archive = c_string("The archive path", &config.archive)?;
        let archive_password = config
            .archive_password
            .as_deref()
            .map(|password| c_string("The archive password", password))
            .transpose()?;
        let host = c_string("The host", &config.host)?;
        let user = c_string("The user name", &config.user)?;
        let password = c_string("The password", &config.password)?;
        let directory = c_string("The directory", &config.directory)?;
        let port =
            c_int::try_from(config.port).map_err(|_| "The port is out of range".to_string())?;
        let buffer_mib = c_uint::try_from(config.buffer_mib)
            .map_err(|_| "The buffer size is out of range".to_string())?;

        let raw_config = RarftpJobConfig {
            archive: archive.as_ptr(),
            archive_password: archive_password
                .as_ref()
                .map_or(ptr::null(), |password| password.as_ptr()),
            host: host.as_ptr(),
            port,
            active_mode: c_int::from(config.mode == Mode::Active),
            user: user.as_ptr(),
            password: password.as_ptr(),
            directory: directory.as_ptr(),
            mkdir: c_int::from(config.mkdir),
            verbose: c_int::from(config.verbose),
            buffer_mib,
        };
        // SAFETY: `raw_config` and the strings it points to outlive the call; the library copies them.
        let job = unsafe { rarftp_job_start(&raw_config) };
        NonNull::new(job)
            .map(|raw| Job { raw })
            .ok_or_else(|| "The library could not start the transfer".to_string())
    }

    /// Current state (contract section A), with the log lines numbered `cursor` and later.
    pub fn poll(&self, cursor: u64) -> Result<Value, String> {
        // SAFETY: `self.raw` is a live job.
        let text = unsafe { rarftp_job_poll(self.raw.as_ptr(), cursor) };
        if text.is_null() {
            return Err("The library returned no state".to_string());
        }
        // SAFETY: a non-NULL result is a NUL-terminated string that we own until `rarftp_free`.
        let parsed = serde_json::from_str(&unsafe { CStr::from_ptr(text) }.to_string_lossy());
        // SAFETY: `text` came from `rarftp_job_poll` and is not used afterwards.
        unsafe { rarftp_free(text) };
        parsed.map_err(|error| format!("The library returned invalid state: {error}"))
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
        unsafe { rarftp_job_answer_password(self.raw.as_ptr(), password) };
        Ok(())
    }

    /// Asks the job to stop; returns immediately.
    pub fn cancel(&self) {
        // SAFETY: `self.raw` is a live job.
        unsafe { rarftp_job_cancel(self.raw.as_ptr()) };
    }
}

impl Drop for Job {
    fn drop(&mut self) {
        // SAFETY: `self.raw` is live and never used again; this cancels, waits and frees.
        unsafe { rarftp_job_free(self.raw.as_ptr()) };
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn config() -> TransferConfig {
        TransferConfig {
            archive: "/nonexistent/rarftp-gui-test.rar".to_string(),
            archive_password: None,
            host: "127.0.0.1".to_string(),
            port: 21,
            mode: Mode::Passive,
            user: String::new(),
            password: String::new(),
            directory: String::new(),
            mkdir: false,
            verbose: false,
            buffer_mib: 0,
        }
    }

    #[test]
    fn library_reports_its_version() {
        assert!(version().starts_with("rarftp "), "{}", version());
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
    }

    #[test]
    fn password_with_nul_is_rejected() {
        let job = Job::start(&config()).unwrap();
        assert!(job.answer_password(Some("a\0b")).is_err());
        assert!(job.answer_password(None).is_ok());
    }
}
