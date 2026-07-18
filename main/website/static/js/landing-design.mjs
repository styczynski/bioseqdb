// Vanilla-JS translation of the interactions in website_design/index.html.
// Markup and styling are generated mechanically from that canonical export.

const root = document.querySelector('[data-screen-label="BioseqDB landing"]');
const motionPreference = window.matchMedia("(prefers-reduced-motion: reduce)");

function annotatedQueryHooks() {
  const code = root?.querySelector("[data-annotated-query]");
  if (!code) return;
  const hooks = [
    ["SELECT", "tok-select"],
    ["WHERE", "tok-where"],
    ["'dna4'", "tok-dna4"],
  ];
  hooks.forEach(([needle, id]) => {
    if (document.getElementById(id)) return;
    const walker = document.createTreeWalker(code, NodeFilter.SHOW_TEXT);
    for (let node = walker.nextNode(); node; node = walker.nextNode()) {
      const index = node.data.indexOf(needle);
      if (index < 0) continue;
      const parent = node.parentElement;
      if (parent && parent !== code && parent.textContent === needle) {
        parent.id = id;
      } else {
        const hook = document.createElement("span");
        hook.id = id;
        hook.textContent = needle;
        node.replaceWith(
          document.createTextNode(node.data.slice(0, index)),
          hook,
          document.createTextNode(node.data.slice(index + needle.length)),
        );
      }
      break;
    }
  });
  if (!code.querySelector("[data-query-cursor]")) {
    const cursor = document.createElement("span");
    cursor.dataset.queryCursor = "";
    cursor.className = "ld-s0063";
    cursor.setAttribute("aria-hidden", "true");
    code.appendChild(cursor);
  }
}

function reveal() {
  const elements = [...root.querySelectorAll("[data-reveal]")];
  if (motionPreference.matches || !window.IntersectionObserver) {
    elements.forEach((element) => element.classList.add("rv-in"));
    return;
  }
  const observer = new IntersectionObserver((entries) => {
    entries.forEach((entry) => {
      if (entry.isIntersecting) {
        entry.target.classList.add("rv-in");
        observer.unobserve(entry.target);
      }
    });
  }, { rootMargin: "0px 0px -7% 0px", threshold: 0.06 });
  elements.forEach((element) => observer.observe(element));
  window.setTimeout(() => elements.forEach((element) => element.classList.add("rv-in")), 3200);
}

function navigation() {
  const nav = document.getElementById("nav");
  if (!nav) return;
  const update = () => {
    const scrolled = window.scrollY > 48;
    nav.style.background = scrolled ? "rgba(7,19,32,0.82)" : "transparent";
    nav.style.borderBottomColor = scrolled ? "rgba(255,255,255,0.08)" : "transparent";
    nav.style.backdropFilter = scrolled ? "blur(14px)" : "none";
    nav.style.webkitBackdropFilter = scrolled ? "blur(14px)" : "none";
  };
  update();
  window.addEventListener("scroll", update, { passive: true });
}

function videos() {
  const kick = (video) => {
    if (motionPreference.matches) {
      video.pause();
      return;
    }
    const attempt = video.play();
    if (attempt && attempt.catch) attempt.catch(() => {});
  };
  const backgrounds = [...root.querySelectorAll("video[data-autobg]")];
  const applyPreference = () => {
    backgrounds.forEach((video) => {
      if (motionPreference.matches) video.pause();
      else if (!document.hidden) kick(video);
    });
  };
  backgrounds.forEach((video) => {
    video.muted = true; video.loop = true; video.playsInline = true; video.setAttribute("loop", "");
    video.addEventListener("ended", () => { try { video.currentTime = 0; } catch {} kick(video); });
    video.addEventListener("pause", () => {
      if (!motionPreference.matches && !document.hidden) {
        requestAnimationFrame(() => {
          if (video.paused && !document.hidden) kick(video);
        });
      }
    });
    kick(video);
  });
  window.setInterval(() => {
    if (motionPreference.matches || document.hidden) return;
    backgrounds.forEach((video) => {
      if (video.paused || video.ended) { try { if (video.ended) video.currentTime = 0; } catch {} kick(video); }
    });
  }, 1500);
  document.addEventListener("visibilitychange", () => {
    if (!document.hidden) applyPreference();
  });
  motionPreference.addEventListener?.("change", applyPreference);
}

