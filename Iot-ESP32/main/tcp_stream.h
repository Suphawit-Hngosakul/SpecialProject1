#ifndef TCP_STREAM_H
#define TCP_STREAM_H

#include <Arduino.h>

// Task: stream raw PCM audio blocks ไปยัง EC2 ผ่าน TCP
// EC2 รับแล้ว forward ไป Kinesis Video Streams (realtime HLS) + S3 WAV backup
void tcpStreamTask(void *parameter);

#endif // TCP_STREAM_H
