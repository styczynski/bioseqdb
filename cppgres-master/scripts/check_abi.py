import json
import sys


def load_struct_layout(file_path):
    """Load struct layout JSON file."""
    with open(file_path, "r") as f:
        return json.load(f)


struct_ignore_list = ["exception"]

def compare_struct_layouts(old_layout, new_layout):
    """Compare struct layouts between two versions."""
    errors = []

    for struct_name, old_struct in old_layout.items():
        if struct_name in struct_ignore_list or struct_name.startswith("__"):
            continue

        if struct_name not in new_layout:
            errors.append(f"❌ struct {struct_name} is missing in the C++ version!")
            continue

        new_struct = new_layout[struct_name]

        # Compare struct size
        if old_struct["size"] != new_struct["size"]:
            errors.append(
                f"❌ struct {struct_name}: Size mismatch (C: {old_struct['size']} bytes, C++: {new_struct['size']} bytes)")

        # Compare field offsets
        old_fields = {field["type"]: field["offset"] for field in old_struct["fields"]}
        new_fields = {field["type"]: field["offset"] for field in new_struct["fields"]}

        for field_name, old_offset in old_fields.items():
            if field_name not in new_fields:
                errors.append(f"❌ struct {struct_name}: Field '{field_name}' is missing in C++ version!")
            elif new_fields[field_name] != old_offset:
                errors.append(
                    f"❌ Struct {struct_name}: Field '{field_name}' offset mismatch (C: {old_offset}, C++: {new_fields[field_name]})")

    return errors


# Load struct JSON files
if len(sys.argv) < 3:
    print("Usage: python compare_structs.py c.json cxx.json")
    sys.exit(1)

old_layout = load_struct_layout(sys.argv[1])
new_layout = load_struct_layout(sys.argv[2])

# Compare and print results
errors = compare_struct_layouts(old_layout, new_layout)

if errors:
    print("\n".join(errors))
    sys.exit(1)  # Return non-zero for CI checks