function examples() {
  const tabs = [...root.querySelectorAll("[data-example-tab]")];
  const select = (index) => {
    root.querySelectorAll("[data-example-panel]").forEach((panel) => {
      panel.hidden = Number(panel.dataset.examplePanel) !== index;
    });
    tabs.forEach((tab) => {
      const active = Number(tab.dataset.exampleTab) === index;
      const labels = tab.querySelectorAll("span");
      if (labels[0]) labels[0].style.color = active ? "#f26522" : "#64798b";
      if (labels[1]) labels[1].style.color = active ? "#f2f7fb" : "#a3b7c9";
      tab.setAttribute("aria-pressed", active ? "true" : "false");
      tab.tabIndex = active ? 0 : -1;
    });
  };
  tabs.forEach((tab) => tab.addEventListener("click", () => select(Number(tab.dataset.exampleTab))));
  tabs.forEach((tab, index) => {
    tab.addEventListener("keydown", (event) => {
      if (event.key !== "ArrowRight" && event.key !== "ArrowLeft") return;
      event.preventDefault();
      const direction = event.key === "ArrowRight" ? 1 : -1;
      const next = (index + direction + tabs.length) % tabs.length;
      tabs[next].focus();
      select(Number(tabs[next].dataset.exampleTab));
    });
  });
  select(0);
  const copy = root.querySelector("[data-copy-install]");
  if (copy) {
    const done = copy.querySelector("[data-copy-done]");
    const idle = copy.querySelector("[data-copy-idle]");
    const idleLabel = idle?.querySelector("span") || idle;
    const original = idleLabel?.textContent || "Copy";
    let timer = 0;
    copy.setAttribute("aria-live", "polite");
    copy.addEventListener("click", async () => {
      let copied = false;
      try {
        await navigator.clipboard.writeText("CREATE EXTENSION bioseq;");
        copied = true;
      } catch {
        const area = document.createElement("textarea");
        area.value = "CREATE EXTENSION bioseq;";
        area.style.position = "fixed";
        area.style.opacity = "0";
        document.body.appendChild(area);
        area.select();
        try { copied = document.execCommand("copy"); } catch { copied = false; }
        area.remove();
      }
      window.clearTimeout(timer);
      if (done) done.hidden = !copied;
      if (idle) idle.hidden = copied;
      if (!copied && idleLabel) idleLabel.textContent = "Copy failed";
      timer = window.setTimeout(() => {
        if (done) done.hidden = true;
        if (idle) idle.hidden = false;
        if (idleLabel) idleLabel.textContent = original;
      }, 1800);
    });
  }
}

