#define _GNU_SOURCE
#include <gio/gio.h>
#include <gio/gunixfdlist.h>
#include <glib-unix.h>
#include <gst/gst.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define PORTAL_NAME      "org.freedesktop.portal.Desktop"
#define PORTAL_PATH      "/org/freedesktop/portal/desktop"
#define SCREENCAST_IFACE "org.freedesktop.portal.ScreenCast"
#define REQUEST_IFACE    "org.freedesktop.portal.Request"
#define SESSION_IFACE    "org.freedesktop.portal.Session"
#define APP_VERSION      "0.2.0"

typedef enum {
    CAPTURE_BACKEND_AUTO,
    CAPTURE_BACKEND_X11,
    CAPTURE_BACKEND_PORTAL
} CaptureBackend;

typedef struct {
    GMainLoop *loop;
    gboolean received;
    guint response;
    GVariant *results;
} PortalResponse;

typedef struct {
    GMainLoop *loop;
    GstElement *pipeline;
    gboolean stopping;
    gboolean failed;
} Recorder;

static guint request_serial = 0;

static void print_error(const gchar *where, const GError *error)
{
    g_printerr("lamecap: %s: %s\n", where,
               error ? error->message : "unknown error");
}

static GVariant *options_with_token(const gchar *token,
                                    gboolean include_types,
                                    guint source_types)
{
    GVariantBuilder builder;
    g_variant_builder_init(&builder, G_VARIANT_TYPE_VARDICT);
    g_variant_builder_add(&builder, "{sv}", "handle_token",
                          g_variant_new_string(token));
    if (include_types)
        g_variant_builder_add(&builder, "{sv}", "types",
                              g_variant_new_uint32(source_types));
    return g_variant_builder_end(&builder);
}

static gchar *expected_request_path(GDBusConnection *connection,
                                    const gchar *token)
{
    const gchar *unique = g_dbus_connection_get_unique_name(connection);
    if (!unique || unique[0] != ':')
        return NULL;
    gchar *sender = g_strdup(unique + 1);
    g_strdelimit(sender, ".", '_');
    gchar *path = g_strdup_printf(
        "/org/freedesktop/portal/desktop/request/%s/%s", sender, token);
    g_free(sender);
    return path;
}

static void portal_response_signal(GDBusConnection *connection,
                                   const gchar *sender_name,
                                   const gchar *object_path,
                                   const gchar *interface_name,
                                   const gchar *signal_name,
                                   GVariant *parameters,
                                   gpointer user_data)
{
    (void)connection; (void)sender_name; (void)object_path;
    (void)interface_name; (void)signal_name;
    PortalResponse *state = user_data;
    guint response = 2;
    GVariant *results = NULL;
    g_variant_get(parameters, "(u@a{sv})", &response, &results);
    state->received = TRUE;
    state->response = response;
    if (state->results)
        g_variant_unref(state->results);
    state->results = results;
    if (state->loop && g_main_loop_is_running(state->loop))
        g_main_loop_quit(state->loop);
}

/* Calls a ScreenCast portal method which returns a Request object path, then
 * waits for that request object's Response signal. This deliberately uses
 * D-Bus directly; it does not spawn gdbus or any other helper process. */
