import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import vm from 'node:vm';
import test from 'node:test';

const source = readFileSync(new URL('../../static/js/analytics.js', import.meta.url), 'utf8');
function fixture(landing = true) {
  const handlers = new Map();
  const scripts = [];
  const window = {};
  const document = {
    body: { classList: { contains: value => landing && value === 'landing-design-page' } },
    addEventListener: (name, callback) => handlers.set(name, callback),
    createElement: (tag) => ({ tagName: tag }),
    head: { append: node => scripts.push({ ...node, queuedEvents: window.dataLayer.length }) },
  };
  vm.runInNewContext(source, { window, document, URL, Date, location: { href: 'https://bioseqdb.org/' } });
  return {
    scripts,
    events: () => JSON.parse(JSON.stringify(window.dataLayer.map(args => [...args]))),
    click(href, type = 'click', button = 0) {
      handlers.get(type)({ type, button, target: { closest: () => ({ href }) } });
    },
    copy(command) { handlers.get('bioseq:install-copy')({ detail: { command } }); },
  };
}

test('initializes the supplied property with advertising personalization disabled', () => {
  const page = fixture();
  const events = page.events();
  assert.equal(events[0][0], 'js');
  assert.deepEqual(events[1], ['config', 'G-989VYH5HT7', {
    allow_google_signals: false, allow_ad_personalization_signals: false,
  }]);
});

test('loads the remote tag asynchronously at low priority after queuing the configuration', () => {
  const page = fixture();
  assert.deepEqual(page.scripts, [{
    tagName: 'script', src: 'https://www.googletagmanager.com/gtag/js?id=G-989VYH5HT7',
    async: true, fetchPriority: 'low', queuedEvents: 2,
  }]);
});

test('package clicks record source, path and placement, excluding visitor query or fragment data', () => {
  const page = fixture();
  const links = [
    ['https://sourceforge.net/projects/bioseqdb/files/latest/download?email=private@example.com#secret', 'sourceforge'],
    ['https://downloads.sourceforge.net/project/bioseqdb/package.tar.gz', 'sourceforge'],
    ['https://hub.docker.com/r/styczynski/bioseqdb', 'docker_hub'],
    ['https://github.com/styczynski/bioseqdb/releases/tag/v0.1', 'github_releases'],
  ];
  for (const [url] of links) page.click(url);
  const events = page.events().slice(2);
  assert.equal(events.length, links.length);
  events.forEach((event, index) => {
    assert.deepEqual(event, ['event', 'package_link_click', {
      package_source: links[index][1], package_path: new URL(links[index][0]).pathname,
      link_location: 'landing', transport_type: 'beacon',
    }]);
  });
  assert.doesNotMatch(JSON.stringify(events), /private|secret|email/);
});

test('documentation and middle-button interest are tracked, unrelated links are not', () => {
  const page = fixture(false);
  for (const url of ['https://github.com/styczynski/bioseqdb', 'https://example.com/',
    'https://hub.docker.com/r/styczynski/bioseqdb-other', 'mailto:private@example.com']) page.click(url);
  page.click('https://hub.docker.com/r/styczynski/bioseqdb', 'auxclick', 2);
  assert.equal(page.events().length, 2);
  page.click('https://hub.docker.com/r/styczynski/bioseqdb', 'auxclick', 1);
  assert.equal(page.events()[2][2].link_location, 'documentation');
});

test('successful Docker copy events contain only the command category, never the copied arguments', () => {
  const page = fixture();
  page.copy('docker pull ghcr.io/styczynski/bioseqdb:latest');
  page.copy('$ docker   run -e PASSWORD=private -v /home/person:/data image');
  page.copy('SELECT secret FROM users');
  page.copy('notdocker pull image');
  page.copy('docker delete image');
  assert.deepEqual(page.events().slice(2), [
    ['event', 'docker_command_copy', { command: 'docker pull', link_location: 'landing' }],
    ['event', 'docker_command_copy', { command: 'docker run', link_location: 'landing' }],
  ]);
});
