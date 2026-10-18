import { linkPopover } from "./link-popover.mjs";

// Glossary definitions and profile actions use the same touch behavior as SQL.
const selector = 'a[data-glossary-description], a[data-social-description]';
const anchors = [...document.querySelectorAll(selector)];
if (anchors.length) {
  const pop = document.createElement('div');
  pop.id = 'blog-glossary-tip';
  pop.className = 'blog-glossary-tip';
  document.body.append(pop);
  linkPopover({
    pop, selector,
    render(content, anchor) {
      const title = document.createElement('strong');
      title.textContent = anchor.dataset.glossaryTerm || anchor.dataset.socialLabel;
      const description = document.createElement('span');
      description.textContent = anchor.dataset.glossaryDescription || anchor.dataset.socialDescription;
      content.replaceChildren(title, description);
    },
  });
  // The server-rendered title remains the no-JavaScript fallback.
  for (const anchor of anchors) anchor.removeAttribute('title');
}
