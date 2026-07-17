// Vanilla-JS translation of the interactions in bioseqdb-webpage/docs.html.

const root = document.querySelector('[data-screen-label="BioseqDB documentation"]');
const HEADER = 84;

function scrollToId(id) {
  const element = document.getElementById(id);
  if (!element) return false;
  const top = element.getBoundingClientRect().top + window.pageYOffset - HEADER;
  try { window.scrollTo({ top, behavior: "smooth" }); } catch { window.scrollTo(0, top); }
  try { history.replaceState(null, "", `#${id}`); } catch {}
  return true;
}

function reveal() {
  const elements = [...root.querySelectorAll("[data-reveal]")];
  if (!window.IntersectionObserver) { elements.forEach((el) => el.classList.add("rv-in")); return; }
  const observer = new IntersectionObserver((entries) => entries.forEach((entry) => {
    if (entry.isIntersecting) { entry.target.classList.add("rv-in"); observer.unobserve(entry.target); }
  }), { rootMargin: "0px 0px -7% 0px", threshold: 0.06 });
  elements.forEach((el) => observer.observe(el));
  window.setTimeout(() => elements.forEach((el) => el.classList.add("rv-in")), 1800);
}

function sidebar() {
  root.querySelectorAll("[data-chapter]").forEach((chapter) => {
    const list = chapter.querySelector("[data-chap-list]");
    const toggle = chapter.querySelector("[data-chap-toggle]");
    const caret = toggle?.querySelector("[data-caret]");
    const startsOpen = chapter.dataset.chap === "tutorials";
    list.style.display = startsOpen ? "block" : "none";
    if (caret) caret.style.transform = startsOpen ? "rotate(90deg)" : "none";
    toggle?.addEventListener("click", () => {
      const open = list.style.display !== "none";
      list.style.display = open ? "none" : "block";
      if (caret) caret.style.transform = open ? "none" : "rotate(90deg)";
    });
  });
  root.querySelectorAll('#sidebar [data-nav][data-page="tutorial-loading"]').forEach((link) => {
    link.style.color = "#eef4f9";
    link.style.background = "rgba(70,184,222,0.1)";
    link.style.borderLeftColor = "#46b8de";
    link.style.fontWeight = "600";
  });
}

