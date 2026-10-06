// Search and version navigation shared by the home, docs and benchmark pages.
import { normalizeSearchText } from "./search_tokens.mjs";
import { rankedSearch, searchDomain, searchType } from "./search-ranking.mjs";

const root = document.querySelector("[data-site-header]");
const SEARCH_GLYPHS = { Tutorial: "❯", Function: "ƒ", Class: "◇", Type: "T", Setting: "S", SQL: "S" };

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

function requireCurrentSchema(data, schema, schemaRevision, context) {
  if (!data || typeof data !== "object" || Array.isArray(data) ||
      data.schema !== schema || !Number.isInteger(data.schema_revision) ||
      data.schema_revision !== schemaRevision) {
    throw new Error(`${context} schema is not current`);
  }
  return data;
}

function search() {
  const input = document.getElementById("doc-search");
  const dropdown = document.getElementById("search-dd");
  const wrap = root.querySelector("[data-search-wrap]");
  const box = root.querySelector("[data-search-box]");
  if (!input || !dropdown || !wrap || !box || input.dataset.searchInitialized) return;
  input.dataset.searchInitialized = "true";
  const source = root.querySelector("[data-search-index]") ||
    document.querySelector("#search-dialog[data-search-index]") ||
    document.querySelector("[data-search-index]");
  const url = firstDataUrl([input, dropdown, wrap, source, root], ["searchIndex"]);
  const labels = {
    Tutorial: root.dataset.searchTutorial, Function: root.dataset.searchFunction,
    Class: root.dataset.searchClass, Type: root.dataset.searchType,
    Setting: root.dataset.searchSetting, SQL: root.dataset.searchSql,
  };
  const domains = {
    SQL: root.dataset.searchDomainSql,
    Documentation: root.dataset.searchDomainDocumentation,
    "C++": root.dataset.searchDomainCpp,
  };
  let records = null;
  let loadPromise = null;
  let options = [];
  let selected = -1;

  input.setAttribute("role", "combobox");
  input.setAttribute("aria-autocomplete", "list");
  input.setAttribute("aria-controls", dropdown.id);
  input.setAttribute("aria-expanded", "false");
  dropdown.setAttribute("role", "listbox");

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
      renderMessage(root.dataset.searchLoading || "");
      return;
    }
    const query = input.value;
    const matches = rankedSearch(records, query);
    dropdown.replaceChildren();
    options = [];
    selected = -1;
    input.removeAttribute("aria-activedescendant");
    if (!matches.length) {
      renderMessage((root.dataset.searchNoMatches || "") + " ", query);
      return;
    }
    let previousDomain = "";
    matches.forEach((record) => {
      const domain = searchDomain(record);
      const type = searchType(record);
      if (domain !== previousDomain) {
        const heading = document.createElement("div");
        heading.className = "doc-search-group";
        heading.textContent = domains[domain] || domain;
        dropdown.appendChild(heading);
        previousDomain = domain;
      }
      const link = document.createElement("a");
      link.className = "doc-search-result";
      link.href = record.route;
      link.id = `doc-search-option-${options.length}`;
      link.dataset.searchResult = "";
      link.dataset.searchId = record.id;
      link.dataset.searchKind = record.kind;
      link.dataset.searchDomain = domain;
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
      const titleRow = document.createElement("span");
      titleRow.className = "doc-search-title-row";
      const kindLabel = document.createElement("span");
      kindLabel.className = "doc-search-kind";
      kindLabel.textContent = labels[type] || type;
      titleRow.append(title, kindLabel);
      text.appendChild(titleRow);
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
          requireCurrentSchema(data, "bioseq.website.search", 2, "search index");
          if (!Array.isArray(data.records)) {
            throw new Error("search index records are invalid");
          }
          records = data.records;
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
      renderMessage(root.dataset.searchUnavailable || "");
      return;
    }
    render();
    try {
      await load();
      if (document.activeElement === input || wrap.contains(document.activeElement)) render();
    } catch {
      if (document.activeElement === input) renderMessage(root.dataset.searchError || "");
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
  if (!button || !menu || button.dataset.versionInitialized) return;
  button.dataset.versionInitialized = "true";
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
  const currentClass = currentTemplate?.className || "site-version-link";
  const regularClass = regularTemplate?.className || "site-version-link";
  const badgeClass = currentTemplate?.querySelector("span")?.className || "site-version-current";
  const currentLabel = currentTemplate?.querySelector("span")?.textContent || "";

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
      requireCurrentSchema(data, "bioseq.website.versions", 1, "versions");
      if (!Array.isArray(data.versions)) {
        throw new Error("versions records are invalid");
      }
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
          badge.textContent = currentLabel;
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

if (root) {
  search();
  version();
  const current = location.pathname.replace(/\/+$/, "/");
  root.querySelectorAll("[data-topnav] a").forEach((link) => {
    const route = new URL(link.dataset.navPrefix || link.href, location.href).pathname.replace(/\/+$/, "/");
    if (current.startsWith(route)) link.setAttribute("aria-current", "page");
  });
  const measure = () => document.documentElement.style.setProperty(
    "--site-header-height", `${Math.ceil(root.getBoundingClientRect().height)}px`);
  measure();
  if (window.ResizeObserver) new ResizeObserver(measure).observe(root);
  else window.addEventListener("resize", measure);
}
