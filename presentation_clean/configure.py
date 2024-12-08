import sys
from datetime import datetime

# © didlly AGPL-3.0 License - github.com/didlly

def is_float(string: str) -> bool:
    try:
        float(string)
        return True
    except ValueError:
        return False


def is_integer(string: str) -> bool:
    try:
        int(string)
        return True
    except ValueError:
        return False


def load(path: str) -> dict:
    with open(path, "r") as yaml:
        levels = []
        data = {}
        indentation_str = ""

        for line in yaml.readlines():
            if line.replace(line.lstrip(), "") != "" and indentation_str == "":
                indentation_str = line.replace(line.lstrip(), "").rstrip("\n")
            if line.strip() == "":
                continue
            elif line.rstrip()[-1] == ":":
                key = line.strip()[:-1]
                quoteless = (
                    is_float(key)
                    or is_integer(key)
                    or key == "True"
                    or key == "False"
                    or ("[" in key and "]" in key)
                )

                if len(line.replace(line.strip(), "")) // 2 < len(levels):
                    if quoteless:
                        levels[len(line.replace(line.strip(), "")) // 2] = f"[{key}]"
                    else:
                        levels[len(line.replace(line.strip(), "")) // 2] = f"['{key}']"
                else:
                    if quoteless:
                        levels.append(f"[{line.strip()[:-1]}]")
                    else:
                        levels.append(f"['{line.strip()[:-1]}']")
                if quoteless:
                    exec(
                        f"data{''.join(str(i) for i in levels[:line.replace(line.lstrip(), '').count(indentation_str) if indentation_str != '' else 0])}[{key}]"
                        + " = {}"
                    )
                else:
                    exec(
                        f"data{''.join(str(i) for i in levels[:line.replace(line.lstrip(), '').count(indentation_str) if indentation_str != '' else 0])}['{key}']"
                        + " = {}"
                    )

                continue

            key = line.split(":")[0].strip()
            value = ":".join(line.split(":")[1:]).strip()

            if (
                is_float(value)
                or is_integer(value)
                or value == "True"
                or value == "False"
                or ("[" in value and "]" in value)
            ):
                if (
                    is_float(key)
                    or is_integer(key)
                    or key == "True"
                    or key == "False"
                    or ("[" in key and "]" in key)
                ):
                    exec(
                        f"data{'' if line == line.strip() else ''.join(str(i) for i in levels[:line.replace(line.lstrip(), '').count(indentation_str) if indentation_str != '' else 0])}[{key}] = {value}"
                    )
                else:
                    exec(
                        f"data{'' if line == line.strip() else ''.join(str(i) for i in levels[:line.replace(line.lstrip(), '').count(indentation_str) if indentation_str != '' else 0])}['{key}'] = {value}"
                    )
            else:
                if (
                    is_float(key)
                    or is_integer(key)
                    or key == "True"
                    or key == "False"
                    or ("[" in key and "]" in key)
                ):
                    exec(
                        f"data{'' if line == line.strip() else ''.join(str(i) for i in levels[:line.replace(line.lstrip(), '').count(indentation_str) if indentation_str != '' else 0])}[{key}] = '{value}'"
                    )
                else:
                    exec(
                        f"data{'' if line == line.strip() else ''.join(str(i) for i in levels[:line.replace(line.lstrip(), '').count(indentation_str) if indentation_str != '' else 0])}['{key}'] = '{value}'"
                    )
    return data


def loads(yaml: str) -> dict:
    levels = []
    data = {}
    indentation_str = ""

    for line in yaml.split("\n"):
        if line.replace(line.lstrip(), "") != "" and indentation_str == "":
            indentation_str = line.replace(line.lstrip(), "")
        if line.strip() == "":
            continue
        elif line.rstrip()[-1] == ":":
            key = line.strip()[:-1]
            quoteless = (
                is_float(key)
                or is_integer(key)
                or key == "True"
                or key == "False"
                or ("[" in key and "]" in key)
            )

            if len(line.replace(line.strip(), "")) // 2 < len(levels):
                if quoteless:
                    levels[len(line.replace(line.strip(), "")) // 2] = f"[{key}]"
                else:
                    levels[len(line.replace(line.strip(), "")) // 2] = f"['{key}']"
            else:
                if quoteless:
                    levels.append(f"[{line.strip()[:-1]}]")
                else:
                    levels.append(f"['{line.strip()[:-1]}']")
            if quoteless:
                exec(
                    f"data{''.join(str(i) for i in levels[:line.replace(line.lstrip(), '').count(indentation_str) if indentation_str != '' else 0])}[{key}]"
                    + " = {}"
                )
            else:
                exec(
                    f"data{''.join(str(i) for i in levels[:line.replace(line.lstrip(), '').count(indentation_str) if indentation_str != '' else 0])}['{key}']"
                    + " = {}"
                )

            continue

        key = line.split(":")[0].strip()
        value = ":".join(line.split(":")[1:]).strip()

        if (
            is_float(value)
            or is_integer(value)
            or value == "True"
            or value == "False"
            or ("[" in value and "]" in value)
        ):
            if (
                is_float(key)
                or is_integer(key)
                or key == "True"
                or key == "False"
                or ("[" in key and "]" in key)
            ):
                exec(
                    f"data{'' if line == line.strip() else ''.join(str(i) for i in levels[:line.replace(line.lstrip(), '').count(indentation_str) if indentation_str != '' else 0])}[{key}] = {value}"
                )
            else:
                exec(
                    f"data{'' if line == line.strip() else ''.join(str(i) for i in levels[:line.replace(line.lstrip(), '').count(indentation_str) if indentation_str != '' else 0])}['{key}'] = {value}"
                )
        else:
            if (
                is_float(key)
                or is_integer(key)
                or key == "True"
                or key == "False"
                or ("[" in key and "]" in key)
            ):
                exec(
                    f"data{'' if line == line.strip() else ''.join(str(i) for i in levels[:line.replace(line.lstrip(), '').count(indentation_str) if indentation_str != '' else 0])}[{key}] = '{value}'"
                )
            else:
                exec(
                    f"data{'' if line == line.strip() else ''.join(str(i) for i in levels[:line.replace(line.lstrip(), '').count(indentation_str) if indentation_str != '' else 0])}['{key}'] = '{value}'"
                )

    return data


def dumps(yaml: dict, indent="") -> str:
    """A procedure which converts the dictionary passed to the procedure into it's yaml equivalent.

    Args:
        yaml (dict): The dictionary to be converted.

    Returns:
        data (str): The dictionary in yaml form.
    """

    data = ""

    for key in yaml.keys():
        if type(yaml[key]) == dict:
            data += f"\n{indent}{key}:\n"
            data += dumps(yaml[key], f"{indent}  ")
        else:
            data += f"{indent}{key}: {yaml[key]}\n"

    return data

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

def configure(
    vars_file_name,
    meta_file_name,
    main_file_name,
    vars_str_defs,
):
    vars_latex_defs = []
    vars_latex_switches_defs = []

    defs = dict()
    defs = load(meta_file_name)

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
        main_meta = loads("\n".join(extracted_main_meta_lines))
        defs = _merge(defs, main_meta)

    for var_name, var_value in defs.items():
        if var_value in ["true", "false"]:
            vars_latex_defs.append(f"\\newif\\ifbuild{var_name}\n")
            vars_latex_switches_defs.append(f"\\build{var_name}{var_value}\n")
        elif var_value.strip() in SPECIAL_VALUES:
            vars_latex_defs.append(f"\\def\\build{var_name}{{{str(SPECIAL_VALUES[var_value.strip()]).strip()}}}\n")
        else:
            vars_latex_defs.append(f"\\def\\build{var_name}{{{str(var_value).strip()}}}\n")

    print(f"Output Latex config to: {vars_file_name}")    
    with open(vars_file_name, "w") as output_file:
        output_file.writelines(vars_latex_defs + ["\n% Switches: \n"] + vars_latex_switches_defs)

if __name__ == '__main__':
    configure(
        vars_file_name=sys.argv[1],
        meta_file_name=sys.argv[2],
        main_file_name=sys.argv[3],
        vars_str_defs=sys.argv[4:],
    )