function connectors() {
  const wrap = document.getElementById("annotate");
  const svg = document.getElementById("annot-svg");
  const card = document.getElementById("query-card");
  if (!wrap || !svg || !card) return;
  const specs = [
    { tok: "tok-select", note: "note-select", color: "#46b8de" },
    { tok: "tok-where", note: "note-where", color: "#f26522" },
    { tok: "tok-dna4", note: "note-dna4", color: "#74c8a8" },
  ];
  let drawn = false;
  const animationTimers = new Set();
  const reduced = () => motionPreference.matches;
  const clearAnimationTimers = () => {
    animationTimers.forEach((timer) => window.clearTimeout(timer));
    animationTimers.clear();
  };
  const schedule = (callback, delay) => {
    const timer = window.setTimeout(() => {
      animationTimers.delete(timer);
      callback();
    }, delay);
    animationTimers.add(timer);
  };
  const rounded = (x, y, width, height, radius) => `M ${x + radius} ${y} H ${x + width - radius} A ${radius} ${radius} 0 0 1 ${x + width} ${y + radius} V ${y + height - radius} A ${radius} ${radius} 0 0 1 ${x + width - radius} ${y + height} H ${x + radius} A ${radius} ${radius} 0 0 1 ${x} ${y + height - radius} V ${y + radius} A ${radius} ${radius} 0 0 1 ${x + radius} ${y} Z`;
  const build = () => {
    const wrapBox = wrap.getBoundingClientRect();
    const width = wrap.clientWidth;
    const height = wrap.clientHeight;
    svg.setAttribute("viewBox", `0 0 ${width} ${height}`);
    if (width <= 820) { svg.hidden = true; svg.replaceChildren(); return; }
    svg.hidden = false;
    const cardRight = card.getBoundingClientRect().right - wrapBox.left;
    const paths = [];
    specs.forEach((spec) => {
      const token = document.getElementById(spec.tok)?.getBoundingClientRect();
      const note = document.getElementById(spec.note)?.getBoundingClientRect();
      if (!token || !note) return;
      const x = token.left - wrapBox.left - 6;
      const y = token.top - wrapBox.top - 4;
      const w = token.width + 12;
      const h = token.height + 8;
      const rowY = y + h / 2;
      const noteX = note.left - wrapBox.left - 3;
      const noteY = note.top - wrapBox.top + Math.min(28, note.height / 2);
      const railX = cardRight + Math.max(22, (noteX - cardRight) * 0.4);
      const index = specs.indexOf(spec);
      paths.push(`<path class="cx-rect" data-i="${index}" d="${rounded(x, y, w, h, 7)}" fill="none" stroke="${spec.color}" stroke-width="1.6"/>`);
      paths.push(`<path class="cx-dash" data-i="${index}" d="M ${cardRight + 2} ${rowY} L ${x + w + 2} ${rowY}" fill="none" stroke="${spec.color}" stroke-width="1.4" stroke-dasharray="2 4"/>`);
      paths.push(`<path class="cx-lead" data-i="${index}" d="M ${noteX} ${noteY} H ${railX} V ${rowY} H ${cardRight + 3}" fill="none" stroke="${spec.color}" stroke-width="1.8" stroke-linejoin="round" stroke-linecap="round"/>`);
      paths.push(`<path class="cx-arrow" data-i="${index}" d="M ${cardRight + 3} ${rowY} l 8 -4 M ${cardRight + 3} ${rowY} l 8 4" fill="none" stroke="${spec.color}" stroke-width="1.8" stroke-linecap="round"/>`);
      paths.push(`<circle class="cx-dot" data-i="${index}" cx="${noteX}" cy="${noteY}" r="3.4" fill="${spec.color}"/>`);
    });
    svg.innerHTML = paths.join("");
    svg.querySelectorAll(".cx-rect,.cx-lead").forEach((path) => {
      const length = path.getTotalLength ? path.getTotalLength() : 260;
      path.style.strokeDasharray = length;
      path.style.strokeDashoffset = drawn || reduced() ? 0 : length;
      path.style.transition = reduced() ? "none" :
        "stroke-dashoffset .9s cubic-bezier(.4,.55,.2,1)";
    });
    svg.querySelectorAll(".cx-arrow,.cx-dash").forEach((path) => {
      path.style.opacity = drawn || reduced() ?
        (path.classList.contains("cx-dash") ? 0.42 : 1) : 0;
      path.style.transition = reduced() ? "none" : "opacity .5s ease";
    });
    svg.querySelectorAll(".cx-dot").forEach((dot) => {
      dot.style.transformBox = "fill-box";
      dot.style.transformOrigin = "center";
      dot.style.opacity = drawn || reduced() ? 1 : 0;
      dot.style.transition = reduced() ? "none" : "opacity .5s ease";
      dot.style.animation = drawn && !reduced() ?
        "sqPulse 1.9s ease-in-out infinite" : "none";
    });
  };
  const finishWithoutMotion = () => {
    drawn = true;
    clearAnimationTimers();
    svg.querySelectorAll(".cx-rect,.cx-lead").forEach((path) => {
      path.style.transition = "none";
      path.style.strokeDashoffset = 0;
    });
    svg.querySelectorAll(".cx-arrow,.cx-dash,.cx-dot").forEach((path) => {
      path.style.transition = "none";
      path.style.opacity = path.classList.contains("cx-dash") ? 0.42 : 1;
      if (path.classList.contains("cx-dot")) path.style.animation = "none";
    });
  };
  const animate = () => {
    if (reduced()) {
      finishWithoutMotion();
      return;
    }
    drawn = true;
    svg.querySelectorAll(".cx-rect").forEach((path, index) => schedule(() => {
      path.style.strokeDashoffset = 0;
    }, index * 150));
    svg.querySelectorAll(".cx-lead").forEach((path, index) => schedule(() => {
      path.style.strokeDashoffset = 0;
    }, 180 + index * 150));
    svg.querySelectorAll(".cx-arrow,.cx-dash,.cx-dot").forEach((path) => {
      const index = Number(path.dataset.i || 0);
      schedule(() => {
        path.style.opacity = path.classList.contains("cx-dash") ? 0.42 : 1;
        if (path.classList.contains("cx-dot")) {
          path.style.animation = "sqPulse 1.9s ease-in-out infinite";
        }
      }, 520 + index * 150);
    });
  };
  build();
  window.addEventListener("resize", build, { passive: true });
  if (window.ResizeObserver) new ResizeObserver(build).observe(wrap);
  let observer = null;
  if (window.IntersectionObserver) {
    observer = new IntersectionObserver((entries) => entries.forEach((entry) => {
      if (!entry.isIntersecting) return;
      animate();
      observer.disconnect();
    }), { threshold: 0.2 });
    observer.observe(wrap);
  } else {
    animate();
  }
  motionPreference.addEventListener?.("change", () => {
    if (reduced()) {
      observer?.disconnect();
      finishWithoutMotion();
    } else {
      build();
    }
  });
  if (reduced()) {
    observer?.disconnect();
    finishWithoutMotion();
  }
}

