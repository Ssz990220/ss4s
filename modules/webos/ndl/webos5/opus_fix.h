#pragma once

#include "opus_config.h"

typedef struct SS4S_NDLOpusFix SS4S_NDLOpusFix;

SS4S_NDLOpusFix *SS4S_NDLOpusFixCreate(const OpusConfig *config);

int SS4S_NDLOpusFixProcess(SS4S_NDLOpusFix *instance, const unsigned char *data, size_t size);

const uint8_t *SS4S_NDLOpusFixGetBuffer(SS4S_NDLOpusFix *instance);

void SS4S_NDLOpusFixDestroy(SS4S_NDLOpusFix *instance);
