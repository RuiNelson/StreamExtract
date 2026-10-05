"use strict";

// Runs only in the separate update windows; closing them never touches a transfer.
(() => {
  const tauri = window.__TAURI__;
  const consent = new URLSearchParams(window.location.search).get("kind") === "consent";
  const $ = (id) => document.getElementById(id);
  const yes = $("update-yes");
  const no = $("update-no");
  let busy = false;

  async function respond(enabled) {
    if (busy) return;
    busy = true;
    yes.disabled = no.disabled = true;
    $("update-error").hidden = true;
    try {
      if (consent) await tauri.core.invoke("answer_update_consent", { enabled });
      else if (enabled) await tauri.core.invoke("open_releases");
      await tauri.window.getCurrentWindow().close();
    } catch (error) {
      $("update-error").textContent = String(error);
      $("update-error").hidden = false;
    } finally {
      busy = false;
      yes.disabled = no.disabled = false;
    }
  }

  async function init() {
    if (consent) {
      $("update-title").textContent = "Check for new versions?";
      $("update-text").textContent = "Allow StreamExtract to check GitHub in the background when the app starts? If a new version is available, you'll be asked whether to open the releases page.";
      $("update-hint").hidden = false;
      yes.textContent = "Yes, check";
      no.textContent = "No";
    } else {
      try {
        const version = await tauri.core.invoke("update_version");
        if (!version) {
          await tauri.window.getCurrentWindow().close();
          return;
        }
        $("update-title").textContent = "New version available";
        $("update-text").textContent = `StreamExtract v${version} is available. Open the GitHub releases page?`;
      } catch (error) {
        $("update-error").textContent = String(error);
        $("update-error").hidden = false;
        no.disabled = false;
        no.addEventListener("click", () => respond(false));
        return;
      }
    }
    yes.disabled = no.disabled = false;
    yes.addEventListener("click", () => respond(true));
    no.addEventListener("click", () => respond(false));
    document.addEventListener("keydown", (event) => {
      if (event.key === "Escape") void respond(false);
    });
  }

  init();
})();
