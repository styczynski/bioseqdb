// Entity-reference tooltips (§12.4, §20.7). Progressive enhancement: on hover or
// keyboard focus of an `a.entity-ref`, lazily fetch its content-addressed entity
// dataset (same-origin, allowed by connect-src 'self'), look the entity up, and
// show a positioned, dismissible tooltip. Fully keyboard-accessible: focus shows
// it, blur and Escape hide it, and aria-describedby binds it to the anchor.

const datasetCache = new Map();

async function loadDataset(url) {
  if (!datasetCache.has(url)) {
    datasetCache.set(
      url,
      fetch(url)
        .then((response) => response.json())
        // The dataset's `entities` is an object keyed by canonical id (§12.4).
        .then((data) => data.entities || {})
        .catch(() => ({}))
    );
  }
  return datasetCache.get(url);
}

function escapeHtml(text) {
  return String(text).replace(/[&<>"']/g, (character) => ({
    "&": "&amp;",
    "<": "&lt;",
    ">": "&gt;",
    '"': "&quot;",
    "'": "&#39;",
  })[character]);
}

function ensureTooltip() {
  let tip = document.getElementById("entity-tooltip");
  if (!tip) {
    tip = document.createElement("div");
    tip.id = "entity-tooltip";
    tip.className = "entity-tooltip";
    tip.setAttribute("role", "tooltip");
    tip.hidden = true;
    document.body.appendChild(tip);
  }
  return tip;
}

function positionTooltip(tip, anchor) {
  const rect = anchor.getBoundingClientRect();
  const tipRect = tip.getBoundingClientRect();
  let top = rect.bottom + window.scrollY + 6;
  if (rect.bottom + tipRect.height + 6 > window.innerHeight) {
    top = rect.top + window.scrollY - tipRect.height - 6; // flip above near the edge
  }
  let left = rect.left + window.scrollX;
  const maxLeft = window.scrollX + window.innerWidth - tipRect.width - 6;
  if (left > maxLeft) {
    left = Math.max(window.scrollX + 6, maxLeft);
  }
  tip.style.top = `${Math.round(top)}px`;
  tip.style.left = `${Math.round(left)}px`;
}

let activeAnchor = null;

async function showTooltip(anchor) {
  const id = anchor.dataset.entityId;
  const url = anchor.dataset.entityDataUrl;
  if (!id || !url) return;
  const entities = await loadDataset(url);
  const record = entities[id];
  if (!record) return;
  const tip = ensureTooltip();
  const parts = [
    `<span class="tt-title">${escapeHtml(record.qualified_name || record.title || id)}</span>`,
  ];
  if (record.signature) {
    parts.push(`<code class="tt-signature">${escapeHtml(record.signature)}</code>`);
  }
  if (record.summary) {
    parts.push(`<span class="tt-summary">${escapeHtml(record.summary)}</span>`);
  }
  tip.innerHTML = parts.join("");
  tip.hidden = false;
  anchor.setAttribute("aria-describedby", "entity-tooltip");
  positionTooltip(tip, anchor);
  activeAnchor = anchor;
}

function hideTooltip() {
  const tip = document.getElementById("entity-tooltip");
  if (tip) tip.hidden = true;
  if (activeAnchor) activeAnchor.removeAttribute("aria-describedby");
  activeAnchor = null;
}

export function initTooltips() {
  const anchors = document.querySelectorAll(
    "a.entity-ref[data-entity-id][data-entity-data-url]"
  );
  if (anchors.length === 0) return;
  for (const anchor of anchors) {
    anchor.addEventListener("mouseenter", () => showTooltip(anchor));
    anchor.addEventListener("mouseleave", hideTooltip);
    anchor.addEventListener("focus", () => showTooltip(anchor));
    anchor.addEventListener("blur", hideTooltip);
  }
  document.addEventListener("keydown", (event) => {
    if (event.key === "Escape") hideTooltip();
  });
}
