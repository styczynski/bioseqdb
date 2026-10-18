// Links keep their native keyboard/desktop behavior. A touch activation opens a
// persistent description with an explicit navigation action instead.
export function linkPopover({ pop, selector, load, render }) {
  let active = null;
  let persistent = false;
  let serial = 0;
  let hideTimer = 0;
  let touchTarget = null;
  let touchTime = 0;
  let restoringFocus = false;
  const closest = (target) => target instanceof Element ? target.closest(selector) : null;
  pop.classList.add("link-popover");
  pop.hidden = true;
  pop.setAttribute("role", "tooltip");
  pop.setAttribute("aria-hidden", "true");

  const hide = (restoreFocus = false) => {
    clearTimeout(hideTimer);
    ++serial; // Invalidate delayed data and animation callbacks.
    const previous = active;
    active = null;
    persistent = false;
    previous?.removeAttribute("aria-describedby");
    previous?.removeAttribute("aria-expanded");
    previous?.removeAttribute("aria-controls");
    pop.hidden = true;
    pop.style.display = "none";
    pop.style.opacity = "0";
    pop.setAttribute("aria-hidden", "true");
    pop.removeAttribute("aria-busy");
    if (restoreFocus && previous?.isConnected) {
      restoringFocus = true;
      previous.focus({ preventScroll: true });
      restoringFocus = false;
    }
  };
  const position = () => {
    if (!active) return;
    const rect = active.getBoundingClientRect();
    if (!persistent && (rect.bottom < 0 || rect.top > innerHeight || rect.right < 0 || rect.left > innerWidth)) {
      hide();
      return;
    }
    const width = pop.offsetWidth;
    const height = pop.offsetHeight;
    const left = Math.max(12, Math.min(innerWidth - width - 12, rect.left));
    const below = rect.bottom + 9;
    const top = below + height <= innerHeight - 12 ? below : rect.top - height - 9;
    pop.style.left = `${left}px`;
    pop.style.top = `${Math.max(12, Math.min(innerHeight - height - 12, top))}px`;
  };
  const show = async (anchor, pinned = false) => {
    if (persistent && !pinned) return;
    hide();
    document.dispatchEvent(new CustomEvent("bioseq:link-popover-open", { detail: pop }));
    active = anchor;
    persistent = pinned;
    const request = ++serial;
    const content = document.createElement("div");
    content.className = "link-popover-content";
    if (pinned) content.setAttribute("aria-live", "polite");
    pop.replaceChildren(content);
    pop.setAttribute("role", pinned ? "dialog" : "tooltip");
    pop.classList.toggle("is-persistent", pinned);
    if (pinned) {
      pop.setAttribute("aria-label", anchor.dataset.glossaryTerm || anchor.dataset.socialLabel || anchor.textContent.trim() || "Link details");
      anchor.setAttribute("aria-expanded", "true");
      anchor.setAttribute("aria-controls", pop.id);
      const actions = document.createElement("div");
      actions.className = "link-popover-actions";
      const more = document.createElement("a");
      more.className = "link-popover-more";
      more.textContent = "Read more →";
      for (const attribute of ["href", "target", "rel", "referrerpolicy"]) {
        if (anchor.hasAttribute(attribute)) more.setAttribute(attribute, anchor.getAttribute(attribute));
      }
      const close = document.createElement("button");
      close.type = "button";
      close.className = "link-popover-close";
      close.textContent = "Close";
      close.setAttribute("aria-label", "Close link details");
      close.addEventListener("click", () => hide(true));
      actions.append(more, close);
      pop.append(actions);
    } else {
      pop.removeAttribute("aria-label");
    }
    // Show a useful fallback immediately, including Read more, even while the
    // generated reference data is loading or the visitor is offline.
    const reveal = () => {
      pop.hidden = false;
      pop.style.display = pinned ? "flex" : "block";
      pop.setAttribute("aria-hidden", "false");
      anchor.setAttribute("aria-describedby", pop.id);
      position();
      requestAnimationFrame(() => {
        if (request !== serial || active !== anchor) return;
        pop.style.opacity = "1";
        pop.style.transform = "none";
      });
    };
    if (pinned || !load) {
      render(content, anchor, null, Boolean(load));
      reveal();
      if (pinned) pop.querySelector(".link-popover-more").focus({ preventScroll: true });
    }
    if (load) {
      pop.setAttribute("aria-busy", "true");
      let data = null;
      try { data = await load(anchor); } catch { /* Keep the usable fallback. */ }
      if (request !== serial || active !== anchor || !anchor.isConnected) return;
      render(content, anchor, data, false);
      pop.removeAttribute("aria-busy");
      if (pinned) position();
      else reveal();
    }
  };
  const scheduleHide = (preserveFocus = false) => {
    if (persistent || (preserveFocus && active?.contains(document.activeElement))) return;
    clearTimeout(hideTimer);
    hideTimer = setTimeout(() => hide(), 120);
  };
  document.addEventListener("pointerdown", (event) => {
    if (event.pointerType === "touch" || event.pointerType === "pen") {
      touchTarget = closest(event.target);
      touchTime = performance.now();
    }
    if (persistent && !pop.contains(event.target) && !active?.contains(event.target)) hide();
  }, true);
  document.addEventListener("click", (event) => {
    const anchor = closest(event.target);
    if (!anchor || event.defaultPrevented || event.button !== 0 || event.metaKey || event.ctrlKey || event.altKey || event.shiftKey) return;
    const touch = event.pointerType === "touch" || event.pointerType === "pen" ||
      event.sourceCapabilities?.firesTouchEvents ||
      (event.detail > 0 && !event.pointerType && touchTarget === anchor && performance.now() - touchTime < 1500) ||
      (event.detail > 0 && !event.pointerType && matchMedia("(hover: none) and (pointer: coarse)").matches);
    if (!touch) return; // Keyboard Enter and mouse clicks still follow the link.
    event.preventDefault();
    show(anchor, true);
  });
  document.addEventListener("pointerover", (event) => {
    if (event.pointerType === "touch" || event.pointerType === "pen") return;
    const anchor = closest(event.target);
    if (anchor && !anchor.contains(event.relatedTarget)) show(anchor);
  });
  document.addEventListener("pointerout", (event) => {
    const anchor = closest(event.target);
    if (anchor && !anchor.contains(event.relatedTarget)) scheduleHide(true);
  });
  document.addEventListener("focusin", (event) => {
    const anchor = closest(event.target);
    if (persistent && !pop.contains(event.target) && event.target !== active) hide();
    if (anchor && !restoringFocus) show(anchor);
  });
  document.addEventListener("focusout", (event) => {
    if (closest(event.target)) scheduleHide();
  });
  document.addEventListener("keydown", (event) => {
    if (event.key === "Escape" && active) {
      const restore = persistent && pop.contains(document.activeElement);
      hide(restore);
    }
  });
  document.addEventListener("bioseq:link-popover-open", (event) => {
    if (event.detail !== pop) hide();
  });
  window.addEventListener("resize", position);
  // Capture scrolls in code listings and independently scrolling docs panes too.
  document.addEventListener("scroll", position, { passive: true, capture: true });
}
