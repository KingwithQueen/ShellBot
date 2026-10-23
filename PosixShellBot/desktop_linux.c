#include "desktop.h"
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <X11/XKBlib.h>
#include <X11/keysym.h>
#include <X11/extensions/XTest.h>
#include <X11/extensions/XInput2.h>
#include <X11/extensions/Xfixes.h>
#include <X11/extensions/Xrandr.h>
#include <xkbcommon/xkbcommon.h>
#include <gio/gio.h>
#include <gio/gunixfdlist.h>
#include <gst/gst.h>
#include <gst/app/gstappsink.h>
#include <gst/video/video.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define PORTAL "org.freedesktop.portal.Desktop"
#define OBJECT "/org/freedesktop/portal/desktop"
#define REMOTE "org.freedesktop.portal.RemoteDesktop"
#define SCREEN "org.freedesktop.portal.ScreenCast"
#define CLIP "org.freedesktop.portal.Clipboard"
typedef struct {
    sb_os_desktop *owner;
    guint node;
    guint64 serial;
    sb_drect bounds;
    bool has_position, has_size;
    atomic_uint width, height;
    int pipefd;
    GstElement *pipeline, *sink;
    GstSample *sample;
    uint64_t sample_time;
} linux_stream;
struct sb_os_desktop {
    sb_desktop_policy policy;
    sb_desktop_interrupt interrupt;
    sb_desktop_guard guard;
    void *context;
    bool wayland;
    atomic_bool ready, cancelled, invalid;
    pthread_mutex_t lock;
    Display *x;
    Window root, window;
    int xi, fixes, xkb, randr;
    bool randr_monitors;
    Atom clipboard, utf8, property;
    uint32_t sequence;
    int *physical;
    size_t physical_count;
    int *synthetic;
    size_t synthetic_count;
    uint64_t keymap_epoch;
    char keymap_sha[65];
    bool action_keys;
    bool action_target;
    sb_dwindow action_window;
    unsigned action_group, action_locked, action_latched;
    char *owned;
    bool selection_done;
    Atom selection_result;
    GDBusConnection *bus;
    GMainContext *loop;
    GCancellable *cancel;
    char *session;
    guint subscriptions[4];
    guint devices;
    linux_stream streams[SB_DESKTOP_MONITORS];
    unsigned stream_count;
    bool atlas, pixel_atlas;
    bool clipboard_enabled, clip_utf8;
    char clip_mime[40];
};
static const char *str(yyjson_val *v, const char *k) {
    const char *s = sb_string(yyjson_obj_get(v, k));
    return s ? s : "";
}
static int num(yyjson_val *v, const char *k, int fallback) {
    yyjson_val *n = yyjson_obj_get(v, k);
    return n ? (int)yyjson_get_sint(n) : fallback;
}
static GVariant *empty(void) {
    GVariantBuilder b;
    g_variant_builder_init(&b, G_VARIANT_TYPE_VARDICT);
    return g_variant_builder_end(&b);
}
static GVariant *call(sb_os_desktop *d, const char *interface, const char *method, GVariant *args,
                      sb_error *e) {
    GError *error = NULL;
    GVariant *v = g_dbus_connection_call_sync(d->bus, PORTAL, OBJECT, interface, method, args, NULL,
                                              G_DBUS_CALL_FLAGS_NONE, 3000, d->cancel, &error);
    if (!v) {
        snprintf(e->text, sizeof(e->text), "Desktop portal %s: %.170s", method,
                 error ? error->message : "unavailable");
        g_clear_error(&error);
    }
    return v;
}
static int fd_call(sb_os_desktop *d, const char *interface, const char *method, GVariant *args, sb_error *e) {
    GError *error = NULL;
    GUnixFDList *fds = NULL;
    GVariant *v = g_dbus_connection_call_with_unix_fd_list_sync(
        d->bus, PORTAL, OBJECT, interface, method, args, G_VARIANT_TYPE("(h)"), G_DBUS_CALL_FLAGS_NONE, 3000,
        NULL, &fds, d->cancel, &error);
    int fd = -1, index = 0;
    if (v) {
        g_variant_get(v, "(h)", &index);
        fd = g_unix_fd_list_get(fds, index, &error);
        g_variant_unref(v);
    }
    if (fds)
        g_object_unref(fds);
    if (fd < 0)
        snprintf(e->text, sizeof(e->text), "Desktop portal %s: %.170s", method,
                 error ? error->message : "missing descriptor");
    else
        fcntl(fd, F_SETFD, FD_CLOEXEC);
    g_clear_error(&error);
    return fd;
}
typedef struct {
    const char *path;
    bool done;
    guint code;
    GVariant *value;
} response;
static void response_signal(GDBusConnection *c, const gchar *sender, const gchar *path,
                            const gchar *interface, const gchar *signal, GVariant *args, gpointer context) {
    (void)c;
    (void)sender;
    (void)interface;
    (void)signal;
    response *r = context;
    if (strcmp(path, r->path) || r->done)
        return;
    g_variant_get(args, "(u@a{sv})", &r->code, &r->value);
    r->done = true;
}
static GVariant *request(sb_os_desktop *d, const char *interface, const char *method, GVariant *args,
                         const char *token, sb_error *e) {
    /* Subscribe before issuing the method; a fast portal may emit Response before the reply. */
    char *sender = g_strdup(g_dbus_connection_get_unique_name(d->bus) + 1);
    for (char *p = sender; *p; p++)
        if (*p == '.')
            *p = '_';
    char *path = g_strdup_printf("%s/request/%s/%s", OBJECT, sender, token);
    g_free(sender);
    response r = {.path = path};
    guint sub =
        g_dbus_connection_signal_subscribe(d->bus, PORTAL, "org.freedesktop.portal.Request", "Response", path,
                                           NULL, G_DBUS_SIGNAL_FLAGS_NONE, response_signal, &r, NULL);
    GVariant *reply = call(d, interface, method, args, e);
    bool valid = false;
    if (reply) {
        const char *actual;
        g_variant_get(reply, "(&o)", &actual);
        valid = !strcmp(actual, path);
        g_variant_unref(reply);
    }
    uint64_t end = sb_platform_millis() + 120000;
    while (valid && !r.done && !atomic_load(&d->cancelled) && sb_platform_millis() < end) {
        g_main_context_iteration(d->loop, false);
        g_usleep(5000);
    }
    g_dbus_connection_signal_unsubscribe(d->bus, sub);
    if (!r.done) {
        g_dbus_connection_call(d->bus, PORTAL, path, "org.freedesktop.portal.Request", "Close", NULL, NULL,
                               G_DBUS_CALL_FLAGS_NONE, 1000, NULL, NULL, NULL);
        if (!*e->text)
            sb_fail(e, "Desktop consent was cancelled, timed out, or returned an invalid request handle.");
    } else if (r.code)
        snprintf(e->text, sizeof(e->text), "Desktop consent was declined or cancelled (%u).", r.code);
    g_free(path);
    if (!r.done || r.code) {
        if (r.value)
            g_variant_unref(r.value);
        return NULL;
    }
    return r.value;
}
static void options(GVariantBuilder *b, char token[48]) {
    static atomic_uint serial;
    snprintf(token, 48, "sb_%u_%u", (unsigned)getpid(), atomic_fetch_add(&serial, 1));
    g_variant_builder_init(b, G_VARIANT_TYPE_VARDICT);
    g_variant_builder_add(b, "{sv}", "handle_token", g_variant_new_string(token));
}
static bool portal_streams(sb_os_desktop *d, GVariant *streams, sb_error *e) {
    if (!streams || !g_variant_is_of_type(streams, G_VARIANT_TYPE("a(ua{sv})")) ||
        !g_variant_n_children(streams) || g_variant_n_children(streams) > SB_DESKTOP_MONITORS) {
        sb_fail(e, "Portal must grant between one and sixteen monitor streams.");
        return false;
    }
    d->stream_count = (unsigned)g_variant_n_children(streams);
    for (unsigned i = 0; i < d->stream_count; i++) {
        linux_stream *s = &d->streams[i];
        s->owner = d;
        s->pipefd = -1;
        atomic_init(&s->width, 0);
        atomic_init(&s->height, 0);
        GVariant *entry = g_variant_get_child_value(streams, i), *props = NULL;
        g_variant_get(entry, "(u@a{sv})", &s->node, &props);
        bool valid = s->node != 0;
        const char *keys[] = {"position", "size", "pipewire-serial", "source_type"};
        const GVariantType *types[] = {G_VARIANT_TYPE("(ii)"), G_VARIANT_TYPE("(ii)"),
                                     G_VARIANT_TYPE_UINT64, G_VARIANT_TYPE_UINT32};
        for (unsigned k = 0; k < 4; k++) {
            GVariantIter entries;
            const char *name;
            GVariant *item;
            unsigned matches = 0;
            g_variant_iter_init(&entries, props);
            while (g_variant_iter_next(&entries, "{&sv}", &name, &item)) {
                matches += !strcmp(name, keys[k]);
                g_variant_unref(item);
            }
            valid &= matches <= 1;
            GVariant *v = g_variant_lookup_value(props, keys[k], NULL);
            if (v) {
                if (!g_variant_is_of_type(v, types[k]))
                    valid = false;
                else if (k == 0) {
                    g_variant_get(v, "(ii)", &s->bounds.x, &s->bounds.y);
                    s->has_position = true;
                } else if (k == 1) {
                    g_variant_get(v, "(ii)", &s->bounds.width, &s->bounds.height);
                    s->has_size = true;
                    valid &= s->bounds.width > 0 && s->bounds.height > 0 &&
                             s->bounds.width <= 32768 && s->bounds.height <= 32768;
                } else if (k == 2) {
                    s->serial = g_variant_get_uint64(v);
                    valid &= s->serial != 0;
                } else
                    valid &= g_variant_get_uint32(v) == 1;
                g_variant_unref(v);
            }
        }
        for (unsigned j = 0; j < i; j++)
            valid &= s->node != d->streams[j].node &&
                     (!s->serial || s->serial != d->streams[j].serial);
        g_variant_unref(props);
        g_variant_unref(entry);
        if (!valid || (d->policy.control && !s->has_size)) {
            sb_fail(e, "Portal monitor metadata is invalid, duplicated, or lacks the logical size required for control.");
            return false;
        }
    }
    /* Sort before creating pipelines: pad callbacks retain each stream's address. */
    for (unsigned i = 1; i < d->stream_count; i++)
        for (unsigned j = i; j && d->streams[j - 1].node > d->streams[j].node; j--) {
            linux_stream swap = d->streams[j - 1];
            d->streams[j - 1] = d->streams[j];
            d->streams[j] = swap;
        }
    return true;
}
static bool portal_layout(sb_os_desktop *d, sb_error *e) {
    d->atlas = false;
    d->pixel_atlas = false;
    for (unsigned i = 0; i < d->stream_count; i++)
        d->pixel_atlas |= !d->streams[i].has_size;
    for (unsigned i = 0; i < d->stream_count; i++) {
        linux_stream *s = &d->streams[i];
        if (d->pixel_atlas) {
            s->bounds.width = (int)atomic_load(&s->width);
            s->bounds.height = (int)atomic_load(&s->height);
        }
        if (s->bounds.width <= 0 || s->bounds.height <= 0 || s->bounds.width > 32768 ||
            s->bounds.height > 32768) {
            sb_fail(e, "Portal stream has no usable frame dimensions.");
            return false;
        }
        if (!s->has_position || d->pixel_atlas || s->bounds.x < -32768 || s->bounds.y < -32768 ||
            (int64_t)s->bounds.x + s->bounds.width > 32768 ||
            (int64_t)s->bounds.y + s->bounds.height > 32768)
            d->atlas = true;
        for (unsigned j = 0; j < i; j++) {
            sb_drect b = d->streams[j].bounds, a = s->bounds;
            if ((int64_t)a.x < (int64_t)b.x + b.width && (int64_t)b.x < (int64_t)a.x + a.width &&
                (int64_t)a.y < (int64_t)b.y + b.height && (int64_t)b.y < (int64_t)a.y + a.height)
                d->atlas = true;
        }
    }
    if (d->atlas) {
        int x = 0, y = 0, row = 0;
        for (unsigned i = 0; i < d->stream_count; i++) {
            linux_stream *s = &d->streams[i];
            if (x + s->bounds.width > 32768) {
                x = 0;
                y += row;
                row = 0;
            }
            if (y + s->bounds.height > 32768) {
                sb_fail(e, "Selected monitor atlas exceeds supported coordinate bounds; select fewer monitors.");
                return false;
            }
            s->bounds.x = x;
            s->bounds.y = y;
            x += s->bounds.width;
            if (s->bounds.height > row)
                row = s->bounds.height;
        }
    }
    return d->stream_count != 0;
}
static linux_stream *portal_point(sb_os_desktop *d, int x, int y) {
    for (unsigned i = 0; i < d->stream_count; i++) {
        linux_stream *s = &d->streams[i];
        if (x >= s->bounds.x && y >= s->bounds.y &&
            (int64_t)x < (int64_t)s->bounds.x + s->bounds.width &&
            (int64_t)y < (int64_t)s->bounds.y + s->bounds.height)
            return s;
    }
    return NULL;
}
static GstPadProbeReturn stream_caps(GstPad *pad, GstPadProbeInfo *info, gpointer context) {
    (void)pad;
    linux_stream *s = context;
    sb_os_desktop *d = s->owner;
    GstEvent *event = GST_PAD_PROBE_INFO_EVENT(info);
    if (GST_EVENT_TYPE(event) != GST_EVENT_CAPS)
        return GST_PAD_PROBE_OK;
    GstCaps *caps = NULL;
    gst_event_parse_caps(event, &caps);
    GstVideoInfo video;
    if (caps && gst_video_info_from_caps(&video, caps) &&
        GST_VIDEO_INFO_FORMAT(&video) == GST_VIDEO_FORMAT_BGRA) {
        unsigned w = GST_VIDEO_INFO_WIDTH(&video), h = GST_VIDEO_INFO_HEIGHT(&video),
                 previous_w = atomic_exchange(&s->width, w),
                 previous_h = atomic_exchange(&s->height, h);
        if (!w || !h || w > 32768 || h > 32768 || (previous_w && (previous_w != w || previous_h != h))) {
            atomic_store(&d->invalid, true);
            atomic_store(&d->ready, false);
            d->interrupt(d->context);
        }
    } else {
        atomic_store(&d->invalid, true);
        atomic_store(&d->ready, false);
        d->interrupt(d->context);
    }
    return GST_PAD_PROBE_OK;
}
static void session_closed(GDBusConnection *c, const gchar *sender, const gchar *path, const gchar *interface,
                           const gchar *signal, GVariant *args, gpointer ctx) {
    (void)c;
    (void)sender;
    (void)path;
    (void)interface;
    (void)signal;
    (void)args;
    sb_os_desktop *d = ctx;
    atomic_store(&d->invalid, true);
    atomic_store(&d->ready, false);
    d->interrupt(d->context);
}
static void clipboard_changed(GDBusConnection *c, const gchar *sender, const gchar *path,
                              const gchar *interface, const gchar *signal, GVariant *args, gpointer ctx) {
    (void)c;
    (void)sender;
    (void)path;
    (void)interface;
    (void)signal;
    sb_os_desktop *d = ctx;
    const char *session;
    GVariant *dict;
    g_variant_get(args, "(&o@a{sv})", &session, &dict);
    if (d->session && !strcmp(session, d->session)) {
        GVariant *types = g_variant_lookup_value(dict, "mime_types", G_VARIANT_TYPE_STRING_ARRAY);
        bool utf8 = false;
        char selected[40] = "";
        if (types) {
            GVariantIter it;
            const char *type;
            g_variant_iter_init(&it, types);
            while (g_variant_iter_next(&it, "&s", &type))
                if (!strcmp(type, "text/plain;charset=utf-8") || !strcmp(type, "text/plain")) {
                    utf8 = true;
                    if (!*selected || !strcmp(type, "text/plain;charset=utf-8"))
                        snprintf(selected, sizeof(selected), "%s", type);
                }
            g_variant_unref(types);
        }
        pthread_mutex_lock(&d->lock);
        d->sequence++;
        d->clip_utf8 = utf8;
        snprintf(d->clip_mime, sizeof(d->clip_mime), "%s", selected);
        gboolean owner = false;
        g_variant_lookup(dict, "session_is_owner", "b", &owner);
        if (!owner && d->owned) {
            memset(d->owned, 0, strlen(d->owned));
            free(d->owned);
            d->owned = NULL;
        }
        pthread_mutex_unlock(&d->lock);
    }
    g_variant_unref(dict);
}
static void clipboard_transfer(GDBusConnection *c, const gchar *sender, const gchar *path,
                               const gchar *interface, const gchar *signal, GVariant *args, gpointer ctx) {
    (void)c;
    (void)sender;
    (void)path;
    (void)interface;
    (void)signal;
    sb_os_desktop *d = ctx;
    const char *session, *mime;
    guint serial;
    g_variant_get(args, "(&o&su)", &session, &mime, &serial);
    if (!d->session || strcmp(session, d->session))
        return;
    pthread_mutex_lock(&d->lock);
    char *text = d->owned ? sb_strdup(d->owned) : NULL;
    pthread_mutex_unlock(&d->lock);
    sb_error e = {0};
    bool ok = false;
    if (text && (!strcmp(mime, "text/plain;charset=utf-8") || !strcmp(mime, "text/plain"))) {
        int fd = fd_call(d, CLIP, "SelectionWrite", g_variant_new("(ou)", d->session, serial), &e);
        if (fd >= 0) {
            fcntl(fd, F_SETFL, O_NONBLOCK);
            size_t at = 0, n = strlen(text);
            uint64_t end = sb_platform_millis() + 1000;
            while (at < n && sb_platform_millis() < end && !atomic_load(&d->cancelled)) {
                ssize_t w = write(fd, text + at, n - at);
                if (w > 0)
                    at += (size_t)w;
                else if (errno == EINTR)
                    continue;
                else if (errno == EAGAIN) {
                    struct pollfd p = {fd, POLLOUT, 0};
                    poll(&p, 1, 20);
                } else
                    break;
            }
            ok = at == n;
            close(fd);
        }
    }
    if (text) {
        memset(text, 0, strlen(text));
        free(text);
    }
    GVariant *v = call(d, CLIP, "SelectionWriteDone", g_variant_new("(oub)", d->session, serial, ok), &e);
    if (v)
        g_variant_unref(v);
}
static bool portal_pipeline(sb_os_desktop *d, linux_stream *s, sb_error *e) {
    s->pipefd = fd_call(d, SCREEN, "OpenPipeWireRemote",
                       g_variant_new("(o@a{sv})", d->session, empty()), e);
    if (s->pipefd < 0)
        return false;
    s->pipeline = gst_pipeline_new(NULL);
    GstElement *source = gst_element_factory_make("pipewiresrc", NULL),
               *convert = gst_element_factory_make("videoconvert", NULL);
    s->sink = gst_element_factory_make("appsink", NULL);
    if (!s->pipeline || !source || !convert || !s->sink) {
        if (source)
            gst_object_unref(source);
        if (convert)
            gst_object_unref(convert);
        if (s->sink)
            gst_object_unref(s->sink);
        s->sink = NULL;
        sb_fail(e, "PipeWire GStreamer source/converter/appsink is unavailable.");
        return false;
    }
    if (s->serial && !g_object_class_find_property(G_OBJECT_GET_CLASS(source), "target-object")) {
        gst_object_unref(source);
        gst_object_unref(convert);
        gst_object_unref(s->sink);
        s->sink = NULL;
        sb_fail(e, "PipeWire source lacks stable serial targeting; update the GStreamer PipeWire plugin.");
        return false;
    }
    char node[32];
    snprintf(node, sizeof(node), "%u", s->node);
    g_object_set(source, "fd", s->pipefd, "path", node, "do-timestamp", TRUE, NULL);
    if (s->serial) {
        char serial[32];
        snprintf(serial, sizeof(serial), "%llu", (unsigned long long)s->serial);
        g_object_set(source, "target-object", serial, NULL);
    }
    GstCaps *caps = gst_caps_new_simple("video/x-raw", "format", G_TYPE_STRING, "BGRA", NULL);
    g_object_set(s->sink, "caps", caps, "max-buffers", 1, "drop", TRUE, "sync", FALSE, NULL);
    gst_caps_unref(caps);
    gst_bin_add_many(GST_BIN(s->pipeline), source, convert, s->sink, NULL);
    GstPad *pad = gst_element_get_static_pad(s->sink, "sink");
    gst_pad_add_probe(pad, GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM, stream_caps, s, NULL);
    gst_object_unref(pad);
    if (!gst_element_link_many(source, convert, s->sink, NULL) ||
        gst_element_set_state(s->pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
        sb_fail(e, "Cannot start the permitted PipeWire stream.");
        return false;
    }
    s->sample = gst_app_sink_try_pull_sample(GST_APP_SINK(s->sink), 3 * GST_SECOND);
    if (!s->sample || !atomic_load(&s->width) || !atomic_load(&s->height)) {
        sb_fail(e, "The permitted PipeWire stream produced no initial frame.");
        return false;
    }
    s->sample_time = sb_platform_millis();
    return true;
}
static bool wayland_connect(sb_os_desktop *d, sb_error *e) {
    d->loop = g_main_context_new();
    g_main_context_push_thread_default(d->loop);
    GError *error = NULL;
    /* A private bus connection binds the portal session to this worker's lifetime. */
    char *address = g_dbus_address_get_for_bus_sync(G_BUS_TYPE_SESSION, d->cancel, &error);
    if (address) {
        d->bus = g_dbus_connection_new_for_address_sync(address,
                                                        G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT |
                                                            G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION,
                                                        NULL, d->cancel, &error);
        g_free(address);
    }
    if (!d->bus) {
        snprintf(e->text, sizeof(e->text), "Cannot connect to the Wayland session bus: %.160s",
                 error ? error->message : "unavailable");
        g_clear_error(&error);
        return false;
    }
    bool remote = d->policy.control || d->policy.clipboard_read || d->policy.clipboard_write;
    const char *interface = remote ? REMOTE : SCREEN;
    char token[48];
    GVariantBuilder b;
    options(&b, token);
    char session_token[48];
    snprintf(session_token, sizeof(session_token), "s_%u", (unsigned)getpid());
    g_variant_builder_add(&b, "{sv}", "session_handle_token", g_variant_new_string(session_token));
    GVariant *v = request(d, interface, "CreateSession", g_variant_new("(a{sv})", &b), token, e);
    if (!v)
        return false;
    const char *session = NULL;
    g_variant_lookup(v, "session_handle", "&s", &session);
    if (session && g_variant_is_object_path(session))
        d->session = g_strdup(session);
    g_variant_unref(v);
    if (!d->session) {
        sb_fail(e, "Portal returned no valid session.");
        return false;
    }
    d->subscriptions[0] =
        g_dbus_connection_signal_subscribe(d->bus, PORTAL, "org.freedesktop.portal.Session", "Closed",
                                           d->session, NULL, 0, session_closed, d, NULL);
    d->subscriptions[1] = g_dbus_connection_signal_subscribe(d->bus, PORTAL, CLIP, "SelectionOwnerChanged",
                                                             OBJECT, NULL, 0, clipboard_changed, d, NULL);
    d->subscriptions[2] = g_dbus_connection_signal_subscribe(d->bus, PORTAL, CLIP, "SelectionTransfer",
                                                             OBJECT, NULL, 0, clipboard_transfer, d, NULL);
    if (remote) {
        options(&b, token);
        g_variant_builder_add(&b, "{sv}", "types", g_variant_new_uint32(d->policy.control ? 3 : 0));
        g_variant_builder_add(&b, "{sv}", "persist_mode", g_variant_new_uint32(0));
        v = request(d, REMOTE, "SelectDevices", g_variant_new("(oa{sv})", d->session, &b), token, e);
        if (!v)
            return false;
        g_variant_unref(v);
    }
    options(&b, token);
    g_variant_builder_add(&b, "{sv}", "types", g_variant_new_uint32(1));
    g_variant_builder_add(&b, "{sv}", "multiple", g_variant_new_boolean(true));
    g_variant_builder_add(&b, "{sv}", "cursor_mode", g_variant_new_uint32(2));
    v = request(d, SCREEN, "SelectSources", g_variant_new("(oa{sv})", d->session, &b), token, e);
    if (!v)
        return false;
    g_variant_unref(v);
    if (d->policy.clipboard_read || d->policy.clipboard_write) {
        v = call(d, CLIP, "RequestClipboard", g_variant_new("(o@a{sv})", d->session, empty()), e);
        if (!v)
            return false;
        g_variant_unref(v);
    }
    options(&b, token);
    v = request(d, interface, "Start", g_variant_new("(osa{sv})", d->session, "", &b), token, e);
    if (!v)
        return false;
    g_variant_lookup(v, "devices", "u", &d->devices);
    gboolean clip = false;
    g_variant_lookup(v, "clipboard_enabled", "b", &clip);
    d->clipboard_enabled = clip;
    GVariant *streams = g_variant_lookup_value(v, "streams", G_VARIANT_TYPE("a(ua{sv})"));
    bool valid = portal_streams(d, streams, e);
    if (streams)
        g_variant_unref(streams);
    g_variant_unref(v);
    if (!valid)
        return false;
    if (d->policy.control && (d->devices & 3) != 3) {
        sb_fail(e, "Portal did not grant the requested monitor and input devices.");
        return false;
    }
    gst_init(NULL, NULL);
    for (unsigned i = 0; i < d->stream_count; i++)
        if (!portal_pipeline(d, &d->streams[i], e))
            return false;
    if (!portal_layout(d, e) || atomic_load(&d->cancelled) || atomic_load(&d->invalid))
        return false;
    atomic_store(&d->ready, true);
    return true;
}
static bool same_uid(pid_t pid) {
    char path[64];
    snprintf(path, sizeof(path), "/proc/%ld", (long)pid);
    struct stat st;
    return pid > 0 && !stat(path, &st) && st.st_uid == geteuid();
}
static unsigned long property(sb_os_desktop *d, Window w, Atom key) {
    Atom type;
    int format;
    unsigned long n, after;
    unsigned char *bytes = NULL;
    unsigned long result = 0;
    if (XGetWindowProperty(d->x, w, key, 0, 1, False, AnyPropertyType, &type, &format, &n, &after, &bytes) ==
            Success &&
        format == 32 && n == 1)
        result = *(unsigned long *)bytes;
    if (bytes)
        XFree(bytes);
    return result;
}
static void selection_request(sb_os_desktop *d, XSelectionRequestEvent *r) {
    XEvent out = {0};
    out.xselection = (XSelectionEvent){.type = SelectionNotify,
                                       .display = r->display,
                                       .requestor = r->requestor,
                                       .selection = r->selection,
                                       .target = r->target,
                                       .property = None,
                                       .time = r->time};
    Atom dest = r->property ? r->property : r->target;
    if (d->owned) {
        if (r->target == XInternAtom(d->x, "TARGETS", False)) {
            Atom targets[] = {XInternAtom(d->x, "TARGETS", False), d->utf8};
            XChangeProperty(d->x, r->requestor, dest, XA_ATOM, 32, PropModeReplace, (unsigned char *)targets,
                            2);
            out.xselection.property = dest;
        } else if (r->target == d->utf8) {
            XChangeProperty(d->x, r->requestor, dest, d->utf8, 8, PropModeReplace, (unsigned char *)d->owned,
                            (int)strlen(d->owned));
            out.xselection.property = dest;
        }
    }
    XSendEvent(d->x, r->requestor, False, 0, &out);
    XFlush(d->x);
}
static bool x_device_physical(const XIDeviceInfo *device) {
    return device && device->enabled && device->deviceid > 0 && device->name &&
           (device->use == XISlavePointer || device->use == XISlaveKeyboard ||
            device->use == XIFloatingSlave) && strcmp(device->name, "Virtual core XTEST pointer") &&
           strcmp(device->name, "Virtual core XTEST keyboard");
}
static bool x_device_synthetic(const XIDeviceInfo *device) {
    return device && device->enabled && device->deviceid > 0 && device->name &&
           ((device->use == XISlavePointer && !strcmp(device->name, "Virtual core XTEST pointer")) ||
            (device->use == XISlaveKeyboard && !strcmp(device->name, "Virtual core XTEST keyboard")));
}
static bool x_devices(sb_os_desktop *d) {
    int n = 0;
    XIDeviceInfo *devices = XIQueryDevice(d->x, XIAllDevices, &n);
    if (!devices)
        return false;
    int *physical = calloc(n ? (size_t)n : 1, sizeof(*physical));
    int *synthetic = calloc(n ? (size_t)n : 1, sizeof(*synthetic));
    if (!physical || !synthetic) {
        free(physical);
        free(synthetic);
        XIFreeDeviceInfo(devices);
        return false;
    }
    size_t count = 0, generated = 0;
    for (int i = 0; i < n; i++) {
        if (x_device_physical(&devices[i]))
            physical[count++] = devices[i].deviceid;
        if (x_device_synthetic(&devices[i]))
            synthetic[generated++] = devices[i].deviceid;
    }
    XIFreeDeviceInfo(devices);
    free(d->physical);
    d->physical = physical;
    d->physical_count = count;
    free(d->synthetic);
    d->synthetic = synthetic;
    d->synthetic_count = generated;
    return true;
}
static bool x_physical(sb_os_desktop *d, int id) {
    for (size_t i = 0; i < d->physical_count; i++)
        if (d->physical[i] == id)
            return true;
    return false;
}
static bool x_raw_interrupt(sb_os_desktop *d, int id) {
    if (x_physical(d, id))
        return true;
    for (size_t i = 0; i < d->synthetic_count; i++)
        if (d->synthetic[i] == id)
            return false;
    /* A device arriving before its hierarchy notification is never treated as our input. */
    return true;
}
static void x_map_number(GChecksum *sum, uint64_t number) {
    guchar bytes[8];
    for (unsigned i = 0; i < 8; i++)
        bytes[i] = (guchar)(number >> (i * 8));
    g_checksum_update(sum, bytes, sizeof(bytes));
}
static void x_map_modifiers(GChecksum *sum, const XkbModsRec *mods) {
    x_map_number(sum, mods->mask);
    x_map_number(sum, mods->real_mods);
    x_map_number(sum, mods->vmods);
}
static bool x_map_digest(sb_os_desktop *d, char digest[65]) {
    _Static_assert(sizeof(XkbAction) == 8 && sizeof(XkbAnyAction) == 8,
                   "XKB actions must retain their fixed protocol byte representation");
    /* Xorg also sends NewKeyboardNotify when copying an unchanged slave map to its master.
     * Compare actual key behavior; request opcodes and unchanged keycode ranges alone are insufficient. */
    XkbDescPtr map = XkbGetMap(d->x, XkbAllMapComponentsMask, XkbUseCoreKbd);
    if (!map || !map->map || !map->server ||
        XkbGetControls(d->x, XkbAllControlsMask, map) != Success || !map->ctrls ||
        XkbGetCompatMap(d->x, XkbAllCompatMask, map) != Success || !map->compat) {
        if (map)
            XkbFreeKeyboard(map, XkbAllComponentsMask, True);
        return false;
    }
    GChecksum *sum = g_checksum_new(G_CHECKSUM_SHA256);
    x_map_number(sum, map->min_key_code);
    x_map_number(sum, map->max_key_code);
    x_map_number(sum, map->map->num_types);
    for (unsigned i = 0; i < map->map->num_types; i++) {
        XkbKeyTypePtr type = &map->map->types[i];
        x_map_modifiers(sum, &type->mods);
        x_map_number(sum, type->num_levels);
        x_map_number(sum, type->map_count);
        for (unsigned j = 0; j < type->map_count; j++) {
            XkbKTMapEntryPtr entry = &type->map[j];
            x_map_number(sum, entry->active);
            x_map_number(sum, entry->level);
            x_map_modifiers(sum, &entry->mods);
            XkbModsRec zero = {0};
            x_map_modifiers(sum, type->preserve ? &type->preserve[j] : &zero);
        }
    }
    g_checksum_update(sum, map->server->vmods, XkbNumVirtualMods);
    for (unsigned k = map->min_key_code; k <= map->max_key_code; k++) {
        XkbSymMapPtr sym = &map->map->key_sym_map[k];
        g_checksum_update(sum, sym->kt_index, XkbNumKbdGroups);
        x_map_number(sum, sym->group_info);
        x_map_number(sum, sym->width);
        unsigned count = XkbKeyNumSyms(map, k);
        x_map_number(sum, count);
        KeySym *symbols = XkbKeySymsPtr(map, k);
        for (unsigned j = 0; j < count; j++)
            x_map_number(sum, symbols[j]);
        x_map_number(sum, map->map->modmap[k]);
        x_map_number(sum, map->server->behaviors[k].type);
        x_map_number(sum, map->server->behaviors[k].data);
        x_map_number(sum, map->server->explicit[k]);
        x_map_number(sum, map->server->vmodmap[k]);
        bool actions = XkbKeyHasActions(map, k);
        x_map_number(sum, actions);
        if (actions)
            g_checksum_update(sum, (const guchar *)XkbKeyActionsPtr(map, k), count * sizeof(XkbAction));
    }
    x_map_number(sum, map->ctrls->enabled_ctrls);
    x_map_number(sum, map->ctrls->num_groups);
    x_map_number(sum, map->ctrls->groups_wrap);
    x_map_modifiers(sum, &map->ctrls->internal);
    x_map_modifiers(sum, &map->ctrls->ignore_lock);
    x_map_number(sum, map->ctrls->mk_dflt_btn);
    x_map_number(sum, map->ctrls->repeat_delay);
    x_map_number(sum, map->ctrls->repeat_interval);
    x_map_number(sum, map->ctrls->slow_keys_delay);
    x_map_number(sum, map->ctrls->debounce_delay);
    x_map_number(sum, map->ctrls->mk_delay);
    x_map_number(sum, map->ctrls->mk_interval);
    x_map_number(sum, map->ctrls->mk_time_to_max);
    x_map_number(sum, map->ctrls->mk_max_speed);
    x_map_number(sum, (uint64_t)(int64_t)map->ctrls->mk_curve);
    x_map_number(sum, map->ctrls->ax_options);
    x_map_number(sum, map->ctrls->ax_timeout);
    x_map_number(sum, map->ctrls->axt_opts_mask);
    x_map_number(sum, map->ctrls->axt_opts_values);
    x_map_number(sum, map->ctrls->axt_ctrls_mask);
    x_map_number(sum, map->ctrls->axt_ctrls_values);
    g_checksum_update(sum, map->ctrls->per_key_repeat, XkbPerKeyBitArraySize);
    for (unsigned i = 0; i < map->ctrls->num_groups && i < XkbNumKbdGroups; i++)
        x_map_modifiers(sum, &map->compat->groups[i]);
    x_map_number(sum, map->compat->num_si);
    for (unsigned i = 0; i < map->compat->num_si; i++) {
        XkbSymInterpretPtr entry = &map->compat->sym_interpret[i];
        x_map_number(sum, entry->sym);
        x_map_number(sum, entry->flags);
        x_map_number(sum, entry->match);
        x_map_number(sum, entry->mods);
        x_map_number(sum, entry->virtual_mod);
        g_checksum_update(sum, (const guchar *)&entry->act, sizeof(entry->act));
    }
    snprintf(digest, 65, "%s", g_checksum_get_string(sum));
    g_checksum_free(sum);
    XkbFreeKeyboard(map, XkbAllComponentsMask, True);
    return true;
}
static void x_pump(sb_os_desktop *d) {
    while (XPending(d->x)) {
        XEvent event;
        XNextEvent(d->x, &event);
        if (d->randr >= 0 && (event.type == d->randr + RRScreenChangeNotify ||
                              event.type == d->randr + RRNotify)) {
            XRRUpdateConfiguration(&event);
            d->keymap_epoch++;
            d->interrupt(d->context);
        } else if (event.type == ConfigureNotify && event.xconfigure.window == d->root) {
            d->keymap_epoch++;
            d->interrupt(d->context);
        } else if (event.type == d->xkb) {
            XkbEvent *xkb = (XkbEvent *)&event;
            if (xkb->any.xkb_type == XkbMapNotify || xkb->any.xkb_type == XkbNewKeyboardNotify ||
                xkb->any.xkb_type == XkbControlsNotify || xkb->any.xkb_type == XkbCompatMapNotify) {
                if (xkb->any.xkb_type == XkbMapNotify)
                    XkbRefreshKeyboardMapping(&xkb->map);
                char digest[65];
                bool known = x_map_digest(d, digest);
                if (!known || xkb->any.xkb_type == XkbMapNotify || strcmp(digest, d->keymap_sha)) {
                    d->keymap_epoch++;
                    d->interrupt(d->context);
                }
                if (known)
                    snprintf(d->keymap_sha, sizeof(d->keymap_sha), "%s", digest);
                else
                    atomic_store(&d->ready, false);
            }
        } else if (event.type == MappingNotify) {
            XRefreshKeyboardMapping(&event.xmapping);
            d->keymap_epoch++;
            d->interrupt(d->context);
        } else if (event.type == d->fixes + XFixesSelectionNotify)
            d->sequence++;
        else if (event.type == SelectionRequest)
            selection_request(d, &event.xselectionrequest);
        else if (event.type == SelectionNotify) {
            d->selection_done = true;
            d->selection_result = event.xselection.property;
        } else if (event.type == SelectionClear) {
            if (d->owned) {
                memset(d->owned, 0, strlen(d->owned));
                free(d->owned);
                d->owned = NULL;
            }
        } else if (event.type == GenericEvent && event.xcookie.extension == d->xi &&
                   XGetEventData(d->x, &event.xcookie)) {
            if (event.xcookie.evtype == XI_HierarchyChanged) {
                if (!x_devices(d))
                    atomic_store(&d->ready, false);
                d->keymap_epoch++;
                d->interrupt(d->context);
            } else if (event.xcookie.evtype == XI_DeviceChanged) {
                XIDeviceChangedEvent *change = event.xcookie.data;
                if (change->reason != XISlaveSwitch) {
                    if (!x_devices(d))
                        atomic_store(&d->ready, false);
                    d->keymap_epoch++;
                    d->interrupt(d->context);
                } else if (x_raw_interrupt(d, change->sourceid))
                    d->interrupt(d->context);
            } else if (event.xcookie.evtype == XI_RawKeyPress || event.xcookie.evtype == XI_RawKeyRelease ||
                       event.xcookie.evtype == XI_RawButtonPress || event.xcookie.evtype == XI_RawButtonRelease ||
                event.xcookie.evtype == XI_RawMotion) {
                XIRawEvent *raw = event.xcookie.data;
                if (x_raw_interrupt(d, raw->sourceid))
                    d->interrupt(d->context);
            }
            XFreeEventData(d->x, &event.xcookie);
        }
    }
}
static int x_error(Display *x, XErrorEvent *e) {
    (void)x;
    (void)e;
    return 0;
} /* Races with a window closing are rejected by subsequent snapshots. */
static bool x_connect(sb_os_desktop *d, sb_error *e) {
    XInitThreads();
    XSetErrorHandler(x_error);
    d->x = XOpenDisplay(NULL);
    if (!d->x) {
        sb_fail(e, "No authenticated X11 desktop connection.");
        return false;
    }
    d->root = DefaultRootWindow(d->x);
    int a, b, c;
    if (XRRQueryExtension(d->x, &d->randr, &a)) {
        int major = 1, minor = 0;
        if (XRRQueryVersion(d->x, &major, &minor)) {
            int mask = RRScreenChangeNotifyMask;
            if (major > 1 || minor >= 2)
                mask |= RRCrtcChangeNotifyMask | RROutputChangeNotifyMask;
            if (major > 1 || minor >= 4)
                mask |= RRResourceChangeNotifyMask;
            d->randr_monitors = major > 1 || minor >= 5;
            XRRSelectInput(d->x, d->root, mask);
        }
    }
    XSelectInput(d->x, d->root, StructureNotifyMask);
    if (!XTestQueryExtension(d->x, &a, &b, &c, &a) ||
        !XQueryExtension(d->x, "XInputExtension", &d->xi, &a, &b) ||
        !XFixesQueryExtension(d->x, &d->fixes, &a)) {
        sb_fail(e, "X11 requires XTest, XInput2 and XFixes.");
        return false;
    }
    int major = 2, minor = 1;
    if (XIQueryVersion(d->x, &major, &minor) != Success || major < 2 || (major == 2 && minor < 1)) {
        sb_fail(e, "XInput 2.1 is required for reliable input source observation.");
        return false;
    }
    if (!x_devices(d))
        return (sb_fail(e, "Cannot enumerate X11 input devices."), false);
    unsigned char mask[XIMaskLen(XI_RawMotion)] = {0};
    XISetMask(mask, XI_RawKeyPress);
    XISetMask(mask, XI_RawKeyRelease);
    XISetMask(mask, XI_RawButtonPress);
    XISetMask(mask, XI_RawButtonRelease);
    XISetMask(mask, XI_RawMotion);
    XISetMask(mask, XI_HierarchyChanged);
    XISetMask(mask, XI_DeviceChanged);
    XIEventMask ev = {XIAllDevices, sizeof(mask), mask};
    XISelectEvents(d->x, d->root, &ev, 1);
    int opcode, error, xmajor = XkbMajorVersion, xminor = XkbMinorVersion;
    if (!XkbQueryExtension(d->x, &opcode, &d->xkb, &error, &xmajor, &xminor))
        return (sb_fail(e, "XKB is required for current-layout keyboard input."), false);
    unsigned events = XkbNewKeyboardNotifyMask | XkbMapNotifyMask | XkbStateNotifyMask |
                      XkbControlsNotifyMask | XkbCompatMapNotifyMask;
    XkbSelectEvents(d->x, XkbUseCoreKbd, events, events);
    if (!x_map_digest(d, d->keymap_sha))
        return (sb_fail(e, "Cannot fingerprint the active X11 keyboard map."), false);
    d->window = XCreateSimpleWindow(d->x, d->root, 0, 0, 1, 1, 0, 0, 0);
    d->clipboard = XInternAtom(d->x, "CLIPBOARD", False);
    d->utf8 = XInternAtom(d->x, "UTF8_STRING", False);
    d->property = XInternAtom(d->x, "_SHELLBOT_CLIPBOARD", False);
    XFixesSelectSelectionInput(d->x, d->window, d->clipboard,
                               XFixesSetSelectionOwnerNotifyMask | XFixesSelectionWindowDestroyNotifyMask |
                                   XFixesSelectionClientCloseNotifyMask);
    XSync(d->x, False);
    d->sequence = 1;
    atomic_store(&d->ready, true);
    return true;
}
sb_os_desktop *sb_os_desktop_new(const sb_desktop_policy *p, sb_desktop_interrupt interrupt,
                                 sb_desktop_guard guard, void *context, sb_error *e) {
    (void)e;
    sb_os_desktop *d = calloc(1, sizeof(*d));
    if (!d)
        return NULL;
    d->policy = *p;
    d->interrupt = interrupt;
    d->guard = guard;
    d->context = context;
    d->randr = -1;
    for (unsigned i = 0; i < SB_DESKTOP_MONITORS; i++)
        d->streams[i].pipefd = -1;
    d->wayland = getenv("WAYLAND_DISPLAY") && *getenv("WAYLAND_DISPLAY");
    const char *type = getenv("XDG_SESSION_TYPE");
    if (type && !strcmp(type, "wayland"))
        d->wayland = true;
    pthread_mutex_init(&d->lock, NULL);
    atomic_init(&d->ready, false);
    atomic_init(&d->cancelled, false);
    atomic_init(&d->invalid, false);
    d->cancel = g_cancellable_new();
    return d;
}
bool sb_os_desktop_connect(sb_os_desktop *d, sb_error *e) {
    bool ok = d->wayland ? wayland_connect(d, e) : x_connect(d, e);
    if (d->wayland && d->loop)
        g_main_context_pop_thread_default(d->loop);
    return ok;
}
bool sb_os_desktop_ready(sb_os_desktop *d) {
    return d && atomic_load(&d->ready) && !atomic_load(&d->cancelled) && !atomic_load(&d->invalid);
}
const char *sb_os_desktop_provider(sb_os_desktop *d) {
    return d->wayland ? "Wayland portal / PipeWire" : "X11 / XTest";
}
const char *sb_os_desktop_coordinates(sb_os_desktop *d) {
    if (!d->wayland)
        return "x11_desktop_pixels";
    if (!atomic_load(&d->ready))
        return "portal_session_pending";
    return d->pixel_atlas ? "portal_stream_atlas_pixels"
                         : d->atlas ? "portal_stream_atlas_logical" : "compositor_logical";
}
bool sb_os_desktop_human_pause(sb_os_desktop *d) { return !d->wayland; }
yyjson_mut_val *sb_os_desktop_permissions(yyjson_mut_doc *doc) {
    yyjson_mut_val *v = yyjson_mut_obj(doc);
    const char *display = getenv("WAYLAND_DISPLAY"), *type = getenv("XDG_SESSION_TYPE");
    bool wayland = (display && *display) || (type && !strcmp(type, "wayland"));
    yyjson_mut_obj_add_str(doc, v, "platform", "linux");
    yyjson_mut_obj_add_str(doc, v, "backend", wayland ? "Wayland portal" : "X11");
    yyjson_mut_obj_add_bool(doc, v, "desktop_environment_present",
                            wayland ? (display && *display) : (getenv("DISPLAY") && *getenv("DISPLAY")));
    yyjson_mut_obj_add_bool(doc, v, "system_confirmation_required", wayland);
    yyjson_mut_obj_add_bool(doc, v, "connected", false);
    yyjson_mut_obj_add_str(doc, v, "meaning",
                           "Environment detection only; use desktop-status on a running project to inspect "
                           "its actual connection.");
    return v;
}
void sb_os_desktop_pump(sb_os_desktop *d) {
    if (d->wayland) {
        if (d->loop)
            while (g_main_context_iteration(d->loop, false)) {
            }
        if (d->bus && g_dbus_connection_is_closed(d->bus) && atomic_exchange(&d->ready, false))
            d->interrupt(d->context);
        for (unsigned i = 0; i < d->stream_count; i++) {
            if (!d->streams[i].pipeline)
                continue;
            GstBus *bus = gst_element_get_bus(d->streams[i].pipeline);
            GstMessage *message = gst_bus_pop_filtered(bus, GST_MESSAGE_ERROR | GST_MESSAGE_EOS);
            if (message) {
                if (atomic_exchange(&d->ready, false))
                    d->interrupt(d->context);
                gst_message_unref(message);
            }
            gst_object_unref(bus);
        }
    } else if (d->x) {
        pthread_mutex_lock(&d->lock);
        x_pump(d);
        pthread_mutex_unlock(&d->lock);
    }
}
void sb_os_desktop_cancel(sb_os_desktop *d) {
    if (d) {
        atomic_store(&d->cancelled, true);
        g_cancellable_cancel(d->cancel);
    }
}
void sb_os_desktop_free(sb_os_desktop *d) {
    if (!d)
        return;
    for (unsigned i = 0; i < d->stream_count; i++) {
        linux_stream *s = &d->streams[i];
        if (s->pipeline) {
            gst_element_set_state(s->pipeline, GST_STATE_NULL);
            gst_object_unref(s->pipeline);
        }
        if (s->sample)
            gst_sample_unref(s->sample);
        if (s->pipefd >= 0)
            close(s->pipefd);
    }
    if (d->bus) {
        if (d->session) {
            GVariant *v =
                g_dbus_connection_call_sync(d->bus, PORTAL, d->session, "org.freedesktop.portal.Session",
                                            "Close", NULL, NULL, 0, 1000, NULL, NULL);
            if (v)
                g_variant_unref(v);
        }
        for (unsigned i = 0; i < 4; i++)
            if (d->subscriptions[i])
                g_dbus_connection_signal_unsubscribe(d->bus, d->subscriptions[i]);
        g_dbus_connection_close_sync(d->bus, NULL, NULL);
        g_object_unref(d->bus);
    }
    if (d->loop)
        g_main_context_unref(d->loop);
    g_free(d->session);
    g_object_unref(d->cancel);
    if (d->owned) {
        memset(d->owned, 0, strlen(d->owned));
        free(d->owned);
    }
    if (d->x) {
        if (d->window)
            XDestroyWindow(d->x, d->window);
        XCloseDisplay(d->x);
    }
    free(d->physical);
    free(d->synthetic);
    pthread_mutex_destroy(&d->lock);
    free(d);
}
static bool unlocked(sb_os_desktop *d, sb_error *e) {
    GDBusConnection *bus = d->bus;
    bool temporary = false;
    if (!bus) {
        bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, NULL);
        temporary = true;
    }
    if (bus) {
        GVariant *v = g_dbus_connection_call_sync(bus, "org.freedesktop.ScreenSaver", "/ScreenSaver",
                                                  "org.freedesktop.ScreenSaver", "GetActive", NULL,
                                                  G_VARIANT_TYPE("(b)"), 0, 500, NULL, NULL);
        gboolean active = false;
        if (v) {
            g_variant_get(v, "(b)", &active);
            g_variant_unref(v);
        }
        if (temporary)
            g_object_unref(bus);
        if (active) {
            d->interrupt(d->context);
            sb_fail(e, "Desktop is locked or the screen saver is active.");
            return false;
        }
    }
    return true;
}
static bool x_window(sb_os_desktop *d, Window w, sb_dwindow *out) {
    pid_t pid = (pid_t)property(d, w, XInternAtom(d->x, "_NET_WM_PID", False));
    if (!same_uid(pid))
        return false;
    XWindowAttributes a;
    if (!XGetWindowAttributes(d->x, w, &a) || a.map_state != IsViewable || a.width <= 0 || a.height <= 0)
        return false;
    Window child;
    int x, y;
    if (!XTranslateCoordinates(d->x, w, d->root, 0, 0, &x, &y, &child))
        return false;
    memset(out, 0, sizeof(*out));
    out->native = w;
    out->pid = pid;
    out->bounds = (sb_drect){x, y, a.width, a.height};
    char *title = NULL;
    if (XFetchName(d->x, w, &title) && title) {
        if (g_utf8_validate(title, -1, NULL))
            snprintf(out->title, sizeof(out->title), "%.500s", title);
        XFree(title);
    }
    return true;
}
bool sb_os_desktop_view(sb_os_desktop *d, sb_dview *view, sb_error *e) {
    if (!sb_os_desktop_ready(d) || !unlocked(d, e))
        return false;
    memset(view, 0, sizeof(*view));
    pthread_mutex_lock(&d->lock);
    if (d->wayland) {
        view->count = d->stream_count;
        for (unsigned i = 0; i < d->stream_count; i++) {
            linux_stream *s = &d->streams[i];
            view->monitors[i] = (sb_dmonitor){s->node, s->bounds,
                                             atomic_load(&s->width) / (double)s->bounds.width};
        }
    } else {
        x_pump(d);
        int n = 0;
        XRRMonitorInfo *mon = d->randr_monitors ? XRRGetMonitors(d->x, d->root, True, &n) : NULL;
        for (int i = 0; mon && i < n && view->count < 16; i++)
            if (mon[i].width > 0 && mon[i].height > 0)
                view->monitors[view->count++] =
                    (sb_dmonitor){mon[i].name, {mon[i].x, mon[i].y, mon[i].width, mon[i].height}, 1};
        if (mon)
            XRRFreeMonitors(mon);
        if (!view->count) {
            XWindowAttributes a;
            XGetWindowAttributes(d->x, d->root, &a);
            view->monitors[view->count++] = (sb_dmonitor){d->root, {0, 0, a.width, a.height}, 1};
        }
        view->focus = property(d, d->root, XInternAtom(d->x, "_NET_ACTIVE_WINDOW", False));
        if (!view->focus) {
            Window w;
            int revert;
            XGetInputFocus(d->x, &w, &revert);
            view->focus = w;
        }
        Window root, parent, *children = NULL;
        unsigned count = 0;
        Atom type;
        int format;
        unsigned long items = 0, after = 0;
        unsigned char *clients = NULL;
        if (XGetWindowProperty(d->x, d->root, XInternAtom(d->x, "_NET_CLIENT_LIST_STACKING", False), 0, 64,
                               False, XA_WINDOW, &type, &format, &items, &after, &clients) == Success &&
            type == XA_WINDOW && format == 32) {
            children = (Window *)clients;
            count = (unsigned)items;
        } else {
            if (clients)
                XFree(clients);
            clients = NULL;
            XQueryTree(d->x, d->root, &root, &parent, &children, &count);
        }
        if (children) {
            for (unsigned i = 0; i < count && view->window_count < 64; i++)
                if (x_window(d, children[i], &view->windows[view->window_count]))
                    view->window_count++;
            if (children)
                XFree(children);
        }
        sb_dwindow w;
        if (x_window(d, view->focus, &w))
            view->focus_bounds = w.bounds;
    }
    view->clipboard = d->sequence;
    char layout[4096];
    size_t at = 0;
    for (unsigned i = 0; i < view->count; i++) {
        sb_dmonitor m = view->monitors[i];
        at += (size_t)snprintf(layout + at, sizeof(layout) - at, "%llu:%d,%d,%d,%d;",
                               (unsigned long long)m.native, m.bounds.x, m.bounds.y, m.bounds.width,
                               m.bounds.height);
        if (d->wayland) {
            linux_stream *s = &d->streams[i];
            at += (size_t)snprintf(layout + at, sizeof(layout) - at, "%llu:%u,%u;",
                                   (unsigned long long)s->serial, atomic_load(&s->width),
                                   atomic_load(&s->height));
        }
    }
    if (d->x) {
        XkbStateRec state;
        if (XkbGetState(d->x, XkbUseCoreKbd, &state) != Success) {
            pthread_mutex_unlock(&d->lock);
            sb_fail(e, "Cannot observe the active X11 keyboard group and lock state.");
            return false;
        }
        at += (size_t)snprintf(layout + at, sizeof(layout) - at, "keys:%llu,%u,%u,%u;",
                               (unsigned long long)d->keymap_epoch, state.group, state.locked_mods,
                               state.latched_mods);
    }
    sb_platform_sha256(layout, at, view->layout, e);
    pthread_mutex_unlock(&d->lock);
    return true;
}
static void dimensions(unsigned sw, unsigned sh, unsigned maximum, unsigned *w, unsigned *h) {
    double scale = sw > sh ? maximum / (double)sw : maximum / (double)sh;
    if (scale > 1)
        scale = 1;
    *w = (unsigned)(sw * scale);
    *h = (unsigned)(sh * scale);
    if (!*w)
        *w = 1;
    if (!*h)
        *h = 1;
}
static unsigned component(unsigned long pixel, unsigned long mask) {
    if (!mask)
        return 0;
    while (!(mask & 1)) {
        pixel >>= 1;
        mask >>= 1;
    }
    return (unsigned)((pixel & mask) * 255 / mask);
}
static bool portal_frame(linux_stream *s, GstSample *sample, const sb_drect *crop,
                         unsigned char *pixels, unsigned w, unsigned h, sb_error *e) {
    if (!s || !sample || !crop || !pixels || !w || !h || w > 1600 || h > 1600 ||
        crop->width <= 0 || crop->height <= 0 || s->bounds.width <= 0 || s->bounds.height <= 0 ||
        crop->x < s->bounds.x || crop->y < s->bounds.y ||
        (int64_t)crop->x + crop->width > (int64_t)s->bounds.x + s->bounds.width ||
        (int64_t)crop->y + crop->height > (int64_t)s->bounds.y + s->bounds.height)
        return (sb_fail(e, "Capture region is outside the selected portal monitor."), false);
    GstVideoInfo info;
    GstVideoFrame frame;
    if (!gst_sample_get_caps(sample) || !gst_sample_get_buffer(sample) ||
        !gst_video_info_from_caps(&info, gst_sample_get_caps(sample)) ||
        GST_VIDEO_INFO_FORMAT(&info) != GST_VIDEO_FORMAT_BGRA ||
        !gst_video_frame_map(&frame, &info, gst_sample_get_buffer(sample), GST_MAP_READ))
        return (sb_fail(e, "Portal stream has no valid BGRA frame."), false);
    int sw = GST_VIDEO_FRAME_WIDTH(&frame), sh = GST_VIDEO_FRAME_HEIGHT(&frame),
        stride = GST_VIDEO_FRAME_PLANE_STRIDE(&frame, 0);
    unsigned char *src = GST_VIDEO_FRAME_PLANE_DATA(&frame, 0);
    bool valid = sw > 0 && sh > 0 && sw <= 32768 && sh <= 32768 &&
                 sw == (int)GST_VIDEO_INFO_WIDTH(&info) && sh == (int)GST_VIDEO_INFO_HEIGHT(&info) &&
                 (!atomic_load(&s->width) || atomic_load(&s->width) == (unsigned)sw) &&
                 (!atomic_load(&s->height) || atomic_load(&s->height) == (unsigned)sh) &&
                 (int64_t)llabs(stride) >= (int64_t)sw * 4 && src;
    if (valid) {
        uintptr_t base = (uintptr_t)frame.map[0].data, start = (uintptr_t)src;
        uint64_t rows = (uint64_t)(sh - 1) * (uint64_t)llabs(stride),
                 width = (uint64_t)sw * 4;
        valid = start >= base && start - base <= frame.map[0].size;
        if (valid) {
            uint64_t offset = start - base, size = frame.map[0].size;
            valid = stride < 0 ? rows <= offset && width <= size - offset
                               : rows <= size - offset && width <= size - offset - rows;
        }
    }
    if (valid)
        for (unsigned y = 0; y < h; y++)
            for (unsigned x = 0; x < w; x++) {
                uint64_t lx = (uint64_t)(crop->x - s->bounds.x) * w + (uint64_t)x * crop->width,
                         ly = (uint64_t)(crop->y - s->bounds.y) * h + (uint64_t)y * crop->height;
                unsigned sx = (unsigned)(lx * (unsigned)sw / ((uint64_t)s->bounds.width * w)),
                         sy = (unsigned)(ly * (unsigned)sh / ((uint64_t)s->bounds.height * h));
                memcpy(pixels + ((size_t)y * w + x) * 4,
                       src + (ptrdiff_t)sy * stride + (size_t)sx * 4, 4);
            }
    gst_video_frame_unmap(&frame);
    if (!valid)
        sb_fail(e, "Portal frame dimensions or stride are invalid.");
    return valid;
}
static bool capture_view(sb_os_desktop *d, const sb_dview *observed, sb_error *e) {
    sb_dview fresh;
    if (!sb_os_desktop_view(d, &fresh, e))
        return false;
    if (strcmp(observed->layout, fresh.layout) || observed->focus != fresh.focus) {
        sb_fail(e, "Desktop layout, keyboard group, or foreground changed during capture; observe again.");
        return false;
    }
    return true;
}
bool sb_os_desktop_capture(sb_os_desktop *d, const sb_dview *view, unsigned monitor, const sb_drect *crop,
                           unsigned maximum, unsigned char **out, unsigned *w, unsigned *h, sb_error *e) {
    if (!view || monitor >= view->count || !crop || !capture_view(d, view, e))
        return false;
    if (!d->wayland && (uint64_t)crop->width * crop->height > 16777216)
        return (sb_fail(e, "X11 source region exceeds 16 megapixels; use a smaller crop."), false);
    dimensions(crop->width, crop->height, maximum, w, h);
    unsigned char *pixels = malloc((size_t)*w * *h * 4);
    if (!pixels)
        return false;
    bool ok = false;
    if (d->wayland) {
        linux_stream *s = NULL;
        for (unsigned i = 0; i < d->stream_count; i++)
            if (d->streams[i].node == view->monitors[monitor].native)
                s = &d->streams[i];
        if (!s) {
            free(pixels);
            return (sb_fail(e, "Selected portal monitor no longer exists."), false);
        }
        GstSample *sample = gst_app_sink_try_pull_sample(GST_APP_SINK(s->sink), 2 * GST_SECOND);
        pthread_mutex_lock(&d->lock);
        if (sample) {
            if (s->sample)
                gst_sample_unref(s->sample);
            s->sample = sample;
            s->sample_time = sb_platform_millis();
        }
        sample = s->sample && sb_platform_millis() - s->sample_time <= 3000
                     ? gst_sample_ref(s->sample) : NULL;
        pthread_mutex_unlock(&d->lock);
        if (sample) {
            ok = portal_frame(s, sample, crop, pixels, *w, *h, e);
            gst_sample_unref(sample);
        }
    } else {
        pthread_mutex_lock(&d->lock);
        XImage *image =
            XGetImage(d->x, d->root, crop->x, crop->y, crop->width, crop->height, AllPlanes, ZPixmap);
        if (image) {
            for (unsigned y = 0; y < *h; y++)
                for (unsigned x = 0; x < *w; x++) {
                    unsigned long p =
                        XGetPixel(image, x * (unsigned)crop->width / *w, y * (unsigned)crop->height / *h);
                    unsigned char *b = pixels + ((size_t)y * *w + x) * 4;
                    b[0] = (unsigned char)component(p, image->blue_mask);
                    b[1] = (unsigned char)component(p, image->green_mask);
                    b[2] = (unsigned char)component(p, image->red_mask);
                    b[3] = 255;
                }
            XDestroyImage(image);
            ok = true;
        }
        pthread_mutex_unlock(&d->lock);
    }
    if (ok)
        ok = capture_view(d, view, e);
    if (!ok) {
        free(pixels);
        if (!*e->text)
            sb_fail(e, "No current permitted desktop frame is available.");
        return false;
    }
    *out = pixels;
    return true;
}
static KeySym key(const char *k) {
    struct {
        const char *name;
        KeySym code;
    } names[] = {{"CTRL", XK_Control_L},      {"ALT", XK_Alt_L},     {"SHIFT", XK_Shift_L},
                 {"META", XK_Super_L},        {"ENTER", XK_Return},  {"TAB", XK_Tab},
                 {"ESC", XK_Escape},          {"ESCAPE", XK_Escape}, {"SPACE", XK_space},
                 {"BACKSPACE", XK_BackSpace}, {"DELETE", XK_Delete}, {"INSERT", XK_Insert},
                 {"HOME", XK_Home},           {"END", XK_End},       {"PAGEUP", XK_Page_Up},
                 {"PAGEDOWN", XK_Page_Down},  {"UP", XK_Up},         {"DOWN", XK_Down},
                 {"LEFT", XK_Left},           {"RIGHT", XK_Right}};
    for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); i++)
        if (!strcmp(k, names[i].name))
            return names[i].code;
    if (k[0] == 'F' && strlen(k) <= 3) {
        int n = atoi(k + 1);
        if (n >= 1 && n <= 12)
            return XK_F1 + n - 1;
    }
    if (strlen(k) == 1 && g_ascii_isalnum(*k))
        return (KeySym)g_ascii_tolower(*k);
    return NoSymbol;
}
static bool guard(sb_os_desktop *d, sb_error *e) {
    if (!d->wayland) {
        x_pump(d);
        if (d->action_keys) {
            XkbStateRec state;
            if (XkbGetState(d->x, XkbUseCoreKbd, &state) != Success ||
                state.group != d->action_group || state.locked_mods != d->action_locked ||
                state.latched_mods != d->action_latched) {
                d->interrupt(d->context);
                sb_fail(e, "Keyboard group or lock state changed during input.");
                return false;
            }
        }
        if (d->action_target) {
            Window current = property(d, d->root, XInternAtom(d->x, "_NET_ACTIVE_WINDOW", False));
            if (!current) {
                int revert;
                XGetInputFocus(d->x, &current, &revert);
            }
            sb_dwindow window;
            if (current != d->action_window.native || !x_window(d, current, &window) ||
                window.pid != d->action_window.pid ||
                memcmp(&window.bounds, &d->action_window.bounds, sizeof(sb_drect))) {
                d->interrupt(d->context);
                sb_fail(e, "Foreground window identity or geometry changed during keyboard input.");
                return false;
            }
        }
    }
    if (!sb_os_desktop_ready(d) || !d->guard(d->context)) {
        sb_fail(e, "Desktop action interrupted or lease expired.");
        return false;
    }
    return true;
}
static bool notify(sb_os_desktop *d, const char *method, GVariant *v, sb_error *e) {
    GVariant *r = call(d, REMOTE, method, v, e);
    if (!r)
        return false;
    g_variant_unref(r);
    return true;
}
static bool x_target(sb_os_desktop *d, int x, int y);
static bool x_pointer_target(sb_os_desktop *d, sb_error *e) {
    Window root, child;
    int x, y, wx, wy;
    unsigned mask;
    if (!XQueryPointer(d->x, d->root, &root, &child, &x, &y, &wx, &wy, &mask) ||
        !x_target(d, x, y)) {
        d->interrupt(d->context);
        sb_fail(e, "Pointer target changed to an unverified application.");
        return false;
    }
    return true;
}
static bool motion(sb_os_desktop *d, int x, int y, sb_error *e) {
    if (!guard(d, e))
        return false;
    if (d->wayland)
    {
        linux_stream *s = portal_point(d, x, y);
        if (!s || !s->has_size)
            return (sb_fail(e, "Pointer position is outside a granted monitor with known logical size."), false);
        return notify(d, "NotifyPointerMotionAbsolute",
                      g_variant_new("(o@a{sv}udd)", d->session, empty(), s->node,
                                    (double)(x - s->bounds.x), (double)(y - s->bounds.y)), e);
    }
    if (!x_target(d, x, y)) {
        d->interrupt(d->context);
        return (sb_fail(e, "Pointer path is outside a verified current-user application."), false);
    }
    return XTestFakeMotionEvent(d->x, -1, x, y, CurrentTime) != 0;
}
static bool button(sb_os_desktop *d, unsigned button, bool down, sb_error *e) {
    if (down && !guard(d, e))
        return false;
    if (down && d->x && !x_pointer_target(d, e))
        return false;
    if (d->wayland) {
        int evdev = button == 1 ? 272 : button == 3 ? 273 : 274;
        return notify(d, "NotifyPointerButton",
                      g_variant_new("(o@a{sv}iu)", d->session, empty(), evdev, down ? 1u : 0u), e);
    }
    return XTestFakeButtonEvent(d->x, button, down, CurrentTime) != 0;
}
static bool keyboard(sb_os_desktop *d, KeySym sym, KeyCode code, bool down, sb_error *e) {
    if (down && !guard(d, e))
        return false;
    if (d->wayland)
        return notify(d, "NotifyKeyboardKeysym",
                      g_variant_new("(o@a{sv}iu)", d->session, empty(), (gint)sym, down ? 1u : 0u), e);
    return code && XTestFakeKeyEvent(d->x, code, down, CurrentTime);
}
static bool x_level3(sb_os_desktop *d, const XkbStateRec *state, KeyCode *code, unsigned *mask) {
    *code = 0;
    *mask = 0;
    XModifierKeymap *modifiers = XGetModifierMapping(d->x);
    if (!modifiers)
        return false;
    for (int i = 0; i < 8 * modifiers->max_keypermod; i++) {
        KeyCode candidate = modifiers->modifiermap[i];
        if (!candidate)
            continue;
        unsigned bits = 0;
        for (int j = 0; j < 8 * modifiers->max_keypermod; j++)
            if (modifiers->modifiermap[j] == candidate)
                bits |= 1u << (j / modifiers->max_keypermod);
        KeySym sym;
        unsigned consumed;
        if (!(bits & (ShiftMask | LockMask | ControlMask | Mod1Mask | Mod4Mask)) &&
            XkbLookupKeySym(d->x, candidate, XkbBuildCoreState(state->mods, state->group), &consumed, &sym) &&
            sym == XK_ISO_Level3_Shift) {
            *code = candidate;
            *mask = bits;
            break;
        }
    }
    XFreeModifiermap(modifiers);
    return *code && *mask;
}
static bool x_symbol_equal(KeySym a, KeySym b) {
    uint32_t first = xkb_keysym_to_utf32((xkb_keysym_t)a),
             second = xkb_keysym_to_utf32((xkb_keysym_t)b);
    return a == b || (first && second && first == second);
}
static bool x_key(sb_os_desktop *d, KeySym target, const XkbStateRec *state,
                   KeyCode *code, unsigned *added) {
    int first, last;
    XDisplayKeycodes(d->x, &first, &last);
    KeyCode level3;
    unsigned level3_mask;
    bool have_level3 = x_level3(d, state, &level3, &level3_mask);
    unsigned masks[] = {0, ShiftMask, have_level3 ? level3_mask : 0,
                        have_level3 ? level3_mask | ShiftMask : 0};
    for (unsigned m = 0; m < (have_level3 ? 4u : 2u); m++)
        for (int k = first; k <= last; k++) {
            KeySym sym = NoSymbol;
            unsigned consumed;
            if (XkbLookupKeySym(d->x, (KeyCode)k,
                               XkbBuildCoreState(state->mods | masks[m], state->group), &consumed, &sym) &&
                x_symbol_equal(sym, target)) {
                *code = (KeyCode)k;
                *added = masks[m];
                return true;
            }
        }
    return false;
}
static bool x_named_key(sb_os_desktop *d, KeySym target, const XkbStateRec *state, KeyCode *code) {
    if (target >= XK_a && target <= XK_z) {
        int first, last;
        XDisplayKeycodes(d->x, &first, &last);
        for (int k = first; k <= last; k++) {
            KeySym sym = NoSymbol;
            unsigned consumed;
            if (XkbLookupKeySym(d->x, (KeyCode)k, XkbBuildCoreState(state->mods, state->group),
                               &consumed, &sym) &&
                (sym == target || sym == target - XK_a + XK_A)) {
                *code = (KeyCode)k;
                return true;
            }
        }
        return false;
    }
    unsigned added;
    return x_key(d, target, state, code, &added) && !added;
}
static bool modifiers_clear(sb_os_desktop *d, sb_error *e) {
    if (d->wayland)
        return true;
    Window root, child;
    int rx, ry, wx, wy;
    unsigned mask;
    char keys[32] = {0};
    bool query = XQueryKeymap(d->x, keys) != 0;
    bool key_held = false;
    for (unsigned i = 0; i < sizeof(keys); i++)
        key_held |= keys[i] != 0;
    XkbStateRec state;
    if (!query || key_held || XkbGetState(d->x, XkbUseCoreKbd, &state) != Success || state.base_mods ||
        state.latched_mods || state.base_group || state.latched_group ||
        !XQueryPointer(d->x, d->root, &root, &child, &rx, &ry, &wx, &wy, &mask) ||
        (mask & (Button1Mask | Button2Mask | Button3Mask | Button4Mask | Button5Mask))) {
        sb_fail(e, "A user key, modifier, or mouse button is held or latched.");
        return false;
    }
    return true;
}
static bool x_target(sb_os_desktop *d, int x, int y) {
    Window target = d->root, chain[18];
    unsigned count = 0;
    for (unsigned depth = 0; depth < 18; depth++) {
        chain[count++] = target;
        Window child = None;
        int dx, dy;
        if (!XTranslateCoordinates(d->x, d->root, target, x, y, &dx, &dy, &child))
            return false;
        if (!child)
            break;
        target = child;
    }
    while (count) {
        Window w = chain[--count];
        pid_t pid = (pid_t)property(d, w, XInternAtom(d->x, "_NET_WM_PID", False));
        if (pid > 0)
            return same_uid(pid);
    }
    return false;
}
bool sb_os_desktop_input(sb_os_desktop *d, const char *name, yyjson_val *v, const sb_dview *view,
                         const sb_dwindow *window, unsigned *sent, sb_error *e) {
    if (!d->policy.control || !unlocked(d, e)) {
        if (!*e->text)
            sb_fail(e, "Desktop input is disabled.");
        return false;
    }
    pthread_mutex_lock(&d->lock);
    bool ok = false;
    KeySym pressed[4] = {0};
    KeyCode pressed_codes[4] = {0};
    unsigned held = 0, mouse = 0;
    typedef struct { KeySym sym; KeyCode code; unsigned mods; } text_key;
    text_key *typing = NULL;
    XkbStateRec state = {0};
    KeyCode shift_code = 0, level3_code = 0;
    unsigned level3_mask = 0;
    if (!guard(d, e) || !modifiers_clear(d, e))
        goto done;
    if (d->x) {
        if (XkbGetState(d->x, XkbUseCoreKbd, &state) != Success)
            goto done;
        d->action_keys = true;
        d->action_group = state.group;
        d->action_locked = state.locked_mods;
        d->action_latched = state.latched_mods;
        unsigned shift_added;
        if (!x_key(d, XK_Shift_L, &state, &shift_code, &shift_added) || shift_added)
            shift_code = 0;
        x_level3(d, &state, &level3_code, &level3_mask);
    }
    if (!strcmp(name, "window_focus")) {
        if (d->wayland) {
            sb_fail(e, "Wayland does not expose arbitrary window enumeration/focus.");
            goto done;
        }
        sb_dwindow fresh;
        if (!window || !x_window(d, (Window)window->native, &fresh) || fresh.pid != window->pid ||
            memcmp(&fresh.bounds, &window->bounds, sizeof(sb_drect))) {
            sb_fail(e, "Window identity changed.");
            goto done;
        }
        XRaiseWindow(d->x, window->native);
        XSetInputFocus(d->x, window->native, RevertToPointerRoot, CurrentTime);
        (*sent)++;
        ok = true;
        goto done;
    }
    if (!d->wayland) {
        sb_dwindow focus;
        if (!x_window(d, (Window)view->focus, &focus)) {
            sb_fail(e, "Foreground window is not a verified current-user application.");
            goto done;
        }
        if (!strcmp(name, "desktop_key") || !strcmp(name, "desktop_type")) {
            d->action_target = true;
            d->action_window = focus;
        }
        if (!strcmp(name, "desktop_click") || !strcmp(name, "desktop_drag") ||
            !strcmp(name, "desktop_scroll")) {
            int x = num(v, "x", 0), y = num(v, "y", 0);
            if (!x_target(d, x, y) ||
                (!strcmp(name, "desktop_drag") && !x_target(d, num(v, "to_x", x), num(v, "to_y", y)))) {
                sb_fail(e, "Pointer target is not a verified current-user application.");
                goto done;
            }
        }
    }
    if (!strcmp(name, "desktop_key")) {
        yyjson_val *keys = yyjson_obj_get(v, "keys");
        unsigned n = (unsigned)yyjson_arr_size(keys);
        KeySym syms[4];
        KeyCode codes[4] = {0};
        for (unsigned i = 0; i < n; i++) {
            syms[i] = key(sb_string(yyjson_arr_get(keys, i)));
            if (!syms[i] || (!d->wayland && !x_named_key(d, syms[i], &state, &codes[i]))) {
                sb_fail(e, "Unsupported key in the current keyboard layout.");
                goto done;
            }
        }
        for (unsigned i = 0; i < n; i++) {
            if (!keyboard(d, syms[i], codes[i], true, e))
                goto done;
            pressed[held] = syms[i];
            pressed_codes[held++] = codes[i];
            (*sent)++;
        }
        ok = true;
    } else if (!strcmp(name, "desktop_type")) {
        const char *text = str(v, "text");
        if (!g_utf8_validate(text, -1, NULL)) {
            sb_fail(e, "Text must be valid UTF-8.");
            goto done;
        }
        size_t chars = (size_t)g_utf8_strlen(text, -1);
        typing = calloc(chars ? chars : 1, sizeof(*typing));
        if (!typing)
            goto done;
        /* Preflight the complete string before sending anything. X11 never remaps the global keyboard or
         * pastes via the clipboard. */
        size_t index = 0;
        for (const char *p = text; *p; p = g_utf8_next_char(p), index++) {
            gunichar ch = g_utf8_get_char(p);
            text_key *k = &typing[index];
            k->sym = ch == '\n' ? XK_Return : ch == '\t' ? XK_Tab : ch < 256 ? ch : 0x01000000 | ch;
            if (!d->wayland) {
                if (!x_key(d, k->sym, &state, &k->code, &k->mods) ||
                    ((k->mods & ShiftMask) && !shift_code)) {
                    sb_fail(e, "X11 text is not directly representable in the current keyboard layout; use "
                               "explicit clipboard tools and paste.");
                    goto done;
                }
            }
        }
        for (index = 0; index < chars; index++) {
            text_key *k = &typing[index];
            unsigned modifiers = held;
            if (k->mods & ShiftMask) {
                if (!keyboard(d, XK_Shift_L, shift_code, true, e))
                    goto done;
                pressed[held] = XK_Shift_L;
                pressed_codes[held++] = shift_code;
                (*sent)++;
            }
            if (k->mods & level3_mask) {
                if (!keyboard(d, XK_ISO_Level3_Shift, level3_code, true, e))
                    goto done;
                pressed[held] = XK_ISO_Level3_Shift;
                pressed_codes[held++] = level3_code;
                (*sent)++;
            }
            if (!keyboard(d, k->sym, k->code, true, e))
                goto done;
            pressed[held] = k->sym;
            pressed_codes[held++] = k->code;
            (*sent)++;
            if (!keyboard(d, k->sym, k->code, false, e))
                goto done;
            held--;
            (*sent)++;
            while (held > modifiers) {
                --held;
                if (!keyboard(d, pressed[held], pressed_codes[held], false, e))
                    goto done;
                (*sent)++;
            }
        }
        ok = true;
    } else {
        int x = num(v, "x", 0), y = num(v, "y", 0);
        unsigned btn = !strcmp(str(v, "button"), "right") ? 3 : !strcmp(str(v, "button"), "middle") ? 2 : 1;
        if (!motion(d, x, y, e))
            goto done;
        (*sent)++;
        if (!strcmp(name, "desktop_click")) {
            for (int i = 0; i < num(v, "clicks", 1); i++) {
                if (!button(d, btn, true, e))
                    goto done;
                mouse = btn;
                (*sent)++;
                if (!button(d, btn, false, e))
                    goto done;
                mouse = 0;
                (*sent)++;
            }
            ok = true;
        } else if (!strcmp(name, "desktop_drag")) {
            if (!button(d, btn, true, e))
                goto done;
            mouse = btn;
            (*sent)++;
            int tx = num(v, "to_x", x), ty = num(v, "to_y", y), duration = num(v, "duration_ms", 200);
            for (int i = 1; i <= 20; i++) {
                g_usleep((gulong)duration * 50);
                if (!motion(d, x + (tx - x) * i / 20, y + (ty - y) * i / 20, e))
                    goto done;
                (*sent)++;
            }
            ok = button(d, btn, false, e);
            if (ok) {
                mouse = 0;
                (*sent)++;
            }
        } else if (!strcmp(name, "desktop_scroll")) {
            int delta = num(v, "delta", 0);
            bool horizontal = yyjson_get_bool(yyjson_obj_get(v, "horizontal"));
            if (d->wayland)
                ok = guard(d, e) &&
                     notify(d, "NotifyPointerAxisDiscrete",
                            g_variant_new("(o@a{sv}ui)", d->session, empty(), horizontal ? 1u : 0u,
                                          (delta > 0 ? -1 : 1) * ((abs(delta) + 119) / 120)),
                            e);
            else {
                unsigned scroll = horizontal ? (delta > 0 ? 6 : 7) : (delta > 0 ? 4 : 5);
                ok = true;
                unsigned n = (unsigned)(abs(delta) + 119) / 120;
                for (unsigned i = 0; i < n; i++) {
                    if (!button(d, scroll, true, e) || !button(d, scroll, false, e)) {
                        ok = false;
                        break;
                    }
                    *sent += 2;
                }
            }
        }
    }
done:
    /* Release every event we inserted even when a pause or deadline occurs halfway through. */
    while (held) {
        --held;
        keyboard(d, pressed[held], pressed_codes[held], false, e);
        (*sent)++;
    }
    if (mouse) {
        button(d, mouse, false, e);
        (*sent)++;
    }
    if (d->x)
        XSync(d->x, False);
    d->action_keys = false;
    d->action_target = false;
    free(typing);
    pthread_mutex_unlock(&d->lock);
    if (!ok && !*e->text)
        sb_fail(e, "Native desktop input was not accepted.");
    return ok;
}
bool sb_os_desktop_clipboard(sb_os_desktop *d, bool write, uint32_t expected, const char *text,
                             char **read_text, uint32_t *sequence, sb_error *e) {
    if (!unlocked(d, e))
        return false;
    pthread_mutex_lock(&d->lock);
    if (d->x) {
        XSync(d->x, False);
        x_pump(d);
    }
    bool ok = false, server_grabbed = false;
    if (expected != d->sequence) {
        sb_fail(e, "Clipboard sequence changed; observe again.");
        goto done;
    }
    if (d->wayland) {
        if (!d->clipboard_enabled) {
            sb_fail(e, "Clipboard access was not granted by the desktop portal.");
            goto done;
        }
        if (write) {
            char *copy = sb_strdup(text);
            if (!copy)
                goto done;
            GVariantBuilder b;
            g_variant_builder_init(&b, G_VARIANT_TYPE_VARDICT);
            const char *types[] = {"text/plain;charset=utf-8", "text/plain", NULL};
            g_variant_builder_add(&b, "{sv}", "mime_types", g_variant_new_strv(types, -1));
            GVariant *v = call(d, CLIP, "SetSelection", g_variant_new("(oa{sv})", d->session, &b), e);
            if (!v) {
                free(copy);
                goto done;
            }
            g_variant_unref(v);
            if (d->owned) {
                memset(d->owned, 0, strlen(d->owned));
                free(d->owned);
            }
            d->owned = copy;
            d->sequence++;
            d->clip_utf8 = true;
            snprintf(d->clip_mime, sizeof(d->clip_mime), "text/plain;charset=utf-8");
            ok = true;
        } else {
            if (!d->clip_utf8) {
                sb_fail(e, "Clipboard has no advertised UTF-8 text.");
                goto done;
            }
            uint32_t before = d->sequence;
            char mime[40];
            snprintf(mime, sizeof(mime), "%s", d->clip_mime);
            pthread_mutex_unlock(&d->lock);
            int fd = fd_call(d, CLIP, "SelectionRead", g_variant_new("(os)", d->session, mime), e);
            char *bytes = NULL;
            size_t at = 0;
            if (fd >= 0) {
                bytes = malloc(32770);
                fcntl(fd, F_SETFL, O_NONBLOCK);
                uint64_t end = sb_platform_millis() + 2000;
                while (bytes && at < 32769 && sb_platform_millis() < end && !atomic_load(&d->cancelled)) {
                    ssize_t n = read(fd, bytes + at, 32769 - at);
                    if (n > 0)
                        at += (size_t)n;
                    else if (!n) {
                        ok = true;
                        break;
                    } else if (errno == EINTR)
                        continue;
                    else if (errno == EAGAIN) {
                        struct pollfd p = {fd, POLLIN, 0};
                        poll(&p, 1, 20);
                    } else
                        break;
                }
                close(fd);
            }
            pthread_mutex_lock(&d->lock);
            if (ok && at <= 32768 && d->sequence == before && g_utf8_validate(bytes, (gssize)at, NULL) &&
                !memchr(bytes, 0, at)) {
                bytes[at] = 0;
                *read_text = bytes;
            } else {
                free(bytes);
                ok = false;
                if (!*e->text)
                    sb_fail(e, "Clipboard changed, is too large, or is not valid UTF-8 text.");
            }
        }
    } else if (write) {
        char *copy = sb_strdup(text);
        if (!copy)
            goto done;
        /* X11 has no clipboard CAS. Hold the server only for the final compare and ownership change. */
        XGrabServer(d->x);
        server_grabbed = true;
        XSync(d->x, False);
        x_pump(d);
        if (expected != d->sequence) {
            free(copy);
            sb_fail(e, "Clipboard sequence changed; observe again.");
            goto done;
        }
        XSetSelectionOwner(d->x, d->clipboard, d->window, CurrentTime);
        XSync(d->x, False);
        if (XGetSelectionOwner(d->x, d->clipboard) != d->window) {
            free(copy);
            sb_fail(e, "Cannot own the X11 clipboard.");
            goto done;
        }
        if (d->owned) {
            memset(d->owned, 0, strlen(d->owned));
            free(d->owned);
        }
        d->owned = copy;
        x_pump(d);
        ok = true;
    } else if (XGetSelectionOwner(d->x, d->clipboard) == d->window) {
        *read_text = sb_strdup(d->owned ? d->owned : "");
        ok = *read_text != NULL;
    } else if (XGetSelectionOwner(d->x, d->clipboard) == None) {
        *read_text = sb_strdup("");
        ok = *read_text != NULL;
    } else {
        Window owner = XGetSelectionOwner(d->x, d->clipboard);
        uint32_t before = d->sequence;
        d->selection_done = false;
        XDeleteProperty(d->x, d->window, d->property);
        XConvertSelection(d->x, d->clipboard, d->utf8, d->property, d->window, CurrentTime);
        XFlush(d->x);
        uint64_t end = sb_platform_millis() + 2000;
        while (!d->selection_done && sb_platform_millis() < end && !atomic_load(&d->cancelled)) {
            x_pump(d);
            g_usleep(5000);
        }
        Atom type;
        int format;
        unsigned long n, after;
        unsigned char *bytes = NULL;
        if (d->selection_done && d->selection_result == d->property &&
            XGetWindowProperty(d->x, d->window, d->property, 0, 8193, True, AnyPropertyType, &type, &format,
                               &n, &after, &bytes) == Success &&
            type == d->utf8 && format == 8 && n <= 32768 && !after && before == d->sequence &&
            owner == XGetSelectionOwner(d->x, d->clipboard) &&
            g_utf8_validate((char *)bytes, (gssize)n, NULL) && !memchr(bytes, 0, n)) {
            *read_text = malloc(n + 1);
            if (*read_text) {
                memcpy(*read_text, bytes, n);
                (*read_text)[n] = 0;
                ok = true;
            }
        }
        if (bytes)
            XFree(bytes);
        XDeleteProperty(d->x, d->window, d->property);
        if (!ok)
            sb_fail(e, "X11 clipboard changed, exceeded the text budget, or did not provide UTF-8 text.");
    }
done:
    if (server_grabbed) {
        XUngrabServer(d->x);
        XFlush(d->x);
    }
    *sequence = d->sequence;
    pthread_mutex_unlock(&d->lock);
    return ok;
}
