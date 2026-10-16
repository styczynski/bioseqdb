"""Offline acceptance checks for Markdown blogs and the shared release banner."""
from __future__ import annotations

import json
import re
import time
from pathlib import Path
from urllib.parse import urlsplit
from xml.etree import ElementTree
from support.badges_checks import serve_badge

from support.site_features_checks import GOOGLE_TAG_URL, _header_geometry, _light_readability


WIDTHS = (320, 420, 568, 666, 768, 1024, 1300, 1600, 1920, 2560)


def _check(value, message):
    if not value:
        raise AssertionError(message)


def _settle(page):
    page.evaluate("() => new Promise(r => requestAnimationFrame(() => requestAnimationFrame(r)))")


def _wait_scroll(page, window, after):
    deadline = time.monotonic() + 6.5
    while time.monotonic() < deadline:
        current = window.evaluate('node => node.scrollLeft')
        _check(current >= after - 1, 'announcement jumped back after a manual scroll or pause')
        if current > after + 2:
            return
        page.wait_for_timeout(80)
    raise AssertionError('overflowing announcement did not begin scrolling')


def _layout(page, width, label):
    _header_geometry(page, width)
    _check(page.locator('[data-site-header] .site-github').is_visible(),
           f"{label}: mobile header lost its GitHub action")
    geometry = page.evaluate("""() => {
      const box = selector => {
        const node = document.querySelector(selector);
        if (!node) return null;
        const b = node.getBoundingClientRect(), s = getComputedStyle(node);
        if (!b.width || !b.height || s.display === 'none') return null;
        return {x:b.x,y:b.y,right:b.right,bottom:b.bottom,width:b.width,height:b.height};
      };
      return {header:box('[data-site-header]'),search:box('.site-search-wrap'),actions:box('.site-actions'),
        main:box('main'),hero:box('.blog-hero'),
        title:box('h1'),description:box('.blog-description'),meta:box('.blog-hero .blog-meta'),
        body:box('.blog-reading-column'),toc:box('.blog-toc'),tools:box('.blog-tools'),
        prose:box('.blog-content'),intro:box('.blog-index-intro'),cards:box('.blog-posts'),
        overflow:document.documentElement.scrollWidth-innerWidth,
        images:[...document.querySelectorAll('main img')].every(i=>i.complete&&i.naturalWidth>0)};
    }""")
    _check(geometry["overflow"] <= 1, f"{label}: document scrolls horizontally")
    _check(geometry["images"], f"{label}: blog image failed to load")
    if width <= 768:
        search, actions = geometry['search'], geometry['actions']
        _check(abs(search['y'] + search['height'] / 2 - actions['y'] - actions['height'] / 2) <= 1,
               f'{label}: mobile search and actions waste a third header row')
    _check(geometry["main"]["y"] >= geometry["header"]["bottom"] - 1,
           f"{label}: main content overlaps the header/banner")
    for key in ("title", "description", "meta", "body", "toc", "cards"):
        rect = geometry[key]
        if rect:
            _check(rect["x"] >= -1 and rect["right"] <= width + 1,
                   f"{label}: {key} escapes the viewport: {rect}")
    for above, below in (("title", "description"), ("description", "meta"),
                         ("hero", "tools"), ("tools", "prose"), ("intro", "cards")):
        a, b = geometry[above], geometry[below]
        if a and b:
            _check(a["bottom"] <= b["y"] + 1, f"{label}: {above} overlaps {below}")
    if geometry["toc"]:
        _check(geometry["body"]["right"] <= geometry["toc"]["x"] + 1,
               f"{label}: table of contents overlaps the article")
    return geometry


def _tooltip_bounds(page, tooltip, label):
    bounds = tooltip.bounding_box()
    viewport = page.viewport_size
    _check(bounds and bounds["x"] >= 0 and bounds["y"] >= 0
           and bounds["x"] + bounds["width"] <= viewport["width"] + 1
           and bounds["y"] + bounds["height"] <= viewport["height"] + 1,
           f"{label}: tooltip is outside the viewport: {bounds}")


