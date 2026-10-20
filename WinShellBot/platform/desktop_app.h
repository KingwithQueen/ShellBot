#ifndef SHELLBOT_DESKTOP_APP_H
#define SHELLBOT_DESKTOP_APP_H
#include "sb_projects.h"
#include <windows.h>
typedef struct sb_desktop_app sb_desktop_app;
typedef struct {
    char id[64],token_env[64],token[257];
    char *name,*directory,*definition;
    bool automatic,read_only;
} sb_desktop_draft;
int sb_desktop_main(const char *directory);
char *sb_desktop_exchange(sb_desktop_app *app,const char *request,sb_error *error);
bool sb_desktop_draft_load(sb_desktop_app *app,const char *id,sb_desktop_draft *draft,sb_error *error);
void sb_desktop_draft_free(sb_desktop_draft *draft);
bool sb_desktop_save(sb_desktop_app *app,const char *original_id,sb_desktop_draft *draft,sb_error *error);
bool sb_desktop_connection(sb_desktop_app *app,unsigned port,const char *origin,sb_error *error);
unsigned sb_desktop_port(sb_desktop_app *app);
const char *sb_desktop_origin(sb_desktop_app *app);
const char *sb_desktop_data(sb_desktop_app *app);
const char *sb_desktop_owner_token(sb_desktop_app *app);
HANDLE sb_desktop_stop(sb_desktop_app *app);
void sb_desktop_request_exit(sb_desktop_app *app);
const wchar_t *sb_desktop_instance_key(sb_desktop_app *app);
bool sb_desktop_project_dialog(HWND owner,sb_desktop_app *app,const char *id);
bool sb_desktop_connection_dialog(HWND owner,sb_desktop_app *app);
bool sb_desktop_is_busy(sb_desktop_app *app);
wchar_t *sb_desktop_wide(const char *text);
char *sb_desktop_join(const char *directory,const char *leaf);
bool sb_desktop_private_write(const char *path,const char *bytes,size_t length,bool replace,sb_error *error);
char *sb_desktop_seal(const char *text,sb_error *error);
char *sb_desktop_unseal(const char *path,sb_error *error);
bool sb_desktop_copy_tree(const char *source,const char *destination,HANDLE cancel,sb_error *error);
bool sb_desktop_make_directory(const char *path,bool empty,sb_error *error);
bool sb_desktop_overlap(const char *a,const char *b);
#endif