function toc() {
  const list = document.getElementById("toc-list");
  const headings = [...root.querySelectorAll("#tutorial-loading [data-toc],#tutorial-loading [data-toc-sub]")];
  if (!list) return;
  list.replaceChildren();
  const entries = headings.map((heading) => {
    const sub = heading.hasAttribute("data-toc-sub");
    const link = document.createElement("a");
    link.href = `#${heading.id}`;
    link.textContent = heading.dataset.title || heading.textContent.replace(/#$/, "").trim();
    link.style.cssText = `display:block;padding:6px 0 6px ${sub ? "26px" : "14px"};margin-left:-1px;border-left:2px solid transparent;font-size:${sub ? "12.5px" : "13px"};line-height:1.4;color:#7d94a6;transition:color .15s,border-color .15s;text-decoration:none`;
    list.appendChild(link);
    link.addEventListener("click", (event) => { event.preventDefault(); scrollToId(heading.id); });
    return { heading, link, sub, leftLink: null };
  });
  root.querySelectorAll("#sidebar [data-inpage]").forEach((node) => node.remove());
  const activeLink = root.querySelector('#sidebar [data-nav][data-page="tutorial-loading"]');
  if (activeLink) {
    const box = document.createElement("div");
    box.dataset.inpage = "";
    box.style.cssText = "padding:2px 0 6px 22px;display:flex;flex-direction:column";
    entries.filter((entry) => !entry.sub).forEach((entry) => {
      const link = document.createElement("a");
      link.href = `#${entry.heading.id}`;
      link.textContent = entry.heading.dataset.title || entry.heading.id;
      link.style.cssText = "display:block;padding:5px 10px;border-left:2px solid transparent;font-size:12.5px;color:#6f8698;transition:color .15s,border-color .15s;text-decoration:none";
      box.appendChild(link);
      link.addEventListener("click", (event) => { event.preventDefault(); scrollToId(entry.heading.id); });
      entry.leftLink = link;
    });
    activeLink.insertAdjacentElement("afterend", box);
  }
  const visible = new Map();
  const update = () => {
    let active = entries.find((entry) => visible.get(entry.heading));
    if (!active) {
      active = entries[0];
      let bestTop = -Infinity;
      entries.forEach((entry) => {
        const top = entry.heading.getBoundingClientRect().top - (HEADER + 20);
        if (top <= 0 && top > bestTop) { bestTop = top; active = entry; }
      });
    }
    entries.forEach((entry) => {
      const on = entry === active;
      const link = entry.link;
      link.style.color = on ? "#7fcbe8" : "#7d94a6";
      link.style.borderLeftColor = on ? "#46b8de" : "transparent";
      if (entry.leftLink) {
        entry.leftLink.style.color = on ? "#cfe6f2" : "#6f8698";
        entry.leftLink.style.borderLeftColor = on ? "#46b8de" : "transparent";
      }
    });
  };
  if (window.IntersectionObserver) {
    const observer = new IntersectionObserver((changes) => {
      changes.forEach((change) => visible.set(change.target, change.isIntersecting));
      update();
    }, { root: null, rootMargin: "-84px 0px -66% 0px", threshold: 0 });
    entries.forEach((entry) => observer.observe(entry.heading));
  }
  let ticking = false;
  window.addEventListener("scroll", () => {
    if (ticking) return;
    ticking = true;
    requestAnimationFrame(() => { ticking = false; update(); });
  }, { passive: true });
  update();
}

const searchData = [
  { title: "Installation", type: "Tutorial", page: "installation", summary: "Requirements, packages, enabling the extension, building from source." },
  { title: "Quick start", type: "Tutorial", page: "quickstart", summary: "Create a table, load a FASTA file, run your first mmsearch." },
  { title: "Loading & searching sequences", type: "Tutorial", page: "tutorial-loading", summary: "The bioseq.seq type, streaming loads, mmsearch, and alignment inspection." },
  { title: "mmsearch()", type: "Function", page: "api-sql", anchor: "mmsearch-fn", summary: "Sequence-search predicate that activates a custom scan in WHERE." },
  { title: "bioseq.load()", type: "Function", page: "api-sql", anchor: "load-fn", summary: "Stream a FASTA/FASTQ file into a table." },
  { title: "bioseq.target_end()", type: "Function", page: "api-sql", anchor: "accessors", summary: "Match end offset on the target sequence." },
  { title: "bioseq.cigar()", type: "Function", page: "api-sql", anchor: "accessors", summary: "CIGAR string describing an alignment." },
  { title: "bioseq.len()", type: "Function", page: "api-sql", anchor: "len-fn", summary: "Number of bases in a stored sequence." },
  { title: "advance_iter()", type: "Function", page: "api-cpp", anchor: "member-functions", summary: "Advance a view iterator by view-unit steps via a shift." },
  { title: "bit_casted_view", type: "Class", page: "api-cpp", anchor: "synopsis", summary: "Reinterpret a compact_sequence_range as a different storage_unit width." },
  { title: "compact_sequence_range", type: "Class", page: "api-cpp", anchor: "notes", summary: "Packed, iterable view over a sequence buffer." },
  { title: "bioseq.seq", type: "Type", page: "api-sql", anchor: "seq-type", summary: "Column type storing a sequence in a chosen alphabet and k-mer width." },
  { title: "storage_unit", type: "Type", page: "api-cpp", anchor: "template-params", summary: "Underlying packed integer unit (uint32_t / uint64_t)." },
  { title: "sequence_custom_alphabet", type: "Type", page: "api-cpp", anchor: "notes", summary: "Alphabet packing policy mapping letters into a storage_unit." },
];

const escapeHtml = (value) => (value || "").replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;");
const highlight = (value, query) => {
  if (!query) return escapeHtml(value);
  const index = value.toLowerCase().indexOf(query);
  if (index < 0) return escapeHtml(value);
  return `${escapeHtml(value.slice(0, index))}<span class="doc-search-highlight">${escapeHtml(value.slice(index, index + query.length))}</span>${escapeHtml(value.slice(index + query.length))}`;
};

const typeMeta = {
  Tutorial: { glyph: "❯", color: "#46b8de", bg: "rgba(70,184,222,0.12)", border: "rgba(70,184,222,0.3)" },
  Function: { glyph: "ƒ", color: "#7fcbe8", bg: "rgba(70,184,222,0.1)", border: "rgba(70,184,222,0.26)" },
  Class: { glyph: "◇", color: "#f4894f", bg: "rgba(242,101,34,0.1)", border: "rgba(242,101,34,0.28)" },
  Type: { glyph: "T", color: "#8fd6ba", bg: "rgba(116,200,168,0.1)", border: "rgba(116,200,168,0.28)" },
};

function search() {
  const input = document.getElementById("doc-search");
  const dropdown = document.getElementById("search-dd");
  const box = root.querySelector("[data-search-box]");
  const wrap = root.querySelector("[data-search-wrap]");
  if (!input || !dropdown || !box || !wrap) return;
  const render = () => {
    const query = input.value.trim().toLowerCase();
    const matches = searchData.filter((item) => !query || item.title.toLowerCase().includes(query) || item.summary.toLowerCase().includes(query) || item.type.toLowerCase().includes(query));
    if (!matches.length) {
      dropdown.innerHTML = `<div class="doc-search-empty">No matches for <span>${escapeHtml(input.value)}</span></div>`;
      dropdown.style.display = "block";
      return;
    }
    const labels = { Tutorial: "Tutorials", Function: "Functions", Class: "Classes", Type: "Types" };
    dropdown.innerHTML = ["Tutorial", "Function", "Class", "Type"].map((type) => {
      const items = matches.filter((item) => item.type === type);
      if (!items.length) return "";
      const meta = typeMeta[type];
      const typeClass = type.toLowerCase();
      return `<div class="doc-search-group">${labels[type]}</div>${items.map((item) => `<a data-goto="${item.page}${item.anchor ? `#${item.anchor}` : ""}" href="${item.page === "tutorial-loading" ? "#tutorial-loading" : location.pathname}" data-search-result class="doc-search-result"><span class="doc-search-badge doc-search-${typeClass}">${meta.glyph}</span><span class="doc-search-text"><span class="doc-search-title">${highlight(item.title, query)}</span><span class="doc-search-summary">${escapeHtml(item.summary)}</span></span></a>`).join("")}`;
    }).join("");
    dropdown.querySelectorAll("[data-search-result]").forEach((link) => {
      link.addEventListener("mouseenter", () => { link.style.background = "rgba(70,184,222,0.09)"; });
      link.addEventListener("mouseleave", () => { link.style.background = "transparent"; });
    });
    dropdown.style.display = "block";
  };
  input.addEventListener("focus", () => { box.style.borderColor = "rgba(70,184,222,.55)"; box.style.background = "rgba(70,184,222,.06)"; render(); });
  input.addEventListener("input", render);
  document.addEventListener("click", (event) => {
    if (!wrap.contains(event.target)) { dropdown.style.display = "none"; box.style.borderColor = "rgba(255,255,255,.14)"; box.style.background = "rgba(255,255,255,.04)"; }
  });
  document.addEventListener("keydown", (event) => {
    if (event.key === "/" && document.activeElement !== input) { event.preventDefault(); input.focus(); }
    if ((event.ctrlKey || event.metaKey) && event.key.toLowerCase() === "k") { event.preventDefault(); input.focus(); }
    if (event.key === "Escape") { dropdown.style.display = "none"; input.blur(); box.style.borderColor = "rgba(255,255,255,.14)"; box.style.background = "rgba(255,255,255,.04)"; }
  });
  dropdown.addEventListener("click", () => {
    dropdown.style.display = "none"; input.value = "";
    box.style.borderColor = "rgba(255,255,255,.14)"; box.style.background = "rgba(255,255,255,.04)";
  });
}

function version() {
  const button = document.getElementById("ver-btn");
  const menu = document.getElementById("ver-menu");
  if (!button || !menu) return;
  const caret = button.querySelector("[data-caret]");
  button.addEventListener("click", (event) => {
    event.stopPropagation();
    const open = menu.style.display !== "block";
    menu.style.display = open ? "block" : "none";
    if (caret) caret.style.transform = open ? "rotate(180deg)" : "none";
  });
  document.addEventListener("click", () => { menu.style.display = "none"; if (caret) caret.style.transform = "none"; });
}

function copyButtons() {
  root.addEventListener("click", async (event) => {
    const button = event.target.closest("[data-copy]");
    if (!button) return;
    const code = button.closest("[data-term]")?.querySelector("[data-code]");
    if (!code) return;
    try { await navigator.clipboard.writeText(code.innerText); } catch {}
    const label = button.textContent; button.textContent = "Copied";
    button.style.background = "#8fd6ba";
    window.setTimeout(() => { button.textContent = label; button.style.background = "#e9f1f8"; }, 1400);
  });
}

const entities = {
  "mmsearch": { kind: "Function", signature: "boolean mmsearch(seq bioseq.seq, query bioseq.seq)", summary: "Sequence-search predicate; activates a custom scan when placed in a WHERE clause.", where: "bioseq — SQL", params: [["seq", "Stored sequence column to scan."], ["query", "Query sequence, cast to bioseq.seq(alphabet, k)."]] },
  "bioseq.load": { kind: "Function", signature: "bigint bioseq.load(rel regclass, path text, format text)", summary: "Streams a FASTA or FASTQ file into a table, mapping header fields to columns.", where: "bioseq — SQL", params: [["rel", "Target table (regclass)."], ["path", "Source file path on the server."], ["format", "'auto', 'fasta', or 'fastq'."]] },
  "bioseq.seq": { kind: "Type", signature: "bioseq.seq(alphabet text, k int)", summary: "Column type storing a biological sequence in a chosen alphabet and seed width.", where: "bioseq — SQL types", params: [["alphabet", "'dna4', 'dna5', 'aa27', …"], ["k", "Seed k-mer width, e.g. 64."]] },
};

const entityMeta = {
  Function: { color: "#7fcbe8", bg: "rgba(70,184,222,0.14)", border: "rgba(70,184,222,0.32)" },
  Class: { color: "#f4894f", bg: "rgba(242,101,34,0.13)", border: "rgba(242,101,34,0.3)" },
  Type: { color: "#8fd6ba", bg: "rgba(116,200,168,0.13)", border: "rgba(116,200,168,0.3)" },
};

function popovers() {
  const pop = document.getElementById("entity-pop");
  if (!pop) return;
  let timer = 0;
  const show = (anchor) => {
    window.clearTimeout(timer);
    const item = entities[anchor.dataset.entity];
    if (!item) return;
    const kindClass = item.kind.toLowerCase();
    const params = item.params.map(([name, detail]) => `<div class="entity-pop-param"><span class="entity-pop-param-name entity-pop-${kindClass}">${escapeHtml(name)}</span><span class="entity-pop-param-detail">${escapeHtml(detail)}</span></div>`).join("");
    pop.innerHTML = `<div class="entity-pop-head"><div class="entity-pop-meta"><span class="entity-pop-kind entity-pop-${kindClass}">${item.kind}</span><span class="entity-pop-where">${escapeHtml(item.where)}</span></div><pre>${escapeHtml(item.signature)}</pre></div><div class="entity-pop-body"><p>${escapeHtml(item.summary)}</p><div class="entity-pop-params">${params}</div><span class="entity-pop-view entity-pop-${kindClass}">View reference →</span></div>`;
    pop.style.display = "block";
    const rect = anchor.getBoundingClientRect();
    const width = 340;
    pop.style.left = `${Math.max(12, Math.min(rect.left, innerWidth - width - 12))}px`;
    const below = rect.bottom + 9;
    pop.style.top = `${below + pop.offsetHeight > innerHeight - 12 ? rect.top - pop.offsetHeight - 9 : below}px`;
    requestAnimationFrame(() => { pop.style.opacity = "1"; pop.style.transform = "none"; });
  };
  const hide = () => { timer = window.setTimeout(() => { pop.style.opacity = "0"; pop.style.transform = "translateY(6px)"; window.setTimeout(() => { if (pop.style.opacity === "0") pop.style.display = "none"; }, 160); }, 120); };
  root.querySelectorAll("[data-entity]").forEach((anchor) => {
    anchor.addEventListener("mouseenter", () => show(anchor)); anchor.addEventListener("mouseleave", hide);
    anchor.addEventListener("focus", () => show(anchor)); anchor.addEventListener("blur", hide);
  });
  pop.addEventListener("mouseenter", () => window.clearTimeout(timer)); pop.addEventListener("mouseleave", hide);
}

function responsive() {
  const grid = root.querySelector("[data-bodygrid]");
  const sidebarEl = document.getElementById("sidebar");
  const rail = root.querySelector("[data-rightrail]");
  const menu = root.querySelector("[data-menu]");
  const backdrop = root.querySelector("[data-backdrop]");
  let open = false;
  const apply = () => {
    const drawer = innerWidth < 760;
    const right = innerWidth >= 1000;
    rail.style.display = right ? "" : "none";
    grid.style.gridTemplateColumns = drawer ? "minmax(0,1fr)" : right ? "248px minmax(0,1fr) 210px" : "248px minmax(0,1fr)";
    menu.style.display = drawer ? "flex" : "none";
    if (drawer) {
      Object.assign(sidebarEl.style, { position: "fixed", top: "64px", left: "0", width: "284px", maxWidth: "82vw", zIndex: "76", background: "#081726", transform: open ? "none" : "translateX(-105%)", transition: "transform .22s cubic-bezier(.2,.7,.2,1)", boxShadow: open ? "0 30px 90px -20px rgba(0,0,0,0.75)" : "none" });
      backdrop.style.display = open ? "block" : "none";
    } else {
      Object.assign(sidebarEl.style, { position: "sticky", transform: "none", width: "", maxWidth: "", zIndex: "", background: "transparent", boxShadow: "none" });
      backdrop.style.display = "none"; open = false;
    }
  };
  menu.addEventListener("click", () => { open = !open; apply(); });
  backdrop.addEventListener("click", () => { open = false; apply(); });
  window.addEventListener("resize", apply); apply();
}

function navigation() {
  const docsUrl = root.querySelector('[data-nav][data-page="installation"]')?.href || location.pathname;
  root.addEventListener("click", (event) => {
    const noop = event.target.closest("[data-noop]");
    if (noop) { event.preventDefault(); return; }
    const goto = event.target.closest("[data-goto]");
    if (goto) {
      event.preventDefault();
      const [page, anchor] = goto.dataset.goto.split("#");
      if (page === "tutorial-loading") scrollToId(anchor || "tutorial-loading");
      else location.assign(`${docsUrl}#${anchor || page}`);
      return;
    }
    const current = event.target.closest('[data-nav][data-page="tutorial-loading"]');
    if (current) { event.preventDefault(); scrollToId("tutorial-loading"); return; }
    const jump = event.target.closest('a[href^="#"]');
    if (jump) {
      const id = jump.getAttribute("href").slice(1);
      if (id && document.getElementById(id)) { event.preventDefault(); scrollToId(id); }
    }
  });
}

if (root) {
  reveal(); sidebar(); toc(); search(); version(); copyButtons(); popovers(); responsive(); navigation();
  root.querySelector("[data-totop]")?.addEventListener("click", (event) => { event.preventDefault(); scrollTo({ top: 0, behavior: "smooth" }); });
}
