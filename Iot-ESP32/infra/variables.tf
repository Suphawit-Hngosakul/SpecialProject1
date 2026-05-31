variable "aws_region" {
  description = "AWS region"
  type        = string
  default     = "ap-southeast-1"
}

variable "project_name" {
  description = "Prefix for all resource names"
  type        = string
  default     = "spl-logger"
}

variable "instance_type" {
  description = "EC2 instance type for InfluxDB + Grafana"
  type        = string
  default     = "t3.small"
}

variable "allowed_cidr" {
  description = "CIDR allowed to access InfluxDB (8086) and Grafana (3000). Use your IP: x.x.x.x/32"
  type        = string
  default     = "0.0.0.0/0"  # แนะนำให้ใส่ IP จริงในการใช้งานจริง
}

variable "influxdb_username" {
  description = "InfluxDB admin username"
  type        = string
  default     = "admin"
}

variable "influxdb_password" {
  description = "InfluxDB admin password (min 8 chars)"
  type        = string
  sensitive   = true
}

variable "influxdb_org" {
  description = "InfluxDB organisation name"
  type        = string
  default     = "spl-org"
}

variable "influxdb_bucket" {
  description = "InfluxDB bucket name"
  type        = string
  default     = "spl-logger"
}

variable "influxdb_retention" {
  description = "InfluxDB data retention period"
  type        = string
  default     = "30d"
}

variable "grafana_password" {
  description = "Grafana admin password"
  type        = string
  sensitive   = true
}

variable "ebs_size_gb" {
  description = "EBS root volume size in GB"
  type        = number
  default     = 30
}