function workflowAndIndex() {
  const fill = document.getElementById("wf-progress-fill");
  const grid = document.getElementById("wf-grid");
  const cards = grid ? [...grid.querySelectorAll("[data-wf-card]")] : [];
  const blocks = [...root.querySelectorAll("[data-idx-block]")];
  const clamp = (value, low, high) => Math.max(low, Math.min(high, value));
  let updateFrame = 0;
  const fitMedia = () => {
    blocks.forEach((block) => {
      const media = block.querySelector("[data-idx-media]");
      const inner = media?.firstElementChild;
      if (!inner) return;
      inner.style.transformOrigin = "left center";
      inner.style.zoom = "1";
      const available = media.clientWidth - 10;
      const natural = inner.scrollWidth || inner.getBoundingClientRect().width;
      if (natural > 0 && available > 0) inner.style.zoom = clamp(available / natural, 0.6, 1).toFixed(3);
    });
  };
  const paintCards = (active) => {
    cards.forEach((card, index) => {
      const number = card.querySelector("[data-wf-num]");
      card.style.background = index === active ? "rgba(242,101,34,0.09)" :
        index < active ? "rgba(242,101,34,0.035)" : "transparent";
      card.style.boxShadow = index === active ?
        "inset 0 0 0 1.5px rgba(242,101,34,0.55)" : "none";
      if (number) {
        number.style.color = index === active ? "#f26522" :
          index < active ? "rgba(242,101,34,0.62)" : "#2ba6d0";
      }
    });
  };
  const finishWithoutMotion = () => {
    if (updateFrame) cancelAnimationFrame(updateFrame);
    updateFrame = 0;
    if (fill) fill.style.width = "0%";
    if (cards.length) paintCards(0);
    blocks.forEach((block) => {
      const media = block.querySelector("[data-idx-media]");
      if (media) {
        media.style.transform = "none";
        media.style.opacity = "1";
      }
    });
  };
  const update = () => {
    updateFrame = 0;
    if (motionPreference.matches) {
      finishWithoutMotion();
      return;
    }
    const viewport = window.innerHeight || 800;
    if (grid && fill && cards.length) {
      const rect = grid.getBoundingClientRect();
      const progress = clamp((viewport * 0.82 - rect.top) / (viewport * 0.52), 0, 1);
      fill.style.width = `${(progress * 100).toFixed(2)}%`;
      const active = clamp(Math.floor(progress * cards.length - 1e-6), 0, cards.length - 1);
      paintCards(active);
    }
    blocks.forEach((block) => {
      const rect = block.getBoundingClientRect();
      const position = clamp((rect.top + rect.height / 2 - viewport / 2) / (viewport * 0.72), -1, 1);
      const side = block.dataset.side === "left" ? -1 : 1;
      const media = block.querySelector("[data-idx-media]");
      if (media) {
        media.style.transform = `translate3d(${(side * position * 42).toFixed(1)}px,0,0)`;
        media.style.opacity = (1 - clamp(Math.abs(position) - 0.5, 0, 0.5)).toFixed(3);
      }
    });
  };
  const scheduleUpdate = () => {
    if (motionPreference.matches) {
      finishWithoutMotion();
    } else if (!updateFrame) {
      updateFrame = requestAnimationFrame(update);
    }
  };
  window.addEventListener("scroll", scheduleUpdate, { passive: true });
  window.addEventListener("resize", () => { fitMedia(); scheduleUpdate(); }, { passive: true });
  motionPreference.addEventListener?.("change", scheduleUpdate);
  fitMedia(); window.setTimeout(fitMedia, 500); scheduleUpdate();
}

