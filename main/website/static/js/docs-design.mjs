// Vanilla-JS translation of the interactions in website_design/docs.html.
// The generated search, versions, and entity JSON files are the only data
// sources; this module contains presentation metadata, never documentation data.

import { normalizeSearchText } from "./search_tokens.mjs";

const root = document.querySelector('[data-screen-label="BioseqDB documentation"]');
const MAX_SEARCH_RESULTS = 40;

function headerOffset() {
  const header = document.getElementById("dnav");
  return (header ? header.getBoundingClientRect().height : 64) + 20;
}

function getScroller() {
  let element = document.getElementById("doc-main");
  while (element) {
    const style = getComputedStyle(element);
    if (element.scrollHeight - element.clientHeight > 8 &&
        /(auto|scroll|overlay)/.test(style.overflowY)) {
      return element;
    }
    element = element.parentElement;
  }
  return document.scrollingElement || document.documentElement;
}

function isWindowScroller(scroller) {
  return scroller === document.scrollingElement ||
    scroller === document.documentElement || scroller === document.body;
}

function scrollToId(id, updateHash = true) {
  const element = document.getElementById(id);
  if (!element) return false;
  const scroller = getScroller();
  const offset = headerOffset();
  if (isWindowScroller(scroller)) {
    const top = element.getBoundingClientRect().top + window.scrollY - offset;
    try {
      window.scrollTo({ top, behavior: "smooth" });
    } catch {
      window.scrollTo(0, top);
    }
  } else {
    const top = element.getBoundingClientRect().top -
      scroller.getBoundingClientRect().top + scroller.scrollTop - offset;
    try {
      scroller.scrollTo({ top, behavior: "smooth" });
    } catch {
      scroller.scrollTop = top;
    }
  }
  if (updateHash) {
    try {
      history.replaceState(null, "", `#${encodeURIComponent(id)}`);
    } catch {
      // A hash update is optional progressive enhancement.
    }
  }
  return true;
}

function reveal() {
  const elements = [...root.querySelectorAll("[data-reveal]")];
  if (!window.IntersectionObserver) {
    elements.forEach((element) => element.classList.add("rv-in"));
    return;
  }
  const observer = new IntersectionObserver((entries) => {
    entries.forEach((entry) => {
      if (!entry.isIntersecting) return;
      entry.target.classList.add("rv-in");
      observer.unobserve(entry.target);
    });
  }, { rootMargin: "0px 0px -7% 0px", threshold: 0.06 });
  elements.forEach((element) => observer.observe(element));
  window.setTimeout(() => {
    elements.forEach((element) => element.classList.add("rv-in"));
  }, 1800);
}

function pageIdentity() {
  const main = document.getElementById("doc-main");
  return root.dataset.currentPage || main?.dataset.currentPage ||
    main?.querySelector("[data-page-panel]")?.dataset.pagePanel || "";
}

function pathMatchesCurrent(link) {
  try {
    const target = new URL(link.href, location.href);
    return target.origin === location.origin &&
      target.pathname.replace(/\/+$/, "") === location.pathname.replace(/\/+$/, "");
  } catch {
    return false;
  }
}

function activeSidebarLink() {
  const sidebar = document.getElementById("sidebar");
  if (!sidebar) return null;
  const explicit = sidebar.querySelector(
    '[data-doc-link][aria-current="page"],[data-nav][aria-current="page"],'
    + '[data-doc-link][data-current],[data-nav][data-current]'
  );
  if (explicit) return explicit;
  const identity = pageIdentity();
  if (identity) {
    const byIdentity = [...sidebar.querySelectorAll("[data-doc-link][data-page],[data-nav][data-page]")]
      .find((link) => link.dataset.page === identity);
    if (byIdentity) return byIdentity;
  }
  return [...sidebar.querySelectorAll("[data-doc-link][href],[data-nav][href]")]
    .find(pathMatchesCurrent) || null;
}

function setChapter(chapter, open) {
  const list = chapter.querySelector("[data-chap-list]");
  const toggle = chapter.querySelector("[data-chap-toggle]");
  const caret = toggle?.querySelector("[data-caret]");
  if (list) list.style.display = open ? "block" : "none";
  if (caret) caret.style.transform = open ? "rotate(90deg)" : "none";
  toggle?.setAttribute("aria-expanded", open ? "true" : "false");
}

