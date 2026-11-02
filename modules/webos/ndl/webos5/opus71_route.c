/* App-private experiment, LGPL-3.0-or-later. No system/plugin patching.
 * NDL normally truncates OpusHead to its seven-byte prefix. Preserve the actual
 * negotiated multistream layout before LG's native decoder sees the CAPS.
 * Eight transport slots alone never qualify as 7.1: require native acmod=21.
 */
#define _GNU_SOURCE
#include <gst/gst.h>
#include <dlfcn.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "opus_config.h"

#if defined(__arm__) && !defined(__aarch64__)
/* Confirmed against the copied C5 decoder's DWARF (_GValue=24 bytes).
 * ARM EABI aligns its uint64/double union to eight bytes, unlike i386. */
_Static_assert(GLIB_SIZEOF_VOID_P == 4 && sizeof(gpointer) == 4 && sizeof(GType) == 4 && sizeof(GValue) == 24,
               "The route shim must use the ARM32 GLib ABI, not host headers");
#endif

static GMutex lock;
static gboolean active;
static unsigned char header[29];
static size_t header_size;
static unsigned generation;
typedef struct {
    GWeakRef element;
    gboolean multich, bypass;
    gboolean has_multich, has_bypass;
} SinkState;
static GPtrArray *sinks;

static void notify(char kind) {
    const char *value = getenv("SS4S_OPUS71_NOTIFY_FD");
    if (!value) return;
    char *end;
    long fd = strtol(value, &end, 10);
    if (*end || fd < 3 || fd > 65535) return;
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    char line[96];
    int n = g_snprintf(line, sizeof(line), "%c %u %llu\n", kind, generation,
                      (unsigned long long)now.tv_sec * 1000 + now.tv_nsec / 1000000);
    // Parent keeps its read end open for the child's entire lifetime.
    (void)write((int)fd, line, (size_t)n);
}

int SS4S_Opus71Begin(const unsigned char *data, size_t size) {
    OpusConfig config;
    if (size != sizeof(header) || !SS4S_ParseOpusConfig(data, size, &config) || config.channels != 8) return -1;
    g_mutex_lock(&lock);
    if (active) { g_mutex_unlock(&lock); return -1; }
    memcpy(header, data, size);
    header_size = size;
    ++generation;
    sinks = g_ptr_array_new();
    active = TRUE;
    notify('B');
    g_mutex_unlock(&lock);
    return 0;
}

void SS4S_Opus71End(void) {
    g_mutex_lock(&lock);
    if (!active) { g_mutex_unlock(&lock); return; }
    active = FALSE;
    notify('E');
    GPtrArray *owned = sinks;
    sinks = NULL;
    header_size = 0;
    g_mutex_unlock(&lock);
    for (guint i = 0; i < owned->len; ++i) {
        SinkState *state = g_ptr_array_index(owned, i);
        GObject *element = g_weak_ref_get(&state->element);
        if (element) {
            gboolean value;
            if (state->has_multich) {
                g_object_get(element, "pcm-multi-ch", &value, NULL);
                if (!value) g_object_set(element, "pcm-multi-ch", state->multich, NULL);
            }
            if (state->has_bypass) {
                g_object_get(element, "pcm-bypass", &value, NULL);
                if (!value) g_object_set(element, "pcm-bypass", state->bypass, NULL);
            }
            g_object_unref(element);
        }
        g_weak_ref_clear(&state->element);
        g_free(state);
    }
    g_ptr_array_free(owned, TRUE);
}

