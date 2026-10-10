"""Offline browser checks for shared themes, contact, and interest analytics.

Every third-party request is intercepted. These tests must never send analytics
or a contact message to the live services, including when a check fails.
"""
from __future__ import annotations

import json
from datetime import datetime
from pathlib import Path
from urllib.parse import urlsplit

GOOGLE_TAG_URL = "https://www.googletagmanager.com/gtag/js?id=G-989VYH5HT7"
FORMSERVE_URL = "https://formserve.io/f/DXTG4KTRmLw"
WIDTHS = (320, 360, 420, 568, 666, 768, 1024, 1300, 1600, 1920, 2560)


def _check(value, message):
    if not value:
        raise AssertionError(message)


def _theme(page, expected):
    page.wait_for_function(
        "value => document.documentElement.dataset.theme === value", arg=expected)
    control = page.locator("[data-theme-toggle]")
    _check(control.count() == 1, "every page needs exactly one theme control")
    label = control.get_attribute("aria-label") or ""
    tooltip = control.get_attribute("title") or control.get_attribute("data-tooltip") or ""
    _check(expected in label.lower() and "light" in label.lower() and "dark" in label.lower(),
           f"theme control must explain current mode and next mode: {label}")
    _check(bool(tooltip), "theme control has no explanatory tooltip")
    return control


def _header_geometry(page, width):
    """Check real controls rather than the wrappers that intentionally contain them."""
    geometry = page.evaluate("""() => {
      const header = document.querySelector('[data-site-header]');
      const selectors = '.site-brand,.site-navigation a,#doc-search,.site-version-button,' +
        '.site-github,[data-theme-toggle],a[href$="/contact/"]';
      const controls = [...header.querySelectorAll(selectors)].filter(node => {
        const style = getComputedStyle(node), box = node.getBoundingClientRect();
        return style.visibility !== 'hidden' && style.display !== 'none' && box.width && box.height;
      }).map(node => { const b = node.getBoundingClientRect(); return {
        name: node.getAttribute('aria-label') || node.textContent.trim(),
        x: b.x, y: b.y, width: b.width, height: b.height,
        theme: node.matches('[data-theme-toggle]'), github: node.matches('.site-github')
      }; });
      return {controls, overflow: document.documentElement.scrollWidth - innerWidth};
    }""")
    _check(geometry["overflow"] <= 1, f"light page overflows at {width}px")
    controls = geometry["controls"]
    for index, current in enumerate(controls):
        _check(current["x"] >= -1 and current["x"] + current["width"] <= width + 1,
               f"header control clips at {width}px: {current}")
        for other in controls[index + 1:]:
            overlap_x = min(current["x"] + current["width"], other["x"] + other["width"]) - max(current["x"], other["x"])
            overlap_y = min(current["y"] + current["height"], other["y"] + other["height"]) - max(current["y"], other["y"])
            _check(overlap_x <= 1 or overlap_y <= 1,
                   f"header controls overlap at {width}px: {current['name']} / {other['name']}")
    toggle = next(item for item in controls if item["theme"])
    github = next((item for item in controls if item["github"]), None)
    if github:
        _check(toggle["x"] >= github["x"] + github["width"] - 1,
               f"theme control is not to the right of GitHub at {width}px")
    return geometry