function sidebar() {
  const active = activeSidebarLink();
  root.querySelectorAll("[data-chapter]").forEach((chapter, index) => {
    const toggle = chapter.querySelector("[data-chap-toggle]");
    const list = chapter.querySelector("[data-chap-list]");
    if (!toggle || !list) return;
    if (!list.id) {
      const hint = (chapter.dataset.chap || "docs")
        .toLowerCase().replace(/[^a-z0-9_-]+/g, "-").replace(/^-+|-+$/g, "") || "docs";
      list.id = `chapter-${hint}-${index + 1}`;
    }
    toggle.setAttribute("aria-controls", list.id);
    setChapter(chapter, Boolean(active && chapter.contains(active)));
    toggle.addEventListener("click", () => {
      setChapter(chapter, toggle.getAttribute("aria-expanded") !== "true");
    });
  });
  root.querySelectorAll("#sidebar [data-doc-link],#sidebar [data-nav]").forEach((link) => {
    const current = link === active;
    link.style.color = current ? "#eef4f9" : "#9db1c2";
    link.style.background = current ? "rgba(70,184,222,0.1)" : "transparent";
    link.style.borderLeftColor = current ? "#46b8de" : "transparent";
    link.style.fontWeight = current ? "600" : "400";
    if (current) link.setAttribute("aria-current", "page");
  });
}

function contentHeadings() {
  const main = document.getElementById("doc-main");
  if (!main) return [];
  // Generated Markdown cannot carry presentation markup. Recreate the supplied
  // `.hx` hash-link component without changing the generator's heading text/id.
  main.querySelectorAll(".docs-content h2[id],.docs-content h3[id]").forEach((heading) => {
    heading.classList.add("hx");
    heading.dataset.title ||= heading.textContent.trim();
    if (heading.tagName === "H3") heading.dataset.tocSub = "";
    else heading.dataset.toc = "";
    if (!heading.querySelector("[data-hash]")) {
      const hash = document.createElement("a");
      hash.dataset.hash = "";
      hash.className = "dd-s0052";
      hash.href = `#${heading.id}`;
      hash.textContent = "#";
      hash.setAttribute("aria-label", `Link to ${heading.dataset.title}`);
      heading.appendChild(hash);
    }
  });
  const annotated = [...main.querySelectorAll("[data-toc],[data-toc-sub]")]
    .filter((heading) => heading.id);
  if (annotated.length) return annotated;
  return [...main.querySelectorAll("h2[id],h3[id]")];
}

