import sys
import os
import glob
import re
import json
import subprocess
from multiprocessing.dummy import Pool

SUPPORTED_LANGUAGES = [
    "polish",
    "english",
]

USE_PACKAGE_PATTERNS = [f'\\\\{macro}(\\[.*\\])?{{([^}}]+)}}' for macro in ["usepackage", "documentclass", "RequirePackage"]]
TLMGR_PACKAGE_PATTERN = '^([A-Za-z0-9_\\\\-]+):$'
TLMGR_WORD_SEARCH_PATTERN = '^([A-Za-z0-9_\\\\-]+) - (.*)$'

pool = Pool(50)

def eprint(*args, **kwargs):
    print(*args, file=sys.stderr, **kwargs)

def detect_packages(tex_files_globs):
    packages = []
    glob_files = set()
    glob_files_names = set()
    for glob_pattern in tex_files_globs:
        for path in glob.glob(glob_pattern, recursive=True):
            glob_files.add(path)
            glob_files_names.add(os.path.basename(path))
    for tex_path in glob_files:
        eprint(tex_path)
        with open(tex_path, 'r') as tex_file:
            for raw_line in tex_file:
                line = raw_line.strip()
                for pattern in USE_PACKAGE_PATTERNS:
                    use_package_search = re.search(pattern, line, re.IGNORECASE)
                    if use_package_search:
                        for new_package in [pkg.strip() for pkg in use_package_search.group(2).split(",")]:
                            if f"{new_package}.cls" not in glob_files_names and f"{new_package}.sty" not in glob_files_names:
                                packages.append((new_package, None))
                            else:
                                eprint(f"[i] Skip local package '{new_package}'")
    # languages
    for lang in SUPPORTED_LANGUAGES:
        packages.append((lang, "ldf"))
    return packages


def tlmgr_lookup_worker(p):
    (package, extension) = p
    try:
        # tlmgr search --file "" 
        eprint(f"Check {package}")
        lookup_file_name = f"{package}.sty"
        if extension:
            lookup_file_name = f"{package}.{extension}"
        output = subprocess.check_output(" ".join(["tlmgr", "search", "--global", "--file", f"/{lookup_file_name}"]), shell=True).decode("utf-8")
        # split it into a list that makes sense
        lines = [line.strip() for line in output.split('\n')]
        found_packages = []
        for line_no in range(len(lines)):
            line =  lines[line_no]
            tlmgr_package_search = re.search(TLMGR_PACKAGE_PATTERN, line, re.IGNORECASE)
            if tlmgr_package_search and not line.startswith("tlmgr:"):
                package_name = tlmgr_package_search.group(1)
                package_match = lines[line_no+1]
                found_packages.append((package_name, package_match))
                line_no += 1
            line_no += 1
        found_packages = [(name, path) for (name, path) in found_packages if path.split("/")[-1].strip() == lookup_file_name]
        if len(found_packages) == 0:
            #raise KeyError(f"Package '{package}' was not found")
            # FALLBACK
            # tlmgr search --global --word "beamer"
            if extension is not None:
                # Cannot do retry
                raise KeyError(f"Package '{package}' (extension: '{extension}') was not found")
            output = subprocess.check_output(" ".join(["tlmgr", "search", "--global", f"{package}"]), shell=True).decode("utf-8")
            lines = [line.strip() for line in output.split('\n')]
            for line in lines:
                tlmgr_package_search = re.search(TLMGR_WORD_SEARCH_PATTERN, line, re.IGNORECASE)
                if tlmgr_package_search and not line.startswith("tlmgr:"):
                    package_name = tlmgr_package_search.group(1).strip()
                    if package_name == package:
                        found_packages.append((package_name, ""))
            if len(found_packages) == 0:
                eprint(lines)
                raise KeyError(f"Package '{package}' was not found")
            else:
                eprint(f"Found package '{package}' on retried by-word search")
                return p, found_packages[0][0], None

        elif len(found_packages) == 1:
            return p, found_packages[0][0], None
        else:
            eprint(f"Found multiple candidates for package '{package}':")
            for found_package in found_packages:
                eprint(f"  - {found_package}")
            #raise KeyError(f"Package '{package}' has multiple candidates")
            return p, found_packages[0][0], None
    except Exception as e:
        return p, None, e

