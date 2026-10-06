const assert = require("node:assert/strict");
const { readFileSync } = require("node:fs");
const { test } = require("node:test");
const vm = require("node:vm");

function frontend(invoke = async () => ({ units: "si", buffer_mib: 64, completion_sound: true }), windowApi = {}, tauriApi = {}) {
  const inputs = ["si", "binary"].map((value) => ({ value, disabled: true, checked: value === "si" }));
  const sounds = [];
  const warnings = [];
  const soundUnlocks = [];
  class AudioContext {
    async resume() {}
    async decodeAudioData(data) { return data; }
    createBufferSource() {
      const source = {
        buffer: null,
        connect() {},
        stop() {},
        start() {
          if (source.buffer) sounds.push("completed");
          else soundUnlocks.push("silent");
        },
      };
      return source;
    }
  }
  const nodes = new Map();
  function node(id) {
    if (!nodes.has(id)) nodes.set(id, {
      value: "", textContent: "", title: "", style: {}, dataset: {}, children: [], firstElementChild: { style: {} },
      classList: { toggle() {} },
      events: new Map(),
      addEventListener(type, listener) {
        if (!this.events.has(type)) this.events.set(type, new Set());
        this.events.get(type).add(listener);
      },
      removeEventListener(type, listener) { this.events.get(type)?.delete(listener); },
      dispatch(type, event = {}) { for (const listener of this.events.get(type) || []) listener(event); },
      showModal() {
        this.open = true;
        queueMicrotask(() => {
          this.onShow?.();
          if (this.answer === "escape") this.dispatch("cancel");
          else this.dispatch("submit", { submitter: { value: this.answer || "select" } });
          this.open = false;
        });
      },
      setAttribute() {}, removeAttribute() {},
      append(...children) { this.children.push(...children); },
      replaceChildren(...children) { this.children = children; }, remove() {}, focus() {}, scrollIntoView() {},
    });
    return nodes.get(id);
  }
  const context = vm.createContext({
    window: { __TAURI__: { core: { invoke }, window: windowApi, ...tauriApi }, AudioContext },
    fetch: async (path) => {
      assert.equal(path, "assets/audio/completed.mp3");
      return { ok: true, arrayBuffer: async () => new ArrayBuffer(1) };
    },
    console: { ...console, warn: (...args) => warnings.push(args) },
    document: {
      getElementById: node,
      querySelectorAll: (selector) => selector.includes('"units"') ? inputs : [],
      createElement: () => ({
        dataset: {}, children: [], attributes: {}, events: {},
        setAttribute(key, value) { this.attributes[key] = value; },
        addEventListener(type, listener) { this.events[type] = listener; },
        append(...children) { this.children.push(...children); }, remove() {},
      }),
      createDocumentFragment: () => ({ append() {} }),
      createTextNode: (text) => ({ textContent: text }),
    },
    setTimeout: () => 0,
  });
  const source = readFileSync(`${__dirname}/../ui/app.js`, "utf8").replace(
    "  init();",
    "  globalThis.api = { state, logLine, addArchives, removeArchive, chooseArchive, initDragDrop, startTransfer, batchRatio, renderBatch, buildConfig, loadPreferences, saveUnits, saveBuffer, saveRetries, saveCompletionSound, savePasswordVisibility, saveUpdateChecks, initUpdateChecks, checkForUpdates, memorySave, memoryRecall, memoryClear, chooseMemorySlot, runMemoryAction, updateMemoryButtons, changeProtocol, renderCurrent, renderTotal, renderStatus, render, updateWindowProgress, requestAttention, flushWindowProgress: () => windowProgressQueue };"
  );
  vm.runInContext(source, context);
  context.api.state.preferencesBusy = false;
  node("buffer").value = "64";
  node("retries").value = "3";
  return { ...context.api, inputs, node, sounds, soundUnlocks, warnings };
}

test("progress uses the engine text for ordinary and streamed archives", () => {
  const app = frontend();
  app.state.units = "si";
  const progress = {
    current_file: "file", current_number: 1, total_files: 1, current_sent: 1000 ** 2,
    current_size: 1000 ** 3, sent_bytes: 1000 ** 2, total_bytes: 1000 ** 3,
    files_done: 0, files_skipped: 1, skipped_bytes: 1000 ** 2,
    average_rate: 1000 ** 2, upload_rate: 2 * 1000 ** 2, unpack_rate: 3 * 1000 ** 2,
    buffer_capacity: 64 * 1024 ** 2, buffer_used: 0, totals_known: true,
    archive_read: 500, archive_size: 1000, text: {
      current_sent: "1.00 MB", current_size: "1.00 GB", sent_bytes: "1.00 MB", total_bytes: "1.00 GB",
      skipped_bytes: "1.00 MB", average_rate: "1.00 MB/s", upload_rate: "2.00 MB/s",
      unpack_rate: "3.00 MB/s", buffer_capacity: "67.1 MB",
    },
  };
  const snap = { phase: "transferring", progress };
  app.renderCurrent(snap);
  app.renderTotal({}, snap);
  app.renderStatus(snap);
  assert.match(app.node("cur-stats").textContent, /1.00 MB \/ 1.00 GB/);
  assert.match(app.node("total-stats").textContent, /1.00 MB \/ 1.00 GB/);
  assert.equal(app.node("stat-upload-value").textContent, "1.00 MB/s");
  assert.equal(app.node("stat-unpack-value").textContent, "3.00 MB/s");
  assert.match(app.node("stat-buffer").title, /67.1 MB/);
  progress.totals_known = false;
  app.renderTotal({}, snap);
  assert.match(app.node("total-stats").textContent, /50.0% of the archive read · 1.00 MB uploaded/);
});