def _butterfly_results(page, width, label):
    """Keep the real thirteen-column alignment readable on narrow screens."""
    result = page.locator('.blog-content .result-table').evaluate_all("""tables => {
      const table = tables.find(t => t.tHead?.rows[0]?.cells[0]?.textContent
        .trim().startsWith('target_accession'));
      if (!table) return null;
      const region = table.closest('.result-scroll');
      return {columns:table.tHead.rows[0].cells.length, rows:table.tBodies[0].rows.length,
        populated:[...table.tBodies[0].querySelectorAll('td')]
          .every(c=>c.textContent.trim() && c.textContent.trim() !== 'NULL'),
        contained:[...table.tBodies[0].querySelectorAll('td')]
          .every(c=>c.scrollWidth <= c.clientWidth + 1),
        cellWidth:Math.min(...[...table.tHead.rows[0].cells].map(c=>c.getBoundingClientRect().width)),
        viewport:region.clientWidth, content:region.scrollWidth,
        overflow:getComputedStyle(region).overflowX, focusable:region.tabIndex===0};
    }""")
    _check(result and result['columns'] == 13 and result['rows'] == 8 and result['populated'],
           f'{label}: expected eight populated alignment rows with thirteen columns: {result}')
    _check(result['cellWidth'] >= 100 and result['focusable'] and result['overflow'] == 'auto',
           f'{label}: alignment columns are squeezed or their scroll region is inaccessible: {result}')
    _check(result['contained'], f'{label}: a long alignment value overlaps another column: {result}')
    if width <= 1300:
        _check(result['content'] > result['viewport'],
               f'{label}: wide alignment has no horizontal scrolling: {result}')
    return result


def _copy_buttons(page, theme):
    """Copied/error feedback must return to readable theme CSS, not stale inline colors."""
    from playwright.sync_api import expect

    button = page.locator('.blog-content .doc-code [data-copy]').first
    page.evaluate("""() => {
      window.__copyOriginal = Object.getOwnPropertyDescriptor(navigator, 'clipboard');
      window.__copyExecOriginal = document.execCommand;
    }""")
    try:
        for success in (True, False):
            page.evaluate("""success => {
              Object.defineProperty(navigator, 'clipboard', {configurable:true, value:{
                writeText:async () => { if (!success) throw new Error('clipboard unavailable'); }
              }});
              document.execCommand = () => false;
            }""", success)
            button.click()
            expect(button).to_have_text('Copied' if success else 'Copy failed')
            for state in ('feedback', 'restored'):
                if state == 'restored':
                    expect(button).to_have_text('Copy')
                    _check(button.evaluate("node => !node.style.background && !node.style.color"),
                           f'{theme}: copied button kept inline colors after reset')
                page.wait_for_timeout(180)  # Let the authored 150 ms color transition finish.
                contrast = button.evaluate("""node => {
                  const luminance = color => color.match(/[\\d.]+/g).slice(0,3)
                    .map(Number).map(x=>x/255)
                    .map(x=>x<=.04045?x/12.92:((x+.055)/1.055)**2.4)
                    .reduce((sum,x,i)=>sum+x*[.2126,.7152,.0722][i],0);
                  const s=getComputedStyle(node), a=luminance(s.color), b=luminance(s.backgroundColor);
                  return (Math.max(a,b)+.05)/(Math.min(a,b)+.05);
                }""")
                _check(contrast >= 4.5,
                       f'{theme}: copy button {state} contrast is only {contrast:.2f}:1')
    finally:
        page.evaluate("""() => {
          if (window.__copyOriginal) Object.defineProperty(navigator, 'clipboard', window.__copyOriginal);
          else delete navigator.clipboard;
          document.execCommand = window.__copyExecOriginal;
          delete window.__copyOriginal;
          delete window.__copyExecOriginal;
        }""")