static gboolean portal_request(GDBusConnection *connection,
                               const gchar *method,
                               GVariant *parameters,
                               const gchar *token,
                               GVariant **results_out,
                               GError **error)
{
    gboolean ok = FALSE;
    gchar *expected_path = expected_request_path(connection, token);
    if (!expected_path) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                            "Could not determine D-Bus request path");
        return FALSE;
    }

    PortalResponse state = {0};
    state.loop = g_main_loop_new(NULL, FALSE);
    guint subscription = g_dbus_connection_signal_subscribe(
        connection, PORTAL_NAME, REQUEST_IFACE, "Response", expected_path,
        NULL, G_DBUS_SIGNAL_FLAGS_NONE, portal_response_signal, &state, NULL);

    GVariant *reply = g_dbus_connection_call_sync(
        connection, PORTAL_NAME, PORTAL_PATH, SCREENCAST_IFACE, method,
        parameters, G_VARIANT_TYPE("(o)"), G_DBUS_CALL_FLAGS_NONE, 30000,
        NULL, error);
    if (!reply)
        goto cleanup;

    gchar *returned_path = NULL;
    g_variant_get(reply, "(o)", &returned_path);
    g_variant_unref(reply);
    if (g_strcmp0(returned_path, expected_path) != 0) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                    "Portal returned unexpected request path: %s",
                    returned_path ? returned_path : "(null)");
        g_free(returned_path);
        goto cleanup;
    }
    g_free(returned_path);

    if (!state.received)
        g_main_loop_run(state.loop);
    if (state.response != 0) {
        if (state.response == 1)
            g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_CANCELLED,
                                "Screen-capture request was cancelled");
        else
            g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                        "Screen-capture portal returned response code %u",
                        state.response);
        goto cleanup;
    }
    if (results_out)
        *results_out = state.results ? g_variant_ref(state.results) : NULL;
    ok = TRUE;

cleanup:
    g_dbus_connection_signal_unsubscribe(connection, subscription);
    if (state.results)
        g_variant_unref(state.results);
    g_main_loop_unref(state.loop);
    g_free(expected_path);
    return ok;
}

static gboolean portal_close_session(GDBusConnection *connection,
                                     const gchar *session_path)
{
    if (!session_path)
        return TRUE;
    GError *error = NULL;
    GVariant *reply = g_dbus_connection_call_sync(
        connection, PORTAL_NAME, session_path, SESSION_IFACE, "Close", NULL,
        G_VARIANT_TYPE("()"), G_DBUS_CALL_FLAGS_NONE, 5000, NULL, &error);
    if (!reply) {
        print_error("closing screen-capture session", error);
        g_clear_error(&error);
        return FALSE;
    }
    g_variant_unref(reply);
    return TRUE;
}

static gint open_portal_pipewire_remote(GDBusConnection *connection,
                                        const gchar *session_path,
                                        GError **error)
{
    GVariantBuilder builder;
    g_variant_builder_init(&builder, G_VARIANT_TYPE_VARDICT);
    GVariant *options = g_variant_builder_end(&builder);
    GUnixFDList *returned_fds = NULL;
    GVariant *reply = g_dbus_connection_call_with_unix_fd_list_sync(
        connection, PORTAL_NAME, PORTAL_PATH, SCREENCAST_IFACE,
        "OpenPipeWireRemote", g_variant_new("(o@a{sv})", session_path, options),
        G_VARIANT_TYPE("(h)"), G_DBUS_CALL_FLAGS_NONE, 10000, NULL,
        &returned_fds, NULL, error);
    if (!reply)
        return -1;

    gint handle_index = -1;
    g_variant_get(reply, "(h)", &handle_index);
    g_variant_unref(reply);
    if (!returned_fds) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                            "Portal did not return a PipeWire file descriptor");
        return -1;
    }
    gint fd = g_unix_fd_list_get(returned_fds, handle_index, error);
    g_object_unref(returned_fds);
    return fd;
}

static gboolean element_factory_exists(const gchar *name)
{
    GstElementFactory *factory = gst_element_factory_find(name);
    if (!factory)
        return FALSE;
    gst_object_unref(factory);
    return TRUE;
}