test("native progress follows the whole archive and clears after every terminal outcome", async () => {
  const calls = [];
  const app = frontend(undefined, {
    ProgressBarStatus: { Normal: "normal", Indeterminate: "indeterminate", None: "none" },
    getCurrentWindow: () => ({ setProgressBar: async (value) => calls.push({ ...value }) }),
  });
  app.updateWindowProgress({ phase: "reading" });
  const snap = { phase: "transferring", progress: {
    totals_known: true, sent_bytes: 25, total_bytes: 100,
    current_sent: 9, current_size: 10, archive_read: 750, archive_size: 1000,
  } };
  app.updateWindowProgress(snap);
  app.updateWindowProgress(snap); // unchanged polls do not repeat native calls
  snap.progress.totals_known = false;
  app.updateWindowProgress(snap);
  for (const status of ["success", "failed", "cancelled"]) {
    app.updateWindowProgress({ phase: "finished", result: { status } });
    await app.flushWindowProgress();
    assert.equal(calls.at(-1).status, "none");
    app.updateWindowProgress({ phase: "reading" });
  }
  await app.flushWindowProgress();
  assert.deepEqual(calls.slice(0, 4), [
    { status: "indeterminate", progress: 0 },
    { status: "normal", progress: 25 },
    { status: "normal", progress: 75 },
    { status: "none", progress: 0 },
  ]);
});

test("attention distinguishes fatal errors and success without notifying cancellation", () => {
  const calls = [];
  const app = frontend(undefined, {
    UserAttentionType: { Critical: 1, Informational: 2 },
    getCurrentWindow: () => ({ requestUserAttention: (type) => { calls.push(type); return Promise.resolve(); } }),
  });
  for (const status of ["failed", "success", "cancelled", null]) app.requestAttention(status);
  assert.deepEqual(calls, [1, 2, null]);
});

test("the final extra poll notifies and sounds only once, respecting the sound preference", async () => {
  for (const [status, enabled] of [["success", true], ["success", false], ["failed", true], ["cancelled", true]]) {
    const attention = [];
    const progress = [];
    const app = frontend(undefined, {
      ProgressBarStatus: { None: "none" },
      UserAttentionType: { Critical: 1, Informational: 2 },
      getCurrentWindow: () => ({
        setProgressBar: async (value) => progress.push({ ...value }),
        requestUserAttention: async (type) => attention.push(type),
      }),
    });
    app.state.completionSound = enabled;
    const job = { finished: false };
    const snap = { phase: "finished", result: { status }, progress: null };
    app.render(job, snap, 1);
    app.render(job, snap, 2);
    await app.flushWindowProgress();
    await new Promise(setImmediate);
    assert.deepEqual(attention, status === "cancelled" ? [] : [status === "failed" ? 1 : 2]);
    assert.deepEqual(progress, [{ status: "none", progress: 0 }]);
    assert.deepEqual(app.sounds, status === "success" && enabled ? ["completed"] : []);
  }
});

test("the sound preference loads and saves without changing units or buffer", async () => {
  const calls = [];
  const app = frontend(async (command, args) => {
    calls.push({ command, args });
    return { units: "binary", buffer_mib: 128, completion_sound: false };
  });
  assert.equal(app.state.completionSound, true);
  await app.loadPreferences();
  assert.equal(app.node("completion-sound").checked, false);
  app.node("completion-sound").checked = true;
  await app.saveCompletionSound();
  assert.deepEqual({ ...calls[1].args.preferences }, {
    units: "binary", buffer_mib: 128, completion_sound: true, show_passwords: false, retries: 3, check_updates: null, prevent_sleep: true,
  });
  assert.equal(app.state.completionSound, true);
  assert.equal(app.node("completion-sound").disabled, false);
  await app.saveUnits({ target: { value: "si" } });
  assert.equal(calls[2].args.preferences.completion_sound, true);
});

