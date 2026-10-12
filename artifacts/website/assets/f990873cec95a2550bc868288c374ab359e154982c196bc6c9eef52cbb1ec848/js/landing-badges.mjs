/* Images use the saved site theme, which can differ from the OS preference. */
const group = document.querySelector('[data-project-badges]');
if (group) {
  const wide = window.matchMedia('(min-width: 1600px)');
  const images = [...group.querySelectorAll('[data-badge-image]')];
  for (const img of images) {
    const link = img.closest('[data-project-badge]');
    img.addEventListener('error', () => { link.dataset.unavailable = ''; });
    img.addEventListener('load', () => { delete link.dataset.unavailable; });
    if (img.complete && !img.naturalWidth) link.dataset.unavailable = '';
  }
  const update = () => {
    const light = document.documentElement.dataset.theme === 'light';
    for (const img of images) {
      const url = new URL(img.dataset.badgeImage);
      url.searchParams.set('style', wide.matches ? 'for-the-badge' : 'flat-square');
      url.searchParams.set('labelColor', light ? group.dataset.labelLight : group.dataset.labelDark);
      url.searchParams.set('color', light ? img.dataset.colorLight : img.dataset.colorDark);
      // The logo lives on the label background, not on the colored value.
      url.searchParams.set('logoColor', light ? '223f53' : 'dbe6f0');
      if (img.src !== url.href) img.src = url.href;
    }
  };
  document.addEventListener('bioseq:themechange', update);
  wide.addEventListener('change', update);
  update();
}
