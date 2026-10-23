#ifndef SB_POSIX_ADMIN_INTERNAL_H
#define SB_POSIX_ADMIN_INTERNAL_H
#include "sb_projects.h"
bool sb_admin_project_eligible(const sb_project *project);
bool sb_admin_project_digest(const sb_project *project, char hash[65], sb_error *error);
bool sb_admin_trusted_path(const char *path, bool directory, sb_error *error);
#endif