test("a failed sound preference save restores the previous checkbox state", async () => {
  const app = frontend(async () => { throw new Error("read-only directory"); });
  app.node("completion-sound").checked = false;
  await app.saveCompletionSound();
  assert.equal(app.state.completionSound, true);
  assert.equal(app.node("completion-sound").checked, true);
  assert.equal(app.node("completion-sound").disabled, false);
});

test("password visibility is shared, persists across preference changes, and keeps input values", async () => {
  const calls = [];
  const app = frontend(async (command, args) => {
    calls.push({ command, args });
    return { units: "binary", buffer_mib: 128, completion_sound: false, show_passwords: true };
  });
  const fields = ["archive-password", "password", "pw-input"];
  for (const id of fields) app.node(id).value = "secret";
  await app.loadPreferences();
  for (const id of fields) {
    assert.equal(app.node(id).type, "text");
    assert.equal(app.node(`${id}-visible`).checked, true);
  }
  await app.saveUnits({ target: { value: "si" } });
  assert.equal(calls[1].args.preferences.show_passwords, true);
  await app.savePasswordVisibility({ target: { checked: false } });
  assert.deepEqual({ ...calls[2].args.preferences }, {
    units: "si", buffer_mib: 128, completion_sound: false, show_passwords: false, retries: 3, check_updates: null, prevent_sleep: true,
  });
  for (const id of fields) {
    assert.equal(app.node(id).type, "password");
    assert.equal(app.node(id).value, "secret");
    assert.equal(app.node(`${id}-visible`).checked, false);
    assert.equal(app.node(`${id}-visible`).disabled, false);
  }
});

test("loading and changing units persists automatically through the backend", async () => {
  const calls = [];
  const app = frontend(async (command, args) => {
    calls.push({ command, args });
    return { units: "si", buffer_mib: 128 };
  });
  await app.loadPreferences();
  assert.equal(app.state.units, "si");
  assert.equal(app.inputs[0].checked, true);
  assert.equal(app.node("buffer").value, "128");
  await app.saveUnits({ target: { value: "binary" } });
  assert.equal(calls[1].command, "preferences_save");
  assert.equal(calls[1].args.preferences.units, "binary");
  assert.equal(calls[1].args.preferences.buffer_mib, 128);
  assert.equal(app.state.units, "binary");
  assert.equal(app.inputs[1].checked, true);
  assert.equal(app.inputs[1].disabled, false);
});

test("a failed save restores the previous selection", async () => {
  const app = frontend(async () => { throw new Error("read-only directory"); });
  await app.saveUnits({ target: { value: "binary" } });
  assert.equal(app.state.units, "si");
  assert.equal(app.inputs[0].checked, true);
  assert.equal(app.inputs[1].checked, false);
  assert.equal(app.inputs[0].disabled, false);
});


test("buffer changes save alongside units; invalid values never overwrite preferences", async () => {
  const calls = [];
  const app = frontend(async (command, args) => calls.push({ command, args }));
  app.state.units = "si";
  app.node("buffer").value = "256";
  await app.saveBuffer();
  assert.equal(calls[0].command, "preferences_save");
  assert.equal(calls[0].args.preferences.units, "si");
  assert.equal(calls[0].args.preferences.buffer_mib, 256);
  assert.equal(app.state.bufferMib, 256);
  for (const value of ["", "0", "4097", "1.5", "abc"]) {
    app.node("buffer").value = value;
    await app.saveBuffer();
  }
  assert.equal(calls.length, 1);
  assert.equal(app.state.bufferMib, 256);
});

test("a failed buffer save restores its previous value", async () => {
  const app = frontend(async () => { throw new Error("read-only directory"); });
  app.node("buffer").value = "256";
  await app.saveBuffer();
  assert.equal(app.state.bufferMib, 64);
  assert.equal(app.node("buffer").value, "64");
  assert.equal(app.node("buffer").disabled, false);
});


test("log messages retain file names and the engine's selected units verbatim", () => {
  const app = frontend();
  app.state.units = "si";
  const text = "Uploaded 64 MiB.txt (67.1 MB, 89.4 MB/s)";
  const line = app.logLine({ level: "info", time: "12:00:00", text });
  assert.equal(line.children.at(-1).textContent, text);
});

test("the selected units are sent to the engine when a transfer starts", () => {
  const app = frontend();
  app.node("port").value = "21";
  app.state.archives = ["/tmp/archive.zip"];
  assert.equal(app.buildConfig().units, "si");
  app.state.units = "binary";
  assert.equal(app.buildConfig().units, "binary");
});