function capabilities() {
  const map = { vulkan: "gpu", simd: "cpu", parallel: "engine", columnar: "shard", mmsearch: "sql", seqmeta: "meta", index: "index", load: "pg" };
  const cards = [...root.querySelectorAll("[data-cap-select]")];
  let active = cards[0]?.dataset.cap || null;
  const sync = () => {
    cards.forEach((card) => {
      const on = card.dataset.cap === active;
      card.style.borderColor = on ? "rgba(70,184,222,0.6)" : "rgba(255,255,255,0.08)";
      card.style.background = on ? "rgba(70,184,222,0.08)" : "rgba(255,255,255,0.022)";
      card.style.boxShadow = on ? "0 0 0 1px rgba(70,184,222,0.45), 0 20px 46px -26px rgba(43,166,208,0.7)" : "none";
      card.setAttribute("aria-pressed", on ? "true" : "false");
      card.tabIndex = on ? 0 : -1;
    });
    const diagram = active ? map[active] : null;
    root.querySelectorAll("[data-diag]").forEach((group) => {
      const on = group.dataset.diag === diagram;
      group.querySelectorAll(".f").forEach((item) => { item.style.fill = on ? "url(#dgGrad)" : ""; });
      group.querySelectorAll(".s").forEach((item) => { item.style.stroke = on ? "#bfe9fb" : ""; });
      group.querySelectorAll(".ink").forEach((item) => { item.style.fill = on ? "#06222e" : ""; item.style.stroke = on ? "#06222e" : ""; });
      group.querySelectorAll(".t").forEach((item) => { item.style.fill = on ? "url(#dgGrad)" : ""; });
    });
    const file = document.getElementById("dgFile");
    if (file) file.style.opacity = diagram === "pg" ? "1" : "0";
  };
  cards.forEach((card, index) => {
    card.setAttribute("role", "button");
    const choose = () => { active = card.dataset.cap; sync(); };
    card.addEventListener("click", choose);
    card.addEventListener("keydown", (event) => {
      if (event.key === "Enter" || event.key === " ") {
        event.preventDefault(); choose();
      } else if (event.key === "ArrowRight" || event.key === "ArrowDown"
                 || event.key === "ArrowLeft" || event.key === "ArrowUp") {
        event.preventDefault();
        const direction = event.key === "ArrowRight" || event.key === "ArrowDown" ? 1 : -1;
        const next = cards[(index + direction + cards.length) % cards.length];
        active = next.dataset.cap;
        sync();
        next.focus();
      }
    });
  });
  root.querySelector("[data-architecture-click]")?.addEventListener("click", (event) => {
    const node = event.target.closest("[data-click]");
    if (node) { active = node.dataset.click; sync(); }
  });
  sync();
}

