#!/usr/bin/env python3
"""
TCP Audio Server — ESP32 raw PCM s16le 44100 Hz mono

Ports:
  5001  TCP   — รับ raw PCM จาก ESP32
  5002  WS    — broadcast PCM chunks ไป browser (realtime listen)
  5002  HTTP  — serve player.html (GET /)

Flow:
  ESP32 ──TCP:5001──► server.py ──WS:5002──► browser (Web Audio API)
                                └──boto3──► S3 recordings/*.wav (60s segments)
"""

import asyncio, json, os, socket, struct, threading
from datetime import datetime, timezone
from pathlib import Path
from urllib.parse import urlparse, parse_qs, unquote

import boto3
from botocore.exceptions import BotoCoreError, ClientError
import websockets
from websockets.server import serve as ws_serve

# ── Config ────────────────────────────────────────────────────────────────────
TCP_PORT     = int(os.environ.get("LISTEN_PORT",  "5001"))
WS_PORT      = int(os.environ.get("WS_PORT",      "5002"))
S3_BUCKET    = os.environ["S3_BUCKET"]
S3_PREFIX    = os.environ.get("S3_PREFIX",   "recordings")
AWS_REGION   = os.environ.get("AWS_REGION",  "ap-southeast-1")
SEGMENT_SEC  = int(os.environ.get("SEGMENT_SEC", "60"))

# ── Audio constants ───────────────────────────────────────────────────────────
SAMPLE_RATE      = int(os.environ.get("SAMPLE_RATE", "16000"))
CHANNELS         = 1
BITS             = 16
BYTES_PER_SAMPLE = CHANNELS * BITS // 8
SEGMENT_BYTES    = SAMPLE_RATE * BYTES_PER_SAMPLE * SEGMENT_SEC  # ~5 MB / 60s

# ── WebSocket client registry ─────────────────────────────────────────────────
_ws_clients: set = set()
_ws_loop: asyncio.AbstractEventLoop | None = None


# ── S3 helpers ────────────────────────────────────────────────────────────────

def _wav_header(data_len: int) -> bytes:
    byte_rate   = SAMPLE_RATE * CHANNELS * BITS // 8
    block_align = CHANNELS * BITS // 8
    return struct.pack(
        "<4sI4s4sIHHIIHH4sI",
        b"RIFF", 36 + data_len, b"WAVE",
        b"fmt ", 16, 1, CHANNELS, SAMPLE_RATE,
        byte_rate, block_align, BITS,
        b"data", data_len,
    )


def _upload(pcm: bytes, seg_ts: str) -> None:
    if not pcm:
        return
    key = f"{S3_PREFIX}/{seg_ts[:8]}/audio_{seg_ts}.wav"
    wav = _wav_header(len(pcm)) + pcm
    try:
        boto3.client("s3", region_name=AWS_REGION).put_object(
            Bucket=S3_BUCKET, Key=key, Body=wav, ContentType="audio/wav"
        )
        print(f"[S3] OK  s3://{S3_BUCKET}/{key}  ({len(wav)//1024} KB)", flush=True)
    except (BotoCoreError, ClientError) as exc:
        print(f"[S3] ERR {key}: {exc}", flush=True)


def _now_ts() -> str:
    return datetime.now(timezone.utc).strftime("%Y%m%d_%H%M%S")


def _list_recordings(max_keys: int = 100) -> list:
    """List WAV recordings จาก S3 เรียงจากใหม่ไปเก่า"""
    try:
        s3  = boto3.client("s3", region_name=AWS_REGION)
        res = s3.list_objects_v2(Bucket=S3_BUCKET, Prefix=f"{S3_PREFIX}/", MaxKeys=max_keys)
        items = [
            {
                "key":           obj["Key"],
                "name":          obj["Key"].split("/")[-1],
                "size_kb":       obj["Size"] // 1024,
                "last_modified": obj["LastModified"].strftime("%Y-%m-%d %H:%M:%S UTC"),
            }
            for obj in res.get("Contents", [])
            if obj["Key"].endswith(".wav")
        ]
        items.sort(key=lambda x: x["key"], reverse=True)
        return items
    except Exception as exc:
        print(f"[S3] list error: {exc}", flush=True)
        return []


def _presign_url(key: str, expires: int = 3600) -> str:
    """สร้าง S3 presigned URL สำหรับ download ไฟล์โดยตรง (1 ชั่วโมง)"""
    try:
        s3 = boto3.client("s3", region_name=AWS_REGION)
        return s3.generate_presigned_url(
            "get_object",
            Params={"Bucket": S3_BUCKET, "Key": key},
            ExpiresIn=expires,
        )
    except Exception as exc:
        print(f"[S3] presign error: {exc}", flush=True)
        return ""


# ── WebSocket broadcast ───────────────────────────────────────────────────────

