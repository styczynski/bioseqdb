#!/usr/bin/env python3
"""Isolated four-host external-aligner provisioning using the existing provisioner."""
import argparse
import json
from pathlib import Path
import shutil
import subprocess
import provisioner

ACCOUNT = "090072209895"
MACHINES = {"M7I": "m7i.xlarge", "M7A": "m7a.xlarge", "M4": "m4.xlarge", "C7G": "c7g.xlarge"}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("operation", choices=["up", "output", "status"])
    parser.add_argument("machine", choices=MACHINES)
    args = parser.parse_args()
    identity = json.loads(subprocess.check_output(["aws", "sts", "get-caller-identity"]))
    if identity["Account"] != ACCOUNT:
        raise SystemExit(f"Refusing account {identity['Account']}; expected {ACCOUNT}")
    root = Path(__file__).resolve().parent
    state = root / ".external_aligner_state" / args.machine.lower()
    state.mkdir(parents=True, exist_ok=True)
    # Keep the established EC2/network/key/SSM implementation, with private state
    # and a toolchain-only bootstrap. No baseline stack is read or modified.
    for source in (root / "testing_infra").glob("*.tf"):
        shutil.copyfile(source, state / source.name)
    shutil.copytree(root / "testing_infra" / "scripts", state / "scripts", dirs_exist_ok=True)
    architecture = "arm64" if args.machine == "C7G" else "amd64"
    main_tf = (state / "main.tf").read_text().replace("/amd64/", f"/{architecture}/")
    main_tf = main_tf.replace("volume_type           = var.root_volume_type",
        "volume_type           = var.root_volume_type\n    iops                  = 6000\n    throughput            = 500")
    (state / "main.tf").write_text(main_tf)
    shutil.copyfile(root / "external_aligner_bootstrap.sh", state / "scripts/user_data.sh.tftpl")
    name = "bioseq-e2e-external-" + args.machine.lower()
    (state / "external.auto.tfvars.json").write_text(json.dumps({
        "name": name, "key_name": name, "run_verify": False,
        "secret_arn": f"arn:aws:secretsmanager:us-east-1:{ACCOUNT}:secret:unused-external-aligners",
    }))
    provisioner.TF_DIR = state
    provisioner.APPLY_STATE_FILE = state / ".last-apply.json"
    if args.operation == "up":
        # Use the architecture-specific Canonical image explicitly: this new
        # account contains no cached images, and unrelated caches are ineligible.
        parameter = f"/aws/service/canonical/ubuntu/server/24.04/stable/current/{architecture}/hvm/ebs-gp3/ami-id"
        ami = json.loads(subprocess.check_output(["aws", "ssm", "get-parameter", "--region", "us-east-1", "--name", parameter]))["Parameter"]["Value"]
        up = provisioner.build_parser().parse_args(["up", "--instance-type", MACHINES[args.machine], "--region", "us-east-1", "--ami", ami, "--volume-size", "300", "--no-verify", "--yes"])
        provisioner.cmd_up(up)
    elif args.operation == "output":
        print(json.dumps(provisioner.tofu_outputs(), indent=2))
    else:
        provisioner.cmd_status(argparse.Namespace(region="us-east-1"))


if __name__ == "__main__":
    main()