def _tooltips(page, output, theme):
    from playwright.sync_api import expect

    page.set_viewport_size({"width": 420, "height": 900})
    terms = page.locator('.blog-content a[data-glossary-term]').evaluate_all(
        'nodes => nodes.map(n => n.dataset.glossaryTerm)')
    _check(len(terms) == len(set(terms)), "glossary decorates more than the first occurrence of a term")
    glossary = page.locator('.blog-content a[data-glossary-description]').first
    _check(glossary.count() == 1, "blog has no automatically linked glossary terms")
    term = glossary.get_attribute("data-glossary-term")
    description = glossary.get_attribute("data-glossary-description")
    tip = page.locator('#blog-glossary-tip')
    glossary.hover()
    tip.wait_for(state="visible")
    _check(term in tip.inner_text() and description in tip.inner_text(),
           "glossary hover lost its authored term or description")
    _tooltip_bounds(page, tip, "glossary hover")
    page.mouse.move(2, 890)
    tip.wait_for(state="hidden")
    glossary.focus()
    tip.wait_for(state="visible")
    _check(glossary.get_attribute('aria-describedby') == tip.get_attribute('id'),
           "glossary focus lacks its accessible description")
    _tooltip_bounds(page, tip, "glossary focus")
    page.screenshot(path=str(output / f"glossary-{theme}-420.png"), animations="disabled")
    page.keyboard.press('Escape')
    tip.wait_for(state="hidden")
    _check(not glossary.get_attribute('aria-describedby'), "Escape left a stale glossary description")
    glossary.blur()
    glossary.focus()
    tip.wait_for(state='visible')
    page.mouse.wheel(0, 1800)
    tip.wait_for(state='hidden')
    _check(not glossary.get_attribute('aria-describedby'),
           'offscreen glossary anchor left a detached visible description')

    # Both prose code and highlighted runnable code use generated SQL entities.
    for name in ('mmsearch', 'bioseq.cigar', 'bioseq.score'):
        refs = page.locator('.blog-content a.entity-ref[data-entity-id]').filter(
            has_text=re.compile(r'^' + re.escape(name) + r'(?:\(\))?$'))
        _check(refs.count() >= 2, f"{name}: missing SQL links in prose or runnable code")
        code_ref = refs.locator('xpath=ancestor::code').count()
        _check(code_ref > 0, f"{name}: SQL code has no linked reference")
        ref = refs.last
        ref.scroll_into_view_if_needed()
        if name == 'mmsearch':
            ref.hover()
            page.locator('#entity-pop').wait_for(state="visible")
            _check(name in page.locator('#entity-pop').inner_text(), "SQL hover tooltip has wrong content")
            page.keyboard.press('Escape')
            page.locator('#entity-pop').wait_for(state="hidden")
        ref.focus()
        entity_tip = page.locator('#entity-pop')
        entity_tip.wait_for(state="visible")
        expect(entity_tip).to_have_css('opacity', '1')
        _check(name in entity_tip.inner_text(), f"{name}: SQL tooltip has wrong content")
        _tooltip_bounds(page, entity_tip, name)
        page.keyboard.press('Escape')
        entity_tip.wait_for(state="hidden")


