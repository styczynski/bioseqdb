// Accessible tab toggler for the landing SQL showcase (§15.2, §15.4). Vanilla
// ES module, self-hosted, no framework and no external host. Progressive
// enhancement only: the markup ships every tabpanel visible, so with JavaScript
// disabled the examples stack and remain fully readable; this module takes over
// to expose a single panel at a time with roving keyboard focus. Nothing here is
// required to establish layout (§15.3).

// Wires one `[data-tabs]` widget: a `[role="tablist"]` of `[role="tab"]` buttons
// whose `aria-controls` names its `[role="tabpanel"]`.
function initTabWidget(widget) {
  const tabs = Array.from(widget.querySelectorAll('[role="tab"]'));
  if (tabs.length === 0) {
    return;
  }
  const panelFor = (tab) => document.getElementById(tab.getAttribute("aria-controls"));

  const select = (tab, { focus = true } = {}) => {
    for (const other of tabs) {
      const selected = other === tab;
      other.setAttribute("aria-selected", selected ? "true" : "false");
      other.tabIndex = selected ? 0 : -1;
      const panel = panelFor(other);
      if (panel) {
        panel.hidden = !selected;
      }
    }
    if (focus) {
      tab.focus();
    }
  };

  // Establish the enhanced initial state: first selected tab wins, else the first
  // tab. Only now do inactive panels become hidden, so no-JS users kept them all.
  const initial = tabs.find((tab) => tab.getAttribute("aria-selected") === "true") || tabs[0];
  select(initial, { focus: false });

  widget.addEventListener("click", (event) => {
    const tab = event.target.closest('[role="tab"]');
    if (tab && tabs.includes(tab)) {
      select(tab);
    }
  });

  widget.addEventListener("keydown", (event) => {
    const current = tabs.indexOf(document.activeElement);
    if (current === -1) {
      return;
    }
    let next = -1;
    if (event.key === "ArrowRight" || event.key === "ArrowDown") {
      next = (current + 1) % tabs.length;
    } else if (event.key === "ArrowLeft" || event.key === "ArrowUp") {
      next = (current - 1 + tabs.length) % tabs.length;
    } else if (event.key === "Home") {
      next = 0;
    } else if (event.key === "End") {
      next = tabs.length - 1;
    }
    if (next !== -1) {
      event.preventDefault();
      select(tabs[next]);
    }
  });
}

// Enhances every tab widget on the page. A no-op when none are present.
export function initTabs() {
  for (const widget of document.querySelectorAll("[data-tabs]")) {
    initTabWidget(widget);
  }
}
