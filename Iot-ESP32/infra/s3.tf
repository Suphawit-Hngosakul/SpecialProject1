# ── S3 Bucket for WAV recordings (uploaded by EC2 TCP stream server) ──────────

resource "aws_s3_bucket" "wav" {
  bucket        = "${var.project_name}-audio-${data.aws_caller_identity.current.account_id}"
  force_destroy = false
}

# Block all public access
resource "aws_s3_bucket_public_access_block" "wav" {
  bucket = aws_s3_bucket.wav.id

  block_public_acls       = true
  block_public_policy     = true
  ignore_public_acls      = true
  restrict_public_buckets = true
}

# Auto-delete recordings after 90 days
resource "aws_s3_bucket_lifecycle_configuration" "wav" {
  bucket = aws_s3_bucket.wav.id

  rule {
    id     = "expire-recordings"
    status = "Enabled"

    filter {
      prefix = "recordings/"
    }

    expiration {
      days = 90
    }
  }
}

# Current AWS account ID (used for unique bucket name)
data "aws_caller_identity" "current" {}
