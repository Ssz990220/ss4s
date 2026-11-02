#include <stddef.h>
#include <dlfcn.h>
#include <string.h>
#include <stdlib.h>

#include "ndl_common.h"
#include "opus_empty.h"
#include "opus_fix.h"

static bool IsOpusPassthroughSupported(const OpusConfig *config);

typedef int (*Opus71Begin)(const unsigned char *, size_t);
typedef void (*Opus71End)(void);
static Opus71Begin opus71_begin;
static Opus71End opus71_end;

static int SupportsPCM6Channel = 0;

static int DriverInit(int argc, char *argv[]) {
    (void) argc;
    (void) argv;
    /*
     * webOS 7.0 introduced PCM 6-channel support as well as NDL_DirectAudioRegisterCallback.
     * We can use this to determine if the platform supports 6-channel PCM. 
     */
    SupportsPCM6Channel = dlsym(RTLD_DEFAULT, "NDL_DirectAudioRegisterCallback") != 0;
#ifdef SS4S_WEBOS_OPUS71_EXPERIMENTAL
    // Only the separate test launcher loads the app-local routing shim.
    const char *enabled = getenv("SS4S_WEBOS_OPUS71");
    if (enabled && strcmp(enabled, "1") == 0) {
        opus71_begin = (Opus71Begin)dlsym(RTLD_DEFAULT, "SS4S_Opus71Begin");
        opus71_end = (Opus71End)dlsym(RTLD_DEFAULT, "SS4S_Opus71End");
    }
#endif
    return 0;
}

static bool GetCapabilities(SS4S_AudioCapabilities *capabilities, SS4S_AudioCodec wantedCodecs) {
    enum SS4S_AudioCodec opusCodec = SS4S_AUDIO_OPUS;
    SS4S_AudioCodec matchedCodecs = wantedCodecs & (SS4S_AUDIO_PCM_S16LE | opusCodec);
    if (matchedCodecs == 0) {
        return false;
    }
    capabilities->codecs = SS4S_AUDIO_PCM_S16LE | opusCodec;
    /*
     * Don't check for system settings to determine the number of channels, just provide 6 channels.
     * webOS should be able to down-mix that anyway.
     */
    capabilities->maxChannels = opus71_begin && opus71_end ? 8 : 6;
    return true;
}

static SS4S_AudioCodec GetPreferredCodecs(const SS4S_AudioInfo *info) {
    if (info->numOfChannels == 6 || (info->numOfChannels == 8 && opus71_begin && opus71_end)) {
        return SS4S_AUDIO_OPUS;
    }
    return SS4S_AUDIO_PCM_S16LE;
}

