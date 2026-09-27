data "aws_caller_identity" "current" {}

data "aws_vpc" "default" {
  default = true
}

locals {
  builders = {
    arm64 = {
      aws_architecture = "arm64"
      instance_type    = "c7g.xlarge"
      ami_override     = var.arm64_ami_id
      base_ami_parameter = (
        "/aws/service/canonical/ubuntu/server/24.04/stable/current/arm64/hvm/ebs-gp3/ami-id"
      )
    }
    x64 = {
      aws_architecture = "x86_64"
      instance_type    = "m4.xlarge"
      ami_override     = var.x64_ami_id
      base_ami_parameter = (
        "/aws/service/canonical/ubuntu/server/24.04/stable/current/amd64/hvm/ebs-gp3/ami-id"
      )
    }
  }

  artifact_bucket_name = var.artifact_bucket_name != "" ? var.artifact_bucket_name : (
    "${var.name_prefix}-${data.aws_caller_identity.current.account_id}-${var.region}"
  )
}

data "aws_ssm_parameter" "ubuntu_base" {
  for_each = {
    for name, builder in local.builders : name => builder
    if builder.ami_override == ""
  }

  name = each.value.base_ami_parameter
}

locals {
  selected_ami_ids = {
    for name, builder in local.builders : name => (
      builder.ami_override != "" ? builder.ami_override : data.aws_ssm_parameter.ubuntu_base[name].value
    )
  }
}

# Each legacy/Graviton instance family may be absent from some availability
# zones. Resolve a compatible subnet independently for each architecture.
data "aws_ec2_instance_type_offerings" "builder" {
  for_each = local.builders

  location_type = "availability-zone"
  filter {
    name   = "instance-type"
    values = [each.value.instance_type]
  }
}

data "aws_subnets" "builder" {
  for_each = local.builders

  filter {
    name   = "vpc-id"
    values = [data.aws_vpc.default.id]
  }

  filter {
    name   = "availability-zone"
    values = data.aws_ec2_instance_type_offerings.builder[each.key].locations
  }
}

# ---------------------------------------------------------------------------
# Artifact repository: private, encrypted, versioned, and protected from an
# accidental `tofu destroy`. Python-native publishers use the EC2 instance role.
# ---------------------------------------------------------------------------

resource "aws_s3_bucket" "artifacts" {
  bucket        = local.artifact_bucket_name
  force_destroy = false

  lifecycle {
    prevent_destroy = true
  }

  tags = {
    Name      = local.artifact_bucket_name
    Component = "package-artifacts"
    Purpose   = "bioseq native package repositories"
  }
}

resource "aws_s3_bucket_ownership_controls" "artifacts" {
  bucket = aws_s3_bucket.artifacts.id

  rule {
    object_ownership = "BucketOwnerEnforced"
  }
}

resource "aws_s3_bucket_public_access_block" "artifacts" {
  bucket = aws_s3_bucket.artifacts.id

  block_public_acls       = true
  block_public_policy     = true
  ignore_public_acls      = true
  restrict_public_buckets = true
}

resource "aws_s3_bucket_server_side_encryption_configuration" "artifacts" {
  bucket = aws_s3_bucket.artifacts.id

  rule {
    apply_server_side_encryption_by_default {
      sse_algorithm = "AES256"
    }
    bucket_key_enabled = true
  }
}

resource "aws_s3_bucket_versioning" "artifacts" {
  bucket = aws_s3_bucket.artifacts.id

  versioning_configuration {
    status = "Enabled"
  }
}

resource "aws_s3_bucket_lifecycle_configuration" "artifacts" {
  bucket = aws_s3_bucket.artifacts.id

  rule {
    id     = "abort-incomplete-multipart-uploads"
    status = "Enabled"

    filter {}

    abort_incomplete_multipart_upload {
      days_after_initiation = 7
    }
  }

  depends_on = [aws_s3_bucket_versioning.artifacts]
}

data "aws_iam_policy_document" "artifact_transport" {
  statement {
    sid    = "DenyInsecureTransport"
    effect = "Deny"

    principals {
      type        = "*"
      identifiers = ["*"]
    }

    actions = ["s3:*"]
    resources = [
      aws_s3_bucket.artifacts.arn,
      "${aws_s3_bucket.artifacts.arn}/*",
    ]

    condition {
      test     = "Bool"
      variable = "aws:SecureTransport"
      values   = ["false"]
    }
  }
}

