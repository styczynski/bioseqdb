// A regular HTML POST remains usable when JavaScript is unavailable.
const form = document.querySelector('[data-contact-form]');
if (form) {
  const submit = form.querySelector('button[type="submit"]');
  const label = form.querySelector('[data-contact-submit-label]');
  const status = form.querySelector('[data-contact-status]');
  const idleLabel = label.textContent;
  let sending = false;
  const showStatus = (message, state) => {
    status.textContent = message;
    status.dataset.state = state;
    status.hidden = false;
    status.setAttribute('role', state === 'error' ? 'alert' : 'status');
  };
  form.addEventListener('submit', async (event) => {
    event.preventDefault();
    if (sending) return;
    if (!form.reportValidity()) {
      showStatus(form.dataset.validation, 'error');
      return;
    }
    sending = true;
    submit.disabled = true;
    label.textContent = form.dataset.sending;
    form.setAttribute('aria-busy', 'true');
    status.hidden = true;
    const controller = new AbortController();
    const timeout = setTimeout(() => controller.abort(), 30000);
    try {
      const fields = Object.fromEntries(new FormData(form).entries());
      const successUrl = new URL(form.dataset.successUrl, window.location.href);
      fields._redirect = successUrl.href;
      const response = await fetch(form.action, {
        method: 'POST',
        headers: { 'Content-Type': 'application/json', Accept: 'application/json' },
        body: JSON.stringify(fields),
        signal: controller.signal,
        credentials: 'omit',
      });
      const result = await response.json();
      // Formserve can report validation/configuration errors with HTTP 200.
      if (!response.ok || result.status !== 'success') throw new Error('Submission failed');
      showStatus(form.dataset.success, 'success');
      form.reset();
      // The project's requested confirmation page is also the native POST redirect.
      setTimeout(() => window.location.assign(successUrl.href), 650);
    } catch {
      showStatus(form.dataset.error, 'error');
      status.focus({ preventScroll: true });
      sending = false;
      submit.disabled = false;
      label.textContent = idleLabel;
    } finally {
      clearTimeout(timeout);
      form.removeAttribute('aria-busy');
    }
  });
}

const thanks = document.querySelector('[data-contact-thanks]');
if (thanks) {
  const countdown = thanks.querySelector('[data-contact-countdown]');
  const ring = thanks.querySelector('[data-countdown-ring]');
  const number = thanks.querySelector('[data-countdown-number]');
  const seconds = thanks.querySelector('[data-countdown-seconds]');
  const caption = thanks.querySelector('[data-countdown-caption]');
  const pausedCaption = thanks.querySelector('[data-countdown-paused]');
  const pauseButton = thanks.querySelector('[data-countdown-pause]');
  const reducedMotion = window.matchMedia('(prefers-reduced-motion: reduce)');
  const duration = 6000;
  let remaining = duration;
  let deadline = performance.now() + remaining;
  let paused = false;
  let completed = false;
  let frame;
  const render = () => {
    const count = Math.max(0, Math.ceil(remaining / 1000));
    number.textContent = String(count);
    seconds.textContent = String(count);
    // Reduced motion shows six discrete steps instead of a continuous sweep.
    const progress = reducedMotion.matches ? count / 6 : remaining / duration;
    ring.style.strokeDashoffset = String(100 * (1 - progress));
  };
  const tick = () => {
    if (paused || completed) return;
    remaining = Math.max(0, deadline - performance.now());
    render();
    if (remaining === 0) {
      completed = true;
      window.location.replace(new URL(thanks.dataset.homeUrl, window.location.href).href);
      return;
    }
    frame = requestAnimationFrame(tick);
  };
  pauseButton.addEventListener('click', () => {
    paused = !paused;
    pauseButton.textContent = paused ? pauseButton.dataset.resumeLabel : pauseButton.dataset.pauseLabel;
    caption.hidden = paused;
    pausedCaption.hidden = !paused;
    if (paused) {
      remaining = Math.max(0, deadline - performance.now());
      cancelAnimationFrame(frame);
      render();
    } else {
      deadline = performance.now() + remaining;
      tick();
    }
  });
  countdown.hidden = false;
  tick();
}
