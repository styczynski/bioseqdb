// Landing-page interactions (§15.2 / §15.3), ported semantically from
// design_source without the Design Component runtime. Every effect is a
// progressive enhancement: the page is fully readable with JavaScript disabled,
// and every motion effect is gated on prefers-reduced-motion.

const REDUCE_MOTION = window.matchMedia("(prefers-reduced-motion: reduce)").matches;

// Reveal-on-scroll: fade/slide elements in as they enter the viewport. The hidden
// initial state is only armed when JS runs AND motion is allowed, so no-JS and
// reduced-motion users always see the content immediately (§15.3).
function initReveal() {
  const targets = Array.from(document.querySelectorAll("[data-reveal]"));
  if (targets.length === 0) {
    return;
  }
  if (REDUCE_MOTION || typeof IntersectionObserver === "undefined") {
    for (const el of targets) {
      el.classList.add("is-revealed");
    }
    return;
  }
  for (const el of targets) {
    el.classList.add("reveal-armed");
  }
  const observer = new IntersectionObserver((entries) => {
    for (const entry of entries) {
      if (entry.isIntersecting) {
        entry.target.classList.add("is-revealed");
        observer.unobserve(entry.target);
      }
    }
  }, { rootMargin: "0px 0px -10% 0px", threshold: 0.12 });
  for (const el of targets) {
    observer.observe(el);
  }
}

// Callout connectors: aria-hidden SVG lines linking each hero-query annotation to
// the query code card. JS only measures positions; layout never depends on it
// (§15.3). Under reduced motion the lines are drawn statically (no dash animation).
function initConnectors() {
  const grid = document.querySelector("[data-connectors]");
  if (!grid) {
    return;
  }
  const svg = grid.querySelector(".connector-layer");
  const card = grid.querySelector("[data-connector-anchor]");
  const sources = Array.from(grid.querySelectorAll("[data-connector-source]"));
  if (!svg || !card || sources.length === 0) {
    return;
  }
  const SVG_NS = "http://www.w3.org/2000/svg";
  const draw = () => {
    const box = grid.getBoundingClientRect();
    if (box.width === 0 || box.height === 0) {
      return;
    }
    // Width is governed by CSS (width:100% of the grid) so the overlay can never
    // widen the page; only the coordinate system and height are set here.
    svg.setAttribute("viewBox", `0 0 ${box.width} ${box.height}`);
    svg.setAttribute("height", String(box.height));
    while (svg.firstChild) {
      svg.removeChild(svg.firstChild);
    }
    const cardBox = card.getBoundingClientRect();
    // Anchor on the card edge nearest the annotations.
    const cardIsRight = cardBox.left - box.left > box.width / 2;
    const targetX = (cardIsRight ? cardBox.left : cardBox.right) - box.left;
    for (const source of sources) {
      const sourceBox = source.getBoundingClientRect();
      const startX = (cardIsRight ? sourceBox.right : sourceBox.left) - box.left;
      const startY = sourceBox.top + sourceBox.height / 2 - box.top;
      const endY = Math.min(
        Math.max(startY, cardBox.top - box.top + 12),
        cardBox.bottom - box.top - 12);
      const midX = (startX + targetX) / 2;
      const path = document.createElementNS(SVG_NS, "path");
      path.setAttribute("d",
        `M ${startX} ${startY} C ${midX} ${startY}, ${midX} ${endY}, ${targetX} ${endY}`);
      path.setAttribute("class",
        REDUCE_MOTION ? "connector-path" : "connector-path connector-path-animated");
      svg.appendChild(path);
    }
  };
  draw();
  let frame = 0;
  const schedule = () => {
    if (frame) {
      cancelAnimationFrame(frame);
    }
    frame = requestAnimationFrame(draw);
  };
  window.addEventListener("resize", schedule);
  // Redraw once fonts/layout settle.
  window.addEventListener("load", draw);
}

// Architecture capability tracer: selecting a capability button highlights the
// architecture node it flows through. Real <button>s, aria-pressed reflects the
// single active selection, arrow keys move a roving tab stop (§15.2 buttons-not-
// divs, §15.4 keyboard-operable).
function initCapabilityTracer() {
  const section = document.querySelector("[data-capability-tracer]");
  if (!section) {
    return;
  }
  const buttons = Array.from(section.querySelectorAll(".capability-option"));
  const nodes = Array.from(section.querySelectorAll("[data-arch-node]"));
  if (buttons.length === 0) {
    return;
  }
  const select = (button) => {
    const traced = button.getAttribute("data-trace") || "";
    for (const other of buttons) {
      const on = other === button;
      other.setAttribute("aria-pressed", on ? "true" : "false");
      other.classList.toggle("is-selected", on);
      other.tabIndex = on ? 0 : -1;
    }
    for (const node of nodes) {
      node.classList.toggle("is-traced", node.getAttribute("data-arch-node") === traced);
    }
  };
  buttons.forEach((button, index) => {
    button.addEventListener("click", () => select(button));
    button.addEventListener("keydown", (event) => {
      let next = -1;
      if (event.key === "ArrowRight" || event.key === "ArrowDown") {
        next = (index + 1) % buttons.length;
      } else if (event.key === "ArrowLeft" || event.key === "ArrowUp") {
        next = (index - 1 + buttons.length) % buttons.length;
      } else if (event.key === "Home") {
        next = 0;
      } else if (event.key === "End") {
        next = buttons.length - 1;
      }
      if (next >= 0) {
        event.preventDefault();
        buttons[next].focus();
        select(buttons[next]);
      }
    });
  });
  select(buttons[0]);
}

