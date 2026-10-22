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
struct sb_os_desktop {
    sb_desktop_policy policy;
    sb_desktop_interrupt interrupt;
    sb_desktop_guard guard;
    void *context;
    bool wayland;
    atomic_bool ready, cancelled;
    pthread_mutex_t lock;
    Display *x;
    Window root, window;
    int xi, fixes;
    Atom clipboard, utf8, property;
    uint32_t sequence;
    unsigned char physical[256];
    char *owned;
    bool selection_done;
    Atom selection_result;
    GDBusConnection *bus;
    GMainContext *loop;
    GCancellable *cancel;
    char *session;
    guint subscriptions[4];
    guint node, devices;
    guint64 pipewire_serial;
    atomic_uint source_width, source_height;
    bool clipboard_enabled, clip_utf8;
    char clip_mime[40];
    sb_drect bounds;
    int pipefd;
    GstElement *pipeline, *sink;
    GstSample *sample;
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
static GstPadProbeReturn stream_caps(GstPad *pad, GstPadProbeInfo *info, gpointer context) {
    (void)pad;
    sb_os_desktop *d = context;
    GstEvent *event = GST_PAD_PROBE_INFO_EVENT(info);
    if (GST_EVENT_TYPE(event) != GST_EVENT_CAPS)
        return GST_PAD_PROBE_OK;
    GstCaps *caps = NULL;
    gst_event_parse_caps(event, &caps);
    GstVideoInfo video;
    if (gst_video_info_from_caps(&video, caps)) {
        unsigned w = GST_VIDEO_INFO_WIDTH(&video), h = GST_VIDEO_INFO_HEIGHT(&video),
                 previous_w = atomic_exchange(&d->source_width, w),
                 previous_h = atomic_exchange(&d->source_height, h);
        if (!w || !h || w > 32768 || h > 32768 || (previous_w && (previous_w != w || previous_h != h))) {
            atomic_store(&d->ready, false);
            d->interrupt(d->context);
        }
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
    g_variant_builder_add(&b, "{sv}", "multiple", g_variant_new_boolean(false));
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
    if (streams && g_variant_n_children(streams) == 1) {
        GVariant *entry = g_variant_get_child_value(streams, 0), *props = NULL;
        g_variant_get(entry, "(u@a{sv})", &d->node, &props);
        gint x = 0, y = 0, w = 0, h = 0;
        g_variant_lookup(props, "position", "(ii)", &x, &y);
        g_variant_lookup(props, "size", "(ii)", &w, &h);
        g_variant_lookup(props, "pipewire-serial", "t", &d->pipewire_serial);
        d->bounds = (sb_drect){x, y, w, h};
        g_variant_unref(props);
        g_variant_unref(entry);
    }
    if (streams)
        g_variant_unref(streams);
    g_variant_unref(v);
    if (!d->node || d->bounds.width <= 0 || d->bounds.height <= 0 || d->bounds.width > 32768 ||
        d->bounds.height > 32768 || (d->policy.control && (d->devices & 3) != 3)) {
        sb_fail(e, "Portal did not grant the requested monitor and input devices.");
        return false;
    }
    d->pipefd = fd_call(d, SCREEN, "OpenPipeWireRemote", g_variant_new("(o@a{sv})", d->session, empty()), e);
    if (d->pipefd < 0)
        return false;
    gst_init(NULL, NULL);
    d->pipeline = gst_pipeline_new(NULL);
    GstElement *source = gst_element_factory_make("pipewiresrc", NULL),
               *convert = gst_element_factory_make("videoconvert", NULL);
    d->sink = gst_element_factory_make("appsink", NULL);
    if (!d->pipeline || !source || !convert || !d->sink) {
        if (source)
            gst_object_unref(source);
        if (convert)
            gst_object_unref(convert);
        if (d->sink)
            gst_object_unref(d->sink);
        d->sink = NULL;
        sb_fail(e, "PipeWire GStreamer source/converter/appsink is unavailable.");
        return false;
    }
    char node[32];
    snprintf(node, sizeof(node), "%u", d->node);
    g_object_set(source, "fd", d->pipefd, "path", node, "do-timestamp", TRUE, NULL);
    if (d->pipewire_serial && g_object_class_find_property(G_OBJECT_GET_CLASS(source), "target-object")) {
        char serial[32];
        snprintf(serial, sizeof(serial), "%llu", (unsigned long long)d->pipewire_serial);
        g_object_set(source, "target-object", serial, NULL);
    }
    GstCaps *caps = gst_caps_new_simple("video/x-raw", "format", G_TYPE_STRING, "BGRA", NULL);
    g_object_set(d->sink, "caps", caps, "max-buffers", 1, "drop", TRUE, "sync", FALSE, NULL);
    gst_caps_unref(caps);
    gst_bin_add_many(GST_BIN(d->pipeline), source, convert, d->sink, NULL);
    GstPad *pad = gst_element_get_static_pad(d->sink, "sink");
    gst_pad_add_probe(pad, GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM, stream_caps, d, NULL);
    gst_object_unref(pad);
    if (!gst_element_link_many(source, convert, d->sink, NULL) ||
        gst_element_set_state(d->pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
        sb_fail(e, "Cannot start the permitted PipeWire stream.");
        return false;
    }
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
static void x_pump(sb_os_desktop *d) {
    while (XPending(d->x)) {
        XEvent event;
        XNextEvent(d->x, &event);
        if (event.type == d->fixes + XFixesSelectionNotify)
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
            if (event.xcookie.evtype == XI_RawKeyPress || event.xcookie.evtype == XI_RawButtonPress ||
                event.xcookie.evtype == XI_RawMotion) {
                XIRawEvent *raw = event.xcookie.data;
                if (raw->sourceid > 0 && raw->sourceid < 256 && d->physical[raw->sourceid])
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
    if (!XTestQueryExtension(d->x, &a, &b, &c, &a) ||
        !XQueryExtension(d->x, "XInputExtension", &d->xi, &a, &b) ||
        !XFixesQueryExtension(d->x, &d->fixes, &a)) {
        sb_fail(e, "X11 requires XTest, XInput2 and XFixes.");
        return false;
    }
    int major = 2, minor = 0;
    if (XIQueryVersion(d->x, &major, &minor) != Success) {
        sb_fail(e, "XInput2 is unavailable.");
        return false;
    }
    int n = 0;
    XIDeviceInfo *devices = XIQueryDevice(d->x, XIAllDevices, &n);
    for (int i = 0; i < n; i++)
        if (devices[i].deviceid < 256 && devices[i].deviceid > 0 &&
            (devices[i].use == XISlavePointer || devices[i].use == XISlaveKeyboard) &&
            !strstr(devices[i].name, "XTEST"))
            d->physical[devices[i].deviceid] = 1;
    if (devices)
        XIFreeDeviceInfo(devices);
    unsigned char mask[XIMaskLen(XI_RawMotion)] = {0};
    XISetMask(mask, XI_RawKeyPress);
    XISetMask(mask, XI_RawButtonPress);
    XISetMask(mask, XI_RawMotion);
    XIEventMask ev = {XIAllMasterDevices, sizeof(mask), mask};
    XISelectEvents(d->x, d->root, &ev, 1);
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
    d->pipefd = -1;
    d->wayland = getenv("WAYLAND_DISPLAY") && *getenv("WAYLAND_DISPLAY");
    const char *type = getenv("XDG_SESSION_TYPE");
    if (type && !strcmp(type, "wayland"))
        d->wayland = true;
    pthread_mutex_init(&d->lock, NULL);
    atomic_init(&d->ready, false);
    atomic_init(&d->cancelled, false);
    atomic_init(&d->source_width, 0);
    atomic_init(&d->source_height, 0);
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
    return d && atomic_load(&d->ready) && !atomic_load(&d->cancelled);
}
const char *sb_os_desktop_provider(sb_os_desktop *d) {
    return d->wayland ? "Wayland portal / PipeWire" : "X11 / XTest";
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
        if (d->pipeline) {
            GstBus *bus = gst_element_get_bus(d->pipeline);
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
    if (d->pipeline) {
        gst_element_set_state(d->pipeline, GST_STATE_NULL);
        gst_object_unref(d->pipeline);
    }
    if (d->sample)
        gst_sample_unref(d->sample);
    if (d->pipefd >= 0)
        close(d->pipefd);
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
        view->count = 1;
        view->monitors[0] = (sb_dmonitor){d->node, d->bounds, 1};
    } else {
        x_pump(d);
        int n = 0;
        XRRMonitorInfo *mon = XRRGetMonitors(d->x, d->root, True, &n);
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
    char layout[2048];
    size_t at = 0;
    for (unsigned i = 0; i < view->count; i++) {
        sb_dmonitor m = view->monitors[i];
        at += (size_t)snprintf(layout + at, sizeof(layout) - at, "%llu:%d,%d,%d,%d;",
                               (unsigned long long)m.native, m.bounds.x, m.bounds.y, m.bounds.width,
                               m.bounds.height);
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
bool sb_os_desktop_capture(sb_os_desktop *d, const sb_dview *view, unsigned monitor, const sb_drect *crop,
                           unsigned maximum, unsigned char **out, unsigned *w, unsigned *h, sb_error *e) {
    (void)view;
    (void)monitor;
    if (!unlocked(d, e))
        return false;
    if (!d->wayland && (uint64_t)crop->width * crop->height > 16777216)
        return (sb_fail(e, "X11 source region exceeds 16 megapixels; use a smaller crop."), false);
    dimensions(crop->width, crop->height, maximum, w, h);
    unsigned char *pixels = malloc((size_t)*w * *h * 4);
    if (!pixels)
        return false;
    bool ok = false;
    if (d->wayland) {
        GstSample *sample = gst_app_sink_try_pull_sample(GST_APP_SINK(d->sink), 2 * GST_SECOND);
        pthread_mutex_lock(&d->lock);
        if (sample) {
            if (d->sample)
                gst_sample_unref(d->sample);
            d->sample = sample;
        }
        sample = d->sample ? gst_sample_ref(d->sample) : NULL;
        pthread_mutex_unlock(&d->lock);
        if (sample) {
            GstVideoInfo info;
            GstVideoFrame frame;
            if (gst_video_info_from_caps(&info, gst_sample_get_caps(sample)) &&
                GST_VIDEO_INFO_FORMAT(&info) == GST_VIDEO_FORMAT_BGRA &&
                gst_video_frame_map(&frame, &info, gst_sample_get_buffer(sample), GST_MAP_READ)) {
                int sw = GST_VIDEO_FRAME_WIDTH(&frame), sh = GST_VIDEO_FRAME_HEIGHT(&frame),
                    stride = GST_VIDEO_FRAME_PLANE_STRIDE(&frame, 0);
                unsigned char *src = GST_VIDEO_FRAME_PLANE_DATA(&frame, 0);
                for (unsigned y = 0; y < *h; y++)
                    for (unsigned x = 0; x < *w; x++) {
                        unsigned sx = (unsigned)(((crop->x - d->bounds.x) + x * (double)crop->width / *w) *
                                                 sw / d->bounds.width),
                                 sy = (unsigned)(((crop->y - d->bounds.y) + y * (double)crop->height / *h) *
                                                 sh / d->bounds.height);
                        if (sx >= (unsigned)sw)
                            sx = (unsigned)sw - 1;
                        if (sy >= (unsigned)sh)
                            sy = (unsigned)sh - 1;
                        memcpy(pixels + ((size_t)y * *w + x) * 4, src + (size_t)sy * stride + sx * 4, 4);
                    }
                ok = true;
                gst_video_frame_unmap(&frame);
            }
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
    if (!ok) {
        free(pixels);
        sb_fail(e, "No permitted desktop frame is available.");
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
    if (!d->wayland)
        x_pump(d);
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
static bool motion(sb_os_desktop *d, int x, int y, sb_error *e) {
    if (!guard(d, e))
        return false;
    if (d->wayland)
        return notify(d, "NotifyPointerMotionAbsolute",
                      g_variant_new("(o@a{sv}udd)", d->session, empty(), d->node, (double)(x - d->bounds.x),
                                    (double)(y - d->bounds.y)),
                      e);
    return XTestFakeMotionEvent(d->x, -1, x, y, CurrentTime) != 0;
}
static bool button(sb_os_desktop *d, unsigned button, bool down, sb_error *e) {
    if (down && !guard(d, e))
        return false;
    if (d->wayland) {
        int evdev = button == 1 ? 272 : button == 3 ? 273 : 274;
        return notify(d, "NotifyPointerButton",
                      g_variant_new("(o@a{sv}iu)", d->session, empty(), evdev, down ? 1u : 0u), e);
    }
    return XTestFakeButtonEvent(d->x, button, down, CurrentTime) != 0;
}
static bool keyboard(sb_os_desktop *d, KeySym sym, bool down, sb_error *e) {
    if (down && !guard(d, e))
        return false;
    if (d->wayland)
        return notify(d, "NotifyKeyboardKeysym",
                      g_variant_new("(o@a{sv}iu)", d->session, empty(), (gint)sym, down ? 1u : 0u), e);
    KeyCode code = XKeysymToKeycode(d->x, sym);
    return code && XTestFakeKeyEvent(d->x, code, down, CurrentTime);
}
static bool modifiers_clear(sb_os_desktop *d, sb_error *e) {
    if (d->wayland)
        return true;
    Window root, child;
    int rx, ry, wx, wy;
    unsigned mask;
    if (!XQueryPointer(d->x, d->root, &root, &child, &rx, &ry, &wx, &wy, &mask) ||
        (mask & (ShiftMask | ControlMask | Mod1Mask | Mod4Mask | Button1Mask | Button2Mask | Button3Mask))) {
        sb_fail(e, "A user modifier or mouse button is held.");
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
    unsigned held = 0, mouse = 0;
    if (!guard(d, e) || !modifiers_clear(d, e))
        goto done;
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
        for (unsigned i = 0; i < n; i++) {
            syms[i] = key(sb_string(yyjson_arr_get(keys, i)));
            if (!syms[i] || (!d->wayland && !XKeysymToKeycode(d->x, syms[i]))) {
                sb_fail(e, "Unsupported key in the current keyboard layout.");
                goto done;
            }
        }
        for (unsigned i = 0; i < n; i++) {
            if (!keyboard(d, syms[i], true, e))
                goto done;
            pressed[held++] = syms[i];
            (*sent)++;
        }
        ok = true;
    } else if (!strcmp(name, "desktop_type")) {
        const char *text = str(v, "text");
        if (!g_utf8_validate(text, -1, NULL)) {
            sb_fail(e, "Text must be valid UTF-8.");
            goto done;
        }
        /* Preflight the complete string before sending anything. X11 never remaps the global keyboard or
         * pastes via the clipboard. */
        for (const char *p = text; *p; p = g_utf8_next_char(p)) {
            gunichar ch = g_utf8_get_char(p);
            KeySym sym = ch == '\n' ? XK_Return : ch == '\t' ? XK_Tab : ch < 128 ? ch : 0x01000000 | ch;
            if (!d->wayland) {
                KeyCode code = XKeysymToKeycode(d->x, sym);
                if (!code || (XkbKeycodeToKeysym(d->x, code, 0, 0) != sym &&
                              XkbKeycodeToKeysym(d->x, code, 0, 1) != sym)) {
                    sb_fail(e, "X11 text is not directly representable in the current keyboard layout; use "
                               "explicit clipboard tools and paste.");
                    goto done;
                }
            }
        }
        for (const char *p = text; *p; p = g_utf8_next_char(p)) {
            gunichar ch = g_utf8_get_char(p);
            KeySym sym = ch == '\n' ? XK_Return : ch == '\t' ? XK_Tab : ch < 128 ? ch : 0x01000000 | ch;
            bool shift = !d->wayland && XkbKeycodeToKeysym(d->x, XKeysymToKeycode(d->x, sym), 0, 0) != sym;
            if (shift) {
                if (!keyboard(d, XK_Shift_L, true, e))
                    goto done;
                pressed[held++] = XK_Shift_L;
                (*sent)++;
            }
            if (!keyboard(d, sym, true, e))
                goto done;
            pressed[held++] = sym;
            (*sent)++;
            if (!keyboard(d, sym, false, e))
                goto done;
            held--;
            (*sent)++;
            if (shift) {
                keyboard(d, XK_Shift_L, false, e);
                held--;
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
                    if (!guard(d, e) || !XTestFakeButtonEvent(d->x, scroll, True, 0) ||
                        !XTestFakeButtonEvent(d->x, scroll, False, 0)) {
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
        keyboard(d, pressed[--held], false, e);
        (*sent)++;
    }
    if (mouse) {
        button(d, mouse, false, e);
        (*sent)++;
    }
    if (d->x)
        XSync(d->x, False);
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