test("Add File appends multiple selections, ignores duplicates, and Remove File empties the queue", async () => {
  const options = [];
  const app = frontend(undefined, {}, { dialog: { open: async (value) => {
    options.push(value);
    return ["/tmp/first.zip", { path: "/tmp/second.7z" }, "/tmp/first.zip"];
  } } });
  app.node("host").value = "ftp.example.com";
  await app.chooseArchive();
  assert.equal(options[0].multiple, true);
  assert.deepEqual(Array.from(app.state.archives), ["/tmp/first.zip", "/tmp/second.7z"]);
  assert.equal(app.node("archive-list").children.length, 2);
  assert.equal(app.node("btn-upload").disabled, false);
  // Exercise the row button's actual handler.
  app.node("archive-list").children[0].children[1].events.click();
  assert.deepEqual(Array.from(app.state.archives), ["/tmp/second.7z"]);
  app.removeArchive(0);
  assert.equal(app.node("archive-list").hidden, true);
  assert.equal(app.node("btn-upload").disabled, true);
});

test("dropping multiple archives appends to the queue and cannot change a running batch", async () => {
  let drop;
  const app = frontend(undefined, {}, { webview: { getCurrentWebview: () => ({
    onDragDropEvent: async (callback) => { drop = callback; },
  }) } });
  app.addArchives(["/tmp/first.rar"]);
  await app.initDragDrop();
  drop({ payload: { type: "drop", paths: ["/tmp/second.zip", "/tmp/third.tar.gz"] } });
  assert.equal(app.state.archives.length, 3);
  app.state.starting = true;
  app.addArchives(["/tmp/ignored.zip"]);
  app.removeArchive(0);
  assert.equal(app.state.archives.length, 3);
  app.state.starting = false;
  app.state.view = "transfer";
  drop({ payload: { type: "drop", paths: ["/tmp/ignored.zip"] } });
  assert.equal(app.state.archives.length, 3);
});

test("starting a batch sends every archive with one shared configuration", async () => {
  const calls = [];
  const app = frontend(async (command, args) => {
    calls.push({ command, args });
    if (command === "poll_transfer") return new Promise(() => {});
  }, {
    ProgressBarStatus: { Indeterminate: "indeterminate" },
    getCurrentWindow: () => ({ setProgressBar: async () => {}, requestUserAttention: async () => {} }),
  });
  app.node("port").value = "21";
  app.node("host").value = "ftp.example.com";
  app.node("directory").value = "/shared";
  app.node("retries").value = "5";
  app.addArchives(["/tmp/first.zip", "/tmp/second.7z"]);
  await app.startTransfer();
  const start = calls.find((call) => call.command === "start_transfer");
  assert.deepEqual(Array.from(start.args.archives), ["/tmp/first.zip", "/tmp/second.7z"]);
  assert.equal(start.args.config.archive, "/tmp/first.zip");
  assert.equal(start.args.config.host, "ftp.example.com");
  assert.equal(start.args.config.directory, "/shared");
  assert.equal(start.args.config.retries, 5);
  assert.equal(app.state.view, "transfer");
  assert.deepEqual(app.soundUnlocks, ["silent"]);
});

test("batch progress includes completed archives and streamed progress, and exposes each outcome", async () => {
  const calls = [];
  const app = frontend(undefined, {
    ProgressBarStatus: { Normal: "normal" },
    getCurrentWindow: () => ({ setProgressBar: async (value) => calls.push({ ...value }) }),
  });
  const snap = { phase: "transferring", progress: {
    totals_known: false, archive_read: 50, archive_size: 100,
  }, batch: { total: 4, completed: 2, current_index: 2, items: [
    { path: "/tmp/first.zip", status: "success" },
    { path: "/tmp/second.rar", status: "failed" },
    { path: "/tmp/third.tar.gz", status: "running" },
    { path: "/tmp/fourth.7z", status: "waiting" },
  ] } };
  app.renderBatch({}, snap);
  assert.equal(app.batchRatio(snap), 0.625);
  assert.equal(app.node("batch-bar").firstElementChild.style.width, "62.5%");
  assert.equal(app.node("batch-count").textContent, "2/4 archives");
  assert.equal(app.node("batch-percent").textContent, "62.5%");
  assert.equal(app.node("batch-issues").textContent, "1 failed");
  assert.match(app.node("batch-stats").textContent, /1 done · 1 failed/);
  assert.deepEqual(app.node("batch-list").children.map((row) => row.children[1].textContent),
    ["Done", "Failed", "In progress", "Waiting"]);
  app.updateWindowProgress(snap);
  await app.flushWindowProgress();
  assert.deepEqual(calls, [{ status: "normal", progress: 62 }]);
  snap.phase = "finished";
  snap.batch.items[2].status = "cancelled";
  snap.batch.items[3].status = "not_started";
  snap.batch.completed = 3;
  app.renderBatch({}, snap);
  assert.equal(app.batchRatio(snap), 0.75);
  assert.match(app.node("batch-stats").textContent, /1 cancelled · 1 not started/);
});

