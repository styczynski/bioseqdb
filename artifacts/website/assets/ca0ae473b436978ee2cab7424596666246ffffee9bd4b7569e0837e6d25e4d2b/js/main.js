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
