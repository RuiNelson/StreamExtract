const assert = require("node:assert/strict");
const { readFileSync } = require("node:fs");
const { test } = require("node:test");
const vm = require("node:vm");

function frontend(invoke = async () => ({ units: "si", buffer_mib: 64 }), windowApi = {}) {
  const inputs = ["si", "binary"].map((value) => ({ value, disabled: true, checked: value === "si" }));
  const nodes = new Map();
  function node(id) {
    if (!nodes.has(id)) nodes.set(id, {
      value: "", textContent: "", title: "", style: {}, dataset: {}, children: [], firstElementChild: { style: {} },
      setAttribute() {}, removeAttribute() {}, append() {}, replaceChildren() {}, remove() {}, focus() {},
    });
    return nodes.get(id);
  }
  const context = vm.createContext({
    window: { __TAURI__: { core: { invoke }, window: windowApi } },
    document: {
      getElementById: node,
      querySelectorAll: (selector) => selector.includes('"units"') ? inputs : [],
      createElement: () => ({
        dataset: {}, children: [], addEventListener() {},
        append(...children) { this.children.push(...children); }, remove() {},
      }),
      createTextNode: (text) => ({ textContent: text }),
    },
    setTimeout: () => 0,
  });
  const source = readFileSync(`${__dirname}/../ui/app.js`, "utf8").replace(
    "  init();",
    "  globalThis.api = { state, logLine, buildConfig, loadPreferences, saveUnits, saveBuffer, renderCurrent, renderTotal, renderStatus, render, updateWindowProgress, requestAttention, flushWindowProgress: () => windowProgressQueue };"
  );
  vm.runInContext(source, context);
  context.api.state.preferencesBusy = false;
  node("buffer").value = "64";
  return { ...context.api, inputs, node };
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

test("the final extra poll notifies only once, including failures before uploading", async () => {
  for (const status of ["success", "failed", "cancelled"]) {
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
    const job = { finished: false };
    const snap = { phase: "finished", result: { status }, progress: null };
    app.render(job, snap, 1);
    app.render(job, snap, 2);
    await app.flushWindowProgress();
    assert.deepEqual(attention, status === "cancelled" ? [] : [status === "failed" ? 1 : 2]);
    assert.deepEqual(progress, [{ status: "none", progress: 0 }]);
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
  app.state.archive = "/tmp/archive.zip";
  assert.equal(app.buildConfig().units, "si");
  app.state.units = "binary";
  assert.equal(app.buildConfig().units, "binary");
});
