#include "sb.h"
#include <stdlib.h>
#include <string.h>

typedef struct {
    sb_workspace *workspace;
    const char *query;
    bool recursive, content, truncated, allocation_failed;
    size_t scanned, matched, maximum, output_bytes, skipped_directories, skipped_files;
    size_t skipped_reparse_directories, files_read, partial_files, bytes_read, per_file, total_bytes;
    uint64_t deadline;
    yyjson_mut_doc *doc;
    yyjson_mut_val *matches;
} search_state;

static bool stop(search_state *s) {
    if (s->allocation_failed || s->scanned >= SB_SEARCH_ENTRIES || s->matched >= s->maximum ||
        sb_platform_millis() >= s->deadline || (s->content && s->bytes_read >= s->total_bytes)) {
        s->truncated = true; return true;
    }
    return false;
}

static bool add_match(search_state *s, const sb_entry *entry, const char *path,
                       uint64_t line, size_t column, const char *excerpt, size_t excerpt_length,
                       bool excerpt_truncated, bool file_truncated, const char *encoding) {
    size_t budget = strlen(path) + strlen(entry->name) + excerpt_length + 256;
    if (s->matched >= s->maximum || s->output_bytes + budget > SB_SEARCH_OUTPUT_LIMIT) {
        s->truncated = true; return false;
    }
    yyjson_mut_val *item = yyjson_mut_obj(s->doc);
    bool ok = item && yyjson_mut_obj_add_strcpy(s->doc, item, "name", entry->name) &&
        yyjson_mut_obj_add_strcpy(s->doc, item, "path", path) &&
        yyjson_mut_obj_add_str(s->doc, item, "type", entry->directory ? "directory" : "file") &&
        yyjson_mut_obj_add_uint(s->doc, item, "size", entry->size) &&
        yyjson_mut_obj_add_bool(s->doc, item, "reparse_point", entry->reparse);
    if (s->content) ok = ok && yyjson_mut_obj_add_uint(s->doc, item, "line", line) &&
        yyjson_mut_obj_add_uint(s->doc, item, "column_byte", column) &&
        yyjson_mut_obj_add_strncpy(s->doc, item, "excerpt", excerpt, excerpt_length) &&
        yyjson_mut_obj_add_bool(s->doc, item, "excerpt_truncated", excerpt_truncated) &&
        yyjson_mut_obj_add_bool(s->doc, item, "file_truncated", file_truncated) &&
        yyjson_mut_obj_add_str(s->doc, item, "encoding", encoding);
    ok = ok && yyjson_mut_arr_add_val(s->matches, item);
    if (!ok) { s->allocation_failed = true; return false; }
    s->output_bytes += budget; ++s->matched;
    return true;
}

static void search_file(search_state *s, const sb_entry *entry, const char *path) {
    sb_error error = {0};
    sb_file *file = sb_platform_open(s->workspace->root, path, false, &error);
    if (!file) { ++s->skipped_files; return; }
    uint64_t size = sb_platform_size(file);
    size_t limit = s->per_file;
    if (limit > s->total_bytes - s->bytes_read) limit = s->total_bytes - s->bytes_read;
    if (size < limit) limit = (size_t)size;
    bool partial = size > limit;
    if (partial) { ++s->partial_files; s->truncated = true; }
    unsigned char *bytes = malloc(limit ? limit : 1);
    if (!bytes) { s->allocation_failed = true; sb_platform_close(file); return; }
    size_t got = 0;
    bool ok = sb_platform_read(file, 0, bytes, limit, &got, &error);
    sb_platform_close(file);
    s->bytes_read += got; ++s->files_read;
    char *text = NULL; size_t text_length = 0, consumed = 0; const char *encoding = NULL;
    if (ok) ok = sb_decode(bytes, got, !partial, "auto", true,
                           &text, &text_length, &consumed, &encoding, &error);
    free(bytes);
    if (!ok) { ++s->skipped_files; return; }
    uint64_t line = 1;
    char *cursor = text, *end = text + text_length;
    while (cursor < end && !s->allocation_failed && s->matched < s->maximum) {
        if (sb_platform_millis() >= s->deadline) { s->truncated = true; break; }
        char *line_end = cursor;
        while (line_end < end && *line_end != '\r' && *line_end != '\n') ++line_end;
        char delimiter = *line_end; *line_end = 0;
        const char *found = strstr(cursor, s->query);
        if (found) {
            size_t line_length = (size_t)(line_end - cursor), position = (size_t)(found - cursor);
            size_t first = position > 96 ? position - 96 : 0;
            while (first && ((unsigned char)cursor[first] & 0xc0) == 0x80) ++first;
            size_t last = line_length < first + 1024 ? line_length : first + 1024;
            while (last < line_length && ((unsigned char)cursor[last] & 0xc0) == 0x80) --last;
            bool added = add_match(s, entry, path, line, position + 1, cursor + first, last - first,
                                  first > 0 || last < line_length, partial, encoding);
            if (!added) { *line_end = delimiter; break; }
        }
        *line_end = delimiter;
        if (line_end == end) break;
        cursor = line_end + 1;
        if (delimiter == '\r' && cursor < end && *cursor == '\n') ++cursor;
        ++line;
    }
    if (s->matched >= s->maximum) s->truncated = true;
    free(text);
}