def tlmgr_install_package_worker(package):
    for cmd_args in [
        ["tlmgr", "install", package],
        ["tlmgr", "path", "add"],
    ]:
        cmd = " ".join(cmd_args)
        eprint(f"Run '{cmd}'")
        subprocess.run(cmd, shell=True, stdout=sys.stderr, stderr=sys.stderr)
    

def tlmgr_install_package(packages):
    #for package in packages:
    cmd = " ".join(["tlmgr", "install", *packages])
    eprint(f"Run '{cmd}'")
    subprocess.run(cmd, shell=True, stdout=sys.stderr, stderr=sys.stderr)
    # for package in packages:
    #     cmd = " ".join(["tlmgr", "install", package])
    #     eprint(f"Run '{cmd}'")
    #     subprocess.run(cmd, shell=True, stdout=sys.stderr, stderr=sys.stderr)
    # for package_name, err in pool.imap(tlmgr_install_package_worker, packages):
    #     if err:
    #         raise err
    #     eprint(f"Installed '{package_name}'")

def tlmgr_lookup(packages):
    packages_to_install = dict()
    if len(packages) == 0:
        return packages_to_install
    for p, package_name, err in pool.imap(tlmgr_lookup_worker, packages):
        if err:
            raise err
        packages_to_install[f"{p[0]}.{p[1]}"] = package_name
    return packages_to_install

def read_lock(lock_file_path):
    lock_mapping = dict()
    if not os.path.exists(lock_file_path):
        eprint(f"Lock '{lock_file_path}' file does not exists anywhere")
        return lock_mapping
    try:
        with open(lock_file_path, 'r') as lock_file:
           lock_mapping = json.load(lock_file)
    except Exception as e:
        eprint(f"Cannot load lock file: '{lock_file_path}'")
        eprint(e)
        pass
    return lock_mapping

def tlmgr_check_lock(lock_file_path, used_packages):
    lock_mapping = read_lock(lock_file_path)
    
    existing_packages = []
    unknown_packages = []

    eprint(f"Packages from lock '{lock_file_path}':")
    eprint(set(lock_mapping.keys()))

    for package in used_packages:
        pacakge_key = f"{package[0]}.{package[1]}"
        if pacakge_key in lock_mapping:
            eprint(f"[X] Package {package[0]} ({pacakge_key}) loaded from lockfile")
            lock_p = lock_mapping[pacakge_key]
            existing_packages.append(lock_p)
        else:
            eprint(f"[.] Package {package[0]} ({pacakge_key}) loaded from lockfile")
            unknown_packages.append(package)
    existing_packages = list(set(existing_packages))

    eprint(f"{len(existing_packages)} existing packages, {len(unknown_packages)} packages to check")
    return existing_packages, unknown_packages

def tlmgr_update_lock(lock_file_path, packages_lookup):
    if len(packages_lookup) == 0:
        eprint('Nothing to update in lock file')
        return
    lock_mapping = read_lock(lock_file_path)
    was_updated = False
    for key, name in packages_lookup.items():
        if key not in lock_mapping or lock_mapping[key] != name:
            was_updated = True
            lock_mapping[key] = name
    if was_updated:
        with open(lock_file_path, 'w') as lock_file:
            json.dump(lock_mapping, lock_file)
            eprint(f"Lock file '{lock_file_path}' was updated ({len(lock_mapping)} entries)")
    else:
        eprint('Nothing to update in lock file')

def tlmgr_install(lock_file_path, tex_files_globs):
    used_packages = list(set(detect_packages(tex_files_globs)))    
    existing_packages, unknown_packages = tlmgr_check_lock(lock_file_path, used_packages)
    packages_lookup = tlmgr_lookup(unknown_packages)
    tlmgr_update_lock(lock_file_path, packages_lookup)
    tlmgr_install_package([*existing_packages, *list(packages_lookup.values())])

if __name__ == '__main__':
    tlmgr_install(sys.argv[1], sys.argv[2:])