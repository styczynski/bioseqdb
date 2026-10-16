/* Apply the saved choice before styles paint. Dark is the deliberate default. */
(() => {
  const key = "bioseq-theme";
  const root = document.documentElement;
  const read = () => {
    try { return localStorage.getItem(key) === "light" ? "light" : "dark"; }
    catch { return "dark"; }
  };
  const apply = (theme) => {
    root.dataset.theme = theme;
    const button = document.querySelector("[data-theme-toggle]");
    if (button) {
      const label = theme === "light" ? button.dataset.lightLabel : button.dataset.darkLabel;
      button.setAttribute("aria-label", label);
      button.setAttribute("title", label);
      button.setAttribute("aria-pressed", String(theme === "light"));
      const tooltip = button.querySelector("[data-theme-tooltip]");
      if (tooltip) tooltip.textContent = label;
    }
    document.dispatchEvent(new CustomEvent("bioseq:themechange", { detail: { theme } }));
  };
  apply(read());
  document.addEventListener("DOMContentLoaded", () => {
    apply(root.dataset.theme);
    document.querySelector("[data-theme-toggle]")?.addEventListener("click", () => {
      const theme = root.dataset.theme === "light" ? "dark" : "light";
      apply(theme);
      try { localStorage.setItem(key, theme); } catch { /* The toggle still works in this tab. */ }
    });
  }, { once: true });
  window.addEventListener("storage", (event) => {
    if (event.key === key || event.key === null) apply(read());
  });
})();
