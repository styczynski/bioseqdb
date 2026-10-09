#!/usr/bin/env python3
r"""Export the original project's TikZ and embedded matplotlib figures as SVG.

Requires TeX Live (latex, dvisvgm, standalone, TikZ, pgfplots), numpy and
matplotlib. Run from any directory; checked-in SVGs keep these dependencies out
of the normal website build. Example:
    python3 website/tools/export_thesis_figures.py --jobs 4

The source graph starts at main.tex and follows metadata !partial paths and
\\input/\\include recursively. Text, plot data and drawing commands come from
those sources unchanged apart from the requested thesis -> project wording.
The website palette is applied at export time; the paper's source colors and
all drawing geometry, labels and plot data remain unchanged.
"""
from __future__ import annotations

import argparse
import concurrent.futures
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import textwrap
import xml.etree.ElementTree as ET
from theme_palette import theme_svg

REPO = Path(__file__).resolve().parents[2]
SOURCE = REPO / 'papers/src/content/bioseqdb/thesis'
OUTPUT = REPO / 'website/static/figures/project'
TIKZ = re.compile(r'\\begin\{tikzpicture\}.*?\\end\{tikzpicture\}', re.S)
PYTHON = re.compile(r'\\begin\{CacheMeCode\}[^\n]*\n(.*?)\\end\{CacheMeCode\}', re.S)
BACKGROUND = '071320'
FOREGROUND = 'CBD8E4'
PALETTE = {
    'Blue': '46B8DE',
    'Teal': '7DD3CF',
    'Red': 'F2897F',
    'Orange': 'F4894F',
    'Gray': '849BB0',
    'Green': '8FD6BA',
    'Purple': 'B5A0E8',
    'Yellow': 'E6C063',
    'BlueLine': '46B8DE',
    'TealLine': '7DD3CF',
    'RedLine': 'F2897F',
    'OrangeLine': 'F4894F',
}


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


def tex_palette():
    # Redefine the mixing endpoints before the author's color aliases/styles.
    # Thus black!12 becomes a subtle light grid on navy, while Blue!10 remains
    # a tinted panel. Merely replacing final SVG black would leave pale panels
    # behind light labels and lose the source's contrast relationships.
    colors = {'black': FOREGROUND, 'white': BACKGROUND,
              'WebsiteSurface': BACKGROUND}
    colors.update({name.lower(): value for name, value in PALETTE.items()
                   if not name.endswith('Line')})
    colors['cyan'] = PALETTE['Blue']
    colors['magenta'] = PALETTE['Purple']
    colors.update({'Thesis' + name: value for name, value in PALETTE.items()})
    return '\n'.join(r'\definecolor{' + name + '}{HTML}{' + value + '}'
                     for name, value in colors.items())


