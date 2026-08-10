# OpenTofu / provider pinning for the bioseq AWS benchmark reference machine.
#
# Run everything through ../provisioner.py rather than calling `tofu` directly
# (the wrapper injects the right -var values, detects your public IP for the SSH
# rule, and adds cost/teardown guardrails). Plain `tofu` still works if you set
# the variables yourself.
terraform {
  required_version = ">= 1.6.0"

  required_providers {
    aws = {
      source  = "hashicorp/aws"
      version = "~> 5.60"
    }
    tls = {
      source  = "hashicorp/tls"
      version = "~> 4.0"
    }
    local = {
      source  = "hashicorp/local"
      version = "~> 2.5"
    }
  }
}

provider "aws" {
  region = var.region

  # The benchmark machine lives in `region` (default us-east-1, where m4.4xlarge
  # is offered). The GitHub secret lives in a *different* region (eu-north-1) and
  # is read at runtime by the instance role via an explicit --region flag, so the
  # provider region and the secret region are intentionally decoupled.
  default_tags {
    tags = {
      Project   = "bioseq"
      Component = "benchmark-machine"
      ManagedBy = "opentofu"
    }
  }
}
