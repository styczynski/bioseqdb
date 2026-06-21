# ===========================================================================
# bioseq AWS benchmark reference machine
#
# A single disposable x86_64 Ubuntu box that mirrors the 64 GB reference dev box
# so benchmark numbers are comparable run-to-run. It installs the full toolchain
# (clang/libc++ 22, CMake 4.2.1, Ninja, PostgreSQL 17, Vulkan via vcpkg), clones
# the repo using credentials pulled from Secrets Manager by the instance role,
# and (optionally) configures + starts a build to prove the box is wired up.
# ===========================================================================

# --- AMI: latest Canonical Ubuntu 24.04 LTS x86_64 (gp3) unless overridden ---
data "aws_ssm_parameter" "ubuntu" {
  count = var.ami_id == "" ? 1 : 0
  name  = "/aws/service/canonical/ubuntu/server/24.04/stable/current/amd64/hvm/ebs-gp3/ami-id"
}

locals {
  ami_id = var.ami_id != "" ? var.ami_id : data.aws_ssm_parameter.ubuntu[0].value
}

# --- Networking: default VPC + a subnet in an AZ that offers the type --------
data "aws_vpc" "default" {
  default = true
}

# Not every AZ offers m4.4xlarge; restrict the subnet choice to AZs that do so
# the launch never fails with "Unsupported" in a stray AZ (e.g. us-east-1e).
data "aws_ec2_instance_type_offerings" "by_az" {
  location_type = "availability-zone"
  filter {
    name   = "instance-type"
    values = [var.instance_type]
  }
}

data "aws_subnets" "default" {
  filter {
    name   = "vpc-id"
    values = [data.aws_vpc.default.id]
  }
  filter {
    name   = "availability-zone"
    values = data.aws_ec2_instance_type_offerings.by_az.locations
  }
}

# --- SSH key pair (generated locally, private key written to .secrets/) -------
resource "tls_private_key" "ssh" {
  algorithm = "ED25519"
}

resource "local_sensitive_file" "ssh_private_key" {
  content         = tls_private_key.ssh.private_key_openssh
  filename        = "${path.module}/.secrets/${var.key_name}.pem"
  file_permission = "0600"
}

resource "aws_key_pair" "ssh" {
  key_name   = var.key_name
  public_key = tls_private_key.ssh.public_key_openssh
}

# --- IAM: instance role allowed to read ONLY the GitHub secret (+ SSM) --------
data "aws_iam_policy_document" "assume" {
  statement {
    actions = ["sts:AssumeRole"]
    principals {
      type        = "Service"
      identifiers = ["ec2.amazonaws.com"]
    }
  }
}

resource "aws_iam_role" "bench" {
  name               = "${var.name}-role"
  assume_role_policy = data.aws_iam_policy_document.assume.json
}

data "aws_iam_policy_document" "secret_read" {
  statement {
    sid       = "ReadGitHubSecret"
    actions   = ["secretsmanager:GetSecretValue", "secretsmanager:DescribeSecret"]
    resources = [var.secret_arn, "${var.secret_arn}*"]
  }
}

resource "aws_iam_role_policy" "secret_read" {
  name   = "${var.name}-secret-read"
  role   = aws_iam_role.bench.id
  policy = data.aws_iam_policy_document.secret_read.json
}

# SSM Session Manager as a keyless fallback way in (handy if the SSH source IP
# ever changes mid-session).
resource "aws_iam_role_policy_attachment" "ssm" {
  role       = aws_iam_role.bench.name
  policy_arn = "arn:aws:iam::aws:policy/AmazonSSMManagedInstanceCore"
}

resource "aws_iam_instance_profile" "bench" {
  name = "${var.name}-profile"
  role = aws_iam_role.bench.name
}

# --- Security group: key-only SSH in, everything out -------------------------
resource "aws_security_group" "bench" {
  name        = "${var.name}-sg"
  description = "bioseq benchmark box: SSH in, all egress (apt/llvm/vcpkg/github)"
  vpc_id      = data.aws_vpc.default.id

  ingress {
    description = "SSH"
    from_port   = 22
    to_port     = 22
    protocol    = "tcp"
    cidr_blocks = [var.ssh_ingress_cidr]
  }

  egress {
    description = "All outbound"
    from_port   = 0
    to_port     = 0
    protocol    = "-1"
    cidr_blocks = ["0.0.0.0/0"]
  }

  tags = { Name = "${var.name}-sg" }
}

# --- The instance ------------------------------------------------------------
resource "aws_instance" "bench" {
  ami                         = local.ami_id
  instance_type               = var.instance_type
  subnet_id                   = data.aws_subnets.default.ids[0]
  key_name                    = aws_key_pair.ssh.key_name
  vpc_security_group_ids      = [aws_security_group.bench.id]
  iam_instance_profile        = aws_iam_instance_profile.bench.name
  associate_public_ip_address = true

  root_block_device {
    volume_size           = var.root_volume_size_gb
    volume_type           = var.root_volume_type
    delete_on_termination = true
  }

  # Require IMDSv2 (the on-box awscli uses it to assume the instance role).
  metadata_options {
    http_endpoint = "enabled"
    http_tokens   = "required"
  }

  user_data = templatefile("${path.module}/scripts/user_data.sh.tftpl", {
    repo           = var.repo
    branch         = var.branch
    secret_name    = var.secret_name
    secret_region  = var.secret_region
    secret_arn     = var.secret_arn
    run_verify     = var.run_verify ? "1" : "0"
    fetch_repo_b64 = base64encode(file("${path.module}/scripts/fetch_repo.sh"))
  })

  # A change to the bootstrap inputs means a different machine — recreate it so
  # the new user-data actually runs (user-data only executes on first boot).
  user_data_replace_on_change = true

  tags = { Name = var.name }
}
