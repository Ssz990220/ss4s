/* Device-free factory/probe/lifecycle test. Both LG elements below are mocks. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <gst/gst.h>

extern int SS4S_Opus71Begin(const unsigned char *, size_t);
extern void SS4S_Opus71End(void);

static int decoded_headers, sink_events, native_acmod = 21;
static unsigned char expected_header[29];
typedef struct { GstElement parent; GstPad *sink, *src; } TestDecoder;
typedef struct { GstElementClass parent; } TestDecoderClass;
G_DEFINE_TYPE(TestDecoder, test_decoder, GST_TYPE_ELEMENT)
typedef struct { GstElement parent; GstPad *sink; gboolean multich, bypass; } TestSink;
typedef struct { GstElementClass parent; } TestSinkClass;
G_DEFINE_TYPE(TestSink, test_sink, GST_TYPE_ELEMENT)
static GstStaticPadTemplate sink_template = GST_STATIC_PAD_TEMPLATE("sink", GST_PAD_SINK, GST_PAD_ALWAYS, GST_STATIC_CAPS_ANY);
static GstStaticPadTemplate src_template = GST_STATIC_PAD_TEMPLATE("src", GST_PAD_SRC, GST_PAD_ALWAYS, GST_STATIC_CAPS_ANY);

static gboolean decode_event(GstPad *pad, GstObject *parent, GstEvent *event) {
    (void)pad;
    TestDecoder *self = (TestDecoder *)parent;
    if (GST_EVENT_TYPE(event) == GST_EVENT_CAPS) {
        GstCaps *caps;
        gst_event_parse_caps(event, &caps);
        const GValue *headers = gst_structure_get_value(gst_caps_get_structure(caps, 0), "streamheader");
        assert(headers && gst_value_array_get_size(headers) == 1);
        GstBuffer *buffer = gst_value_get_buffer(gst_value_array_get_value(headers, 0));
        unsigned char bytes[29];
        assert(gst_buffer_get_size(buffer) == 29);
        assert(gst_buffer_extract(buffer, 0, bytes, 29) == 29 && memcmp(bytes, expected_header, 29) == 0);
        ++decoded_headers;
        GstCaps *raw = gst_caps_from_string("audio/x-raw,channels=(int)8,rate=(int)48000,format=S32LE,layout=interleaved,codec=(uint)17,acmod=(uint)21,out-channels=(int)-1");
        gst_structure_set(gst_caps_get_structure(raw, 0), "acmod", G_TYPE_UINT, (guint)native_acmod, NULL);
        gboolean result = gst_pad_push_event(self->src, gst_event_new_caps(raw));
        gst_caps_unref(raw);
        gst_event_unref(event);
        return result;
    }
    return gst_pad_push_event(self->src, event);
}

static gboolean sink_event(GstPad *pad, GstObject *parent, GstEvent *event) {
    (void)pad;
    if (GST_EVENT_TYPE(event) == GST_EVENT_CAPS) {
        TestSink *self = (TestSink *)parent;
        GstCaps *caps;
        gint output;
        gst_event_parse_caps(event, &caps);
        assert(gst_structure_get_int(gst_caps_get_structure(caps, 0), "out-channels", &output));
        if (native_acmod == 21) assert(output == 8 && !self->multich && !self->bypass);
        else assert(output == -1 && self->multich && self->bypass);
        ++sink_events;
    }
    gst_event_unref(event);
    return TRUE;
}

static void test_decoder_init(TestDecoder *self) {
    self->sink = gst_pad_new_from_static_template(&sink_template, "sink");
    self->src = gst_pad_new_from_static_template(&src_template, "src");
    gst_pad_set_event_function(self->sink, decode_event);
    gst_element_add_pad(GST_ELEMENT(self), self->sink);
    gst_element_add_pad(GST_ELEMENT(self), self->src);
}
static void test_decoder_class_init(TestDecoderClass *klass) {
    GstElementClass *element = GST_ELEMENT_CLASS(klass);
    gst_element_class_set_static_metadata(element, "Mock decoder", "Codec/Decoder/Audio", "No decoding hardware", "SS4S tests");
    gst_element_class_add_static_pad_template(element, &sink_template);
    gst_element_class_add_static_pad_template(element, &src_template);
}
static void sink_set(GObject *object, guint id, const GValue *value, GParamSpec *pspec) {
    (void)pspec;
    TestSink *self = (TestSink *)object;
    if (id == 1) self->multich = g_value_get_boolean(value);
    if (id == 2) self->bypass = g_value_get_boolean(value);
}
static void sink_get(GObject *object, guint id, GValue *value, GParamSpec *pspec) {
    (void)pspec;
    TestSink *self = (TestSink *)object;
    g_value_set_boolean(value, id == 1 ? self->multich : self->bypass);
}
static void test_sink_init(TestSink *self) {
    self->multich = self->bypass = TRUE;
    self->sink = gst_pad_new_from_static_template(&sink_template, "sink");
    gst_pad_set_event_function(self->sink, sink_event);
    gst_element_add_pad(GST_ELEMENT(self), self->sink);
}
static void test_sink_class_init(TestSinkClass *klass) {
    GObjectClass *object = G_OBJECT_CLASS(klass);
    object->set_property = sink_set;
    object->get_property = sink_get;
    g_object_class_install_property(object, 1, g_param_spec_boolean("pcm-multi-ch", "multi", "multi", TRUE, G_PARAM_READWRITE));
    g_object_class_install_property(object, 2, g_param_spec_boolean("pcm-bypass", "bypass", "bypass", TRUE, G_PARAM_READWRITE));
    GstElementClass *element = GST_ELEMENT_CLASS(klass);
    gst_element_class_set_static_metadata(element, "Mock sink", "Sink/Audio", "No sound device", "SS4S tests");
    gst_element_class_add_static_pad_template(element, &sink_template);
}

int main(int argc, char **argv) {
    gst_init(&argc, &argv);
    assert(gst_element_register(NULL, "lgaudiodec", GST_RANK_NONE, test_decoder_get_type()));
    assert(gst_element_register(NULL, "audiosink", GST_RANK_NONE, test_sink_get_type()));
    memcpy(expected_header, "OpusHead", 8);
    expected_header[8] = 1; expected_header[9] = 8;
    expected_header[12] = 0x80; expected_header[13] = 0xbb;
    expected_header[18] = 1; expected_header[19] = 5; expected_header[20] = 3;
    for (int i = 0; i < 8; ++i) expected_header[21 + i] = (unsigned char)i;
    TestDecoder *decoder = (TestDecoder *)gst_element_factory_make("lgaudiodec", NULL);
    TestSink *sink = (TestSink *)gst_element_factory_make("audiosink", NULL);
    assert(decoder && sink);
    GstPad *source = gst_pad_new("source", GST_PAD_SRC);
    assert(gst_pad_link(source, decoder->sink) == GST_PAD_LINK_OK);
    assert(gst_pad_link(decoder->src, sink->sink) == GST_PAD_LINK_OK);
    gst_pad_set_active(source, TRUE); gst_pad_set_active(decoder->sink, TRUE);
    gst_pad_set_active(decoder->src, TRUE); gst_pad_set_active(sink->sink, TRUE);
    assert(gst_pad_push_event(source, gst_event_new_stream_start("mock-opus71")));
    for (int session = 0; session < 2; ++session) {
        native_acmod = session ? 7 : 21;
        assert(SS4S_Opus71Begin(expected_header, 29) == 0);
        assert(SS4S_Opus71Begin(expected_header, 29) == -1);
        GstCaps *caps = gst_caps_from_string("audio/x-opus,channels=(int)8,rate=(int)48000");
        // Change a harmless value to force a fresh CAPS event for session two.
        gst_structure_set(gst_caps_get_structure(caps, 0), "mock-session", G_TYPE_INT, session, NULL);
        assert(gst_pad_push_event(source, gst_event_new_caps(caps)));
        gst_caps_unref(caps);
        assert(decoded_headers == session + 1 && sink_events == session + 1);
        SS4S_Opus71End();
        SS4S_Opus71End();
        assert(sink->multich && sink->bypass);
    }
    gst_pad_set_active(source, FALSE); gst_pad_set_active(decoder->sink, FALSE);
    gst_pad_set_active(decoder->src, FALSE); gst_pad_set_active(sink->sink, FALSE);
    gst_pad_unlink(source, decoder->sink); gst_pad_unlink(decoder->src, sink->sink);
    gst_object_unref(source); gst_object_unref(decoder); gst_object_unref(sink);
    puts("PASS: factory interception, complete OpusHead, native CAPS gate, property restoration, reconnect");
    return 0;
}
