import sys

def configure(
    vars_file_name,
    vars_str_defs,
):
    vars_latex_defs = []
    vars_latex_switches_defs = []
    for var_str in vars_str_defs:
        tokens = var_str.split("=")
        if len(tokens) > 1:
            [var_name, var_value, *_] = tokens
            if var_value in ["true", "false"]:
                vars_latex_defs.append(f"\\newif\\ifbuild{var_name}\n")
                vars_latex_switches_defs.append(f"\\build{var_name}{var_value}\n")
            else:
                vars_latex_defs.append(f"\\def\\build{var_name}{{{str(var_value).strip()}}}\n")
    print(f"Output Latex config to: {vars_file_name}")    
    with open(vars_file_name, "w") as output_file:
        output_file.writelines(vars_latex_defs + ["\n% Switches: \n"] + vars_latex_switches_defs)

if __name__ == '__main__':
    configure(
        vars_file_name=sys.argv[1],
        vars_str_defs=sys.argv[2:],
    )