#ifndef SB_TASK_PROCESS_H
#define SB_TASK_PROCESS_H
#include "sb.h"
typedef struct sb_win_tasks sb_win_tasks;
sb_win_tasks *sb_win_tasks_new(sb_execution_profile profile);
sb_win_tasks *sb_win_tasks_new_limited(sb_execution_profile profile, const sb_resource_limits *limits);
bool sb_win_tasks_owned(sb_win_tasks *tasks, const char *actor, const char *id);
void sb_win_tasks_free(sb_win_tasks *tasks);
char *sb_win_tasks_call(sb_win_tasks *tasks, const char *name, yyjson_val *arguments);
#endif