function toc() {
  const list = document.getElementById("toc-list");
  if (!list) return;
  const headings = contentHeadings();
  list.replaceChildren();
  const entries = headings.map((heading) => {
    const sub = heading.hasAttribute("data-toc-sub") || heading.tagName === "H3";
    const link = document.createElement("a");
    link.href = `#${heading.id}`;
    link.textContent = heading.dataset.title ||
      heading.textContent.replace(/#\s*$/, "").trim();
    Object.assign(link.style, {
      display: "block",
      padding: `6px 0 6px ${sub ? "26px" : "14px"}`,
      marginLeft: "-1px",
      borderLeft: "2px solid transparent",
      fontSize: sub ? "12.5px" : "13px",
      lineHeight: "1.4",
      color: "#7d94a6",
      transition: "color .15s,border-color .15s",
      textDecoration: "none",
    });
    list.appendChild(link);
    link.addEventListener("click", (event) => {
      event.preventDefault();
      scrollToId(heading.id);
    });
    return { heading, link, sub, leftLink: null };
  });

  root.querySelectorAll("#sidebar [data-inpage]").forEach((node) => node.remove());
  const active = activeSidebarLink();
  if (active && entries.some((entry) => !entry.sub)) {
    const box = document.createElement("div");
    box.dataset.inpage = "";
    Object.assign(box.style, {
      padding: "2px 0 6px 22px",
      display: "flex",
      flexDirection: "column",
    });
    entries.filter((entry) => !entry.sub).forEach((entry) => {
      const link = document.createElement("a");
      link.href = `#${entry.heading.id}`;
      link.textContent = entry.heading.dataset.title ||
        entry.heading.textContent.replace(/#\s*$/, "").trim();
      Object.assign(link.style, {
        display: "block",
        padding: "5px 10px",
        borderLeft: "2px solid transparent",
        fontSize: "12.5px",
        color: "#6f8698",
        transition: "color .15s,border-color .15s",
        textDecoration: "none",
      });
      link.addEventListener("click", (event) => {
        event.preventDefault();
        scrollToId(entry.heading.id);
      });
      box.appendChild(link);
      entry.leftLink = link;
    });
    active.insertAdjacentElement("afterend", box);
  }

  if (!entries.length) return;
  const visible = new Map();
  const update = () => {
    let selected = entries.find((entry) => visible.get(entry.heading));
    if (!selected) {
      selected = entries[0];
      let bestTop = -Infinity;
      entries.forEach((entry) => {
        const top = entry.heading.getBoundingClientRect().top - (headerOffset() + 20);
        if (top <= 0 && top > bestTop) {
          bestTop = top;
          selected = entry;
        }
      });
    }
    entries.forEach((entry) => {
      const current = entry === selected;
      entry.link.style.color = current ? "#7fcbe8" : "#7d94a6";
      entry.link.style.borderLeftColor = current ? "#46b8de" : "transparent";
      if (current) entry.link.setAttribute("aria-current", "location");
      else entry.link.removeAttribute("aria-current");
      if (entry.leftLink) {
        entry.leftLink.style.color = current ? "#cfe6f2" : "#6f8698";
        entry.leftLink.style.borderLeftColor = current ? "#46b8de" : "transparent";
      }
    });
  };
  if (window.IntersectionObserver) {
    const scroller = getScroller();
    const observer = new IntersectionObserver((changes) => {
      changes.forEach((change) => visible.set(change.target, change.isIntersecting));
      update();
    }, {
      root: isWindowScroller(scroller) ? null : scroller,
      rootMargin: "-84px 0px -66% 0px",
      threshold: 0,
    });
    entries.forEach((entry) => observer.observe(entry.heading));
  }
  const scroller = getScroller();
  const scrollTarget = isWindowScroller(scroller) ? window : scroller;
  let ticking = false;
  scrollTarget.addEventListener("scroll", () => {
    if (ticking) return;
    ticking = true;
    requestAnimationFrame(() => {
      ticking = false;
      update();
    });
  }, { passive: true });
  update();
}

const SEARCH_TYPES = ["Tutorial", "Function", "Class", "Type"];
const SEARCH_LABELS = {
  Tutorial: "Tutorials",
  Function: "Functions",
  Class: "Classes",
  Type: "Types",
};
const SEARCH_GLYPHS = { Tutorial: "❯", Function: "ƒ", Class: "◇", Type: "T" };

function searchType(record) {
  const kind = String(record.kind || "").toLowerCase();
  const group = String(record.group || "").toLowerCase();
  if (group === "tutorials" || group === "pages" ||
      kind === "tutorial" || kind === "page") return "Tutorial";
  if (kind.includes("function") || kind === "overload_group") return "Function";
  if (kind.includes("type") || kind.includes("alias") ||
      kind.includes("enum") || kind === "sql_schema") return "Type";
  return "Class";
}

function rankedSearch(records, query) {
  const normalized = normalizeSearchText(query);
  if (!normalized) return records.slice(0, MAX_SEARCH_RESULTS);
  const tokens = normalized.split(" ").filter(Boolean);
  const scored = [];
  records.forEach((record) => {
    const title = normalizeSearchText(record.title || "");
    const qualified = normalizeSearchText(record.qualified_name || "");
    const names = String(record.name_tokens || `${title} ${qualified}`);
    const text = String(record.text_tokens || normalizeSearchText(record.summary || ""));
    let tier = -1;
    if (title === normalized || qualified === normalized) tier = 0;
    else if (title.startsWith(normalized) || qualified.startsWith(normalized) ||
             names.split(" ").some((token) => token.startsWith(normalized))) tier = 1;
    else if (tokens.every((token) => names.includes(token))) tier = 2;
    else if (tokens.every((token) => `${names} ${text}`.includes(token))) tier = 3;
    if (tier >= 0) scored.push({ record, tier });
  });
  scored.sort((left, right) => left.tier - right.tier ||
    String(left.record.title || "").localeCompare(String(right.record.title || "")));
  return scored.slice(0, MAX_SEARCH_RESULTS).map((entry) => entry.record);
}

function appendHighlight(target, text, query) {
  const display = String(text || "");
  const raw = query.trim().toLowerCase();
  const candidates = [raw, ...normalizeSearchText(query).split(" ")].filter(Boolean);
  let start = -1;
  let length = 0;
  for (const candidate of candidates) {
    start = display.toLowerCase().indexOf(candidate);
    if (start >= 0) {
      length = candidate.length;
      break;
    }
  }
  if (start < 0) {
    target.textContent = display;
    return;
  }
  target.append(document.createTextNode(display.slice(0, start)));
  const mark = document.createElement("span");
  mark.className = "doc-search-highlight";
  mark.textContent = display.slice(start, start + length);
  target.append(mark, document.createTextNode(display.slice(start + length)));
}

function firstDataUrl(elements, names) {
  for (const element of elements) {
    if (!element) continue;
    for (const name of names) {
      if (element.dataset[name]) return element.dataset[name];
    }
  }
  return "";
}

function search() {
  const input = document.getElementById("doc-search");
  const dropdown = document.getElementById("search-dd");
  const wrap = root.querySelector("[data-search-wrap]");
  const box = root.querySelector("[data-search-box]");
  if (!input || !dropdown || !wrap || !box) return;
  const source = root.querySelector("[data-search-index]") ||
    document.querySelector("#search-dialog[data-search-index]") ||
    document.querySelector("[data-search-index]");
  const url = firstDataUrl([input, dropdown, wrap, source, root], ["searchIndex"]);
  let records = null;
  let loadPromise = null;
  let options = [];
  let selected = -1;

  input.setAttribute("role", "combobox");
  input.setAttribute("aria-autocomplete", "list");
  input.setAttribute("aria-controls", dropdown.id);
  input.setAttribute("aria-expanded", "false");
  dropdown.setAttribute("role", "listbox");
  dropdown.setAttribute("aria-label", "Search results");

  const setOpen = (open) => {
    dropdown.style.display = open ? "block" : "none";
    input.setAttribute("aria-expanded", open ? "true" : "false");
    if (!open) {
      selected = -1;
      input.removeAttribute("aria-activedescendant");
    }
  };
  const setFocusStyle = (focused) => {
    box.style.borderColor = focused ?
      "rgba(70,184,222,0.55)" : "rgba(255,255,255,0.14)";
    box.style.background = focused ?
      "rgba(70,184,222,0.06)" : "rgba(255,255,255,0.04)";
  };
  const setSelected = (next) => {
    if (!options.length) {
      selected = -1;
      input.removeAttribute("aria-activedescendant");
      return;
    }
    selected = (next + options.length) % options.length;
    options.forEach((option, index) => {
      const active = index === selected;
      option.setAttribute("aria-selected", active ? "true" : "false");
      option.style.background = active ? "rgba(70,184,222,0.09)" : "transparent";
      if (active) {
        input.setAttribute("aria-activedescendant", option.id);
        option.scrollIntoView({ block: "nearest" });
      }
    });
  };
  const renderMessage = (message, value = "") => {
    dropdown.replaceChildren();
    options = [];
    selected = -1;
    input.removeAttribute("aria-activedescendant");
    const empty = document.createElement("div");
    empty.className = "doc-search-empty";
    empty.append(document.createTextNode(message));
    if (value) {
      const display = document.createElement("span");
      display.textContent = value;
      empty.append(display);
    }
    dropdown.appendChild(empty);
    setOpen(true);
  };
  const render = () => {
    if (!records) {
      renderMessage("Loading search index…");
      return;
    }
    const query = input.value;
    const matches = rankedSearch(records, query);
    dropdown.replaceChildren();
    options = [];
    selected = -1;
    input.removeAttribute("aria-activedescendant");
    if (!matches.length) {
      renderMessage("No matches for ", query);
      return;
    }
    SEARCH_TYPES.forEach((type) => {
      const items = matches.filter((record) => searchType(record) === type);
      if (!items.length) return;
      const heading = document.createElement("div");
      heading.className = "doc-search-group";
      heading.textContent = SEARCH_LABELS[type];
      dropdown.appendChild(heading);
      items.forEach((record) => {
        const link = document.createElement("a");
        link.className = "doc-search-result";
        link.href = record.route;
        link.id = `doc-search-option-${options.length}`;
        link.dataset.searchResult = "";
        link.setAttribute("role", "option");
        link.setAttribute("aria-selected", "false");
        link.tabIndex = -1;
        const badge = document.createElement("span");
        badge.className = `doc-search-badge doc-search-${type.toLowerCase()}`;
        badge.setAttribute("aria-hidden", "true");
        badge.textContent = SEARCH_GLYPHS[type];
        const text = document.createElement("span");
        text.className = "doc-search-text";
        const title = document.createElement("span");
        title.className = "doc-search-title";
        appendHighlight(title, record.title || record.qualified_name || record.route, query);
        text.appendChild(title);
        if (record.summary) {
          const summary = document.createElement("span");
          summary.className = "doc-search-summary";
          summary.textContent = record.summary;
          text.appendChild(summary);
        }
        link.append(badge, text);
        const index = options.length;
        link.addEventListener("mouseenter", () => setSelected(index));
        link.addEventListener("focus", () => setSelected(index));
        link.addEventListener("click", () => setOpen(false));
        dropdown.appendChild(link);
        options.push(link);
      });
    });
    setOpen(true);
  };
  const load = () => {
    if (records) return Promise.resolve(records);
    if (!url) {
      records = [];
      return Promise.resolve(records);
    }
    if (!loadPromise) {
      loadPromise = fetch(url, { credentials: "same-origin" })
        .then((response) => {
          if (!response.ok) throw new Error(`search index ${response.status}`);
          return response.json();
        })
        .then((data) => {
          records = Array.isArray(data.records) ? data.records : [];
          return records;
        })
        .catch(() => {
          loadPromise = null;
          throw new Error("search index unavailable");
        });
    }
    return loadPromise;
  };
  const open = async () => {
    setFocusStyle(true);
    setOpen(true);
    if (!url) {
      renderMessage("Search index is unavailable.");
      return;
    }
    render();
    try {
      await load();
      if (document.activeElement === input || wrap.contains(document.activeElement)) render();
    } catch {
      if (document.activeElement === input) renderMessage("Search is temporarily unavailable.");
    }
  };
  const close = (blur = false) => {
    setOpen(false);
    setFocusStyle(false);
    if (blur) input.blur();
  };

  input.addEventListener("focus", open);
  input.addEventListener("input", () => {
    if (records) render();
    else open();
  });
  input.addEventListener("keydown", (event) => {
    if (event.key === "ArrowDown") {
      event.preventDefault();
      setSelected(selected + 1);
    } else if (event.key === "ArrowUp") {
      event.preventDefault();
      setSelected(selected < 0 ? options.length - 1 : selected - 1);
    } else if (event.key === "Home" && options.length) {
      event.preventDefault();
      setSelected(0);
    } else if (event.key === "End" && options.length) {
      event.preventDefault();
      setSelected(options.length - 1);
    } else if (event.key === "Enter" && options.length) {
      event.preventDefault();
      const option = options[selected < 0 ? 0 : selected];
      location.assign(option.href);
    } else if (event.key === "Escape") {
      event.preventDefault();
      close(true);
    }
  });
  wrap.addEventListener("focusout", (event) => {
    const next = event.relatedTarget;
    if (!(next instanceof Node) || !wrap.contains(next)) close();
  });
  document.addEventListener("click", (event) => {
    if (!wrap.contains(event.target)) close();
  });
  document.addEventListener("keydown", (event) => {
    const target = event.target;
    const tag = target?.tagName?.toLowerCase() || "";
    const typing = ["input", "textarea", "select"].includes(tag) || target?.isContentEditable;
    if ((event.ctrlKey || event.metaKey) && event.key.toLowerCase() === "k") {
      event.preventDefault();
      input.focus();
      input.select();
    } else if (event.key === "/" && !typing && !event.altKey && !event.ctrlKey && !event.metaKey) {
      event.preventDefault();
      input.focus();
    }
  });
}

function version() {
  const button = document.getElementById("ver-btn");
  const menu = document.getElementById("ver-menu");
  if (!button || !menu) return;
  const wrap = button.closest("[data-ver-wrap]");
  const caret = button.querySelector("[data-caret]");
  const source = document.querySelector("[data-versions-url],[data-versions]");
  const url = firstDataUrl(
    [menu, button, wrap, root, source],
    ["versionsUrl", "versions"]
  );
  const currentTemplate = menu.querySelector('[data-ver][aria-current="page"], [data-ver]');
  const allVersionsTemplate = menu.querySelector("[data-ver-all]")?.cloneNode(true);
  const regularTemplate = [...menu.querySelectorAll("[data-ver]")]
    .find((link) => link !== currentTemplate);
  const currentClass = currentTemplate?.className || "dd-s0026";
  const regularClass = regularTemplate?.className || "dd-s0028 dd-h001";
  const badgeClass = currentTemplate?.querySelector("span")?.className || "dd-s0027";

  button.setAttribute("aria-haspopup", "menu");
  button.setAttribute("aria-controls", menu.id);
  button.setAttribute("aria-expanded", "false");
  menu.setAttribute("role", "menu");
  const setOpen = (open) => {
    menu.style.display = open ? "block" : "none";
    button.setAttribute("aria-expanded", open ? "true" : "false");
    if (caret) caret.style.transform = open ? "rotate(180deg)" : "none";
  };
  const menuItems = () => [...menu.querySelectorAll('a[href]')];

  button.addEventListener("click", (event) => {
    event.stopPropagation();
    setOpen(button.getAttribute("aria-expanded") !== "true");
  });
  button.addEventListener("keydown", (event) => {
    if (event.key !== "ArrowDown" && event.key !== "ArrowUp") return;
    event.preventDefault();
    setOpen(true);
    const items = menuItems();
    items[event.key === "ArrowUp" ? items.length - 1 : 0]?.focus();
  });
  menu.addEventListener("keydown", (event) => {
    const items = menuItems();
    const index = items.indexOf(document.activeElement);
    if (event.key === "ArrowDown" && items.length) {
      event.preventDefault();
      items[(index + 1 + items.length) % items.length].focus();
    } else if (event.key === "ArrowUp" && items.length) {
      event.preventDefault();
      items[(index - 1 + items.length) % items.length].focus();
    } else if (event.key === "Home" && items.length) {
      event.preventDefault();
      items[0].focus();
    } else if (event.key === "End" && items.length) {
      event.preventDefault();
      items[items.length - 1].focus();
    } else if (event.key === "Escape") {
      event.preventDefault();
      setOpen(false);
      button.focus();
    }
  });
  document.addEventListener("click", (event) => {
    if (!wrap?.contains(event.target)) setOpen(false);
  });

  if (!url) return;
  fetch(url, { credentials: "same-origin" })
    .then((response) => {
      if (!response.ok) throw new Error(`versions ${response.status}`);
      return response.json();
    })
    .then((data) => {
      if (!Array.isArray(data.versions)) return;
      const fragment = document.createDocumentFragment();
      let foundCurrent = false;
      data.versions.forEach((record) => {
        if (!record || typeof record.route !== "string") return;
        const link = document.createElement("a");
        link.dataset.ver = "";
        link.href = record.route;
        link.setAttribute("role", "menuitem");
        const targetPath = new URL(record.route, location.href).pathname.replace(/\/+$/, "/");
        const currentPath = location.pathname.replace(/\/+$/, "/");
        const current = currentPath.startsWith(targetPath) && !foundCurrent;
        if (current) foundCurrent = true;
        link.className = current ? currentClass : regularClass;
        link.append(document.createTextNode(record.label || record.route));
        if (current) {
          link.setAttribute("aria-current", "page");
          const badge = document.createElement("span");
          badge.className = badgeClass;
          badge.textContent = "current";
          link.append(document.createTextNode(" "), badge);
        }
        fragment.appendChild(link);
      });
      if (fragment.childElementCount) {
        if (allVersionsTemplate instanceof Element) {
          allVersionsTemplate.setAttribute("role", "menuitem");
          fragment.appendChild(allVersionsTemplate);
        }
        menu.replaceChildren(fragment);
      }
    })
    .catch(() => {
      // Keep the server-rendered current-version link when the manifest is absent.
    });
}

const entityDatasets = new Map();

function loadEntityDataset(url) {
  if (!entityDatasets.has(url)) {
    entityDatasets.set(url, fetch(url, { credentials: "same-origin" })
      .then((response) => {
        if (!response.ok) throw new Error(`entity data ${response.status}`);
        return response.json();
      })
      .then((data) => data && typeof data.entities === "object" ? data.entities : {})
      .catch(() => {
        entityDatasets.delete(url);
        return {};
      }));
  }
  return entityDatasets.get(url);
}

function entityKind(kind) {
  const value = String(kind || "").toLowerCase();
  if (value.includes("function") || value === "overload_group") {
    return {
      label: "Function", tone: "function", color: "#7fcbe8",
      background: "rgba(70,184,222,0.14)", border: "rgba(70,184,222,0.32)",
    };
  }
  if (value.includes("type") || value.includes("alias") || value.includes("enum")) {
    return {
      label: "Type", tone: "type", color: "#8fd6ba",
      background: "rgba(116,200,168,0.13)", border: "rgba(116,200,168,0.3)",
    };
  }
  return {
    label: "Class", tone: "class", color: "#f4894f",
    background: "rgba(242,101,34,0.13)", border: "rgba(242,101,34,0.3)",
  };
}

function entityLocation(kind) {
  const value = String(kind || "").toLowerCase();
  return value.startsWith("sql_") ? "bioseq — SQL" : "bioseq — C++ reference";
}

function popovers() {
  const pop = document.getElementById("entity-pop");
  if (!pop) return;
  const selector = "[data-entity-id][data-entity-data-url]";
  let hideTimer = 0;
  let activeAnchor = null;
  let requestSerial = 0;
  pop.setAttribute("role", "tooltip");
  pop.setAttribute("aria-hidden", "true");

  const position = (anchor) => {
    const rect = anchor.getBoundingClientRect();
    const width = 340;
    const left = Math.max(12, Math.min(rect.left, innerWidth - width - 12));
    const below = rect.bottom + 9;
    const top = below + pop.offsetHeight > innerHeight - 12 ?
      rect.top - pop.offsetHeight - 9 : below;
    pop.style.left = `${left}px`;
    pop.style.top = `${Math.max(12, top)}px`;
  };
  const render = (record) => {
    const meta = entityKind(record.kind);
    const head = document.createElement("div");
    head.className = "entity-pop-head";
    const metadata = document.createElement("div");
    metadata.className = "entity-pop-meta";
    const kind = document.createElement("span");
    kind.className = `entity-pop-kind entity-pop-${meta.tone}`;
    kind.textContent = meta.label;
    kind.style.color = meta.color;
    kind.style.background = meta.background;
    kind.style.borderColor = meta.border;
    const where = document.createElement("span");
    where.className = "entity-pop-where";
    where.textContent = entityLocation(record.kind);
    metadata.append(kind, where);
    const signature = document.createElement("pre");
    signature.textContent = record.signature || record.qualified_name || record.title || "";
    head.append(metadata, signature);

    const body = document.createElement("div");
    body.className = "entity-pop-body";
    const summary = document.createElement("p");
    summary.textContent = record.summary || "View the generated reference for details.";
    body.appendChild(summary);
    if (Array.isArray(record.parameters) && record.parameters.length) {
      const parameters = document.createElement("div");
      parameters.className = "entity-pop-params";
      record.parameters.forEach((parameter) => {
        const row = document.createElement("div");
        row.className = "entity-pop-param";
        const name = document.createElement("span");
        name.className = `entity-pop-param-name entity-pop-${meta.tone}`;
        name.style.color = meta.color;
        const detail = document.createElement("span");
        detail.className = "entity-pop-param-detail";
        if (Array.isArray(parameter)) {
          name.textContent = parameter[0] || "";
          detail.textContent = parameter[1] || "";
        } else {
          name.textContent = parameter.name || "";
          const description = parameter.description || parameter.summary || "";
          detail.textContent = [parameter.type, description].filter(Boolean).join(" — ");
        }
        row.append(name, detail);
        parameters.appendChild(row);
      });
      body.appendChild(parameters);
    }
    if (record.return_type) {
      const returns = document.createElement("p");
      returns.className = "entity-pop-return";
      returns.append(document.createTextNode("Returns "));
      const type = document.createElement("code");
      type.textContent = record.return_type;
      returns.appendChild(type);
      body.appendChild(returns);
    }
    if (record.route) {
      const view = document.createElement("span");
      view.className = `entity-pop-view entity-pop-${meta.tone}`;
      view.textContent = "View reference →";
      view.style.color = meta.color;
      body.appendChild(view);
    }
    pop.replaceChildren(head, body);
  };
  const show = async (anchor) => {
    window.clearTimeout(hideTimer);
    const id = anchor.dataset.entityId;
    const url = anchor.dataset.entityDataUrl;
    if (!id || !url) return;
    const serial = ++requestSerial;
    if (activeAnchor && activeAnchor !== anchor) {
      activeAnchor.removeAttribute("aria-describedby");
    }
    activeAnchor = anchor;
    const records = await loadEntityDataset(url);
    if (serial !== requestSerial || activeAnchor !== anchor || !anchor.isConnected) return;
    const record = records[id];
    if (!record) return;
    render(record);
    pop.style.display = "block";
    pop.setAttribute("aria-hidden", "false");
    anchor.setAttribute("aria-describedby", pop.id);
    position(anchor);
    requestAnimationFrame(() => {
      pop.style.opacity = "1";
      pop.style.transform = "none";
    });
  };
  const hide = () => {
    window.clearTimeout(hideTimer);
    requestSerial += 1;
    if (activeAnchor) activeAnchor.removeAttribute("aria-describedby");
    activeAnchor = null;
    pop.style.opacity = "0";
    pop.style.transform = "translateY(6px)";
    pop.setAttribute("aria-hidden", "true");
    window.setTimeout(() => {
      if (pop.style.opacity === "0") pop.style.display = "none";
    }, 160);
  };
  const scheduleHide = () => {
    window.clearTimeout(hideTimer);
    hideTimer = window.setTimeout(hide, 120);
  };
  const closestAnchor = (target) => target instanceof Element ? target.closest(selector) : null;
  root.addEventListener("mouseover", (event) => {
    const anchor = closestAnchor(event.target);
    if (!anchor || anchor.contains(event.relatedTarget)) return;
    show(anchor);
  });
  root.addEventListener("mouseout", (event) => {
    const anchor = closestAnchor(event.target);
    if (!anchor || anchor.contains(event.relatedTarget)) return;
    scheduleHide();
  });
  root.addEventListener("focusin", (event) => {
    const anchor = closestAnchor(event.target);
    if (anchor) show(anchor);
  });
  root.addEventListener("focusout", (event) => {
    const anchor = closestAnchor(event.target);
    if (anchor && !anchor.contains(event.relatedTarget)) scheduleHide();
  });
  document.addEventListener("keydown", (event) => {
    if (event.key === "Escape" && activeAnchor) hide();
  });
  window.addEventListener("resize", () => {
    if (activeAnchor) position(activeAnchor);
  });
  window.addEventListener("scroll", () => {
    if (activeAnchor) position(activeAnchor);
  }, { passive: true });
}

function copyButtons() {
  const timers = new WeakMap();
  root.querySelectorAll("[data-copy]").forEach((button) => {
    button.setAttribute("aria-live", "polite");
  });
  root.addEventListener("click", async (event) => {
    const button = event.target.closest("[data-copy]");
    if (!button) return;
    const terminal = button.closest("[data-term]");
    const code = terminal?.querySelector("[data-code]");
    if (!code) return;
    const raw = code.textContent || "";
    const text = button.dataset.copyText || raw.replace(/^\s*\$\s?/gm, "").trim();
    let copied = false;
    try {
      await navigator.clipboard.writeText(text);
      copied = true;
    } catch {
      const area = document.createElement("textarea");
      area.value = text;
      area.style.position = "fixed";
      area.style.opacity = "0";
      document.body.appendChild(area);
      area.select();
      try { copied = document.execCommand("copy"); } catch { copied = false; }
      area.remove();
    }
    const original = button.dataset.copyLabel || button.textContent;
    button.dataset.copyLabel = original;
    button.textContent = copied ? "Copied" : "Copy failed";
    button.style.background = copied ? "#8fd6ba" : "#e8a19b";
    window.clearTimeout(timers.get(button));
    timers.set(button, window.setTimeout(() => {
      button.textContent = original;
      button.style.background = "#e9f1f8";
    }, 1400));
  });
}

function responsive() {
  const grid = root.querySelector("[data-bodygrid]");
  const sidebarElement = document.getElementById("sidebar");
  const rail = root.querySelector("[data-rightrail]");
  const menu = root.querySelector("[data-menu]");
  const backdrop = root.querySelector("[data-backdrop]");
  if (!grid || !sidebarElement || !menu || !backdrop) return;
  let open = false;
  let wasDrawer = false;
  menu.setAttribute("aria-controls", sidebarElement.id);
  menu.setAttribute("aria-expanded", "false");
  const apply = () => {
    const drawer = innerWidth < 760;
    const showRight = innerWidth >= 1000;
    if (rail) rail.style.display = showRight ? "" : "none";
    grid.style.gridTemplateColumns = drawer ? "minmax(0,1fr)" :
      showRight ? "248px minmax(0,1fr) 210px" : "248px minmax(0,1fr)";
    menu.style.display = drawer ? "flex" : "none";
    if (drawer) {
      Object.assign(sidebarElement.style, {
        position: "fixed",
        top: "64px",
        left: "0",
        width: "284px",
        maxWidth: "82vw",
        zIndex: "76",
        background: "#081726",
        transform: open ? "none" : "translateX(-105%)",
        transition: "transform .22s cubic-bezier(.2,.7,.2,1)",
        boxShadow: open ? "0 30px 90px -20px rgba(0,0,0,0.75)" : "none",
      });
      backdrop.style.display = open ? "block" : "none";
      backdrop.setAttribute("aria-hidden", open ? "false" : "true");
      menu.setAttribute("aria-expanded", open ? "true" : "false");
    } else {
      Object.assign(sidebarElement.style, {
        position: "sticky",
        transform: "none",
        width: "",
        maxWidth: "",
        zIndex: "",
        background: "transparent",
        boxShadow: "none",
      });
      backdrop.style.display = "none";
      backdrop.setAttribute("aria-hidden", "true");
      menu.setAttribute("aria-expanded", "false");
      open = false;
    }
    wasDrawer = drawer;
  };
  const setOpen = (next, focus = false) => {
    if (innerWidth >= 760) return;
    open = next;
    apply();
    if (open && focus) sidebarElement.querySelector("a,button")?.focus();
  };
  menu.addEventListener("click", () => setOpen(!open, !open));
  backdrop.addEventListener("click", () => {
    setOpen(false);
    menu.focus();
  });
  sidebarElement.addEventListener("click", (event) => {
    if (wasDrawer && event.target.closest("a[href]")) setOpen(false);
  });
  document.addEventListener("keydown", (event) => {
    if (event.key === "Escape" && open) {
      setOpen(false);
      menu.focus();
    }
  });
  let resizeFrame = 0;
  window.addEventListener("resize", () => {
    cancelAnimationFrame(resizeFrame);
    resizeFrame = requestAnimationFrame(apply);
  });
  apply();
}

function navigation() {
  root.addEventListener("click", (event) => {
    const noop = event.target.closest("[data-noop]");
    if (noop) {
      event.preventDefault();
      return;
    }
    const top = event.target.closest("[data-totop]");
    if (top) {
      event.preventDefault();
      const scroller = getScroller();
      if (isWindowScroller(scroller)) window.scrollTo({ top: 0, behavior: "smooth" });
      else scroller.scrollTo({ top: 0, behavior: "smooth" });
      return;
    }
    const link = event.target.closest('a[href^="#"]');
    if (!link) return;
    const id = decodeURIComponent(link.getAttribute("href").slice(1));
    if (id && document.getElementById(id)) {
      event.preventDefault();
      scrollToId(id);
    }
  });
  window.addEventListener("hashchange", () => {
    const id = decodeURIComponent(location.hash.replace(/^#/, ""));
    if (id) scrollToId(id, false);
  });
  if (location.hash) {
    const id = decodeURIComponent(location.hash.slice(1));
    requestAnimationFrame(() => requestAnimationFrame(() => scrollToId(id, false)));
  }
}

if (root) {
  reveal();
  sidebar();
  toc();
  search();
  version();
  copyButtons();
  popovers();
  responsive();
  navigation();
}