function ctaGrid() {
  const wrap = document.getElementById("cta-grid");
  if (!wrap) return;
  const alphabet = "ACGTN";
  const dark = ["#0c2033", "#0e2739", "#0a1c2e", "#102a3f"];
  const pitch = 52;
  let rows = [];
  let animation = 0;
  const styleTile = (tile) => {
    tile.textContent = alphabet[(Math.random() * alphabet.length) | 0];
    const hot = Math.random() < 0.19;
    tile.style.background = hot ? "#f26522" : dark[(Math.random() * dark.length) | 0];
    tile.style.color = hot ? "#fff" : "rgba(150,188,214,0.30)";
  };
  const build = () => {
    wrap.replaceChildren(); rows = [];
    const columns = Math.ceil(wrap.clientWidth / pitch) + 4;
    const count = Math.ceil(wrap.clientHeight / pitch) + 1;
    for (let r = 0; r < count; r += 1) {
      const row = document.createElement("div");
      row.className = "cta-grid-row";
      row.style.cssText = `position:absolute;left:0;top:${r * pitch}px;height:46px;will-change:transform`;
      const tiles = [];
      for (let c = 0; c < columns; c += 1) {
        const tile = document.createElement("div");
        tile.className = "cta-grid-tile";
        tile.style.cssText = `position:absolute;top:0;left:${(c - 1) * pitch}px;width:46px;height:46px;display:flex;align-items:center;justify-content:center;border-radius:6px;font-family:'IBM Plex Mono',monospace;font-size:19px;font-weight:600;line-height:1`;
        styleTile(tile); row.appendChild(tile); tiles.push(tile);
      }
      wrap.appendChild(row);
      rows.push({ row, tiles, shift: Math.random() * pitch, speed: 11 + Math.random() * 29 });
    }
  };
  let previous = 0;
  const frame = (time) => {
    if (motionPreference.matches) {
      animation = 0;
      return;
    }
    const delta = Math.min(0.08, previous ? (time - previous) / 1000 : 0); previous = time;
    rows.forEach((item) => {
      item.shift += item.speed * delta;
      while (item.shift >= pitch) {
        item.shift -= pitch;
        for (let index = item.tiles.length - 1; index > 0; index -= 1) {
          item.tiles[index].textContent = item.tiles[index - 1].textContent;
          item.tiles[index].style.background = item.tiles[index - 1].style.background;
          item.tiles[index].style.color = item.tiles[index - 1].style.color;
        }
        styleTile(item.tiles[0]);
      }
      item.row.style.transform = `translate3d(${item.shift.toFixed(2)}px,0,0)`;
    });
    animation = requestAnimationFrame(frame);
  };
  const start = () => {
    if (!motionPreference.matches && !animation) {
      previous = 0;
      animation = requestAnimationFrame(frame);
    }
  };
  const stop = () => { if (animation) { cancelAnimationFrame(animation); animation = 0; } };
  const applyMotionPreference = () => {
    if (motionPreference.matches) {
      stop();
      rows.forEach((item) => { item.row.style.transform = "none"; });
    } else {
      start();
    }
  };
  build(); window.setTimeout(build, 500); applyMotionPreference();
  if (window.IntersectionObserver) {
    const observer = new IntersectionObserver((entries) => entries.forEach((entry) => {
      if (entry.isIntersecting && !motionPreference.matches) start();
      else stop();
    }), { threshold: 0, rootMargin: "150px" });
    observer.observe(wrap);
  }
  let resizeTimer = 0;
  const resize = () => { window.clearTimeout(resizeTimer); resizeTimer = window.setTimeout(build, 180); };
  window.addEventListener("resize", resize, { passive: true });
  if (window.ResizeObserver) {
    let first = true;
    new ResizeObserver(() => { if (first) { first = false; return; } resize(); }).observe(wrap);
  }
  motionPreference.addEventListener?.("change", applyMotionPreference);
}

