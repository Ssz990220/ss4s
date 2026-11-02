/* Linked after the route shim, this tests its C++ symbol interposition ABI. */
#include <assert.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <glib.h>

extern int SS4S_Opus71Begin(const unsigned char *, size_t);
extern void SS4S_Opus71End(void);
extern char *SS4S_Opus71LoadConfig(const char *, const unsigned char *, size_t);
extern const char *mock_last_input(void);
typedef void (*Callback)(int, long long, const char *);
extern bool load(void *, const char *, Callback) __asm__("_ZN17StarfishMediaAPIs4LoadEPKcPFvixS1_E");

static const char *fixture = "{\"mediaTransportType\":\"DIRECTMEDIA-ES-PLAYER\",\"option\":{"
 "\"appId\":\"com.ssz990220.moonlight.opus71\",\"lowDelayMode\":true,"
 "\"externalStreamingInfo\":{\"contents\":{\"codec\":{\"video\":\"H265\",\"audio\":\"OPUS\"},"
 "\"opusInfo\":{\"channels\":6,\"sampleRate\":48,\"streamHeader\":\"T3B1c0hlYWQBBgA=\"}}},"
 "\"adaptiveStreaming\":{\"maxWidth\":3840,\"maxHeight\":2160,\"maxFrameRate\":120}}}";
int main(void) {
    unsigned char header[29] = "OpusHead";
    header[8]=1; header[9]=8; header[12]=0x80; header[13]=0xbb; header[18]=1;
    for (int i=0;i<8;++i) header[21+i]=(unsigned char)i;
    assert(load((void *)0x1234,fixture,NULL));
    assert(strcmp(mock_last_input(),fixture)==0); /* Inactive session unchanged. */
    for (int quality=0;quality<2;++quality) {
        header[19]=quality?8:5; header[20]=quality?0:3;
        char *wrapped=g_strdup_printf("{\"args\":[%s]}",fixture);
        const char *source=quality?wrapped:fixture;
        assert(SS4S_Opus71Begin(header,29)==0);
        assert(load((void *)0x1234,source,NULL));
        const char *actual=mock_last_input();
        assert(strstr(actual,"\"channels\":8") && !strstr(actual,"\"channels\":6"));
        assert(strstr(actual,"\"video\":\"H265\"") && strstr(actual,"\"maxFrameRate\":120"));
        assert(strstr(actual,"\"maxWidth\":3840") && strstr(actual,"\"maxHeight\":2160"));
        char *encoded=g_base64_encode(header,29);
        assert(strstr(actual,encoded));
        g_free(encoded);
        assert(!SS4S_Opus71LoadConfig("{broken",header,29));
        assert(!SS4S_Opus71LoadConfig("{} trailing",header,29));
        char *wrong=g_strdup(fixture);
        char *app=strstr(wrong,"com.ssz990220"); app[0]='x';
        assert(!SS4S_Opus71LoadConfig(wrong,header,29));g_free(wrong);
        assert(!SS4S_Opus71LoadConfig(fixture,header,28));
        SS4S_Opus71End();
        assert(load((void *)0x1234,source,NULL));
        assert(strcmp(mock_last_input(),source)==0);
        g_free(wrapped);
    }
    return 0;
}