resource "aws_s3_bucket_policy" "artifact_transport" {
  bucket = aws_s3_bucket.artifacts.id
  policy = data.aws_iam_policy_document.artifact_transport.json
}

# OpenTofu owns only the durable secret container. The package entrypoint
# creates the first secret version at runtime, so signing-key material never
# appears in configuration, plans, state, or user-data. Keep the historical
# Terraform resource address and AWS name: renaming either would risk replacing
# the protected secret that already contains the production signing identity.
resource "aws_secretsmanager_secret" "omnipackage_signing_key" {
  name                    = "${var.name_prefix}-omnipackage-signing-key"
  description             = "Persistent GPG private signing key used by bioseq Python-native package publishers"
  recovery_window_in_days = 30

  lifecycle {
    prevent_destroy = true
  }

  tags = {
    Name      = "${var.name_prefix}-omnipackage-signing-key"
    Component = "package-signing"
    Purpose   = "Python-native package repository GPG private key"
  }
}

# ---------------------------------------------------------------------------
# Shared builder identity: SSM fallback plus the minimum S3 repository actions
# required by the native publisher. No static AWS keys are created or stored in
# state.
# ---------------------------------------------------------------------------

data "aws_iam_policy_document" "builder_assume_role" {
  statement {
    actions = ["sts:AssumeRole"]

    principals {
      type        = "Service"
      identifiers = ["ec2.amazonaws.com"]
    }
  }
}

resource "aws_iam_role" "builder" {
  name               = "${var.name_prefix}-role"
  assume_role_policy = data.aws_iam_policy_document.builder_assume_role.json

  tags = {
    Name      = "${var.name_prefix}-role"
    Component = "package-build-machine"
  }
}

data "aws_iam_policy_document" "artifact_publish" {
  statement {
    sid = "ReadArtifactBucketLocation"
    actions = [
      "s3:GetBucketLocation",
      # The publisher performs HeadBucket before any prefix-scoped operation;
      # AWS authorizes HeadBucket through s3:ListBucket without prefix context.
      "s3:ListBucket",
      "s3:ListBucketMultipartUploads",
    ]
    resources = [aws_s3_bucket.artifacts.arn]
  }

  statement {
    sid = "PublishArtifacts"
    actions = [
      "s3:AbortMultipartUpload",
      "s3:DeleteObject",
      "s3:GetObject",
      "s3:GetObjectVersion",
      "s3:ListMultipartUploadParts",
      "s3:PutObject",
    ]
    resources = ["${aws_s3_bucket.artifacts.arn}/${var.artifact_prefix}/*"]
  }
}

resource "aws_iam_role_policy" "artifact_publish" {
  name   = "${var.name_prefix}-artifact-publish"
  role   = aws_iam_role.builder.id
  policy = data.aws_iam_policy_document.artifact_publish.json
}

data "aws_iam_policy_document" "signing_key_access" {
  statement {
    sid = "ManagePersistentSigningKeyValue"
    actions = [
      "secretsmanager:DescribeSecret",
      "secretsmanager:GetSecretValue",
      "secretsmanager:PutSecretValue",
    ]
    resources = [aws_secretsmanager_secret.omnipackage_signing_key.arn]
  }
}

resource "aws_iam_role_policy" "signing_key_access" {
  name   = "${var.name_prefix}-signing-key-access"
  role   = aws_iam_role.builder.id
  policy = data.aws_iam_policy_document.signing_key_access.json
}

resource "aws_iam_role_policy_attachment" "ssm" {
  role       = aws_iam_role.builder.name
  policy_arn = "arn:aws:iam::aws:policy/AmazonSSMManagedInstanceCore"
}

resource "aws_iam_instance_profile" "builder" {
  name = "${var.name_prefix}-profile"
  role = aws_iam_role.builder.name
}

# ---------------------------------------------------------------------------
# SSH and builder instances. This stack has names/tags/state separate from the
# benchmark/development stack; lifecycle operations must use these exact ids.
# ---------------------------------------------------------------------------

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

  tags = {
    Name      = var.key_name
    Component = "package-build-machine"
  }
}

