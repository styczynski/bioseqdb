#!/usr/bin/env python3
"""Generate checked SQL/SeqKit listings and the downloadable tutorial bundle.

Run --write after changing canonical queries or measured SeqKit previews.
--check verifies committed content without a database or network connection.
The CMake execution phase independently runs every sql runnable block.
"""
import argparse
import json
import re
import zipfile
import io
from pathlib import Path
import xml.etree.ElementTree as ET

WEBSITE = Path(__file__).resolve().parents[1]
SOURCE = WEBSITE / 'static/examples/seqkit-workflows'
POST = WEBSITE / 'blog/seqkit-workflows-in-bioseqdb.md'
CSS = WEBSITE / 'static/css/blog.css'
ARCHIVE = WEBSITE / 'static/examples/seqkit-workflows.zip'
SETTINGS = ('SET max_parallel_workers_per_gather = 0;\n'
            'SET bioseq.index.insert_maintenance.enabled = off;\n'
            'SET bioseq.aligner.vulkan.enabled = off;\n')
SINGLE = {'preflight', 'schema', 'import', 'windows', 'inspect', 'base_counts'}


def render(identifier, queries, previews):
    full = (SOURCE / 'fixture_setup.sql').read_text()
    if identifier in ('preflight', 'schema'):
        setup = SETTINGS
    elif identifier == 'import':
        setup = SETTINGS + queries['schema']
    elif identifier == 'windows':
        setup = full.split('CREATE TABLE seqkit_lab.windows')[0]
    else:
        setup = full
    teardown = 'SELECT 1;' if identifier == 'preflight' else 'DROP SCHEMA seqkit_lab CASCADE;'
    sql = (f'```sql runnable id=seqkit_{identifier} timeout=60s\n'
           f'-- bioseq:setup\n{setup.rstrip()}\n'
           f'-- bioseq:visible\n{queries[identifier].rstrip()}\n'
           f'-- bioseq:teardown\n{teardown}\n```')
    if identifier in SINGLE:
        return sql
    preview = previews[identifier]
    # Shell outputs are measured by SeqKit, not invented by the website renderer.
    other = '```sh\n' + preview['command'] + '\n```\n\n```text\n' + preview['output'] + '\n```'
    return (f'<div class="tool-comparison" role="group" aria-label="{identifier.replace("_", " ").capitalize()} comparison">\n'
            '<div class="tool-comparison-panel">\n<p class="tool-comparison-label">BioseqDB</p>\n\n'
            + sql + '\n\n</div>\n<div class="tool-comparison-panel">\n'
            '<p class="tool-comparison-label">SeqKit</p>\n\n' + other + '\n\n</div>\n</div>')


def bundle():
    output = io.BytesIO()
    with zipfile.ZipFile(output, 'w', zipfile.ZIP_DEFLATED) as archive:
        for path in sorted(SOURCE.rglob('*')):
            if not path.is_file() or '__pycache__' in path.parts or path.name == 'fixture_setup.sql':
                continue
            info = zipfile.ZipInfo('seqkit-workflows/' + str(path.relative_to(SOURCE)), date_time=(2026, 10, 19, 0, 0, 0))
            info.compress_type = zipfile.ZIP_DEFLATED
            info.external_attr = 0o644 << 16
            archive.writestr(info, path.read_bytes())
    return output.getvalue()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument('--write', action='store_true')
    mode.add_argument('--check', action='store_true')
    args = parser.parse_args()
    queries = json.loads((SOURCE / 'queries.json').read_text())
    previews = json.loads((SOURCE / 'seqkit-previews.json').read_text())
    original = POST.read_text()
    identifiers = re.findall(r'<!-- seqkit:([a-z_]+):start -->', original)
    if not identifiers or len(set(identifiers)) != len(identifiers):
        raise ValueError('Missing or duplicate SeqKit listing markers')
    rendered = original
    for identifier in identifiers:
        pattern = f'<!-- seqkit:{identifier}:start -->.*?<!-- seqkit:{identifier}:end -->'
        block = f'<!-- seqkit:{identifier}:start -->\n{render(identifier, queries, previews)}\n<!-- seqkit:{identifier}:end -->'
        rendered, n = re.subn(pattern, lambda _: block, rendered, flags=re.S)
        if n != 1:
            raise ValueError(f'Expected one complete marker pair for {identifier}')
    expected_queries = '\\set ON_ERROR_STOP on\n\n' + '\n\n'.join(
        '-- ' + k + '\n' + v for k, v in queries.items()
        if k not in ('schema', 'import', 'windows', 'preflight'))
    start, end = '/* seqkit-figure-ratios:start */', '/* seqkit-figure-ratios:end */'
    rules = []
    for figure in sorted((WEBSITE / 'static/figures/blog/seqkit-workflows').glob('*.svg')):
        _, _, w, h = ET.parse(figure).getroot().attrib['viewBox'].split()
        rules.append(f'.blog-content img[src$="/figures/blog/seqkit-workflows/{figure.name}"] {{ aspect-ratio: {w} / {h}; }}')
    block = start + '\n' + '\n'.join(rules) + '\n' + end
    css = CSS.read_text()
    new_css = re.sub(re.escape(start)+'.*?'+re.escape(end), lambda _: block, css, flags=re.S) if start in css else css.rstrip()+'\n\n'+block+'\n'
    outputs = {POST: rendered.encode(), CSS: new_css.encode(), SOURCE / '03_queries.sql': expected_queries.encode()}
    mismatches = []
    for path, data in outputs.items():
        if args.write:
            path.write_bytes(data)
        elif not path.exists() or path.read_bytes() != data:
            mismatches.append(str(path.relative_to(WEBSITE)))
    archive = bundle()
    if args.write:
        ARCHIVE.write_bytes(archive)
    elif not ARCHIVE.exists() or ARCHIVE.read_bytes() != archive:
        mismatches.append(str(ARCHIVE.relative_to(WEBSITE)))
    if mismatches:
        raise SystemExit('Stale SeqKit content; run --write: ' + ', '.join(mismatches))
    print(f'SeqKit tutorial: {len(identifiers)} SQL listings, {len(identifiers)-len(set(identifiers)&SINGLE)} comparisons checked')

if __name__ == '__main__':
    main()