def _light_readability(page, label):
    """Sample text over flat surfaces, with alpha compositing for inherited layers."""
    samples = page.evaluate("""() => {
      const parse = value => {
        const values = value.match(/[\\d.]+/g)?.map(Number);
        return values?.length >= 3 ? [...values.slice(0,3), values[3] ?? 1] : [0,0,0,0];
      };
      const over = (front, back) => {
        const a = front[3] + back[3] * (1-front[3]);
        return [0,1,2].map(i => a ? (front[i]*front[3] + back[i]*back[3]*(1-front[3]))/a : 0).concat(a);
      };
      const luminance = rgb => rgb.slice(0,3).map(x => x/255)
        .map(x => x <= .04045 ? x/12.92 : ((x+.055)/1.055)**2.4)
        .reduce((sum,x,i) => sum + x*[.2126,.7152,.0722][i],0);
      const selectors = 'main p,main td,main th,.doc-code .token,.algorithm-keyword,.algorithm-line-number,' +
        '.site-search-box input,.contact-form label';
      return [...document.querySelectorAll(selectors)].filter(node => {
        const box = node.getBoundingClientRect();
        return box.width && box.height && node.textContent.trim();
      }).slice(0,160).flatMap(node => {
        let background = [255,255,255,1];
        const ancestors = [];
        for (let parent = node; parent; parent = parent.parentElement) ancestors.push(parent);
        for (const parent of ancestors.reverse()) {
          const style = getComputedStyle(parent);
          // Gradient text and gradient surfaces need the separate screenshot review.
          if (style.backgroundImage !== 'none' || Number(style.opacity) !== 1) return [];
          background = over(parse(style.backgroundColor), background);
        }
        const style = getComputedStyle(node), foreground = over(parse(style.color), background);
        const values = [luminance(foreground), luminance(background)].sort((a,b) => a-b);
        return [{text: node.textContent.trim().slice(0,65), selector: node.className,
          ratio: (values[1]+.05)/(values[0]+.05), foreground: style.color,
          background, fontSize: parseFloat(style.fontSize),
          minimum: node.matches('.algorithm-line-number') || parseFloat(style.fontSize) >= 24 ||
            (parseFloat(style.fontSize) >= 18.66 && Number(style.fontWeight) >= 700) ? 3 : 4.5}];
      });
    }""")
    failures = [item for item in samples if item["ratio"] < item["minimum"]]
    _check(not failures, f"{label}: unreadable light-mode text samples: {failures[:6]}")
    return {"sample_count": len(samples), "minimum_contrast": min((x["ratio"] for x in samples), default=None)}


def exercise_site_features(browser, server, base_path, version, artifact_dir: Path):
    """Run after the existing dark checks, using independent browser storage."""
    root = base_path.rstrip("/")
    path = lambda suffix="": root + "/" + suffix.lstrip("/")
    reports = []
    external_requests = []
    endpoint_state = {"response": "failure", "requests": []}
    context = browser.new_context(viewport={"width": 1600, "height": 1000})

    def offline(route):
        url = route.request.url
        host = urlsplit(url).hostname
        if host in ("127.0.0.1", "localhost", server._host):
            route.continue_()
        elif url == GOOGLE_TAG_URL:
            external_requests.append(url)
            route.fulfill(status=200, content_type="application/javascript", body="/* offline analytics */")
        elif url == FORMSERVE_URL:
            if route.request.method == "OPTIONS":
                route.fulfill(status=204, headers={"access-control-allow-origin": "*",
                    "access-control-allow-methods": "POST, OPTIONS", "access-control-allow-headers": "content-type"})
                return
            endpoint_state["requests"].append({"method": route.request.method, "body": route.request.post_data or ""})
            response = endpoint_state["response"]
            route.fulfill(status=503 if response == "failure" else 200,
                          content_type="application/json", body=json.dumps({"status": "success" if response == "success" else "error"}),
                          headers={"access-control-allow-origin": "*",
                                   "access-control-allow-methods": "POST, OPTIONS",
                                   "access-control-allow-headers": "content-type"})
        else:
            external_requests.append(url)
            route.abort()

    context.route("**/*", offline)
    page = context.new_page()
    errors = []
    page.on("pageerror", lambda error: errors.append(str(error)))
    page.goto(server.url_for(path()), wait_until="networkidle")
    toggle = _theme(page, "dark")
    _check(page.evaluate("localStorage.getItem('bioseq-theme')") in (None, "dark"),
           "an unconfigured visitor did not start in dark mode")
    toggle.click()
    _theme(page, "light")
    _check(page.evaluate("localStorage.getItem('bioseq-theme')") == "light", "light preference was not saved")
    page.reload(wait_until="networkidle")
    _theme(page, "light")

    routes = {
        "landing": path(), "benchmarks": path("benchmarks/"),
        "tutorial": path(f"{version}/tutorials/getting_started/"),
        "figures": path(f"{version}/tutorials/project_biology/"),
        "algorithms": path(f"{version}/tutorials/project_minimizers/"),
        "settings": path(f"{version}/tutorials/project_configuration_reference/"),
        "contact": path("contact/"), "error": path("error.html"),
    }
    artifact_dir = Path(artifact_dir) / "light-mode"
    artifact_dir.mkdir(parents=True, exist_ok=True)
    for name, route in routes.items():
        page.goto(server.url_for(route), wait_until="networkidle")
        _theme(page, "light")
        _check(page.locator('[data-site-header] a[href$="/contact/"]').count() == 1,
               f"{name}: header is missing Contact me")
        for width in WIDTHS:
            page.set_viewport_size({"width": width, "height": 1000})
            page.evaluate("() => new Promise(resolve => requestAnimationFrame(() => requestAnimationFrame(resolve)))")
            report = {"page": name, "width": width, "header": _header_geometry(page, width)}
            if width in (420, 1600):
                report["contrast"] = _light_readability(page, name)
                screenshot = artifact_dir / f"{name}-{width}.png"
                page.screenshot(path=str(screenshot), full_page=True, animations="disabled")
                report["screenshot"] = screenshot.name
            reports.append(report)
        if name == "figures":
            _check(page.locator('img[src*="/figures/project/"]').evaluate_all(
                "nodes => nodes.length > 0 && nodes.every(n => n.complete && n.naturalWidth > 0)"),
                "light figures did not load")
        if name == "benchmarks":
            cell = page.locator('.bd-s0072[tabindex="0"]').filter(has_text='×').first
            cell.focus()
            tooltip = page.locator('[data-benchmark-tip]')
            page.wait_for_function("() => getComputedStyle(document.querySelector('[data-benchmark-tip]')).opacity === '1'")
            page.evaluate("() => new Promise(resolve => requestAnimationFrame(() => requestAnimationFrame(resolve)))")
            _check("×" in tooltip.inner_text(), "light benchmark tooltip lost comparison data")
            page.screenshot(path=str(artifact_dir / "benchmark-tooltip.png"), animations="disabled")
            page.keyboard.press("Escape")
    page.locator('[data-theme-toggle]').click()
    _theme(page, "dark")
    page.goto(server.url_for(path()), wait_until="networkidle")
    _theme(page, "dark")
    _exercise_interest_events(page, server, path, version)
    _exercise_contact(page, server, path, endpoint_state)
    _check(not errors, f"new features generated page errors: {errors}")
    _check(set(external_requests) == {GOOGLE_TAG_URL},
           f"unexpected third-party requests: {external_requests}")
    context.close()

    # Both privacy-mode storage exceptions must leave the toggle usable.
    blocked = browser.new_context()
    blocked.route("**/*", offline)
    blocked.add_init_script("""Object.defineProperty(window, 'localStorage', {
      get() { throw new DOMException('Storage is unavailable', 'SecurityError'); }
    });""")
    blocked_page = blocked.new_page()
    blocked_page.goto(server.url_for(path()), wait_until="networkidle")
    _theme(blocked_page, "dark").click()
    _theme(blocked_page, "light").click()
    _theme(blocked_page, "dark")
    blocked.close()
    _exercise_thanks(browser, offline, server, path, artifact_dir)
    (artifact_dir / "report.json").write_text(json.dumps({"layouts": reports}, indent=2) + "\n")


