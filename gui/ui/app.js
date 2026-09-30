"use strict";

/*
 * rarftp-gui frontend. Plain JS, no dependencies. Talks to the Rust backend through
 * window.__TAURI__ (app.withGlobalTauri). See the contract: poll JSON (A), commands (B),
 * window / close flow / drag and drop (C).
 */
(() => {
  const tauri = window.__TAURI__;

  const POLL_INTERVAL_MS = 200;
  const POLL_MAX_FAILURES = 25;
  const LOG_MAX_LINES = 2000;
  const QUIT_WAIT_MS = 30000;
  const UNKNOWN_TIME = "--:--:--";
  const LEVELS = ["debug", "info", "warn", "error"];

  const $ = (id) => document.getElementById(id);
  const els = {
    app: $("app"),
    version: $("app-version"),
    // setup
    setupView: $("setup-view"),
    dropzone: $("dropzone"),
    dzTitle: $("dz-title"),
    dzSub: $("dz-sub"),
    btnChoose: $("btn-choose"),
    archiveHint: $("archive-hint"),
    rarPassword: $("rar-password"),
    host: $("host"),
    port: $("port"),
    portError: $("port-error"),
    modeGroup: document.querySelectorAll('input[name="mode"]'),
    user: $("user"),
    password: $("password"),
    directory: $("directory"),
    mkdir: $("mkdir"),
    advanced: $("advanced"),
    buffer: $("buffer"),
    bufferError: $("buffer-error"),
    verbose: $("verbose"),
    memSave: $("btn-mem-save"),
    memRecall: $("btn-mem-recall"),
    memClear: $("btn-mem-clear"),
    submitHint: $("submit-hint"),
    btnUpload: $("btn-upload"),
    // transfer
    transferView: $("transfer-view"),
    tArchive: $("t-archive"),
    tArrow: $("t-arrow"),
    tTarget: $("t-target"),
    tSub: $("t-sub"),
    phaseDot: $("phase-dot"),
    phaseText: $("phase-text"),
    chips: $("chips"),
    currentBlock: $("current-block"),
    curLabel: $("cur-label"),
    curName: $("cur-name"),
    curBar: $("cur-bar"),
    curStats: $("cur-stats"),
    totalBlock: $("total-block"),
    totalBar: $("total-bar"),
    totalStats: $("total-stats"),
    totalFiles: $("total-files"),
    statusRow: $("status-row"),
    statUpload: $("stat-upload"),
    statUploadValue: $("stat-upload-value"),
    statUnpackValue: $("stat-unpack-value"),
    statBuffer: $("stat-buffer"),
    statBufferValue: $("stat-buffer-value"),
    bufferMeter: $("buffer-meter"),
    statElapsedValue: $("stat-elapsed-value"),
    log: $("log"),
    btnLatest: $("btn-latest"),
    tActions: $("t-actions"),
    btnCancel: $("btn-cancel"),
    result: $("t-result"),
    resultBanner: $("result-banner"),
    resultTitle: $("result-title"),
    resultError: $("result-error"),
    resultSummary: $("result-summary"),
    problems: $("result-problems"),
    problemsSummary: $("problems-summary"),
    problemsList: $("problems-list"),
    problemsDropped: $("problems-dropped"),
    btnNew: $("btn-new"),
    // shared
    srStatus: $("sr-status"),
    toasts: $("toasts"),
    dlgMemory: $("dlg-memory"),
    dlgMemoryPath: $("dlg-memory-path"),
    dlgPassword: $("dlg-password"),
    pwForm: $("pw-form"),
    pwText: $("pw-text"),
    pwInput: $("pw-input"),
    pwError: $("pw-error"),
    pwCancel: $("pw-cancel"),
    dlgQuit: $("dlg-quit"),
  };

  const state = {
    view: "setup",
    archive: null, // absolute path of the chosen archive
    info: null, // app_info()
    memory: { saved: false, store_credentials: null },
    memoryBusy: false,
    starting: false,
    quitting: false,
    job: null, // the transfer being shown, see newJob()
    logStick: true, // log follows the newest line
  };

  /* ------------------------------------------------------------ helpers */

  const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms));

  function invoke(command, args) {
    return tauri.core.invoke(command, args);
  }

  function errText(e) {
    if (typeof e === "string") return e;
    if (e && typeof e.message === "string") return e.message;
    try {
      return JSON.stringify(e);
    } catch (_) {
      return String(e);
    }
  }

  function el(tag, className, text) {
    const node = document.createElement(tag);
    if (className) node.className = className;
    if (text != null) node.textContent = text;
    return node;
  }

  function setText(node, text) {
    if (node.textContent !== text) node.textContent = text;
  }

  function setTitle(node, text) {
    if (node.title !== text) node.title = text;
  }

  function setHidden(node, hidden) {
    if (node.hidden !== hidden) node.hidden = hidden;
  }

  function basename(path) {
    const parts = String(path).split(/[\\/]/).filter(Boolean);
    return parts.length ? parts[parts.length - 1] : String(path);
  }

  function plural(n, word) {
    return `${n} ${word}${n === 1 ? "" : "s"}`;
  }

  function capitalize(s) {
    return s ? s.charAt(0).toUpperCase() + s.slice(1) : s;
  }

  function toast(message, kind) {
    const last = els.toasts.lastElementChild;
    if (last && last.textContent === message) return; // no stacked duplicates
    const node = el("div", "toast", message);
    node.dataset.kind = kind || "info";
    node.addEventListener("click", () => node.remove());
    els.toasts.append(node);
    while (els.toasts.children.length > 4) els.toasts.firstElementChild.remove();
    setTimeout(() => node.remove(), kind === "error" ? 7000 : 3000);
  }

  function toastError(prefix, e) {
    toast(prefix ? `${prefix}: ${errText(e)}` : errText(e), "error");
  }

  function announce(text) {
    els.srStatus.textContent = "";
    // A changed text node makes screen readers speak the live region again.
    setTimeout(() => {
      els.srStatus.textContent = text;
    }, 30);
  }

  // Shows a modal <dialog> whose buttons are <button type="submit" value="..."> of a
  // method="dialog" form. Resolves with the pressed button's value, or "" when dismissed
  // with Esc. It does not wait for the "close" event, which can be delayed (hidden window).
  function askDialog(dialog) {
    return new Promise((resolve) => {
      const done = (value) => {
        dialog.removeEventListener("submit", onSubmit);
        dialog.removeEventListener("cancel", onCancel);
        dialog.removeEventListener("close", onClose);
        resolve(value);
      };
      const onSubmit = (event) => done(event.submitter ? event.submitter.value : "");
      const onCancel = () => done("");
      const onClose = () => done(dialog.returnValue);
      dialog.addEventListener("submit", onSubmit);
      dialog.addEventListener("cancel", onCancel);
      dialog.addEventListener("close", onClose);
      dialog.returnValue = "";
      dialog.showModal();
    });
  }

  /* ---------------------------------------------------------------- views */

  function showView(name) {
    state.view = name;
    setHidden(els.setupView, name !== "setup");
    setHidden(els.transferView, name !== "transfer");
    setDragging(false);
  }

  /* ---------------------------------------------------------- setup: form */

  function selectedMode() {
    for (const radio of els.modeGroup) if (radio.checked) return radio.value;
    return "passive";
  }

  function setMode(mode) {
    for (const radio of els.modeGroup) radio.checked = radio.value === mode;
  }

  function setFieldError(input, errorEl, message) {
    if (message) input.setAttribute("aria-invalid", "true");
    else input.removeAttribute("aria-invalid");
    errorEl.textContent = message;
    setHidden(errorEl, !message);
  }

  function parseIntInRange(input, errorEl, min, max, message) {
    const value = input.value.trim();
    const n = /^\d+$/.test(value) ? Number(value) : NaN;
    const ok = Number.isInteger(n) && n >= min && n <= max;
    setFieldError(input, errorEl, ok ? "" : message);
    return ok ? n : null;
  }

  const validatePort = () =>
    parseIntInRange(els.port, els.portError, 1, 65535, "Port must be a number between 1 and 65535.");
  const validateBuffer = () =>
    parseIntInRange(els.buffer, els.bufferError, 1, 4096, "Buffer must be a number between 1 and 4096 MiB.");

  function updateSubmitState() {
    const host = els.host.value.trim();
    const hasArchive = Boolean(state.archive);
    els.btnUpload.disabled = !(hasArchive && host) || state.starting;
    let hint;
    if (!hasArchive && !host) hint = "Choose an archive and enter a host to continue.";
    else if (!hasArchive) hint = "Choose an archive to continue.";
    else if (!host) hint = "Enter a host to continue.";
    else hint = `${basename(state.archive)} → ${host}`;
    setText(els.submitHint, hint);
  }

  function setArchive(path) {
    state.archive = path;
    const name = basename(path);
    els.dropzone.dataset.state = "chosen";
    setText(els.dzTitle, name);
    setTitle(els.dzTitle, name);
    setText(els.dzSub, path);
    setTitle(els.dzSub, path);
    els.btnChoose.textContent = "Change…";
    const looksLikeRar = /\.rar$/i.test(name);
    els.archiveHint.textContent = looksLikeRar
      ? ""
      : "This file does not have a .rar extension. rarftp will still try to read it.";
    setHidden(els.archiveHint, looksLikeRar);
    updateSubmitState();
  }

  async function chooseArchive() {
    try {
      const selected = await tauri.dialog.open({
        multiple: false,
        directory: false,
        filters: [{ name: "RAR archives", extensions: ["rar"] }],
      });
      const path = Array.isArray(selected) ? selected[0] : selected;
      const value = path && typeof path === "object" ? path.path : path;
      if (value) setArchive(value);
    } catch (e) {
      toastError("Could not open the file dialog", e);
    }
  }

  function readServer() {
    return {
      host: els.host.value.trim(),
      mode: selectedMode(),
      user: els.user.value.trim(),
      password: els.password.value,
      directory: els.directory.value.trim(),
      mkdir: els.mkdir.checked,
    };
  }

  // Focus a field and keep it, with its error message below, clear of the footer.
  function focusField(input) {
    input.focus({ preventScroll: true });
    input.scrollIntoView({ block: "center" });
  }

  function buildConfig() {
    const port = validatePort();
    const buffer = validateBuffer();
    if (port === null) {
      focusField(els.port);
      return null;
    }
    if (buffer === null) {
      els.advanced.open = true;
      focusField(els.buffer);
      return null;
    }
    const server = readServer();
    return {
      archive: state.archive,
      rar_password: els.rarPassword.value === "" ? null : els.rarPassword.value,
      host: server.host,
      port,
      mode: server.mode,
      user: server.user,
      password: server.password,
      directory: server.directory,
      mkdir: server.mkdir,
      verbose: els.verbose.checked,
      buffer_mib: buffer,
    };
  }

  /* --------------------------------------------------- setup: drag and drop */

  function setDragging(on) {
    els.dropzone.classList.toggle("drag", on && state.view === "setup");
  }

  function anyDialogOpen() {
    return els.dlgMemory.open || els.dlgPassword.open || els.dlgQuit.open;
  }

  async function initDragDrop() {
    try {
      await tauri.webview.getCurrentWebview().onDragDropEvent((event) => {
        const payload = event.payload || {};
        if (state.view !== "setup" || anyDialogOpen()) {
          setDragging(false);
          return;
        }
        switch (payload.type) {
          case "enter":
            setDragging(!payload.paths || payload.paths.length > 0);
            break;
          case "over":
            setDragging(true);
            break;
          case "leave":
            setDragging(false);
            break;
          case "drop":
            setDragging(false);
            if (payload.paths && payload.paths.length) setArchive(payload.paths[0]);
            break;
          default:
            break;
        }
      });
    } catch (e) {
      toastError("Drag and drop is unavailable", e);
    }
  }

  /* ------------------------------------------------------- setup: memory */

  function updateMemoryButtons() {
    const busy = state.memoryBusy;
    els.memSave.disabled = busy;
    els.memRecall.disabled = busy || !state.memory.saved;
    els.memClear.disabled = busy || !state.memory.saved;
  }

  async function refreshMemory() {
    try {
      const status = await invoke("memory_status");
      state.memory = status || { saved: false, store_credentials: null };
    } catch (e) {
      state.memory = { saved: false, store_credentials: null };
      toastError("Could not read the saved server settings", e);
    }
    updateMemoryButtons();
  }

  async function runMemoryAction(action) {
    if (state.memoryBusy) return;
    state.memoryBusy = true;
    updateMemoryButtons();
    try {
      await action();
    } finally {
      state.memoryBusy = false;
      await refreshMemory();
    }
  }

  async function memorySave() {
    const port = validatePort();
    if (port === null) {
      focusField(els.port);
      return;
    }
    const server = readServer();
    if (!server.host) {
      toast("Enter a host before saving to memory.", "error");
      focusField(els.host);
      return;
    }
    try {
      const status = await invoke("memory_status");
      let storeCredentials = null;
      if (server.user && status && status.store_credentials == null) {
        els.dlgMemoryPath.textContent = (state.info && state.info.memory_path) || "the memory file";
        const answer = await askDialog(els.dlgMemory);
        if (answer !== "store" && answer !== "dont") return; // Esc: abort the save
        storeCredentials = answer === "store";
      }
      await invoke("memory_save", {
        server: {
          host: server.host,
          port,
          mode: server.mode,
          user: server.user,
          password: server.password,
          directory: server.directory,
          mkdir: server.mkdir,
        },
        store_credentials: storeCredentials,
      });
      const answered = storeCredentials !== null ? storeCredentials : status && status.store_credentials;
      const withoutLogin = Boolean(server.user) && answered === false;
      toast(withoutLogin ? "Saved to memory, without the login" : "Saved to memory");
    } catch (e) {
      toastError("Could not save to memory", e);
    }
  }

  async function memoryRecall() {
    try {
      const saved = await invoke("memory_recall");
      if (!saved) {
        toast("Nothing is saved in memory");
        return;
      }
      els.host.value = saved.host || "";
      els.port.value = String(saved.port == null ? 21 : saved.port);
      setMode(saved.mode === "active" ? "active" : "passive");
      els.directory.value = saved.directory || "";
      els.mkdir.checked = Boolean(saved.mkdir);
      if (saved.user != null) {
        els.user.value = saved.user;
        if (saved.user === "") els.password.value = "";
      }
      if (saved.password != null) els.password.value = saved.password;
      validatePort();
      updateSubmitState();
      toast("Recalled from memory");
    } catch (e) {
      toastError("Could not recall from memory", e);
    }
  }

  async function memoryClear() {
    try {
      await invoke("memory_clear");
      toast("Memory cleared");
    } catch (e) {
      toastError("Could not clear memory", e);
    }
  }

  /* ------------------------------------------------------ transfer: start */

  function newJob(config) {
    return {
      archiveName: basename(config.archive),
      cursor: 0, // next log sequence to fetch
      pollSeq: 0, // id of the latest poll request
      answeredSeq: -1, // polls up to this id predate our last password answer
      answering: false,
      cancelRequested: false,
      failures: 0,
      finished: false, // a snapshot with phase "finished" was rendered
      done: false, // the polling loop has ended
      waiters: [],
      chipsKey: "",
      srKey: "",
      hadProgress: false, // a snapshot with progress numbers was rendered
    };
  }

  async function startTransfer() {
    if (state.starting) return;
    if (!state.archive || !els.host.value.trim()) return;
    const config = buildConfig();
    if (!config) return;
    state.starting = true;
    updateSubmitState();
    try {
      await invoke("start_transfer", { config });
    } catch (e) {
      toastError("Could not start the upload", e);
      return;
    } finally {
      state.starting = false;
      updateSubmitState();
    }
    const job = newJob(config);
    state.job = job;
    resetTransferView(job, config);
    showView("transfer");
    pollLoop(job);
  }

  function resetTransferView(job, config) {
    setText(els.tArchive, job.archiveName);
    setTitle(els.tArchive, config.archive);
    setHidden(els.tArrow, true);
    setHidden(els.tTarget, true);
    setText(els.tTarget, "");
    setText(els.tSub, `${capitalize(config.mode)} mode · ${config.user || "anonymous"}`);

    setText(els.phaseText, "Reading the archive…");
    els.phaseDot.dataset.state = "";
    els.chips.replaceChildren();

    setHidden(els.currentBlock, false);
    setHidden(els.totalBlock, false);
    setHidden(els.statusRow, false);
    els.currentBlock.dataset.state = "";
    els.totalBlock.dataset.state = "";
    setText(els.curLabel, "File");
    setText(els.curName, "-");
    setTitle(els.curName, "");
    setBar(els.curBar, 0);
    setText(els.curStats, "");
    setBar(els.totalBar, 0);
    setText(els.totalStats, "");
    setText(els.totalFiles, "");

    setText(els.statUploadValue, "-");
    setText(els.statUnpackValue, "-");
    setText(els.statBufferValue, "-");
    setMeter(els.bufferMeter, 0);
    setText(els.statElapsedValue, "-");

    els.log.replaceChildren();
    state.logStick = true;
    setHidden(els.btnLatest, true);

    setHidden(els.tActions, false);
    els.btnCancel.disabled = false;
    els.btnCancel.textContent = "Cancel";
    setHidden(els.result, true);
    els.problemsList.replaceChildren();
    els.problems.open = false;
  }

  /* ----------------------------------------------------- transfer: polling */

  async function pollLoop(job) {
    let finalPass = false;
    try {
      while (state.job === job) {
        const seq = ++job.pollSeq;
        let snap;
        try {
          snap = await invoke("poll_transfer", { cursor: job.cursor });
          job.failures = 0;
        } catch (e) {
          if (state.job !== job) return;
          const message = errText(e);
          job.failures += 1;
          if (/no transfer/i.test(message) || job.failures >= POLL_MAX_FAILURES) {
            jobLost(job, message);
            return;
          }
          toastError("Lost contact with the transfer", e);
          await sleep(Math.min(2000, POLL_INTERVAL_MS * job.failures));
          continue;
        }
        if (state.job !== job) return;
        render(job, snap, seq);
        if (snap.phase === "finished") {
          if (finalPass) return;
          finalPass = true;
          await sleep(60); // one more poll, to flush any log lines written last
          continue;
        }
        await sleep(POLL_INTERVAL_MS);
      }
    } finally {
      job.done = true;
      for (const resolve of job.waiters.splice(0)) resolve();
    }
  }

  function waitForDone(job, timeoutMs) {
    if (job.done) return Promise.resolve();
    return new Promise((resolve) => {
      job.waiters.push(resolve);
      setTimeout(resolve, timeoutMs);
    });
  }

  // The backend no longer knows the job (or stopped answering): show a local failure.
  function jobLost(job, message) {
    if (!job.finished) {
      finishJob(job, {
        status: "failed",
        error: message,
        summary: [`FAILED: ${message}`],
        problems: [],
        problems_dropped: 0,
      });
    }
  }

  /* --------------------------------------------------- transfer: rendering */

  function ratio(done, total) {
    return total > 0 ? Math.max(0, Math.min(1, done / total)) : 0;
  }

  function formatPercent(r) {
    return `${(r * 100).toFixed(1)}%`;
  }

  function etaText(text, seconds) {
    return seconds >= 0 && text ? text : UNKNOWN_TIME;
  }

  function setBar(bar, r) {
    const fill = bar.firstElementChild;
    const width = `${(r * 100).toFixed(1)}%`;
    if (fill.style.width !== width) {
      fill.style.width = width;
      bar.setAttribute("aria-valuenow", String(Math.round(r * 100)));
    }
  }

  function setMeter(meter, r) {
    const fill = meter.firstElementChild;
    const width = `${(r * 100).toFixed(0)}%`;
    if (fill.style.width !== width) fill.style.width = width;
  }

  function phaseLabel(snap) {
    if (snap.phase === "finished") return "Finished";
    if (snap.cancelling) return "Cancelling…";
    switch (snap.phase) {
      case "reading":
        return snap.prompt ? "Waiting for the archive password…" : "Reading the archive…";
      case "connecting":
        return "Connecting…";
      case "checking":
        return "Checking files already on the server…";
      case "transferring":
        return "Uploading…";
      default:
        return String(snap.phase);
    }
  }

  function render(job, snap, seq) {
    if (snap.progress) job.hadProgress = true;
    renderHeader(job, snap);
    renderPhase(job, snap);
    renderCurrent(snap);
    renderTotal(job, snap);
    renderStatus(snap);
    renderLog(job, snap.log);
    renderPrompt(job, snap, seq);
    renderCancel(job, snap);
    if (snap.phase === "finished" && !job.finished) finishJob(job, snap.result, snap);
  }

  function renderHeader(job, snap) {
    setText(els.tArchive, (snap.archive && snap.archive.name) || job.archiveName);
    if (snap.target) {
      setText(els.tTarget, snap.target);
      setTitle(els.tTarget, snap.target);
      setHidden(els.tTarget, false);
      setHidden(els.tArrow, false);
    }
    if (snap.mode && snap.user) setText(els.tSub, `${capitalize(snap.mode)} mode · ${snap.user}`);
  }

  function renderPhase(job, snap) {
    let label = phaseLabel(snap);
    const key = `${snap.phase}|${Boolean(snap.cancelling)}|${Boolean(snap.prompt)}`;
    if (key !== job.srKey) {
      job.srKey = key;
      if (snap.phase !== "finished") announce(label);
    }
    if (snap.phase === "checking" && snap.probe && snap.probe.total > 0) {
      label += ` ${snap.probe.done}/${snap.probe.total}`;
    }
    setText(els.phaseText, label);
    renderChips(job, snap.archive);
  }

  function renderChips(job, archive) {
    const key = archive ? JSON.stringify(archive) : "";
    if (key === job.chipsKey) return;
    job.chipsKey = key;
    els.chips.replaceChildren();
    if (!archive) return;
    const add = (text) => els.chips.append(el("li", null, text));
    add(plural(archive.files, "file"));
    add(archive.bytes_text);
    add(plural(archive.volumes, "volume"));
    if (archive.solid) add("Solid");
    if (archive.encrypted) add("Encrypted");
  }

  function renderCurrent(snap) {
    const p = snap.progress;
    setHidden(els.currentBlock, snap.phase === "finished");
    if (!p) return;
    const t = p.text || {};
    const uploading = Boolean(p.current_file);
    const r = uploading ? ratio(p.current_sent, p.current_size) : 0;
    setText(els.curLabel, uploading ? `File ${p.current_number}/${p.total_files}` : "File");
    const name = uploading ? p.current_file : p.activity || "-";
    setText(els.curName, name);
    setTitle(els.curName, name);
    setBar(els.curBar, r);
    setText(
      els.curStats,
      uploading
        ? `${formatPercent(r)} · ${t.current_sent} / ${t.current_size} · ETA ${etaText(t.eta_file, p.eta_file)}`
        : ""
    );
  }

  function renderTotal(job, snap) {
    const p = snap.progress;
    if (!p) return;
    const t = p.text || {};
    let r = ratio(p.sent_bytes, p.total_bytes);
    if (p.total_bytes === 0 && snap.phase === "finished" && snap.result && snap.result.status === "success") r = 1;
    setBar(els.totalBar, r);
    const finished = snap.phase === "finished";
    setText(
      els.totalStats,
      finished
        ? `${formatPercent(r)} · ${t.sent_bytes} / ${t.total_bytes}`
        : `${formatPercent(r)} · ${t.sent_bytes} / ${t.total_bytes} · ETA ${etaText(t.eta_total, p.eta_total)}`
    );
    let files = `${p.files_done} of ${plural(p.total_files, "file")} uploaded`;
    if (p.files_skipped > 0) {
      files += ` · ${p.files_skipped} skipped, already on the server (${t.skipped_bytes})`;
    }
    setText(els.totalFiles, files);
  }

  function renderStatus(snap) {
    const p = snap.progress;
    if (!p) return;
    const t = p.text || {};
    setText(els.statUploadValue, t.average_rate || "-");
    setTitle(els.statUpload, t.upload_rate ? `Current: ${t.upload_rate}` : "");
    setText(els.statUnpackValue, t.unpack_rate || "-");
    const buffer = ratio(p.buffer_used, p.buffer_capacity);
    setText(els.statBufferValue, `${Math.round(buffer * 100)}%`);
    setMeter(els.bufferMeter, buffer);
    setTitle(
      els.statBuffer,
      `${t.buffer_capacity ? `Capacity ${t.buffer_capacity}. ` : ""}Full: the network is the bottleneck. Empty: the CPU is.`
    );
    setText(els.statElapsedValue, t.elapsed || "-");
  }

  function renderCancel(job, snap) {
    const cancelling = Boolean(snap.cancelling) || job.cancelRequested;
    els.btnCancel.disabled = cancelling;
    setText(els.btnCancel, cancelling ? "Cancelling…" : "Cancel");
  }

  /* --------------------------------------------------------- transfer: log */

  function logAtBottom() {
    return els.log.scrollHeight - els.log.scrollTop - els.log.clientHeight < 12;
  }

  function scrollLogToBottom() {
    els.log.scrollTop = els.log.scrollHeight;
  }

  function logLine(entry) {
    const level = LEVELS.includes(entry.level) ? entry.level : "info";
    const line = el("div", `line lvl-${level}${entry.time ? " timed" : ""}`);
    if (entry.time) line.append(el("span", "time", entry.time));
    line.append(document.createTextNode(entry.text == null ? "" : String(entry.text)));
    return line;
  }

  function renderLog(job, log) {
    if (!log) return;
    const lines = Array.isArray(log.lines) ? log.lines : [];
    if (lines.length) {
      const fragment = document.createDocumentFragment();
      for (const entry of lines) fragment.append(logLine(entry));
      els.log.append(fragment);
      for (let excess = els.log.childElementCount - LOG_MAX_LINES; excess > 0; excess--) {
        els.log.firstElementChild.remove();
      }
      if (state.logStick) scrollLogToBottom();
    }
    if (typeof log.next === "number") job.cursor = log.next;
  }

  /* ------------------------------------------------- transfer: RAR password */

  function setPasswordError(message) {
    els.pwError.textContent = message || "";
    setHidden(els.pwError, !message);
    if (message) els.pwInput.setAttribute("aria-invalid", "true");
    else els.pwInput.removeAttribute("aria-invalid");
  }

  function renderPrompt(job, snap, seq) {
    const prompt = snap.prompt;
    const wanted = prompt && prompt.kind === "rar_password";
    if (!wanted) {
      if (els.dlgPassword.open && !job.answering) els.dlgPassword.close();
      return;
    }
    if (els.dlgPassword.open) {
      setPasswordError(prompt.error);
      return;
    }
    // Ignore the prompt while our answer is in flight, and in snapshots of polls that were
    // requested before the answer landed: they still describe the prompt we just answered.
    if (job.answering || seq <= job.answeredSeq) return;
    els.pwText.replaceChildren(el("strong", null, prompt.archive || job.archiveName), " is encrypted.");
    els.pwInput.value = "";
    setPasswordError(prompt.error);
    els.dlgPassword.showModal();
    els.pwInput.focus();
  }

  async function answerPassword(password) {
    const job = state.job;
    if (!job || job.answering) return;
    job.answering = true;
    els.dlgPassword.close();
    els.pwInput.value = "";
    try {
      await invoke("answer_password", { password });
    } catch (e) {
      toastError("Could not send the password", e);
    } finally {
      job.answeredSeq = job.pollSeq;
      job.answering = false;
    }
  }

  /* --------------------------------------------------- transfer: cancel/end */

  async function cancelTransfer() {
    const job = state.job;
    if (!job || job.finished) return;
    job.cancelRequested = true;
    els.btnCancel.disabled = true;
    setText(els.btnCancel, "Cancelling…");
    try {
      await invoke("cancel_transfer");
    } catch (e) {
      job.cancelRequested = false;
      els.btnCancel.disabled = false;
      setText(els.btnCancel, "Cancel");
      toastError("Could not cancel", e);
    }
  }

  const RESULT_TITLES = { success: "Done", failed: "Failed", cancelled: "Cancelled" };

  function finishJob(job, result, snap) {
    job.finished = true;
    const res = result || {
      status: "failed",
      error: "The transfer ended without a result.",
      summary: [],
      problems: [],
      problems_dropped: 0,
    };
    const status = RESULT_TITLES[res.status] ? res.status : "failed";

    if (els.dlgPassword.open) els.dlgPassword.close();
    els.phaseDot.dataset.state = status;
    els.totalBlock.dataset.state = status;
    setHidden(els.currentBlock, true);
    // Failed before the transfer started: there are no numbers worth showing.
    setHidden(els.totalBlock, !job.hadProgress);
    setHidden(els.statusRow, !job.hadProgress);
    setText(els.phaseText, "Finished");
    if (snap) renderTotal(job, snap); // final numbers, without the ETA

    els.resultBanner.dataset.status = status;
    setText(els.resultTitle, RESULT_TITLES[status]);
    const showError = status !== "success" && Boolean(res.error);
    setText(els.resultError, showError ? res.error : "");
    setHidden(els.resultError, !showError);

    // The banner already shows the error; the summary repeats it as "FAILED: <error>".
    const summary = (res.summary || []).filter((line) => !(res.error && line === `FAILED: ${res.error}`));
    els.resultSummary.replaceChildren(...summary.map((line) => el("div", null, line)));
    setHidden(els.resultSummary, summary.length === 0);

    const problems = Array.isArray(res.problems) ? res.problems : [];
    const dropped = res.problems_dropped || 0;
    els.problemsList.replaceChildren(
      ...problems.map((entry) => {
        const level = entry.level === "error" ? "error" : "warn";
        const row = el("div", `line lvl-${level}${entry.time ? " timed" : ""}`);
        if (entry.time) row.append(el("span", "time", entry.time));
        row.append(document.createTextNode(`${level === "error" ? "Error" : "Warning"}: ${entry.text}`));
        return row;
      })
    );
    setText(els.problemsSummary, `Warnings and errors (${problems.length + dropped})`);
    setHidden(els.problemsDropped, dropped === 0);
    setText(
      els.problemsDropped,
      dropped === 0 ? "" : `${plural(dropped, "earlier message")} not shown.`
    );
    setHidden(els.problems, problems.length === 0 && dropped === 0);

    setHidden(els.tActions, true);
    setHidden(els.result, false);
    announce(status === "failed" && res.error ? `Failed: ${res.error}` : RESULT_TITLES[status]);
    if (!anyDialogOpen() && !state.quitting) els.btnNew.focus({ preventScroll: true });
  }

  async function newTransfer() {
    els.btnNew.disabled = true;
    try {
      await invoke("close_transfer");
    } catch (e) {
      toastError("Could not release the finished transfer", e);
    } finally {
      els.btnNew.disabled = false;
    }
    state.job = null;
    showView("setup");
    updateSubmitState();
    els.btnUpload.focus({ preventScroll: true });
  }

  /* --------------------------------------------------------- window close */

  function transferRunning() {
    return Boolean(state.job) && !state.job.finished;
  }

  async function quitNow(win) {
    state.quitting = true;
    els.app.inert = true;
    toast("Cancelling the upload…");
    const job = state.job;
    try {
      if (job && !job.finished) {
        job.cancelRequested = true;
        await invoke("cancel_transfer");
        await waitForDone(job, QUIT_WAIT_MS);
      }
    } catch (e) {
      toastError("Could not cancel the upload", e);
    }
    try {
      await invoke("close_transfer");
    } catch (e) {
      toastError("Could not release the transfer", e);
    }
    try {
      await win.destroy();
    } catch (e) {
      state.quitting = false;
      els.app.inert = false;
      toastError("Could not close the window", e);
    }
  }

  async function initCloseHandler() {
    try {
      const win = tauri.window.getCurrentWindow();
      let asking = false;
      await win.onCloseRequested(async (event) => {
        if (state.quitting) {
          event.preventDefault();
          return;
        }
        if (!transferRunning()) return; // nothing to lose: close normally
        event.preventDefault();
        if (asking) return;
        asking = true;
        try {
          const answer = await askDialog(els.dlgQuit);
          if (answer === "quit") await quitNow(win);
        } finally {
          asking = false;
        }
      });
    } catch (e) {
      toastError("Could not watch for window close", e);
    }
  }

  /* ------------------------------------------------------------ wiring up */

  function bindEvents() {
    els.setupView.addEventListener("submit", (event) => {
      event.preventDefault();
      startTransfer();
    });
    els.host.addEventListener("input", updateSubmitState);
    els.port.addEventListener("input", validatePort);
    els.buffer.addEventListener("input", validateBuffer);
    els.btnChoose.addEventListener("click", chooseArchive);

    els.memSave.addEventListener("click", () => runMemoryAction(memorySave));
    els.memRecall.addEventListener("click", () => runMemoryAction(memoryRecall));
    els.memClear.addEventListener("click", () => runMemoryAction(memoryClear));

    els.btnCancel.addEventListener("click", cancelTransfer);
    els.btnNew.addEventListener("click", newTransfer);

    // Following the log stops only when the user scrolls up (wheel, touch, keys or the
    // scrollbar), never because of layout changes or trimmed lines. It resumes at the bottom.
    let pointerDown = false;
    let userScrollUntil = 0;
    const markUser = () => {
      userScrollUntil = performance.now() + 800;
    };
    for (const type of ["wheel", "touchmove", "keydown"]) {
      els.log.addEventListener(type, markUser, { passive: true });
    }
    els.log.addEventListener("pointerdown", () => {
      pointerDown = true;
      markUser();
    });
    window.addEventListener("pointerup", () => {
      pointerDown = false;
    });
    els.log.addEventListener("scroll", () => {
      if (logAtBottom()) state.logStick = true;
      else if (pointerDown || performance.now() < userScrollUntil) state.logStick = false;
      setHidden(els.btnLatest, state.logStick);
    });
    els.btnLatest.addEventListener("click", () => {
      state.logStick = true;
      scrollLogToBottom();
      setHidden(els.btnLatest, true);
      els.log.focus({ preventScroll: true });
    });
    if (typeof ResizeObserver === "function") {
      new ResizeObserver(() => {
        if (state.logStick) scrollLogToBottom();
      }).observe(els.log);
    }

    els.pwForm.addEventListener("submit", (event) => {
      event.preventDefault();
      answerPassword(els.pwInput.value);
    });
    els.pwCancel.addEventListener("click", () => answerPassword(null));
    els.dlgPassword.addEventListener("cancel", (event) => {
      event.preventDefault(); // Esc: same as the Cancel button
      answerPassword(null);
    });
  }

  async function loadAppInfo() {
    try {
      state.info = await invoke("app_info");
      const gui = state.info.gui_version || (/\d+\.\d+\.\d+\S*/.exec(state.info.version || "") || [""])[0];
      setText(els.version, gui ? `v${gui}` : "");
      setTitle(els.version, state.info.version || "");
    } catch (e) {
      toastError("Could not read the application info", e);
    }
  }

  async function init() {
    if (!tauri || !tauri.core) {
      toast("The Tauri API is not available, so this page cannot talk to rarftp.", "error");
      els.btnChoose.disabled = true;
      els.btnUpload.disabled = true;
      return;
    }
    bindEvents();
    showView("setup");
    validatePort();
    updateSubmitState();
    updateMemoryButtons();
    initDragDrop();
    initCloseHandler();
    await Promise.all([loadAppInfo(), refreshMemory()]);
  }

  init();
})();
