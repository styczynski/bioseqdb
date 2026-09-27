import os
import sys
from collections import namedtuple
from string import Template

def eprint(*args, **kwargs):
    print(*args, file=sys.stderr, **kwargs)

TemplateConfPaths = namedtuple('TemplateConfPaths', [
    'main',
    'main_path',
    'main_comment_header',
    'doc_preamble',
    'doc_epilogue',
    'doc_class',
    'common',
])

ROOT_TEMPLATE = Template("""$main_comment_header
\input{vars.tex}
$doc_preamble

$main

$doc_epilogue            
\end{document}
""")

def template_load(path):
    with open(path, 'r') as f:
        return f.read()

def replace_tex_root_directive(
    main_tex_path,
    root_directive_path,
):
    with open(main_tex_path, 'r') as main_tex_file:
        lines = main_tex_file.readlines()
        prev_lines = lines
        if len(lines) > 0:
            if lines[0].lower().replace(" ", "").startswith("%!texroot="):
                lines = lines[1:]
        lines = [f"%!TEX root = {root_directive_path}\n"] + lines
    if lines != prev_lines:
        with open(main_tex_path, 'w') as main_tex_file_w:
            main_tex_file_w.writelines(lines)
        eprint(f"[OK] Updated tex root directive in: {main_tex_path}")

def extract_comment_header(path):
    comment_lines = []
    with open(path, 'r') as tex_file:
        lines = tex_file.readlines()
        for line in lines:
            if line.replace(" ", "").strip().startswith("%"):
                comment_lines.append(line)
            elif len(line) > 0:
                break
    return "".join(comment_lines)

def generate_root_tex(
    output_file,
    resource_files,
):
    conf = TemplateConfPaths(
        main=None,
        main_path=None,
        main_comment_header=None,
        doc_preamble=None,
        doc_epilogue=None,
        doc_class=None,
        common=None,
    )
    #print(resource_files)
    for path in resource_files:
        parent_name = os.path.basename(os.path.dirname(path))
        rel_out_path = os.path.relpath(path, os.path.dirname(output_file))
        is_in_defs = parent_name == "documents_defs"
        if os.path.basename(path) == "main.tex" and conf.main is None:
            conf = conf._replace(
                main = f"\input{{{rel_out_path}}}",
                main_path = path,
                main_comment_header=extract_comment_header(path),
            )
            #conf = conf._replace(main = template_load(path))
        elif is_in_defs and path.endswith(".tex") and conf.doc_preamble is None:
            conf = conf._replace(doc_preamble = template_load(path))
        elif is_in_defs and path.endswith(".epilogue.tex") and conf.doc_epilogue is None:
            conf = conf._replace(doc_epilogue = template_load(path))
        elif is_in_defs and path.endswith(".cls") and conf.doc_class is None:
            conf = conf._replace(doc_class = template_load(path))
        elif os.path.basename(path) == "common.tex" and conf.common is None:
            conf = conf._replace(common = f"\input{{{rel_out_path}}}")

    eprint(f"[i] Generated Latex root file: '{output_file}' for main '{conf.main}'")
    rendered_content = ROOT_TEMPLATE.substitute(
        {k: (v or "") for k, v in conf._asdict().items()}
    )
    if conf.common:
        rendered_content = rendered_content.replace("\input{common.tex}", conf.common)
        
    with open(output_file, 'w') as out_file:
        out_file.write(rendered_content)

    tex_root_directive_path = os.path.relpath(output_file, os.path.dirname(os.path.abspath(conf.main_path)))
    replace_tex_root_directive(
        main_tex_path=conf.main_path,
        root_directive_path=tex_root_directive_path,
    )
