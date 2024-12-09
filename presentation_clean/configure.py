import sys
from datetime import datetime
import yaml
import yaml_include
import os

# End YAML parser

SPECIAL_VALUES = dict(
    CURRENT_DATE=datetime.today().strftime('%Y-%m-%d'),
)

def _merge(a: dict, b: dict, path=[]):
    for key in b:
        if key in a:
            if isinstance(a[key], dict) and isinstance(b[key], dict):
                _merge(a[key], b[key], path + [str(key)])
            elif a[key] != b[key]:
                # Conflict
                a[key] = b[key]
        else:
            a[key] = b[key]
    return a

# Define digit mapping
romanNumeralMap = (('M', 1000),
                   ('CM', 900),
                   ('D', 500),
                   ('CD', 400),
                   ('C', 100),
                   ('XC', 90),
                   ('L', 50),
                   ('XL', 40),
                   ('X', 10),
                   ('IX', 9),
                   ('V', 5),
                   ('IV', 4),
                   ('I', 1))

_replacements = {
    "&": r"\&",
    "%": r"\%",
    "$": r"\$",
    "#": r"\#",
    "_": r"\_",
    "{": r"\{",
    "}": r"\}",
    "~": r"\textasciitilde{}",
    "^": r"\^{}",
    "\\": r"\textbackslash{}",
    "\n": "\\newline%\n",
    "-": r"{-}",
    "\xA0": "~",  # Non-breaking space
    "[": r"{[}",
    "]": r"{]}",
}
_replacement_lookup = str.maketrans(_replacements)


def escape(string):
    """
    Replace special characters with their equivalent LaTeX macros.

    Args:
        string (str): The string to process.

    Returns:
        str: The string with special characters replaced with macros.
    """
    return string.translate(_replacement_lookup)

def toRoman(n):
    """convert integer to Roman numeral"""
    if not isinstance(n, int):
        raise Exception("decimals cannot be converted")
    if not (-1 < n < 5000):
        raise Exception("number out of range (must be 0..4999)")

    # special case
    if n == 0:
        return 'N'

    result = ""
    for numeral, integer in romanNumeralMap:
        while n >= integer:
            result += numeral
            n -= integer
    return result

def _extract_paths(path, data):
    all_paths = dict()
    all_indices = []
    if isinstance(data, list):
        for (index, element) in enumerate(data):
            all_paths.update(_extract_paths(f"{path}.{index}", element))
            all_indices.append(str(index))
        all_paths[path] = "{"+",".join([escape(idx) for idx in all_indices])+"}"
    elif isinstance(data, dict):
        for (key, element) in data.items():
            all_paths.update(_extract_paths(f"{path}.{key}", element))
            all_indices.append(str(key))
        all_paths[path] = "{"+",".join([escape(idx) for idx in all_indices])+"}"
    else:
        all_paths[path] = f"{{{escape(str(data))}}}"
    return all_paths

def _convert(data):
    tex = [
        "\\makeatletter",
        "\\def\\data#1{",
    ]
    # Extract paths
    paths = list(enumerate(sorted(_extract_paths("", data).items())))
    for (index, (path, value)) in paths:
        path_value = path.removeprefix(".")
        tex.append(f"\\ifnum\\pdfstrcmp{{#1}}{{{escape(path_value)}}}=0{value}\\else")
    tex.append("??")
    for _ in range(len(paths)):
        tex.append("\\fi")
    tex += [
        "}",
        "\\providecommand{\\foreachdata}[2]{",
        "\\def\\v{\\data{#1}}",
        "\\expanded{\\noexpand\\foreach\\noexpand#2 in \\v}",
        "}",
        "\\providecommand\\dataeq[2]{TT\\fi\\edef\\v{\\expanded{\\data{#1}}}\\ifnum\\pdfstrcmp\\v{#2}=0}",
        "\\makeatother"
    ]
    return tex # [f"{line}" for line in tex]


def configure(
    vars_file_name,
    meta_file_name,
    main_file_name,
    vars_str_defs,
):
    yaml.add_constructor("!include", yaml_include.Constructor(base_dir=os.path.dirname(main_file_name)))

    defs = dict()
    with open(meta_file_name, 'r') as meta_file:
        defs = yaml.full_load(meta_file)

    for var_str in vars_str_defs:
        tokens = var_str.split("=")
        if len(tokens) > 1:
            [var_name, var_value, *_] = tokens
            defs[var_name] = var_value
    
    # Load main
    extracted_main_meta_lines = []
    with open(main_file_name, "r") as main_file:
        for line_raw in main_file:
            line = line_raw.replace("\n", "").replace("\r", "").strip()
            if line.startswith("%"):
                buf_line = line.removeprefix("%")
                if len(buf_line.strip()) > 0:
                    extracted_main_meta_lines.append(buf_line)
            if len(line) > 0 and not line.startswith("%"):
                # Non-comment detected
                break
    if len(extracted_main_meta_lines) > 0:
        base_indent = len(extracted_main_meta_lines[0]) - len(extracted_main_meta_lines[0].lstrip())
        yaml_str = "\n".join([line[base_indent:] for line in extracted_main_meta_lines])
        print(yaml_str)
        main_meta = yaml.full_load(yaml_str)
        defs = _merge(defs, main_meta)
        print(defs)

    # for var_name, var_value in defs.items():
    #     if var_value in ["true", "false"]:
    #         vars_latex_defs.append(f"\\newif\\ifbuild{var_name}\n")
    #         vars_latex_switches_defs.append(f"\\build{var_name}{var_value}\n")
    #     elif var_value.strip() in SPECIAL_VALUES:
    #         vars_latex_defs.append(f"\\def\\build{var_name}{{{str(SPECIAL_VALUES[var_value.strip()]).strip()}}}\n")
    #     else:
    #         vars_latex_defs.append(f"\\def\\build{var_name}{{{str(var_value).strip()}}}\n")

    print(f"Output Latex config to: {vars_file_name}")    
    with open(vars_file_name, "w") as output_file:
        output_file.writelines([
            "%% Auto-generated variables\n",
            "\n",
            *_convert(defs),
            "\n",
            "%% End\n",
        ])

if __name__ == '__main__':
    configure(
        vars_file_name=sys.argv[1],
        meta_file_name=sys.argv[2],
        main_file_name=sys.argv[3],
        vars_str_defs=sys.argv[4:],
    )