resource "aws_security_group" "builder" {
  name                   = "${var.name_prefix}-sg"
  description            = "bioseq package builders: fail-closed SSH ingress and unrestricted build egress"
  vpc_id                 = data.aws_vpc.default.id
  revoke_rules_on_delete = true

  ingress {
    description = "SSH from the explicitly supplied operator IPv4 address"
    from_port   = 22
    to_port     = 22
    protocol    = "tcp"
    cidr_blocks = [var.ssh_ingress_cidr]
  }

  egress {
    description = "Package mirrors, source registries, GitHub, and AWS APIs"
    from_port   = 0
    to_port     = 0
    protocol    = "-1"
    cidr_blocks = ["0.0.0.0/0"]
  }

  tags = {
    Name      = "${var.name_prefix}-sg"
    Component = "package-build-machine"
  }
}

locals {
  builder_user_data = {
    for name, builder in local.builders : name => templatefile("${path.module}/scripts/user_data.sh.tftpl", {
      artifact_bucket_name   = aws_s3_bucket.artifacts.bucket
      artifact_prefix        = var.artifact_prefix
      aws_architecture       = builder.aws_architecture
      bootstrap_gzip_b64     = base64gzip(file("${path.module}/scripts/bootstrap.sh"))
      builder_architecture   = name
      compiler_rt_priority   = var.compiler_rt_priority
      disk_swap_priority     = 10
      disk_swap_size_gb      = var.disk_swap_size_gb
      region                 = var.region
      schema_revision        = var.ami_schema_revision
      signing_key_secret_arn = aws_secretsmanager_secret.omnipackage_signing_key.arn
      ssh_rt_priority        = 2
      target_user            = var.target_user
      zram_fraction_percent  = var.zram_fraction_percent
      zram_swap_priority     = 100
    })
  }
}

resource "aws_instance" "builder" {
  for_each = local.builders

  ami                                  = local.selected_ami_ids[each.key]
  instance_type                        = each.value.instance_type
  subnet_id                            = sort(tolist(data.aws_subnets.builder[each.key].ids))[0]
  key_name                             = aws_key_pair.ssh.key_name
  vpc_security_group_ids               = [aws_security_group.builder.id]
  iam_instance_profile                 = aws_iam_instance_profile.builder.name
  associate_public_ip_address          = true
  disable_api_stop                     = true
  disable_api_termination              = true
  instance_initiated_shutdown_behavior = "stop"
  ebs_optimized                        = true

  root_block_device {
    volume_size           = var.root_volume_size_gb
    volume_type           = "gp3"
    iops                  = 6000
    throughput            = 500
    encrypted             = true
    delete_on_termination = false
  }

  metadata_options {
    http_endpoint               = "enabled"
    http_tokens                 = "required"
    http_put_response_hop_limit = 2
    instance_metadata_tags      = "enabled"
  }

  user_data                   = local.builder_user_data[each.key]
  user_data_replace_on_change = false

  volume_tags = {
    Name                = "${var.name_prefix}-${each.key}-root"
    Component           = "package-build-machine"
    BuildArchitecture   = each.key
    Architecture        = each.value.aws_architecture
    AmiSchemaRevision   = var.ami_schema_revision
    ProtectedFromDelete = "true"
  }

  tags = {
    Name                = "${var.name_prefix}-${each.key}"
    Component           = "package-build-machine"
    BuildArchitecture   = each.key
    Architecture        = each.value.aws_architecture
    AmiSchemaRevision   = var.ami_schema_revision
    ProtectedFromDelete = "true"
    Lifecycle           = "persistent-build-cache"
  }

  lifecycle {
    prevent_destroy = true
    # Cache AMIs apply only at first creation. Running persistent builders are
    # reconciled in place over SSH; AWS reports public-IP association and
    # user-data drift in ways that otherwise propose replacing these
    # prevent_destroy instances.
    ignore_changes = [
      ami,
      associate_public_ip_address,
      user_data,
    ]
  }

  depends_on = [
    aws_iam_role_policy.artifact_publish,
    aws_iam_role_policy.signing_key_access,
    aws_iam_role_policy_attachment.ssm,
    aws_s3_bucket_policy.artifact_transport,
  ]
}
