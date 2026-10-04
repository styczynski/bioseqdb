/* Render the original LaTeX locally; no CDN or remote requests. */
document.querySelectorAll('[data-math]').forEach((node) => {
  if (!window.katex) return;
  window.katex.render(node.textContent, node, {
    displayMode: node.dataset.math === 'display',
    throwOnError: false,
    strict: 'ignore',
    trust: false,
    output: 'mathml',
    macros: {
      '\\ensuremath': '#1',
      '\\textsc': '\\mathrm{#1}',
      '\\Call': '\\operatorname{#1}(#2)',
    },
  });
});
