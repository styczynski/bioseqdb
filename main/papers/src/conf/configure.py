import os, sys
sys.path.append(os.path.abspath(os.path.join(os.path.dirname(__file__), "..")))

from conf.vars import (
    create_vars_tex,
    create_ltex_json,
    create_vars_json,
)
from conf.root_gen import generate_root_tex

if __name__ == '__main__':
    if sys.argv[1] == "mode-tex":
        create_vars_tex(
            vars_file_name=sys.argv[2],
            meta_file_name=sys.argv[3],
            main_file_name=sys.argv[4],
            vars_str_defs=sys.argv[5:],
        )
    elif sys.argv[1] == "mode-ltex-json":
        create_ltex_json(
            ltex_json_file_name=sys.argv[2],
            meta_file_name=sys.argv[3],
            main_file_name=sys.argv[4],
            vars_str_defs=sys.argv[5:],
        )
    elif sys.argv[1] == "mode-json":
        create_vars_json(
            vars_file_name=sys.argv[2],
            meta_file_name=sys.argv[3],
            main_file_name=sys.argv[4],
            vars_str_defs=sys.argv[5:],
        )
    elif sys.argv[1] == "create-root":
        generate_root_tex(
            output_file=sys.argv[2],
            resource_files=sys.argv[3:],
        )