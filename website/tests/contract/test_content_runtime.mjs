import assert from 'node:assert/strict';
import test from 'node:test';
import { loadOptionalContent } from '../../static/js/content-runtime.mjs';

function fixture({ math = false, benchmarks = false, idle = true } = {}) {
  const resources = [], frames = [], idleCallbacks = [], imports = [];
  const sharedStyles = {};
  const root = {
    documentElement: { dataset: {} },
    defaultView: {
      requestAnimationFrame: callback => frames.push(callback),
      setTimeout: callback => idleCallbacks.push(callback),
      ...(idle ? { requestIdleCallback: callback => idleCallbacks.push(callback) } : {}),
    },
    querySelector: selector => ({
      '[data-math]': math, '.project-benchmark[data-table]': benchmarks,
      '[data-shared-styles]': sharedStyles,
    })[selector] || null,
    createElement: tagName => {
      const callbacks = {};
      return { tagName, addEventListener: (name, callback) => { callbacks[name] = callback; },
        finish: (name = 'load') => callbacks[name]() };
    },
    head: {
      insertBefore(node, before) { assert.equal(before, sharedStyles); resources.push(node); },
      append(node) { resources.push(node); },
    },
  };
  return {
    root, resources, frames, idleCallbacks, imports,
    load: () => loadOptionalContent(root, async url => { imports.push(url); }),
    firstPaint() { frames.shift()(); frames.shift()(); idleCallbacks.shift()(); },
  };
}

test('ordinary pages load no optional stylesheets, scripts or module graphs', async () => {
  const page = fixture();
  await page.load();
  assert.deepEqual(page.resources, []);
  assert.deepEqual(page.imports, []);
  assert.deepEqual(page.frames, []);
});

for (const idle of [true, false]) {
  test(`math waits for paint and ${idle ? 'idle time' : 'the timer fallback'}, then waits for its stylesheet`, async () => {
    const page = fixture({ math: true, idle });
    const complete = page.load();
    assert.equal(page.root.documentElement.dataset.mathState, 'queued');
    assert.deepEqual(page.resources, []);
    page.firstPaint();
    await Promise.resolve();
    assert.equal(page.root.documentElement.dataset.mathState, 'loading');
    const [css, js] = page.resources;
    assert.match(css.href, /vendor\/katex\/katex.min.css$/);
    assert.match(js.src, /vendor\/katex\/katex.min.js$/);
    assert.equal(js.async, true);
    js.finish();
    await Promise.resolve();
    assert.deepEqual(page.imports, []);
    css.finish();
    await complete;
    assert.equal(page.root.documentElement.dataset.mathState, 'ready');
    assert.match(page.imports[0], /project-math.js$/);
    assert.equal(page.imports.length, 1);
  });
}

test('benchmark excerpts load only their own stylesheet and module', async () => {
  const page = fixture({ benchmarks: true });
  const complete = page.load();
  assert.equal(page.resources.length, 1);
  assert.match(page.resources[0].href, /css\/project-benchmarks.css$/);
  assert.deepEqual(page.imports, []);
  page.resources[0].finish();
  await complete;
  assert.equal(page.imports.length, 1);
  assert.match(page.imports[0], /project-benchmarks.mjs$/);
});

test('math resource failure keeps the authored text and records a completed failure state', async () => {
  const page = fixture({ math: true });
  const complete = page.load();
  page.firstPaint();
  await Promise.resolve();
  page.resources[0].finish('error');
  await complete;
  assert.equal(page.root.documentElement.dataset.mathState, 'error');
  assert.deepEqual(page.imports, []);
});
