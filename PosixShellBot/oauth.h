#ifndef SB_POSIX_OAUTH_H
#define SB_POSIX_OAUTH_H
#include "credentials.h"
typedef struct sb_oauth sb_oauth;
typedef struct {
    const char *id;
    uint64_t revision;
    bool enabled;
    char binding[65];
} sb_oauth_project;
sb_oauth *sb_oauth_new(sb_secrets *secrets, const char *origin, sb_error *error);
void sb_oauth_free(sb_oauth *oauth);
bool sb_oauth_http(sb_oauth *oauth, const sb_oauth_project *projects, size_t count,
                   const sb_http_request *request, sb_http_response *response);
bool sb_oauth_authorized(sb_oauth *oauth, const sb_oauth_project *project, const char *authorization);
char *sb_oauth_challenge(sb_oauth *oauth, const char *path);
char *sb_oauth_owner(sb_oauth *oauth, const sb_oauth_project *projects, size_t count, yyjson_val *request);
void sb_oauth_summary(sb_oauth *oauth, yyjson_mut_doc *doc, yyjson_mut_val *root);
#endif
