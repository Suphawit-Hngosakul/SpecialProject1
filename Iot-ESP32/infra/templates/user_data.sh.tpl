#!/bin/bash
set -euo pipefail
exec > /var/log/user_data.log 2>&1

echo "=== SPL Logger Server Setup ==="

# ── System update ─────────────────────────────────────────────────────────────
apt-get update -y
apt-get install -y ca-certificates curl gnupg lsb-release

# ── Docker ────────────────────────────────────────────────────────────────────
install -m 0755 -d /etc/apt/keyrings
curl -fsSL https://download.docker.com/linux/ubuntu/gpg \
  | gpg --dearmor -o /etc/apt/keyrings/docker.gpg
chmod a+r /etc/apt/keyrings/docker.gpg

echo "deb [arch=$(dpkg --print-architecture) signed-by=/etc/apt/keyrings/docker.gpg] \
  https://download.docker.com/linux/ubuntu $(lsb_release -cs) stable" \
  | tee /etc/apt/sources.list.d/docker.list > /dev/null

apt-get update -y
apt-get install -y docker-ce docker-ce-cli containerd.io docker-compose-plugin
systemctl enable docker
systemctl start docker

# ── Project directory ─────────────────────────────────────────────────────────
mkdir -p /opt/spl-logger
cd /opt/spl-logger

# ── docker-compose.yml ────────────────────────────────────────────────────────
cat > /opt/spl-logger/docker-compose.yml <<'COMPOSE'
version: '3.8'

services:

  influxdb:
    image: influxdb:2.7
    container_name: influxdb
    restart: unless-stopped
    ports:
      - "8086:8086"
    volumes:
      - influxdb-data:/var/lib/influxdb2
      - influxdb-config:/etc/influxdb2
    environment:
      - DOCKER_INFLUXDB_INIT_MODE=setup
      - DOCKER_INFLUXDB_INIT_USERNAME=${influxdb_username}
      - DOCKER_INFLUXDB_INIT_PASSWORD=${influxdb_password}
      - DOCKER_INFLUXDB_INIT_ORG=${influxdb_org}
      - DOCKER_INFLUXDB_INIT_BUCKET=${influxdb_bucket}
      - DOCKER_INFLUXDB_INIT_RETENTION=${influxdb_retention}

  grafana:
    image: grafana/grafana:latest
    container_name: grafana
    restart: unless-stopped
    ports:
      - "3000:3000"
    volumes:
      - grafana-data:/var/lib/grafana
    environment:
      - GF_SECURITY_ADMIN_PASSWORD=${grafana_password}
      - GF_USERS_ALLOW_SIGN_UP=false
    depends_on:
      - influxdb

volumes:
  influxdb-data:
  influxdb-config:
  grafana-data:
COMPOSE

# ── Start services ────────────────────────────────────────────────────────────
docker compose -f /opt/spl-logger/docker-compose.yml up -d


# ── TCP Audio Stream Server ────────────────────────────────────────────────────
apt-get install -y python3-pip
pip3 install boto3 websockets --break-system-packages 2>/dev/null || pip3 install boto3 websockets

mkdir -p /opt/audio_server

# server.py
cat > /opt/audio_server/server.py << 'PYEOF'
#!/usr/bin/env python3
import asyncio, os, socket, struct, threading
from datetime import datetime, timezone
from pathlib import Path
import boto3
from botocore.exceptions import BotoCoreError, ClientError
import websockets
from websockets.server import serve as ws_serve

TCP_PORT     = int(os.environ.get("LISTEN_PORT", "5001"))
WS_PORT      = int(os.environ.get("WS_PORT",     "5002"))
S3_BUCKET    = os.environ["S3_BUCKET"]
S3_PREFIX    = os.environ.get("S3_PREFIX",  "recordings")
AWS_REGION   = os.environ.get("AWS_REGION", "ap-southeast-1")
SEGMENT_SEC  = int(os.environ.get("SEGMENT_SEC", "60"))

SAMPLE_RATE      = 44100
CHANNELS         = 1
BITS             = 16
BYTES_PER_SAMPLE = CHANNELS * BITS // 8
SEGMENT_BYTES    = SAMPLE_RATE * BYTES_PER_SAMPLE * SEGMENT_SEC

_ws_clients = set()
_ws_loop    = None

def _wav_header(n):
    br = SAMPLE_RATE * CHANNELS * BITS // 8
    ba = CHANNELS * BITS // 8
    return struct.pack("<4sI4s4sIHHIIHH4sI",
        b"RIFF", 36+n, b"WAVE", b"fmt ", 16, 1, CHANNELS,
        SAMPLE_RATE, br, ba, BITS, b"data", n)

