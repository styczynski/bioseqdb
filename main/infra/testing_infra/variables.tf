# ---------------------------------------------------------------------------
# Inputs for the bioseq AWS benchmark reference machine.
# Sensible defaults are baked in; override via ../provisioner.py flags, a
# terraform.tfvars file, or -var on the CLI.
# ---------------------------------------------------------------------------

variable "region" {
  description = "AWS region to launch the instance in. Default us-east-1 because m4.4xlarge is offered there (it is NOT offered in eu-north-1, where the GitHub secret lives — that secret is still read cross-region)."
  type        = string
  default     = "us-east-1"
}

variable "instance_type" {
  description = "EC2 instance type. Default m4.4xlarge (16 vCPU / 64 GiB) to mirror the 64 GB reference dev box used for benchmark parity."
  type        = string
  default     = "m4.4xlarge"
}

variable "root_volume_size_gb" {
  description = "Root EBS volume size (GiB). The build needs room for LLVM (~3 GB), the vcpkg buildtrees/downloads (~15-25 GB), the CMake build tree (~10 GB) plus the repo; 150 GiB leaves comfortable headroom for benchmark temp-file spill."
  type        = number
  default     = 150
}

variable "root_volume_type" {
  description = "Root EBS volume type. gp3 gives a good baseline of IOPS/throughput at low cost for a disposable box."
  type        = string
  default     = "gp3"
}

variable "repo" {
  description = "GitHub repository (owner/name) to clone onto the machine."
  type        = string
  default     = "styczynski/bioseqdb"
}

variable "branch" {
  description = "Branch to check out. Default 'main' as requested. NOTE: the AWS_MACHINE_X64 toolchain support lives on the feature branch until it is merged, so pass --branch research/aws-machine when provisioning against an unmerged tree."
  type        = string
  default     = "main"
}

variable "secret_arn" {
  description = "ARN of the Secrets Manager secret holding gh_ssh_private + gh_access_token. Only the ARN (an identifier) is referenced here; the secret VALUE is fetched at runtime by the instance role and never touches Terraform state or any file."
  type        = string
  default     = "arn:aws:secretsmanager:eu-north-1:778900739727:secret:github_secrets-DCYSun"
}

variable "secret_name" {
  description = "Secrets Manager secret id/name used by the on-box `aws secretsmanager get-secret-value` call."
  type        = string
  default     = "github_secrets"
}

variable "secret_region" {
  description = "Region the GitHub secret lives in (read cross-region from the instance)."
  type        = string
  default     = "eu-north-1"
}

variable "ssh_ingress_cidr" {
  description = "CIDR allowed to reach TCP/22. provisioner.py auto-detects your current public IP as a /32; widen only if you must. SSH is key-only regardless."
  type        = string
  default     = "0.0.0.0/0"
}

variable "run_verify" {
  description = "If true, the bootstrap runs verify.sh at the end (cmake configure + a build of bioseq_modules_nopg) and records the outcome in /opt/bioseq/PROVISION_STATUS."
  type        = bool
  default     = true
}

variable "ami_id" {
  description = "Override the AMI. Empty string => look up the latest Canonical Ubuntu 24.04 LTS x86_64 (gp3) AMI via SSM in `region`."
  type        = string
  default     = ""
}

variable "key_name" {
  description = "Name for the generated EC2 key pair (and the local .pem filename)."
  type        = string
  default     = "bioseq-bench-aws"
}

variable "name" {
  description = "Name tag for the instance and related resources."
  type        = string
  default     = "bioseq-bench-aws"
}
