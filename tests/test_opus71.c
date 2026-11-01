#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <gst/gst.h>
#include <opus_multistream.h>
#include "opus_config.h"

extern GstCaps *SS4S_Opus71InputCaps(const GstCaps *, const unsigned char *, size_t);
extern gboolean SS4S_Opus71NativeCaps(const GstCaps *);

static void make_header(unsigned char data[29], int quality) {
    const unsigned char map[8] = {0, 6, 1, 2, 3, 4, 5, 7};
    memset(data, 0, 29);
    memcpy(data, "OpusHead", 8);
    data[8] = 1; data[9] = 8;
    data[12] = 0x80; data[13] = 0xbb; data[18] = 1;
    data[19] = quality ? 8 : 5; data[20] = quality ? 0 : 3;
    memcpy(data + 21, map, 8);
}

static void roundtrip(int quality) {
    unsigned char data[29];
    make_header(data, quality);
    OpusConfig config;
    assert(SS4S_ParseOpusConfig(data, sizeof(data), &config));
    int error;
    OpusMSEncoder *encoder = opus_multistream_encoder_create(48000, 8, config.streamCount,
        config.coupledCount, config.mapping, OPUS_APPLICATION_RESTRICTED_LOWDELAY, &error);
    assert(encoder && error == OPUS_OK);
    opus_multistream_encoder_ctl(encoder, OPUS_SET_BITRATE(quality ? 2048000 : 450000));
    OpusMSDecoder *decoder = opus_multistream_decoder_create(48000, 8, config.streamCount,
        config.coupledCount, config.mapping, &error);
    assert(decoder && error == OPUS_OK);
    // All eight isolated channel impulses travel through real libopus. No sound device.
    for (int channel = 0; channel < 8; ++channel) {
        opus_multistream_encoder_ctl(encoder, OPUS_RESET_STATE);
        opus_multistream_decoder_ctl(decoder, OPUS_RESET_STATE);
        double energy[8] = {0};
        for (int frame = 0; frame < 12; ++frame) {
            float input[240 * 8] = {0}, output[240 * 8];
            for (int i = 0; i < 240; ++i) input[i * 8 + channel] = .1f * sinf((frame * 240 + i) * .1f);
            unsigned char packet[8192];
            int size = opus_multistream_encode_float(encoder, input, 240, packet, sizeof(packet));
            assert(size > 0);
            int count = opus_multistream_decode_float(decoder, packet, size, output, 240, 0);
            assert(count == 240);
            for (int i = 0; i < count; ++i)
                for (int c = 0; c < 8; ++c) energy[c] += output[i * 8 + c] * output[i * 8 + c];
        }
        assert(energy[channel] > 1.0);
        for (int c = 0; c < 8; ++c) if (c != channel) assert(energy[c] < energy[channel] * .02);
    }
    opus_multistream_encoder_destroy(encoder);
    opus_multistream_decoder_destroy(decoder);
}

int main(int argc, char **argv) {
    gst_init(&argc, &argv);
    unsigned char data[29];
    OpusConfig config;
    make_header(data, 0);
    assert(SS4S_ParseOpusConfig(data, 29, &config) && config.channels == 8 && config.streamCount == 5);
    for (size_t n = 0; n < 29; ++n) assert(!SS4S_ParseOpusConfig(data, n, &config));
    data[9] = 9; assert(!SS4S_ParseOpusConfig(data, 29, &config)); data[9] = 8;
    data[20] = 6; assert(!SS4S_ParseOpusConfig(data, 29, &config)); data[20] = 3;
    data[21] = 8; assert(!SS4S_ParseOpusConfig(data, 29, &config)); data[21] = 0;
    data[18] = 0; assert(!SS4S_ParseOpusConfig(data, 29, &config)); data[18] = 1;
    unsigned char stereo[19] = "OpusHead";
    stereo[8] = 1; stereo[9] = 2;
    assert(SS4S_ParseOpusConfig(stereo, 19, &config) && config.mapping[1] == 1);

    GstCaps *input = gst_caps_from_string("audio/x-opus,channels=(int)8,rate=(int)48000");
    GstCaps *next = SS4S_Opus71InputCaps(input, data, sizeof(data));
    assert(next);
    const GValue *array = gst_structure_get_value(gst_caps_get_structure(next, 0), "streamheader");
    assert(gst_value_array_get_size(array) == 1);
    GstBuffer *buf = gst_value_get_buffer(gst_value_array_get_value(array, 0));
    unsigned char copied[29];
    assert(gst_buffer_extract(buf, 0, copied, sizeof(copied)) == 29 && memcmp(copied, data, 29) == 0);
    assert(!gst_structure_has_field(gst_caps_get_structure(input, 0), "streamheader"));
    gst_caps_unref(next); gst_caps_unref(input);
    input = gst_caps_from_string("audio/x-opus,channels=(int)6,rate=(int)48000");
    assert(!SS4S_Opus71InputCaps(input, data, 29)); gst_caps_unref(input);

    GstCaps *native = gst_caps_from_string("audio/x-raw,channels=(int)8,rate=(int)48000,format=S32LE,layout=interleaved,codec=(uint)17,acmod=(uint)21");
    assert(SS4S_Opus71NativeCaps(native));
    GstStructure *s = gst_caps_get_structure(native, 0);
    gst_structure_set(s, "acmod", G_TYPE_UINT, 7u, NULL);
    assert(!SS4S_Opus71NativeCaps(native)); // Padded 5.1 is not 7.1.
    gst_structure_set(s, "acmod", G_TYPE_INT, 21, NULL);
    assert(!SS4S_Opus71NativeCaps(native)); // Exact native field types matter.
    gst_structure_set(s, "acmod", G_TYPE_UINT, 21u, "DUMMY", G_TYPE_INT, 1, NULL);
    assert(!SS4S_Opus71NativeCaps(native));
    gst_caps_unref(native);
    roundtrip(0); roundtrip(1);
    puts("PASS: bounded OpusHead parser, exact CAPS, 16 isolated Opus channel roundtrips; TV untested");
    return 0;
}