def _upload(pcm, ts):
    if not pcm: return
    key = f"{S3_PREFIX}/{ts[:8]}/audio_{ts}.wav"
    wav = _wav_header(len(pcm)) + pcm
    try:
        boto3.client("s3", region_name=AWS_REGION).put_object(
            Bucket=S3_BUCKET, Key=key, Body=wav, ContentType="audio/wav")
        print(f"[S3] OK  s3://{S3_BUCKET}/{key}  ({len(wav)//1024}KB)", flush=True)
    except (BotoCoreError, ClientError) as e:
        print(f"[S3] ERR {key}: {e}", flush=True)

def _now_ts():
    return datetime.now(timezone.utc).strftime("%Y%m%d_%H%M%S")

def _broadcast(data):
    if not _ws_loop or not _ws_clients: return
    async def _send():
        dead = set()
        for ws in list(_ws_clients):
            try: await ws.send(data)
            except: dead.add(ws)
        _ws_clients.difference_update(dead)
    asyncio.run_coroutine_threadsafe(_send(), _ws_loop)

async def _ws_handler(ws):
    _ws_clients.add(ws)
    print(f"[WS]  {ws.remote_address} connected ({len(_ws_clients)} listeners)", flush=True)
    try: await ws.wait_closed()
    finally:
        _ws_clients.discard(ws)
        print(f"[WS]  {ws.remote_address} disconnected", flush=True)

async def _http_handler(path, hdrs):
    if hdrs.get("Upgrade","").lower() == "websocket": return None
    f = Path(__file__).parent / "player.html"
    body = f.read_bytes() if f.exists() else b"<h1>player.html not found</h1>"
    return (200, [("Content-Type","text/html; charset=utf-8")], body)

async def _run_ws():
    global _ws_loop
    _ws_loop = asyncio.get_running_loop()
    async with ws_serve(_ws_handler, "0.0.0.0", WS_PORT, process_request=_http_handler):
        print(f"[WS]  listening on :{WS_PORT}", flush=True)
        await asyncio.Future()

def _handle(conn, addr):
    print(f"[TCP] {addr} connected", flush=True)
    buf = bytearray()
    seg_ts = _now_ts()
    try:
        while True:
            chunk = conn.recv(8192)
            if not chunk: break
            buf.extend(chunk)
            _broadcast(bytes(chunk))
            while len(buf) >= SEGMENT_BYTES:
                pcm = bytes(buf[:SEGMENT_BYTES])
                buf = buf[SEGMENT_BYTES:]
                ts, seg_ts = seg_ts, _now_ts()
                threading.Thread(target=_upload, args=(pcm, ts), daemon=True).start()
    except OSError as e:
        print(f"[TCP] {addr} error: {e}", flush=True)
    finally:
        if buf: threading.Thread(target=_upload, args=(bytes(buf), seg_ts), daemon=True).start()
        conn.close()
        print(f"[TCP] {addr} disconnected", flush=True)

def main():
    print(f"[SERVER] bucket={S3_BUCKET} segment={SEGMENT_SEC}s tcp={TCP_PORT} ws={WS_PORT}", flush=True)
    threading.Thread(target=lambda: asyncio.run(_run_ws()), daemon=True).start()
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("0.0.0.0", TCP_PORT))
    srv.listen(4)
    print(f"[TCP] listening on :{TCP_PORT}", flush=True)
    while True:
        conn, addr = srv.accept()
        threading.Thread(target=_handle, args=(conn, addr), daemon=True).start()

if __name__ == "__main__":
    main()
PYEOF

# .env — inject S3_BUCKET และ AWS_REGION จาก Terraform
cat > /opt/audio_server/.env << EOF
S3_BUCKET=${s3_bucket}
AWS_REGION=${aws_region}
S3_PREFIX=recordings
SEGMENT_SEC=60
LISTEN_PORT=5001
WS_PORT=5002
EOF

chmod 600 /opt/audio_server/.env
chown -R ubuntu:ubuntu /opt/audio_server

# systemd unit
cat > /etc/systemd/system/audio-server.service << 'SVCEOF'
[Unit]
Description=ESP32 TCP Audio Stream Server
After=network-online.target
Wants=network-online.target

[Service]
Type=simple
User=ubuntu
WorkingDirectory=/opt/audio_server
EnvironmentFile=/opt/audio_server/.env
ExecStart=/usr/bin/python3 /opt/audio_server/server.py
Restart=always
RestartSec=5
StandardOutput=journal
StandardError=journal
SyslogIdentifier=audio-server

[Install]
WantedBy=multi-user.target
SVCEOF

systemctl daemon-reload
systemctl enable audio-server
systemctl start audio-server

echo "=== Setup complete ==="
