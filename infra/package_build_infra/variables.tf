variable "region" {
  description = "AWS region containing both package builders and the artifact bucket."
  type        = string
  default     = "us-east-1"

  validation {
    condition     = can(regex("^[a-z0-9]+(?:-[a-z0-9]+)+-[0-9]+$", var.region))
    error_message = "region must be a valid AWS region identifier."
  }
}

variable "name_prefix" {
  description = "Lower-case prefix used for package-builder AWS resources."
  type        = string
  default     = "bioseq-package-builder"

  validation {
    condition     = can(regex("^[a-z0-9][a-z0-9-]{2,31}$", var.name_prefix))
    error_message = "name_prefix must be 3-32 lower-case letters, digits, or hyphens and start with a letter or digit."
  }
}

variable "ssh_ingress_cidr" {
  description = "Required fail-closed IPv4 /32 allowed to SSH to package builders. There is deliberately no public default."
  type        = string

  validation {
    condition = (
      can(cidrnetmask(var.ssh_ingress_cidr)) &&
      can(regex("/32$", var.ssh_ingress_cidr)) &&
      var.ssh_ingress_cidr != "0.0.0.0/0"
    )
    error_message = "ssh_ingress_cidr must be an explicit IPv4 /32; broad and fail-open CIDRs are rejected."
  }
}

variable "key_name" {
  description = "EC2 key-pair name. The generated private key is stored only under this stack's ignored .secrets directory."
  type        = string
  default     = "bioseq-package-builder"

  validation {
    condition     = can(regex("^[A-Za-z0-9._-]{3,64}$", var.key_name))
    error_message = "key_name must contain only letters, digits, dot, underscore, and hyphen."
  }
}

variable "artifact_bucket_name" {
  description = "Optional globally unique S3 bucket name. Empty derives one from name_prefix, account id, and region."
  type        = string
  default     = ""

  validation {
    condition = (
      var.artifact_bucket_name == "" ||
      can(regex("^[a-z0-9][a-z0-9.-]{1,61}[a-z0-9]$", var.artifact_bucket_name))
    )
    error_message = "artifact_bucket_name must be empty or a valid lower-case S3 bucket name."
  }
}

variable "artifact_prefix" {
  description = "Bucket prefix to which the builders' instance role is restricted."
  type        = string
  default     = "bioseq"

  validation {
    condition = (
      length(var.artifact_prefix) >= 1 &&
      length(var.artifact_prefix) <= 128 &&
      can(regex("^[A-Za-z0-9][A-Za-z0-9._/-]*$", var.artifact_prefix)) &&
      !endswith(var.artifact_prefix, "/") &&
      !strcontains(var.artifact_prefix, "//")
    )
    error_message = "artifact_prefix must be a non-empty relative S3 prefix without a trailing or doubled slash."
  }
}

variable "root_volume_size_gb" {
  description = "Encrypted gp3 root volume size for each builder. Current builders require at least 500 GiB."
  type        = number
  default     = 500

  validation {
    condition     = var.root_volume_size_gb >= 500 && floor(var.root_volume_size_gb) == var.root_volume_size_gb
    error_message = "root_volume_size_gb must be an integer of at least 500 GiB."
  }
}

variable "disk_swap_size_gb" {
  description = "Low-priority EBS-backed swapfile size on each builder."
  type        = number
  default     = 96

  validation {
    condition     = var.disk_swap_size_gb >= 32 && var.disk_swap_size_gb <= 256 && floor(var.disk_swap_size_gb) == var.disk_swap_size_gb
    error_message = "disk_swap_size_gb must be an integer between 32 and 256 GiB."
  }
}

variable "zram_fraction_percent" {
  description = "Percentage of physical RAM exposed as high-priority compressed zram swap."
  type        = number
  default     = 100

  validation {
    condition     = var.zram_fraction_percent >= 25 && var.zram_fraction_percent <= 200 && floor(var.zram_fraction_percent) == var.zram_fraction_percent
    error_message = "zram_fraction_percent must be an integer between 25 and 200."
  }
}

variable "target_user" {
  description = "Unprivileged Ubuntu AMI user that runs builds over SSH."
  type        = string
  default     = "ubuntu"

  validation {
    condition     = can(regex("^[a-z_][a-z0-9_-]{0,30}$", var.target_user))
    error_message = "target_user must be a conventional lower-case Linux username."
  }
}

variable "arm64_ami_id" {
  description = "Optional architecture-specific cached AMI for the c7g arm64 builder. Empty uses Canonical Ubuntu 24.04 arm64."
  type        = string
  default     = ""

  validation {
    condition     = var.arm64_ami_id == "" || can(regex("^ami-[0-9a-f]+$", var.arm64_ami_id))
    error_message = "arm64_ami_id must be empty or a valid AMI id."
  }
}

variable "x64_ami_id" {
  description = "Optional architecture-specific cached AMI for the m4 x86_64 builder. Empty uses Canonical Ubuntu 24.04 amd64."
  type        = string
  default     = ""

  validation {
    condition     = var.x64_ami_id == "" || can(regex("^ami-[0-9a-f]+$", var.x64_ami_id))
    error_message = "x64_ami_id must be empty or a valid AMI id."
  }
}

variable "ami_schema_revision" {
  description = "Bootstrap/cache schema tag. Increment when cached package-builder AMIs become incompatible."
  type        = number
  default     = 2

  validation {
    condition     = var.ami_schema_revision == 2
    error_message = "ami_schema_revision must be the current revision 2."
  }
}

variable "compiler_rt_priority" {
  description = "Lowest Linux SCHED_RR priority used only by the validated compiler launcher."
  type        = number
  default     = 1

  validation {
    condition     = var.compiler_rt_priority == 1
    error_message = "compiler_rt_priority is intentionally capped at exactly 1."
  }
}