def _publication(page, context, server, path):
    _check(page.locator('main article.blog-article').count() == 1, "post has no semantic article")
    _check(page.locator('h1').count() == 1, "post needs one title")
    meta = page.locator('.blog-hero .blog-meta')
    _check(meta.locator('time').get_attribute('datetime') == '2026-10-01', "first post has wrong publication date")
    reading = re.search(r'\b([1-9][0-9]*) min read\b', meta.inner_text())
    _check(reading and int(reading[1]) <= 15, "reading time is missing or counts hidden fixture SQL")
    keywords = page.locator('meta[name="keywords"]').get_attribute('content') or ''
    _check('PostgreSQL' in keywords and 'sequence alignment' in keywords, "authored SEO keywords are missing")
    linked_data = page.locator('script[type="application/ld+json"]').evaluate_all(
        'nodes => nodes.flatMap(n => { const d=JSON.parse(n.textContent); return d["@graph"] || [d]; })')
    article = next((item for item in linked_data if item.get('@type') == 'BlogPosting'), None)
    _check(article and article.get('datePublished') == '2026-10-01'
           and article.get('timeRequired') == f'PT{reading[1]}M', "BlogPosting metadata does not match the visible post")
    canonical = page.locator('link[rel="canonical"]').get_attribute('href')
    sitemap = context.request.get(server.url_for(path('sitemap.xml')))
    _check(sitemap.ok, "sitemap is unavailable")
    locations = {node.text for node in ElementTree.fromstring(sitemap.text()).iter()
                 if node.tag.endswith('}loc')}
    _check(canonical in locations, "blog entry is missing from the sitemap")

    markdown = page.locator('.blog-markdown')
    _check(markdown.inner_text().strip() == 'Markdown', "post Markdown button is missing")
    url = markdown.get_attribute('href')
    _check(url and url.startswith(path('blog/')) and url.endswith('.md'), "Markdown link is outside the mounted blog")
    _check(page.locator('link[rel="alternate"][type="text/markdown"]').get_attribute('href') == url,
           "Markdown alternate and visible link disagree")
    response = context.request.get(server.url_for(url))
    text = response.text()
    _check(response.ok and 'Introducing BioseqDB' in text and 'SELECT t.patient_name' in text,
           "published Markdown is not readable post content")
    _check('<!doctype' not in text.lower() and '-- bioseq:setup' not in text
           and 'SET bioseq.' not in text and 'DROP TABLE samples' not in text,
           "public Markdown contains HTML or hidden database fixtures")
    _check(response.headers.get('content-type', '').startswith('text/plain'),
           "Markdown must be served as plain text for an inline browser view")
    downloads = []
    on_download = lambda download: downloads.append(download)
    page.on('download', on_download)
    post_url = page.url
    markdown.click()
    page.wait_for_url(server.url_for(url))
    _check(not downloads and 'Introducing BioseqDB' in page.locator('body').inner_text()
           and page.locator('article').count() == 0,
           "Markdown action did not open the plain text document in the browser")
    page.remove_listener('download', on_download)
    # Return to the article for the remaining interaction and layout checks.
    page.goto(post_url, wait_until='networkidle')
    rows = page.locator('.blog-content .result-table tbody tr')
    _check(rows.count() == 5, "first post did not render its two executed SQL results")
    _check(all(cell.strip() and cell.strip() != 'NULL'
               for cell in page.locator('.blog-content .result-table tbody td').all_text_contents()),
           "blog SQL results contain NULL or empty cells")
    _check(page.locator('.blog-cta a[href*="/tutorials/"]').count() >= 1,
           "post footer has no documentation call to action")
    search = page.locator('#doc-search')
    search.fill('Introducing BioseqDB')
    result = page.locator('#search-dd a[data-search-result][href$="/blog/introducing-bioseqdb/"]')
    result.wait_for(state='visible')
    _check('Blog' in result.inner_text(), 'blog search result does not identify its content type')
    search.fill('')
    page.keyboard.press('Escape')
    search.blur()


