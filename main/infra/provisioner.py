#!/usr/bin/env python3
"""
provisioner.py — end-to-end driver for the bioseq AWS benchmark reference machine.

This is the single entry point an engineer should use to spin up, inspect, and
tear down the disposable x86_64 Ubuntu box that mirrors the 64 GB reference dev
box (so benchmark numbers are comparable run-to-run). It wraps OpenTofu
(`tofu`, falling back to `terraform`) over the HCL in ./testing_infra and adds
the conveniences and guardrails you actually want day to day.

WHAT IT PROVISIONS
  * one EC2 instance (default m4.4xlarge = 16 vCPU / 64 GiB, x86_64) on a roomy
    gp3 root volume, in a region where that type is offered (default us-east-1);
  * an IAM instance role scoped to read ONLY the GitHub secret in Secrets
    Manager (cross-region in eu-north-1) — the secret VALUE never lands in any
    file or in Terraform state;
  * a generated SSH key pair (private key saved under
    testing_infra/.secrets/, git-ignored);
  * user-data that installs the full toolchain (clang/libc++ 22, CMake 4.2.1,
    Ninja, PostgreSQL 17, Vulkan via vcpkg), clones the repo, and verifies the
    build configures and fails ONLY on our own (::NEON) code.

QUICK START
  # Provision against the branch that carries the AWS toolchain support and wait
  # for the build verification to finish:
  ./provisioner.py up --branch research/aws-machine --wait

  # Watch the live provisioning log / check the recorded status:
  ./provisioner.py logs
  ./provisioner.py status

  # SSH in to debug the compile:
  ./provisioner.py ssh

  # Re-run just the build verification on the running box:
  ./provisioner.py verify

  # Tear it down when done (leave nothing running):
  ./provisioner.py down

COMMANDS
  up        init + apply (create/update the instance). --wait to block until the
            on-box provisioning reaches a terminal status.
  down      destroy everything this stack manages.
  status    show Terraform outputs, the live EC2 state, a count of running
            benchmark instances (a cost guardrail), and the on-box
            PROVISION_STATUS marker.
  ssh       open an interactive SSH session on the box.
  logs      tail the provisioning log (/var/log/bioseq-provision.log).
  verify    re-run verify.sh on the box (configure + build, classify failures).
  wait      block until the on-box provisioning reaches a terminal status.
  output    print Terraform outputs as JSON.

COST / SAFETY
  * m4.4xlarge is an on-demand box and costs real money — run `down` when done.
  * `up` refuses (without --yes) if benchmark instances are already running, so
    you don't accidentally stack several boxes.
  * `down` destroys against the region the stack was applied in (recorded by `up`)
    and then sweeps any benchmark instance still tagged-and-running in that region,
    so a region mismatch can never leave a paid box orphaned.

This script uses only the Python standard library.
"""

from __future__ import annotations

import argparse
import json
import os
import shutil
import subprocess
import sys
import time
import urllib.request
from pathlib import Path

HERE = Path(__file__).resolve().parent
TF_DIR = HERE / "testing_infra"
# Sidecar written by `up` and read by `down`: records the region the stack was
# applied in so the destroy pins the AWS provider to it. The provider's region is
# var.region (NOT remembered in state across runs), so a region-less destroy would
# target the default region and silently orphan everything that lives elsewhere.
APPLY_STATE_FILE = TF_DIR / ".last-apply.json"

# Terminal on-box statuses written to /opt/bioseq/PROVISION_STATUS.
TERMINAL_STATUSES = {
    "TOOLCHAIN_READY",
    "VERIFY_OK_FAILS_ON_OUR_CODE",
    "VERIFY_BUILD_SUCCEEDED",
    "VERIFY_3RD_PARTY_FAILURE",
    "VERIFY_CONFIGURE_FAILED",
    "VERIFY_UNCLASSIFIED_FAILURE",
    "VERIFY_SCRIPT_MISSING",
    "VERIFY_NO_REPO",
}
GOOD_STATUSES = {"TOOLCHAIN_READY", "VERIFY_OK_FAILS_ON_OUR_CODE", "VERIFY_BUILD_SUCCEEDED"}