function teamWeb() {
  const hub = document.getElementById("who-hub");
  const svg = document.getElementById("who-svg");
  const center = document.getElementById("who-center");
  const grid = document.getElementById("who-grid");
  if (!hub || !svg || !center || !grid) return;
  const namespace = "http://www.w3.org/2000/svg";
  const accent = "#f26522";
  const clamp = (value, low, high) => Math.max(low, Math.min(high, value));
  const cards = [
    { element: document.getElementById("who-card-0"), side: "left" },
    { element: document.getElementById("who-card-1"), side: "left" },
    { element: document.getElementById("who-card-2"), side: "right" },
    { element: document.getElementById("who-card-3"), side: "right" },
  ].filter((card) => card.element);
  let connections = [];
  let lastSignature = "";
  let lastProgress = -1;
  let revealFrame = 0;

  const finishWithoutMotion = () => {
    if (revealFrame) cancelAnimationFrame(revealFrame);
    revealFrame = 0;
    connections.forEach((connection) => {
      connection.dots.forEach((dot) => {
        dot.element.style.transition = "none";
        dot.element.style.opacity = "1";
      });
      connection.end.style.transition = "none";
      connection.end.style.opacity = "1";
    });
    lastProgress = 1;
  };

  const reveal = () => {
    if (!connections.length) return;
    if (motionPreference.matches) {
      finishWithoutMotion();
      return;
    }
    const viewport = window.innerHeight || 800;
    const top = hub.getBoundingClientRect().top;
    const progress = clamp((viewport * 0.82 - top) / (viewport * 0.82 - viewport * 0.34), 0, 1);
    if (Math.abs(progress - lastProgress) < 0.0015) return;
    lastProgress = progress;
    connections.forEach((connection) => {
      connection.dots.forEach((dot) => {
        dot.element.style.transition = "opacity .2s ease";
      });
      connection.end.style.transition = "opacity .3s ease";
      const distance = progress * connection.total + 0.5;
      connection.dots.forEach((dot) => { dot.element.style.opacity = dot.distance <= distance ? "1" : "0"; });
      connection.end.style.opacity = progress >= 0.97 ? "1" : "0";
    });
  };

  const build = () => {
    const width = hub.clientWidth;
    const height = hub.clientHeight;
    svg.setAttribute("width", width); svg.setAttribute("height", height); svg.setAttribute("viewBox", `0 0 ${width} ${height}`);
    const wide = width >= 780;
    grid.style.gridTemplateColumns = wide ? "1fr minmax(230px,320px) 1fr" : "1fr";
    center.style.order = wide ? "0" : "-1";
    if (!wide) { svg.style.display = "none"; svg.replaceChildren(); connections = []; lastSignature = "narrow"; lastProgress = -1; return; }
    svg.style.display = "block";
    const hubRect = hub.getBoundingClientRect();
    const centerRect = center.getBoundingClientRect();
    const left = centerRect.left - hubRect.left;
    const right = centerRect.right - hubRect.left;
    const middle = centerRect.top - hubRect.top + centerRect.height / 2;
    const signature = `${width},${height},${Math.round(left)},${Math.round(right)},${Math.round(middle)}|${cards.map(({ element }) => { const rect = element.getBoundingClientRect(); return [Math.round(rect.top - hubRect.top), Math.round(rect.left - hubRect.left), Math.round(rect.width), Math.round(rect.height)].join(","); }).join(";")}`;
    if (signature === lastSignature) return;
    lastSignature = signature;
    const geometry = cards.map(({ element, side }) => {
      const rect = element.getBoundingClientRect();
      const cardMiddle = rect.top - hubRect.top + rect.height / 2;
      const direction = side === "left" ? -1 : 1;
      const start = direction < 0 ? left : right;
      const end = direction < 0 ? rect.right - hubRect.left : rect.left - hubRect.left;
      const rail = start + direction * Math.abs(end - start) * 0.58;
      return { path: `M ${start} ${middle} H ${rail} V ${cardMiddle} H ${end}`, end, endY: cardMiddle };
    });
    svg.replaceChildren();
    connections = geometry.map((item) => {
      const measure = document.createElementNS(namespace, "path");
      measure.setAttribute("d", item.path);
      const total = measure.getTotalLength ? measure.getTotalLength() : 300;
      const count = Math.max(2, Math.floor(total / 9));
      const dots = [];
      for (let index = 0; index <= count; index += 1) {
        const distance = Math.min(index * 9, total);
        const point = measure.getPointAtLength(distance);
        const dot = document.createElementNS(namespace, "rect");
        dot.setAttribute("x", (point.x - 1.5).toFixed(2)); dot.setAttribute("y", (point.y - 1.5).toFixed(2));
        dot.setAttribute("width", "3"); dot.setAttribute("height", "3"); dot.setAttribute("fill", accent);
        dot.style.opacity = motionPreference.matches ? "1" : "0";
        dot.style.transition = motionPreference.matches ? "none" : "opacity .2s ease";
        svg.appendChild(dot); dots.push({ element: dot, distance });
      }
      const end = document.createElementNS(namespace, "circle");
      end.setAttribute("cx", item.end.toFixed(2)); end.setAttribute("cy", item.endY.toFixed(2)); end.setAttribute("r", "4.2"); end.setAttribute("fill", accent);
      end.style.opacity = motionPreference.matches ? "1" : "0";
      end.style.transition = motionPreference.matches ? "none" : "opacity .3s ease";
      end.style.filter = "drop-shadow(0 0 5px rgba(242,101,34,0.75))";
      svg.appendChild(end);
      return { dots, end, total };
    });
    lastProgress = -1;
    reveal();
  };

  window.addEventListener("scroll", () => {
    if (motionPreference.matches || revealFrame) return;
    revealFrame = requestAnimationFrame(() => {
      revealFrame = 0;
      reveal();
    });
  }, { passive: true });
  window.addEventListener("resize", build, { passive: true });
  if (window.ResizeObserver) new ResizeObserver(build).observe(hub);
  motionPreference.addEventListener?.("change", () => {
    if (motionPreference.matches) {
      finishWithoutMotion();
    } else {
      lastProgress = -1;
      reveal();
    }
  });
  build(); window.setTimeout(build, 500); reveal();
}

if (root) {
  annotatedQueryHooks(); reveal(); navigation(); videos(); examples(); connectors(); workflowAndIndex(); capabilities(); teamWeb(); ctaGrid();
}
