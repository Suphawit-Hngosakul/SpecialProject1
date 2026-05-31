#ifndef AUDIO_ENGINE_H
#define AUDIO_ENGINE_H

#include "config.h"
#include <Arduino.h>
#include <FS.h>


struct WAVHeader {
  char riff[4] = {'R', 'I', 'F', 'F'};
  uint32_t fileSize;
  char wave[4] = {'W', 'A', 'V', 'E'};
  char fmt[4] = {'f', 'm', 't', ' '};
  uint32_t fmtSize = 16;
  uint16_t audioFormat = 1;
  uint16_t numChannels = 1;
  uint32_t sampleRate = SAMPLE_RATE;
  uint32_t byteRate = SAMPLE_RATE * 2;
  uint16_t blockAlign = 2;
  uint16_t bitsPerSample = BITS_PER_SAMPLE;
  char data[4] = {'d', 'a', 't', 'a'};
  uint32_t dataSize;
};

void createWAVHeader(WAVHeader &h, uint32_t dataSize);
void writeWAVHeader(File &file, uint32_t dataSize);
void micTask(void *parameter);

#endif // AUDIO_ENGINE_H
