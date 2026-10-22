#ifndef SB_POSIX_H
#define SB_POSIX_H
#include "sb.h"
#include <sys/stat.h>
#include <stdatomic.h>
#include <pthread.h>
#include <signal.h>
#include <sys/types.h>
struct sb_root { int fd; char *path; };
struct sb_file { int fd; bool directory; struct stat initial; };
struct sb_lease { sb_root *root; int lock; unsigned backups; pthread_mutex_t mutex; };
struct sb_mutex { pthread_mutex_t value; };
struct sb_journal { int fd; };
typedef struct { uint32_t h[8]; uint64_t bytes; unsigned char block[64]; size_t used; } sb_sha;
void sb_sha_init(sb_sha *ctx);
void sb_sha_update(sb_sha *ctx, const void *data, size_t length);
void sb_sha_finish(sb_sha *ctx, char hex[65]);
void sb_posix_error(sb_error *error, const char *operation);
bool sb_posix_write(int fd, const void *bytes, size_t length, sb_error *error);
bool sb_posix_private(sb_root *root, sb_error *error);
bool sb_posix_socket_path(const char *directory, char *out, size_t capacity, sb_error *error);
extern volatile sig_atomic_t sb_posix_signal;
void sb_posix_set_program(const char *argv0);
const char *sb_posix_program(void);
void sb_posix_close_inherited(void);
bool sb_posix_peer(int fd, bool parent);
char *sb_posix_frame_read(int fd, unsigned timeout_ms, sb_error *error);
bool sb_posix_frame_write(int fd, const char *bytes, unsigned timeout_ms, sb_error *error);
bool sb_posix_spawn_channel(char *const argv[], int *channel, pid_t *pid, sb_error *error);
bool sb_posix_wait_child(pid_t pid, unsigned timeout_ms, int *status);
char *sb_posix_json(yyjson_mut_doc *doc, yyjson_mut_val *root);
char *sb_posix_json_error(const char *message);
bool sb_posix_execution_bind(sb_execution *execution, sb_host *host, sb_error *error);
void sb_posix_execution_cancel_all(sb_execution *execution);
void sb_posix_execution_instance_bind(sb_execution *execution, const char *instance);
int sb_posix_task_agent(int fd);
int sb_posix_worker_run(sb_host *host, int fd);
bool sb_posix_manager_option(const char *option);
int sb_posix_manager_cli(int argc, char **argv);
bool sb_posix_secret(const char *a, const char *b);
char *sb_posix_bearer(const char *name, sb_error *error);
#endif
