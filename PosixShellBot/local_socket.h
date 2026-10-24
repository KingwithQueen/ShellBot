#ifndef SB_LOCAL_SOCKET_H
#define SB_LOCAL_SOCKET_H
#include "posix.h"

/* Tokens are names, never caller-provided paths. The caller owns the socket FD. */
typedef struct sb_local_socket sb_local_socket;
#define SB_LOCAL_SOCKET_TOKEN_LENGTH 32
bool sb_local_socket_token_valid(const char *token);
sb_local_socket *sb_local_socket_create(sb_error *error);
sb_local_socket *sb_local_socket_open(const char *token, sb_error *error);
const char *sb_local_socket_path(const sb_local_socket *socket);
const char *sb_local_socket_token(const sb_local_socket *socket);
bool sb_local_socket_bind(sb_local_socket *socket, int fd, int backlog, sb_error *error);
bool sb_local_socket_check(const sb_local_socket *socket, sb_error *error);
/* Only a confirmed stale, unchanged endpoint is removed; active/uncertain fails. */
bool sb_local_socket_reclaim(const char *token, sb_error *error);
void sb_local_socket_close(sb_local_socket *socket);
#endif