def _exercise_interest_events(page, server, path, version):
    page.evaluate("""() => {
      window.__copiedInstallCommands = [];
      Object.defineProperty(navigator, 'clipboard', { configurable: true, value: {
        writeText: async text => { window.__copiedInstallCommands.push(text); }
      }});
      window.dataLayer.length = 0;
      document.addEventListener('click', event => {
        if (event.target.closest('a')) event.preventDefault();
      });
    }""")
    page.locator('[data-copy-install]').click()
    page.wait_for_function("() => window.dataLayer.some(args => args[0] === 'event' && args[1] === 'docker_command_copy')")
    _check(page.evaluate("window.__copiedInstallCommands.length") == 1, "install copy did not copy its command")
    events = page.evaluate("() => window.dataLayer.map(args => [...args])")
    copies = [event for event in events if event[0:2] == ['event', 'docker_command_copy']]
    _check(len(copies) == 1 and copies[0][2].get('command') in ('docker pull', 'docker run'),
           f"Docker copy did not report a useful command category: {copies}")
    _check(not any(key in copies[0][2] for key in ('email', 'name', 'message', 'command_text')),
           "analytics sends user content")
    page.evaluate("""() => {
      navigator.clipboard.writeText = async () => { throw new Error('clipboard denied'); };
      document.execCommand = () => false;
    }""")
    page.locator('[data-copy-install]').click()
    page.wait_for_function("() => !document.querySelector('[data-copy-idle]').hidden")
    count = page.evaluate("() => window.dataLayer.filter(args => args[0] === 'event' && args[1] === 'docker_command_copy').length")
    _check(count == 1, "failed clipboard copy incorrectly emitted an interest event")
    page.goto(server.url_for(path(f'{version}/tutorials/getting_started/')), wait_until='networkidle')
    page.evaluate("""() => document.addEventListener('click', event => {
      if (event.target.closest('a')) event.preventDefault();
    })""")
    packages = page.locator('a[href*="sourceforge.net/projects/bioseqdb/"],a[href*="hub.docker.com/r/styczynski/bioseqdb"]')
    _check(packages.count() > 0, "installation docs have no package link to measure")
    packages.first.click()
    page.wait_for_function("() => window.dataLayer.some(args => args[0] === 'event' && args[1] === 'package_link_click')")
    event = page.evaluate("() => [...window.dataLayer.find(args => args[0] === 'event' && args[1] === 'package_link_click')]")
    _check(event[2]['link_location'] == 'documentation', "package event has the wrong page context")


