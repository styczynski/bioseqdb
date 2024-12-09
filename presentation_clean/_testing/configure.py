
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
        all_paths[path] = ",".join(all_indices)
    elif isinstance(data, dict):
        for (key, element) in data.items():
            all_paths.update(_extract_paths(f"{path}.{key}", element))
            all_indices.append(str(key))
        all_paths[path] = ",".join(all_indices)
    else:
        all_paths[path] = str(data)
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
        tex.append(f"\\ifnum\\pdfstrcmp{{#1}}{{{escape(path_value)}}}=0{{{escape(value)}}}\\else")
    tex.append("??")
    for _ in range(len(paths)):
        tex.append("\\fi")
    tex += [
        "}",
        "\\newcommand{\\foreachdata}[2]{",
        "\def\\v{\data{#1}}",
        "\expanded{\\noexpand\\foreach\\noexpand#2 in \\v}",
        "}",
        "\\newcommand\dataif[2]{\ifnum\pdfstrcmp\data{#1}{#2}=0}"
        "\\makeatother"
    ]
    return "".join(tex)

if __name__ == '__main__':
    x = _convert({
        "a": 5,
        "b": [4242, 2137, 6969, 420],
        "c": [
            {
                "d": 2137,
            },
            {
                "d": 6969,
            },
        ],
    })
    print(x)