static SS4S_AudioOpenResult OpenAudio(const SS4S_AudioInfo *info, SS4S_AudioInstance **instance,
                                      SS4S_PlayerContext *context) {
    SS4S_NDL_webOS5_Log(SS4S_LogLevelInfo, "NDL", "OpenAudio called");
    pthread_mutex_lock(&SS4S_NDL_webOS5_Lock);
    SS4S_AudioOpenResult result;
    switch (info->codec) {
        case SS4S_AUDIO_PCM_S16LE: {
            // Never feed eight-channel samples through NDL's stereo fallback.
            if (info->numOfChannels != 1 && info->numOfChannels != 2 && info->numOfChannels != 6) {
                result = SS4S_AUDIO_OPEN_UNSUPPORTED_CODEC;
                goto finish;
            }
            const char *mode = "stereo";
            if (info->numOfChannels == 1) {
                mode = "mono";
            } else if (info->numOfChannels == 6) {
                if (SupportsPCM6Channel) {
                    mode = "6-channel";
                } else {
                    SS4S_NDL_webOS5_Log(SS4S_LogLevelWarn, "NDL", "6-channel PCM is not supported, "
                                                                  "falling back to stereo");
                }
            }
            NDL_DIRECTAUDIO_PCM_INFO_T pcmInfo = {
                    .type = NDL_AUDIO_TYPE_PCM,
                    .format = NDL_DIRECTMEDIA_AUDIO_PCM_FORMAT_S16LE,
                    .channelMode = mode,
                    .sampleRate = NDL_DIRECTMEDIA_AUDIO_PCM_SAMPLE_RATE_OF(info->sampleRate),
            };
            context->mediaInfo.audio.pcm = pcmInfo;
            break;
        }
        case SS4S_AUDIO_OPUS: {
            NDL_DIRECTMEDIA_AUDIO_OPUS_INFO_T opusInfo = {
                    .type = NDL_AUDIO_TYPE_OPUS,
                    .channels = info->numOfChannels,
                    .sampleRate = info->sampleRate / 1000.0,
            };
            if (info->codecData && info->codecDataLen) {
                OpusConfig opusConfig;
                if (!SS4S_ParseOpusConfig(info->codecData, info->codecDataLen, &opusConfig) ||
                    opusConfig.channels != info->numOfChannels || info->sampleRate != 48000) {
                    result = SS4S_AUDIO_OPEN_UNSUPPORTED_CODEC;
                    goto finish;
                }
                if (opusConfig.channels == 8) {
                    if (!opus71_begin || !opus71_end ||
                        !((opusConfig.streamCount == 5 && opusConfig.coupledCount == 3) ||
                          (opusConfig.streamCount == 8 && opusConfig.coupledCount == 0)) ||
                        opus71_begin(info->codecData, info->codecDataLen) != 0) {
                        result = SS4S_AUDIO_OPEN_UNSUPPORTED_CODEC;
                        goto finish;
                    }
                    context->opus71 = true;
                    /* C5 NDL rejects OPUS 8ch before it constructs a decoder.
                     * Bootstrap the accepted six-channel descriptor only; the
                     * active app-local decoder probe restores all eight actual
                     * channels and the full negotiated header before decoding.
                     * Native output must still independently prove PCM8/21. */
                    opusInfo.channels = 6;
                    SS4S_NDL_webOS5_Log(SS4S_LogLevelInfo, "NDL",
                                        "Opus71: NDL descriptor 6ch; decoder receives full OpusHead 8ch");
                }
                // Feed one empty frame to the decoder to ensure it is ready
                context->opusEmpty = SS4S_OpusEmptyCreate(opusConfig.channels, opusConfig.streamCount,
                                                          opusConfig.coupledCount);
                if (!context->opusEmpty) {
                    result = SS4S_AUDIO_OPEN_ERROR;
                    goto finish;
                }
                if (opusConfig.channels == 6 && !IsOpusPassthroughSupported(&opusConfig)) {
                    SS4S_NDL_webOS5_Log(SS4S_LogLevelWarn, "NDL",
                                        "Channel config is not supported, enabling re-encoding. "
                                        "This will introduce audio latency");
                    context->opusFix = SS4S_NDLOpusFixCreate(&opusConfig);
                    if (!context->opusFix) {
                        result = SS4S_AUDIO_OPEN_ERROR;
                        goto finish;
                    }
                }
            } else if (info->numOfChannels > 2) {
                result = SS4S_AUDIO_OPEN_UNSUPPORTED_CODEC;
                goto finish;
            }
            context->mediaInfo.audio.opus = opusInfo;
            break;
        }
        default: {
            result = SS4S_AUDIO_OPEN_UNSUPPORTED_CODEC;
            goto finish;
        }
    }
    if (SS4S_NDL_webOS5_ReloadMedia(context) != 0) {
        result = SS4S_AUDIO_OPEN_ERROR;
        goto finish;
    }
    *instance = (SS4S_AudioInstance *) context;
    result = SS4S_AUDIO_OPEN_OK;

    finish:
    if (result != SS4S_AUDIO_OPEN_OK) {
        if (context->opus71 && opus71_end) opus71_end();
        context->opus71 = false;
        if (context->opusEmpty) SS4S_OpusEmptyDestroy(context->opusEmpty);
        if (context->opusFix) SS4S_NDLOpusFixDestroy(context->opusFix);
        context->opusEmpty = NULL;
        context->opusFix = NULL;
        context->mediaInfo.audio.type = 0;
    }
    pthread_mutex_unlock(&SS4S_NDL_webOS5_Lock);
    SS4S_NDL_webOS5_Log(SS4S_LogLevelInfo, "NDL", "OpenAudio returned %d", result);
    return result;
}

