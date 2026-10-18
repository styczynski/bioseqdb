#!/usr/bin/env python3
"""Check generated links with real touch input, without visiting external sites."""
from __future__ import annotations

import argparse
import json
from pathlib import Path
import sys
import time
from urllib.parse import urlsplit

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from support.static_fixture_server import StaticFixtureServer


def check(value, message):
    if not value:
        raise AssertionError(message)


def until(checking, message):
    deadline = time.monotonic() + 10
    while time.monotonic() < deadline:
        if checking():
            return
        time.sleep(.025)
    raise AssertionError(message)


def bounds(page, locator):
    rect = locator.bounding_box()
    viewport = page.viewport_size
    check(rect and rect['x'] >= 10 and rect['y'] >= 10
          and rect['x'] + rect['width'] <= viewport['width'] - 9
          and rect['y'] + rect['height'] <= viewport['height'] - 9,
          f"Popover exceeds viewport: {rect}, {viewport}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--site', type=Path, required=True)
    parser.add_argument('--base-path', default='/')
    parser.add_argument('--version', default='v0.1')
    parser.add_argument('--output', type=Path, default=Path('build/touch-popovers'))
    parser.add_argument('--source-assets', action='store_true', help='Preview just the edited tooltip JS/CSS against an existing generated site')
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    from playwright.sync_api import sync_playwright, expect
    prefix = args.base_path.rstrip('/')
    static = Path(__file__).resolve().parents[2] / 'static'
    overrides = {'js/link-popover.mjs', 'js/docs-design.mjs', 'js/blog.mjs', 'css/project-content.css', 'css/blog.css'}
    errors = []
    checks = []
    with StaticFixtureServer(args.site, args.base_path) as server, sync_playwright() as pw:
        browser = pw.chromium.launch(headless=True)

        def route_local(route):
            parsed = urlsplit(route.request.url)
            if parsed.hostname not in ('127.0.0.1', 'localhost'):
                route.fulfill(status=204, body='')
                return
            relative = '/'.join(parsed.path.split('/')[-2:])
            if args.source_assets and relative in overrides:
                route.fulfill(path=str(static / relative), content_type='text/css' if relative.endswith('.css') else 'text/javascript')
            else:
                route.continue_()

        def context(touch=True, theme='dark', width=420, height=760):
            ctx = browser.new_context(viewport={'width': width, 'height': height}, is_mobile=touch, has_touch=touch, reduced_motion='reduce')
            ctx.add_init_script(f"localStorage.setItem('bioseq-theme', {json.dumps(theme)});")
            ctx.route('**/*', route_local)
            page = ctx.new_page()
            page.on('pageerror', lambda error: errors.append(str(error)))
            return ctx, page

        def open_page(page, path):
            page.goto(server.origin + prefix + path, wait_until='networkidle')
            page.locator('#entity-pop[data-initialized]').wait_for(state='attached')
            if args.source_assets:
                # New builds combine their CSS. Append the two authored files
                # only in this explicit development mode so they can be checked
                # before regenerating all pages and their hashed bundles.
                for name in ('project-content.css', 'blog.css'):
                    page.evaluate('''url => new Promise((resolve, reject) => {
                      const link = document.createElement('link');
                      link.rel = 'stylesheet'; link.href = url;
                      link.onload = resolve; link.onerror = reject;
                      document.head.append(link);
                    })''', server.origin + prefix + '/assets/tooltip-preview/css/' + name)

        def tap(page, anchor, popup):
            anchor.scroll_into_view_if_needed()
            before = page.url
            href = anchor.get_attribute('href')
            anchor.tap()
            expect(popup).to_be_visible()
            expect(popup).to_have_attribute('role', 'dialog')
            expect(popup.locator('.link-popover-more')).to_have_attribute('href', href)
            check(page.url == before, 'First tap navigated instead of showing a description')
            expect(anchor).to_have_attribute('aria-expanded', 'true')
            check(popup.locator('.link-popover-more').evaluate('node => node === document.activeElement'), 'Touch action did not receive focus')
            bounds(page, popup)
            return href

        for theme in ('dark', 'light'):
            ctx, page = context(theme=theme)
            open_page(page, '/blog/introducing-bioseqdb/')
            glossary = page.locator('a[data-glossary-description]')
            popup = page.locator('#blog-glossary-tip')
            first = glossary.first
            tap(page, first, popup)
            expect(popup).to_contain_text(first.get_attribute('data-glossary-description'))
            page.wait_for_timeout(250)
            expect(popup).to_be_visible()
            page.screenshot(path=str(args.output / f'glossary-{theme}-420.png'))
            popup.get_by_role('button', name='Close link details').tap()
            expect(popup).to_be_hidden()
            check(not first.get_attribute('aria-expanded'), 'Close left stale expanded state')
            check(first.evaluate('node => node === document.activeElement'), 'Close did not restore trigger focus')
            tap(page, first, popup)
            second = glossary.nth(1)
            tap(page, second, popup)
            check(not first.get_attribute('aria-describedby'), 'Switching links left stale description')
            page.keyboard.press('Escape')
            expect(popup).to_be_hidden()
            tap(page, first, popup)
            page.locator('h1').tap()
            expect(popup).to_be_hidden()
            # A SQL activation replaces a glossary popover rather than stacking it.
            tap(page, first, popup)
            entity = page.locator('a.entity-ref[data-entity-id]').first
            entity_popup = page.locator('#entity-pop')
            tap(page, entity, entity_popup)
            expect(popup).to_be_hidden()
            expect(entity_popup).not_to_have_attribute('aria-busy', 'true')
            check('could not be loaded' not in entity_popup.inner_text(), 'Generated SQL detail failed to load')
            page.screenshot(path=str(args.output / f'sql-{theme}-420.png'))
            page.keyboard.press('Escape')
            social = page.locator('a[data-social-description]').first
            tap(page, social, popup)
            page.keyboard.press('Escape')
            # Existing GitHub code references preserve their complete line anchor.
            open_page(page, f'/{args.version}/tutorials/project_postgres_storage/')
            source = page.locator('a.source-link').first
            href = tap(page, source, entity_popup)
            check('github.com' in href and '#L' in href, 'Expected a concrete GitHub code link')
            expect(entity_popup).to_contain_text('Source code')
            page.keyboard.press('Escape')
            # Long setting descriptions and their actions fit short phone screens.
            open_page(page, f'/{args.version}/tutorials/project_configuration_reference/')
            refs = page.locator('a.entity-ref[data-entity-id]')
            setting = refs.filter(has_text='insert_maintenance.enabled').first
            for width, height in ((320,568), (420,760), (568,420), (666,420), (768,768), (1024,768), (1600,900)):
                page.set_viewport_size({'width': width, 'height': height})
                tap(page, setting, entity_popup)
                expect(entity_popup).to_contain_text('Default')
                bounds(page, entity_popup.locator('.link-popover-actions'))
                if width in (320, 568, 1600):
                    page.screenshot(path=str(args.output / f'setting-{theme}-{width}.png'))
                page.keyboard.press('Escape')
            page.set_viewport_size({'width': 320, 'height': 390})
            long_setting = refs.filter(has_text='sequence_minimizer_sort.algorithm').first
            tap(page, long_setting, entity_popup)
            expect(entity_popup).to_contain_text('simd_u128_vec_stream_4')
            content = entity_popup.locator('.link-popover-content')
            check(content.evaluate('node => node.scrollHeight > node.clientHeight'), 'Long setting did not scroll within the short viewport')
            content.evaluate('node => node.scrollTop = node.scrollHeight')
            bounds(page, entity_popup.locator('.link-popover-actions'))
            page.screenshot(path=str(args.output / f'long-setting-{theme}-320.png'))
            page.keyboard.press('Escape')
            # Simulate modified pointer/keyboard activations without following
            # their destinations; the handler must leave them untouched.
            native = long_setting.evaluate('''anchor => {
              const result = [];
              const observe = event => { result.push(event.defaultPrevented); event.preventDefault(); };
              document.addEventListener('click', observe);
              for (const detail of [0, 1]) {
                anchor.dispatchEvent(new MouseEvent('click', {bubbles:true, cancelable:true, button:0, detail, ctrlKey:detail === 1}));
              }
              anchor.dispatchEvent(new PointerEvent('click', {bubbles:true, cancelable:true, button:0, detail:1, pointerType:'mouse'}));
              document.removeEventListener('click', observe);
              return result;
            }''')
            check(native == [False, False, False], 'Keyboard, modified, or hybrid-device mouse click was intercepted')
            checks.append(f'{theme}: glossary, SQL, social, GitHub source, 7 viewport sizes, dismissal and switching')
            ctx.close()

        # A slow/failed tooltip dataset must not trap users or reopen a dismissed dialog.
        ctx, page = context()
        pending = []
        ctx.route('**/entities.*.json', lambda route: pending.append(route))
        open_page(page, f'/{args.version}/tutorials/getting_started/')
        first = page.locator('a.entity-ref[data-entity-id]').first
        popup = page.locator('#entity-pop')
        tap(page, first, popup)
        expect(popup).to_contain_text('Loading reference details')
        until(lambda: bool(pending), 'Tooltip dataset was not requested')
        popup.get_by_role('button', name='Close link details').tap()
        pending.pop().fulfill(status=503, body='')
        page.wait_for_timeout(250)
        expect(popup).to_be_hidden()
        tap(page, first, popup)
        until(lambda: bool(pending), 'Failed tooltip dataset was not retried')
        pending.pop().fulfill(status=503, body='')
        expect(popup).to_contain_text('could not be loaded')
        expect(popup.locator('.link-popover-more')).to_have_attribute('href', first.get_attribute('href'))
        checks.append('Loading, failure, retry, and stale asynchronous completion')
        ctx.close()

        ctx, page = context(touch=False, width=1280)
        open_page(page, f'/{args.version}/tutorials/getting_started/')
        first = page.locator('a.entity-ref[data-entity-id]').first
        popup = page.locator('#entity-pop')
        first.hover()
        expect(popup).to_be_visible()
        expect(popup).to_have_attribute('role', 'tooltip')
        check(popup.locator('a, button').count() == 0, 'Desktop tooltip contains interactive controls')
        check(popup.evaluate('node => getComputedStyle(node).pointerEvents') == 'none', 'Desktop tooltip steals pointer input')
        page.keyboard.press('Escape')
        first.focus()
        expect(popup).to_be_visible()
        target = first.evaluate('node => node.href')
        first.press('Enter')
        expect(page).to_have_url(target)
        checks.append('Desktop hover semantics and native keyboard navigation')
        ctx.close()
        browser.close()
    check(not errors, f'Browser errors: {errors}')
    (args.output / 'report.json').write_text(json.dumps({'checks': checks, 'errors': errors}, indent=2) + '\n')
    print(f'Touch popover audit passed: {len(checks)} groups; dark/light, 7 viewport sizes')


if __name__ == '__main__':
    main()