static gboolean choose_backend(const gchar *requested,
                               CaptureBackend *selected,
                               GError **error)
{
    const gchar *session_type = g_getenv("XDG_SESSION_TYPE");
    const gchar *wayland_display = g_getenv("WAYLAND_DISPLAY");
    const gchar *x_display = g_getenv("DISPLAY");

    if (g_strcmp0(requested, "auto") == 0) {
        if (g_strcmp0(session_type, "x11") == 0) {
            *selected = CAPTURE_BACKEND_X11;
        } else if (g_strcmp0(session_type, "wayland") == 0 ||
                   (wayland_display && *wayland_display)) {
            *selected = CAPTURE_BACKEND_PORTAL;
        } else if (x_display && *x_display) {
            *selected = CAPTURE_BACKEND_X11;
        } else {
            g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                "Could not detect X11 or Wayland. Set DISPLAY/WAYLAND_DISPLAY or pass --backend explicitly.");
            return FALSE;
        }
    } else if (g_strcmp0(requested, "x11") == 0) {
        *selected = CAPTURE_BACKEND_X11;
    } else if (g_strcmp0(requested, "portal") == 0) {
        *selected = CAPTURE_BACKEND_PORTAL;
    } else {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                    "Unknown backend '%s' (use auto, x11, or portal)",
                    requested ? requested : "(null)");
        return FALSE;
    }

    if (*selected == CAPTURE_BACKEND_X11 && (!x_display || !*x_display)) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
            "The X11 backend requires DISPLAY to be set. Use --backend portal on Wayland.");
        return FALSE;
    }
    return TRUE;
}

static GstElement *make_first_available(const gchar **names,
                                        const gchar *instance_name,
                                        const gchar **chosen_name)
{
    for (guint i = 0; names[i]; ++i) {
        GstElement *element = gst_element_factory_make(names[i], instance_name);
        if (element) {
            if (chosen_name)
                *chosen_name = names[i];
            return element;
        }
    }
    return NULL;
}

static gboolean set_optional_boolean(GObject *object, const gchar *name,
                                     gboolean value)
{
    GParamSpec *pspec = g_object_class_find_property(G_OBJECT_GET_CLASS(object), name);
    if (!pspec || !(pspec->flags & G_PARAM_WRITABLE))
        return FALSE;
    g_object_set(object, name, value, NULL);
    return TRUE;
}

static void configure_video_encoder(GstElement *encoder,
                                    const gchar *factory_name)
{
    if (g_strcmp0(factory_name, "x264enc") == 0) {
        g_object_set(encoder, "bitrate", 5000, "key-int-max", 60, NULL);
        /* Real-time-friendly settings; gst_util resolves enum/flag nicknames. */
        gst_util_set_object_arg(G_OBJECT(encoder), "speed-preset", "ultrafast");
        gst_util_set_object_arg(G_OBJECT(encoder), "tune", "zerolatency");
    } else if (g_strcmp0(factory_name, "openh264enc") == 0) {
        g_object_set(encoder, "bitrate", 5000000, NULL);
    } else if (g_strcmp0(factory_name, "avenc_h264") == 0) {
        g_object_set(encoder, "bitrate", (gint64)5000000, NULL);
    }
}

