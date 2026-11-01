#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct OpusConfig {
    uint32_t sampleRate;
    uint8_t channels;
    uint8_t streamCount;
    uint8_t coupledCount;
    uint8_t mapping[8];
} OpusConfig;

bool SS4S_ParseOpusConfig(const unsigned char *data, size_t size, OpusConfig *config);