GstCaps *SS4S_Opus71InputCaps(const GstCaps *caps, const unsigned char *data, size_t size) {
    OpusConfig config;
    if (!caps || !gst_caps_is_fixed(caps) || gst_caps_get_size(caps) != 1 ||
        !SS4S_ParseOpusConfig(data, size, &config) || config.channels != 8) return NULL;
    const GstStructure *s = gst_caps_get_structure(caps, 0);
    gint channels, rate;
    if (!gst_structure_has_name(s, "audio/x-opus") ||
        !gst_structure_get_int(s, "channels", &channels) || channels != 8 ||
        !gst_structure_get_int(s, "rate", &rate) || rate != 48000) return NULL;
    GstCaps *next = gst_caps_copy(caps);
    GstStructure *out = gst_caps_get_structure(next, 0);
    GstBuffer *buffer = gst_buffer_new_allocate(NULL, size, NULL);
    gst_buffer_fill(buffer, 0, data, size);
    GValue array = G_VALUE_INIT, item = G_VALUE_INIT;
    g_value_init(&array, GST_TYPE_ARRAY);
    g_value_init(&item, GST_TYPE_BUFFER);
    gst_value_set_buffer(&item, buffer);
    gst_value_array_append_value(&array, &item);
    gst_structure_set_value(out, "streamheader", &array);
    g_value_unset(&item);
    g_value_unset(&array);
    gst_buffer_unref(buffer);
    gst_structure_set(out, "channel-mapping-family", G_TYPE_INT, 1,
                      "stream-count", G_TYPE_INT, (gint)config.streamCount,
                      "coupled-count", G_TYPE_INT, (gint)config.coupledCount, NULL);
    g_value_init(&array, GST_TYPE_ARRAY);
    g_value_init(&item, G_TYPE_INT);
    for (guint i = 0; i < config.channels; ++i) {
        g_value_set_int(&item, config.mapping[i]);
        gst_value_array_append_value(&array, &item);
    }
    gst_structure_set_value(out, "channel-mapping", &array);
    g_value_unset(&item);
    g_value_unset(&array);
    return next;
}

gboolean SS4S_Opus71NativeCaps(const GstCaps *caps) {
    if (!caps || !gst_caps_is_fixed(caps) || gst_caps_get_size(caps) != 1) return FALSE;
    const GstStructure *s = gst_caps_get_structure(caps, 0);
    gint channels, rate;
    guint codec, acmod;
    const char *format = gst_structure_get_string(s, "format");
    return gst_structure_has_name(s, "audio/x-raw") &&
        !gst_structure_has_field(s, "DUMMY") && !gst_structure_has_field(s, "BYPASS") &&
        gst_structure_get_int(s, "channels", &channels) && channels == 8 &&
        gst_structure_get_int(s, "rate", &rate) && rate == 48000 &&
        gst_structure_get_uint(s, "codec", &codec) && codec == 17 &&
        gst_structure_get_uint(s, "acmod", &acmod) && acmod == 21 &&
        g_strcmp0(gst_structure_get_string(s, "layout"), "interleaved") == 0 &&
        (g_strcmp0(format, "S16LE") == 0 || g_strcmp0(format, "S24LE") == 0 || g_strcmp0(format, "S32LE") == 0);
}

static gboolean bool_property(GObject *object, const char *name) {
    GParamSpec *p = g_object_class_find_property(G_OBJECT_GET_CLASS(object), name);
    return p && G_PARAM_SPEC_VALUE_TYPE(p) == G_TYPE_BOOLEAN &&
        (p->flags & G_PARAM_READWRITE) == G_PARAM_READWRITE && !(p->flags & G_PARAM_CONSTRUCT_ONLY);
}

