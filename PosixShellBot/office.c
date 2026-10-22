#include "media.h"
#include <expat.h>
#include <zlib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define PART_LIMIT (8u * 1024u * 1024u)
#define UNIT_LIMIT 256
#define EVENT_LIMIT 1000000
typedef struct {
    char name[1025];
    uint32_t offset, input, output, crc;
    unsigned method;
} part;
typedef struct {
    const unsigned char *bytes;
    part *parts;
    unsigned count;
} package;
typedef struct {
    char *value;
    size_t size, capacity;
    bool truncated;
} text_buffer;
typedef struct {
    char id[128], name[256];
} office_unit;
typedef struct {
    const char *format;
    office_unit units[UNIT_LIMIT];
    unsigned count;
    char **shared;
    size_t shared_count, shared_bytes;
    text_buffer text, cell;
    char cell_type[16], reference[32];
    unsigned capture_depth, skip_depth, cell_depth;
    bool in_shared;
} office_context;
static unsigned le16(const unsigned char *p) { return (unsigned)p[0] | ((unsigned)p[1] << 8); }
static uint32_t le32(const unsigned char *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static unsigned char *unpack(package *z, part *p) {
    unsigned char *out = malloc((size_t)p->output + 1);
    if (!out)
        return NULL;
    bool ok;
    if (!p->method) {
        ok = p->input == p->output;
        if (ok)
            memcpy(out, z->bytes + p->offset, p->output);
    } else {
        z_stream s = {0};
        s.next_in = (Bytef *)(z->bytes + p->offset);
        s.avail_in = p->input;
        s.next_out = out;
        s.avail_out = p->output + 1;
        ok = inflateInit2(&s, -MAX_WBITS) == Z_OK;
        if (ok) {
            int rc = inflate(&s, Z_FINISH);
            ok = rc == Z_STREAM_END && s.total_out == p->output && s.total_in == p->input;
            inflateEnd(&s);
        }
    }
    if (ok)
        ok = (uint32_t)crc32(0, out, p->output) == p->crc;
    if (!ok) {
        free(out);
        return NULL;
    }
    out[p->output] = 0;
    return out;
}
static bool zip_open(package *z, const unsigned char *bytes, size_t size) {
    if (size < 22)
        return false;
    size_t at = size - 22, minimum = size > 65557 ? size - 65557 : 0;
    for (;;) {
        if (size - at >= 22 && le32(bytes + at) == 0x06054b50 &&
            (size_t)le16(bytes + at + 20) == size - at - 22)
            break;
        if (at == minimum)
            return false;
        --at;
    }
    unsigned count = le16(bytes + at + 10);
    uint32_t directory = le32(bytes + at + 16), length = le32(bytes + at + 12);
    if (le16(bytes + at + 4) || le16(bytes + at + 6) || !count || count > 2048 ||
        le16(bytes + at + 8) != count || directory > at || length != at - directory)
        return false;
    z->parts = calloc(count, sizeof(part));
    if (!z->parts)
        return false;
    z->bytes = bytes;
    z->count = count;
    size_t pos = directory;
    uint64_t expanded = 0;
    for (unsigned i = 0; i < count; i++) {
        if (at - pos < 46 || le32(bytes + pos) != 0x02014b50)
            return false;
        unsigned flags = le16(bytes + pos + 8), method = le16(bytes + pos + 10), n = le16(bytes + pos + 28),
                 extra = le16(bytes + pos + 30), comment = le16(bytes + pos + 32);
        uint32_t output = le32(bytes + pos + 24), input = le32(bytes + pos + 20),
                 local = le32(bytes + pos + 42);
        if (flags & ~0x080eu || flags & 1 || (method != 0 && method != 8) || le16(bytes + pos + 34) || !n ||
            n > 1024 || (size_t)n + extra + comment > at - pos - 46 || output > PART_LIMIT || input > size ||
            local > directory || directory - local < 30)
            return false;
        part *p = &z->parts[i];
        const unsigned char *name = bytes + pos + 46;
        if (memchr(name, 0, n) || name[0] == '/' || memchr(name, '\\', n) || memchr(name, ':', n) ||
            memchr(name, '%', n))
            return false;
        memcpy(p->name, name, n);
        p->name[n] = 0;
        if (strstr(p->name, "../") || strstr(p->name, "/..") || strstr(p->name, "//") ||
            strstr(p->name, "/./") || !strcmp(p->name, ".."))
            return false;
        const char *base = strrchr(p->name, '/');
        base = base ? base + 1 : p->name;
        if (!strcasecmp(base, "vbaProject.bin"))
            return false;
        if (le32(bytes + local) != 0x04034b50 || le16(bytes + local + 6) != flags ||
            le16(bytes + local + 8) != method || le16(bytes + local + 26) != n)
            return false;
        size_t header = 30u + n + le16(bytes + local + 28);
        if (header > directory - local || memcmp(bytes + local + 30, name, n) ||
            input > directory - local - header)
            return false;
        if (!(flags & 8) && (le32(bytes + local + 14) != le32(bytes + pos + 16) ||
                             le32(bytes + local + 18) != input || le32(bytes + local + 22) != output))
            return false;
        p->offset = local + (uint32_t)header;
        p->input = input;
        p->output = output;
        p->method = method;
        p->crc = le32(bytes + pos + 16);
        for (unsigned j = 0; j < i; j++)
            if (!strcasecmp(p->name, z->parts[j].name) ||
                (local < z->parts[j].offset + z->parts[j].input && p->offset + input > z->parts[j].offset))
                return false;
        expanded += output;
        if (expanded > 64u * 1024u * 1024u)
            return false;
        unsigned char *valid = unpack(z, p);
        if (!valid)
            return false;
        free(valid);
        pos += 46u + n + extra + comment;
    }
    return pos == at;
}
static part *find(package *z, const char *name) {
    for (unsigned i = 0; i < z->count; i++)
        if (!strcmp(name, z->parts[i].name))
            return &z->parts[i];
    return NULL;
}
static bool append(text_buffer *b, const char *value, size_t size) {
    size_t n = size;
    if (n > SB_DOCUMENT_TEXT_LIMIT - b->size) {
        n = SB_DOCUMENT_TEXT_LIMIT - b->size;
        b->truncated = true;
        while (n && n < size && ((unsigned char)value[n] & 0xc0) == 0x80)
            --n;
    }
    if (b->size + n + 1 > b->capacity) {
        size_t c = b->capacity ? b->capacity : 256;
        while (c < b->size + n + 1) {
            c *= 2;
            if (c > SB_DOCUMENT_TEXT_LIMIT + 1)
                c = SB_DOCUMENT_TEXT_LIMIT + 1;
        }
        char *p = realloc(b->value, c);
        if (!p)
            return false;
        b->value = p;
        b->capacity = c;
    }
    memcpy(b->value + b->size, value, n);
    b->size += n;
    b->value[b->size] = 0;
    return true;
}
/* Namespace-aware SAX parser: no DTD, entities, network resolver or disk input. */
typedef struct parser parser;
typedef void (*start_fn)(parser *, const char *, const char **);
typedef void (*end_fn)(parser *, const char *);
typedef void (*text_fn)(parser *, const char *, int);
struct parser {
    XML_Parser xml;
    void *context;
    start_fn start;
    end_fn end;
    text_fn text;
    unsigned depth, events;
    bool ok, root;
};
static void stop(parser *p) {
    p->ok = false;
    XML_StopParser(p->xml, XML_FALSE);
}
static const char *local(const char *n) {
    const char *s = strchr(n, '|');
    return s ? s + 1 : n;
}
static bool ns(const char *n, const char *uri) {
    size_t k = strlen(uri);
    return !strncmp(n, uri, k) && n[k] == '|';
}
static bool space(const char *n, const char *kind) {
    const char *a = !strcmp(kind, "docx")   ? "http://schemas.openxmlformats.org/wordprocessingml/2006/main"
                    : !strcmp(kind, "xlsx") ? "http://schemas.openxmlformats.org/spreadsheetml/2006/main"
                                            : "http://schemas.openxmlformats.org/presentationml/2006/main";
    const char *b = !strcmp(kind, "docx")   ? "http://purl.oclc.org/ooxml/wordprocessingml/main"
                    : !strcmp(kind, "xlsx") ? "http://purl.oclc.org/ooxml/spreadsheetml/main"
                                            : "http://purl.oclc.org/ooxml/presentationml/main";
    return ns(n, a) || ns(n, b);
}
static bool drawing(const char *n) {
    return ns(n, "http://schemas.openxmlformats.org/drawingml/2006/main") ||
           ns(n, "http://purl.oclc.org/ooxml/drawingml/main");
}
static const char *attr(const char **a, const char *n) {
    for (unsigned i = 0; a[i]; i += 2)
        if (!strcmp(a[i], n))
            return a[i + 1];
    return "";
}
static bool copy(char *out, size_t cap, const char *in) {
    size_t n = strlen(in);
    if (n >= cap)
        return false;
    memcpy(out, in, n + 1);
    return true;
}
static void XMLCALL start(void *ctx, const char *n, const char **a) {
    parser *p = ctx;
    if (++p->events > EVENT_LIMIT || ++p->depth > 64) {
        stop(p);
        return;
    }
    if (p->start)
        p->start(p, n, a);
    p->root = true;
}
static void XMLCALL end(void *ctx, const char *n) {
    parser *p = ctx;
    if (++p->events > EVENT_LIMIT) {
        stop(p);
        return;
    }
    if (p->end)
        p->end(p, n);
    --p->depth;
}
static void XMLCALL text(void *ctx, const char *s, int n) {
    parser *p = ctx;
    if (++p->events > EVENT_LIMIT) {
        stop(p);
        return;
    }
    if (p->text)
        p->text(p, s, n);
}
static void XMLCALL dtd(void *ctx, const char *name, const char *system, const char *public_id,
                        int internal) {
    (void)name;
    (void)system;
    (void)public_id;
    (void)internal;
    stop(ctx);
}
static bool parse(package *z, part *part_value, parser *p) {
    if (!part_value || !part_value->output)
        return false;
    unsigned char *bytes = unpack(z, part_value);
    if (!bytes)
        return false;
    p->xml = XML_ParserCreateNS(NULL, '|');
    p->ok = p->xml != NULL;
    if (p->ok) {
        XML_SetUserData(p->xml, p);
        XML_SetElementHandler(p->xml, start, end);
        XML_SetCharacterDataHandler(p->xml, text);
        XML_SetStartDoctypeDeclHandler(p->xml, dtd);
        XML_SetParamEntityParsing(p->xml, XML_PARAM_ENTITY_PARSING_NEVER);
        p->ok = XML_Parse(p->xml, (char *)bytes, (int)part_value->output, XML_TRUE) == XML_STATUS_OK &&
                p->ok && p->root && !p->depth;
        XML_ParserFree(p->xml);
    }
    free(bytes);
    return p->ok;
}
typedef struct {
    char id[128], type[256], target[1025];
    bool external;
} relation;
typedef struct {
    relation items[512];
    unsigned count;
} relations;
static void rel_start(parser *p, const char *n, const char **a) {
    relations *r = p->context;
    if (p->depth == 1) {
        if (strcmp(n, "http://schemas.openxmlformats.org/package/2006/relationships|Relationships"))
            stop(p);
        return;
    }
    if (p->depth != 2 ||
        strcmp(n, "http://schemas.openxmlformats.org/package/2006/relationships|Relationship") ||
        r->count == 512) {
        stop(p);
        return;
    }
    relation *v = &r->items[r->count];
    const char *mode = attr(a, "TargetMode");
    v->external = !strcmp(mode, "External");
    if (!copy(v->id, sizeof(v->id), attr(a, "Id")) || !*v->id ||
        !copy(v->type, sizeof(v->type), attr(a, "Type")) || !*v->type ||
        !copy(v->target, sizeof(v->target), attr(a, "Target")) || !*v->target ||
        (*mode && strcmp(mode, "External") && strcmp(mode, "Internal"))) {
        stop(p);
        return;
    }
    for (unsigned i = 0; i < r->count; i++)
        if (!strcmp(v->id, r->items[i].id)) {
            stop(p);
            return;
        }
    r->count++;
}
static bool type_is(const char *type, const char *suffix) {
    char a[256], b[256];
    snprintf(a, sizeof(a), "http://schemas.openxmlformats.org/officeDocument/2006/relationships/%s", suffix);
    snprintf(b, sizeof(b), "http://purl.oclc.org/ooxml/officeDocument/relationships/%s", suffix);
    return !strcmp(type, a) || !strcmp(type, b);
}
static relation *rel_find(relations *r, const char *id, const char *kind, bool *valid) {
    relation *result = NULL;
    for (unsigned i = 0; i < r->count; i++)
        if (id ? !strcmp(id, r->items[i].id) : type_is(r->items[i].type, kind)) {
            if (result || !type_is(r->items[i].type, kind)) {
                *valid = false;
                return NULL;
            }
            result = &r->items[i];
        }
    return result;
}
static part *target(package *z, const char *source, relation *r) {
    if (!r || r->external)
        return NULL;
    char path[2048], decoded[1025];
    size_t k = 0;
    for (size_t i = 0; r->target[i]; i++) {
        unsigned char c = (unsigned char)r->target[i];
        if (c == '%') {
            unsigned v = 0;
            for (unsigned j = 0; j < 2; j++) {
                char t = r->target[++i];
                if (!t)
                    return NULL;
                unsigned x = t >= '0' && t <= '9'   ? (unsigned)(t - '0')
                             : t >= 'a' && t <= 'f' ? (unsigned)(t - 'a' + 10)
                             : t >= 'A' && t <= 'F' ? (unsigned)(t - 'A' + 10)
                                                    : 256;
                if (x > 15)
                    return NULL;
                v = v * 16 + x;
            }
            c = (unsigned char)v;
        }
        if (c < 32 || c == '\\' || c == ':' || c == '?' || c == '#' || k + 1 >= sizeof(decoded))
            return NULL;
        decoded[k++] = (char)c;
    }
    decoded[k] = 0;
    const char *slash = strrchr(source, '/');
    int prefix = slash ? (int)(slash - source + 1) : 0;
    int written = decoded[0] == '/' ? snprintf(path, sizeof(path), "%s", decoded + 1)
                                    : snprintf(path, sizeof(path), "%.*s%s", prefix, source, decoded);
    if (written < 1 || (size_t)written >= sizeof(path))
        return NULL;
    char normalized[1025] = "", *save = NULL;
    size_t used = 0, starts[128];
    unsigned depth = 0;
    for (char *s = strtok_r(path, "/", &save); s; s = strtok_r(NULL, "/", &save)) {
        if (!strcmp(s, "."))
            continue;
        if (!strcmp(s, "..")) {
            if (!depth)
                return NULL;
            used = starts[--depth];
            normalized[used] = 0;
            continue;
        }
        size_t n = strlen(s);
        if (depth == 128 || used + n + 2 > sizeof(normalized))
            return NULL;
        starts[depth++] = used;
        if (used)
            normalized[used++] = '/';
        memcpy(normalized + used, s, n + 1);
        used += n;
    }
    return find(z, normalized);
}
static bool load_rels(package *z, const char *source, relations *r) {
    char name[1200];
    const char *slash = strrchr(source, '/');
    if (!*source)
        strcpy(name, "_rels/.rels");
    else
        snprintf(name, sizeof(name), "%.*s_rels/%s.rels", slash ? (int)(slash - source + 1) : 0, source,
                 slash ? slash + 1 : source);
    parser p = {.context = r, .start = rel_start};
    return parse(z, find(z, name), &p);
}
typedef struct {
    const char *name, *expected;
    bool found;
} content_type;
static void ct_start(parser *p, const char *n, const char **a) {
    content_type *c = p->context;
    const char *uri = "http://schemas.openxmlformats.org/package/2006/content-types";
    if (p->depth == 1) {
        if (!ns(n, uri) || strcmp(local(n), "Types"))
            stop(p);
        return;
    }
    if (p->depth != 2 || !ns(n, uri)) {
        stop(p);
        return;
    }
    if (!strcmp(local(n), "Override") &&
        !strcmp(attr(a, "PartName") + (*attr(a, "PartName") == '/'), c->name)) {
        if (c->found || strcmp(attr(a, "ContentType"), c->expected))
            stop(p);
        c->found = true;
    }
}
typedef struct {
    office_context *c;
    unsigned mode;
    bool keep;
} body_context;
static bool cell_finish(office_context *c) {
    bool ok = append(&c->text, c->reference, strlen(c->reference)) && append(&c->text, "\t", 1);
    if (!strcmp(c->cell_type, "s")) {
        size_t index = 0;
        const char *v = c->cell.value ? c->cell.value : "";
        if (!*v)
            return false;
        for (; *v; v++) {
            if (*v < '0' || *v > '9' || index > 32768)
                return false;
            index = index * 10 + (unsigned)(*v - '0');
        }
        if (index >= c->shared_count)
            return false;
        ok = ok && append(&c->text, c->shared[index], strlen(c->shared[index]));
    } else
        ok = ok && append(&c->text, c->cell.value ? c->cell.value : "", c->cell.size);
    c->text.truncated |= c->cell.truncated;
    return ok && append(&c->text, "\n", 1);
}
static void body_start(parser *p, const char *n, const char **a) {
    body_context *b = p->context;
    office_context *c = b->c;
    const char *l = local(n);
    bool own = space(n, c->format), sheet = space(n, "xlsx"), word = space(n, "docx"), draw = drawing(n);
    if (p->depth == 1) {
        const char *expected = b->mode == 2                 ? "sst"
                               : b->mode == 3               ? "worksheet"
                               : !strcmp(c->format, "docx") ? "document"
                               : !strcmp(c->format, "xlsx") ? "workbook"
                               : b->mode == 0               ? "presentation"
                                                            : "sld";
        if (!own || strcmp(l, expected)) {
            stop(p);
            return;
        }
    }
    if (c->skip_depth)
        return;
    if ((own && !strcmp(l, "del")) || (sheet && !strcmp(l, "rPh"))) {
        c->skip_depth = p->depth;
        return;
    }
    if (b->mode == 0 && own &&
        ((!strcmp(c->format, "xlsx") && !strcmp(l, "sheet")) ||
         (!strcmp(c->format, "pptx") && !strcmp(l, "sldId")))) {
        if (c->count == UNIT_LIMIT) {
            stop(p);
            return;
        }
        office_unit *u = &c->units[c->count];
        const char *id = attr(a, "http://schemas.openxmlformats.org/officeDocument/2006/relationships|id");
        if (!*id)
            id = attr(a, "http://purl.oclc.org/ooxml/officeDocument/relationships|id");
        if (!*id || !copy(u->id, sizeof(u->id), id)) {
            stop(p);
            return;
        }
        for (unsigned i = 0; i < c->count; i++)
            if (!strcmp(id, c->units[i].id)) {
                stop(p);
                return;
            }
        if (!strcmp(c->format, "xlsx")) {
            if (!*attr(a, "name") || !copy(u->name, sizeof(u->name), attr(a, "name"))) {
                stop(p);
                return;
            }
        } else
            snprintf(u->name, sizeof(u->name), "Slide %u", c->count + 1);
        c->count++;
    }
    if (b->mode == 2 && sheet && !strcmp(l, "si")) {
        if (c->in_shared) {
            stop(p);
            return;
        }
        c->in_shared = true;
        c->cell.size = 0;
        c->cell.truncated = false;
        if (c->cell.value)
            c->cell.value[0] = 0;
    }
    if (b->mode == 3 && sheet && !strcmp(l, "c")) {
        if (c->cell_depth || !copy(c->reference, sizeof(c->reference), attr(a, "r")) ||
            !copy(c->cell_type, sizeof(c->cell_type), attr(a, "t"))) {
            stop(p);
            return;
        }
        unsigned col = 0, row = 0;
        const char *r = c->reference;
        while (*r >= 'A' && *r <= 'Z') {
            if (col > 16384) {
                stop(p);
                return;
            }
            col = col * 26 + (unsigned)(*r++ - 'A' + 1);
        }
        if (*r < '1' || *r > '9') {
            stop(p);
            return;
        }
        while (*r >= '0' && *r <= '9') {
            if (row > 1048576) {
                stop(p);
                return;
            }
            row = row * 10 + (unsigned)(*r++ - '0');
        }
        if (*r || !col || col > 16384 || !row || row > 1048576) {
            stop(p);
            return;
        }
        c->cell_depth = p->depth;
        c->cell.size = 0;
        c->cell.truncated = false;
        if (c->cell.value)
            c->cell.value[0] = 0;
    }
    bool capture = (b->mode == 2 && c->in_shared && sheet && !strcmp(l, "t")) ||
                   (b->mode == 3 && c->cell_depth && sheet &&
                    (!strcmp(l, "v") || (!strcmp(l, "t") && !strcmp(c->cell_type, "inlineStr")))) ||
                   (b->keep && (word || draw) && !strcmp(l, "t"));
    if (capture)
        c->capture_depth = p->depth;
    if (b->keep && word && (!strcmp(l, "tab") || !strcmp(l, "br") || !strcmp(l, "cr")))
        if (!append(&c->text, !strcmp(l, "tab") ? "\t" : "\n", 1))
            stop(p);
}
static void body_text(parser *p, const char *s, int n) {
    body_context *b = p->context;
    if (!b->c->skip_depth && b->c->capture_depth &&
        !append(b->mode >= 2 ? &b->c->cell : &b->c->text, s, (size_t)n))
        stop(p);
}
static void body_end(parser *p, const char *n) {
    body_context *b = p->context;
    office_context *c = b->c;
    const char *l = local(n);
    bool sheet = space(n, "xlsx"), word = space(n, "docx"), draw = drawing(n);
    if (c->skip_depth) {
        if (p->depth == c->skip_depth)
            c->skip_depth = 0;
        return;
    }
    if (b->keep && (word || draw) && !strcmp(l, "p"))
        if (!append(&c->text, "\n", 1))
            stop(p);
    if (b->keep && word && !strcmp(l, "tc"))
        if (!append(&c->text, "\t", 1))
            stop(p);
    if (p->depth == c->capture_depth)
        c->capture_depth = 0;
    if (b->mode == 2 && sheet && !strcmp(l, "si")) {
        if (!c->in_shared || c->shared_count >= 32768 || c->cell.truncated ||
            c->cell.size > 4u * 1024u * 1024u - c->shared_bytes) {
            stop(p);
            return;
        }
        char **list = realloc(c->shared, (c->shared_count + 1) * sizeof(char *));
        if (!list) {
            stop(p);
            return;
        }
        c->shared = list;
        list[c->shared_count] = sb_strdup(c->cell.value ? c->cell.value : "");
        if (!list[c->shared_count]) {
            stop(p);
            return;
        }
        c->shared_count++;
        c->shared_bytes += c->cell.size;
        c->in_shared = false;
    }
    if (b->mode == 3 && sheet && !strcmp(l, "c")) {
        if (!cell_finish(c))
            stop(p);
        c->cell_depth = 0;
    }
}
static bool xml_body(package *z, part *part_value, office_context *c, unsigned mode, bool keep) {
    body_context b = {c, mode, keep};
    parser p = {.context = &b, .start = body_start, .end = body_end, .text = body_text};
    return parse(z, part_value, &p) && !c->capture_depth && !c->cell_depth && !c->in_shared;
}
bool sb_office_document(const unsigned char *bytes, size_t size, const char *format, const char *operation,
                        unsigned unit, yyjson_mut_doc *doc, sb_document *document, sb_error *error) {
    package z = {0};
    office_context *c = calloc(1, sizeof(*c));
    relations *roots = calloc(1, sizeof(*roots)), *rels = calloc(1, sizeof(*rels));
    bool ok = c && roots && rels && zip_open(&z, bytes, size), valid = true;
    part *main = NULL;
    if (c)
        c->format = format;
    if (ok)
        ok = load_rels(&z, "", roots);
    if (ok) {
        main = target(&z, "", rel_find(roots, NULL, "officeDocument", &valid));
        ok = main && valid;
    }
    bool word = !strcmp(format, "docx"), info = !strcmp(operation, "document_info");
    const char *expected =
        word ? "application/vnd.openxmlformats-officedocument.wordprocessingml.document.main+xml"
        : !strcmp(format, "xlsx")
            ? "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet.main+xml"
            : "application/vnd.openxmlformats-officedocument.presentationml.presentation.main+xml";
    if (ok) {
        content_type ct = {main->name, expected, false};
        parser p = {.context = &ct, .start = ct_start};
        ok = parse(&z, find(&z, "[Content_Types].xml"), &p) && ct.found;
    }
    if (ok)
        ok = xml_body(&z, main, c, 0, word && !info);
    if (ok && word) {
        c->count = 1;
        strcpy(c->units[0].name, "Document text");
    }
    if (ok) {
        document->unit_count = c->count;
        document->unit_kind = word ? "document" : !strcmp(format, "xlsx") ? "worksheet" : "slide";
        ok = c->count > 0;
    }
    if (ok && info) {
        document->units = yyjson_mut_arr(doc);
        for (unsigned i = 0; i < c->count && i < 128; i++) {
            yyjson_mut_val *item = yyjson_mut_obj(doc);
            yyjson_mut_obj_add_uint(doc, item, "unit", i + 1);
            yyjson_mut_obj_add_strcpy(doc, item, "name", c->units[i].name);
            yyjson_mut_arr_add_val(document->units, item);
        }
    } else if (ok) {
        ok = unit >= 1 && unit <= c->count;
        if (ok)
            strcpy(document->unit_name, c->units[unit - 1].name);
        if (ok && !word) {
            ok = load_rels(&z, main->name, rels);
            if (ok && !strcmp(format, "xlsx")) {
                relation *r = rel_find(rels, NULL, "sharedStrings", &valid);
                ok = valid;
                if (ok && r) {
                    part *shared = target(&z, main->name, r);
                    ok = shared && xml_body(&z, shared, c, 2, false);
                }
            }
            if (ok) {
                relation *r = rel_find(rels, c->units[unit - 1].id,
                                       !strcmp(format, "xlsx") ? "worksheet" : "slide", &valid);
                part *body = valid ? target(&z, main->name, r) : NULL;
                ok = body && xml_body(&z, body, c, !strcmp(format, "xlsx") ? 3 : 1, true);
            }
        }
        if (ok) {
            ok = append(&c->text, "", 0);
            if (ok) {
                document->text = c->text.value;
                document->text_size = c->text.size;
                document->extraction_truncated = c->text.truncated;
                c->text.value = NULL;
            }
        }
    }
    if (c) {
        free(c->text.value);
        free(c->cell.value);
        for (size_t i = 0; i < c->shared_count; i++)
            free(c->shared[i]);
        free(c->shared);
        free(c);
    }
    free(roots);
    free(rels);
    free(z.parts);
    if (!ok)
        sb_fail(error, "Office ZIP, content type, relationships, XML or extraction budget is invalid; no "
                       "content returned.");
    return ok;
}
