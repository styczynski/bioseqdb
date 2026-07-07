import re
import json
import sys


def parse_clang_layout(file_path):
    """Parses Clang's -fdump-record-layouts output into a JSON structure."""
    structs = {}
    current_struct = None
    field_pattern = re.compile(r"^\s*(\d+)\s*\|\s\s\s([^\s].+)")
    size_pattern = re.compile(r"^\s*\|?\s*\[\s*sizeof=(\d+),(?:\s*dsize=\d+,)?\s*align=(\d+)\s*")


    previous_line_was_marker = False

    with open(file_path, "r") as file:

        for line in file:
            line = line.strip()
            if line.startswith("*** Dumping AST Record Layout"):
                previous_line_was_marker = True
                continue  # Skip to next line

            # Match struct declaration
            struct_match = re.match(r"^\s*\d+\s*\|\s*struct (.+)", line)
            if struct_match and previous_line_was_marker:
                # Cleanup
                if current_struct is not None and current_struct.startswith("(unnamed"):
                    del structs[current_struct]
                current_struct = struct_match.group(1)
                structs[current_struct] = {"fields": [], "size": None, "align": None}
                previous_line_was_marker = False  # Reset marker flag
                continue

            union_match = re.match(r"^\s*\d+\s*\|\s*union (.+)", line)
            if union_match and previous_line_was_marker:
                # Cleanup
                if current_struct is not None and current_struct.startswith("(unnamed"):
                    del structs[current_struct]
                current_struct = None
                continue

            # Match field declaration
            if current_struct:
                field_match = field_pattern.match(line)
                if field_match:
                    offset = int(field_match.group(1))
                    field_info = field_match.group(2).strip().rpartition(" ")[-1]
                    structs[current_struct]["fields"].append({"offset": offset, "type": field_info})

                # Match struct size and alignment
                size_match = size_pattern.match(line)
                if size_match:
                    structs[current_struct]["size"] = int(size_match.group(1))
                    structs[current_struct]["align"] = int(size_match.group(2))

    return structs


# Read and convert struct layouts
parsed_layouts = parse_clang_layout(sys.argv[1])

# Save as JSON
with open(sys.argv[2], "w") as json_file:
    json.dump(parsed_layouts, json_file, indent=4)
