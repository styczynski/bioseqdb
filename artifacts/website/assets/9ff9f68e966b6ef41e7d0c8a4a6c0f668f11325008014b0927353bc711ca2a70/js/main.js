// Shared post-Prism decorator. prism-init.js disables Prism's deferred automatic
// pass, so this synchronous pass always finishes before entity ranges or landing
// query hooks are added. Page-specific behavior lives in the two literal design
// ports (landing-design.mjs and docs-design.mjs), avoiding duplicate copy, search,
// navigation, or tooltip handlers.

if (!window.Prism || typeof window.Prism.highlightAll !== "function") {
  throw new Error("the local Prism bundle did not initialize");
}
window.Prism.manual = true;
window.Prism.highlightAll();

function locateTextOffset(code, offset) {
  const walker = document.createTreeWalker(code, NodeFilter.SHOW_TEXT);
  let consumed = 0;
  for (let node = walker.nextNode(); node; node = walker.nextNode()) {
    const end = consumed + node.data.length;
    if (offset >= consumed && offset <= end) {
      return { node, offset: offset - consumed };
    }
    consumed = end;
  }
  return null;
}

function wrapEntityRange(code, metadata) {
  const start = locateTextOffset(code, metadata.start);
  const end = locateTextOffset(code, metadata.start + metadata.len);
  if (!start || !end) return;
  const range = document.createRange();
  range.setStart(start.node, start.offset);
  range.setEnd(end.node, end.offset);
  const link = document.createElement("a");
  link.className = "entity-ref code-entity-ref";
  link.href = metadata.route;
  link.dataset.entityId = metadata.id;
  if (metadata.tooltip) link.dataset.entityDataUrl = metadata.tooltip;
  link.appendChild(range.extractContents());
  range.insertNode(link);
}

function decorateCode(code) {
  if (code.dataset.decorated || !code.dataset.entityRanges) return;
  let ranges;
  try {
    ranges = JSON.parse(code.dataset.entityRanges);
  } catch {
    return;
  }
  if (!Array.isArray(ranges)) return;
  code.dataset.decorated = "true";
  // Right-to-left extraction preserves the generator's byte offsets.
  ranges.sort((left, right) => right.start - left.start);
  ranges.forEach((range) => wrapEntityRange(code, range));
}

document.querySelectorAll("code[data-entity-ranges]").forEach(decorateCode);

// Keep transparent vector diagrams on the site's dark surface when enlarged.
document.addEventListener("click", (event) => {
  const link = event.target.closest?.("a.project-figure-link");
  if (!link || event.ctrlKey || event.metaKey || event.shiftKey || event.altKey) return;
  event.preventDefault();
  const dialog = document.createElement("dialog");
  dialog.className = "figure-viewer";
  dialog.setAttribute("aria-label", "Full-size diagram");
  const toolbar = document.createElement("div");
  toolbar.className = "figure-viewer-bar";
  const download = document.createElement("a");
  download.href = link.href;
  download.download = "";
  download.textContent = "Download SVG";
  const close = document.createElement("button");
  close.type = "button";
  close.textContent = "Close";
  close.addEventListener("click", () => dialog.close());
  toolbar.append(download, close);
  const scroll = document.createElement("div");
  scroll.className = "figure-viewer-scroll";
  const image = document.createElement("img");
  image.src = link.href;
  image.alt = link.querySelector("img")?.alt || "Diagram";
  scroll.append(image);
  dialog.append(toolbar, scroll);
  document.body.append(dialog);
  dialog.addEventListener("close", () => { dialog.remove(); link.focus(); });
  dialog.addEventListener("click", (click) => { if (click.target === dialog) dialog.close(); });
  dialog.showModal();
});
