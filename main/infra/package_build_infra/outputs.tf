output "builders" {
  description = "Package-builder connection and identity data keyed by arm64/x64."
  value = {
    for name, builder in local.builders : name => {
      instance_id   = aws_instance.builder[name].id
      public_ip     = aws_instance.builder[name].public_ip
      private_ip    = aws_instance.builder[name].private_ip
      instance_type = builder.instance_type
      architecture  = builder.aws_architecture
      source_ami_id = aws_instance.builder[name].ami
    }
  }
}

output "ssh_private_key_path" {
  description = "Absolute path to the generated package-builder SSH private key."
  value       = abspath(local_sensitive_file.ssh_private_key.filename)
}

output "region" {
  description = "AWS region containing package-builder resources."
  value       = var.region
}

output "package_bucket_name" {
  description = "Private S3 bucket receiving omnipackage artifacts."
  value       = aws_s3_bucket.artifacts.bucket
}

output "package_bucket_arn" {
  description = "ARN of the private S3 artifact bucket."
  value       = aws_s3_bucket.artifacts.arn
}

output "signing_key_secret_arn" {
  description = "ARN of the empty persistent Secrets Manager container for the omnipackage GPG private key."
  value       = aws_secretsmanager_secret.omnipackage_signing_key.arn
}
