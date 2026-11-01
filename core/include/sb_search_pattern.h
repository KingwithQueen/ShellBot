#ifndef SB_SEARCH_PATTERN_H
#define SB_SEARCH_PATTERN_H
#include "sb.h"
typedef struct sb_search_pattern sb_search_pattern;
sb_search_pattern *sb_search_pattern_new(const char *query,yyjson_val *args,uint64_t deadline,sb_error *error);
void sb_search_pattern_free(sb_search_pattern *pattern);
int sb_search_pattern_match(sb_search_pattern *pattern,const char *text,size_t length,bool path,size_t *offset);
int sb_search_pattern_include(sb_search_pattern *pattern,const char *path);
const char *sb_search_pattern_error(sb_search_pattern *pattern);
#endif
