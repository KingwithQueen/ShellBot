#include "desktop.h"
#import <Cocoa/Cocoa.h>
#import <ApplicationServices/ApplicationServices.h>
#import <ScreenCaptureKit/ScreenCaptureKit.h>
#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>
#include <libproc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const int64_t event_tag = 0x53424f5435504f53;
struct sb_os_desktop {
    sb_desktop_policy policy;
    sb_desktop_interrupt interrupt;
    sb_desktop_guard guard;
    void *context;
    atomic_bool ready, cancelled;
    CFMachPortRef tap;
    CFRunLoopSourceRef source;
    CFRunLoopRef loop;
};
/* The stream and all asynchronous callbacks retain this object until capture has stopped. */
@interface SBFrame : NSObject <SCStreamOutput, SCStreamDelegate>
@property dispatch_semaphore_t signal;
@property CVPixelBufferRef pixel;
@property NSError *error;
@end
@implementation SBFrame
- (instancetype)init {
    if ((self = [super init]))
        _signal = dispatch_semaphore_create(0);
    return self;
}
- (void)dealloc {
    if (_pixel)
        CVPixelBufferRelease(_pixel);
}
- (void)stream:(SCStream *)stream didStopWithError:(NSError *)error {
    (void)stream;
    @synchronized(self) {
        self.error = error;
    }
    dispatch_semaphore_signal(self.signal);
}
- (void)stream:(SCStream *)stream
    didOutputSampleBuffer:(CMSampleBufferRef)sample
                   ofType:(SCStreamOutputType)type {
    (void)stream;
    if (type != SCStreamOutputTypeScreen || !CMSampleBufferIsValid(sample))
        return;
    CFArrayRef attachments = CMSampleBufferGetSampleAttachmentsArray(sample, false);
    if (!attachments || !CFArrayGetCount(attachments))
        return;
    NSDictionary *info = (__bridge NSDictionary *)CFArrayGetValueAtIndex(attachments, 0);
    if ([info[SCStreamFrameInfoStatus] integerValue] != SCFrameStatusComplete)
        return;
    CVPixelBufferRef pixel = CMSampleBufferGetImageBuffer(sample);
    if (!pixel)
        return;
    @synchronized(self) {
        if (!self.pixel) {
            self.pixel = CVPixelBufferRetain(pixel);
            dispatch_semaphore_signal(self.signal);
        }
    }
}
@end
@interface SBShare : NSObject
@property dispatch_semaphore_t signal;
@property SCShareableContent *content;
@property NSError *error;
@end
@implementation SBShare
- (instancetype)init {
    if ((self = [super init]))
        _signal = dispatch_semaphore_create(0);
    return self;
}
@end
static bool wait_signal(dispatch_semaphore_t s, unsigned seconds) {
    return dispatch_semaphore_wait(s, dispatch_time(DISPATCH_TIME_NOW, (int64_t)seconds * NSEC_PER_SEC)) == 0;
}
static bool fail(sb_error *e, const char *text) {
    sb_fail(e, text);
    return false;
}
static bool active(sb_os_desktop *d, sb_error *e) {
    if (atomic_load(&d->cancelled))
        return fail(e, "Desktop connection was cancelled.");
    CFDictionaryRef session = CGSessionCopyCurrentDictionary();
    bool unlocked = false;
    if (session) {
        CFBooleanRef on = CFDictionaryGetValue(session, kCGSessionOnConsoleKey),
                     login = CFDictionaryGetValue(session, kCGSessionLoginDoneKey),
                     locked = CFDictionaryGetValue(session, CFSTR("CGSSessionScreenIsLocked"));
        unlocked = on == kCFBooleanTrue && login == kCFBooleanTrue && locked != kCFBooleanTrue;
        CFRelease(session);
    }
    if (!unlocked) {
        d->interrupt(d->context);
        return fail(e, "No unlocked local console desktop.");
    }
    return true;
}
static CGEventRef observe(CGEventTapProxy proxy, CGEventType type, CGEventRef event, void *ctx) {
    (void)proxy;
    sb_os_desktop *d = ctx;
    if (type == kCGEventTapDisabledByTimeout || type == kCGEventTapDisabledByUserInput) {
        atomic_store(&d->ready, false);
        d->interrupt(d->context);
        return event;
    }
    if (CGEventGetIntegerValueField(event, kCGEventSourceUserData) != event_tag)
        d->interrupt(d->context);
    return event;
}
sb_os_desktop *sb_os_desktop_new(const sb_desktop_policy *policy, sb_desktop_interrupt interrupt,
                                 sb_desktop_guard guard, void *context, sb_error *e) {
    (void)e;
    sb_os_desktop *d = calloc(1, sizeof(*d));
    if (d) {
        d->policy = *policy;
        d->interrupt = interrupt;
        d->guard = guard;
        d->context = context;
        atomic_init(&d->ready, false);
        atomic_init(&d->cancelled, false);
    }
    return d;
}
bool sb_os_desktop_connect(sb_os_desktop *d, sb_error *e) {
    @autoreleasepool {
        if (!active(d, e))
            return false;
        /* Only an explicit local owner command reaches these system permission requests. */
        if (!CGPreflightScreenCaptureAccess()) {
            CGRequestScreenCaptureAccess();
            return fail(e, "Grant Screen Recording to ShellBot in System Settings, then restart the project "
                           "and connect again.");
        }
        if (d->policy.control) {
            NSDictionary *options = @{(__bridge NSString *)kAXTrustedCheckOptionPrompt : @YES};
            if (!AXIsProcessTrustedWithOptions((__bridge CFDictionaryRef)options))
                return fail(e, "Grant Accessibility to ShellBot in System Settings, then restart the project "
                               "and connect again.");
            if (!CGPreflightListenEventAccess()) {
                CGRequestListenEventAccess();
                return fail(
                    e, "Grant Input Monitoring to ShellBot, then restart the project and connect again.");
            }
            CGEventMask mask = CGEventMaskBit(kCGEventKeyDown) | CGEventMaskBit(kCGEventFlagsChanged) |
                               CGEventMaskBit(kCGEventMouseMoved) | CGEventMaskBit(kCGEventLeftMouseDown) |
                               CGEventMaskBit(kCGEventRightMouseDown) |
                               CGEventMaskBit(kCGEventOtherMouseDown) | CGEventMaskBit(kCGEventScrollWheel) |
                               CGEventMaskBit(kCGEventLeftMouseDragged) |
                               CGEventMaskBit(kCGEventRightMouseDragged);
            d->tap = CGEventTapCreate(kCGSessionEventTap, kCGHeadInsertEventTap, kCGEventTapOptionListenOnly,
                                      mask, observe, d);
            if (!d->tap)
                return fail(e, "Cannot monitor local human input; desktop control remains disconnected.");
            d->source = CFMachPortCreateRunLoopSource(NULL, d->tap, 0);
            d->loop = CFRunLoopGetCurrent();
            CFRetain(d->loop);
            CFRunLoopAddSource(d->loop, d->source, kCFRunLoopDefaultMode);
            CGEventTapEnable(d->tap, true);
        }
        atomic_store(&d->ready, true);
        return true;
    }
}
bool sb_os_desktop_ready(sb_os_desktop *d) {
    return d && atomic_load(&d->ready) && !atomic_load(&d->cancelled);
}
const char *sb_os_desktop_provider(sb_os_desktop *d) {
    (void)d;
    return "macOS ScreenCaptureKit";
}
bool sb_os_desktop_human_pause(sb_os_desktop *d) { return d->policy.control; }
yyjson_mut_val *sb_os_desktop_permissions(yyjson_mut_doc *doc) {
    yyjson_mut_val *v = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_str(doc, v, "platform", "macos");
    yyjson_mut_obj_add_bool(doc, v, "screen_recording", CGPreflightScreenCaptureAccess());
    yyjson_mut_obj_add_bool(doc, v, "accessibility", AXIsProcessTrusted());
    yyjson_mut_obj_add_bool(doc, v, "input_monitoring", CGPreflightListenEventAccess());
    yyjson_mut_obj_add_bool(doc, v, "ssh_cannot_grant_permissions", true);
    return v;
}
void sb_os_desktop_pump(sb_os_desktop *d) {
    if (d->loop)
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0, true);
    if (atomic_load(&d->ready) &&
        (!CGPreflightScreenCaptureAccess() ||
         (d->policy.control && (!AXIsProcessTrusted() || !CGPreflightListenEventAccess())))) {
        atomic_store(&d->ready, false);
        d->interrupt(d->context);
    }
}
void sb_os_desktop_cancel(sb_os_desktop *d) {
    if (d) {
        atomic_store(&d->cancelled, true);
        if (d->loop)
            CFRunLoopWakeUp(d->loop);
    }
}
void sb_os_desktop_free(sb_os_desktop *d) {
    if (!d)
        return;
    if (d->source) {
        CFRunLoopRemoveSource(d->loop, d->source, kCFRunLoopDefaultMode);
        CFRelease(d->source);
    }
    if (d->tap) {
        CGEventTapEnable(d->tap, false);
        CFMachPortInvalidate(d->tap);
        CFRelease(d->tap);
    }
    if (d->loop)
        CFRelease(d->loop);
    free(d);
}
static bool same_uid(pid_t pid) {
    struct proc_bsdinfo info;
    return pid > 0 && proc_pidinfo(pid, PROC_PIDTBSDINFO, 0, &info, sizeof(info)) == (int)sizeof(info) &&
           info.pbi_uid == geteuid() && info.pbi_ruid == geteuid();
}
static bool target_owned(CGPoint point) {
    CFArrayRef list = CGWindowListCopyWindowInfo(
        kCGWindowListOptionOnScreenOnly | kCGWindowListExcludeDesktopElements, kCGNullWindowID);
    bool allowed = false;
    for (NSDictionary *w in (__bridge NSArray *)list) {
        CGRect b;
        if (!CGRectMakeWithDictionaryRepresentation(
                (__bridge CFDictionaryRef)w[(__bridge NSString *)kCGWindowBounds], &b) ||
            !CGRectContainsPoint(b, point) || [w[(__bridge NSString *)kCGWindowAlpha] doubleValue] < 0.01)
            continue;
        allowed = same_uid([w[(__bridge NSString *)kCGWindowOwnerPID] intValue]);
        break;
    }
    if (list)
        CFRelease(list);
    return allowed;
}
static sb_drect rect(CGRect r) {
    return (sb_drect){(int)r.origin.x, (int)r.origin.y, (int)r.size.width, (int)r.size.height};
}
bool sb_os_desktop_view(sb_os_desktop *d, sb_dview *view, sb_error *e) {
    @autoreleasepool {
        if (!sb_os_desktop_ready(d) || !active(d, e) || !CGPreflightScreenCaptureAccess())
            return false;
        memset(view, 0, sizeof(*view));
        CGDirectDisplayID ids[16];
        uint32_t n = 0;
        if (CGGetActiveDisplayList(16, ids, &n) != kCGErrorSuccess || !n)
            return fail(e, "No active permitted display.");
        for (unsigned i = 0; i < n; i++) {
            CGRect b = CGDisplayBounds(ids[i]);
            view->monitors[view->count++] =
                (sb_dmonitor){ids[i], rect(b), CGDisplayPixelsWide(ids[i]) / b.size.width};
        }
        pid_t front = NSWorkspace.sharedWorkspace.frontmostApplication.processIdentifier;
        CFArrayRef list = CGWindowListCopyWindowInfo(
            kCGWindowListOptionOnScreenOnly | kCGWindowListExcludeDesktopElements, kCGNullWindowID);
        for (NSDictionary *w in (__bridge NSArray *)list) {
            pid_t pid = [w[(__bridge NSString *)kCGWindowOwnerPID] intValue];
            if ([w[(__bridge NSString *)kCGWindowLayer] intValue] != 0 || !same_uid(pid))
                continue;
            CGRect b;
            CGRectMakeWithDictionaryRepresentation(
                (__bridge CFDictionaryRef)w[(__bridge NSString *)kCGWindowBounds], &b);
            if (b.size.width <= 0 || b.size.height <= 0)
                continue;
            uint64_t id = [w[(__bridge NSString *)kCGWindowNumber] unsignedLongLongValue];
            if (pid == front && !view->focus) {
                view->focus = id;
                view->focus_bounds = rect(b);
            }
            if (view->window_count < 64) {
                sb_dwindow *entry = &view->windows[view->window_count++];
                entry->native = id;
                entry->pid = pid;
                entry->bounds = rect(b);
                NSString *title = w[(__bridge NSString *)kCGWindowName];
                snprintf(entry->title, sizeof(entry->title), "%.500s", title.UTF8String ?: "");
            }
        }
        if (list)
            CFRelease(list);
        view->clipboard = (uint32_t)NSPasteboard.generalPasteboard.changeCount;
        char layout[2048];
        size_t at = 0;
        for (unsigned i = 0; i < view->count; i++) {
            sb_dmonitor m = view->monitors[i];
            at += (size_t)snprintf(layout + at, sizeof(layout) - at, "%llu:%d,%d,%d,%d:%.3f;",
                                   (unsigned long long)m.native, m.bounds.x, m.bounds.y, m.bounds.width,
                                   m.bounds.height, m.scale);
        }
        sb_platform_sha256(layout, at, view->layout, e);
        return true;
    }
}
bool sb_os_desktop_capture(sb_os_desktop *d, const sb_dview *view, unsigned monitor, const sb_drect *crop,
                           unsigned maximum, unsigned char **out, unsigned *width, unsigned *height,
                           sb_error *e) {
    @autoreleasepool {
        if (!active(d, e) || !CGPreflightScreenCaptureAccess())
            return fail(e, "Screen Recording permission is unavailable.");
        SBShare *share = [SBShare new];
        [SCShareableContent
            getShareableContentExcludingDesktopWindows:NO
                                   onScreenWindowsOnly:YES
                                     completionHandler:^(SCShareableContent *content, NSError *error) {
                                       @synchronized(share) {
                                           share.content = content;
                                           share.error = error;
                                       }
                                       dispatch_semaphore_signal(share.signal);
                                     }];
        if (!wait_signal(share.signal, 5) || !share.content)
            return fail(e, "ScreenCaptureKit could not enumerate the permitted displays.");
        SCDisplay *display = nil;
        for (SCDisplay *item in share.content.displays)
            if (item.displayID == view->monitors[monitor].native)
                display = item;
        if (!display)
            return fail(e, "The observed display is no longer available.");
        double scale = maximum / (double)(crop->width > crop->height ? crop->width : crop->height);
        if (scale > 1)
            scale = 1;
        unsigned w = (unsigned)(crop->width * scale), h = (unsigned)(crop->height * scale);
        if (!w)
            w = 1;
        if (!h)
            h = 1;
        SCContentFilter *filter = [[SCContentFilter alloc] initWithDisplay:display excludingWindows:@[]];
        SCStreamConfiguration *config = [SCStreamConfiguration new];
        sb_drect bounds = view->monitors[monitor].bounds;
        config.sourceRect = CGRectMake(crop->x - bounds.x, crop->y - bounds.y, crop->width, crop->height);
        config.width = w;
        config.height = h;
        config.pixelFormat = kCVPixelFormatType_32BGRA;
        config.showsCursor = YES;
        config.queueDepth = 3;
        config.minimumFrameInterval = CMTimeMake(1, 30);
        SBFrame *frame = [SBFrame new];
        SCStream *stream = [[SCStream alloc] initWithFilter:filter configuration:config delegate:frame];
        NSError *error = nil;
        dispatch_queue_t queue = dispatch_queue_create("org.shellbot.capture", DISPATCH_QUEUE_SERIAL);
        if (![stream addStreamOutput:frame
                                type:SCStreamOutputTypeScreen
                  sampleHandlerQueue:queue
                               error:&error])
            return fail(e, "Cannot attach a ScreenCaptureKit frame output.");
        SBShare *start = [SBShare new];
        [stream startCaptureWithCompletionHandler:^(NSError *err) {
          start.error = err;
          dispatch_semaphore_signal(start.signal);
        }];
        bool ok = wait_signal(start.signal, 5) && !start.error && wait_signal(frame.signal, 5);
        SBShare *stop = [SBShare new];
        [stream stopCaptureWithCompletionHandler:^(NSError *err) {
          stop.error = err;
          dispatch_semaphore_signal(stop.signal);
        }];
        bool stopped = wait_signal(stop.signal, 5);
        (void)stopped;
        CVPixelBufferRef pixel = NULL;
        @synchronized(frame) {
            if (frame.pixel)
                pixel = CVPixelBufferRetain(frame.pixel);
        }
        if (!ok || !pixel) {
            if (pixel)
                CVPixelBufferRelease(pixel);
            return fail(e, "No complete ScreenCaptureKit frame was delivered.");
        }
        unsigned sw = (unsigned)CVPixelBufferGetWidth(pixel), sh = (unsigned)CVPixelBufferGetHeight(pixel);
        unsigned char *bytes = malloc((size_t)w * h * 4);
        if (!bytes) {
            CVPixelBufferRelease(pixel);
            return false;
        }
        CVPixelBufferLockBaseAddress(pixel, kCVPixelBufferLock_ReadOnly);
        const unsigned char *source = CVPixelBufferGetBaseAddress(pixel);
        size_t stride = CVPixelBufferGetBytesPerRow(pixel);
        for (unsigned y = 0; y < h; y++)
            for (unsigned x = 0; x < w; x++)
                memcpy(bytes + ((size_t)y * w + x) * 4,
                       source + (size_t)(y * sh / h) * stride + (x * sw / w) * 4, 4);
        CVPixelBufferUnlockBaseAddress(pixel, kCVPixelBufferLock_ReadOnly);
        CVPixelBufferRelease(pixel);
        if (!active(d, e)) {
            free(bytes);
            return false;
        }
        *out = bytes;
        *width = w;
        *height = h;
        return true;
    }
}
static const char *str(yyjson_val *v, const char *key) {
    const char *s = sb_string(yyjson_obj_get(v, key));
    return s ? s : "";
}
static int num(yyjson_val *v, const char *key, int fallback) {
    yyjson_val *n = yyjson_obj_get(v, key);
    return n ? (int)yyjson_get_sint(n) : fallback;
}
static int key(const char *k) {
    struct {
        const char *name;
        int code;
    } keys[] = {{"CTRL", 59},      {"SHIFT", 56},   {"ALT", 58},       {"META", 55},  {"ENTER", 36},
                {"TAB", 48},       {"SPACE", 49},   {"BACKSPACE", 51}, {"ESC", 53},   {"ESCAPE", 53},
                {"DELETE", 117},   {"INSERT", 114}, {"HOME", 115},     {"END", 119},  {"PAGEUP", 116},
                {"PAGEDOWN", 121}, {"UP", 126},     {"DOWN", 125},     {"LEFT", 123}, {"RIGHT", 124},
                {"F1", 122},       {"F2", 120},     {"F3", 99},        {"F4", 118},   {"F5", 96},
                {"F6", 97},        {"F7", 98},      {"F8", 100},       {"F9", 101},   {"F10", 109},
                {"F11", 103},      {"F12", 111}};
    for (unsigned i = 0; i < sizeof(keys) / sizeof(keys[0]); i++)
        if (!strcmp(k, keys[i].name))
            return keys[i].code;
    /* Named shortcuts use physical US key positions; desktop_type provides Unicode text. */
    static const int codes[26] = {0,  11, 8,  2,  14, 3, 5,  4,  34, 38, 40, 37, 46,
                                  45, 31, 35, 12, 15, 1, 17, 32, 9,  13, 7,  16, 6};
    if (strlen(k) == 1 && *k >= 'A' && *k <= 'Z')
        return codes[*k - 'A'];
    if (strlen(k) == 1 && *k >= 'a' && *k <= 'z')
        return codes[*k - 'a'];
    static const int digits[] = {29, 18, 19, 20, 21, 23, 22, 26, 28, 25};
    if (strlen(k) == 1 && *k >= '0' && *k <= '9')
        return digits[*k - '0'];
    return -1;
}
static CGEventFlags flags(int code) {
    return code == 59   ? kCGEventFlagMaskControl
           : code == 56 ? kCGEventFlagMaskShift
           : code == 58 ? kCGEventFlagMaskAlternate
           : code == 55 ? kCGEventFlagMaskCommand
                        : 0;
}
static bool allowed(sb_os_desktop *d, sb_error *e) {
    return active(d, e) && d->guard(d->context) && sb_os_desktop_ready(d) && AXIsProcessTrusted() &&
                   CGPreflightListenEventAccess()
               ? true
               : fail(e, "Desktop action interrupted, permission revoked, or lease expired.");
}
static bool post(sb_os_desktop *d, CGEventRef event, bool cleanup, sb_error *e) {
    if (!event)
        return fail(e, "Cannot allocate a native input event.");
    bool ok = cleanup || allowed(d, e);
    if (ok) {
        CGEventSetIntegerValueField(event, kCGEventSourceUserData, event_tag);
        CGEventPost(kCGSessionEventTap, event);
    }
    CFRelease(event);
    return ok;
}
bool sb_os_desktop_input(sb_os_desktop *d, const char *name, yyjson_val *v, const sb_dview *view,
                         const sb_dwindow *window, unsigned *sent, sb_error *e) {
    @autoreleasepool {
        if (!d->policy.control || !active(d, e) || !allowed(d, e))
            return false;
        CGEventFlags held = CGEventSourceFlagsState(kCGEventSourceStateCombinedSessionState);
        if (held & (kCGEventFlagMaskCommand | kCGEventFlagMaskControl | kCGEventFlagMaskAlternate |
                    kCGEventFlagMaskShift) ||
            CGEventSourceButtonState(kCGEventSourceStateCombinedSessionState, kCGMouseButtonLeft) ||
            CGEventSourceButtonState(kCGEventSourceStateCombinedSessionState, kCGMouseButtonRight) ||
            CGEventSourceButtonState(kCGEventSourceStateCombinedSessionState, kCGMouseButtonCenter))
            return fail(e, "A user modifier or mouse button is held.");
        if (!strcmp(name, "window_focus")) {
            sb_dview fresh;
            if (!window || !same_uid(window->pid) || !sb_os_desktop_view(d, &fresh, e))
                return fail(e, "Window is no longer a current-user application.");
            bool found = false;
            for (unsigned i = 0; i < fresh.window_count; i++)
                if (fresh.windows[i].native == window->native && fresh.windows[i].pid == window->pid &&
                    !memcmp(&fresh.windows[i].bounds, &window->bounds, sizeof(sb_drect)))
                    found = true;
            if (!found)
                return fail(e, "Window identity changed.");
            AXUIElementRef app = AXUIElementCreateApplication(window->pid);
            CFTypeRef list = NULL;
            AXError result = AXUIElementCopyAttributeValue(app, kAXWindowsAttribute, &list);
            AXUIElementRef target = NULL;
            if (result == kAXErrorSuccess && list) {
                for (id object in (__bridge NSArray *)list) {
                    AXUIElementRef item = (__bridge AXUIElementRef)object;
                    CFTypeRef pos = NULL, size = NULL;
                    AXUIElementCopyAttributeValue(item, kAXPositionAttribute, &pos);
                    AXUIElementCopyAttributeValue(item, kAXSizeAttribute, &size);
                    CGPoint p = {0};
                    CGSize s = {0};
                    if (pos && size && AXValueGetValue(pos, kAXValueCGPointType, &p) &&
                        AXValueGetValue(size, kAXValueCGSizeType, &s) &&
                        !memcmp(&(sb_drect){(int)p.x, (int)p.y, (int)s.width, (int)s.height}, &window->bounds,
                                sizeof(sb_drect))) {
                        if (target) {
                            CFRelease(target);
                            target = NULL;
                            if (pos)
                                CFRelease(pos);
                            if (size)
                                CFRelease(size);
                            break;
                        }
                        target = (AXUIElementRef)CFRetain(item);
                    }
                    if (pos)
                        CFRelease(pos);
                    if (size)
                        CFRelease(size);
                }
            }
            bool ok = target && allowed(d, e) &&
                      AXUIElementPerformAction(target, kAXRaiseAction) == kAXErrorSuccess;
            if (ok)
                ok = [[NSRunningApplication runningApplicationWithProcessIdentifier:window->pid]
                    activateWithOptions:NSApplicationActivateIgnoringOtherApps];
            if (target)
                CFRelease(target);
            if (list)
                CFRelease(list);
            CFRelease(app);
            if (ok)
                (*sent)++;
            else if (!*e->text)
                fail(e, "Cannot uniquely match and raise this accessibility window.");
            return ok;
        }
        pid_t foreground = NSWorkspace.sharedWorkspace.frontmostApplication.processIdentifier;
        if (!view->focus || !same_uid(foreground))
            return fail(e, "Foreground application is not a verified current-user process.");
        if (!strcmp(name, "desktop_type")) {
            NSString *text = [[NSString alloc] initWithUTF8String:str(v, "text")];
            if (!text)
                return fail(e, "Text must be valid UTF-8.");
            UniChar units[4096];
            NSUInteger n = text.length;
            if (n > 4096)
                return fail(e, "Text exceeds the Unicode input budget.");
            [text getCharacters:units range:NSMakeRange(0, n)];
            for (NSUInteger i = 0; i < n;) {
                NSUInteger count = 1;
                if (CFStringIsSurrogateHighCharacter(units[i]) && i + 1 < n &&
                    CFStringIsSurrogateLowCharacter(units[i + 1]))
                    count = 2;
                CGEventRef down = CGEventCreateKeyboardEvent(NULL, 0, true);
                CGEventKeyboardSetUnicodeString(down, count, units + i);
                if (!post(d, down, false, e))
                    return false;
                (*sent)++;
                CGEventRef up = CGEventCreateKeyboardEvent(NULL, 0, false);
                CGEventKeyboardSetUnicodeString(up, count, units + i);
                post(d, up, true, e);
                (*sent)++;
                i += count;
            }
            return true;
        }
        if (!strcmp(name, "desktop_key")) {
            yyjson_val *keys = yyjson_obj_get(v, "keys");
            unsigned n = (unsigned)yyjson_arr_size(keys), pressed = 0;
            int codes[4];
            CGEventFlags f = 0;
            for (unsigned i = 0; i < n; i++) {
                codes[i] = key(sb_string(yyjson_arr_get(keys, i)));
                if (codes[i] < 0)
                    return fail(e, "Unsupported keyboard key.");
            }
            bool ok = true;
            for (unsigned i = 0; i < n; i++) {
                f |= flags(codes[i]);
                CGEventRef event = CGEventCreateKeyboardEvent(NULL, (CGKeyCode)codes[i], true);
                CGEventSetFlags(event, f);
                if (!post(d, event, false, e)) {
                    ok = false;
                    break;
                }
                pressed++;
                (*sent)++;
            }
            while (pressed) {
                int code = codes[--pressed];
                f &= ~flags(code);
                CGEventRef event = CGEventCreateKeyboardEvent(NULL, (CGKeyCode)code, false);
                CGEventSetFlags(event, f);
                post(d, event, true, e);
                (*sent)++;
            }
            return ok;
        }
        CGPoint p = CGPointMake(num(v, "x", 0), num(v, "y", 0));
        if (!target_owned(p) ||
            (!strcmp(name, "desktop_drag") &&
             !target_owned(CGPointMake(num(v, "to_x", (int)p.x), num(v, "to_y", (int)p.y)))))
            return fail(e, "Pointer target is not a verified current-user application.");
        CGMouseButton button = !strcmp(str(v, "button"), "right")    ? kCGMouseButtonRight
                               : !strcmp(str(v, "button"), "middle") ? kCGMouseButtonCenter
                                                                     : kCGMouseButtonLeft;
        CGEventType down = button == 0   ? kCGEventLeftMouseDown
                           : button == 1 ? kCGEventRightMouseDown
                                         : kCGEventOtherMouseDown,
                    up = button == 0   ? kCGEventLeftMouseUp
                         : button == 1 ? kCGEventRightMouseUp
                                       : kCGEventOtherMouseUp,
                    drag = button == 0   ? kCGEventLeftMouseDragged
                           : button == 1 ? kCGEventRightMouseDragged
                                         : kCGEventOtherMouseDragged;
        if (!post(d, CGEventCreateMouseEvent(NULL, kCGEventMouseMoved, p, button), false, e))
            return false;
        (*sent)++;
        if (!strcmp(name, "desktop_scroll")) {
            int delta = num(v, "delta", 0);
            bool horizontal = yyjson_get_bool(yyjson_obj_get(v, "horizontal"));
            bool ok = post(d,
                           CGEventCreateScrollWheelEvent(NULL, kCGScrollEventUnitPixel, 2,
                                                         horizontal ? 0 : delta, horizontal ? delta : 0),
                           false, e);
            if (ok)
                (*sent)++;
            return ok;
        }
        if (!strcmp(name, "desktop_click")) {
            for (int i = 1; i <= num(v, "clicks", 1); i++) {
                CGEventRef event = CGEventCreateMouseEvent(NULL, down, p, button);
                CGEventSetIntegerValueField(event, kCGMouseEventClickState, i);
                if (!post(d, event, false, e))
                    return false;
                (*sent)++;
                event = CGEventCreateMouseEvent(NULL, up, p, button);
                CGEventSetIntegerValueField(event, kCGMouseEventClickState, i);
                post(d, event, true, e);
                (*sent)++;
            }
            return true;
        }
        if (!post(d, CGEventCreateMouseEvent(NULL, down, p, button), false, e))
            return false;
        (*sent)++;
        int duration = num(v, "duration_ms", 200);
        CGPoint end = CGPointMake(num(v, "to_x", (int)p.x), num(v, "to_y", (int)p.y)), last = p;
        bool ok = true;
        for (int i = 1; i <= 20; i++) {
            usleep((useconds_t)duration * 50);
            last = CGPointMake(p.x + (end.x - p.x) * i / 20, p.y + (end.y - p.y) * i / 20);
            if (!post(d, CGEventCreateMouseEvent(NULL, drag, last, button), false, e)) {
                ok = false;
                break;
            }
            (*sent)++;
        }
        post(d, CGEventCreateMouseEvent(NULL, up, last, button), true, e);
        (*sent)++;
        return ok;
    }
}
bool sb_os_desktop_clipboard(sb_os_desktop *d, bool write, uint32_t expected, const char *text, char **read,
                             uint32_t *sequence, sb_error *e) {
    @autoreleasepool {
        if (!active(d, e))
            return false;
        NSPasteboard *paste = NSPasteboard.generalPasteboard;
        uint32_t before = (uint32_t)paste.changeCount;
        if (before != expected)
            return fail(e, "Clipboard sequence changed; observe again.");
        if (write) {
            NSString *value = [[NSString alloc] initWithUTF8String:text];
            if (!value)
                return fail(e, "Text must be valid UTF-8.");
            [paste clearContents];
            if (![paste setString:value forType:NSPasteboardTypeString])
                return fail(e, "Cannot write text to the macOS pasteboard.");
            *sequence = (uint32_t)paste.changeCount;
            return true;
        }
        NSString *value = [paste stringForType:NSPasteboardTypeString];
        NSData *data = value ? [value dataUsingEncoding:NSUTF8StringEncoding] : [NSData data];
        if (data.length > 32768 || data.length != strlen(value.UTF8String ?: ""))
            return fail(e, "Clipboard exceeds the text budget or contains embedded NUL.");
        if ((uint32_t)paste.changeCount != before)
            return fail(e, "Clipboard changed during the read.");
        *read = sb_strdup(value.UTF8String ?: "");
        *sequence = before;
        return *read != NULL;
    }
}