static GstElement *build_pipeline(CaptureBackend backend,
                                  gint pipewire_fd,
                                  const gchar *target_object,
                                  const gchar *output_path,
                                  const gchar *audio_monitor,
                                  GError **error)
{
    static const gchar *video_encoders[] = { "x264enc", "openh264enc",
                                              "avenc_h264", NULL };
    static const gchar *audio_encoders[] = { "avenc_aac", "voaacenc", NULL };
    const gchar *video_encoder_name = NULL;
    const gchar *audio_encoder_name = NULL;
    const gchar *source_factory = backend == CAPTURE_BACKEND_X11
        ? "ximagesrc" : "pipewiresrc";

    GstElement *pipeline = gst_pipeline_new("lamecap-recording");
    GstElement *src = gst_element_factory_make(source_factory, "screen-source");
    GstElement *vqueue = gst_element_factory_make("queue", "video-queue");
    GstElement *convert = gst_element_factory_make("videoconvert", "video-convert");
    GstElement *rate = gst_element_factory_make("videorate", "video-rate");
    GstElement *capsfilter = gst_element_factory_make("capsfilter", "video-caps");
    GstElement *encoder = make_first_available(video_encoders, "video-encoder",
                                                &video_encoder_name);
    GstElement *parser = gst_element_factory_make("h264parse", "h264-parser");
    GstElement *encoded_queue = gst_element_factory_make("queue", "encoded-video-queue");
    GstElement *mux = gst_element_factory_make("mp4mux", "mp4-mux");
    GstElement *sink = gst_element_factory_make("filesink", "output-file");

    if (!pipeline || !src || !vqueue || !convert || !rate || !capsfilter || !encoder ||
        !parser || !encoded_queue || !mux || !sink) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
            "Missing a required GStreamer element for the %s backend or MP4 encoding. "
            "Check that the GStreamer X11/PipeWire, base, good, bad, ugly and libav plugins are installed.",
            backend == CAPTURE_BACKEND_X11 ? "X11" : "portal/PipeWire");
        goto fail;
    }

    if (backend == CAPTURE_BACKEND_X11) {
        /* ximagesrc captures the full X11 root window (the virtual desktop). */
        set_optional_boolean(G_OBJECT(src), "use-damage", FALSE);
        set_optional_boolean(G_OBJECT(src), "do-timestamp", TRUE);
    } else {
        if (pipewire_fd < 0 || !target_object || !*target_object) {
            g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                "The portal backend requires a PipeWire file descriptor and target object.");
            goto fail;
        }
        g_object_set(src, "fd", pipewire_fd, "target-object", target_object,
                     "do-timestamp", TRUE, NULL);
    }

    g_object_set(sink, "location", output_path, "sync", FALSE, NULL);
    GstCaps *caps = gst_caps_from_string("video/x-raw,format=I420,framerate=30/1");
    g_object_set(capsfilter, "caps", caps, NULL);
    gst_caps_unref(caps);
    configure_video_encoder(encoder, video_encoder_name);
    set_optional_boolean(G_OBJECT(mux), "faststart", TRUE);

    gst_bin_add_many(GST_BIN(pipeline), src, vqueue, convert, rate, capsfilter,
                     encoder, parser, encoded_queue, mux, sink, NULL);
    if (!gst_element_link_many(src, vqueue, convert, rate, capsfilter, encoder,
                               parser, encoded_queue, mux, NULL) ||
        !gst_element_link(mux, sink)) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                            "Could not link the screen-to-MP4 GStreamer pipeline");
        goto fail_pipeline;
    }

    if (audio_monitor && *audio_monitor) {
        GstElement *audio_src = gst_element_factory_make("pulsesrc", "system-audio-source");
        GstElement *aqueue = gst_element_factory_make("queue", "audio-queue");
        GstElement *aconvert = gst_element_factory_make("audioconvert", "audio-convert");
        GstElement *aresample = gst_element_factory_make("audioresample", "audio-resample");
        GstElement *aencoder = make_first_available(audio_encoders, "audio-encoder",
                                                     &audio_encoder_name);
        GstElement *aparser = gst_element_factory_make("aacparse", "aac-parser");
        GstElement *aencoded_queue = gst_element_factory_make("queue", "encoded-audio-queue");
        if (!audio_src || !aqueue || !aconvert || !aresample || !aencoder ||
            !aparser || !aencoded_queue) {
            g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                "Audio requested, but PulseAudio capture or an AAC encoder/parser is unavailable");
            if (audio_src) gst_object_unref(audio_src);
            if (aqueue) gst_object_unref(aqueue);
            if (aconvert) gst_object_unref(aconvert);
            if (aresample) gst_object_unref(aresample);
            if (aencoder) gst_object_unref(aencoder);
            if (aparser) gst_object_unref(aparser);
            if (aencoded_queue) gst_object_unref(aencoded_queue);
            goto fail_pipeline;
        }
        g_object_set(audio_src, "device", audio_monitor, "do-timestamp", TRUE, NULL);
        if (g_strcmp0(audio_encoder_name, "avenc_aac") == 0)
            g_object_set(aencoder, "bitrate", (gint64)192000, NULL);
        else if (g_strcmp0(audio_encoder_name, "voaacenc") == 0)
            g_object_set(aencoder, "bitrate", 128000, NULL);

        gst_bin_add_many(GST_BIN(pipeline), audio_src, aqueue, aconvert,
                         aresample, aencoder, aparser, aencoded_queue, NULL);
        if (!gst_element_link_many(audio_src, aqueue, aconvert, aresample,
                                   aencoder, aparser, aencoded_queue, mux, NULL)) {
            g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                                "Could not link the requested audio monitor into the MP4 muxer");
            goto fail_pipeline;
        }
    }

    g_print("lamecap: capture backend: %s\n",
            backend == CAPTURE_BACKEND_X11 ? "X11 direct capture" : "desktop portal / PipeWire");
    g_print("lamecap: video encoder: %s\n", video_encoder_name);
    if (audio_monitor && *audio_monitor)
        g_print("lamecap: audio monitor: %s (AAC: %s)\n", audio_monitor,
                audio_encoder_name ? audio_encoder_name : "unknown");
    return pipeline;