# --------------------------------------------------------------------------- #
# small helpers
# --------------------------------------------------------------------------- #
def c(text: str, color: str) -> str:
    codes = {"red": 31, "green": 32, "yellow": 33, "blue": 34, "bold": 1, "dim": 2}
    if not sys.stdout.isatty():
        return text
    return f"\033[{codes[color]}m{text}\033[0m"


def die(msg: str, code: int = 1):
    print(c(f"error: {msg}", "red"), file=sys.stderr)
    sys.exit(code)


def tofu_bin() -> str:
    for cand in ("tofu", "terraform"):
        if shutil.which(cand):
            return cand
    die("neither `tofu` nor `terraform` found on PATH")


def run(cmd: list[str], cwd: Path | None = None, capture: bool = False, check: bool = True,
        env: dict | None = None) -> subprocess.CompletedProcess:
    printable = " ".join(cmd)
    print(c(f"$ {printable}", "dim"))
    return subprocess.run(
        cmd, cwd=str(cwd) if cwd else None, text=True, check=check,
        capture_output=capture, env=env,
    )


def detect_public_ip() -> str | None:
    for url in ("https://checkip.amazonaws.com", "https://api.ipify.org"):
        try:
            with urllib.request.urlopen(url, timeout=6) as r:
                ip = r.read().decode().strip()
                if ip:
                    return ip
        except Exception:
            continue
    return None


def tofu_outputs() -> dict:
    bin_ = tofu_bin()
    res = run([bin_, "output", "-json"], cwd=TF_DIR, capture=True, check=False)
    if res.returncode != 0 or not res.stdout.strip():
        return {}
    try:
        raw = json.loads(res.stdout)
        outs = {k: v.get("value") for k, v in raw.items()}
        return refresh_live_network_outputs(outs)
    except json.JSONDecodeError:
        return {}


def refresh_live_network_outputs(outs: dict) -> dict:
    """Refresh network fields that change after EC2 stop/start.

    Terraform outputs keep the public IP/DNS captured at apply time. A stopped
    and started instance keeps its instance id and volume but gets a new public
    IP, so SSH helpers must prefer the live EC2 value.
    """
    instance_id = outs.get("instance_id")
    region = (outs.get("region")
              or os.environ.get("AWS_REGION")
              or os.environ.get("AWS_DEFAULT_REGION")
              or "us-east-1")
    if not instance_id or not shutil.which("aws"):
        return outs

    res = subprocess.run(
        ["aws", "ec2", "describe-instances", "--region", region,
         "--instance-ids", str(instance_id),
         "--query", "Reservations[0].Instances[0].{state:State.Name,ip:PublicIpAddress,dns:PublicDnsName}",
         "--output", "json"],
        text=True, capture_output=True, check=False,
    )
    if res.returncode != 0 or not res.stdout.strip():
        return outs
    try:
        live = json.loads(res.stdout)
    except json.JSONDecodeError:
        return outs
    if not isinstance(live, dict) or live.get("state") != "running":
        return outs

    ip = live.get("ip")
    dns = live.get("dns")
    if ip:
        outs["public_ip"] = ip
        key = outs.get("ssh_key_path")
        if key:
            outs["ssh_command"] = (
                f"ssh -i {key} -o StrictHostKeyChecking=accept-new ubuntu@{ip}")
            outs["provision_log_hint"] = (
                f"ssh -i {key} ubuntu@{ip} 'tail -f /var/log/bioseq-provision.log'")
    if dns:
        outs["public_dns"] = dns
    return outs


def ensure_init():
    if not (TF_DIR / ".terraform").exists():
        run([tofu_bin(), "init", "-input=false"], cwd=TF_DIR)


def var_args(args, ssh_cidr: str | None) -> list[str]:
    """Translate CLI flags into -var arguments (omit None => use the tf default)."""
    out: list[str] = []
    mapping = {
        "region": getattr(args, "region", None),
        "instance_type": getattr(args, "instance_type", None),
        "repo": getattr(args, "repo", None),
        "branch": getattr(args, "branch", None),
        "root_volume_size_gb": getattr(args, "volume_size", None),
        "ami_id": getattr(args, "ami", None),
        "ssh_ingress_cidr": ssh_cidr,
    }
    if getattr(args, "no_verify", False):
        mapping["run_verify"] = "false"
    for k, v in mapping.items():
        if v is not None:
            out += ["-var", f"{k}={v}"]
    return out


