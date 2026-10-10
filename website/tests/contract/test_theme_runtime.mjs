import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import vm from 'node:vm';
import test from 'node:test';

const source = readFileSync(new URL('../../static/js/theme.js', import.meta.url), 'utf8');
function fixture({ saved = null, failRead = false, failWrite = false } = {}) {
  const listeners = new Map();
  const root = { dataset: {} };
  const tooltip = { textContent: '' };
  const attributes = {};
  let buttonReady = false;
  const button = {
    dataset: { darkLabel: 'Dark mode. Switch to light mode.', lightLabel: 'Light mode. Switch to dark mode.' },
    setAttribute: (name, value) => { attributes[name] = value; },
    querySelector: () => tooltip,
    addEventListener: (name, callback) => listeners.set(`button:${name}`, callback),
  };
  const updates = [];
  const document = {
    documentElement: root,
    querySelector: () => buttonReady ? button : null,
    addEventListener: (name, callback) => listeners.set(`document:${name}`, callback),
    dispatchEvent: event => updates.push(event.detail.theme),
  };
  const storage = new Map(saved === null ? [] : [['bioseq-theme', saved]]);
  const localStorage = {
    getItem: key => { if (failRead) throw new Error('denied'); return storage.get(key) ?? null; },
    setItem: (key, value) => { if (failWrite) throw new Error('denied'); storage.set(key, value); },
  };
  const window = { addEventListener: (name, callback) => listeners.set(`window:${name}`, callback) };
  vm.runInNewContext(source, {
    window, document, localStorage,
    CustomEvent: class { constructor(name, options) { this.type = name; this.detail = options.detail; } },
  });
  return { root, attributes, tooltip, updates, storage,
    ready() { buttonReady = true; listeners.get('document:DOMContentLoaded')(); },
    click() { listeners.get('button:click')(); },
    storageEvent(key) { listeners.get('window:storage')({ key }); },
  };
}

test('dark is the default before the header exists; the control explains both modes', () => {
  const page = fixture();
  assert.equal(page.root.dataset.theme, 'dark');
  page.ready();
  assert.equal(page.attributes['aria-pressed'], 'false');
  assert.match(page.attributes['aria-label'], /Dark mode.*light mode/);
  assert.equal(page.tooltip.textContent, page.attributes.title);
  page.click();
  assert.equal(page.root.dataset.theme, 'light');
  assert.equal(page.storage.get('bioseq-theme'), 'light');
  assert.equal(page.attributes['aria-pressed'], 'true');
  assert.match(page.attributes['aria-label'], /Light mode.*dark mode/);
  page.click();
  assert.equal(page.storage.get('bioseq-theme'), 'dark');
});

test('saved light applies before paint while invalid saved values fall back to dark', () => {
  assert.equal(fixture({ saved: 'light' }).root.dataset.theme, 'light');
  for (const saved of ['dark', 'system', '', '<script>']) {
    assert.equal(fixture({ saved }).root.dataset.theme, 'dark');
  }
});

test('storage denial does not stop either direction of the toggle', () => {
  const page = fixture({ failRead: true, failWrite: true });
  page.ready();
  page.click();
  assert.equal(page.root.dataset.theme, 'light');
  page.click();
  assert.equal(page.root.dataset.theme, 'dark');
});

test('another tab updates the theme, unrelated storage changes do not, clearing resets dark', () => {
  const page = fixture();
  page.ready();
  page.storage.set('bioseq-theme', 'light');
  page.storageEvent('unrelated');
  assert.equal(page.root.dataset.theme, 'dark');
  page.storageEvent('bioseq-theme');
  assert.equal(page.root.dataset.theme, 'light');
  page.storage.clear();
  page.storageEvent(null);
  assert.equal(page.root.dataset.theme, 'dark');
});
