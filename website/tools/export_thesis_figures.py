#!/usr/bin/env python3
r"""Export the original project's TikZ and embedded matplotlib figures as SVG.

Requires TeX Live (latex, dvisvgm, standalone, TikZ, pgfplots), numpy and
matplotlib. Run from any directory; checked-in SVGs keep these dependencies out
of the normal website build. Example:
    python3 website/tools/export_thesis_figures.py --jobs 4

The source graph starts at main.tex and follows metadata !partial paths and
\\input/\\include recursively. Text, plot data and drawing commands come from
those sources unchanged apart from the requested thesis -> project wording.
"""
from __future__ import annotations

import argparse
import concurrent.futures
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import textwrap
import xml.etree.ElementTree as ET

REPO = Path(__file__).resolve().parents[2]
SOURCE = REPO / 'papers/src/content/bioseqdb/thesis'
OUTPUT = REPO / 'website/static/figures/project'
TIKZ = re.compile(r'\\begin\{tikzpicture\}.*?\\end\{tikzpicture\}', re.S)
PYTHON = re.compile(r'\\begin\{CacheMeCode\}[^\n]*\n(.*?)\\end\{CacheMeCode\}', re.S)


def source_graph():
    seen = set()
    result = []
    def visit(path):
        if not path.suffix:
            path = path.with_suffix('.tex')
        path = path.resolve()
        if path in seen:
            return
        if not path.is_file():
            raise FileNotFoundError(path)
        seen.add(path)
        result.append(path)
        text = path.read_text()
        includes = re.findall(r'^%\s*path:\s*(\S+)', text, re.M)
        includes += re.findall(r'\\(?:input|include)\{([^{}]+)\}', text)
        for name in includes:
            if '\\' in name:
                continue  # CacheMeCode's dynamically generated PGF name.
            target = SOURCE / name
            if not target.with_suffix('.tex').exists() and not target.exists():
                target = path.parent / name
            visit(target)
    visit(SOURCE / 'main.tex')
    return result


def balanced_end(text, start):
    depth = 0
    for position in range(start, len(text)):
        char = text[position]
        if char in '{}':
            backslashes = len(text[:position]) - len(text[:position].rstrip('\\'))
            if backslashes % 2:
                continue
            depth += 1 if char == '{' else -1
            if not depth:
                return position + 1
    raise ValueError('Unbalanced TeX argument')


def drawing_definitions(paths):
    definitions = []
    for path in paths:
        if path.name == 'figure_palette.tex':
            continue
        text = TIKZ.sub('', path.read_text())
        text = PYTHON.sub('', text)
        for match in re.finditer(r'\\(tikzset|pgfplotsset|colorlet|def)\b', text):
            start = text.find('{', match.end())
            if start < 0:
                continue
            end = balanced_end(text, start)
            if match.group(1) == 'colorlet':
                end = balanced_end(text, text.find('{', end))
            definitions.append(text[match.start():end])
    return '\n'.join(definitions)


def project_wording(text):
    text = re.sub(r'\bthesis\b', lambda m: 'Project' if m[0][0].isupper() else 'project', text, flags=re.I)
    # This cross-reference is the only one inside a drawing. Use its website
    # destination rather than an undefined standalone LaTeX figure number.
    return text.replace(
        r'\ref{fig:sequence-abstraction-layers}',
        r'\href{/v0.1/tutorials/project_sequence_abstractions/#fig-sequence-abstraction-layers}{(figure)}',
    )


def preamble(definitions):
    return r'''\def\pgfsysdriver{pgfsys-dvisvgm.def}
\documentclass[tikz,border=4pt]{standalone}
\usepackage[T1]{fontenc}
\usepackage[utf8]{inputenc}
\usepackage{lmodern}
\usepackage{amsmath,amssymb,mathtools}
\usepackage{pgfplots,pgfplotstable}
\usepackage{graphicx,booktabs,array,multirow,fontawesome5}
\usepackage[hypertex]{hyperref}
\hypersetup{colorlinks=true,allcolors=blue}
\usetikzlibrary{arrows.meta,calc,positioning,fit,backgrounds,decorations.pathreplacing,decorations.pathmorphing,patterns,shapes.geometric,shapes.misc,matrix,chains,intersections}
\usepgfplotslibrary{groupplots,fillbetween,statistics}
\pgfplotsset{compat=1.18}
\setlength{\textwidth}{16cm}
\setlength{\linewidth}{16cm}
\renewcommand{\footnotemark}{}
\input{figure_palette.tex}
''' + definitions + '\n\\begin{document}\n'


