// Scroll from the beginning to the end only when the announcement overflows.
// The original text and links remain usable without JavaScript.
export function initAnnouncement(root) {
  if (!root) return;
  const viewport = root.querySelector('[data-announcement-window]');
  const content = root.querySelector('[data-announcement-text]');
  const button = root.querySelector('[data-announcement-toggle]');
  if (!viewport || !content || !button) return;
  const reduced = matchMedia('(prefers-reduced-motion: reduce)');
  let paused = false;
  let hovered = false;
  let interacting = false;
  let maximum = 0;
  let position = 0;
  let previous = 0;
  let holdUntil = performance.now() + 2200;
  let frame = 0;
  let wasRunning = false;
  const label = () => {
    button.setAttribute('aria-pressed', String(paused));
    button.title = paused ? button.dataset.resumeLabel : button.dataset.pauseLabel;
    button.setAttribute('aria-label', button.title);
  };
  const measure = () => {
    button.hidden = true;
    maximum = Math.max(0, content.scrollWidth - viewport.clientWidth);
    button.hidden = maximum <= 1 || reduced.matches;
    maximum = Math.max(0, content.scrollWidth - viewport.clientWidth);
    root.dataset.overflow = String(maximum > 1);
    position = 0;
    viewport.scrollLeft = 0;
    holdUntil = performance.now() + 2200;
  };
  const tick = (now) => {
    const elapsed = previous ? Math.min(now - previous, 80) : 0;
    previous = now;
    const focused = viewport.contains(document.activeElement);
    const running = !document.hidden && maximum > 1 && !reduced.matches && !paused && !hovered && !interacting && !focused && now >= holdUntil;
    if (running) {
      // Wheel scrolling and touch momentum may continue after their input
      // handlers return. Resume from the final browser position, not a stale
      // value captured before the default scroll action.
      if (!wasRunning) position = Math.max(0, Math.min(maximum, viewport.scrollLeft));
      if (position >= maximum) {
        position = 0;
        holdUntil = now + 2200;
      } else {
        position = Math.min(maximum, position + elapsed * .024);
        if (position >= maximum) holdUntil = now + 2200;
      }
      viewport.scrollLeft = position;
    }
    wasRunning = running;
    frame = requestAnimationFrame(tick);
  };
  button.addEventListener('click', () => { paused = !paused; label(); });
  root.addEventListener('pointerenter', (event) => { if (event.pointerType !== 'touch') hovered = true; });
  root.addEventListener('pointerleave', () => { hovered = false; });
  viewport.addEventListener('pointerdown', () => { interacting = true; });
  const endInteraction = () => { interacting = false; position = viewport.scrollLeft; holdUntil = performance.now() + 2200; };
  window.addEventListener('pointerup', endInteraction);
  window.addEventListener('pointercancel', endInteraction);
  viewport.addEventListener('wheel', () => { position = viewport.scrollLeft; holdUntil = performance.now() + 4000; }, { passive: true });
  root.addEventListener('focusout', () => { position = viewport.scrollLeft; holdUntil = performance.now() + 2200; });
  reduced.addEventListener('change', measure);
  window.addEventListener('resize', measure);
  document.fonts?.ready.then(measure);
  window.addEventListener('pagehide', () => cancelAnimationFrame(frame));
  window.addEventListener('pageshow', () => { cancelAnimationFrame(frame); previous = 0; frame = requestAnimationFrame(tick); });
  label();
  measure();
  frame = requestAnimationFrame(tick);
}

if (typeof document !== 'undefined') initAnnouncement(document.querySelector('[data-announcement]'));
