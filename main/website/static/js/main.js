// Progressive-enhancement entry module (§15.2). Self-hosted ES module, no
// framework, no external host. Establishes copy-button behavior with a polite
// live region and initializes the landing tab toggler; layout is CSS-only, so
// nothing here is required to render. Landing-only widgets are guarded by their
// markers, so this single entry is inert on documentation/reference pages.

import { initTabs } from "./tabs.mjs";
import { initTooltips } from "./tooltips.mjs";
import { initSearch } from "./search.mjs";
import { initNav } from "./nav.mjs";
import { initLanding } from "./landing.mjs";

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

// Entity-range code decorator (§12.6). Prism (classic script, earlier in the DOM)
// highlights synchronously on DOMContentLoaded; this deferred module's handler runs
// after it, so wrapping happens post-highlight and Prism never erases it. Only text
// nodes are touched (never a regex over HTML), and each wrapped span keeps the
// original text, so copied source / textContent are unchanged (§20.7).
function wrapEntityRange(code, range) {
  // Prism may split a qualified identifier across several token spans. Build one
  // DOM Range across all of those nodes and extract it into one anchor, preserving
  // the Prism spans inside. This keeps `bioseq.len` a single hover/focus target and
  // a single visual pill instead of three adjacent boxes.
  const end = range.start + range.len;
  const walker = document.createTreeWalker(code, NodeFilter.SHOW_TEXT);
  let startNode = null;
  let startOffset = 0;
  let endNode = null;
  let endOffset = 0;
  let offset = 0;
  for (let node = walker.nextNode(); node; node = walker.nextNode()) {
    const nodeEnd = offset + node.data.length;
    if (!startNode && range.start >= offset && range.start <= nodeEnd) {
      startNode = node;
      startOffset = Math.max(0, range.start - offset);
    }
    if (end >= offset && end <= nodeEnd) {
      endNode = node;
      endOffset = Math.max(0, end - offset);
      break;
    }
    offset = nodeEnd;
  }
  if (!startNode || !endNode) return;
  const domRange = document.createRange();
  domRange.setStart(startNode, startOffset);
  domRange.setEnd(endNode, endOffset);
  const anchor = document.createElement("a");
  anchor.className = "entity-ref code-entity-ref";
  anchor.setAttribute("href", range.route);
  anchor.setAttribute("data-entity-id", range.id);
  if (range.tooltip) anchor.setAttribute("data-entity-data-url", range.tooltip);
  anchor.appendChild(domRange.extractContents());
  domRange.insertNode(anchor);
}

function decorateCode(code) {
  if (code.dataset.decorated) return;
  const raw = code.getAttribute("data-entity-ranges");
  if (!raw) return;
  let ranges;
  try {
    ranges = JSON.parse(raw);
  } catch {
    return;
  }
  if (!Array.isArray(ranges) || ranges.length === 0) return;
  code.dataset.decorated = "1";
  // Work right-to-left so extraction cannot shift the offsets of later ranges.
  ranges.sort((a, b) => b.start - a.start);
  for (const range of ranges) wrapEntityRange(code, range);
}

function initCodeDecorator() {
  document.querySelectorAll("code[data-entity-ranges]").forEach(decorateCode);
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
  initNav();
  initSearch();
  initCopyButtons();
  initTabs();
  initCodeDecorator();
  initTooltips();
  initLanding();
  document.documentElement.dataset.reducedMotion = reduceMotion ? "true" : "false";
});
