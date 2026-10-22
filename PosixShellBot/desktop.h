#ifndef SB_POSIX_DESKTOP_CONTROL_H
#define SB_POSIX_DESKTOP_CONTROL_H
#include "posix.h"
#include "sb_projects.h"
#define SB_DESKTOP_MONITORS 16
#define SB_DESKTOP_WINDOWS 64
typedef struct {
    int x, y, width, height;
} sb_drect;
typedef struct {
    uint64_t native;
    sb_drect bounds;
    double scale;
} sb_dmonitor;
typedef struct {
    uint64_t native;
    pid_t pid;
    sb_drect bounds;
    char title[513];
} sb_dwindow;
typedef struct {
    sb_dmonitor monitors[SB_DESKTOP_MONITORS];
    unsigned count;
    sb_dwindow windows[SB_DESKTOP_WINDOWS];
    unsigned window_count;
    uint64_t focus;
    sb_drect focus_bounds;
    uint32_t clipboard;
    char layout[65];
} sb_dview;
typedef struct sb_os_desktop sb_os_desktop;
typedef void (*sb_desktop_interrupt)(void *context);
typedef bool (*sb_desktop_guard)(void *context);
sb_os_desktop *sb_os_desktop_new(const sb_desktop_policy *policy, sb_desktop_interrupt interrupt,
                                 sb_desktop_guard guard, void *context, sb_error *error);
bool sb_os_desktop_connect(sb_os_desktop *os, sb_error *error);
bool sb_os_desktop_ready(sb_os_desktop *os);
void sb_os_desktop_pump(sb_os_desktop *os);
void sb_os_desktop_cancel(sb_os_desktop *os);
void sb_os_desktop_free(sb_os_desktop *os);
const char *sb_os_desktop_provider(sb_os_desktop *os);
bool sb_os_desktop_human_pause(sb_os_desktop *os);
yyjson_mut_val *sb_os_desktop_permissions(yyjson_mut_doc *doc);
bool sb_os_desktop_view(sb_os_desktop *os, sb_dview *view, sb_error *error);
bool sb_os_desktop_capture(sb_os_desktop *os, const sb_dview *view, unsigned monitor, const sb_drect *crop,
                           unsigned maximum, unsigned char **bgra, unsigned *width, unsigned *height,
                           sb_error *error);
bool sb_os_desktop_input(sb_os_desktop *os, const char *name, yyjson_val *args, const sb_dview *view,
                         const sb_dwindow *window, unsigned *submitted, sb_error *error);
bool sb_os_desktop_clipboard(sb_os_desktop *os, bool write, uint32_t expected, const char *text, char **read,
                             uint32_t *sequence, sb_error *error);
sb_desktop *sb_posix_desktop_new(sb_host *host, sb_error *error);
void sb_posix_desktop_free(sb_desktop *desktop);
void sb_posix_desktop_pause(sb_desktop *desktop);
char *sb_posix_desktop_owner(sb_host *host, const char *command);
char *sb_posix_owner_request(sb_host *host, const char *bytes, size_t length);
bool sb_posix_png(const unsigned char *bgra, unsigned width, unsigned height, sb_image *image,
                  sb_error *error);
#endif
