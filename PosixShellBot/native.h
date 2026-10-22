#ifndef SB_NATIVE_H
#define SB_NATIVE_H
#include "posix.h"
typedef struct sb_native_manager sb_native_manager;
/* All controller calls run serially on one background queue. Native widgets stay on the main thread. */
sb_native_manager *sb_desktop_open(const char *directory, const char *key_file, sb_error *error);
char *sb_desktop_work(sb_native_manager *desktop, const char *request);
void sb_desktop_close(sb_native_manager *desktop);
const char *sb_desktop_directory(sb_native_manager *desktop);
bool sb_desktop_raise(sb_native_manager *desktop);
int sb_native_main(int argc, char **argv);
const char *sb_gui_string(yyjson_val *object, const char *key);
bool sb_gui_bool(yyjson_val *object, const char *key);
char *sb_gui_project_text(yyjson_val *project, yyjson_val *gateway, yyjson_val *status);
char *sb_gui_request_text(yyjson_val *request, yyjson_val *configuration);
char *sb_gui_desktop_text(yyjson_val *state);
#endif
