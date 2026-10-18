// Optional document features must not hold up the first view of every page.
// The server-rendered TeX remains readable while its local renderer loads.
const asset = (path) => new URL(`../${path}`, import.meta.url).href;

function stylesheet(root, path) {
  return new Promise((resolve, reject) => {
    const link = root.createElement('link');
    link.rel = 'stylesheet';
    link.href = asset(path);
    link.addEventListener('load', resolve, { once: true });
    link.addEventListener('error', reject, { once: true });
    // Keep the original cascade: shared layout and theme refinements win.
    root.head.insertBefore(link, root.querySelector('[data-shared-styles]'));
  });
}

function script(root, path) {
  return new Promise((resolve, reject) => {
    const node = root.createElement('script');
    node.src = asset(path);
    node.async = true;
    node.fetchPriority = 'low';
    node.addEventListener('load', resolve, { once: true });
    node.addEventListener('error', reject, { once: true });
    root.head.append(node);
  });
}

function afterFirstPaint(view) {
  return new Promise((resolve) => {
    view.requestAnimationFrame(() => view.requestAnimationFrame(() => {
      if (view.requestIdleCallback) view.requestIdleCallback(resolve, { timeout: 1000 });
      else view.setTimeout(resolve, 0);
    }));
  });
}

export async function loadOptionalContent(root, loadModule = (url) => import(url)) {
  const tasks = [];
  if (root.querySelector('[data-math]')) {
    root.documentElement.dataset.mathState = 'queued';
    tasks.push((async () => {
      await afterFirstPaint(root.defaultView);
      root.documentElement.dataset.mathState = 'loading';
      await Promise.all([
        stylesheet(root, 'vendor/katex/katex.min.css'),
        script(root, 'vendor/katex/katex.min.js'),
      ]);
      await loadModule(new URL('./project-math.js', import.meta.url).href);
      root.documentElement.dataset.mathState = 'ready';
    })().catch(() => {
      // Preserve the original TeX if a resource fails; never leave empty maths.
      root.documentElement.dataset.mathState = 'error';
    }));
  }
  // The dedicated benchmark page already owns a full explorer and its data
  // request. Chapter excerpt enhancement applies only to documentation pages.
  if (!root.body.classList.contains('benchmarks-design-page') &&
      root.querySelector('.project-benchmark[data-table]')) {
    tasks.push((async () => {
      await stylesheet(root, 'css/project-benchmarks.css');
      await loadModule(new URL('./project-benchmarks.mjs', import.meta.url).href);
    })().catch(() => {
      // Keep an optional resource failure isolated from the rest of the page.
      root.documentElement.dataset.benchmarkState = 'error';
    }));
  }
  await Promise.all(tasks);
}

if (typeof document !== 'undefined') void loadOptionalContent(document);
