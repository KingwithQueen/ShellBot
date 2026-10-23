#ifndef SB_COMMAND_TREE_H
#define SB_COMMAND_TREE_H
#include "posix.h"
#include "restricted.h"
#define SB_COMMAND_TREE_SAMPLE_MS 20
#define SB_COMMAND_TREE_PROCESS_LIMIT 1024
typedef struct {
    pid_t supervisor, command;
    int fd, cancel_fd;
    unsigned received;
    unsigned char frame[512];
    uint64_t memory_peak;
    unsigned process_peak;
    bool final, cleanup_complete, cancel_sent;
    int exit_code;
    char reason[48], error[256];
} sb_command_watch;
typedef struct {
    int program, cwd, input, output, error;
    char *const *argv;
    char *const *env;
    sb_restricted *restricted;
    unsigned memory_mb;
    uint64_t timeout_ms;
} sb_command_spec;
bool sb_command_watch_start(const sb_command_spec *spec, sb_command_watch *watch, sb_error *error);
bool sb_command_watch_poll(sb_command_watch *watch);
void sb_command_watch_cancel(sb_command_watch *watch);
void sb_command_watch_close(sb_command_watch *watch);
const char *sb_command_tree_scope(void);
const char *sb_command_tree_memory(void);
#endif