def aws_region(args=None) -> str:
    outs = tofu_outputs()
    if outs.get("region"):
        return outs["region"]
    if args is not None and getattr(args, "region", None):
        return args.region
    return os.environ.get("AWS_REGION") or os.environ.get("AWS_DEFAULT_REGION") or "us-east-1"


def _save_apply_state(region: str) -> None:
    """Persist the applied region so `down` can destroy against it (see APPLY_STATE_FILE)."""
    try:
        APPLY_STATE_FILE.write_text(json.dumps({"region": region}, indent=2) + "\n")
    except OSError:
        pass


def _load_apply_state() -> dict:
    try:
        return json.loads(APPLY_STATE_FILE.read_text())
    except (OSError, json.JSONDecodeError):
        return {}


def bench_instances(region: str, states: str = "pending,running") -> list[dict]:
    if not shutil.which("aws"):
        return []
    res = run(
        ["aws", "ec2", "describe-instances", "--region", region,
         "--filters", "Name=tag:Project,Values=bioseq",
         "Name=tag:Component,Values=benchmark-machine",
         f"Name=instance-state-name,Values={states}",
         "--query", "Reservations[].Instances[].{id:InstanceId,ip:PublicIpAddress,state:State.Name}",
         "--output", "json"],
        capture=True, check=False,
    )
    if res.returncode != 0 or not res.stdout.strip():
        return []
    try:
        return json.loads(res.stdout)
    except json.JSONDecodeError:
        return []


def ssh_target() -> tuple[str, str]:
    outs = tofu_outputs()
    ip = outs.get("public_ip")
    key = outs.get("ssh_key_path")
    if not ip or not key:
        die("no instance found in Terraform outputs — run `up` first")
    # The output path is relative to the Terraform dir; resolve it to an absolute
    # path so ssh works regardless of the caller's current working directory.
    key_path = Path(key)
    if not key_path.is_absolute():
        key_path = (TF_DIR / key).resolve()
    if not key_path.exists():
        die(f"ssh key not found at {key_path} — was the stack created on this machine?")
    return ip, str(key_path)


def ssh_base(key: str) -> list[str]:
    # The reference builder can spend ~50 minutes in a single heavily templated
    # clang++ translation unit with CPU saturated. Keep SSH alive through that
    # compile and let the MCP/build hard timeout, not SSH liveness, decide when a
    # clean build has really exceeded the 90-minute budget.
    return ["ssh", "-i", key, "-o", "StrictHostKeyChecking=accept-new",
            "-o", "UserKnownHostsFile=/dev/null", "-o", "LogLevel=ERROR",
            "-o", "ConnectTimeout=90",
            "-o", "ServerAliveInterval=60", "-o", "ServerAliveCountMax=90"]


def remote_status() -> str | None:
    ip, key = ssh_target()
    res = run(ssh_base(key) + [f"ubuntu@{ip}", "cat /opt/bioseq/PROVISION_STATUS 2>/dev/null"],
              capture=True, check=False)
    out = (res.stdout or "").strip()
    return out or None


# --------------------------------------------------------------------------- #
# commands
# --------------------------------------------------------------------------- #
def cmd_up(args):
    region = args.region or "us-east-1"
    running = bench_instances(region)
    if running and not args.yes:
        print(c(f"warning: {len(running)} benchmark instance(s) already running in {region}:", "yellow"))
        for i in running:
            print(f"   {i.get('id')}  {i.get('ip')}  {i.get('state')}")
        print("Re-run with --yes to apply anyway (Terraform updates the one it manages;\n"
              "use `down` first if you want a clean machine).")
        die("aborted by guardrail")

    ssh_cidr = args.ssh_cidr
    if ssh_cidr is None:
        ip = detect_public_ip()
        ssh_cidr = f"{ip}/32" if ip else "0.0.0.0/0"
        print(c(f"SSH ingress locked to {ssh_cidr}", "blue"))

    ensure_init()
    run([tofu_bin(), "apply", "-input=false", "-auto-approve"] + var_args(args, ssh_cidr), cwd=TF_DIR)

    outs = tofu_outputs()
    # Remember the region we applied in so `down` can destroy against the same
    # provider (var.region is not remembered in state across runs).
    _save_apply_state(outs.get("region") or region)
    print()
    print(c("instance up:", "green"), outs.get("instance_id"), "@", outs.get("public_ip"))
    print(c("ssh:", "green"), outs.get("ssh_command"))
    print(c("logs:", "green"), f"./provisioner.py logs")
    if args.wait:
        wait_for_provision(args.timeout)