fail_pipeline:
    gst_object_unref(pipeline);
    return NULL;
fail:
    /* Elements have not been added to the pipeline yet. */
    if (pipeline) gst_object_unref(pipeline);
    if (src) gst_object_unref(src);
    if (vqueue) gst_object_unref(vqueue);
    if (convert) gst_object_unref(convert);
    if (rate) gst_object_unref(rate);
    if (capsfilter) gst_object_unref(capsfilter);
    if (encoder) gst_object_unref(encoder);
    if (parser) gst_object_unref(parser);
    if (encoded_queue) gst_object_unref(encoded_queue);
    if (mux) gst_object_unref(mux);
    if (sink) gst_object_unref(sink);
    return NULL;
}

static gboolean bus_message(GstBus *bus, GstMessage *message, gpointer user_data)
{
    (void)bus;
    Recorder *recorder = user_data;
    switch (GST_MESSAGE_TYPE(message)) {
    case GST_MESSAGE_ERROR: {
        GError *error = NULL;
        gchar *debug = NULL;
        gst_message_parse_error(message, &error, &debug);
        print_error("GStreamer recording error", error);
        if (debug && *debug)
            g_printerr("lamecap: debug: %s\n", debug);
        g_clear_error(&error);
        g_free(debug);
        recorder->failed = TRUE;
        g_main_loop_quit(recorder->loop);
        break;
    }
    case GST_MESSAGE_EOS:
        g_print("\nlamecap: MP4 finalized.\n");
        g_main_loop_quit(recorder->loop);
        break;
    default:
        break;
    }
    return G_SOURCE_CONTINUE;
}

static gboolean stop_recording(gpointer user_data)
{
    Recorder *recorder = user_data;
    if (recorder->stopping) {
        g_printerr("lamecap: second interrupt; exiting immediately\n");
        recorder->failed = TRUE;
        g_main_loop_quit(recorder->loop);
        return G_SOURCE_REMOVE;
    }
    recorder->stopping = TRUE;
    g_print("\nlamecap: stopping cleanly; finalizing MP4...\n");
    if (!gst_element_send_event(recorder->pipeline, gst_event_new_eos())) {
        g_printerr("lamecap: warning: could not send EOS to the pipeline\n");
        recorder->failed = TRUE;
        g_main_loop_quit(recorder->loop);
    }
    return G_SOURCE_CONTINUE;
}

static void usage(FILE *stream)
{
    fprintf(stream,
        "lamecap %s - command-line screen recorder\n\n"
        "Usage: lamecap [options]\n\n"
        "Options:\n"
        "  -o, --output FILE       Output MP4 path (default: timestamped file)\n"
        "      --backend MODE      auto, x11, or portal (default: auto)\n"
        "      --source MODE       Portal source: monitor, window, or either\n"
        "      --audio-monitor ID  Capture a PulseAudio/PipeWire monitor source\n"
        "      --help              Show this help\n"
        "      --version           Show version\n\n"
        "On X11, auto mode captures the full desktop directly, without a portal dialog.\n"
        "On Wayland, auto mode uses the desktop ScreenCast portal and PipeWire.\n"
        "Press Ctrl+C once to stop and finalize the MP4. Audio is opt-in.\n",
        APP_VERSION);
}