static SS4S_AudioFeedResult FeedAudio(SS4S_AudioInstance *instance, const unsigned char *data, size_t size) {
    pthread_mutex_lock(&SS4S_NDL_webOS5_Lock);
    const SS4S_PlayerContext *context = (void *) instance;
    if (!context->mediaLoaded) {
        pthread_mutex_unlock(&SS4S_NDL_webOS5_Lock);
        return SS4S_AUDIO_FEED_NOT_READY;
    }
    int rc;
    if (context->opusFix) {
        int fixedSize = SS4S_NDLOpusFixProcess(context->opusFix, data, size);
        if (fixedSize < 0) {
            SS4S_NDL_webOS5_Log(SS4S_LogLevelWarn, "NDL", "SS4S_NDLOpusFixProcess returned %d", fixedSize);
            pthread_mutex_unlock(&SS4S_NDL_webOS5_Lock);
            return SS4S_AUDIO_FEED_ERROR;
        }
        data = SS4S_NDLOpusFixGetBuffer(context->opusFix);
        size = fixedSize;
    }
    uint64_t pts = SS4S_NDL_webOS5_GetPts(context);
    rc = NDL_DirectAudioPlay((void *) data, size, (long long) pts);
    if (rc != 0) {
        SS4S_NDL_webOS5_Log(SS4S_LogLevelWarn, "NDL", "NDL_DirectAudioPlay returned %d: %s", rc,
                            NDL_DirectMediaGetError());
        pthread_mutex_unlock(&SS4S_NDL_webOS5_Lock);
        return SS4S_AUDIO_FEED_ERROR;
    }
    pthread_mutex_unlock(&SS4S_NDL_webOS5_Lock);
    return SS4S_AUDIO_FEED_OK;
}

static void CloseAudio(SS4S_AudioInstance *instance) {
    SS4S_NDL_webOS5_Log(SS4S_LogLevelInfo, "NDL", "CloseAudio called");
    pthread_mutex_lock(&SS4S_NDL_webOS5_Lock);
    SS4S_PlayerContext *context = (void *) instance;
    context->mediaInfo.audio.type = 0;
    if (context->opusFix) {
        SS4S_NDLOpusFixDestroy(context->opusFix);
        context->opusFix = NULL;
    }
    if (context->opusEmpty) {
        SS4S_OpusEmptyDestroy(context->opusEmpty);
        context->opusEmpty = NULL;
    }
    SS4S_NDL_webOS5_UnloadMedia(context);
    if (context->opus71 && opus71_end) opus71_end();
    context->opus71 = false;
    pthread_mutex_unlock(&SS4S_NDL_webOS5_Lock);
}


bool IsOpusPassthroughSupported(const OpusConfig *config) {
    static const uint8_t wantedMapping[6] = {0, 1, 4, 5, 2, 3};
    return config->streamCount == 4 && config->coupledCount == 2 && memcmp(config->mapping, wantedMapping, 6) == 0;
}

const SS4S_AudioDriver SS4S_NDL_webOS5_AudioDriver = {
        .Base = {
                .Init = DriverInit,
                .PostInit = SS4S_NDL_webOS5_Driver_PostInit,
                .Quit = SS4S_NDL_webOS5_Driver_Quit,
        },
        .GetCapabilities = GetCapabilities,
        .GetPreferredCodecs = GetPreferredCodecs,
        .Open = OpenAudio,
        .Feed = FeedAudio,
        .Close = CloseAudio,
};
