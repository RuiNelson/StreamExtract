//! StreamExtract backend: exposes the `libstreamextractcore` transfer jobs and the "memory" file to the
//! web front-end as Tauri commands (see the contract, section B).

mod batch;
mod ffi;
mod ini;
mod memory;
mod preferences;
mod sleep_inhibitor;
mod updates;

use std::path::PathBuf;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Mutex, MutexGuard, PoisonError};

use serde::{Deserialize, Serialize};
use serde_json::{json, Value};
use tauri::{AppHandle, Manager, State};
use tauri_plugin_opener::OpenerExt;

use batch::Batch;

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

/// Control connection protocol, including the two FTPS negotiation modes.
#[derive(Debug, Default, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum Protocol {
    #[default]
    Ftp,
    FtpsExplicit,
    FtpsImplicit,
    Sftp,
}

impl Protocol {
    fn as_str(self) -> &'static str {
        match self {
            Self::Ftp => "ftp",
            Self::FtpsExplicit => "ftps_explicit",
            Self::FtpsImplicit => "ftps_implicit",
            Self::Sftp => "sftp",
        }
    }

    fn default_port(self) -> u32 {
        if self == Self::Sftp {
            22
        } else if self == Self::FtpsImplicit {
            990
        } else {
            21
        }
    }
}

/// Everything needed to start a transfer. `user` "" is anonymous, `directory` "" the login
/// directory, `archive_password` `null` means "ask when needed". Deliberately not `Debug`: it
/// holds passwords.
#[derive(Clone, Deserialize)]
#[serde(rename_all = "snake_case")]
pub struct TransferConfig {
    pub archive: String,
    pub archive_password: Option<String>,
    pub host: String,
    pub port: u32,
    #[serde(default)]
    pub protocol: Protocol,
    #[serde(default)]
    pub private_key: Option<String>,
    #[serde(default)]
    pub private_key_passphrase: Option<String>,
    #[serde(default)]
    pub known_hosts: Option<String>,
    pub mode: Mode,
    pub user: String,
    pub password: String,
    pub directory: String,
    pub mkdir: bool,
    pub verbose: bool,
    pub buffer_mib: u32,
    #[serde(default = "preferences::default_retries")]
    pub retries: u32,
    #[serde(default = "preferences::default_prevent_sleep")]
    pub prevent_sleep: bool,
    #[serde(default)]
    pub units: preferences::Units,
}

/// The server settings kept by Memory Save. `user`/`password` are `null` when the login was not stored.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub struct ServerSettings {
    pub host: String,
    pub port: u32,
    #[serde(default)]
    pub protocol: Protocol,
    #[serde(default)]
    pub private_key: Option<String>,
    #[serde(default)]
    pub private_key_passphrase: Option<String>,
    #[serde(default)]
    pub known_hosts: Option<String>,
    pub mode: Mode,
    pub user: Option<String>,
    pub password: Option<String>,
    pub directory: String,
    pub mkdir: bool,
}

/// The one transfer job of the app (if any). A finished job stays here until it is replaced or closed.
#[derive(Default)]
struct AppState {
    job: Mutex<Option<Batch>>,
}

/// At most one release lookup per launch, after consent. Separate from the transfer lock.
#[derive(Default)]
struct UpdateState {
    checked: AtomicBool,
    consent: Mutex<Option<tokio::sync::oneshot::Sender<bool>>>,
    version: Mutex<Option<String>>,
}

