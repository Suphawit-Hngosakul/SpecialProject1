# ── Audio Server Deployment ────────────────────────────────────────────────────
# Re-deploys server.py / player.html / service ไปยัง EC2 ผ่าน SSH
# ทำงานอัตโนมัติเมื่อไฟล์เปลี่ยน (trigger ด้วย filesha256) หรือเมื่อ EC2 ถูกสร้างใหม่

resource "null_resource" "audio_server_deploy" {
  triggers = {
    server_py_hash   = filesha256("${path.module}/audio_server/server.py")
    player_html_hash = filesha256("${path.module}/audio_server/player.html")
    service_hash     = filesha256("${path.module}/audio_server/audio_server.service")
    instance_id      = aws_instance.main.id
  }

  connection {
    type        = "ssh"
    host        = aws_eip.main.public_ip
    user        = "ubuntu"
    private_key = tls_private_key.main.private_key_pem
    timeout     = "10m"
  }

  # ── Step 1: รอ cloud-init เสร็จ + สร้าง directory ────────────────────────────
  provisioner "remote-exec" {
    inline = [
      # รอ user_data / cloud-init ให้เสร็จก่อน (สำคัญมากสำหรับ EC2 ใหม่)
      "cloud-init status --wait 2>/dev/null || true",
      "sudo mkdir -p /opt/audio_server",
      "sudo chown ubuntu:ubuntu /opt/audio_server",
    ]
  }

  # ── Step 2: copy ไฟล์ ─────────────────────────────────────────────────────────
  provisioner "file" {
    source      = "${path.module}/audio_server/server.py"
    destination = "/opt/audio_server/server.py"
  }

  provisioner "file" {
    source      = "${path.module}/audio_server/player.html"
    destination = "/opt/audio_server/player.html"
  }

  provisioner "file" {
    source      = "${path.module}/audio_server/audio_server.service"
    destination = "/tmp/audio_server.service"
  }

  # ── Step 3: ติดตั้ง deps + configure .env + register + start ─────────────────
  provisioner "remote-exec" {
    inline = [
      # ติดตั้ง pip3 ถ้ายังไม่มี (EC2 เก่าที่ user_data ไม่ได้ติดตั้ง)
      "command -v pip3 >/dev/null 2>&1 || sudo apt-get install -y python3-pip",

      # Install Python deps (ลอง --break-system-packages ก่อน, fallback ถ้าไม่รองรับ)
      "pip3 install websockets boto3 --break-system-packages 2>/dev/null || pip3 install websockets boto3 --user 2>/dev/null || sudo pip3 install websockets boto3",

      # Create .env ถ้ายังไม่มี (EC2 เก่าที่ user_data ไม่ได้สร้างไว้)
      "[ -f /opt/audio_server/.env ] || printf 'S3_BUCKET=${aws_s3_bucket.wav.bucket}\\nAWS_REGION=${var.aws_region}\\nS3_PREFIX=recordings\\nSEGMENT_SEC=60\\nLISTEN_PORT=5001\\nWS_PORT=5002\\nSAMPLE_RATE=16000\\n' > /opt/audio_server/.env",
      "chmod 600 /opt/audio_server/.env",

      # เพิ่ม WS_PORT / SAMPLE_RATE ถ้ายังไม่มี (EC2 ที่สร้างก่อน feature นี้)
      "grep -q '^WS_PORT=' /opt/audio_server/.env || echo 'WS_PORT=5002' >> /opt/audio_server/.env",
      "grep -q '^SAMPLE_RATE=' /opt/audio_server/.env || echo 'SAMPLE_RATE=16000' >> /opt/audio_server/.env",

      # Register + enable + restart systemd service
      "sudo cp /tmp/audio_server.service /etc/systemd/system/audio-server.service",
      "sudo systemctl daemon-reload",
      "sudo systemctl enable audio-server",
      "sudo systemctl restart audio-server",
      "sleep 3",

      # แสดง log หลัง start (|| true: ไม่ให้ fail apply ถ้า service ยัง warm up)
      "sudo journalctl -u audio-server --no-pager -n 20 || true",
    ]
  }

  depends_on = [aws_eip.main]
}
