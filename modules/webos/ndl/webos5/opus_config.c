#include "opus_config.h"
#include <string.h>

bool SS4S_ParseOpusConfig(const unsigned char *data, size_t size, OpusConfig *config) {
    if (!data || !config || size < 19 || memcmp(data, "OpusHead", 8) || data[8] > 15) {
        return false;
    }
    OpusConfig next = {0};
    next.channels = data[9];
    if (next.channels != 1 && next.channels != 2 && next.channels != 6 && next.channels != 8) {
        return false;
    }
    // OpusHead input rate is informational. The NDL path always uses 48 kHz.
    next.sampleRate = 48000;
    if (data[18] == 0) {
        if (next.channels > 2) return false;
        next.streamCount = 1;
        next.coupledCount = next.channels - 1;
        next.mapping[1] = 1;
    } else {
        if (data[18] != 1 || size < (size_t)21 + next.channels) return false;
        next.streamCount = data[19];
        next.coupledCount = data[20];
        unsigned coded = next.streamCount + next.coupledCount;
        if (!next.streamCount || next.coupledCount > next.streamCount || coded > next.channels) return false;
        for (unsigned i = 0; i < next.channels; ++i) {
            if (data[21 + i] != 255 && data[21 + i] >= coded) return false;
            next.mapping[i] = data[21 + i];
        }
    }
    *config = next;
    return true;
}
