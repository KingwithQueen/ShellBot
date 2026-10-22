#ifndef SB_MIGRATION_H
#define SB_MIGRATION_H
#include "credentials.h"
typedef struct { uint64_t files, directories, bytes; atomic_bool *cancel; } sb_migration;
sb_root *sb_migration_destination(const char *path, sb_error *error);
bool sb_migration_copy(sb_root *source, sb_root *target, sb_migration *state, sb_error *error);
bool sb_migration_write(sb_root *root, const char *name, const char *bytes, size_t length, sb_error *error);
bool sb_migration_can_start(sb_lease *lease, sb_error *error);
bool sb_secrets_relocate(sb_lease *source, sb_lease *target, const sb_secret_options *options,
    sb_secrets **old_secrets, sb_secrets **new_secrets, sb_error *error);
#endif
