// Accessible client search (§12.5). No remote provider, no WebAssembly database,
// no fuzzy dependency, no network request outside the generated site: the whole
// index is one deterministic per-version JSON file, loaded lazily on first open.
// The dialog is a native <dialog> element, so modal focus trapping and Escape are
// browser-native; we add the shortcut/keyboard/ranking/grouping/highlight layer.

import { normalizeSearchText } from "./search_tokens.mjs";

const GROUP_ORDER = ["Tutorials", "SQL", "C++", "Pages"];
const MAX_RESULTS = 40;

function announce(message) {
  const region = document.getElementById("search-status");
  if (region) region.textContent = message;
}

// One index load per page, memoized as a promise so concurrent opens share it.
function makeLoader(url) {
  let promise = null;
  return () => {
    if (!promise) {
      promise = fetch(url, { credentials: "same-origin" })
        .then((response) => {
          if (!response.ok) throw new Error(`search index ${response.status}`);
          return response.json();
        })
        .then((data) => (Array.isArray(data.records) ? data.records : []))
        .catch(() => {
          promise = null; // allow a retry on the next open
          return null;
        });
    }
    return promise;
  };
}

function scoreRecord(record, normalizedQuery, queryTokens) {
  const name = record.name_tokens || "";
  const nameTokens = name.length ? name.split(" ") : [];
  const qualified = normalizeSearchText(record.qualified_name || "");
  const title = normalizeSearchText(record.title || "");
  if (normalizedQuery === qualified || normalizedQuery === title) return 0;
  if (name.startsWith(normalizedQuery) || nameTokens.some((t) => t.startsWith(normalizedQuery))) {
    return 1;
  }
  if (queryTokens.every((t) => name.includes(t))) return 2;
  const combined = `${name} ${record.text_tokens || ""}`;
  if (queryTokens.every((t) => combined.includes(t))) return 3;
  return -1;
}

function rankRecords(records, query) {
  const normalizedQuery = normalizeSearchText(query);
  if (normalizedQuery.length === 0) return [];
  const queryTokens = normalizedQuery.split(" ");
  const scored = [];
  for (const record of records) {
    const tier = scoreRecord(record, normalizedQuery, queryTokens);
    if (tier >= 0) scored.push({ record, tier });
  }
  scored.sort((a, b) => {
    if (a.tier !== b.tier) return a.tier - b.tier;
    const at = a.record.title || "";
    const bt = b.record.title || "";
    if (at.length !== bt.length) return at.length - bt.length;
    return at < bt ? -1 : at > bt ? 1 : 0;
  });
  return scored.slice(0, MAX_RESULTS).map((entry) => entry.record);
}

// Highlight query-token occurrences in `text`, building only DOM text nodes and
// <mark> elements (never innerHTML), so a hostile title cannot inject markup.
function highlightInto(target, text, queryTokens) {
  const lower = normalizeSearchText(text);
  // Map each normalized-token hit back to display offsets is lossy across
  // case/width changes; instead highlight on a case-insensitive display scan of
  // the ASCII-lowercased display string, which is adequate for Latin identifiers.
  const display = text;
  const scan = display.toLowerCase();
  const marks = [];
  for (const token of queryTokens) {
    if (!token) continue;
    let from = 0;
    for (;;) {
      const at = scan.indexOf(token, from);
      if (at < 0) break;
      marks.push([at, at + token.length]);
      from = at + token.length;
    }
  }
  if (marks.length === 0) {
    target.appendChild(document.createTextNode(display));
    return;
  }
  marks.sort((a, b) => a[0] - b[0]);
  const merged = [];
  for (const [s, e] of marks) {
    const last = merged[merged.length - 1];
    if (last && s <= last[1]) last[1] = Math.max(last[1], e);
    else merged.push([s, e]);
  }
  let cursor = 0;
  for (const [s, e] of merged) {
    if (s > cursor) target.appendChild(document.createTextNode(display.slice(cursor, s)));
    const mark = document.createElement("mark");
    mark.textContent = display.slice(s, e);
    target.appendChild(mark);
    cursor = e;
  }
  if (cursor < display.length) target.appendChild(document.createTextNode(display.slice(cursor)));
  void lower;
}

