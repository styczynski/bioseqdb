// Glossary definitions share one accessible popover, keeping prose uncluttered.
const anchors = [...document.querySelectorAll('a[data-glossary-description]')];
if (anchors.length) {
  const tip = document.createElement('div');
  tip.id = 'blog-glossary-tip';
  tip.className = 'blog-glossary-tip';
  tip.setAttribute('role', 'tooltip');
  tip.hidden = true;
  document.body.append(tip);
  let active = null;
  const position = () => {
    if (!active) return;
    const rect = active.getBoundingClientRect();
    if (rect.bottom < 0 || rect.top > innerHeight || rect.right < 0 || rect.left > innerWidth) {
      hide();
      return;
    }
    const width = tip.offsetWidth;
    const height = tip.offsetHeight;
    const left = Math.max(12, Math.min(innerWidth - width - 12, rect.left));
    const below = rect.bottom + 9;
    const preferred = below + height < innerHeight - 12 ? below : rect.top - height - 9;
    const top = Math.max(12, Math.min(innerHeight - height - 12, preferred));
    tip.style.left = `${left}px`;
    tip.style.top = `${top}px`;
  };
  const hide = () => {
    active?.removeAttribute('aria-describedby');
    active = null;
    tip.hidden = true;
  };
  const show = (anchor) => {
    hide();
    active = anchor;
    const title = document.createElement('strong');
    title.textContent = anchor.dataset.glossaryTerm;
    const description = document.createElement('span');
    description.textContent = anchor.dataset.glossaryDescription;
    tip.replaceChildren(title, description);
    tip.hidden = false;
    anchor.setAttribute('aria-describedby', tip.id);
    position();
  };
  for (const anchor of anchors) {
    // The server-rendered title remains the no-JavaScript fallback.
    anchor.removeAttribute('title');
    anchor.addEventListener('pointerenter', () => show(anchor));
    anchor.addEventListener('pointerleave', () => { if (active === anchor && document.activeElement !== anchor) hide(); });
    anchor.addEventListener('focus', () => show(anchor));
    anchor.addEventListener('blur', () => { if (active === anchor) hide(); });
  }
  document.addEventListener('keydown', (event) => { if (event.key === 'Escape') hide(); });
  window.addEventListener('resize', position);
  window.addEventListener('scroll', position, { passive: true });
}
