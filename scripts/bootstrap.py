"""Lazy provisioning for the build MCP.

The serve path starts _serve() immediately so the MCP handshake never blocks and the
stdlib build tools always work. Doc tools call ensure_doc_env() on first use (idempotent):
npm install qmd if missing; make jsonschema importable by appending the poetry venv's
site-packages to sys.path (venv pinned to the running interpreter). If poetry is unavailable,
schemas.validate() uses its minimal fallback. No process re-exec."""
from __future__ import annotations

import glob
import os
import shutil
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
QMD_BIN = HERE / "node_modules" / ".bin" / "qmd"
_done = {"npm": False, "py": False}


def _log(m: str) -> None:
    print(f"[bioseq-build-mcp:bootstrap] {m}", file=sys.stderr, flush=True)


def ensure_npm_deps() -> None:
    if _done["npm"] or QMD_BIN.exists():
        _done["npm"] = True
        return
    npm = shutil.which("npm")
    if not npm:
        _log("npm not on PATH; qmd-backed doc tools unavailable")
        return
    _log("npm install (first run)...")
    r = subprocess.run([npm, "install", "--no-audit", "--no-fund"], cwd=HERE,
                       capture_output=True, text=True)
    _done["npm"] = (r.returncode == 0)
    if r.returncode:
        _log(f"npm install failed: {r.stderr[-400:]}")


def ensure_jsonschema() -> None:
    if _done["py"]:
        return
    try:
        import jsonschema  # noqa: F401
        _done["py"] = True
        return
    except Exception:
        pass
    poetry = shutil.which("poetry")
    if poetry:
        env = {**os.environ, "POETRY_VIRTUALENVS_IN_PROJECT": "true"}
        # Pin the venv to the interpreter running this server so compiled deps (rpds-py) match.
        subprocess.run([poetry, "env", "use", sys.executable], cwd=HERE,
                       capture_output=True, text=True, env=env)
        subprocess.run([poetry, "install", "--no-root"], cwd=HERE,
                       capture_output=True, text=True, env=env)
        info = subprocess.run([poetry, "env", "info", "-p"], cwd=HERE,
                              capture_output=True, text=True, env=env).stdout.strip()
        if info:
            tag = f"python{sys.version_info.major}.{sys.version_info.minor}"
            for sp in (glob.glob(str(Path(info) / "lib" / tag / "site-packages")) +
                       glob.glob(str(Path(info) / "lib" / "python*" / "site-packages"))):
                if tag in sp and sp not in sys.path:
                    sys.path.insert(0, sp)
    try:
        import jsonschema  # noqa: F401
        _done["py"] = True
    except Exception:
        _log("jsonschema unavailable; using minimal request validator")
        _done["py"] = True


def ensure_doc_env() -> None:
    ensure_npm_deps()
    ensure_jsonschema()