impl AppState {
    fn lock(&self) -> MutexGuard<'_, Option<Batch>> {
        self.job.lock().unwrap_or_else(PoisonError::into_inner)
    }

    /// Starts a transfer, replacing a finished one; refused while a transfer is still running.
    fn start(&self, config: &TransferConfig, archives: Vec<String>) -> Result<(), String> {
        let old_job = {
            let mut slot = self.lock();
            if let Some(job) = slot.as_ref() {
                // Ask the library rather than trusting the last poll of the front-end.
                if !is_finished(&job.poll(u64::MAX)?) {
                    return Err("A transfer is already in progress".to_string());
                }
            }
            let job = Batch::start(config, archives)?;
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
    fn take(&self) -> Option<Batch> {
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
async fn preferences_load(app: AppHandle) -> Result<preferences::Preferences, String> {
    preferences::load(&memory_file(&app)?.with_file_name("preferences.ini"))
}

#[tauri::command(rename_all = "snake_case")]
async fn preferences_save(
    app: AppHandle,
    preferences: preferences::Preferences,
) -> Result<(), String> {
    preferences::save(
        &memory_file(&app)?.with_file_name("preferences.ini"),
        preferences,
    )
}

#[tauri::command]
async fn ask_update_consent(app: AppHandle, state: State<'_, UpdateState>) -> Result<bool, String> {
    let (sender, receiver) = tokio::sync::oneshot::channel();
    {
        let mut consent = state.consent.lock().unwrap_or_else(PoisonError::into_inner);
        if consent.is_some() {
            return Err("The update consent window is already open".to_string());
        }
        *consent = Some(sender);
    }
    let window = tauri::WebviewWindowBuilder::new(
        &app,
        "update-consent",
        tauri::WebviewUrl::App("updates.html?kind=consent".into()),
    )
    .title("StreamExtract — Update checks")
    .inner_size(460.0, 240.0)
    .resizable(false)
    .maximizable(false)
    .minimizable(false)
    .build();
    match window {
        Ok(window) => {
            let handle = app.clone();
            window.on_window_event(move |event| {
                if matches!(event, tauri::WindowEvent::Destroyed) {
                    let state = handle.state::<UpdateState>();
                    let sender = state
                        .consent
                        .lock()
                        .unwrap_or_else(PoisonError::into_inner)
                        .take();
                    if let Some(sender) = sender {
                        let _ = sender.send(false);
                    }
                }
            });
        }
        Err(error) => {
            state
                .consent
                .lock()
                .unwrap_or_else(PoisonError::into_inner)
                .take();
            return Err(error.to_string());
        }
    }
    Ok(receiver.await.unwrap_or(false))
}

#[tauri::command]
async fn answer_update_consent(state: State<'_, UpdateState>, enabled: bool) -> Result<(), String> {
    if let Some(sender) = state
        .consent
        .lock()
        .unwrap_or_else(PoisonError::into_inner)
        .take()
    {
        let _ = sender.send(enabled);
    }
    Ok(())
}

#[tauri::command]
async fn update_version(state: State<'_, UpdateState>) -> Result<Option<String>, String> {
    Ok(state
        .version
        .lock()
        .unwrap_or_else(PoisonError::into_inner)
        .clone())
}

#[tauri::command]
async fn show_update_available(
    app: AppHandle,
    state: State<'_, UpdateState>,
) -> Result<(), String> {
    if preferences::load(&memory_file(&app)?.with_file_name("preferences.ini"))?.check_updates
        != Some(true)
        || state
            .version
            .lock()
            .unwrap_or_else(PoisonError::into_inner)
            .is_none()
        || app.get_webview_window("update-available").is_some()
    {
        return Ok(());
    }
    tauri::WebviewWindowBuilder::new(
        &app,
        "update-available",
        tauri::WebviewUrl::App("updates.html?kind=release".into()),
    )
    .title("StreamExtract — New version")
    .inner_size(460.0, 220.0)
    .resizable(false)
    .maximizable(false)
    .minimizable(false)
    .focused(false) // A background result must not take focus from a transfer or another app.
    .build()
    .map(|_| ())
    .map_err(|error| error.to_string())
}

#[tauri::command]
async fn dismiss_update_prompt(app: AppHandle) -> Result<(), String> {
    close_update_windows(&app)
}

fn close_update_windows(app: &AppHandle) -> Result<(), String> {
    for label in ["update-consent", "update-available"] {
        if let Some(window) = app.get_webview_window(label) {
            window.destroy().map_err(|error| error.to_string())?;
        }
    }
    Ok(())
}

#[tauri::command]
async fn check_for_updates(
    app: AppHandle,
    state: State<'_, UpdateState>,
) -> Result<Option<String>, String> {
    let path = memory_file(&app)?.with_file_name("preferences.ini");
    if preferences::load(&path)?.check_updates != Some(true)
        || state.checked.swap(true, Ordering::Relaxed)
    {
        return Ok(None);
    }
    // github_release_check uses blocking HTTP; never hold a job lock or run it on the UI thread.
    let result = tauri::async_runtime::spawn_blocking(updates::check)
        .await
        .map_err(|error| error.to_string())??;
    // The user may have disabled checks while the request was in flight.
    if preferences::load(&path)?.check_updates == Some(true) {
        *state.version.lock().unwrap_or_else(PoisonError::into_inner) = result.clone();
        Ok(result)
    } else {
        Ok(None)
    }
}

#[tauri::command]
async fn open_releases(app: AppHandle) -> Result<(), String> {
    app.opener()
        .open_url(updates::RELEASES_URL, None::<&str>)
        .map_err(|error| error.to_string())
}

#[tauri::command(rename_all = "snake_case")]
async fn start_transfer(
    state: State<'_, AppState>,
    config: TransferConfig,
    archives: Option<Vec<String>>,
) -> Result<(), String> {
    let archives = archives.unwrap_or_else(|| vec![config.archive.clone()]);
    state.start(&config, archives)
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
        .plugin(
            tauri_plugin_opener::Builder::new()
                .open_js_links_on_click(false)
                .build(),
        )
        .manage(AppState::default())
        .manage(UpdateState::default())
        .on_window_event(|window, event| {
            if window.label() == "main" && matches!(event, tauri::WindowEvent::Destroyed) {
                let _ = close_update_windows(window.app_handle());
            }
        })
        .invoke_handler(tauri::generate_handler![
            app_info,
            preferences_load,
            preferences_save,
            check_for_updates,
            ask_update_consent,
            answer_update_consent,
            update_version,
            show_update_available,
            dismiss_update_prompt,
            open_releases,
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
        .expect("error while building the StreamExtract application");

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
            archive: "/nonexistent/streamextract-test.rar".to_string(),
            archive_password: None,
            host: "127.0.0.1".to_string(),
            port: 1,
            protocol: crate::Protocol::Ftp,
            private_key: None,
            private_key_passphrase: None,
            known_hosts: None,
            mode: Mode::Passive,
            user: String::new(),
            password: String::new(),
            directory: String::new(),
            mkdir: false,
            verbose: false,
            buffer_mib: 1,
            retries: 3,
            prevent_sleep: true,
            units: preferences::Units::Binary,
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
        state
            .start(
                &missing_archive_config(),
                vec![missing_archive_config().archive],
            )
            .unwrap();
        let snapshot = wait_until_finished(&state);
        assert_eq!(snapshot["result"]["status"], "failed", "{snapshot}");
        assert_eq!(snapshot["cancelling"], false);
        assert!(snapshot["log"]["next"].is_u64(), "{snapshot}");

        // A finished job does not block the next one.
        state
            .start(
                &missing_archive_config(),
                vec![missing_archive_config().archive],
            )
            .unwrap();
        wait_until_finished(&state);

        drop(state.take());
        assert_eq!(state.poll(0).unwrap_err(), "no transfer");
    }

    #[test]
    fn dropping_a_running_job_cancels_it() {
        let state = AppState::default();
        state
            .start(
                &missing_archive_config(),
                vec![missing_archive_config().archive],
            )
            .unwrap();
        state.cancel();
        drop(state.take()); // must not hang
    }
}
