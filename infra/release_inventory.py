"""Pure release-inventory rules shared by builders and publishers.

Keep this module dependency-free: native package tests and publishing do not
need the benchmark fleet's AWS/configuration dependencies.
"""


def cell_key(cell):
    """Identify one architecture/distro/PostgreSQL package cell."""
    return f"pg{cell['postgres_major']}/{cell['distro']}/{cell['architecture']}"


def package_release_ready(package):
    """Require payload validation and the current installed-SQL checks."""
    return (package.get("validation", {}).get("passed") is True
            and all(package.get("installed_sql_smoke", {}).get(field) is True
                    for field in ("passed", "default_insert_search", "default_persisted_search",
                                  "column_defaults_and_vacuum")))


def pending_package_repairs(state, inventory):
    """Keep diagnosed archive repairs pending through admission and failures.

    This tracks only explicit repairs, never Git age or compiled-input hashes.
    A qualified replacement with different bytes releases the old archive hold.
    History retains the hold even after a failed worker takes another cell.
    """
    packages = {cell_key(p): p for p in inventory.get("packages", [])}
    pending = {cell_key(c) for name in ("package_rebuilds", "deferred_package_rebuilds")
               for queue in state.get(name, {}).values() for c in queue}
    jobs = [entry.get("previous_job", {}) for entry in state.get("history", [])]
    jobs.extend(state.get("jobs", {}).values())
    for job in jobs:
        previous_sha = job.get("replaces_package_sha256")
        if not job.get("cell") or not previous_sha:
            continue
        key = cell_key(job["cell"])
        replacement = packages.get(key, {})
        if replacement.get("sha256") == previous_sha or not package_release_ready(replacement):
            pending.add(key)
    return pending