test("intermediate archive failure does not notify, and final batch failure notifies once", async () => {
  const attention = [];
  const app = frontend(undefined, {
    ProgressBarStatus: { Normal: "normal", None: "none" },
    UserAttentionType: { Critical: 1 },
    getCurrentWindow: () => ({
      setProgressBar: async () => {}, requestUserAttention: async (type) => attention.push(type),
    }),
  });
  const job = { finished: false };
  app.state.archives = ["/tmp/first.zip", "/tmp/second.zip"];
  const snap = { phase: "reading", progress: null, batch: {
    total: 2, completed: 1, current_index: 1, items: [
      { path: "/tmp/first.zip", status: "failed" }, { path: "/tmp/second.zip", status: "running" },
    ],
  } };
  app.render(job, snap, 1);
  assert.equal(job.finished, false);
  assert.equal(attention.length, 0);
  assert.match(app.node("phase-text").textContent, /Archive 2\/2/);
  assert.equal(app.node("t-archive").textContent, "second.zip");
  snap.phase = "finished";
  snap.result = { status: "failed", error: "1 archive failed" };
  snap.batch.completed = 2;
  snap.batch.items[1].status = "success";
  app.render(job, snap, 2);
  app.render(job, snap, 3);
  await app.flushWindowProgress();
  assert.equal(app.node("result-title").textContent, "Batch finished with errors");
  assert.equal(app.node("t-archive").textContent, "Batch of 2 archives");
  assert.equal(app.node("total-block").hidden, true);
  assert.deepEqual(Array.from(app.state.archives), []);
  assert.equal(app.node("archive-list").hidden, true);
  assert.deepEqual(attention, [1]);
  assert.equal(app.sounds.length, 0);
});


test("upload attempts persist and reach the engine; invalid values block transfer", async () => {
  const calls = [];
  const app = frontend(async (command, args) => {
    calls.push({ command, args });
    return { units: "si", buffer_mib: 128, retries: 5 };
  });
  await app.loadPreferences();
  assert.equal(app.node("retries").value, "5");
  app.node("port").value = "21";
  app.state.archives = ["/tmp/archive.zip"];
  assert.equal(app.buildConfig().retries, 5);
  app.node("retries").value = "1";
  await app.saveRetries();
  assert.equal(calls[1].args.preferences.retries, 1);
  assert.equal(calls[1].args.preferences.buffer_mib, 128);
  assert.equal(app.buildConfig().retries, 1);
  await app.saveUnits({ target: { value: "binary" } });
  assert.equal(calls[2].args.preferences.retries, 1);
  for (const value of ["", "0", "-1", "1.5", "abc", "4294967296"]) {
    app.node("retries").value = value;
    await app.saveRetries();
    assert.equal(app.buildConfig(), null);
    assert.equal(app.node("advanced").open, true);
  }
  assert.equal(calls.length, 3);
  assert.equal(app.state.retries, 1);
});

test("a failed attempt count save restores the previous value", async () => {
  const app = frontend(async () => { throw new Error("read-only directory"); });
  app.node("retries").value = "5";
  await app.saveRetries();
  assert.equal(app.state.retries, 3);
  assert.equal(app.node("retries").value, "3");
  assert.equal(app.node("retries").disabled, false);
});


test("first-launch consent uses a separate window and is saved before GitHub is checked", async () => {
  const calls = [];
  let answer;
  let saved = { units: "si", buffer_mib: 64, retries: 3, check_updates: null };
  const invoke = async (command, args) => {
    calls.push(command);
    if (command === "preferences_load") return saved;
    if (command === "preferences_save") saved = { ...args.preferences };
    if (command === "ask_update_consent") return new Promise((resolve) => { answer = resolve; });
    if (command === "check_for_updates") {
      assert.equal(saved.check_updates, true);
      return null;
    }
  };
  const app = frontend(invoke);
  await app.loadPreferences();
  const consent = app.initUpdateChecks();
  assert.deepEqual(calls, ["preferences_load", "ask_update_consent"]);
  assert.equal(app.state.preferencesBusy, false);
  answer(true);
  await consent;
  assert.deepEqual(calls, ["preferences_load", "ask_update_consent", "preferences_save", "check_for_updates"]);
  assert.equal(app.node("check-updates").checked, true);
  const relaunched = frontend(invoke);
  await relaunched.loadPreferences();
  await relaunched.initUpdateChecks();
  assert.equal(calls.filter((command) => command === "ask_update_consent").length, 1);
  assert.equal(calls.filter((command) => command === "check_for_updates").length, 2);
});

test("declining consent persists no, suppresses later questions and never contacts GitHub", async () => {
  const calls = [];
  let saved = { units: "si", buffer_mib: 64 };
  const invoke = async (command, args) => {
    calls.push(command);
    if (command === "preferences_load") return saved;
    if (command === "preferences_save") saved = { ...args.preferences };
    if (command === "ask_update_consent") return false;
  };
  const app = frontend(invoke);
  await app.loadPreferences();
  await app.initUpdateChecks();
  assert.equal(saved.check_updates, false);
  assert.equal(app.node("check-updates").checked, false);
  const relaunched = frontend(invoke);
  await relaunched.loadPreferences();
  await relaunched.initUpdateChecks();
  assert.equal(calls.filter((command) => command === "ask_update_consent").length, 1);
  assert.equal(calls.includes("check_for_updates"), false);
});