def figures(paths):
    items = []
    for path in paths:
        text = path.read_text()
        source = path.relative_to(SOURCE).as_posix()
        slug = re.sub(r'[^a-zA-Z0-9]+', '-', source.removesuffix('.tex')).strip('-')
        for kind, pattern in [('tikz', TIKZ), ('python', PYTHON)]:
            for index, match in enumerate(pattern.finditer(text)):
                content = match.group(1) if kind == 'python' else match.group()
                name = f'{slug}-{kind}-{index + 1:02}.svg'
                items.append({
                    'source': source,
                    'kind': kind,
                    'index': index,
                    'svg': 'figures/project/' + name,
                    'source_sha256': hashlib.sha256(content.encode()).hexdigest(),
                    '_content': project_wording(content),
                })
    return items


def command(args, cwd, env=None):
    result = subprocess.run(args, cwd=cwd, env=env, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, text=True)
    if result.returncode:
        raise RuntimeError(f'{args[0]} failed in {cwd}:\n{result.stdout[-7000:]}')


def render_tikz(item, definitions, work):
    work.mkdir(parents=True, exist_ok=True)
    tex = work / 'figure.tex'
    tex.write_text(preamble(definitions) + item['_content'] + '\n\\end{document}\n')
    env = os.environ.copy()
    env['TEXINPUTS'] = str(SOURCE) + os.pathsep + env.get('TEXINPUTS', '')
    command(['latex', '-interaction=nonstopmode', '-halt-on-error', 'figure.tex'], work, env)
    command(['dvisvgm', '--no-fonts', '--exact', '--bbox=papersize', '--linkmark=none', '--output=figure.svg', 'figure.dvi'], work)
    return work / 'figure.svg'


def render_python(item, work):
    work.mkdir(parents=True, exist_ok=True)
    content = textwrap.dedent(item['_content']).strip()
    palette = (SOURCE / 'figure_palette.tex').read_text()
    for name, value in re.findall(r'\\newcommand\{\\Thesis(\w+)Hex\}\{([0-9A-F]+)\}', palette):
        key = re.sub(r'([a-z])([A-Z])', r'\1_\2', name).upper()
        content = content.replace('__THESIS_' + key + '__', value)
    content = content.replace('__LINEWIDTH__', str(16 / 2.54))
    content = content.replace('matplotlib.use("pgf")', 'matplotlib.use("svg")')
    content = content.replace("matplotlib.use('pgf')", "matplotlib.use('svg')")
    content = content.replace('get_filename_from_extension(".pgf")', '"figure.svg"')
    content = content.replace("get_filename_from_extension('.pgf')", '"figure.svg"')
    # Fixed metadata and hash salt make checked-in SVG output reproducible.
    bootstrap = "import matplotlib\nmatplotlib.rcParams['svg.hashsalt'] = 'bioseq-website'\n"
    content = re.sub(r'(plt\.savefig\([^\n]*)(\))', r'\1, metadata={"Date": None}\2', content)
    script = work / 'figure.py'
    script.write_text(bootstrap + content + '\n')
    command([sys.executable, str(script)], work)
    return work / 'figure.svg'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--jobs', type=int, default=4)
    parser.add_argument('--only', help='Only export matching source/SVG names')
    parser.add_argument('--manifest-only', action='store_true')
    parser.add_argument('--work-dir', type=Path, default=Path(tempfile.gettempdir()) / 'bioseq-website-figures')
    args = parser.parse_args()
    paths = source_graph()
    items = figures(paths)
    definitions = drawing_definitions(paths)
    OUTPUT.mkdir(parents=True, exist_ok=True)
    def render(item):
        destination = OUTPUT / Path(item['svg']).name
        if args.manifest_only or args.only and args.only not in item['svg']:
            return
        work = args.work_dir / destination.stem
        svg = render_tikz(item, definitions, work) if item['kind'] == 'tikz' else render_python(item, work)
        shutil.copyfile(svg, destination)
        print('Exported ' + item['svg'], flush=True)
    failures = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as pool:
        tasks = {pool.submit(render, item): item for item in items}
        for task in concurrent.futures.as_completed(tasks):
            try:
                task.result()
            except Exception as error:
                item = tasks[task]
                failures.append(item['svg'])
                print(f"FAILED {item['svg']}: {error}", file=sys.stderr, flush=True)
    for item in items:
        item.pop('_content')
        destination = OUTPUT / Path(item['svg']).name
        if destination.is_file():
            attrs = ET.parse(destination).getroot().attrib
            item.update({key: attrs[key] for key in ('width', 'height', 'viewBox') if key in attrs})
    (OUTPUT / 'manifest.json').write_text(json.dumps({
        'entrypoint': 'papers/src/content/bioseqdb/thesis/main.tex',
        'sources': [p.relative_to(SOURCE).as_posix() for p in paths],
        'figures': items,
    }, indent=2) + '\n')
    print(f'{len(items)} figures, {len(failures)} failures', flush=True)
    return bool(failures)


if __name__ == '__main__':
    sys.exit(main())
