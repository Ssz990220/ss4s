/* App-local Starfish Load configuration adapter, LGPL-3.0-or-later.
 * NDL validates its six-channel descriptor, then this boundary restores the
 * real negotiated Opus8 metadata before Starfish creates the media pipeline.
 * json-c is resolved at runtime; no vendor library or global setting changes.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <pthread.h>
#include <string.h>
#include <glib.h>
#include "opus_config.h"

typedef struct json_object Json;
typedef struct json_tokener Tokener;
static struct {
    Tokener *(*new_tokener)(void);
    void (*free_tokener)(Tokener *);
    void (*flags)(Tokener *, int);
    Json *(*parse)(Tokener *, const char *, int);
    int (*error)(Tokener *);
    int (*type)(const Json *);
    int (*field)(const Json *, const char *, Json **);
    size_t (*length)(const Json *);
    Json *(*at)(const Json *, size_t);
    const char *(*string)(Json *);
    int (*integer)(const Json *);
    Json *(*new_int)(int);
    Json *(*new_string)(const char *);
    int (*add)(Json *, const char *, Json *);
    const char *(*serialize)(Json *, int);
    int (*put)(Json *);
    int ready;
} api;
static pthread_once_t once = PTHREAD_ONCE_INIT;
static void initialize(void) {
    void *lib = dlopen("libjson-c.so.5", RTLD_LAZY | RTLD_LOCAL);
    if (!lib) lib = dlopen("libjson-c.so", RTLD_LAZY | RTLD_LOCAL);
    if (!lib) return;
#define RESOLVE(member, symbol) do { *(void **)(&api.member) = dlsym(lib, symbol); if (!api.member) return; } while (0)
    RESOLVE(new_tokener, "json_tokener_new");
    RESOLVE(free_tokener, "json_tokener_free");
    RESOLVE(flags, "json_tokener_set_flags");
    RESOLVE(parse, "json_tokener_parse_ex");
    RESOLVE(error, "json_tokener_get_error");
    RESOLVE(type, "json_object_get_type");
    RESOLVE(field, "json_object_object_get_ex");
    RESOLVE(length, "json_object_array_length");
    RESOLVE(at, "json_object_array_get_idx");
    RESOLVE(string, "json_object_get_string");
    RESOLVE(integer, "json_object_get_int");
    RESOLVE(new_int, "json_object_new_int");
    RESOLVE(new_string, "json_object_new_string");
    RESOLVE(add, "json_object_object_add");
    RESOLVE(serialize, "json_object_to_json_string_ext");
    RESOLVE(put, "json_object_put");
#undef RESOLVE
    api.ready = 1;
}
static Json *field(Json *object, const char *name) {
    Json *value = NULL;
    return object && api.type(object) == 4 && api.field(object, name, &value) ? value : NULL;
}
static gboolean matches(Json *object, const char *name, const char *value) {
    Json *item = field(object, name);
    return item && api.type(item) == 6 && g_strcmp0(api.string(item), value) == 0;
}

/* NULL means no eligible change. Returned text is owned by the caller. */
char *SS4S_Opus71LoadConfig(const char *text, const unsigned char *header, size_t size) {
    OpusConfig config;
    if (!text || !SS4S_ParseOpusConfig(header, size, &config) || config.channels != 8) return NULL;
    size_t length = strnlen(text, 65537);
    if (!length || length > 65536) return NULL;
    pthread_once(&once, initialize);
    if (!api.ready) return NULL;
    Tokener *tokener = api.new_tokener();
    if (!tokener) return NULL;
    api.flags(tokener, 1); /* JSON_TOKENER_STRICT */
    Json *root = api.parse(tokener, text, (int)length + 1);
    int error = api.error(tokener);
    api.free_tokener(tokener);
    if (!root || error) { if (root) api.put(root); return NULL; }
    char *result = NULL;
    Json *entry = root, *args = field(root, "args");
    if (args) {
        if (api.type(args) != 5 || api.length(args) != 1) goto finish;
        entry = api.at(args, 0);
    }
    if (!matches(entry, "mediaTransportType", "DIRECTMEDIA-ES-PLAYER")) goto finish;
    Json *option = field(entry, "option");
    if (!matches(option, "appId", "com.ssz990220.moonlight.opus71")) goto finish;
    Json *contents = field(field(option, "externalStreamingInfo"), "contents");
    if (!matches(field(contents, "codec"), "audio", "OPUS")) goto finish;
    Json *opus = field(contents, "opusInfo");
    Json *channels = field(opus, "channels"), *rate = field(opus, "sampleRate");
    Json *old_header = field(opus, "streamHeader");
    if (!channels || api.type(channels) != 3 || api.integer(channels) != 6 ||
        !rate || (api.type(rate) != 2 && api.type(rate) != 3) || api.integer(rate) != 48 ||
        !old_header || api.type(old_header) != 6) goto finish;
    const char *encoded = api.string(old_header);
    if (!encoded || strlen(encoded) > 128) goto finish;
    gsize decoded_size = 0;
    guchar *decoded = g_base64_decode(encoded, &decoded_size);
    gboolean prefix = decoded_size >= 10 && memcmp(decoded, "OpusHead", 8) == 0 && decoded[9] == 6;
    g_free(decoded);
    if (!prefix) goto finish;
    gchar *full_header = g_base64_encode(header, size);
    Json *new_channels = api.new_int(8), *new_header = api.new_string(full_header);
    g_free(full_header);
    if (!new_channels || !new_header) {
        if (new_channels) api.put(new_channels);
        if (new_header) api.put(new_header);
        goto finish;
    }
    if (api.add(opus, "channels", new_channels) != 0) {
        api.put(new_channels); api.put(new_header); goto finish;
    }
    if (api.add(opus, "streamHeader", new_header) != 0) { api.put(new_header); goto finish; }
    result = g_strdup(api.serialize(root, 16)); /* PLAIN | NOSLASHESCAPE */
finish:
    api.put(root);
    return result;
}