test("unreadable preferences and failed consent saves never trigger a GitHub request", async () => {
  const calls = [];
  const app = frontend(async (command) => {
    calls.push(command);
    if (command === "ask_update_consent") return true;
    throw new Error("unreadable preferences");
  });
  await app.loadPreferences();
  await app.initUpdateChecks();
  assert.deepEqual(calls, ["preferences_load"]);
  app.state.preferencesLoaded = true;
  await app.initUpdateChecks();
  assert.equal(app.state.checkUpdates, null);
  assert.deepEqual(calls, ["preferences_load", "ask_update_consent", "preferences_save"]);
});

test("update requests run once per launch and open a separate window without changing a transfer", async () => {
  const calls = [];
  let complete;
  const app = frontend(async (command) => {
    calls.push(command);
    if (command === "check_for_updates") return new Promise((resolve) => { complete = resolve; });
  });
  app.state.checkUpdates = true;
  app.state.job = { finished: false };
  app.state.view = "transfer";
  const job = app.state.job;
  const check = app.checkForUpdates();
  await app.checkForUpdates();
  assert.deepEqual(calls, ["check_for_updates"]);
  assert.equal(app.state.preferencesBusy, false);
  complete("2.10.0");
  await check;
  assert.equal(app.state.job, job);
  assert.equal(app.state.view, "transfer");
  assert.equal(app.node("btn-cancel").disabled, undefined);
  assert.equal(app.node("toasts").children.length, 0);
  assert.deepEqual(calls, ["check_for_updates", "show_update_available"]);
});

test("current versions and network errors are silent; disabling suppresses an in-flight result", async () => {
  for (const result of [null, new Error("offline"), new Error("GitHub rate limit")]) {
    const calls = [];
    const app = frontend(async (command) => {
      calls.push(command);
      if (result instanceof Error) throw result;
      return result;
    });
    app.state.checkUpdates = true;
    await app.checkForUpdates();
    assert.deepEqual(calls, ["check_for_updates"]);
    assert.equal(app.node("toasts").children.length, 0);
    assert.equal(app.warnings.length, result instanceof Error ? 1 : 0);
  }
  const calls = [];
  let complete;
  const app = frontend(async (command) => {
    calls.push(command);
    if (command === "check_for_updates") return new Promise((resolve) => { complete = resolve; });
  });
  app.state.checkUpdates = true;
  const check = app.checkForUpdates();
  app.node("check-updates").checked = false;
  await app.saveUpdateChecks();
  complete("9.0.0");
  await check;
  assert.equal(calls.includes("show_update_available"), false);
  assert.equal(calls.includes("dismiss_update_prompt"), true);
  assert.equal(app.state.checkUpdates, false);
});

test("Advanced persists update checks across other preferences and rolls back failed changes", async () => {
  const calls = [];
  const app = frontend(async (command, args) => {
    calls.push({ command, args });
    if (command === "preferences_load") return { units: "binary", buffer_mib: 128, check_updates: false };
  });
  await app.loadPreferences();
  await app.saveUnits({ target: { value: "si" } });
  assert.equal(calls.at(-1).args.preferences.check_updates, false);
  app.node("check-updates").checked = true;
  await app.saveUpdateChecks();
  assert.equal(app.state.checkUpdates, true);
  const enabled = calls.find((call) => call.command === "preferences_save" && call.args.preferences.check_updates);
  assert.equal(enabled.args.preferences.buffer_mib, 128);
  assert.equal(calls.at(-1).command, "check_for_updates");
  await app.saveCompletionSound();
  assert.equal(calls.at(-1).args.preferences.check_updates, true);
  app.node("check-updates").checked = false;
  await app.saveUpdateChecks();
  assert.equal(app.state.checkUpdates, false);
  assert.equal(calls.at(-1).command, "dismiss_update_prompt");

  const failed = frontend(async () => { throw new Error("read-only directory"); });
  failed.state.checkUpdates = true;
  failed.node("check-updates").checked = false;
  await failed.saveUpdateChecks();
  assert.equal(failed.state.checkUpdates, true);
  assert.equal(failed.node("check-updates").checked, true);
  assert.equal(failed.node("check-updates").disabled, false);
});