def _banner(page):
    page.set_viewport_size({"width": 1920, "height": 900})
    page.evaluate('scrollTo(0,0)')
    _settle(page)
    banner = page.locator('[data-announcement]')
    _check(banner.count() == 1 and banner.is_visible(), "shared release announcement is missing")
    _check(banner.get_attribute('data-overflow') == 'false', "wide release banner unnecessarily scrolls")
    _check(page.locator('[data-announcement-toggle]').is_hidden(), "wide release banner has an unnecessary pause button")
    page.set_viewport_size({"width": 320, "height": 900})
    _settle(page)
    _check(banner.get_attribute('data-overflow') == 'true', "narrow release banner did not detect overflowing text")
    window = page.locator('[data-announcement-window]')
    button = page.locator('[data-announcement-toggle]')
    _check(button.is_visible(), "scrolling banner has no pause control")
    page.mouse.move(2, 890)
    page.evaluate('document.activeElement.blur()')
    _wait_scroll(page, window, 0)
    button.click()
    _check(button.get_attribute('aria-pressed') == 'true' and 'Resume' in button.get_attribute('aria-label'),
           "announcement pause control does not announce its paused state")
    page.mouse.move(2, 890)
    page.evaluate('document.activeElement.blur()')
    frozen = window.evaluate('node => node.scrollLeft')
    page.wait_for_timeout(300)
    _check(abs(window.evaluate('node => node.scrollLeft') - frozen) <= 1, "paused announcement kept moving")
    button.click()
    _check(button.get_attribute('aria-pressed') == 'false', "announcement did not resume")
    page.mouse.move(2, 890)
    _check(button.evaluate('node => document.activeElement === node'),
           'resume check must preserve keyboard focus on the button')
    _wait_scroll(page, window, frozen)

    # Native wheel scrolling happens after the listener. Automatic scrolling
    # must continue from that final position when the interaction hold ends.
    window.hover()
    before_wheel = window.evaluate('node => node.scrollLeft')
    page.mouse.wheel(140, 0)
    page.wait_for_timeout(150)
    after_wheel = window.evaluate('node => node.scrollLeft')
    _check(after_wheel > before_wheel + 50, 'wheel check did not manually scroll the banner')
    page.mouse.move(2, 890)
    _wait_scroll(page, window, after_wheel)

    # Native focus must reveal links outside the clipped announcement window.
    window.focus()
    for suffix in ('/issues', '/contact/'):
        page.keyboard.press('Tab')
        focused = page.locator(':focus')
        _check((focused.get_attribute('href') or '').endswith(suffix),
               f"announcement link {suffix} is not reachable in keyboard order")
        rect = focused.bounding_box()
        clip = window.bounding_box()
        _check(rect['x'] >= clip['x'] - 1 and rect['x'] + rect['width'] <= clip['x'] + clip['width'] + 1,
               f"focused announcement link {suffix} remained clipped")
    frozen = window.evaluate('node => node.scrollLeft')
    page.wait_for_timeout(300)
    _check(abs(window.evaluate('node => node.scrollLeft') - frozen) <= 1,
           "announcement moves while a reader focuses its links")