// Workflow progress: a decorative bar that tracks how many steps have scrolled
// into view, plus a per-step active highlight. Under reduced motion every step is
// active and the bar is full immediately — progress changes are not animated
// (§15.3). The bar itself is aria-hidden; the steps remain an ordinary list.
function initWorkflowProgress() {
  const workflow = document.querySelector("[data-workflow]");
  if (!workflow) {
    return;
  }
  const fill = workflow.querySelector("[data-workflow-progress]");
  const steps = Array.from(workflow.querySelectorAll("[data-wf-step]"));
  if (steps.length === 0) {
    return;
  }
  const setProgress = (count) => {
    const ratio = Math.max(0, Math.min(1, count / steps.length));
    if (fill) {
      fill.style.width = `${(ratio * 100).toFixed(2)}%`;
    }
  };
  if (REDUCE_MOTION || typeof IntersectionObserver === "undefined") {
    for (const step of steps) {
      step.classList.add("is-active");
    }
    setProgress(steps.length);
    return;
  }
  const seen = new Set();
  const observer = new IntersectionObserver((entries) => {
    for (const entry of entries) {
      if (entry.isIntersecting) {
        entry.target.classList.add("is-active");
        seen.add(entry.target);
        observer.unobserve(entry.target);
      }
    }
    setProgress(seen.size);
  }, { rootMargin: "0px 0px -25% 0px", threshold: 0.5 });
  for (const step of steps) {
    observer.observe(step);
  }
}

// Decorative media band: a self-hosted, muted background loop without player UI.
// It never autoplays under reduced motion; otherwise
// it plays only while in view and pauses when scrolled away or the tab is hidden.
function initMediaBand() {
  const video = document.querySelector("video[data-media-band]");
  if (!video) {
    return;
  }
  if (REDUCE_MOTION || typeof IntersectionObserver === "undefined") {
    video.pause();
    return;
  }
  const safePlay = () => {
    const attempt = video.play();
    if (attempt && typeof attempt.catch === "function") {
      attempt.catch(() => {});
    }
  };
  const observer = new IntersectionObserver((entries) => {
    for (const entry of entries) {
      if (entry.isIntersecting && !document.hidden) {
        safePlay();
      } else {
        video.pause();
      }
    }
  }, { threshold: 0.35 });
  observer.observe(video);
  document.addEventListener("visibilitychange", () => {
    if (document.hidden) {
      video.pause();
    }
  });
}

// Moving DNA tiles behind the final CTA, matching the reference design. Rows
// drift at different speeds and recycle their leading tile; reduced-motion users
// receive the same grid as a still composition.
function initCtaGrid() {
  const wrap = document.getElementById("cta-grid");
  if (!wrap) return;
  const alphabet = "ACGTN";
  const dark = ["#0c2033", "#0e2739", "#0a1c2e", "#102a3f"];
  const tileSize = 46;
  const pitch = 52;
  let rows = [];
  let raf = 0;
  let last = 0;
  const pick = () => alphabet[(Math.random() * alphabet.length) | 0];
  const restyle = (tile) => {
    tile.textContent = pick();
    const hot = Math.random() < 0.19;
    tile.classList.toggle("is-hot", hot);
    tile.style.background = hot ? "#f26522" : dark[(Math.random() * dark.length) | 0];
  };
  const build = () => {
    const width = wrap.clientWidth;
    const height = wrap.clientHeight;
    if (!width || !height) return;
    wrap.replaceChildren();
    rows = [];
    const columns = Math.ceil(width / pitch) + 4;
    const rowCount = Math.ceil(height / pitch) + 1;
    for (let r = 0; r < rowCount; r += 1) {
      const row = document.createElement("div");
      row.className = "cta-grid-row";
      row.style.top = `${r * pitch}px`;
      const tiles = [];
      for (let c = 0; c < columns; c += 1) {
        const tile = document.createElement("span");
        tile.className = "cta-grid-tile";
        tile.style.left = `${(c - 1) * pitch}px`;
        tile.style.width = `${tileSize}px`;
        tile.style.height = `${tileSize}px`;
        restyle(tile);
        row.appendChild(tile);
        tiles.push(tile);
      }
      wrap.appendChild(row);
      rows.push({ row, tiles, shift: Math.random() * pitch, speed: 11 + Math.random() * 29 });
    }
  };
  const frame = (time) => {
    if (!last) last = time;
    const delta = Math.min(0.08, (time - last) / 1000);
    last = time;
    for (const item of rows) {
      item.shift += item.speed * delta;
      if (item.shift >= pitch) {
        item.shift -= pitch;
        for (let i = item.tiles.length - 1; i > 0; i -= 1) {
          item.tiles[i].textContent = item.tiles[i - 1].textContent;
          item.tiles[i].className = item.tiles[i - 1].className;
          item.tiles[i].style.background = item.tiles[i - 1].style.background;
        }
        restyle(item.tiles[0]);
      }
      item.row.style.transform = `translate3d(${item.shift.toFixed(2)}px,0,0)`;
    }
    raf = requestAnimationFrame(frame);
  };
  build();
  if (!REDUCE_MOTION) raf = requestAnimationFrame(frame);
  let resizeTimer = 0;
  window.addEventListener("resize", () => {
    window.clearTimeout(resizeTimer);
    resizeTimer = window.setTimeout(build, 160);
  }, { passive: true });
  document.addEventListener("visibilitychange", () => {
    if (REDUCE_MOTION) return;
    if (document.hidden && raf) {
      cancelAnimationFrame(raf);
      raf = 0;
    } else if (!document.hidden && !raf) {
      last = 0;
      raf = requestAnimationFrame(frame);
    }
  });
}

export function initLanding() {
  if (!document.querySelector(".landing-hero")) {
    return;
  }
  initReveal();
  initConnectors();
  initCapabilityTracer();
  initWorkflowProgress();
  initMediaBand();
  initCtaGrid();
}
