import click
import subprocess
import sys
import os
from threading import Thread

# "--shell-escape" "--enable-pipes" "--enable-write18"
# "-interaction" "nonstopmode" "-halt-on-error" "-file-line-error"

DEFAULT_ARGS = [
    "--shell-escape",
    "--enable-pipes",
    "--enable-write18",
    "-interaction", "nonstopmode",
    "-halt-on-error",
    "-file-line-error",
]

def eprint(*args, **kwargs):
    print(*args, file=sys.stderr, **kwargs)

_LOG_BUFFER = []
_RUN_THREADS = True

def logstream(stream):
    global _LOG_BUFFER, _RUN_THREADS
    while _RUN_THREADS:
        try:
            out = stream.readline()
        except ValueError:
            # Need to termiante the thread
            return
        if out:
            try:
                line = out.decode("utf-8").strip()
            except UnicodeDecodeError:
                line = ""
            for linet in line.split("\n"):
                _LOG_BUFFER.append(linet.strip())
            linel = line.lower()
            if "unrecognized" in linel:
                eprint(f"[!] Error: {line}")
            elif "error" in linel:
                eprint(f"[!] Error: {line}")
            elif "warning" in linel:
                eprint(f"[!] Warning: {line}")
            # print(f"-- {line}")
        else:
            break

@click.command()
@click.argument('latex-executable')
@click.option("--biber", is_flag=True, show_default=True, default=False, help="Are we running Biber")
@click.option("--draft", is_flag=True, show_default=True, default=False, help="Use draft mode")
@click.option("--times", show_default=True, default=1, help="How many times to rerun Latex")
@click.option("--timeout", show_default=True, default=180, help="Latex job timeout in seconds")
@click.argument('output-directory')
def latex_run(latex_executable, biber, draft, times, timeout, output_directory):
    """Simple program that greets NAME for a total of COUNT times."""
    global _LOG_BUFFER, _RUN_THREADS
    eprint(f"[i] Will rerun {times} times")
    for run_no in range(times):
        process_cwd = None
        process_log_file = None
        if biber:
            target_file = "main"
            process_cwd = output_directory
            cmd_args = []
            cmd_args.append(target_file)
            cmd = " ".join([latex_executable, *cmd_args])
        else:
            target_file = os.path.join(output_directory, "main.tex")
            process_log_file = os.path.join(output_directory, "main.log")
            cmd_args = [*DEFAULT_ARGS]
            if draft:
                cmd_args.append("-draftmode")
            cmd_args += ["-output-directory", output_directory]
            cmd_args.append(target_file)
            cmd = " ".join([latex_executable, *cmd_args])
        eprint(f"[i] Run '{cmd}'")
        
        process_env = os.environ.copy()
        if "TERM" not in process_env or (not process_env["TERM"]):
            process_env["TERM"] = "unknown"
        venv_bin = os.path.dirname(os.path.abspath(sys.executable))
        wrapper_bin = os.path.join(os.path.dirname(os.path.abspath(__file__)), "bin")
        process_env["PATH"] = (
            wrapper_bin + os.pathsep + venv_bin + os.pathsep + process_env.get("PATH", "")
        )
        if not biber:
            process_env["TEXMF_OUTPUT_DIRECTORY"] = output_directory

        if process_log_file and os.path.exists(process_log_file):
            try:
                os.remove(process_log_file)
                print(f"[i] Removed old log file '{process_log_file}'")
            except:
                pass

        p = subprocess.Popen(
            cmd,
            shell=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            stdin=subprocess.DEVNULL,
            cwd=process_cwd,
            env=process_env,
        )
        
        stdout_thread = Thread(target=logstream,
            args=(p.stdout,))

        stderr_thread = Thread(target=logstream,
            args=(p.stderr,))

        stdout_thread.start()
        stderr_thread.start()

        exit_code = None
        try:
            p.communicate(timeout=timeout)
            exit_code = p.returncode
        except subprocess.TimeoutExpired:
            eprint(f"[FAIL] Latex job timeout reached (timeout set to {timeout} sec.)")
            # Forced termination
            _RUN_THREADS = False
            p.kill()

        try:
            stdout_thread.join(1)
            stderr_thread.join(1)
        except:
            pass

        if exit_code == 0 and _RUN_THREADS:
            eprint(f"[...] Done processing {target_file} (run {run_no+1} / {times})")
            continue
        else:
            can_print_file_log = True
            eprint(f"[FAIL] Latex job has failed on {target_file} (exit code was '{exit_code}')")
            if process_log_file and os.path.exists(process_log_file):
                try:
                    eprint(f"[DUMP] Full file log from '{process_log_file}'")
                    with open(process_log_file, 'r') as log_file:
                        for raw_line in log_file:
                            line = raw_line.strip()
                            eprint(f"    {line}")
                    eprint(f"[DUMP] End of full file log from '{process_log_file}'")
                except:
                    can_print_file_log = False
            else:
                can_print_file_log = False
            if not can_print_file_log:
                eprint("[DUMP] Full stdout log:")
                for line in _LOG_BUFFER:
                    eprint(f"    {line}")
                eprint("[DUMP] End of stdout log.")
            os._exit(22)
    
    eprint(f"[OK] Done processing {target_file}")
    os._exit(0)

if __name__ == '__main__':
    latex_run()