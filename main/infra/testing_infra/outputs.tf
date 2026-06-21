output "instance_id" {
  description = "EC2 instance id of the benchmark machine."
  value       = aws_instance.bench.id
}

output "public_ip" {
  description = "Public IPv4 address of the benchmark machine."
  value       = aws_instance.bench.public_ip
}

output "public_dns" {
  description = "Public DNS name of the benchmark machine."
  value       = aws_instance.bench.public_dns
}

output "region" {
  description = "Region the instance runs in."
  value       = var.region
}

output "ssh_key_path" {
  description = "Local path to the generated private key."
  value       = abspath(local_sensitive_file.ssh_private_key.filename)
}

output "ssh_command" {
  description = "Ready-to-use SSH command."
  value       = "ssh -i ${abspath(local_sensitive_file.ssh_private_key.filename)} -o StrictHostKeyChecking=accept-new ubuntu@${aws_instance.bench.public_ip}"
}

output "provision_log_hint" {
  description = "How to follow the provisioning log once the box is up."
  value       = "ssh -i ${abspath(local_sensitive_file.ssh_private_key.filename)} ubuntu@${aws_instance.bench.public_ip} 'tail -f /var/log/bioseq-provision.log'"
}
