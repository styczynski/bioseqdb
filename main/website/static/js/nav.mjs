// Header navigation behavior (§15.1): the mobile-nav toggle and the version
// selector. Layout is CSS-only, so both widgets are progressive enhancements —
// the links stay reachable and the /versions/ page link is a real anchor if this
// module never runs.

// Mobile navigation: a real <button> with aria-expanded/aria-controls, focus
// management, Escape close, and outside-click close.
function initMobileNav() {
  const toggle = document.getElementById("nav-toggle");
  const panel = document.getElementById("nav-primary");
  if (!toggle || !panel) return;

  function setOpen(open) {
    toggle.setAttribute("aria-expanded", open ? "true" : "false");
    panel.classList.toggle("is-open", open);
    document.documentElement.classList.toggle("nav-open", open);
  }

  toggle.addEventListener("click", () => {
    const open = toggle.getAttribute("aria-expanded") === "true";
    setOpen(!open);
    if (!open) {
      const firstLink = panel.querySelector("a, button");
      if (firstLink) firstLink.focus();
    }
  });

  document.addEventListener("keydown", (event) => {
    if (event.key === "Escape" && toggle.getAttribute("aria-expanded") === "true") {
      setOpen(false);
      toggle.focus();
    }
  });

  document.addEventListener("click", (event) => {
    if (toggle.getAttribute("aria-expanded") !== "true") return;
    const target = event.target;
    if (target !== toggle && !toggle.contains(target) && !panel.contains(target)) {
      setOpen(false);
    }
  });
}

// Version selector: a native <details> disclosure (keyboard-operable for free).
// The list is server-rendered with the current version; here we merge in every
// version from versions.json so an assembled multi-version site shows them all,
// still degrading to the ordinary /versions/ link.
function initVersionSelector() {
  const list = document.getElementById("version-list");
  if (!list) return;
  const url = list.dataset.versions;
  if (!url) return;
  const current = list.querySelector("a[aria-current='true']");
  const currentHref = current ? current.getAttribute("href") : null;
  fetch(url, { credentials: "same-origin" })
    .then((response) => (response.ok ? response.json() : null))
    .then((data) => {
      if (!data || !Array.isArray(data.versions)) return;
      const fragment = document.createDocumentFragment();
      for (const version of data.versions) {
        if (!version || typeof version.route !== "string") continue;
        const item = document.createElement("li");
        const link = document.createElement("a");
        link.setAttribute("href", version.route);
        link.textContent = version.label || version.route;
        if (version.route === currentHref) link.setAttribute("aria-current", "true");
        item.appendChild(link);
        fragment.appendChild(item);
      }
      if (fragment.childElementCount > 0) list.replaceChildren(fragment);
    })
    .catch(() => {
      /* keep the server-rendered current version + /versions/ link */
    });
}

export function initNav() {
  initMobileNav();
  initVersionSelector();
}
