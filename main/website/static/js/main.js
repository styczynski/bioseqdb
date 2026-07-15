// Progressive-enhancement entry module (§15.2). Self-hosted ES module, no
// framework, no external host. Establishes copy-button behavior with a polite
// live region; layout is CSS-only, so nothing here is required to render.

const reduceMotion = window.matchMedia("(prefers-reduced-motion: reduce)").matches;

function announce(message) {
  let region = document.getElementById("sr-status");
  if (!region) {
    region = document.createElement("div");
    region.id = "sr-status";
    region.setAttribute("aria-live", "polite");
    region.setAttribute("role", "status");
    region.style.position = "absolute";
    region.style.left = "-999px";
    document.body.appendChild(region);
  }
  region.textContent = message;
}

function initCopyButtons() {
  for (const button of document.querySelectorAll("[data-copy-target]")) {
    button.addEventListener("click", async () => {
      const target = document.getElementById(button.dataset.copyTarget);
      if (!target) return;
      try {
        await navigator.clipboard.writeText(target.textContent.trim());
        announce("Copied to clipboard");
      } catch {
        announce("Copy failed");
      }
    });
  }
}

document.addEventListener("DOMContentLoaded", () => {
  initCopyButtons();
  document.documentElement.dataset.reducedMotion = reduceMotion ? "true" : "false";
});