def _terminate_orphans(region: str):
    """Terminate any benchmark instance still alive in `region` after a destroy.

    Defends against a stack whose Terraform state already lost the instance to a
    previous mis-regioned `down`: Terraform cannot destroy what is no longer in
    its state, so we sweep by tag directly and `down` always leaves nothing
    running. (Any security-group / key-pair orphans are freed by the next
    region-correct `up`/`down`.)
    """
    leftover = bench_instances(region, "pending,running,stopping,stopped")
    ids = [i["id"] for i in leftover if i.get("id")]
    if not ids:
        return
    print(c(f"sweeping {len(ids)} orphaned benchmark instance(s) in {region}: "
            + ", ".join(ids), "yellow"))
    run(["aws", "ec2", "terminate-instances", "--region", region,
         "--instance-ids", *ids], capture=True, check=False)


def cmd_down(args):
    if not args.yes:
        ans = input("Destroy the benchmark instance and all its resources? [y/N] ").strip().lower()
        if ans not in ("y", "yes"):
            die("aborted")
    ensure_init()
    # Terraform's AWS provider region comes from var.region, which is NOT recorded
    # in state across runs. A bare `destroy` configures the provider for the
    # DEFAULT region, fails to find resources that live elsewhere, drops them from
    # state as "already gone", and silently orphans a paid instance. Recover the
    # region the stack was applied in (explicit flag > persisted apply-state > live
    # outputs/env) and pin the provider to it.
    region = (getattr(args, "region", None)
              or _load_apply_state().get("region")
              or aws_region(args))
    print(c(f"destroying stack in region {region}", "blue"))
    run([tofu_bin(), "destroy", "-input=false", "-auto-approve",
         "-var", f"region={region}"], cwd=TF_DIR)
    # Safety net for a stack already orphaned by a past region-less destroy.
    _terminate_orphans(region)
    try:
        APPLY_STATE_FILE.unlink()
    except OSError:
        pass
    print(c("destroyed.", "green"))


def cmd_status(args):
    outs = tofu_outputs()
    if not outs:
        print("no Terraform state / outputs — nothing provisioned.")
    else:
        print(c("Terraform outputs:", "bold"))
        for k in ("instance_id", "public_ip", "public_dns", "region", "ssh_key_path"):
            if outs.get(k):
                print(f"  {k:14} {outs[k]}")
    region = aws_region(args)
    running = bench_instances(region)
    print(c(f"\nRunning benchmark instances in {region}: {len(running)}", "bold"))
    for i in running:
        print(f"  {i.get('id')}  {i.get('ip')}  {i.get('state')}")
    if len(running) > 1:
        print(c("  WARNING: more than one running — leave only one and `down` the rest.", "yellow"))
    if outs.get("public_ip"):
        st = remote_status()
        col = "green" if st in GOOD_STATUSES else ("yellow" if st else "dim")
        print(c(f"\nOn-box PROVISION_STATUS: {st or '(unreachable / still booting)'}", col))


def cmd_ssh(args):
    ip, key = ssh_target()
    os.execvp("ssh", ssh_base(key) + [f"ubuntu@{ip}"] + (args.extra or []))


def cmd_logs(args):
    ip, key = ssh_target()
    follow = "-f" if args.follow else ""
    cmd = f"tail -n 200 {follow} /var/log/bioseq-provision.log"
    os.execvp("ssh", ssh_base(key) + ["-t", f"ubuntu@{ip}", cmd])


def cmd_verify(args):
    ip, key = ssh_target()
    remote = "bash -lc '~/bioseqdb/main/infra/testing_infra/scripts/verify.sh'"
    rc = run(ssh_base(key) + ["-t", f"ubuntu@{ip}", remote], check=False).returncode
    print(c(f"\nfinal status: {remote_status()}", "bold"))
    sys.exit(rc)