export function initSearch() {
  const dialog = document.getElementById("search-dialog");
  const button = document.getElementById("search-button");
  const input = document.getElementById("search-input");
  const results = document.getElementById("search-results");
  if (!dialog || !button || !input || !results) return;
  if (typeof dialog.showModal !== "function") return; // no-JS fallback link stays usable

  const indexUrl = dialog.dataset.searchIndex;
  const load = indexUrl ? makeLoader(indexUrl) : () => Promise.resolve([]);
  let records = null;
  let options = []; // flat list of result <a> elements for roving selection
  let selected = -1;
  let lastFocus = null;

  function setSelected(next) {
    if (options.length === 0) {
      selected = -1;
      input.removeAttribute("aria-activedescendant");
      return;
    }
    selected = (next + options.length) % options.length;
    options.forEach((option, index) => {
      const active = index === selected;
      option.setAttribute("aria-selected", active ? "true" : "false");
      if (active) {
        input.setAttribute("aria-activedescendant", option.id);
        option.scrollIntoView({ block: "nearest" });
      }
    });
  }

  function render(query) {
    results.replaceChildren();
    options = [];
    selected = -1;
    input.removeAttribute("aria-activedescendant");
    if (!records) {
      const loading = document.createElement("p");
      loading.className = "search-empty";
      loading.textContent = "Loading index…";
      results.appendChild(loading);
      return;
    }
    const ranked = rankRecords(records, query);
    if (query.trim().length === 0) {
      const hint = document.createElement("p");
      hint.className = "search-empty";
      hint.textContent = "Type to search tutorials, SQL, and C++ reference.";
      results.appendChild(hint);
      announce("");
      return;
    }
    if (ranked.length === 0) {
      const none = document.createElement("p");
      none.className = "search-empty";
      none.textContent = `No matches for “${query}”.`;
      results.appendChild(none);
      announce("No matches");
      return;
    }
    const queryTokens = normalizeSearchText(query).split(" ").filter(Boolean);
    const byGroup = new Map();
    for (const record of ranked) {
      const group = GROUP_ORDER.includes(record.group) ? record.group : "Pages";
      if (!byGroup.has(group)) byGroup.set(group, []);
      byGroup.get(group).push(record);
    }
    let optionIndex = 0;
    for (const group of GROUP_ORDER) {
      const groupRecords = byGroup.get(group);
      if (!groupRecords || groupRecords.length === 0) continue;
      const section = document.createElement("div");
      section.className = "search-group";
      const heading = document.createElement("p");
      heading.className = "search-group-title";
      heading.id = `search-group-${group.replace(/\W+/g, "-").toLowerCase()}`;
      heading.textContent = group;
      section.appendChild(heading);
      const list = document.createElement("ul");
      list.className = "search-group-list";
      list.setAttribute("role", "group");
      list.setAttribute("aria-labelledby", heading.id);
      for (const record of groupRecords) {
        const item = document.createElement("li");
        const link = document.createElement("a");
        link.className = "search-result";
        link.id = `search-option-${optionIndex}`;
        link.setAttribute("role", "option");
        link.setAttribute("aria-selected", "false");
        link.href = record.route;
        const titleEl = document.createElement("span");
        titleEl.className = "search-result-title";
        highlightInto(titleEl, record.title || record.qualified_name || record.route, queryTokens);
        link.appendChild(titleEl);
        if (record.qualified_name && record.qualified_name !== record.title) {
          const qn = document.createElement("span");
          qn.className = "search-result-qualified";
          qn.textContent = record.qualified_name;
          link.appendChild(qn);
        }
        if (record.summary) {
          const summary = document.createElement("span");
          summary.className = "search-result-summary";
          summary.textContent = record.summary;
          link.appendChild(summary);
        }
        item.appendChild(link);
        list.appendChild(item);
        options.push(link);
        optionIndex += 1;
      }
      section.appendChild(list);
      results.appendChild(section);
    }
    announce(`${ranked.length} result${ranked.length === 1 ? "" : "s"}`);
  }

  async function open() {
    if (dialog.open) return;
    lastFocus = document.activeElement;
    dialog.showModal();
    button.setAttribute("aria-expanded", "true");
    input.value = "";
    render("");
    input.focus();
    if (!records) {
      const loaded = await load();
      records = loaded || [];
      if (dialog.open) render(input.value);
    }
  }

  function close() {
    if (dialog.open) dialog.close();
  }

  button.addEventListener("click", open);

  dialog.addEventListener("close", () => {
    button.setAttribute("aria-expanded", "false");
    if (lastFocus && typeof lastFocus.focus === "function") lastFocus.focus();
  });

  // Explicit Escape-to-close so closing does not depend on the native dialog
  // cancel default alone (deterministic across browsers and for tests).
  dialog.addEventListener("keydown", (event) => {
    if (event.key === "Escape") {
      event.preventDefault();
      close();
    }
  });

  // Clicking the backdrop (outside the panel) closes the dialog.
  dialog.addEventListener("click", (event) => {
    if (event.target === dialog) close();
  });

  input.addEventListener("input", () => render(input.value));

  input.addEventListener("keydown", (event) => {
    if (event.key === "ArrowDown") {
      event.preventDefault();
      setSelected(selected + 1);
    } else if (event.key === "ArrowUp") {
      event.preventDefault();
      setSelected(selected - 1);
    } else if (event.key === "Enter") {
      // A bare Enter follows the first result rather than submitting the dialog
      // form (which would only close it).
      if (selected < 0 && options.length) selected = 0;
      if (selected >= 0 && options[selected]) {
        event.preventDefault();
        window.location.href = options[selected].href;
      }
    } else if (event.key === "Home" && options.length) {
      event.preventDefault();
      setSelected(0);
    } else if (event.key === "End" && options.length) {
      event.preventDefault();
      setSelected(options.length - 1);
    }
  });

  // Global shortcuts: Ctrl/Cmd+K anywhere; "/" only when not already typing.
  document.addEventListener("keydown", (event) => {
    const key = event.key;
    if ((event.ctrlKey || event.metaKey) && (key === "k" || key === "K")) {
      event.preventDefault();
      if (dialog.open) close();
      else open();
      return;
    }
    if (key === "/" && !dialog.open) {
      const target = event.target;
      const tag = target && target.tagName ? target.tagName.toLowerCase() : "";
      const typing = tag === "input" || tag === "textarea" || tag === "select" ||
        (target && target.isContentEditable);
      if (!typing) {
        event.preventDefault();
        open();
      }
    }
  });
}
