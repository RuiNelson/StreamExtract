const assert = require("node:assert/strict");
const { readFileSync } = require("node:fs");
const { test } = require("node:test");
const vm = require("node:vm");

function updateWindow(kind, invoke = async () => "2.10.0", close = async () => {}) {
  const nodes = new Map();
  const events = {};
  const node = (id) => {
    if (!nodes.has(id)) nodes.set(id, {
      textContent: "", hidden: true, disabled: true, events: {},
      addEventListener(type, callback) { this.events[type] = callback; },
    });
    return nodes.get(id);
  };
  const context = vm.createContext({
    URLSearchParams,
    window: {
      location: { search: `?kind=${kind}` },
      __TAURI__: { core: { invoke }, window: { getCurrentWindow: () => ({ close }) } },
    },
    document: {
      getElementById: node,
      addEventListener(type, callback) { events[type] = callback; },
    },
  });
  const source = readFileSync(`${__dirname}/../ui/updates.js`, "utf8")
    .replace("  init();", "  globalThis.ready = init();");
  vm.runInContext(source, context);
  return { node, events, ready: context.ready };
}

test("the separate consent window answers yes or no without opening a browser", async () => {
  for (const enabled of [true, false]) {
    const calls = [];
    let closed = 0;
    const app = updateWindow("consent", async (command, args) => { calls.push({ command, args }); },
      async () => { closed++; });
    await app.ready;
    assert.deepEqual(calls, []);
    assert.equal(app.node("update-title").textContent, "Check for new versions?");
    assert.equal(app.node("update-hint").hidden, false);
    assert.equal(app.node("update-yes").disabled, false);
    await app.node(enabled ? "update-yes" : "update-no").events.click();
    assert.equal(calls[0].command, "answer_update_consent");
    assert.equal(calls[0].args.enabled, enabled);
    assert.equal(closed, 1);
  }
});

test("release windows ask before opening; Not now closes only the secondary window", async () => {
  for (const open of [true, false]) {
    const calls = [];
    let closed = 0;
    const app = updateWindow("release", async (command) => { calls.push(command); return "2.10.0"; },
      async () => { closed++; });
    await app.ready;
    assert.deepEqual(calls, ["update_version"]);
    assert.match(app.node("update-text").textContent, /v2\.10\.0.*Open the GitHub releases page/);
    await app.node(open ? "update-yes" : "update-no").events.click();
    assert.deepEqual(calls, open ? ["update_version", "open_releases"] : ["update_version"]);
    assert.equal(closed, 1);
  }
});

test("a failed browser open keeps the secondary window usable and shows its error", async () => {
  let closed = false;
  const app = updateWindow("release", async (command) => {
    if (command === "open_releases") throw new Error("no browser");
    return "2.10.0";
  }, async () => { closed = true; });
  await app.ready;
  await app.node("update-yes").events.click();
  assert.equal(closed, false);
  assert.equal(app.node("update-yes").disabled, false);
  assert.equal(app.node("update-no").disabled, false);
  assert.equal(app.node("update-error").hidden, false);
  assert.match(app.node("update-error").textContent, /no browser/);
});

test("repeated clicks during a browser open do not open duplicate tabs", async () => {
  let complete;
  const calls = [];
  const app = updateWindow("release", async (command) => {
    calls.push(command);
    if (command === "open_releases") return new Promise((resolve) => { complete = resolve; });
    return "2.10.0";
  });
  await app.ready;
  const click = app.node("update-yes").events.click();
  await app.node("update-yes").events.click();
  assert.deepEqual(calls, ["update_version", "open_releases"]);
  assert.equal(app.node("update-no").disabled, true);
  complete();
  await click;
});

test("Escape declines consent and dismisses a release without opening the browser", async () => {
  for (const kind of ["consent", "release"]) {
    const calls = [];
    let closed = 0;
    const app = updateWindow(kind, async (command, args) => { calls.push({ command, args }); return "2.10.0"; },
      async () => { closed++; });
    await app.ready;
    app.events.keydown({ key: "Escape" });
    await new Promise(setImmediate);
    assert.equal(closed, 1);
    assert.equal(calls.some((call) => call.command === "open_releases"), false);
    if (kind === "consent") assert.equal(calls[0].args.enabled, false);
  }
});