static GstPadProbeReturn caps_probe(GstPad *pad, GstPadProbeInfo *info, gpointer mode) {
    GstEvent *event = GST_PAD_PROBE_INFO_EVENT(info);
    if (GST_EVENT_TYPE(event) != GST_EVENT_CAPS) return GST_PAD_PROBE_OK;
    GstCaps *caps, *next = NULL;
    gst_event_parse_caps(event, &caps);
    g_mutex_lock(&lock);
    if (active && GPOINTER_TO_INT(mode) == 1) {
        next = SS4S_Opus71InputCaps(caps, header, header_size);
    } else if (active && SS4S_Opus71NativeCaps(caps)) {
        GstElement *element = gst_pad_get_parent_element(pad);
        if (element) {
            gboolean seen = FALSE;
            for (guint i = 0; i < sinks->len; ++i) {
                SinkState *entry = g_ptr_array_index(sinks, i);
                GObject *previous = g_weak_ref_get(&entry->element);
                seen |= previous == G_OBJECT(element);
                if (previous) g_object_unref(previous);
            }
            if (!seen) {
                SinkState *state = g_new0(SinkState, 1);
                g_weak_ref_init(&state->element, element);
                state->has_multich = bool_property(G_OBJECT(element), "pcm-multi-ch");
                state->has_bypass = bool_property(G_OBJECT(element), "pcm-bypass");
                if (state->has_multich) {
                    g_object_get(element, "pcm-multi-ch", &state->multich, NULL);
                    g_object_set(element, "pcm-multi-ch", FALSE, NULL);
                }
                if (state->has_bypass) {
                    g_object_get(element, "pcm-bypass", &state->bypass, NULL);
                    g_object_set(element, "pcm-bypass", FALSE, NULL);
                }
                g_ptr_array_add(sinks, state);
            }
            gst_object_unref(element);
        }
        next = gst_caps_copy(caps);
        gst_structure_set(gst_caps_get_structure(next, 0), "out-channels", G_TYPE_INT, 8,
                          "lfe-enable", G_TYPE_BOOLEAN, TRUE, NULL);
        notify('F');
    } else if (active && GPOINTER_TO_INT(mode) == 2 && gst_caps_get_size(caps) == 1) {
        const GstStructure *s = gst_caps_get_structure(caps, 0);
        if (gst_structure_has_name(s, "audio/x-raw") &&
            !gst_structure_has_field(s, "DUMMY") && !gst_structure_has_field(s, "BYPASS")) {
            // A later real stereo/5.1 format revokes an existing LPCM8 lease.
            notify('R');
        }
    }
    g_mutex_unlock(&lock);
    if (next) {
        GstEvent *replacement = gst_event_new_caps(next);
        gst_event_set_seqnum(replacement, gst_event_get_seqnum(event));
        GST_PAD_PROBE_INFO_DATA(info) = replacement;
        gst_event_unref(event);
        gst_caps_unref(next);
    }
    return GST_PAD_PROBE_OK;
}

static void attach(GstElement *element) {
    if (!element) return;
    GstElementFactory *factory = gst_element_get_factory(element);
    if (!factory) return;
    const gchar *name = gst_plugin_feature_get_name(GST_PLUGIN_FEATURE(factory));
    int mode = g_strcmp0(name, "lgaudiodec") == 0 ? 1 : g_strcmp0(name, "audiosink") == 0 ? 2 : 0;
    if (!mode) return;
    GQuark key = g_quark_from_static_string("ss4s-opus71-probe");
    if (g_object_get_qdata(G_OBJECT(element), key)) return;
    g_object_set_qdata(G_OBJECT(element), key, GINT_TO_POINTER(1));
    GstPad *pad = gst_element_get_static_pad(element, "sink");
    if (pad) {
        gst_pad_add_probe(pad, GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM, caps_probe, GINT_TO_POINTER(mode), NULL);
        gst_object_unref(pad);
    }
}

static pthread_once_t resolve_once = PTHREAD_ONCE_INIT;
static GstElement *(*real_make)(const gchar *, const gchar *);
static GstElement *(*real_create)(GstElementFactory *, const gchar *);
static void resolve(void) {
    real_make = dlsym(RTLD_NEXT, "gst_element_factory_make");
    real_create = dlsym(RTLD_NEXT, "gst_element_factory_create");
}
GstElement *gst_element_factory_make(const gchar *factory, const gchar *name) {
    pthread_once(&resolve_once, resolve);
    GstElement *element = real_make ? real_make(factory, name) : NULL;
    attach(element);
    return element;
}
GstElement *gst_element_factory_create(GstElementFactory *factory, const gchar *name) {
    pthread_once(&resolve_once, resolve);
    GstElement *element = real_create ? real_create(factory, name) : NULL;
    attach(element);
    return element;
}
