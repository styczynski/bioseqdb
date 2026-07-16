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
  // A range may span several highlight tokens; wrap the covered segment inside each
  // text node it touches. textContent is invariant under wrapping, so offsets stay
  // correct even after earlier ranges split nodes (we re-walk per range).
  const end = range.start + range.len;
  const walker = document.createTreeWalker(code, NodeFilter.SHOW_TEXT);
  const segments = [];
  let offset = 0;
  for (let node = walker.nextNode(); node; node = walker.nextNode()) {
    const nodeEnd = offset + node.data.length;
    if (offset < end && range.start < nodeEnd) {
      segments.push({
        node,
        from: Math.max(0, range.start - offset),
        to: Math.min(node.data.length, end - offset),
      });
    }
    offset = nodeEnd;
  }
  for (const segment of segments) {
    const data = segment.node.data;
    const anchor = document.createElement("a");
    anchor.className = "entity-ref";
    anchor.setAttribute("href", range.route);
    anchor.setAttribute("data-entity-id", range.id);
    if (range.tooltip) anchor.setAttribute("data-entity-data-url", range.tooltip);
    anchor.textContent = data.slice(segment.from, segment.to);
    const fragment = document.createDocumentFragment();
    if (segment.from > 0) fragment.appendChild(document.createTextNode(data.slice(0, segment.from)));
    fragment.appendChild(anchor);
    if (segment.to < data.length) fragment.appendChild(document.createTextNode(data.slice(segment.to)));
    segment.node.replaceWith(fragment);
  }
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
  // Generator guarantees non-overlapping ranges, so order does not matter.
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
