//! Runs archives in order with shared settings, independently of webview polling.
//! The C API continues to handle one archive; only the GUI adds batch metadata.

use std::collections::{HashMap, VecDeque};
use std::sync::{Arc, Mutex, MutexGuard, PoisonError};
use std::thread::{self, JoinHandle};
use std::time::Duration;

use serde_json::{json, Value};

use crate::{ffi::Job, is_finished, sleep_inhibitor::SleepInhibitor, TransferConfig};

const LOG_LIMIT: usize = 5000;

struct BatchState {
    job: Option<Job>,
    snapshot: Value,
    items: Vec<Value>,
    index: usize,
    engine_cursor: u64,
    next_log: u64,
    logs: VecDeque<Value>,
    cancelled: bool,
    finished: bool,
}

impl BatchState {
    fn append_log(&mut self, mut line: Value) {
        line["seq"] = json!(self.next_log);
        self.next_log += 1;
        self.logs.push_back(line);
        if self.logs.len() > LOG_LIMIT {
            self.logs.pop_front();
        }
    }

    fn record_snapshot(&mut self, mut snapshot: Value) {
        if let Some(lines) = snapshot["log"]["lines"].as_array() {
            for line in lines {
                let mut line = line.clone();
                if self.items.len() > 1 {
                    line["text"] = json!(format!(
                        "[{}] {}",
                        self.items[self.index]["path"].as_str().unwrap_or_default(),
                        line["text"].as_str().unwrap_or_default()
                    ));
                }
                self.append_log(line);
            }
        }
        self.engine_cursor = snapshot["log"]["next"].as_u64().unwrap_or(0);
        snapshot["log"] = Value::Null; // logs are kept once, with a batch-wide cursor
        self.snapshot = snapshot;
    }

    fn finish(&mut self) {
        self.finished = true;
        for item in &mut self.items {
            if item["status"] == "waiting" {
                item["status"] = json!("not_started");
            }
        }
        self.snapshot["phase"] = json!("finished");
        self.snapshot["cancelling"] = json!(false);
        self.snapshot["prompt"] = Value::Null;
        if self.items.len() > 1 {
            self.snapshot["result"] = batch_result(&self.items, self.cancelled);
        }
    }
}

fn lock(state: &Mutex<BatchState>) -> MutexGuard<'_, BatchState> {
    state.lock().unwrap_or_else(PoisonError::into_inner)
}

pub struct Batch {
    state: Arc<Mutex<BatchState>>,
    worker: Option<JoinHandle<()>>,
}

impl Batch {
    pub fn start(
        config: &TransferConfig,
        archives: Vec<String>,
        extraction_roots: HashMap<String, String>,
        archive_passwords: HashMap<String, String>,
    ) -> Result<Self, String> {
        if archives.is_empty() || archives.iter().any(|path| path.trim().is_empty()) {
            return Err("Choose at least one archive".to_string());
        }
        let mut config = config.clone();
        config.archive = archives[0].clone();
        config.archive_password = archive_passwords.get(&config.archive).cloned();
        config.extraction_root = extraction_roots
            .get(&config.archive)
            .cloned()
            .unwrap_or_default();
        let job = Job::start(&config)?;
        let snapshot = job.poll(0)?;
        let mut state = BatchState {
            job: Some(job),
            snapshot: Value::Null,
            items: archives
                .into_iter()
                .map(|path| {
                    json!({
                        "extraction_root": extraction_roots.get(&path).cloned().unwrap_or_default(),
                        "path": path, "status": "waiting", "result": null,
                    })
                })
                .collect(),
            index: 0,
            engine_cursor: 0,
            next_log: 0,
            logs: VecDeque::new(),
            cancelled: false,
            finished: false,
        };
        state.items[0]["status"] = json!("running");
        state.record_snapshot(snapshot);
        let state = Arc::new(Mutex::new(state));
        let worker_state = Arc::clone(&state);
        let worker = thread::Builder::new()
            .name("archive-batch".into())
            .spawn(move || {
                // Windows execution requirements are tied to the calling thread, so this guard
                // must be created and dropped by the batch controller itself. It remains active
                // during cancellation cleanup as well as every archive in the batch.
                let _sleep_inhibitor = SleepInhibitor::new(config.prevent_sleep);
                loop {
                    {
                        let mut state = lock(&worker_state);
                        if let Some(job) = state.job.as_ref() {
                            let snapshot = job
                                .poll(state.engine_cursor)
                                .unwrap_or_else(failed_snapshot);
                            state.record_snapshot(snapshot);
                        }
                        if is_finished(&state.snapshot) {
                            let index = state.index;
                            let result = state.snapshot["result"].clone();
                            state.items[index]["status"] = result["status"].clone();
                            state.items[index]["result"] = result;
                            // A declined password fails this archive only; the Cancel button
                            // sets the batch flag and prevents every remaining archive from starting.
                            if state.cancelled || index + 1 == state.items.len() {
                                state.finish();
                                break;
                            }
                            state.job.take(); // finished; joins and frees the previous engine
                            state.index += 1;
                            let index = state.index;
                            config.archive =
                                state.items[index]["path"].as_str().unwrap().to_string();
                            config.archive_password =
                                archive_passwords.get(&config.archive).cloned();
                            config.extraction_root = state.items[index]["extraction_root"]
                                .as_str()
                                .unwrap()
                                .to_string();
                            state.items[index]["status"] = json!("running");
                            state.engine_cursor = 0;
                            match Job::start(&config) {
                                Ok(job) => {
                                    state.record_snapshot(
                                        job.poll(0).unwrap_or_else(failed_snapshot),
                                    );
                                    state.job = Some(job);
                                }
                                Err(error) => state.record_snapshot(failed_snapshot(error)),
                            }
                        }
                    }
                    thread::sleep(Duration::from_millis(100));
                }
            })
            .map_err(|error| format!("Could not start the batch: {error}"))?;
        Ok(Self {
            state,
            worker: Some(worker),
        })
    }