def _broadcast(pcm_chunk: bytes) -> None:
    """เรียกจาก TCP thread — schedule coroutine เข้า asyncio event loop"""
    if not _ws_loop or not _ws_clients:
        return

    async def _send_all():
        dead = set()
        for ws in list(_ws_clients):
            try:
                await ws.send(pcm_chunk)
            except Exception:
                dead.add(ws)
        _ws_clients.difference_update(dead)

    asyncio.run_coroutine_threadsafe(_send_all(), _ws_loop)


# ── WebSocket server ──────────────────────────────────────────────────────────

PLAYER_HTML = Path(__file__).parent / "player.html"


async def _ws_handler(websocket):
    """รับ WebSocket connection จาก browser"""
    addr = websocket.remote_address
    print(f"[WS]   {addr} connected ({len(_ws_clients)+1} listeners)", flush=True)
    _ws_clients.add(websocket)
    try:
        await websocket.wait_closed()
    finally:
        _ws_clients.discard(websocket)
        print(f"[WS]   {addr} disconnected ({len(_ws_clients)} listeners)", flush=True)


async def _http_handler(path, request_headers):
    """HTTP router: player.html + /api/recordings + /api/url"""
    if request_headers.get("Upgrade", "").lower() == "websocket":
        return None  # ปล่อยให้ websockets library handle WS upgrade

    parsed   = urlparse(path)
    pathname = parsed.path

    if pathname in ("/", ""):
        html = PLAYER_HTML.read_bytes() if PLAYER_HTML.exists() else b"<h1>player.html not found</h1>"
        return (200, [("Content-Type", "text/html; charset=utf-8")], html)

    if pathname == "/api/recordings":
        data = json.dumps(_list_recordings()).encode()
        return (200, [("Content-Type", "application/json"),
                      ("Cache-Control", "no-cache")], data)

    if pathname == "/api/url":
        qs  = parse_qs(parsed.query)
        key = qs.get("key", [None])[0]
        if not key:
            return (400, [], b"missing key")
        url = _presign_url(unquote(key))
        if not url:
            return (500, [], b"presign failed")
        return (200, [("Content-Type", "text/plain")], url.encode())

    return (404, [], b"not found")


async def _run_ws_server():
    global _ws_loop
    _ws_loop = asyncio.get_running_loop()
    async with ws_serve(_ws_handler, "0.0.0.0", WS_PORT,
                        process_request=_http_handler):
        print(f"[WS]   listening on :{WS_PORT}  (player: http://<IP>:{WS_PORT}/)", flush=True)
        await asyncio.Future()


def _start_ws_thread():
    asyncio.run(_run_ws_server())


# ── TCP connection handler ────────────────────────────────────────────────────

def _handle(conn: socket.socket, addr) -> None:
    print(f"[TCP]  {addr} connected", flush=True)
    buf      = bytearray()
    seg_ts   = _now_ts()
    leftover = b""  # byte ที่ค้างจาก recv ก่อน (รอครบ 2 bytes = 1 int16 sample)

    try:
        while True:
            chunk = conn.recv(8192)
            if not chunk:
                break
            buf.extend(chunk)

            # ── Broadcast ไป WebSocket: ต้องส่งเป็น multiple of 2 bytes เสมอ ──
            # TCP recv ไม่รับประกัน boundary — ถ้า broadcast จำนวน bytes คี่
            # Int16Array ใน browser จะ misalign ทำให้เสียงเพี้ยนทั้งหมด
            pending  = leftover + bytes(chunk)
            even_len = len(pending) & ~1   # round down เป็น even number
            leftover = pending[even_len:]  # เก็บ 0 หรือ 1 byte ไว้รอบหน้า
            if even_len:
                _broadcast(pending[:even_len])

            # ── S3 segment upload ─────────────────────────────────────────────
            while len(buf) >= SEGMENT_BYTES:
                pcm    = bytes(buf[:SEGMENT_BYTES])
                buf    = buf[SEGMENT_BYTES:]
                ts     = seg_ts
                seg_ts = _now_ts()
                threading.Thread(target=_upload, args=(pcm, ts), daemon=True).start()

    except OSError as exc:
        print(f"[TCP]  {addr} error: {exc}", flush=True)
    finally:
        if buf:
            threading.Thread(target=_upload, args=(bytes(buf), seg_ts), daemon=True).start()
        conn.close()
        print(f"[TCP]  {addr} disconnected", flush=True)


# ── Main ──────────────────────────────────────────────────────────────────────

def main() -> None:
    print(
        f"[SERVER] bucket={S3_BUCKET}  prefix={S3_PREFIX}  "
        f"region={AWS_REGION}  segment={SEGMENT_SEC}s",
        flush=True,
    )

    # เริ่ม WebSocket server ใน daemon thread แยก
    threading.Thread(target=_start_ws_thread, daemon=True).start()

    # TCP server (main thread)
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("0.0.0.0", TCP_PORT))
    srv.listen(4)
    print(f"[TCP]  listening on :{TCP_PORT}", flush=True)

    while True:
        conn, addr = srv.accept()
        threading.Thread(target=_handle, args=(conn, addr), daemon=True).start()


if __name__ == "__main__":
    main()