def theme_tex_colors(text):
    # xcolor's implicit mix endpoint is hardcoded RGB white, even when the
    # named white color is redefined. Spell out that endpoint so tint fills
    # use the website surface, including styles shared by multiple drawings.
    return re.sub(r'(!\d+(?:\.\d+)?)(?![\d.!])', r'\1!WebsiteSurface', text)


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
''' + tex_palette() + '\n' + theme_tex_colors(definitions) + '\n\\begin{document}\n\\color{black}\n'


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
    tex.write_text(preamble(definitions) + theme_tex_colors(item['_content']) + '\n\\end{document}\n')
    env = os.environ.copy()
    env['TEXINPUTS'] = str(SOURCE) + os.pathsep + env.get('TEXINPUTS', '')
    command(['latex', '-interaction=nonstopmode', '-halt-on-error', 'figure.tex'], work, env)
    command(['dvisvgm', '--no-fonts', '--exact', '--bbox=papersize', '--linkmark=none', '--output=figure.svg', 'figure.dvi'], work)
    svg = work / 'figure.svg'
    # hypertex treats site-absolute paths as DVI files. Restore website routes
    # after vector export while leaving ordinary external HTTPS links intact.
    content = re.sub(r'file:(/[^\"\']*?)\.dvi#', r'\1#', svg.read_text())
    # Hypertex/font icon specials occasionally emit SVG's literal black
    # independently of the TeX color stack. Adapt only their paint attributes.
    content = re.sub(r'''(fill|stroke)=(['"])black\2''',
                     lambda m: m[1] + '=' + m[2] + '#' + FOREGROUND.lower() + m[2], content)
    svg.write_text(content)
    return svg


def render_python(item, work):
    work.mkdir(parents=True, exist_ok=True)
    content = textwrap.dedent(item['_content']).strip()
    for name, value in PALETTE.items():
        key = re.sub(r'([a-z])([A-Z])', r'\1_\2', name).upper()
        content = content.replace('__THESIS_' + key + '__', value)
    content = content.replace('__LINEWIDTH__', str(16 / 2.54))
    content = content.replace('matplotlib.use("pgf")', 'matplotlib.use("svg")')
    content = content.replace("matplotlib.use('pgf')", "matplotlib.use('svg')")
    content = content.replace('get_filename_from_extension(".pgf")', '"figure.svg"')
    content = content.replace("get_filename_from_extension('.pgf')", '"figure.svg"')
    # Only color literals are adapted; author data, labels and artist geometry
    # are executed unchanged. Opaque marker centers still mask the line below.
    literals = {'white': '#' + BACKGROUND, 'black': '#' + FOREGROUND,
                '#000000': '#' + FOREGROUND, '#333333': '#' + FOREGROUND,
                '#555555': '#9BB0C0', '#e5e5e5': '#344B5E'}
    content = re.sub(r'([\'"])(white|black|#[0-9a-fA-F]{6})\1',
                     lambda m: m[1] + literals.get(m[2], m[2]) + m[1], content)
    # Fixed metadata and hash salt make checked-in SVG output reproducible.
    # transparent=True removes the figure and axes backgrounds, not the bars,
    # confidence bands or marker centers that carry information.
    settings = {'svg.hashsalt': 'bioseq-website', 'text.color': '#' + FOREGROUND,
                'axes.labelcolor': '#' + FOREGROUND, 'axes.edgecolor': '#849BB0',
                'xtick.color': '#' + FOREGROUND, 'ytick.color': '#' + FOREGROUND,
                'grid.color': '#849BB0', 'hatch.color': '#' + FOREGROUND}
    bootstrap = 'import matplotlib\nmatplotlib.rcParams.update(' + repr(settings) + ')\n'
    content = re.sub(r'(plt\.savefig\([^\n]*)(\))',
                     r'\1, transparent=True, metadata={"Date": None}\2', content)
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
        # Matplotlib's multiline path data otherwise leaves trailing spaces.
        # SVG treats these as equivalent separators; keep generated diffs clean.
        destination.write_text(theme_svg('\n'.join(line.rstrip() for line in svg.read_text().splitlines()) + '\n'))
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
            themed = theme_svg(destination.read_text())
            if themed != destination.read_text():
                destination.write_text(themed)
            attrs = ET.parse(destination).getroot().attrib
            item.update({key: attrs[key] for key in ('width', 'height', 'viewBox') if key in attrs})
            item['svg_sha256'] = hashlib.sha256(destination.read_bytes()).hexdigest()
    (OUTPUT / 'manifest.json').write_text(json.dumps({
        'entrypoint': 'papers/src/content/bioseqdb/thesis/main.tex',
        'theme': {'name': 'website-adaptive', 'background': 'transparent',
                  'surface': '#' + BACKGROUND, 'foreground': '#' + FOREGROUND,
                  'palette': {name: '#' + value for name, value in PALETTE.items()}},
        'sources': [p.relative_to(SOURCE).as_posix() for p in paths],
        'figures': items,
    }, indent=2) + '\n')
    print(f'{len(items)} figures, {len(failures)} failures', flush=True)
    return bool(failures)


if __name__ == '__main__':
    sys.exit(main())
