#!/usr/bin/env python3
"""Open a dedicated visible SourceForge session for human login.

Run with the publication-only Playwright virtual environment. This never
reads passwords, exports cookies, records video/HAR or touches the normal
Chrome profile. Keep the console alive while the user completes login.
"""
from __future__ import annotations

import code
import json
import os
from pathlib import Path
import subprocess
import tempfile
import time
from urllib.parse import urlsplit

from playwright.sync_api import sync_playwright

ROOT = Path(__file__).resolve().parents[1]
SESSION_ROOT = ROOT / 'reporting/release_publish/browser_sessions'
PAGES = {
    'keys': 'https://sourceforge.net/auth/shell_services',
    'profile': 'https://sourceforge.net/p/bioseqdb/admin/overview',
}


def apply_project_profile(page):
    """Save prepared public metadata via the inspected, logged-in settings form."""
    profile = json.loads((ROOT / 'reporting/release_publish/sourceforge-profile.json').read_text())
    page.goto(PAGES['profile'], wait_until='domcontentloaded', timeout=30000)
    url = urlsplit(page.url)
    if url.scheme != 'https' or url.netloc != 'sourceforge.net' or url.path.rstrip('/') != '/p/bioseqdb/admin/overview':
        raise RuntimeError('Human project-admin login required')
    form = page.locator('form').filter(has=page.locator('textarea[name="short_description"]'))
    action = urlsplit(form.evaluate('(form) => form.action'))
    if action.netloc != 'sourceforge.net' or action.path != '/p/bioseqdb/admin/update':
        raise RuntimeError('Unexpected project settings form')
    if not form.locator('#removal_active_cb').is_checked():
        raise RuntimeError('Refusing to change an inactive project')

    # SourceForge requires PNG/JPEG/GIF. Rasterize the existing SVG unchanged;
    # never redraw the logo or modify the signed-off website artifact tree.
    icon = ROOT / 'reporting/release_publish/sourceforge-logo.png'
    preview = page.context.new_page()
    try:
        preview.set_viewport_size({'width': 512, 'height': 512})
        preview.set_content('<style>body{margin:0}svg{width:512px;height:512px}</style>' +
                            Path(profile['icon_file']).read_text())
        preview.locator('svg').screenshot(path=str(icon), omit_background=True)
    finally:
        preview.close()
    for name in ('name', 'summary', 'external_homepage', 'short_description'):
        field = form.locator(f'[name="{name}"]')
        maximum = field.get_attribute('maxlength')
        if maximum and int(maximum) >= 0 and len(profile[name]) > int(maximum):
            raise RuntimeError(f'Prepared {name} exceeds the form limit')
        field.fill(profile[name])
    form.locator('input[name="icon"]').set_input_files(str(icon))
    # Native form submission also works when macOS hides the browser window.
    # Keep HTML validation and the specific Save submitter (not removal).
    with page.expect_navigation(wait_until='domcontentloaded', timeout=30000):
        form.evaluate('(f) => f.requestSubmit(f.querySelector("input[type=submit][name=save]"))')
    # Read a fresh settings page, not echoed values from a validation-error form.
    page.goto(PAGES['profile'], wait_until='domcontentloaded', timeout=30000)
    verified = {name: page.locator(f'[name="{name}"]').input_value().replace('\r\n', '\n') == profile[name]
                for name in ('name', 'summary', 'external_homepage', 'short_description')}
    if not all(verified.values()):
        raise RuntimeError('Project settings did not retain the requested public fields')
    return {'public_fields_verified': verified, 'icon_uploaded': str(icon)}


def main():
    SESSION_ROOT.mkdir(parents=True, exist_ok=True, mode=0o700)
    SESSION_ROOT.chmod(0o700)
    profile = Path(tempfile.mkdtemp(prefix='sourceforge-', dir=SESSION_ROOT))
    with sync_playwright() as playwright:
        # Launch through macOS LaunchServices, not the executable directly:
        # a direct child inherits the agent's Background audit session, where
        # the window may appear but cannot receive desktop keyboard input.
        # -W keeps the launcher alive for browser_open/lifetime tracking.
        # No automation session is attached while the human enters credentials.
        browser_process = subprocess.Popen([
            '/usr/bin/open', '-W', '-n', '-a', '/Applications/Google Chrome.app', '--args',
            f'--user-data-dir={profile}', '--remote-debugging-address=127.0.0.1',
            '--remote-debugging-port=0', '--no-first-run', '--no-default-browser-check',
            PAGES['keys']], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
            start_new_session=True,
            env={k: v for k, v in os.environ.items() if k not in {
                'SOURCE_FORGE_RELEASE_TOKEN', 'SOURCE_FORCE_OAUTH_TOKEN',
                'AWS_SECRET_ACCESS_KEY', 'AWS_SESSION_TOKEN'}})
        port_file = profile / 'DevToolsActivePort'
        deadline = time.monotonic() + 30
        while not port_file.exists():
            if browser_process.poll() is not None or time.monotonic() >= deadline:
                raise RuntimeError('Dedicated Chrome did not start')
            time.sleep(0.2)
        port = int(port_file.read_text().splitlines()[0])
        context = page = browser = None

        def connect():
            nonlocal context, page, browser
            if browser is None:
                browser = playwright.chromium.connect_over_cdp(
                    f'http://127.0.0.1:{port}', no_defaults=True)
                context = browser.contexts[0]
                page = next(p for p in context.pages if urlsplit(p.url).netloc == 'sourceforge.net')
            return page

        def status():
            """Return only origin/path, never login query strings or credentials."""
            if page is None:
                return dict(browser_open=browser_process.poll() is None,
                            automation_connected=False)
            url = urlsplit(page.url)
            return dict(origin=f'{url.scheme}://{url.netloc}', path=url.path,
                        browser_open=not page.is_closed())

        def navigate(area):
            connect()
            page.goto(PAGES[area], wait_until='domcontentloaded', timeout=60000)
            return status()

        def inspect_forms():
            """Inspect field names after login, never values or auth forms."""
            connect()
            url = urlsplit(page.url)
            if url.scheme != 'https' or url.netloc != 'sourceforge.net' or url.path not in {
                    '/auth/shell_services', '/auth/shell_services/',
                    '/p/bioseqdb/admin/overview', '/p/bioseqdb/admin/overview/'}:
                return {'status': 'waiting for human login', **status()}
            return page.locator('form').evaluate_all('''forms => forms.map(form => ({
                action: new URL(form.action).pathname, method: form.method,
                fields: [...form.elements].map(el => ({tag: el.tagName,
                    type: el.type, name: el.name, id: el.id}))
            }))''')

        def publish_profile():
            return apply_project_profile(connect())

        print(json.dumps({'browser_ready': True, 'pid': os.getpid(),
                          'launcher_pid': browser_process.pid, 'loopback_port': port,
                          'private_profile': str(profile), **status()}), flush=True)
        code.interact(banner='Log in directly in the Chrome window. No passwords are logged or cookies exported.\n'
                     'After human login: page = connect(); inspect_forms().',
                     local={'connect': connect, 'status': status,
                            'navigate': navigate, 'inspect_forms': inspect_forms,
                            'publish_profile': publish_profile})
        if browser is not None:
            browser.close()


if __name__ == '__main__':
    main()
