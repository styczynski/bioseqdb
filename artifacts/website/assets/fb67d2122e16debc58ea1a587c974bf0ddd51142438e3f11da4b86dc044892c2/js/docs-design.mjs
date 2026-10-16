// Vanilla-JS translation of the interactions in design_source/docs.html.
// The generated search, versions, and entity JSON files are the only data
// sources; this module contains presentation metadata, never documentation data.

const root = document.querySelector('[data-screen-label="BioseqDB documentation"]');

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
    link.style.color = current ? "var(--theme-eef4f9, #eef4f9)" : "var(--theme-9db1c2, #9db1c2)";
    link.style.background = current ? "var(--theme-46b8de-a0_1, rgba(70,184,222,0.1))" : "transparent";
    link.style.borderLeftColor = current ? "var(--theme-46b8de, #46b8de)" : "transparent";
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
      color: "var(--theme-7d94a6, #7d94a6)",
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
        color: "var(--theme-6f8698, #6f8698)",
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
      entry.link.style.color = current ? "var(--theme-7fcbe8, #7fcbe8)" : "var(--theme-7d94a6, #7d94a6)";
      entry.link.style.borderLeftColor = current ? "var(--theme-46b8de, #46b8de)" : "transparent";
      if (current) entry.link.setAttribute("aria-current", "location");
      else entry.link.removeAttribute("aria-current");
      if (entry.leftLink) {
        entry.leftLink.style.color = current ? "var(--theme-cfe6f2, #cfe6f2)" : "var(--theme-6f8698, #6f8698)";
        entry.leftLink.style.borderLeftColor = current ? "var(--theme-46b8de, #46b8de)" : "transparent";
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

function requireCurrentSchema(data, schema, schemaRevision, context) {
  if (!data || typeof data !== "object" || Array.isArray(data) ||
      data.schema !== schema || !Number.isInteger(data.schema_revision) ||
      data.schema_revision !== schemaRevision) {
    throw new Error(`${context} schema is not current`);
  }
  return data;
}

const entityDatasets = new Map();

function loadEntityDataset(url) {
  if (!entityDatasets.has(url)) {
    entityDatasets.set(url, fetch(url, { credentials: "same-origin" })
      .then((response) => {
        if (!response.ok) throw new Error(`entity data ${response.status}`);
        return response.json();
      })
      .then((data) => {
        requireCurrentSchema(
          data, "bioseq.website.entity_tooltips", 1, "entity tooltips");
        if (!data.entities || typeof data.entities !== "object" ||
            Array.isArray(data.entities)) {
          throw new Error("entity tooltip records are invalid");
        }
        return data.entities;
      })
      .catch(() => {
        entityDatasets.delete(url);
        return {};
      }));
  }
  return entityDatasets.get(url);
}

function entityKind(kind) {
  const value = String(kind || "").toLowerCase();
  if (value === "setting") {
    return { label: "Setting", tone: "type", color: "var(--theme-8fd6ba, #8fd6ba)",
      background: "var(--theme-74c8a8-a0_13, rgba(116,200,168,0.13))", border: "var(--theme-74c8a8-a0_3, rgba(116,200,168,0.3))" };
  }
  if (value === "sql_object") {
    return { label: "SQL object", tone: "type", color: "var(--theme-8fd6ba, #8fd6ba)",
      background: "var(--theme-74c8a8-a0_13, rgba(116,200,168,0.13))", border: "var(--theme-74c8a8-a0_3, rgba(116,200,168,0.3))" };
  }
  if (value.includes("function") || value === "overload_group") {
    return {
      label: "Function", tone: "function", color: "var(--theme-7fcbe8, #7fcbe8)",
      background: "var(--theme-46b8de-a0_14, rgba(70,184,222,0.14))", border: "var(--theme-46b8de-a0_32, rgba(70,184,222,0.32))",
    };
  }
  if (value.includes("type") || value.includes("alias") || value.includes("enum")) {
    return {
      label: "Type", tone: "type", color: "var(--theme-8fd6ba, #8fd6ba)",
      background: "var(--theme-74c8a8-a0_13, rgba(116,200,168,0.13))", border: "var(--theme-74c8a8-a0_3, rgba(116,200,168,0.3))",
    };
  }
  return {
    label: "Class", tone: "class", color: "var(--theme-f4894f, #f4894f)",
    background: "var(--theme-f26522-a0_13, rgba(242,101,34,0.13))", border: "var(--theme-f26522-a0_3, rgba(242,101,34,0.3))",
  };
}

function entityLocation(kind) {
  const value = String(kind || "").toLowerCase();
  return value.startsWith("sql_") || value === "overload_group" || value === "setting" ? "bioseq — SQL" : "bioseq — C++ reference";
}

function popovers() {
  const pop = document.getElementById("entity-pop");
  if (!pop || pop.dataset.initialized) return;
  pop.dataset.initialized = "true";
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
    if (record.setting) {
      const settings = document.createElement("div");
      settings.className = "entity-pop-params";
      for (const key of ["Type", "Default", "Minimum", "Maximum", "Unit", "Possible values"]) {
        const value = record.setting[key];
        if (value == null || value === "Not applicable") continue;
        const row = document.createElement("div");
        row.className = "entity-pop-param";
        const label = document.createElement("span");
        label.className = "entity-pop-param-name";
        label.textContent = key;
        const detail = document.createElement("span");
        detail.className = "entity-pop-param-detail";
        detail.textContent = value === "" ? "''" : value;
        row.append(label, detail);
        settings.appendChild(row);
      }
      body.appendChild(settings);
    }
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
    if (Array.isArray(record.fields) && record.fields.length) {
      const fields = document.createElement("div");
      fields.className = "entity-pop-params";
      record.fields.slice(0, 6).forEach((field) => {
        const row = document.createElement("div");
        row.className = "entity-pop-param";
        const name = document.createElement("code");
        name.className = "entity-pop-param-name";
        name.textContent = field.name;
        const detail = document.createElement("span");
        detail.className = "entity-pop-param-detail";
        detail.textContent = [field.type, field.description].filter(Boolean).join(" — ");
        row.append(name, detail);
        fields.appendChild(row);
      });
      body.appendChild(fields);
      if (record.fields.length > 6) {
        const more = document.createElement("p");
        more.className = "entity-pop-return";
        more.textContent = `${record.fields.length} fields · Open the reference for the full list.`;
        body.appendChild(more);
      }
    }
    if (Array.isArray(record.enum_values) && record.enum_values.length) {
      const values = document.createElement("p");
      values.className = "entity-pop-return";
      values.textContent = `Values: ${record.enum_values.join(", ")}`;
      body.appendChild(values);
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
      if (serial !== requestSerial || activeAnchor !== anchor || !anchor.isConnected) return;
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
  document.addEventListener("mouseover", (event) => {
    const anchor = closestAnchor(event.target);
    if (!anchor || anchor.contains(event.relatedTarget)) return;
    show(anchor);
  });
  document.addEventListener("mouseout", (event) => {
    const anchor = closestAnchor(event.target);
    if (!anchor || anchor.contains(event.relatedTarget)) return;
    scheduleHide();
  });
  document.addEventListener("focusin", (event) => {
    const anchor = closestAnchor(event.target);
    if (anchor) show(anchor);
  });
  document.addEventListener("focusout", (event) => {
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
    button.style.background = copied ? "var(--theme-8fd6ba, #8fd6ba)" : "var(--theme-e8a19b, #e8a19b)";
    button.style.color = "var(--theme-0e1a10, #0e1a10)";
    window.clearTimeout(timers.get(button));
    timers.set(button, window.setTimeout(() => {
      button.textContent = original;
      button.style.removeProperty("background");
      button.style.removeProperty("color");
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
    const drawer = innerWidth <= 768;
    const showRight = innerWidth >= 1300;
    if (rail) rail.style.display = showRight ? "" : "none";
    menu.style.display = drawer ? "flex" : "none";
    if (drawer) {
      Object.assign(sidebarElement.style, {
        position: "fixed",
        top: "64px",
        left: "0",
        width: "284px",
        maxWidth: "82vw",
        zIndex: "76",
        background: "var(--theme-081726, #081726)",
        transform: open ? "none" : "translateX(-105%)",
        transition: "transform .22s cubic-bezier(.2,.7,.2,1)",
        boxShadow: open ? "0 30px 90px -20px var(--theme-000000-a0_75, rgba(0,0,0,0.75))" : "none",
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
    if (innerWidth > 768) return;
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
  copyButtons();
  responsive();
  navigation();
}

// SQL references are shared by documentation, product examples and benchmarks.
popovers();
