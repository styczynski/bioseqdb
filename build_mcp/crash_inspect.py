"""Read-only PostgreSQL postmortem inspection through the builder MCP.

Only the standard Ubuntu PostgreSQL Apport report is admitted. Never attaches
to a live process, executes inferior code, or rebuilds/installs an artifact.
"""
import json
from pathlib import Path
import shutil
import subprocess
import tempfile


def inspect_postgres_crash():
    report = Path('/var/crash/_usr_lib_postgresql_17_bin_postgres.111.crash')
    if not report.is_file():
        return json.dumps({'error': 'No saved PostgreSQL Apport report'})
    if not shutil.which('gdb') or not shutil.which('apport-unpack'):
        return json.dumps({'error': 'Postmortem inspection requires gdb and apport-unpack'})
    output = Path(tempfile.mkdtemp(prefix='bioseq-postmortem-'))
    unpack = output / 'unpacked'
    unpack.mkdir()
    result = subprocess.run(['sudo', '-n', 'apport-unpack', str(report), str(unpack)],
                            capture_output=True, text=True, timeout=120)
    if result.returncode:
        return json.dumps({'error': result.stderr[-2000:], 'output': str(output)})
    # Files remain root-readable only; debug output contains no locals or env.
    command = ['sudo', '-n', 'gdb', '-nx', '-nh', '--batch',
               '-iex', 'set auto-load off', '-iex', 'set debuginfod enabled off',
               '-ex', 'set pagination off', '-ex', 'set print frame-arguments none',
               '/usr/lib/postgresql/17/bin/postgres', str(unpack / 'CoreDump'),
               '-ex', 'info sharedlibrary', '-ex', 'thread apply all bt 32',
               '-ex', 'info registers rip rsp rbp', '-ex', 'x/8i $pc']
    result = subprocess.run(command, capture_output=True, text=True, timeout=120)
    transcript = output / 'backtrace.txt'
    transcript.write_text(result.stdout + '\n' + result.stderr)
    metadata = {}
    for field in ('Date', 'Signal', 'ExecutablePath', 'ExecutableTimestamp'):
        value = subprocess.run(['sudo', '-n', 'head', '-c', '500', str(unpack / field)],
                               capture_output=True, text=True, timeout=5)
        metadata[field] = value.stdout.strip()
    return json.dumps({'report': str(report), 'report_metadata': metadata,
                       'transcript': str(transcript), 'exit_code': result.returncode,
                       'backtrace': result.stdout[-48000:], 'warnings': result.stderr[-4000:],
                       'scope': 'Saved historical core; not proof of the newest failure'}, indent=2)