def _exercise_contact(page, server, path, endpoint_state):
    page.goto(server.url_for(path('contact/')), wait_until='networkidle')
    form = page.locator('[data-contact-form]')
    _check(form.get_attribute('method').upper() == 'POST', "contact form is not a POST")
    _check(form.get_attribute('action') == FORMSERVE_URL, "contact action differs from configured endpoint")
    honeypot = form.locator('[name="_honeypot"]')
    _check(honeypot.count() == 1 and honeypot.input_value() == '', "contact honeypot missing or prefilled")
    _check(honeypot.get_attribute('required') is None and honeypot.get_attribute('tabindex') == '-1',
           "honeypot must not trap real visitors")
    submit = form.locator('button[type="submit"]')
    submit.click()
    _check(not endpoint_state['requests'], "invalid contact form was submitted")
    form.locator('[name="name"]').fill('Website browser check')
    form.locator('[name="email"]').fill('not-an-email')
    form.locator('[name="message"]').fill('Offline test message. Never send to the real endpoint.')
    submit.click()
    _check(not endpoint_state['requests'], "invalid email was submitted")
    form.locator('[name="email"]').fill('offline@example.invalid')
    submit.click()
    status = form.locator('[data-contact-status]')
    status.locator('xpath=self::*[@data-state="error"]').wait_for(state='visible')
    _check(status.inner_text() == form.get_attribute('data-error'), "contact failure leaks technical errors")
    _check(not submit.is_disabled() and form.locator('[name="email"]').input_value() == 'offline@example.invalid',
           "failed contact form lost input or prevents retry")
    endpoint_state['response'] = 'success'
    submit.click(no_wait_after=True)
    status.locator('xpath=self::*[@data-state="success"]').wait_for(state='visible')
    _check(status.inner_text() == form.get_attribute('data-success'), "successful contact lacks inline confirmation")
    page.wait_for_url('**' + path('contact_thanks/'))
    _check(len(endpoint_state['requests']) == 2, "contact retry submitted an unexpected number of requests")
    sent = endpoint_state['requests'][-1]
    payload = json.loads(sent['body'])
    _check(sent['method'] == 'POST' and payload['email'] == 'offline@example.invalid' and payload['_honeypot'] == '',
           "contact request lost configured named fields")
    _check(payload['_redirect'] == server.url_for(path('contact_thanks/')), "contact redirect ignores the site mount")


def _exercise_thanks(browser, offline, server, path, artifact_dir):
    context = browser.new_context(viewport={'width': 420, 'height': 900})
    context.route('**/*', offline)
    page = context.new_page()
    page.clock.install(time=datetime(2026, 1, 1))
    page.clock.pause_at(datetime(2026, 1, 1))
    page.goto(server.url_for(path('contact_thanks/')), wait_until='load')
    _check(page.locator('[data-countdown-number]').inner_text() == '6', "confirmation timer did not start at six")
    _theme(page, 'dark').click()
    _theme(page, 'light')
    _header_geometry(page, 420)
    page.screenshot(path=str(artifact_dir / 'thanks-420.png'), animations='disabled')
    # Let the first animation frame after the three-second boundary run.
    page.clock.run_for(3020)
    _check(page.locator('[data-countdown-number]').inner_text() == '3', "confirmation timer did not count down")
    progress = page.locator('[data-countdown-ring]').evaluate('node => Number(node.style.strokeDashoffset)')
    _check(45 <= progress <= 55, "confirmation circular progress does not match the remaining time")
    page.locator('[data-countdown-pause]').click()
    page.clock.run_for(7000)
    _check(urlsplit(page.url).path == path('contact_thanks/'), "paused confirmation timer still redirected")
    page.locator('[data-countdown-pause]').click()
    page.clock.run_for(2900)
    _check(urlsplit(page.url).path == path('contact_thanks/'), "confirmation redirected before its six-second countdown")
    page.clock.run_for(200)
    page.wait_for_url(server.url_for(path()))
    _theme(page, 'light')
    context.close()
