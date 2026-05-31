"""
Lambda: generate S3 presigned PUT URL for WAV upload.

Query param: ?file=audio_20260411_103000.wav
Response:    { "url": "https://bucket.s3.amazonaws.com/wav/audio_...?X-Amz-..." }
"""

import boto3
import json
import os
import re

s3     = boto3.client("s3")
BUCKET = os.environ["S3_BUCKET"]
EXPIRY = int(os.environ.get("URL_EXPIRY_SECONDS", "3600"))

# Allow only safe filenames: letters, digits, underscore, hyphen, dot
_SAFE = re.compile(r"^[\w\-\.]+$")


def handler(event, context):
    params    = event.get("queryStringParameters") or {}
    filename  = params.get("file", "audio.wav")

    # Sanitise filename
    filename = os.path.basename(filename)           # strip any path
    if not _SAFE.match(filename):
        return _response(400, {"error": "Invalid filename"})

    key = f"wav/{filename}"

    url = s3.generate_presigned_url(
        "put_object",
        Params={
            "Bucket":      BUCKET,
            "Key":         key,
            "ContentType": "audio/wav",
        },
        ExpiresIn=EXPIRY,
    )

    return _response(200, {"url": url, "key": key, "bucket": BUCKET})


def _response(status_code, body):
    return {
        "statusCode": status_code,
        "headers":    {"Content-Type": "application/json"},
        "body":       json.dumps(body),
    }