    pub fn poll(&self, cursor: u64) -> Result<Value, String> {
        let state = lock(&self.state);
        let mut snapshot = state.snapshot.clone();
        // An engine can finish before the controller records its result and starts
        // the next archive. Only the controller can declare the batch finished.
        if !state.finished && is_finished(&snapshot) {
            snapshot["phase"] = json!("reading");
            snapshot["result"] = Value::Null;
        }
        let completed = state
            .items
            .iter()
            .filter(|item| item["result"].is_object())
            .count();
        snapshot["batch"] = json!({
            "total": state.items.len(), "completed": completed,
            "current_index": state.index, "items": state.items,
        });
        snapshot["log"] = json!({
            "next": state.next_log,
            "lines": state.logs.iter().filter(|line| line["seq"].as_u64().unwrap_or(0) >= cursor)
                .collect::<Vec<_>>(),
        });
        if !is_finished(&snapshot) && state.cancelled {
            snapshot["cancelling"] = json!(true);
        }
        Ok(snapshot)
    }

    pub fn answer_password(&self, password: Option<&str>) -> Result<(), String> {
        let mut state = lock(&self.state);
        if let Some(job) = state.job.as_ref() {
            job.answer_password(password)?;
            state.snapshot["prompt"] = Value::Null;
        }
        Ok(())
    }

    pub fn cancel(&self) {
        let mut state = lock(&self.state);
        if state.finished {
            return;
        }
        state.cancelled = true;
        if let Some(job) = state.job.as_ref() {
            job.cancel();
        }
    }
}

impl Drop for Batch {
    fn drop(&mut self) {
        self.cancel();
        if let Some(worker) = self.worker.take() {
            let _ = worker.join();
        }
        // The last Job is dropped with the state, cancelling and awaiting cleanup if needed.
    }
}

fn failed_snapshot(error: String) -> Value {
    json!({
        "phase": "finished", "cancelling": false, "prompt": null, "archive": null,
        "target": null, "mode": null, "user": null, "probe": null, "progress": null,
        "log": { "next": 0, "lines": [] },
        "result": { "status": "failed", "error": error, "summary": [],
            "problems": [], "problems_dropped": 0 },
    })
}

