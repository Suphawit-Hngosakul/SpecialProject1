# ── EC2 ───────────────────────────────────────────────────────────────────────

output "ec2_public_ip" {
  description = "Elastic IP ของ EC2"
  value       = aws_eip.main.public_ip
}

output "influxdb_url" {
  description = "InfluxDB endpoint สำหรับ ESP32 (INFLUX_URL)"
  value       = "http://${aws_eip.main.public_ip}:8086"
}

output "grafana_url" {
  description = "Grafana dashboard URL"
  value       = "http://${aws_eip.main.public_ip}:3000"
}

output "private_key_path" {
  description = "ไฟล์ private key สำหรับ SSH"
  value       = local_sensitive_file.private_key.filename
}

output "ssh_command" {
  description = "SSH command เข้า EC2"
  value       = "ssh -i ${local_sensitive_file.private_key.filename} ubuntu@${aws_eip.main.public_ip}"
}

output "tcp_stream_endpoint" {
  description = "TCP Audio Stream endpoint สำหรับ ESP32 (EC2_IP:5001)"
  value       = "${aws_eip.main.public_ip}:5001"
}

output "audio_player_url" {
  description = "Realtime audio player URL (browser)"
  value       = "http://${aws_eip.main.public_ip}:5002/"
}


# ── S3 ────────────────────────────────────────────────────────────────────────

output "s3_bucket_name" {
  description = "S3 bucket สำหรับเก็บ WAV recordings"
  value       = aws_s3_bucket.wav.bucket
}

output "s3_recordings_prefix" {
  description = "S3 path prefix ของ recordings"
  value       = "s3://${aws_s3_bucket.wav.bucket}/recordings/"
}


# ── config.h values ───────────────────────────────────────────────────────────

output "esp32_config_snippet" {
  description = "ค่าสำหรับ copy ใส่ config.h บน ESP32"
  value = <<-EOT
    // ===== ใส่ใน config.h =====
    #define INFLUX_URL  "http://${aws_eip.main.public_ip}:8086"
    #define EC2_IP      "${aws_eip.main.public_ip}"
    #define EC2_PORT    5001
    // ==========================
  EOT
}