static void search_directory(search_state *s, const char *path, unsigned depth) {
    if (stop(s)) return;
    sb_error error = {0};
    sb_file *directory = sb_platform_open(s->workspace->root, path, true, &error);
    if (!directory) { ++s->skipped_directories; return; }
    sb_entry *entries = NULL; size_t count = 0; bool more = false;
    bool ok = sb_platform_list(directory, 0, SB_SEARCH_ENTRIES - s->scanned, &entries, &count, &more, &error);
    sb_platform_close(directory);
    if (!ok) { ++s->skipped_directories; return; }
    s->truncated |= more;
    for (size_t i = 0; i < count; ++i) {
        if (stop(s)) break;
        ++s->scanned;
        size_t length = strlen(path) + strlen(entries[i].name) + 2;
        if (length >= SB_PATH_CAP) { s->truncated = true; break; }
        char *relative = malloc(length);
        if (!relative) { s->allocation_failed = true; break; }
        size_t prefix = strlen(path);
#ifdef SB_POSIX
        if (!strcmp(path,".")) prefix = 0;
        if (prefix) { memcpy(relative, path, prefix); relative[prefix] = '/'; }
        strcpy(relative + (prefix ? prefix + 1 : 0), entries[i].name);
#else
        memcpy(relative, path, prefix); relative[prefix] = '/';
        strcpy(relative + prefix + 1, entries[i].name);
#endif
        if (s->content && !entries[i].directory) search_file(s, &entries[i], relative);
        else if (!s->content && strstr(entries[i].name, s->query))
            add_match(s, &entries[i], relative, 0, 0, NULL, 0, false, false, NULL);
        if (s->recursive && entries[i].directory) {
            if (entries[i].reparse) ++s->skipped_reparse_directories;
            else if (depth < SB_SEARCH_DEPTH) search_directory(s, relative, depth + 1);
            else s->truncated = true;
        }
        free(relative);
    }
    sb_platform_entries_free(entries, count);
}

yyjson_mut_val *sb_search(sb_workspace *workspace, const char *path, const char *query,
                         bool recursive, bool content, size_t maximum, size_t per_file,
                         size_t total_bytes, yyjson_mut_doc *doc, sb_error *error) {
    sb_file *start = sb_platform_open(workspace->root, path, true, error);
    if (!start) return NULL;
    sb_platform_close(start);
    search_state state = {0};
    state.workspace = workspace; state.query = query; state.recursive = recursive; state.content = content;
    state.maximum = maximum; state.per_file = per_file; state.total_bytes = total_bytes;
    state.deadline = sb_platform_millis() + 2000; state.doc = doc; state.matches = yyjson_mut_arr(doc);
    if (!state.matches) { sb_fail(error, "Cannot allocate search results."); return NULL; }
    search_directory(&state, path, 0);
    yyjson_mut_val *data = yyjson_mut_obj(doc);
    bool ok = !state.allocation_failed && data && yyjson_mut_obj_add_val(doc, data, "matches", state.matches) &&
        yyjson_mut_obj_add_str(doc, data, "mode", content ? "content" : "filename") &&
        yyjson_mut_obj_add_uint(doc, data, "scanned_entries", state.scanned) &&
        yyjson_mut_obj_add_uint(doc, data, "skipped_directories", state.skipped_directories) &&
        yyjson_mut_obj_add_uint(doc, data, "skipped_reparse_directories", state.skipped_reparse_directories) &&
        yyjson_mut_obj_add_bool(doc, data, "truncated", state.truncated);
    if (content) ok = ok && yyjson_mut_obj_add_uint(doc, data, "files_read", state.files_read) &&
        yyjson_mut_obj_add_uint(doc, data, "bytes_read", state.bytes_read) &&
        yyjson_mut_obj_add_uint(doc, data, "partial_files", state.partial_files) &&
        yyjson_mut_obj_add_uint(doc, data, "skipped_files", state.skipped_files);
    if (!ok) { sb_fail(error, "Cannot allocate search results."); return NULL; }
    return data;
}