fn batch_result(items: &[Value], cancelled: bool) -> Value {
    let count = |status: &str| items.iter().filter(|item| item["status"] == status).count();
    let success = count("success");
    let failed = count("failed");
    let cancelled_count = count("cancelled");
    let not_started = count("not_started");
    let status = if cancelled {
        "cancelled"
    } else if failed > 0 {
        "failed"
    } else if cancelled_count > 0 {
        "cancelled"
    } else {
        "success"
    };
    let mut summary = vec![json!(format!(
        "{} archives: {} completed, {} failed, {} cancelled, {} not started",
        items.len(),
        success,
        failed,
        cancelled_count,
        not_started
    ))];
    let mut problems = VecDeque::new();
    let mut dropped = 0;
    for item in items {
        let path = item["path"].as_str().unwrap_or_default();
        let label = match item["status"].as_str().unwrap_or_default() {
            "success" => "Done",
            "failed" => "Failed",
            "cancelled" => "Cancelled",
            "not_started" => "Not started",
            _ => "Waiting",
        };
        summary.push(json!(format!("{path} — {label}")));
        if let Some(lines) = item["result"]["summary"].as_array() {
            summary.extend(lines.iter().cloned());
        }
        // Startup failures may have no engine summary.
        if let Some(error) = item["result"]["error"].as_str().filter(|s| !s.is_empty()) {
            if item["result"]["summary"]
                .as_array()
                .is_none_or(|lines| lines.is_empty())
            {
                summary.push(json!(error));
            }
        }
        dropped += item["result"]["problems_dropped"].as_u64().unwrap_or(0);
        if let Some(lines) = item["result"]["problems"].as_array() {
            for line in lines {
                let mut line = line.clone();
                line["text"] = json!(format!(
                    "[{path}] {}",
                    line["text"].as_str().unwrap_or_default()
                ));
                problems.push_back(line);
                if problems.len() > LOG_LIMIT {
                    problems.pop_front();
                    dropped += 1;
                }
            }
        }
    }
    json!({
        "status": status,
        "error": if failed > 0 { Some(format!("{failed} archive(s) failed. See the results below.")) } else { None },
        "summary": summary, "problems": problems, "problems_dropped": dropped,
    })
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::{preferences::Units, Mode};
    use std::path::PathBuf;
    use std::sync::atomic::{AtomicU64, Ordering};
    use std::time::Instant;

    fn config() -> TransferConfig {
        TransferConfig {
            archive: "/nonexistent/batch-first.zip".into(),
            extraction_root: String::new(),
            archive_password: None,
            host: "127.0.0.1".into(),
            port: 1,
            protocol: crate::Protocol::Ftp,
            private_key: None,
            private_key_passphrase: None,
            known_hosts: None,
            mode: Mode::Passive,
            user: String::new(),
            password: String::new(),
            directory: "/batch".into(),
            mkdir: false,
            verbose: false,
            buffer_mib: 1,
            retries: 3,
            prevent_sleep: true,
            units: Units::Si,
        }
    }

    fn wait_for(batch: &Batch, condition: impl Fn(&Value) -> bool) -> Value {
        let deadline = Instant::now() + Duration::from_secs(10);
        loop {
            let snapshot = batch.poll(0).unwrap();
            if condition(&snapshot) {
                return snapshot;
            }
            assert!(
                Instant::now() < deadline,
                "batch did not reach expected state: {snapshot}"
            );
            thread::sleep(Duration::from_millis(20));
        }
    }

    struct FixtureArchive(PathBuf);

    impl FixtureArchive {
        fn new() -> Self {
            Self::from_fixture("kAesZip")
        }

        fn from_fixture(name: &str) -> Self {
            static NEXT: AtomicU64 = AtomicU64::new(0);
            let path = std::env::temp_dir().join(format!(
                "streamextract-batch-{}-{}.zip",
                std::process::id(),
                NEXT.fetch_add(1, Ordering::Relaxed)
            ));
            // Reuse real archive fixtures shared by the C++ tests.
            let source = include_str!("../../../tests/archive_fixtures.hpp");
            let array = source
                .split_once(&format!("{name}[] = {{"))
                .unwrap()
                .1
                .split_once("};")
                .unwrap()
                .0;
            let bytes: Vec<u8> = array
                .split(',')
                .filter_map(|value| {
                    let value = value.trim();
                    if value.is_empty() {
                        None
                    } else {
                        Some(u8::from_str_radix(value.trim_start_matches("0x"), 16).unwrap())
                    }
                })
                .collect();
            std::fs::write(&path, bytes).unwrap();
            Self(path)
        }

        fn path(&self) -> String {
            self.0.to_str().unwrap().to_string()
        }
    }

    impl Drop for FixtureArchive {
        fn drop(&mut self) {
            let _ = std::fs::remove_file(&self.0);
        }
    }

    #[test]
    fn empty_batches_are_rejected() {
        assert!(Batch::start(&config(), vec![], HashMap::new(), HashMap::new()).is_err());
        assert!(Batch::start(
            &config(),
            vec![String::new()],
            HashMap::new(),
            HashMap::new()
        )
        .is_err());
    }

    #[test]
    fn every_archive_uses_its_own_root_and_missing_selections_use_the_archive_root() {
        let first = FixtureArchive::from_fixture("kExtractionRootZip");
        let second = FixtureArchive::from_fixture("kExtractionRootZip");
        let third = FixtureArchive::from_fixture("kExtractionRootZip");
        let roots = HashMap::from([
            (first.path(), "missing-first".to_string()),
            (second.path(), "missing-second".to_string()),
        ]);
        let mut settings = config();
        settings.extraction_root = "must-not-carry-over".into();
        let batch = Batch::start(
            &settings,
            vec![first.path(), second.path(), third.path()],
            roots,
            HashMap::new(),
        )
        .unwrap();
        let snapshot = wait_for(&batch, is_finished);
        let items = snapshot["batch"]["items"].as_array().unwrap();
        for (index, root) in ["missing-first", "missing-second"].iter().enumerate() {
            assert_eq!(items[index]["extraction_root"], *root);
            assert_eq!(
                items[index]["result"]["error"],
                format!("extraction root \"{root}\" is not a directory in the archive")
            );
        }
        assert_eq!(items[2]["extraction_root"], "");
        assert!(items[2]["result"]["error"]
            .as_str()
            .unwrap()
            .contains("connect"));
        assert_eq!(snapshot["batch"]["completed"], 3);
    }

    #[test]
    fn failures_continue_without_frontend_polls_and_logs_have_one_cursor() {
        let mut batch = Batch::start(
            &config(),
            vec![
                "/nonexistent/batch-first.zip".into(),
                "/nonexistent/batch-second.7z".into(),
                "/nonexistent/batch-third.tar.gz".into(),
            ],
            HashMap::new(),
            HashMap::new(),
        )
        .unwrap();
        // Wait for the controller itself, without driving it with GUI polls.
        batch.worker.take().unwrap().join().unwrap();
        let snapshot = batch.poll(0).unwrap();
        assert!(is_finished(&snapshot));
        assert_eq!(snapshot["batch"]["completed"], 3);
        assert_eq!(snapshot["batch"]["current_index"], 2);
        assert_eq!(snapshot["result"]["status"], "failed");
        assert!(snapshot["batch"]["items"]
            .as_array()
            .unwrap()
            .iter()
            .all(|item| item["status"] == "failed"));
        let lines = snapshot["log"]["lines"].as_array().unwrap();
        assert!(lines.len() >= 3);
        for pair in lines.windows(2) {
            assert!(pair[0]["seq"].as_u64().unwrap() < pair[1]["seq"].as_u64().unwrap());
        }
        let cursor = snapshot["log"]["next"].as_u64().unwrap();
        assert!(batch.poll(cursor).unwrap()["log"]["lines"]
            .as_array()
            .unwrap()
            .is_empty());
        assert_eq!(batch.poll(cursor).unwrap()["result"], snapshot["result"]);
    }

    #[test]
    fn an_archive_that_cannot_start_does_not_block_later_archives() {
        let batch = Batch::start(
            &config(),
            vec![
                config().archive,
                "bad\0path".into(),
                "/nonexistent/last.zip".into(),
            ],
            HashMap::new(),
            HashMap::new(),
        )
        .unwrap();
        let snapshot = wait_for(&batch, is_finished);
        assert_eq!(snapshot["batch"]["completed"], 3);
        assert!(snapshot["batch"]["items"][1]["result"]["error"]
            .as_str()
            .unwrap()
            .contains("NUL"));
        assert_eq!(snapshot["batch"]["items"][2]["status"], "failed");
    }

    #[test]
    fn cancelling_a_password_prompt_stops_the_batch_and_marks_waiting_archives() {
        let archive = FixtureArchive::new();
        let batch = Batch::start(
            &config(),
            vec![archive.path(), "/nonexistent/never-start.zip".into()],
            HashMap::new(),
            HashMap::new(),
        )
        .unwrap();
        wait_for(&batch, |snap| snap["prompt"]["kind"] == "archive_password");
        batch.cancel();
        let snapshot = wait_for(&batch, is_finished);
        assert_eq!(snapshot["result"]["status"], "cancelled");
        assert_eq!(snapshot["batch"]["items"][0]["status"], "cancelled");
        assert_eq!(snapshot["batch"]["items"][1]["status"], "not_started");
        assert_eq!(snapshot["batch"]["completed"], 1);
    }

    #[test]
    fn passwords_are_answered_for_each_archive_and_declining_one_continues() {
        let first = FixtureArchive::new();
        let second = FixtureArchive::new();
        let batch = Batch::start(
            &config(),
            vec![first.path(), second.path()],
            HashMap::new(),
            HashMap::new(),
        )
        .unwrap();
        wait_for(&batch, |snap| snap["prompt"]["kind"] == "archive_password");
        batch.answer_password(Some("wrong")).unwrap();
        let snapshot = wait_for(&batch, |snap| snap["prompt"]["error"].is_string());
        assert_eq!(snapshot["batch"]["current_index"], 0);
        batch.answer_password(None).unwrap();
        let snapshot = wait_for(&batch, |snap| {
            snap["batch"]["current_index"] == 1 && snap["prompt"].is_object()
        });
        assert_eq!(snapshot["batch"]["items"][0]["status"], "failed");
        batch.answer_password(Some("secret")).unwrap();
        let snapshot = wait_for(&batch, is_finished);
        // The correct password passes archive reading, then the deliberately closed FTP port fails.
        assert!(snapshot["batch"]["items"][1]["result"]["error"]
            .as_str()
            .unwrap()
            .contains("connect"));
        assert_eq!(snapshot["batch"]["completed"], 2);
    }

    #[test]
    fn supplied_passwords_are_per_archive_and_never_exposed_in_snapshots() {
        let first = FixtureArchive::new();
        let second = FixtureArchive::new();
        let third = FixtureArchive::new();
        let passwords = HashMap::from([
            (first.path(), "secret".to_string()),
            (second.path(), "second-private-password".to_string()),
        ]);
        let mut settings = config();
        settings.archive_password = Some("must-not-carry-over".into());
        let batch = Batch::start(
            &settings,
            vec![first.path(), second.path(), third.path()],
            HashMap::new(),
            passwords,
        )
        .unwrap();
        let snapshot = wait_for(&batch, |snap| {
            snap["batch"]["current_index"] == 1 && snap["prompt"]["error"] == "Wrong password"
        });
        assert!(snapshot["batch"]["items"][0]["result"]["error"]
            .as_str()
            .unwrap()
            .contains("connect"));
        for password in ["secret", "second-private-password", "must-not-carry-over"] {
            assert!(!snapshot.to_string().contains(password));
        }
        batch.answer_password(None).unwrap();
        let snapshot = wait_for(&batch, |snap| {
            snap["batch"]["current_index"] == 2 && snap["prompt"].is_object()
        });
        assert!(snapshot["prompt"]["error"].is_null());
        batch.answer_password(None).unwrap();
        assert_eq!(wait_for(&batch, is_finished)["batch"]["completed"], 3);
    }

    #[test]
    fn summaries_report_mixed_outcomes_and_keep_warnings_with_the_archive() {
        let items = vec![
            json!({"path": "first.zip", "status": "success", "result": {
                "summary": ["Uploaded 2 files"], "problems": [{"level": "warn", "text": "Ignored a link"}], "problems_dropped": 2,
            }}),
            json!({"path": "second.zip", "status": "failed", "result": {"error": "Bad archive", "summary": []}}),
        ];
        let result = batch_result(&items, false);
        assert_eq!(result["status"], "failed");
        assert_eq!(result["problems"][0]["text"], "[first.zip] Ignored a link");
        assert_eq!(result["problems_dropped"], 2);
        assert!(result["summary"]
            .as_array()
            .unwrap()
            .contains(&json!("Uploaded 2 files")));
        assert!(result["summary"]
            .as_array()
            .unwrap()
            .contains(&json!("Bad archive")));
        assert_eq!(batch_result(&items[..1], false)["status"], "success");
        assert_eq!(batch_result(&items, true)["status"], "cancelled");
    }
}
