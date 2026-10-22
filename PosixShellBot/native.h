#ifndef SB_NATIVE_H
#define SB_NATIVE_H
#include "posix.h"
typedef struct sb_desktop sb_desktop;
/* All controller calls run serially on one background queue. Native widgets stay on the main thread. */
sb_desktop *sb_desktop_open(const char *directory, const char *key_file, sb_error *error);
char *sb_desktop_work(sb_desktop *desktop, const char *request);
void sb_desktop_close(sb_desktop *desktop);
const char *sb_desktop_directory(sb_desktop *desktop);
bool sb_desktop_raise(sb_desktop *desktop);
int sb_native_main(int argc, char **argv);
const char *sb_gui_string(yyjson_val *object, const char *key);
bool sb_gui_bool(yyjson_val *object, const char *key);
char *sb_gui_project_text(yyjson_val *project, yyjson_val *gateway, yyjson_val *status);
char *sb_gui_request_text(yyjson_val *request, yyjson_val *configuration);
#endif