def _blog_browser(browser, server, path, output, offline):
    """Exercise real controls against enough fixture posts to cross page boundaries."""
    from playwright.sync_api import expect

    index_url = server.url_for(path('blog/'))
    context = browser.new_context(viewport={'width': 1600, 'height': 1000})
    context.route('**/*', offline)
    errors = []

    def fixture(route):
        if urlsplit(route.request.url).path != path('blog/'):
            route.fallback()
            return
        response = route.fetch()
        html = response.text()
        articles = list(re.finditer(r'<article class="blog-card"[^>]*>.*?</article>', html, re.S))
        # Keep one real BioseqDB post and add 14 isolated database posts. This
        # tests paging even while the real site has fewer than seven entries.
        _check(articles, 'blog paging fixture could not find a generated card')
        original = articles[0].group()
        bioseq = re.sub(r'data-blog-category="[^"]*"', 'data-blog-category="bioseqdb"', original)
        copies = [bioseq]
        for number in range(1, 15):
            title = f'Fixture post {number:02d}'
            card = re.sub(r'data-blog-category="[^"]*"', 'data-blog-category="others"', original)
            card = re.sub(r'data-blog-date="[^"]*"', f'data-blog-date="2026-09-{number:02d}"', card)
            card = re.sub(r'data-blog-title="[^"]*"', f'data-blog-title="{title}"', card)
            card = re.sub(r'data-blog-search-text="[^"]*"',
                          'data-blog-search-text="quasar in full text"' if number == 8 else 'data-blog-search-text="ordinary storage notes"', card)
            card = re.sub(r'(<h2><a\b[^>]*>).*?(</a></h2>)', lambda m: m[1] + title + m[2], card, flags=re.S)
            topic = 'Fixture even' if number % 2 == 0 else 'Fixture odd'
            card = re.sub(r'(<ul class="blog-card-keywords"[^>]*>).*?(</ul>)',
                          lambda m: m[1] + f'<li data-blog-keyword>{topic}</li>' + m[2], card, flags=re.S)
            copies.append(card)
        html = html[:articles[0].start()] + '\n'.join(copies) + html[articles[-1].end():]
        route.fulfill(response=response, body=html)

    context.route('**/*', fixture)
    page = context.new_page()
    page.on('pageerror', lambda error: errors.append(str(error)))
    page.goto(index_url, wait_until='networkidle')
    expect(page.locator('[data-blog-browser]')).to_have_attribute('data-blog-ready', 'true')
    visible = page.locator('[data-blog-card]:visible')
    expect(visible).to_have_count(1)
    expect(page.locator('[data-blog-category-button="bioseqdb"]')).to_have_attribute('aria-pressed', 'true')
    page.locator('[data-blog-category-button="others"]').click()
    expect(visible).to_have_count(6)
    expect(page.locator('[data-blog-result-count]')).to_have_text('14 posts · Others')
    expect(page.locator('[data-blog-page-number]')).to_have_text('Page 1 of 3')
    _check(visible.first.get_attribute('data-blog-title') == 'Fixture post 14', 'newest sorting is incorrect')
    _check(page.evaluate('localStorage.getItem("bioseq-blog-category")') == 'others', 'category was not saved')
    page.goto(index_url, wait_until='networkidle')
    expect(page.locator('[data-blog-category-button="others"]')).to_have_attribute('aria-pressed', 'true')
    expect(visible).to_have_count(6)
    page.locator('[data-blog-sort]').select_option('oldest')
    _check(visible.first.get_attribute('data-blog-title') == 'Fixture post 01', 'oldest sorting is incorrect')
    page.locator('[data-blog-sort]').select_option('title')
    page.locator('[data-blog-next]').click()
    expect(page.locator('[data-blog-page-number]')).to_have_text('Page 2 of 3')
    _check(visible.first.get_attribute('data-blog-title') == 'Fixture post 07', 'second page starts at wrong post')
    page.locator('[data-blog-next]').click()
    expect(visible).to_have_count(2)
    expect(page.locator('[data-blog-next]')).to_be_disabled()
    page.locator('[data-blog-previous]').click()
    expect(visible).to_have_count(6)
    expect(page.locator('[data-blog-page-number]')).to_have_text('Page 2 of 3')
    page.locator('[data-blog-keyword-select]').select_option('Fixture even')
    expect(page.locator('[data-blog-page-number]')).to_have_text('Page 1 of 2')
    expect(page.locator('[data-blog-result-count]')).to_have_text('7 posts · Others')
    page.locator('[data-blog-search]').fill('quasar')
    expect(visible).to_have_count(1)
    _check(visible.first.get_attribute('data-blog-title') == 'Fixture post 08', 'dedicated search omitted the authored body')
    _check(page.locator('#doc-search').input_value() == '', 'blog search changed the shared header search')
    page.locator('[data-blog-search]').fill('this-does-not-exist')
    expect(visible).to_have_count(0)
    expect(page.locator('[data-blog-empty]')).to_be_visible()
    expect(page.locator('[data-blog-pagination]')).to_be_hidden()
    page.locator('[data-blog-reset]').click()
    expect(page.locator('[data-blog-search]')).to_be_focused()
    expect(page.locator('[data-blog-category-button="others"]')).to_have_attribute('aria-pressed', 'true')
    expect(visible).to_have_count(6)

    # A category change in another tab follows the same storage event contract
    # as the theme preference, without throwing away this tab's search text.
    sibling = context.new_page()
    sibling.goto(index_url, wait_until='networkidle')
    sibling.locator('[data-blog-category-button="bioseqdb"]').click()
    expect(page.locator('[data-blog-category-button="bioseqdb"]')).to_have_attribute('aria-pressed', 'true')
    expect(visible).to_have_count(1)
    sibling.locator('[data-blog-category-button="others"]').click()
    expect(visible).to_have_count(6)
    sibling.close()
    page.goto(index_url + '?category=others&q=quasar&sort=oldest&page=999', wait_until='networkidle')
    expect(visible).to_have_count(1)
    expect(page.locator('[data-blog-search]')).to_have_value('quasar')
    expect(page.locator('[data-blog-page-number]')).to_have_text('Page 1 of 1')
    page.locator('[data-blog-reset]').click()

    layouts = []
    for theme in ('dark', 'light'):
        if page.locator('html').get_attribute('data-theme') != theme:
            page.locator('[data-theme-toggle]').click()
            page.locator('[data-theme-toggle]').blur()
        for width in WIDTHS:
            page.set_viewport_size({'width': width, 'height': 1000})
            page.evaluate('scrollTo(0,0)')
            _settle(page)
            label = f'filtered-index/{theme}/{width}'
            report = {'theme': theme, 'width': width, 'geometry': _layout(page, width, label)}
            controls = page.locator('.blog-filters :is(input,select)').evaluate_all('nodes => nodes.map(node => { const r=node.getBoundingClientRect(); return {left:r.left,right:r.right,width:r.width}; })')
            _check(all(box['left'] >= 0 and box['right'] <= width and box['width'] >= 90 for box in controls),
                   f'{label}: a blog filter is clipped or too narrow')
            if width in (420, 1600):
                page.screenshot(path=str(output / f'filters-{theme}-{width}.png'), full_page=True, animations='disabled')
                if theme == 'light':
                    report['contrast'] = _light_readability(page, label)
            layouts.append(report)
    _check(not errors, f'blog controls raised browser errors: {errors}')
    context.close()

    blocked = browser.new_context()
    blocked.route('**/*', offline)
    blocked.add_init_script("""for (const method of ['getItem','setItem']) {
      const original = Storage.prototype[method];
      Storage.prototype[method] = function(key, ...args) {
        if (key === 'bioseq-blog-category') throw new DOMException('Storage unavailable', 'SecurityError');
        return original.call(this, key, ...args);
      };
    }""")
    page = blocked.new_page()
    page.goto(index_url, wait_until='networkidle')
    expect(page.locator('[data-blog-category-button="bioseqdb"]')).to_have_attribute('aria-pressed', 'true')
    page.locator('[data-blog-category-button="others"]').click()
    expect(page.locator('[data-blog-category-button="others"]')).to_have_attribute('aria-pressed', 'true')
    blocked.close()

    nojs = browser.new_context(java_script_enabled=False)
    nojs.route('**/*', offline)
    page = nojs.new_page()
    page.goto(index_url, wait_until='networkidle')
    _check(page.locator('[data-blog-card]').count() == page.locator('[data-blog-card]:visible').count(),
           'server-rendered posts disappear without JavaScript')
    expect(page.locator('[data-blog-controls]')).to_be_hidden()
    nojs.close()
    (output / 'filters-report.json').write_text(json.dumps({'layouts': layouts}, indent=2) + '\n')