int main(int argc, char **argv)
{
    const gchar *output_arg = NULL;
    const gchar *audio_monitor = NULL;
    const gchar *source_mode = "either";
    const gchar *backend_arg = "auto";
    const gchar *target_object = NULL;
    gchar *default_output = NULL;
    gchar *session_path = NULL;
    GDBusConnection *connection = NULL;
    gint pipewire_fd = -1;
    GstElement *pipeline = NULL;
    gint exit_status = 1;
    GError *error = NULL;
    CaptureBackend backend = CAPTURE_BACKEND_AUTO;
    gchar serial_text[32] = {0};

    for (int i = 1; i < argc; ++i) {
        if ((strcmp(argv[i], "-o") == 0 || strcmp(argv[i], "--output") == 0) && i + 1 < argc)
            output_arg = argv[++i];
        else if (strcmp(argv[i], "--audio-monitor") == 0 && i + 1 < argc)
            audio_monitor = argv[++i];
        else if (strcmp(argv[i], "--source") == 0 && i + 1 < argc)
            source_mode = argv[++i];
        else if (strcmp(argv[i], "--backend") == 0 && i + 1 < argc)
            backend_arg = argv[++i];
        else if (strcmp(argv[i], "--help") == 0) { usage(stdout); return 0; }
        else if (strcmp(argv[i], "--version") == 0) { puts("lamecap " APP_VERSION); return 0; }
        else {
            g_printerr("lamecap: unknown option or missing value: %s\n", argv[i]);
            usage(stderr);
            return 2;
        }
    }

    guint source_types = 3; /* portal: MONITOR | WINDOW */
    if (strcmp(source_mode, "monitor") == 0) source_types = 1;
    else if (strcmp(source_mode, "window") == 0) source_types = 2;
    else if (strcmp(source_mode, "either") != 0) {
        g_printerr("lamecap: --source must be monitor, window, or either\n");
        return 2;
    }

    if (output_arg && !*output_arg) {
        g_printerr("lamecap: output path cannot be empty\n");
        return 2;
    }
    if (!output_arg) {
        GDateTime *now = g_date_time_new_now_local();
        gchar *stamp = g_date_time_format(now, "%Y%m%d-%H%M%S");
        default_output = g_strdup_printf("lamecap-%s.mp4", stamp);
        output_arg = default_output;
        g_free(stamp);
        g_date_time_unref(now);
    }

    gst_init(NULL, NULL);
    if (!choose_backend(backend_arg, &backend, &error)) {
        print_error("selecting capture backend", error);
        g_clear_error(&error);
        goto cleanup;
    }

    if (backend == CAPTURE_BACKEND_X11) {
        if (!element_factory_exists("ximagesrc")) {
            g_printerr("lamecap: GStreamer element 'ximagesrc' is missing. Install the GStreamer X11 plugin (often package gstreamer1.0-x).\n");
            goto cleanup;
        }
        if (strcmp(source_mode, "either") != 0) {
            g_printerr("lamecap: --source applies to the portal backend; use --backend portal to select a monitor or window.\n");
            exit_status = 2;
            goto cleanup;
        }
    } else {
        if (!element_factory_exists("pipewiresrc")) {
            g_printerr("lamecap: GStreamer element 'pipewiresrc' is missing. Install the GStreamer PipeWire plugin.\n");
            goto cleanup;
        }

        connection = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &error);
        if (!connection) {
            print_error("connecting to the desktop session bus", error);
            g_clear_error(&error);
            goto cleanup;
        }

        gchar token[64];
        g_snprintf(token, sizeof(token), "lamecap_create_%u", ++request_serial);
        GVariant *results = NULL;
        if (!portal_request(connection, "CreateSession",
                            g_variant_new("(@a{sv})", options_with_token(token, FALSE, 0)),
                            token, &results, &error)) {
            print_error("creating screen-capture session", error);
            g_clear_error(&error);
            goto cleanup;
        }
        if (!results || !g_variant_lookup(results, "session_handle", "o", &session_path)) {
            g_printerr("lamecap: portal did not return a session handle\n");
            if (results) g_variant_unref(results);
            goto cleanup;
        }
        g_variant_unref(results);

        g_snprintf(token, sizeof(token), "lamecap_select_%u", ++request_serial);
        if (!portal_request(connection, "SelectSources",
                            g_variant_new("(o@a{sv})", session_path,
                                          options_with_token(token, TRUE, source_types)),
                            token, NULL, &error)) {
            print_error("selecting capture source", error);
            g_clear_error(&error);
            goto cleanup;
        }

        g_snprintf(token, sizeof(token), "lamecap_start_%u", ++request_serial);
        if (!portal_request(connection, "Start",
                            g_variant_new("(os@a{sv})", session_path, "",
                                          options_with_token(token, FALSE, 0)),
                            token, &results, &error)) {
            print_error("starting screen capture", error);
            g_clear_error(&error);
            goto cleanup;
        }

        GVariant *streams = NULL;
        guint32 node_id = 0;
        guint64 serial = 0;
        if (!results || !g_variant_lookup(results, "streams", "@a(ua{sv})", &streams) ||
            g_variant_n_children(streams) == 0) {
            g_printerr("lamecap: portal started, but returned no screen-capture stream\n");
            if (streams) g_variant_unref(streams);
            if (results) g_variant_unref(results);
            goto cleanup;
        }
        GVariant *stream_props = NULL;
        g_variant_get_child(streams, 0, "(u@a{sv})", &node_id, &stream_props);
        if (!g_variant_lookup(stream_props, "pipewire-serial", "t", &serial))
            serial = (guint64)node_id;
        g_variant_unref(stream_props);
        g_variant_unref(streams);
        g_variant_unref(results);

        pipewire_fd = open_portal_pipewire_remote(connection, session_path, &error);
        if (pipewire_fd < 0) {
            print_error("opening the portal PipeWire connection", error);
            g_clear_error(&error);
            goto cleanup;
        }
        g_snprintf(serial_text, sizeof(serial_text), "%" G_GUINT64_FORMAT, serial);
        target_object = serial_text;
    }

    pipeline = build_pipeline(backend, pipewire_fd, target_object,
                              output_arg, audio_monitor, &error);
    if (!pipeline) {
        print_error("building recording pipeline", error);
        g_clear_error(&error);
        goto cleanup;
    }

    Recorder recorder = {0};
    recorder.loop = g_main_loop_new(NULL, FALSE);
    recorder.pipeline = pipeline;
    GstBus *bus = gst_element_get_bus(pipeline);
    gst_bus_add_watch(bus, bus_message, &recorder);
    gst_object_unref(bus);
    g_unix_signal_add(SIGINT, stop_recording, &recorder);
    g_unix_signal_add(SIGTERM, stop_recording, &recorder);

    GstStateChangeReturn state_change = gst_element_set_state(pipeline, GST_STATE_PLAYING);
    if (state_change == GST_STATE_CHANGE_FAILURE) {
        g_printerr("lamecap: failed to start recording pipeline\n");
        recorder.failed = TRUE;
    } else {
        g_print("lamecap: recording to %s\n", output_arg);
        g_print("lamecap: Ctrl+C once to stop and finalize the MP4\n");
        g_main_loop_run(recorder.loop);
    }

    gst_element_set_state(pipeline, GST_STATE_NULL);
    g_main_loop_unref(recorder.loop);
    if (!recorder.failed)
        exit_status = 0;

cleanup:
    if (pipeline) {
        gst_element_set_state(pipeline, GST_STATE_NULL);
        gst_object_unref(pipeline);
    }
    if (pipewire_fd >= 0)
        close(pipewire_fd);
    if (connection && session_path)
        portal_close_session(connection, session_path);
    g_free(session_path);
    if (connection)
        g_object_unref(connection);
    g_free(default_output);
    return exit_status;
}
