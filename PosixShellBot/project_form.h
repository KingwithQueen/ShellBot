#ifndef SB_PROJECT_FORM_H
#define SB_PROJECT_FORM_H
#include "sb_projects.h"
typedef struct {
    const char *path, *label, *page, *kind, *fallback, *choices, *titles;
    unsigned minimum, maximum;
} sb_form_field;
extern const sb_form_field sb_form_fields[];
extern const unsigned sb_form_field_count;
yyjson_val *sb_form_get(yyjson_val *root, const char *path);
void sb_form_set(yyjson_mut_doc *doc, yyjson_mut_val *root, const char *path, yyjson_mut_val *value);
yyjson_mut_doc *sb_form_project(yyjson_val *project, const char *directory);
const char *sb_form_tool_label(unsigned index);
void sb_form_normalize(yyjson_mut_doc *doc, yyjson_mut_val *project);
#endif