def cmd_output(args):
    print(json.dumps(tofu_outputs(), indent=2))


def wait_for_provision(timeout_min: int):
    print(c(f"waiting for on-box provisioning (timeout {timeout_min} min)...", "blue"))
    deadline = time.time() + timeout_min * 60
    last = None
    while time.time() < deadline:
        st = remote_status()
        if st and st != last:
            print(c(f"  status: {st}", "dim"))
            last = st
        if st in TERMINAL_STATUSES:
            col = "green" if st in GOOD_STATUSES else "yellow"
            print(c(f"provisioning finished: {st}", col))
            return
        time.sleep(30)
    print(c("timed out waiting for provisioning; check `./provisioner.py logs`", "yellow"))


def cmd_wait(args):
    wait_for_provision(args.timeout)


# --------------------------------------------------------------------------- #
# argument parsing
# --------------------------------------------------------------------------- #
def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(
        prog="provisioner.py",
        description="Provision/manage the bioseq AWS benchmark machine (wraps OpenTofu).",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="Run `./provisioner.py <command> -h` for command-specific options.",
    )
    sub = p.add_subparsers(dest="command", required=True)

    up = sub.add_parser("up", help="create/update the instance (init + apply)")
    up.add_argument("--instance-type", dest="instance_type", help="EC2 type (default m4.4xlarge)")
    up.add_argument("--region", default="us-east-1", help="AWS region (default us-east-1)")
    up.add_argument("--repo", help="owner/name to clone (default styczynski/bioseqdb)")
    up.add_argument("--branch", default="main",
                    help="branch to clone (default main; use research/aws-machine for the unmerged toolchain support)")
    up.add_argument("--volume-size", dest="volume_size", type=int, help="root volume GiB (default 150)")
    up.add_argument("--ami", help="override AMI id (default: latest Ubuntu 24.04 x86_64)")
    up.add_argument("--ssh-cidr", dest="ssh_cidr", help="CIDR allowed to SSH (default: your detected /32)")
    up.add_argument("--no-verify", action="store_true", help="skip the on-box build verification")
    up.add_argument("--wait", action="store_true", help="block until on-box provisioning finishes")
    up.add_argument("--timeout", type=int, default=120, help="--wait timeout in minutes (default 120)")
    up.add_argument("--yes", action="store_true", help="skip the running-instance guardrail prompt")
    up.set_defaults(func=cmd_up)

    down = sub.add_parser("down", help="destroy the instance and all resources")
    down.add_argument("--yes", action="store_true", help="do not prompt for confirmation")
    down.add_argument("--region",
                      help="region the stack was applied in (default: persisted from `up`, else outputs/env)")
    down.set_defaults(func=cmd_down)

    st = sub.add_parser("status", help="show outputs, live state, and on-box status")
    st.add_argument("--region", help="region to query (default: from outputs / env)")
    st.set_defaults(func=cmd_status)

    sh = sub.add_parser("ssh", help="open an interactive SSH session")
    sh.add_argument("extra", nargs=argparse.REMAINDER, help="extra args/command for ssh")
    sh.set_defaults(func=cmd_ssh)

    lg = sub.add_parser("logs", help="tail the provisioning log")
    lg.add_argument("--follow", "-f", action="store_true", help="follow (tail -f)")
    lg.set_defaults(func=cmd_logs)

    vf = sub.add_parser("verify", help="re-run verify.sh on the box")
    vf.set_defaults(func=cmd_verify)

    wt = sub.add_parser("wait", help="block until on-box provisioning finishes")
    wt.add_argument("--timeout", type=int, default=120, help="timeout in minutes (default 120)")
    wt.set_defaults(func=cmd_wait)

    op = sub.add_parser("output", help="print Terraform outputs as JSON")
    op.set_defaults(func=cmd_output)

    return p


def main():
    if not TF_DIR.exists():
        die(f"expected Terraform dir not found: {TF_DIR}")
    args = build_parser().parse_args()
    args.func(args)


if __name__ == "__main__":
    main()
