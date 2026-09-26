"""MCP-owned container assembly from previously qualified native packages."""
from __future__ import annotations

import os
from pathlib import Path
import sys

from package_linux import _run

MODE = "container-images"
REPORTING_SUBDIR = "container_images"


def run_container_images(*, repository_root, log_handle, deadline, progress):
    """Bootstrap Jinja, then build and SQL-test one immutable image request."""
    root = Path(repository_root)
    request = Path(os.environ["BIOSEQ_CONTAINER_REQUEST"]).resolve(strict=True)
    if not request.is_relative_to((root / "reporting/container_images/requests").resolve()):
        raise ValueError("Container request is outside the owned request directory")
    environment = dict(os.environ, BIOSEQ_CONTAINER_MCP_WORKER="1")
    venv = root / "build/container-images/venv"
    def run(args):
        return _run(args, cwd=root, environment=environment, log=log_handle, deadline=deadline)
    progress("preparing Jinja container renderer; no C++ compilation")
    if not (venv / "bin/python").exists():
        run([sys.executable, "-m", "venv", str(venv)])
    run([str(venv / "bin/python"), "-m", "pip", "install", "Jinja2==3.1.6", "PyYAML==6.0.2"])
    run([str(venv / "bin/python"), "-m", "unittest", "discover", "-s",
         str(root / "infra/container_images/tests")])
    progress("building runtime image and checking installed SQL through psql")
    run([str(venv / "bin/python"), str(root / "infra/build_container_images.py"),
         "worker", "--request", str(request)])
    return {"receipt_path": str(request.with_suffix(".result.json")), "compiled": False}