def exercise_blog(browser, server, base_path, artifact_dir: Path):
    """Exercise actual generated pages, preserving network isolation of the main suite."""
    root = base_path.rstrip('/')
    path = lambda suffix='': root + '/' + suffix.lstrip('/')
    output = Path(artifact_dir) / 'blog'
    output.mkdir(parents=True, exist_ok=True)
    reports, errors, unexpected_requests = [], [], []

    def offline(route):
        host = urlsplit(route.request.url).hostname
        if host in ('127.0.0.1', 'localhost', server._host):
            route.continue_()
        elif route.request.url == GOOGLE_TAG_URL:
            route.fulfill(status=200, content_type='application/javascript', body='/* offline analytics */')
        elif serve_badge(route):
            pass
        else:
            unexpected_requests.append(route.request.url)
            route.abort()

    context = browser.new_context(viewport={'width': 1600, 'height': 1000})
    context.route('**/*', offline)
    page = context.new_page()
    page.on('pageerror', lambda error: errors.append(str(error)))
    for theme in ('dark', 'light'):
        for name, route in (
            ('index', path('blog/')),
            ('post', path('blog/introducing-bioseqdb/')),
            ('butterflies', path('blog/comparing-butterfly-genomes-with-bioseqdb/')),
        ):
            page.goto(server.url_for(route), wait_until='networkidle')
            if page.locator('html').get_attribute('data-theme') != theme:
                page.locator('[data-theme-toggle]').click()
                page.mouse.move(2, 990)
                page.locator('[data-theme-toggle]').blur()
            _check(page.locator('[data-site-header] .site-navigation a[href$="/blog/"]').count() == 1,
                   'Blog is missing from the unified header')
            if name == 'index':
                _check(page.locator('.blog-card h2 a[href$="/introducing-bioseqdb/"]').count() == 1,
                       'first post is missing from the blog index')
            elif name == 'post':
                _publication(page, context, server, path)
            for width in WIDTHS:
                page.set_viewport_size({'width': width, 'height': 1000})
                page.evaluate('scrollTo(0,0)')
                _settle(page)
                label = f'{name}/{theme}/{width}'
                report = {'page': name, 'theme': theme, 'width': width, 'geometry': _layout(page, width, label)}
                if name == 'butterflies':
                    report['alignment_table'] = _butterfly_results(page, width, label)
                if width in (420, 1600):
                    screenshot = output / f'{name}-{theme}-{width}.png'
                    page.screenshot(path=str(screenshot), full_page=True, animations='disabled')
                    report['screenshot'] = screenshot.name
                    if name == 'butterflies':
                        page.screenshot(path=str(output / f'{name}-opening-{theme}-{width}.png'),
                                        animations='disabled')
                    if theme == 'light':
                        report['contrast'] = _light_readability(page, label)
                reports.append(report)
            if name == 'post':
                _copy_buttons(page, theme)
                _tooltips(page, output, theme)
    _banner(page)
    _check(not errors, f'blog or banner raised browser errors: {errors}')
    context.close()
    _blog_browser(browser, server, path, output, offline)

    reduced = browser.new_context(viewport={'width': 320, 'height': 900}, reduced_motion='reduce')
    reduced.route('**/*', offline)
    page = reduced.new_page()
    page.goto(server.url_for(path('blog/')), wait_until='networkidle')
    _check(page.locator('[data-announcement-toggle]').is_hidden(), 'reduced-motion banner shows a motion control')
    window = page.locator('[data-announcement-window]')
    page.wait_for_timeout(2500)
    _check(window.evaluate('node => node.scrollLeft') == 0, 'reduced-motion banner auto-scrolls')
    window.focus()
    page.keyboard.press('Tab')
    page.keyboard.press('Tab')
    _check((page.locator(':focus').get_attribute('href') or '').endswith('/contact/'),
           'reduced-motion reader cannot reach the contact link')
    _check(window.evaluate('node => node.scrollLeft') > 0, 'reduced-motion banner hides focused links')
    reduced.close()
    _check(not unexpected_requests, f'blog depended on unexpected network requests: {unexpected_requests}')
    (output / 'report.json').write_text(json.dumps({'layouts': reports}, indent=2) + '\n')