test("an Advanced choice made while the consent window is open takes precedence", async () => {
  let answer;
  const calls = [];
  const app = frontend(async (command, args) => {
    calls.push({ command, args });
    if (command === "ask_update_consent") return new Promise((resolve) => { answer = resolve; });
  });
  app.state.preferencesLoaded = true;
  const consent = app.initUpdateChecks();
  app.node("check-updates").checked = true;
  await app.saveUpdateChecks();
  answer(false); // closing the consent window must not overwrite Advanced's choice
  await consent;
  assert.equal(app.state.checkUpdates, true);
  assert.equal(calls.filter((call) => call.command === "preferences_save").length, 1);
  assert.equal(calls.filter((call) => call.command === "check_for_updates").length, 1);
});


test("protocol changes use the right default port without replacing a custom port", () => {
  const app = frontend();
  app.node("port").value = "21";
  app.node("protocol").value = "ftps_implicit";
  app.changeProtocol();
  assert.equal(app.node("port").value, "990");
  app.node("protocol").value = "ftps_explicit";
  app.changeProtocol();
  assert.equal(app.node("port").value, "21");
  app.node("port").value = "2121";
  for (const protocol of ["ftps_implicit", "ftp", "ftps_explicit"]) {
    app.node("protocol").value = protocol;
    app.changeProtocol();
    assert.equal(app.node("port").value, "2121");
  }
});

test("the protocol and connection mode reach the transfer and server memory", async () => {
  const calls = [];
  const saved = { host: "example.org", port: 990, protocol: "ftps_implicit", mode: "active" };
  const app = frontend(async (command, args) => {
    calls.push({ command, args });
    if (command === "memory_recall") return saved;
    if (command === "memory_status") return { saved: true, configs: [{ id: "ed8c5e75-f51f-48b2-a8e8-8e5de7b9a2e6", ...saved, store_credentials: false }] };
  });
  app.state.archives = ["one.zip", "two.zip"];
  app.node("port").value = "21";
  await app.memoryRecall();
  assert.equal(app.node("protocol").value, "ftps_implicit");
  assert.equal(app.node("mode").value, "active");
  assert.equal(app.buildConfig().protocol, "ftps_implicit");
  assert.equal(app.buildConfig().mode, "active");
  await app.memorySave();
  const server = calls.find((call) => call.command === "memory_save").args.server;
  assert.equal(server.protocol, "ftps_implicit");
  assert.equal(server.port, 990);
  app.node("protocol").value = "ftp";
  app.changeProtocol();
  assert.equal(app.node("port").value, "21");
  delete saved.protocol;
  saved.port = 2121;
  await app.memoryRecall();
  assert.equal(app.node("protocol").value, "ftp");
  assert.equal(app.node("port").value, "2121");
});

test("SFTP selects port 22, hides FTP mode, and passes SSH settings", () => {
  const ui = frontend();
  ui.node("mode").value = "active";
  ui.node("port").value = "21";
  ui.node("protocol").value = "sftp";
  ui.changeProtocol();
  assert.equal(ui.node("port").value, "22");
  assert.equal(ui.node("mode").disabled, true);
  assert.equal(ui.node("mode-field").hidden, true);
  assert.equal(ui.node("btn-ssh").hidden, false);
  ui.node("private-key").value = "/keys/custom";
  ui.node("private-key-passphrase").value = "key passphrase";
  ui.node("verify-host-key").checked = true;
  ui.node("known-hosts").value = "";
  const config = ui.buildConfig();
  assert.equal(config.private_key, "/keys/custom");
  assert.equal(config.private_key_passphrase, "key passphrase");
  assert.equal(config.known_hosts, "");
  ui.node("verify-host-key").checked = false;
  assert.equal(ui.buildConfig().known_hosts, null);
  ui.node("port").value = "2222";
  ui.node("protocol").value = "ftp";
  ui.changeProtocol();
  assert.equal(ui.node("port").value, "2222");
  assert.equal(ui.node("mode").disabled, false);
  assert.equal(ui.node("mode-field").hidden, false);
  assert.equal(ui.node("mode").value, "active");
});

test("SFTP memory recalls private key and host verification settings", async () => {
  const saved = { protocol: "sftp", port: 22, host: "ssh.example", user: "",
    private_key: "/keys/custom", private_key_passphrase: "secret", known_hosts: "" };
  const ui = frontend(async (command) => command === "memory_status"
    ? { saved: true, configs: [{ id: "ed8c5e75-f51f-48b2-a8e8-8e5de7b9a2e6", ...saved }] } : saved);
  await ui.memoryRecall();
  assert.equal(ui.node("protocol").value, "sftp");
  assert.equal(ui.node("mode-field").hidden, true);
  assert.equal(ui.node("private-key").value, "/keys/custom");
  assert.equal(ui.node("private-key-passphrase").value, "secret");
  assert.equal(ui.node("verify-host-key").checked, true);
  assert.equal(ui.buildConfig().known_hosts, "");
});


