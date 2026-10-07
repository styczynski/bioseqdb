/* Google tag setup lives in a local file so the site keeps its no-inline-script CSP.
 * https://developers.google.com/tag-platform/gtagjs/reference
 */
window.dataLayer = window.dataLayer || [];
window.gtag = window.gtag || function () { window.dataLayer.push(arguments); };
window.gtag("js", new Date());
window.gtag("config", "G-989VYH5HT7", {
  allow_google_signals: false,
  allow_ad_personalization_signals: false,
});

(() => {
  const packageSource = (url) => {
    if ((url.hostname === "sourceforge.net" && url.pathname.startsWith("/projects/bioseqdb/")) ||
        (url.hostname.endsWith(".sourceforge.net") && url.pathname.startsWith("/project/bioseqdb/"))) return "sourceforge";
    if (url.hostname === "hub.docker.com" && /^\/r\/styczynski\/bioseqdb(?:\/|$)/.test(url.pathname)) return "docker_hub";
    if (url.hostname === "github.com" && /^\/styczynski\/bioseqdb\/releases(?:\/|$)/.test(url.pathname)) return "github_releases";
    return null;
  };
  const onLink = (event) => {
    if (event.type === "auxclick" && event.button !== 1) return;
    const link = event.target.closest?.("a[href]");
    if (!link) return;
    let url;
    try { url = new URL(link.href, location.href); } catch { return; }
    const source = packageSource(url);
    if (!source) return;
    window.gtag("event", "package_link_click", {
      package_source: source,
      package_path: url.pathname,
      link_location: document.body.classList.contains("landing-design-page") ? "landing" : "documentation",
      transport_type: "beacon",
    });
  };
  document.addEventListener("click", onLink);
  document.addEventListener("auxclick", onLink);
  document.addEventListener("bioseq:install-copy", (event) => {
    const command = /^\s*(?:\$\s*)?(docker\s+(?:pull|run))\b/.exec(event.detail?.command || "");
    if (!command) return;
    window.gtag("event", "docker_command_copy", {
      command: command[1].replace(/\s+/, " "),
      link_location: "landing",
    });
  });
})();
