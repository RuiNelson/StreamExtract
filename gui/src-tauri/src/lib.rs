//! rarftp-gui backend: exposes the `librarftpcore` transfer jobs and the "memory" file to the
//! web front-end as Tauri commands (see the contract, section B).

mod ffi;
mod memory;

use std::path::PathBuf;
use std::sync::{Mutex, MutexGuard, PoisonError};

use serde::{Deserialize, Serialize};
use serde_json::{json, Value};
use tauri::{AppHandle, Manager, State};

use ffi::Job;

/// FTP data connection mode.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum Mode {
    Passive,
    Active,
}

impl Mode {
    fn as_str(self) -> &'static str {
        match self {
            Mode::Passive => "passive",
            Mode::Active => "active",
        }
    }
}

/// Everything needed to start a transfer. `user` "" is anonymous, `directory` "" the login
/// directory, `rar_password` `null` means "ask when needed". Deliberately not `Debug`: it holds passwords.
#[derive(Deserialize)]
#[serde(rename_all = "snake_case")]
pub struct TransferConfig {
    pub archive: String,
    pub rar_password: Option<String>,
    pub host: String,
    pub port: u32,
    pub mode: Mode,
    pub user: String,
    pub password: String,
    pub directory: String,
    pub mkdir: bool,
    pub verbose: bool,
    pub buffer_mib: u32,
}

/// The server settings kept by Memory Save. `user`/`password` are `null` when the login was not stored.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub struct ServerSettings {
    pub host: String,
    pub port: u32,
    pub mode: Mode,
    pub user: Option<String>,
    pub password: Option<String>,
    pub directory: String,
    pub mkdir: bool,
}

/// The one transfer job of the app (if any). A finished job stays here until it is replaced or closed.
#[derive(Default)]
struct AppState {
    job: Mutex<Option<Job>>,
}

impl AppState {
    fn lock(&self) -> MutexGuard<'_, Option<Job>> {
        self.job.lock().unwrap_or_else(PoisonError::into_inner)
    }

    /// Starts a transfer, replacing a finished one; refused while a transfer is still running.
    fn start(&self, config: &TransferConfig) -> Result<(), String> {
        let old_job = {
            let mut slot = self.lock();
            if let Some(job) = slot.as_ref() {
                // Ask the library rather than trusting the last poll of the front-end.
                if !is_finished(&job.poll(u64::MAX)?) {
                    return Err("A transfer is already in progress".to_string());
                }
            }
            let job = Job::start(config)?;
            slot.replace(job)
        };
        drop(old_job); // finished: nothing to wait for
        Ok(())
    }

    fn poll(&self, cursor: u64) -> Result<Value, String> {
        match self.lock().as_ref() {
            Some(job) => job.poll(cursor),
            None => Err("no transfer".to_string()),
        }
    }

    fn answer_password(&self, password: Option<&str>) -> Result<(), String> {
        match self.lock().as_ref() {
            Some(job) => job.answer_password(password),
            None => Ok(()),
        }
    }

    fn cancel(&self) {
        if let Some(job) = self.lock().as_ref() {
            job.cancel();
        }
    }

    /// Takes the job out of the state; dropping it cancels and waits if it is still running.
    fn take(&self) -> Option<Job> {
        self.lock().take()
    }
}

fn is_finished(state: &Value) -> bool {
    state.get("phase").and_then(Value::as_str) == Some("finished")
}

fn memory_file(app: &AppHandle) -> Result<PathBuf, String> {
    let home = app
        .path()
        .home_dir()
        .map_err(|error| format!("Cannot determine the home directory: {error}"))?;
    Ok(memory::memory_path(&home))
}

#[tauri::command(rename_all = "snake_case")]
async fn app_info(app: AppHandle) -> Result<Value, String> {
    Ok(json!({
        "version": ffi::version(),
        "gui_version": env!("CARGO_PKG_VERSION"),
        "memory_path": memory_file(&app)?.to_string_lossy(),
    }))
}

#[tauri::command(rename_all = "snake_case")]
async fn start_transfer(state: State<'_, AppState>, config: TransferConfig) -> Result<(), String> {
    state.start(&config)
}

#[tauri::command(rename_all = "snake_case")]
async fn poll_transfer(state: State<'_, AppState>, cursor: u64) -> Result<Value, String> {
    state.poll(cursor)
}

#[tauri::command(rename_all = "snake_case")]
async fn answer_password(
    state: State<'_, AppState>,
    password: Option<String>,
) -> Result<(), String> {
    state.answer_password(password.as_deref())
}

