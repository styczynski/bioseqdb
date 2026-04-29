import sys, subprocess, os

build_path = sys.argv[1]
normalized_target = "autobuild_"+build_path.replace("/", "_").replace("\\", "_")

print(f"{normalized_target}")
#print(["bash", "-c", "'"+" ".join(["make", normalized_target])+"'"])
#subprocess.run(["bash", "-c", "'"+" ".join(["make", normalized_target])+"'"], shell=True)

# additional_path = os.path.abspath("build/venv/bin")
# print(f"APPEND PATH = {additional_path}")

new_path = os.pathsep.join([
    os.environ.get("PATH", ""),
    os.path.abspath("build/venv/bin"),
    os.path.abspath("build/venv/bin/pygmentize")
])
curr_env = os.environ.copy()
modified_env = curr_env | {"PATH": new_path}

#subprocess.run(["which", "pygmentize"], shell=True, env=modified_env)
subprocess.run(["make", normalized_target], shell=True, env=modified_env)