"use strict";

/*
 * StreamExtract frontend. Plain JS, no dependencies. Talks to the Rust backend through
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
  // What the file dialog offers; .001 is the first part of a split ZIP, 7z or tar archive.
  const ARCHIVE_EXTENSIONS = ["rar", "zip", "7z", "tar", "exfat", "ffpfsc", "ffpfs", "pfs", "ufs", "ffpkg", "gz", "tgz", "bz2", "tbz2", "tbz", "xz", "txz", "lzma",
    "zst", "tzst", "lz4", "001"];

  const $ = (id) => document.getElementById(id);
  const els = {
    app: $("app"),
    version: $("app-version"),
    unitsGroup: document.querySelectorAll('input[name="units"]'),
    completionSound: $("completion-sound"),
    checkUpdates: $("check-updates"),
    passwordVisibility: [$("archive-password-visible"), $("password-visible"), $("pw-input-visible")],
    // setup
    setupView: $("setup-view"),
    dropzone: $("dropzone"),
    dzTitle: $("dz-title"),
    dzSub: $("dz-sub"),
    btnChoose: $("btn-choose"),
    archiveHint: $("archive-hint"),
    archiveList: $("archive-list"),
    batchBlock: $("batch-block"),
    batchCount: $("batch-count"),
    batchPercent: $("batch-percent"),
    batchIssues: $("batch-issues"),
    batchBar: $("batch-bar"),
    batchStats: $("batch-stats"),
    batchList: $("batch-list"),
    archivePassword: $("archive-password"),
    host: $("host"),
    hostRow: $("host-row"),
    port: $("port"),
    protocol: $("protocol"),
    sshButton: $("btn-ssh"),
    sshDialog: $("dlg-ssh"),
    privateKey: $("private-key"),
    privateKeyPassphrase: $("private-key-passphrase"),
    privateKeyButton: $("btn-private-key"),
    verifyHostKey: $("verify-host-key"),
    knownHosts: $("known-hosts"),
    knownHostsButton: $("btn-known-hosts"),
    portError: $("port-error"),
    mode: $("mode"),
    modeField: $("mode-field"),
    user: $("user"),
    password: $("password"),
    directory: $("directory"),
    mkdir: $("mkdir"),
    advanced: $("advanced"),
    buffer: $("buffer"),
    bufferError: $("buffer-error"),
    retries: $("retries"),
    retriesError: $("retries-error"),
    preventSleep: $("prevent-sleep"),
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
    archives: [], // absolute paths in batch order
    info: null, // app_info()
    memory: { saved: false, store_credentials: null },
    memoryBusy: false,
    units: "si",
    bufferMib: 64,
    retries: 3,
    preventSleep: true,
    completionSound: true,
    showPasswords: false,
    checkUpdates: null, // no GitHub request until the user chooses
    preferencesLoaded: false,
    updateCheckStarted: false,
    preferencesBusy: true,
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

  function syncPreferences() {
    for (const input of els.unitsGroup) {
      input.checked = input.value === state.units;
      input.disabled = state.preferencesBusy;
    }
    for (const input of els.passwordVisibility) {
      input.disabled = state.preferencesBusy;
      input.checked = state.showPasswords;
    }
    for (const input of [els.archivePassword, els.password, els.pwInput, els.privateKeyPassphrase]) {
      input.type = state.showPasswords ? "text" : "password";
    }
    els.buffer.disabled = state.preferencesBusy;
    els.retries.disabled = state.preferencesBusy;
    els.preventSleep.disabled = state.preferencesBusy;
    els.preventSleep.checked = state.preventSleep;
    els.completionSound.disabled = state.preferencesBusy;
    els.completionSound.checked = state.completionSound;
    els.checkUpdates.disabled = state.preferencesBusy;
    els.checkUpdates.checked = state.checkUpdates === true;
    els.buffer.value = String(state.bufferMib);
    validateBuffer();
    els.retries.value = String(state.retries);
    validateRetries();
    updateSubmitState();
  }

  async function loadPreferences() {
    try {
      const preferences = await invoke("preferences_load");
      state.units = preferences.units;
      state.bufferMib = preferences.buffer_mib;
      state.retries = preferences.retries ?? 3;
      state.preventSleep = preferences.prevent_sleep ?? true;
      state.completionSound = preferences.completion_sound ?? true;
      state.showPasswords = preferences.show_passwords ?? false;
      state.checkUpdates = preferences.check_updates ?? null;
      state.preferencesLoaded = true;
    } catch (e) {
      toastError("Could not read preferences", e);
    } finally {
      state.preferencesBusy = false;
      syncPreferences();
    }
  }

  async function savePreferences(units, bufferMib, completionSound = state.completionSound,
    showPasswords = state.showPasswords, retries = state.retries, checkUpdates = state.checkUpdates,
    preventSleep = state.preventSleep) {
    if (state.preferencesBusy) return false;
    state.preferencesBusy = true;
    updateSubmitState();
    for (const input of els.unitsGroup) input.disabled = true;
    els.buffer.disabled = true;
    els.retries.disabled = true;
    els.preventSleep.disabled = true;
    els.completionSound.disabled = true;
    els.checkUpdates.disabled = true;
    for (const input of els.passwordVisibility) input.disabled = true;
    try {
      await invoke("preferences_save", { preferences: {
        units, buffer_mib: bufferMib, completion_sound: completionSound, show_passwords: showPasswords,
        retries, check_updates: checkUpdates, prevent_sleep: preventSleep,
      } });
      state.units = units;
      state.bufferMib = bufferMib;
      state.retries = retries;
      state.preventSleep = preventSleep;
      state.completionSound = completionSound;
      state.showPasswords = showPasswords;
      state.checkUpdates = checkUpdates;
      return true;
    } catch (e) {
      toastError("Could not save preferences", e);
      return false;
    } finally {
      state.preferencesBusy = false;
      syncPreferences();
    }
  }

  function saveUnits(event) {
    return savePreferences(event.target.value, state.bufferMib);
  }

  function saveBuffer() {
    const bufferMib = validateBuffer();
    if (bufferMib === null) return;
    return savePreferences(state.units, bufferMib);
  }

  function saveCompletionSound() {
    return savePreferences(state.units, state.bufferMib, els.completionSound.checked);
  }

  function saveRetries() {
    const retries = validateRetries();
    if (retries === null) return;
    return savePreferences(state.units, state.bufferMib, state.completionSound, state.showPasswords, retries);
  }

  function savePasswordVisibility(event) {
    return savePreferences(state.units, state.bufferMib, state.completionSound, event.target.checked);
  }

  function savePreventSleep(event) {
    return savePreferences(state.units, state.bufferMib, state.completionSound,
      state.showPasswords, state.retries, state.checkUpdates, event.target.checked);
  }

  /* --------------------------------------------------------- release checks */

  async function saveUpdateChecks() {
    const saved = await savePreferences(state.units, state.bufferMib, state.completionSound,
      state.showPasswords, state.retries, els.checkUpdates.checked);
    if (!saved) return;
    try {
      await invoke("dismiss_update_prompt");
    } catch (error) {
      console.warn("Could not close the update window", error);
    }
    if (state.checkUpdates) void checkForUpdates();
  }

  async function initUpdateChecks() {
    // An unreadable file is not consent to contact GitHub or overwrite a saved choice.
    if (!state.preferencesLoaded) return;
    try {
      if (state.checkUpdates === null) {
        const enabled = await invoke("ask_update_consent");
        // The separate window leaves Advanced usable. Preserve any choice made there,
        // and wait for an in-flight preference save before saving the consent answer.
        while (state.preferencesBusy && !state.quitting) await sleep(20);
        if (state.quitting) return;
        if (state.checkUpdates === null) {
          await savePreferences(state.units, state.bufferMib, state.completionSound,
            state.showPasswords, state.retries, enabled);
        }
      }
      void checkForUpdates();
    } catch (error) {
      console.warn("Could not ask about update checks", error);
    }
  }

  async function checkForUpdates() {
    if (state.checkUpdates !== true || state.updateCheckStarted) return;
    state.updateCheckStarted = true;
    try {
      const version = await invoke("check_for_updates");
      if (!version || state.checkUpdates !== true || state.quitting) return;
      await invoke("show_update_available");
    } catch (error) {
      console.warn("Could not check for updates", error);
    }
  }

  let completionAudioContext = null;
  let completionAudioBuffer = null;
  let completionAudioUnlocked = false;

  function unlockCompletionSound() {
    if (completionAudioUnlocked) return;
    try {
      if (!completionAudioContext) {
        completionAudioContext = new (window.AudioContext || window.webkitAudioContext)();
      }
      // Some WebViews only grant background audio to a context that actually starts a source
      // during the user's Upload action. A source with no buffer outputs silence.
      const source = completionAudioContext.createBufferSource();
      source.connect(completionAudioContext.destination);
      source.start();
      source.stop();
      completionAudioContext.resume().catch((error) => console.warn("Could not unlock the completion sound", error));
      completionAudioUnlocked = true;
    } catch (error) {
      // Audio support must never prevent a transfer from starting.
      console.warn("Could not unlock the completion sound", error);
    }
  }

  async function prepareCompletionSound() {
    if (!completionAudioContext) {
      completionAudioContext = new (window.AudioContext || window.webkitAudioContext)();
    }
    // Resume during the Upload button gesture so completion can sound in the background.
    const resumed = completionAudioContext.resume();
    if (!completionAudioBuffer) {
      completionAudioBuffer = fetch("assets/audio/completed.mp3")
        .then((response) => {
          if (!response.ok) throw new Error("Could not load the completion sound");
          return response.arrayBuffer();
        })
        .then((data) => completionAudioContext.decodeAudioData(data))
        .catch((error) => {
          completionAudioBuffer = null;
          throw error;
        });
    }
    const [, buffer] = await Promise.all([resumed, completionAudioBuffer]);
    return buffer;
  }

  function playCompletionSound() {
    if (!state.completionSound || state.quitting) return;
    prepareCompletionSound().then((buffer) => {
      if (!state.completionSound || state.quitting) return;
      const source = completionAudioContext.createBufferSource();
      source.buffer = buffer;
      source.connect(completionAudioContext.destination);
      source.start();
    }).catch((error) => console.warn("Could not play the completion sound", error));
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
    return els.mode.value === "active" ? "active" : "passive";
  }

  function setMode(mode) {
    els.mode.value = mode;
  }

  function defaultPort(protocol) {
    return protocol === "sftp" ? 22 : protocol === "ftps_implicit" ? 990 : 21;
  }

  function updateProtocolFields() {
    const ssh = els.protocol.value === "sftp";
    els.mode.disabled = ssh;
    setHidden(els.modeField, ssh);
    els.hostRow.classList.toggle("sftp", ssh);
    els.user.placeholder = ssh ? "current local username" : "anonymous";
    setHidden(els.sshButton, !ssh);
    els.knownHosts.disabled = !els.verifyHostKey.checked;
    els.knownHostsButton.disabled = !els.verifyHostKey.checked;
  }

  async function chooseSshFile(input) {
    try {
      const file = await tauri.dialog.open({ multiple: false, directory: false });
      if (typeof file === "string") input.value = file;
    } catch (e) { toastError("Could not choose a file", e); }
  }

  function changeProtocol() {
    const previous = els.protocol.dataset.previous || "ftp";
    if (els.port.value.trim() === String(defaultPort(previous))) {
      els.port.value = String(defaultPort(els.protocol.value));
    }
    els.protocol.dataset.previous = els.protocol.value;
    updateProtocolFields();
    validatePort();
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
  const validateRetries = () =>
    parseIntInRange(els.retries, els.retriesError, 1, 4294967295,
      "Upload attempts must be a whole number between 1 and 4294967295.");

  function updateSubmitState() {
    const host = els.host.value.trim();
    const hasArchive = state.archives.length > 0;
    els.btnUpload.disabled = !(hasArchive && host) || state.starting || state.preferencesBusy;
    let hint;
    if (!hasArchive && !host) hint = "Choose an archive and enter a host to continue.";
    else if (!hasArchive) hint = "Choose an archive to continue.";
    else if (!host) hint = "Enter a host to continue.";
    else hint = `${plural(state.archives.length, "archive")} → ${host}`;
    els.btnChoose.disabled = state.starting;
    setText(els.submitHint, hint);
  }

  function renderArchiveList() {
    const paths = state.archives;
    const count = paths.length;
    els.dropzone.dataset.state = count ? "chosen" : "empty";
    setText(els.dzTitle, count ? `${plural(count, "archive")} queued` : "Drop archives here");
    setText(els.dzSub, "For multi-volume sets, use the first volume (.part1.rar, zip.001, etc.).");
    els.archiveList.replaceChildren(...paths.map((path, index) => {
      const row = el("li", "archive-row");
      const details = el("div", "archive-details selectable");
      details.title = path;
      details.append(el("div", "archive-name", basename(path)), el("div", "archive-path", path));
      const remove = el("button", "btn btn-small", "Remove File");
      remove.type = "button";
      remove.disabled = state.starting;
      remove.setAttribute("aria-label", `Remove ${basename(path)}`);
      remove.addEventListener("click", () => removeArchive(index));
      row.append(details, remove);
      return row;
    }));
    setHidden(els.archiveList, count === 0);
    const unusual = paths.some((path) => !new RegExp(`\\.(${ARCHIVE_EXTENSIONS.join("|")})$`, "i").test(path));
    setText(els.archiveHint, unusual
      ? "Some files do not look like archives or filesystem images. StreamExtract will still try to read them." : "");
    setHidden(els.archiveHint, !unusual);
    updateSubmitState();
  }

  function addArchives(paths) {
    if (state.starting || state.view !== "setup") return;
    for (const selected of paths) {
      const path = selected && typeof selected === "object" ? selected.path : selected;
      if (typeof path === "string" && path && !state.archives.includes(path)) state.archives.push(path);
    }
    renderArchiveList();
  }

  function removeArchive(index) {
    if (state.starting || state.view !== "setup") return;
    state.archives.splice(index, 1);
    renderArchiveList();
    els.btnChoose.focus({ preventScroll: true });
  }

  async function chooseArchive() {
    try {
      const selected = await tauri.dialog.open({
        multiple: true,
        directory: false,
        filters: [{ name: "Archives and filesystem images", extensions: ARCHIVE_EXTENSIONS }],
      });
      if (selected) addArchives(Array.isArray(selected) ? selected : [selected]);
    } catch (e) {
      toastError("Could not open the file dialog", e);
    }
  }

  function readServer() {
    return {
      host: els.host.value.trim(),
      protocol: els.protocol.value || "ftp",
      private_key: els.privateKey.value.trim() || null,
      private_key_passphrase: els.privateKeyPassphrase.value || null,
      known_hosts: els.verifyHostKey.checked ? els.knownHosts.value.trim() : null,
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
    const retries = validateRetries();
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
    if (retries === null) {
      els.advanced.open = true;
      focusField(els.retries);
      return null;
    }
    return {
      archive: state.archives[0],
      archive_password: els.archivePassword.value === "" ? null : els.archivePassword.value,
      host: server.host,
      protocol: server.protocol,
      private_key: server.private_key,
      private_key_passphrase: server.private_key_passphrase,
      known_hosts: server.known_hosts,
      port,
      mode: server.mode,
      user: server.user,
      password: server.password,
      directory: server.directory,
      mkdir: server.mkdir,
      verbose: els.verbose.checked,
      buffer_mib: buffer,
      retries,
      prevent_sleep: state.preventSleep,
      units: state.units,
    };
  }

  /* --------------------------------------------------- setup: drag and drop */

  function setDragging(on) {
    els.dropzone.classList.toggle("drag", on && state.view === "setup");
  }

  function anyDialogOpen() {
    return els.dlgMemory.open || els.dlgPassword.open || els.dlgQuit.open || els.sshDialog.open;
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
            if (payload.paths && payload.paths.length) addArchives(payload.paths);
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
      const hasCredentials = Boolean(server.user || (server.protocol === "sftp" && (server.password || server.private_key_passphrase)));
      if (hasCredentials && status && status.store_credentials == null) {
        els.dlgMemoryPath.textContent = (state.info && state.info.memory_path) || "the memory file";
        const answer = await askDialog(els.dlgMemory);
        if (answer !== "store" && answer !== "dont") return; // Esc: abort the save
        storeCredentials = answer === "store";
      }
      await invoke("memory_save", {
        server: {
          host: server.host,
          protocol: server.protocol,
          private_key: server.private_key,
          private_key_passphrase: server.private_key_passphrase,
          known_hosts: server.known_hosts,
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
      const withoutLogin = hasCredentials && answered === false;
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
      els.protocol.value = ["ftps_explicit", "ftps_implicit", "sftp"].includes(saved.protocol) ? saved.protocol : "ftp";
      els.protocol.dataset.previous = els.protocol.value;
      els.privateKey.value = saved.private_key || "";
      els.privateKeyPassphrase.value = saved.private_key_passphrase || "";
      els.verifyHostKey.checked = saved.known_hosts != null;
      els.knownHosts.value = saved.known_hosts || "";
      updateProtocolFields();
      els.port.value = String(saved.port == null ? defaultPort(els.protocol.value) : saved.port);
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
      config,
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
    if (state.starting || state.preferencesBusy) return;
    if (!state.archives.length || !els.host.value.trim()) return;
    const config = buildConfig();
    if (!config) return;
    if (state.completionSound) {
      unlockCompletionSound();
      prepareCompletionSound().catch((error) => console.warn("Could not prepare the completion sound", error));
    }
    state.starting = true;
    renderArchiveList();
    const archives = [...state.archives];
    try {
      await invoke("start_transfer", { config, archives });
    } catch (e) {
      toastError("Could not start the upload", e);
      requestAttention("failed");
      return;
    } finally {
      state.starting = false;
      renderArchiveList();
    }
    const job = newJob(config);
    state.job = job;
    requestAttention(null);
    updateWindowProgress({ phase: "reading" });
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
    setText(els.tSub, `${config.protocol === "sftp" ? "SFTP" : capitalize(config.mode) + " mode"} · ${config.user || (config.protocol === "sftp" ? "current local user" : "anonymous")}`);

    setHidden(els.batchBlock, true);
    els.batchBlock.open = false;
    els.batchList.replaceChildren();
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
    // Red when empty, through yellow at half capacity, to green when full.
    fill.style.backgroundColor = `hsl(${r * 120}, 75%, 45%)`;
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

  // Each archive contributes an equal share. Archive sizes and uncompressed totals
  // are not all known up front, especially for streamed tar archives.
  function batchRatio(snap) {
    const batch = snap.batch;
    if (!batch || !batch.total) return 0;
    const p = snap.progress;
    const current = snap.phase === "transferring" && p
      ? (p.totals_known ? ratio(p.sent_bytes, p.total_bytes) : ratio(p.archive_read, p.archive_size)) : 0;
    return Math.min(1, (batch.completed + current) / batch.total);
  }

  const ARCHIVE_STATUSES = {
    waiting: "Waiting", running: "In progress", success: "Done", failed: "Failed",
    cancelled: "Cancelled", not_started: "Not started",
  };

  function renderBatch(job, snap) {
    const batch = snap.batch;
    setHidden(els.batchBlock, !batch || batch.total <= 1);
    if (!batch || batch.total <= 1) return;
    setText(els.batchCount, `${batch.completed}/${plural(batch.total, "archive")}`);
    const progress = batchRatio(snap);
    setBar(els.batchBar, progress);
    setText(els.batchPercent, formatPercent(progress));
    const counts = (status) => batch.items.filter((item) => item.status === status).length;
    const stats = [`${counts("success")} done`, `${counts("failed")} failed`];
    const failed = counts("failed");
    setText(els.batchIssues, failed ? `${failed} failed` : "");
    setHidden(els.batchIssues, failed === 0);
    if (counts("cancelled")) stats.push(`${counts("cancelled")} cancelled`);
    if (counts("not_started")) stats.push(`${counts("not_started")} not started`);
    stats.push("Progress by archive");
    setText(els.batchStats, stats.join(" · "));
    setTitle(els.batchCount, `${batch.completed} of ${plural(batch.total, "archive")} processed`);
    const key = JSON.stringify(batch.items.map((item) => [item.path, item.status]));
    if (job.batchKey === key) return;
    job.batchKey = key;
    els.batchList.replaceChildren(...batch.items.map((item) => {
      const row = el("li", "archive-row");
      row.dataset.status = item.status;
      const details = el("div", "archive-details selectable");
      details.title = item.path;
      details.append(el("div", "archive-name", basename(item.path)));
      row.append(details, el("span", "archive-status", ARCHIVE_STATUSES[item.status] || item.status));
      return row;
    }));
  }

  function render(job, snap, seq) {
    if (snap.progress) job.hadProgress = true;
    renderBatch(job, snap);
    renderHeader(job, snap);
    renderPhase(job, snap);
    renderCurrent(snap);
    renderTotal(job, snap);
    renderStatus(snap);
    renderLog(job, snap.log);
    renderPrompt(job, snap, seq);
    renderCancel(job, snap);
    if (!job.finished && snap.phase !== "finished") updateWindowProgress(snap);
    if (snap.phase === "finished" && !job.finished) finishJob(job, snap.result, snap);
  }

  function renderHeader(job, snap) {
    const path = snap.batch?.items[snap.batch.current_index]?.path;
    const batchFinished = snap.phase === "finished" && snap.batch?.total > 1;
    setText(els.tArchive, batchFinished ? `Batch of ${plural(snap.batch.total, "archive")}`
      : (snap.archive && snap.archive.name) || (path ? basename(path) : job.archiveName));
    if (batchFinished) setTitle(els.tArchive, "");
    else if (path) setTitle(els.tArchive, path);
    if (snap.target) {
      setText(els.tTarget, snap.target);
      setTitle(els.tTarget, snap.target);
      setHidden(els.tTarget, false);
      setHidden(els.tArrow, false);
    }
    if (snap.mode && snap.user) setText(els.tSub, `${job.config.protocol === "sftp" ? "SFTP" : capitalize(snap.mode) + " mode"} · ${snap.user}`);
  }

  function renderPhase(job, snap) {
    let label = phaseLabel(snap);
    if (snap.batch && snap.batch.total > 1 && snap.phase !== "finished") {
      label = `Archive ${snap.batch.current_index + 1}/${snap.batch.total} · ${label}`;
    }
    const key = `${snap.batch?.current_index ?? 0}|${snap.phase}|${Boolean(snap.cancelling)}|${Boolean(snap.prompt)}`;
    if (key !== job.srKey) {
      job.srKey = key;
      if (snap.phase !== "finished") announce(label);
    }
    if (snap.phase === "checking" && snap.probe && snap.probe.total > 0) {
      label += ` ${snap.probe.done}/${snap.probe.total}`;
    }
    setText(els.phaseText, label);
    renderChips(job, snap.phase === "finished" && snap.batch?.total > 1 ? null : snap.archive);
  }

  function renderChips(job, archive) {
    const key = `${state.units}|${archive ? JSON.stringify(archive) : ""}`;
    if (key === job.chipsKey) return;
    job.chipsKey = key;
    els.chips.replaceChildren();
    if (!archive) return;
    const add = (text) => els.chips.append(el("li", null, text));
    if (archive.format) add(archive.compression ? `${archive.format} (${archive.compression})` : archive.format);
    // A compressed tar is read as it is uploaded: its contents are not known up front.
    if (archive.files !== null) add(plural(archive.files, "file"));
    if (archive.bytes_text !== null) add(archive.bytes_text);
    add(plural(archive.volumes, "volume"));
    if (archive.solid) add("Solid");
    if (archive.encrypted) add("Encrypted");
  }

  function renderCurrent(snap) {
    const p = snap.progress;
    setHidden(els.currentBlock, snap.phase === "finished");
    if (!p) {
      setText(els.curLabel, "File");
      setText(els.curName, "-");
      setTitle(els.curName, "");
      setText(els.curStats, "");
      setBar(els.curBar, 0);
      return;
    }
    const t = p.text || {};
    const uploading = Boolean(p.current_file);
    const r = uploading ? ratio(p.current_sent, p.current_size) : 0;
    const number = p.totals_known ? `${p.current_number}/${p.total_files}` : `${p.current_number}`;
    setText(els.curLabel, uploading ? `File ${number}` : "File");
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
    if (!p) {
      setBar(els.totalBar, 0);
      setText(els.totalStats, "");
      setText(els.totalFiles, "");
      return;
    }
    const t = p.text || {};
    const finished = snap.phase === "finished";
    if (!p.totals_known) {
      // A streamed archive (compressed tar): no totals until its end, progress by position in the archive.
      const r = ratio(p.archive_read, p.archive_size);
      setBar(els.totalBar, r);
      setText(
        els.totalStats,
        `${formatPercent(r)} of the archive read · ${t.sent_bytes} uploaded · ETA ${etaText(t.eta_total, p.eta_total)}`
      );
      let files = `${plural(p.files_done, "file")} uploaded so far`;
      if (p.files_skipped > 0) {
        files += ` · ${p.files_skipped} skipped, already on the server (${t.skipped_bytes})`;
      }
      setText(els.totalFiles, files);
      return;
    }
    let r = ratio(p.sent_bytes, p.total_bytes);
    if (p.total_bytes === 0 && finished && snap.result && snap.result.status === "success") r = 1;
    setBar(els.totalBar, r);
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
    if (!p) {
      for (const node of [els.statUploadValue, els.statUnpackValue, els.statBufferValue, els.statElapsedValue]) {
        setText(node, "-");
      }
      setMeter(els.bufferMeter, 0);
      return;
    }
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
    setText(els.btnCancel, cancelling ? "Cancelling…" : snap.batch?.total > 1 ? "Cancel batch" : "Cancel");
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

  /* --------------------------------------------- transfer: archive password */

  function setPasswordError(message) {
    els.pwError.textContent = message || "";
    setHidden(els.pwError, !message);
    if (message) els.pwInput.setAttribute("aria-invalid", "true");
    else els.pwInput.removeAttribute("aria-invalid");
  }

  function renderPrompt(job, snap, seq) {
    const prompt = snap.prompt;
    const wanted = prompt && prompt.kind === "archive_password";
    setText(els.pwCancel, snap.batch?.total > 1 ? "Skip archive" : "Cancel");
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

  let windowProgressKey = "";
  let windowProgressQueue = Promise.resolve();

  function updateWindowProgress(snap) {
    const statuses = tauri.window.ProgressBarStatus;
    const p = snap.progress;
    let status = statuses.Indeterminate;
    let progress = 0;
    if (snap.phase === "finished") {
      status = statuses.None;
    } else if (snap.batch && snap.batch.total > 1) {
      status = statuses.Normal;
      progress = Math.floor(batchRatio(snap) * 100);
    } else if (snap.phase === "transferring" && p) {
      const total = p.totals_known ? p.total_bytes : p.archive_size;
      const done = p.totals_known ? p.sent_bytes : p.archive_read;
      if (total > 0) {
        status = statuses.Normal;
        progress = Math.floor(ratio(done, total) * 100);
      }
    }
    const key = `${status}|${progress}`;
    if (key === windowProgressKey) return;
    windowProgressKey = key;
    // Serialize native updates so a slow call cannot restore progress after completion.
    windowProgressQueue = windowProgressQueue.then(() =>
      tauri.window.getCurrentWindow().setProgressBar({ status, progress })
    ).catch(() => {
      if (windowProgressKey === key) windowProgressKey = "";
    });
  }

  function requestAttention(status) {
    if (status !== null && status !== "success" && status !== "failed") return;
    try {
      const win = tauri.window.getCurrentWindow();
      const type = {
        success: tauri.window.UserAttentionType.Informational,
        failed: tauri.window.UserAttentionType.Critical,
      };
      Promise.resolve(win.requestUserAttention(status === null ? null : type[status])).catch(() => {});
    } catch (e) {
      return;
    }
  }

  function finishJob(job, result, snap) {
    job.finished = true;
    updateWindowProgress({ phase: "finished" });
    const res = result || {
      status: "failed",
      error: "The transfer ended without a result.",
      summary: [],
      problems: [],
      problems_dropped: 0,
    };
    const status = RESULT_TITLES[res.status] ? res.status : "failed";
    const batch = snap?.batch?.total > 1;

    // A completed batch should not be accidentally submitted again from the setup view.
    // Keep a partially cancelled queue so its remaining archives can still be retried.
    if (snap?.batch && snap.batch.completed === snap.batch.total) {
      state.archives = [];
      renderArchiveList();
    }

    if (els.dlgPassword.open) els.dlgPassword.close();
    els.phaseDot.dataset.state = status;
    els.totalBlock.dataset.state = status;
    setHidden(els.currentBlock, true);
    // Failed before the transfer started: there are no numbers worth showing.
    setHidden(els.totalBlock, batch || !job.hadProgress);
    setHidden(els.statusRow, batch || !job.hadProgress);
    setText(els.phaseText, "Finished");
    if (snap) renderTotal(job, snap); // final numbers, without the ETA

    els.resultBanner.dataset.status = status;
    setText(els.resultTitle, batch
      ? ({ success: "Batch complete", failed: "Batch finished with errors", cancelled: "Batch cancelled" })[status]
      : RESULT_TITLES[status]);
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
    requestAttention(status);
    if (status === "success") playCompletionSound();
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
    toast("Cancelling the batch…");
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
      await win.onFocusChanged(({ payload: focused }) => {
        if (focused) requestAttention(null);
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
    els.protocol.addEventListener("change", changeProtocol);
    els.sshButton.addEventListener("click", () => els.sshDialog.showModal());
    els.privateKeyButton.addEventListener("click", () => chooseSshFile(els.privateKey));
    els.knownHostsButton.addEventListener("click", () => chooseSshFile(els.knownHosts));
    els.verifyHostKey.addEventListener("change", updateProtocolFields);
    updateProtocolFields();
    els.buffer.addEventListener("input", validateBuffer);
    els.buffer.addEventListener("change", saveBuffer);
    els.retries.addEventListener("input", validateRetries);
    els.retries.addEventListener("change", saveRetries);
    els.preventSleep.addEventListener("change", savePreventSleep);
    els.btnChoose.addEventListener("click", chooseArchive);
    for (const input of els.unitsGroup) input.addEventListener("change", saveUnits);
    els.completionSound.addEventListener("change", saveCompletionSound);
    els.checkUpdates.addEventListener("change", saveUpdateChecks);
    for (const input of els.passwordVisibility) input.addEventListener("change", savePasswordVisibility);

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
      toast("The Tauri API is not available, so this page cannot talk to StreamExtract.", "error");
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
    await Promise.all([loadAppInfo(), refreshMemory(), loadPreferences()]);
    void initUpdateChecks();
  }

  init();
})();