#[tauri::command(rename_all = "snake_case")]
async fn cancel_transfer(state: State<'_, AppState>) -> Result<(), String> {
    state.cancel();
    Ok(())
}

/// Drops the job: cancels it and waits if it is still running, which can take a while (the partial
/// remote file is deleted first), so it is done on a blocking thread.
#[tauri::command(rename_all = "snake_case")]
async fn close_transfer(state: State<'_, AppState>) -> Result<(), String> {
    if let Some(job) = state.take() {
        tauri::async_runtime::spawn_blocking(move || drop(job))
            .await
            .map_err(|error| format!("Cannot close the transfer: {error}"))?;
    }
    Ok(())
}

#[tauri::command(rename_all = "snake_case")]
async fn memory_status(app: AppHandle) -> Result<memory::Status, String> {
    memory::status(&memory_file(&app)?)
}

#[tauri::command(rename_all = "snake_case")]
async fn memory_save(
    app: AppHandle,
    server: ServerSettings,
    store_credentials: Option<bool>,
) -> Result<(), String> {
    memory::save(&memory_file(&app)?, &server, store_credentials)
}

#[tauri::command(rename_all = "snake_case")]
async fn memory_recall(app: AppHandle) -> Result<Option<ServerSettings>, String> {
    memory::recall(&memory_file(&app)?)
}

#[tauri::command(rename_all = "snake_case")]
async fn memory_clear(app: AppHandle) -> Result<(), String> {
    memory::clear(&memory_file(&app)?)
}

pub fn run() {
    let app = tauri::Builder::default()
        .plugin(tauri_plugin_dialog::init())
        .manage(AppState::default())
        .invoke_handler(tauri::generate_handler![
            app_info,
            start_transfer,
            poll_transfer,
            answer_password,
            cancel_transfer,
            close_transfer,
            memory_status,
            memory_save,
            memory_recall,
            memory_clear,
        ])
        .build(tauri::generate_context!())
        .expect("error while building the rarftp-gui application");

    app.run(|app_handle, event| {
        if let tauri::RunEvent::Exit = event {
            // A transfer still running is cancelled and awaited, so the partial remote file is removed.
            if let Some(state) = app_handle.try_state::<AppState>() {
                drop(state.take());
            }
        }
    });
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::time::{Duration, Instant};

    /// Needs the real library: the archive does not exist, so the job fails by itself, quickly.
    fn missing_archive_config() -> TransferConfig {
        TransferConfig {
            archive: "/nonexistent/rarftp-gui-test.rar".to_string(),
            rar_password: None,
            host: "127.0.0.1".to_string(),
            port: 1,
            mode: Mode::Passive,
            user: String::new(),
            password: String::new(),
            directory: String::new(),
            mkdir: false,
            verbose: false,
            buffer_mib: 1,
        }
    }

    fn wait_until_finished(state: &AppState) -> Value {
        let deadline = Instant::now() + Duration::from_secs(30);
        loop {
            let snapshot = state.poll(0).unwrap();
            if is_finished(&snapshot) {
                return snapshot;
            }
            assert!(
                Instant::now() < deadline,
                "the job did not finish: {snapshot}"
            );
            std::thread::sleep(Duration::from_millis(20));
        }
    }

    #[test]
    fn commands_without_a_job() {
        let state = AppState::default();
        assert_eq!(state.poll(0).unwrap_err(), "no transfer");
        state.answer_password(Some("pw")).unwrap();
        state.cancel();
        assert!(state.take().is_none());
    }

    #[test]
    fn a_failed_job_is_reported_and_can_be_replaced_and_closed() {
        let state = AppState::default();
        state.start(&missing_archive_config()).unwrap();
        let snapshot = wait_until_finished(&state);
        assert_eq!(snapshot["result"]["status"], "failed", "{snapshot}");
        assert_eq!(snapshot["cancelling"], false);
        assert!(snapshot["log"]["next"].is_u64(), "{snapshot}");

        // A finished job does not block the next one.
        state.start(&missing_archive_config()).unwrap();
        wait_until_finished(&state);

        drop(state.take());
        assert_eq!(state.poll(0).unwrap_err(), "no transfer");
    }

    #[test]
    fn dropping_a_running_job_cancels_it() {
        let state = AppState::default();
        state.start(&missing_archive_config()).unwrap();
        state.cancel();
        drop(state.take()); // must not hang
    }
}