const memoryConfigs = [
  { id: "ed8c5e75-f51f-48b2-a8e8-8e5de7b9a2e6", host: "ftp.example", port: 21,
    protocol: "ftp", user: "alice", directory: "/uploads", store_credentials: true },
  { id: "bd1072ad-b9fb-4d8e-a46b-a3e480f5c87b", host: "ssh.example", port: 22,
    protocol: "sftp", user: null, directory: "/backup", store_credentials: false },
];

function memoryFrontend() {
  const calls = [];
  const ui = frontend(async (command, args) => {
    calls.push({ command, args });
    if (command === "memory_status") return { saved: true, configs: memoryConfigs };
    if (command === "memory_recall") return { ...memoryConfigs.find((config) => config.id === args.id), password: null };
  });
  ui.node("host").value = "new.example";
  ui.node("port").value = "2121";
  ui.node("protocol").value = "ftp";
  const select = (id) => {
    ui.node("memory-slot").value = id;
    ui.node("memory-slot").dispatch("change");
  };
  return { ui, calls, select };
}

test("memory save lists configs and defaults to a new slot without names", async () => {
  const { ui, calls } = memoryFrontend();
  await ui.memorySave();
  const options = ui.node("memory-slot").children;
  assert.equal(options.length, 3);
  assert.equal(options[0].textContent, "New slot");
  assert.equal(options[1].textContent, "ftp.example:21 · /uploads");
  const args = calls.find((call) => call.command === "memory_save").args;
  assert.equal(args.id, null);
  assert.equal(args.server.host, "new.example");
  assert.equal(args.name, undefined);
});

test("memory overwrite uses the selected UUID and that slot's consent", async () => {
  const { ui, calls, select } = memoryFrontend();
  ui.node("user").value = "bob";
  ui.node("password").value = "secret";
  ui.node("dlg-memory-slots").onShow = () => select(memoryConfigs[1].id);
  await ui.memorySave();
  const args = calls.find((call) => call.command === "memory_save").args;
  assert.equal(args.id, memoryConfigs[1].id);
  assert.equal(args.store_credentials, null);
  assert.equal(ui.node("memory-slot-action").textContent, "Overwrite");
  assert.match(ui.node("memory-slot-details").textContent, /SFTP · ssh.example:22/);
});

test("new memory slot asks consent even when another slot stores credentials", async () => {
  const { ui, calls } = memoryFrontend();
  ui.node("user").value = "bob";
  ui.node("dlg-memory").answer = "dont";
  await ui.memorySave();
  const args = calls.find((call) => call.command === "memory_save").args;
  assert.equal(args.id, null);
  assert.equal(args.store_credentials, false);
});

test("memory recall loads the chosen UUID and clears credentials absent from the slot", async () => {
  const { ui, calls, select } = memoryFrontend();
  ui.node("user").value = "old user";
  ui.node("password").value = "old secret";
  ui.node("dlg-memory-slots").onShow = () => select(memoryConfigs[1].id);
  await ui.memoryRecall();
  assert.equal(calls.find((call) => call.command === "memory_recall").args.id, memoryConfigs[1].id);
  assert.equal(ui.node("host").value, "ssh.example");
  assert.equal(ui.node("directory").value, "/backup");
  assert.equal(ui.node("user").value, "");
  assert.equal(ui.node("password").value, "");
});

test("memory clear deletes only the chosen configuration", async () => {
  const { ui, calls, select } = memoryFrontend();
  ui.node("dlg-memory-slots").onShow = () => select(memoryConfigs[1].id);
  await ui.memoryClear();
  assert.equal(ui.node("memory-slot").children.length, 2);
  assert.equal(calls.find((call) => call.command === "memory_clear").args.id, memoryConfigs[1].id);
});

test("cancel or Escape in every memory picker leaves settings untouched", async () => {
  for (const action of ["memorySave", "memoryRecall", "memoryClear"]) {
    for (const answer of ["cancel", "escape"]) {
      const { ui, calls } = memoryFrontend();
      ui.node("dlg-memory-slots").answer = answer;
      await ui[action]();
      assert.deepEqual(calls.map((call) => call.command), ["memory_status"]);
      assert.equal(ui.node("host").value, "new.example");
    }
  }
});

test("cancel in the credential dialog aborts saving a new slot", async () => {
  const { ui, calls } = memoryFrontend();
  ui.node("user").value = "bob";
  ui.node("dlg-memory").answer = "escape";
  await ui.memorySave();
  assert.equal(calls.some((call) => call.command === "memory_save"), false);
});

test("empty memory disables recall and clear while allowing a new save", async () => {
  const ui = frontend(async () => ({ saved: false, configs: [] }));
  ui.updateMemoryButtons();
  assert.equal(ui.node("btn-mem-save").disabled, false);
  assert.equal(ui.node("btn-mem-recall").disabled, true);
  assert.equal(ui.node("btn-mem-clear").disabled, true);
  const result = await ui.chooseMemorySlot("recall", { configs: [] });
  assert.equal(result, null);
});
