# ── Key Pair (สร้างใน Tofu แล้ว download .pem มาเอง) ─────────────────────────

# สร้าง RSA key pair
resource "tls_private_key" "main" {
  algorithm = "RSA"
  rsa_bits  = 4096
}

# อัปโหลด public key ขึ้น AWS
resource "aws_key_pair" "main" {
  key_name   = "${var.project_name}-key"
  public_key = tls_private_key.main.public_key_openssh
}

# บันทึก private key ลงเครื่อง (ใช้ SSH เข้า EC2)
resource "local_sensitive_file" "private_key" {
  content         = tls_private_key.main.private_key_pem
  filename        = "${path.module}/${var.project_name}-key.pem"
  file_permission = "0600"  # อ่านได้เฉพาะ owner (Linux/Mac)
}


# ── Security Group ────────────────────────────────────────────────────────────

resource "aws_security_group" "ec2" {
  name        = "${var.project_name}-ec2-sg"
  description = "SPL Logger EC2: SSH, InfluxDB, Grafana"
  vpc_id      = aws_vpc.main.id

  # SSH
  ingress {
    description = "SSH"
    from_port   = 22
    to_port     = 22
    protocol    = "tcp"
    cidr_blocks = [var.allowed_cidr]
  }

  # InfluxDB — ESP32 + dashboard access
  ingress {
    description = "InfluxDB"
    from_port   = 8086
    to_port     = 8086
    protocol    = "tcp"
    cidr_blocks = [var.allowed_cidr]
  }

  # Grafana — dashboard access
  ingress {
    description = "Grafana"
    from_port   = 3000
    to_port     = 3000
    protocol    = "tcp"
    cidr_blocks = [var.allowed_cidr]
  }

  # TCP Audio Stream — ESP32 stream raw PCM มาให้ EC2 forward ไป KVS + S3
  ingress {
    description = "TCP Audio Stream"
    from_port   = 5001
    to_port     = 5001
    protocol    = "tcp"
    cidr_blocks = ["0.0.0.0/0"]
  }

  # WebSocket Audio Player — browser realtime listen + player.html
  ingress {
    description = "WebSocket Audio Player"
    from_port   = 5002
    to_port     = 5002
    protocol    = "tcp"
    cidr_blocks = ["0.0.0.0/0"]
  }

  # Allow all outbound (apt-get, Docker Hub, NTP, etc.)
  egress {
    from_port   = 0
    to_port     = 0
    protocol    = "-1"
    cidr_blocks = ["0.0.0.0/0"]
  }
}


# ── EC2 Instance ──────────────────────────────────────────────────────────────

resource "aws_instance" "main" {
  ami                    = data.aws_ami.ubuntu.id
  instance_type          = var.instance_type
  key_name               = aws_key_pair.main.key_name
  subnet_id              = aws_subnet.main.id
  vpc_security_group_ids = [aws_security_group.ec2.id]
  iam_instance_profile   = aws_iam_instance_profile.ec2_profile.name

  root_block_device {
    volume_type           = "gp3"
    volume_size           = var.ebs_size_gb
    delete_on_termination = true
  }

  user_data = templatefile("${path.module}/templates/user_data.sh.tpl", {
    influxdb_username  = var.influxdb_username
    influxdb_password  = var.influxdb_password
    influxdb_org       = var.influxdb_org
    influxdb_bucket    = var.influxdb_bucket
    influxdb_retention = var.influxdb_retention
    grafana_password   = var.grafana_password
    s3_bucket          = aws_s3_bucket.wav.bucket
    aws_region         = var.aws_region
  })

  tags = {
    Name = "${var.project_name}-server"
  }
}


# ── Elastic IP (stable public IP แม้ reboot) ─────────────────────────────────

resource "aws_eip" "main" {
  instance = aws_instance.main.id
  domain   = "vpc"
}
