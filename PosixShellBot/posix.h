#ifndef SB_POSIX_H
#define SB_POSIX_H
#include "sb.h"
#include <sys/stat.h>
#include <stdatomic.h>
#include <pthread.h>
#include <signal.h>
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
#endif
