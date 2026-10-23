#ifndef SB_COMMAND_MEMORY_H
#define SB_COMMAND_MEMORY_H
#include "posix.h"
/* Darwin's public APIs provide a sampled process-group physical footprint,
   not an allocation-time virtual-memory ceiling. The leader stays unreaped. */
bool sb_command_memory(pid_t group, uint64_t *bytes, sb_error *error);
#endif
