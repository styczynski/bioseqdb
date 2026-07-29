output "builders" {
  description = "Package-builder connection and identity data keyed by arm64/x64."
  value = {
    for name, builder in local.builders : name => {
      instance_id   = aws_instance.builder[name].id
      public_ip     = aws_instance.builder[name].public_ip
      private_ip    = aws_instance.builder[name].private_ip
      instance_type = builder.instance_type
      architecture  = builder.aws_architecture
      # Canonical's public SSM parameter is provider-marked sensitive even
      # though an AMI id is public metadata. Remove only that inherited marker.
      source_ami_id = nonsensitive(aws_instance.builder[name].ami)
    }
  }
}

output "builder_user_data" {
  description = "Exact rendered bootstrap user data used to reconcile persistent running builders in place."
  value       = local.builder_user_data
  sensitive   = true
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
  description = "Private S3 bucket receiving Python-native package artifacts."
  value       = aws_s3_bucket.artifacts.bucket
}

output "package_bucket_arn" {
  description = "ARN of the private S3 artifact bucket."
  value       = aws_s3_bucket.artifacts.arn
}

output "signing_key_secret_arn" {
  description = "ARN of the persistent Secrets Manager container for the Python-native publisher GPG private key."
  value       = aws_secretsmanager_secret.omnipackage_signing_key.arn
}
