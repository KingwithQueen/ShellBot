#include "oauth.h"
#include <strings.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#define CLIENTS 32
#define GRANTS 64
#define FLOWS 16
#define REDIRECTS 4
#define URI_CAP 1024
#define STATE_CAP (512u * 1024u)
#define ACCESS_LEASES 8
typedef struct {
    char id[65], name[129], redirects[REDIRECTS][URI_CAP];
    size_t count;
    uint64_t created;
} client;
typedef struct {
    char access[65];
    uint64_t issued, end;
} access_lease;
typedef struct {
    char id[65], client[65], project[64], resource[512], scope[64];
    char access[65], refresh[65], previous[65], seed[65], binding[65], revocation_reason[32];
    uint64_t revision, issued, access_end, refresh_end, generation;
    bool revoked, persistent;
    access_lease leases[ACCESS_LEASES];
    size_t lease_count;
} grant;
typedef struct {
    char id[65], browser[65], code[65], client[65], project[64], redirect[URI_CAP], resource[512], scope[64],
        state[1025], challenge[44];
    char binding[65];
    uint64_t revision, end;
    int decision;
    bool delivered, used, persistent;
} flow;
struct sb_oauth {
    pthread_mutex_t lock;
    sb_secrets *secrets;
    char origin[320];
    bool enabled, failed;
    client clients[CLIENTS];
    grant grants[GRANTS];
    flow flows[FLOWS];
    size_t client_count, grant_count;
    uint64_t registration_window;
    unsigned registrations;
    char last_failure[40], failure_project[64];
    uint64_t failure_at;
};
typedef struct {
    char key[64], value[4097];
} parameter;
typedef struct {
    parameter items[20];
    size_t count;
} parameters;
static const char *str(yyjson_val *o, const char *key) {
    const char *s = sb_string(yyjson_obj_get(o, key));
    return s ? s : "";
}
static uint64_t now(void) { return sb_platform_unix_ms() / 1000; }
static bool hash(const char *s, char out[65]) {
    sb_error e = {0};
    return sb_platform_sha256(s, strlen(s), out, &e);
}
static bool equal(const char *a, const char *b) {
    size_t n = strlen(a);
    if (n != strlen(b))
        return false;
    volatile unsigned d = 0;
    for (size_t i = 0; i < n; ++i)
        d |= (unsigned char)a[i] ^ (unsigned char)b[i];
    return d == 0;
}
static bool random_id(char out[65]) {
    unsigned char b[32];
    sb_error e = {0};
    if (!sb_platform_random(b, sizeof(b), &e))
        return false;
    for (size_t i = 0; i < 32; ++i)
        snprintf(out + i * 2, 3, "%02x", b[i]);
    sb_posix_wipe(b, sizeof(b));
    return true;
}
static bool visible(const char *s, size_t min, size_t max) {
    size_t n = strlen(s);
    if (n < min || n > max)
        return false;
    for (size_t i = 0; i < n; ++i)
        if ((unsigned char)s[i] < 32 || (unsigned char)s[i] == 127)
            return false;
    return true;
}
static bool hex64(const char *s) { return strlen(s) == 64 && strspn(s, "0123456789abcdef") == 64; }
static char *json_error(const char *code, const char *message) {
    yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
    if (!d)
        return NULL;
    yyjson_mut_val *o = yyjson_mut_obj(d);
    yyjson_mut_doc_set_root(d, o);
    yyjson_mut_obj_add_str(d, o, "error", code);
    yyjson_mut_obj_add_str(d, o, "error_description", message);
    char *s = yyjson_mut_write(d, 0, NULL);
    yyjson_mut_doc_free(d);
    return s;
}
static bool error(sb_http_response *r, int status, const char *code, const char *message) {
    r->status = status;
    r->body = json_error(code, message);
    r->content_type = "application/json";
    return true;
}
static char *write_doc(yyjson_mut_doc *d) {
    char *s = yyjson_mut_write(d, 0, NULL);
    yyjson_mut_doc_free(d);
    return s;
}
static int unhex(char c) {
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}
static bool decode(const char *s, size_t n, char *out, size_t cap) {
    size_t j = 0;
    for (size_t i = 0; i < n; ++i) {
        unsigned char c = (unsigned char)s[i];
        if (c == '%') {
            if (i + 2 >= n || unhex(s[i + 1]) < 0 || unhex(s[i + 2]) < 0)
                return false;
            c = (unsigned char)(unhex(s[i + 1]) * 16 + unhex(s[i + 2]));
            i += 2;
        } else if (c == '+')
            c = ' ';
        if (!c || c < 32 || c == 127 || j + 1 >= cap)
            return false;
        out[j++] = (char)c;
    }
    out[j] = 0;
    return true;
}
static bool parse(parameters *p, const char *s) {
    memset(p, 0, sizeof(*p));
    if (!s || !*s)
        return true;
    if (strlen(s) > 8192)
        return false;
    while (*s) {
        if (p->count == 20)
            return false;
        const char *end = strchr(s, '&');
        if (!end)
            end = s + strlen(s);
        const char *eq = memchr(s, '=', (size_t)(end - s));
        if (!eq || eq == s)
            return false;
        parameter *v = &p->items[p->count];
        if (!decode(s, (size_t)(eq - s), v->key, sizeof(v->key)) ||
            !decode(eq + 1, (size_t)(end - eq - 1), v->value, sizeof(v->value)))
            return false;
        for (size_t i = 0; i < p->count; ++i)
            if (!strcmp(p->items[i].key, v->key))
                return false;
        ++p->count;
        if (!*end)
            break;
        s = end + 1;
        if (!*s)
            return false;
    }
    return true;
}
static const char *get(parameters *p, const char *key) {
    for (size_t i = 0; i < p->count; ++i)
        if (!strcmp(p->items[i].key, key))
            return p->items[i].value;
    return "";
}
static char *encode(const char *s) {
    size_t n = strlen(s);
    char *out = malloc(n * 3 + 1);
    if (!out)
        return NULL;
    size_t j = 0;
    static const char hex[] = "0123456789ABCDEF";
    for (size_t i = 0; i < n; ++i) {
        unsigned char c = (unsigned char)s[i];
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~')
            out[j++] = (char)c;
        else {
            out[j++] = '%';
            out[j++] = hex[c >> 4];
            out[j++] = hex[c & 15];
        }
    }
    out[j] = 0;
    return out;
}
static bool redirect_valid(const char *uri) {
    if (!visible(uri, 9, URI_CAP - 1) || strchr(uri, '#') || strchr(uri, '\\'))
        return false;
    for (const char *p = uri; *p; ++p)
        if ((unsigned char)*p >= 127 || *p == ' ' || *p == '"' || *p == '<' || *p == '>')
            return false;
    const char *start = !strncmp(uri, "https://", 8) ? uri + 8 : !strncmp(uri, "http://", 7) ? uri + 7 : NULL;
    if (!start)
        return false;
    const char *end = strpbrk(start, "/?");
    if (!end)
        end = start + strlen(start);
    if (end == start || memchr(start, '@', (size_t)(end - start)))
        return false;
    char origin[320];
    size_t n = (size_t)(end - uri);
    if (n >= sizeof(origin))
        return false;
    memcpy(origin, uri, n);
    origin[n] = 0;
    sb_config config = {0};
    sb_error e = {0};
    if (!sb_config_public_url(&config, origin, &e))
        return false;
    if (!strncmp(uri, "http://", 7) && strncmp(start, "127.0.0.1:", 10) && strncmp(start, "[::1]:", 6))
        return false;
    for (const char *p = uri; *p; ++p)
        if (*p == '%' && (unhex(p[1]) < 0 || !p[1] || !p[2] || unhex(p[2]) < 0))
            return false;
    return true;
}
static bool scope_valid(const char *s) {
    if (!*s)
        return true;
    char copy[64];
    if (strlen(s) >= sizeof(copy))
        return false;
    strcpy(copy, s);
    char *context = NULL, *part = strtok_r(copy, " ", &context);
    bool found = false, offline = false;
    while (part) {
        if (!strcmp(part, "mcp") && !found)
            found = true;
        else if (!strcmp(part, "offline_access") && !offline)
            offline = true;
        else
            return false;
        part = strtok_r(NULL, " ", &context);
    }
    return found;
}
static bool scope_matches(const char *requested, const char *granted) {
    return !*requested || (scope_valid(requested) &&
                           (!strstr(requested, "offline_access") || strstr(granted, "offline_access")));
}
static bool challenge_valid(const char *s) {
    return strlen(s) == 43 &&
           strspn(s, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_") == 43;
}
static bool pkce(const char *verifier, const char *challenge) {
    size_t n = strlen(verifier);
    if (n < 43 || n > 128 ||
        strspn(verifier, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-._~") != n)
        return false;
    char h[65];
    unsigned char raw[32];
    if (!hash(verifier, h))
        return false;
    for (size_t i = 0; i < 32; i++)
        raw[i] = (unsigned char)(unhex(h[i * 2]) * 16 + unhex(h[i * 2 + 1]));
    char *base64 = sb_base64(raw, 32);
    if (!base64)
        return false;
    for (size_t i = 0; i < 43; i++) {
        if (base64[i] == '+')
            base64[i] = '-';
        else if (base64[i] == '/')
            base64[i] = '_';
    }
    base64[43] = 0;
    bool ok = equal(base64, challenge);
    free(base64);
    return ok;
}
static bool refresh_mac(grant *g, uint64_t generation, char out[65]) {
    unsigned char bytes[32];
    char input[96];
    snprintf(input, sizeof(input), "%.64s.%llu", g->id, (unsigned long long)generation);
    bool ok = sb_secret_hmac((const unsigned char *)g->seed, 64, input, strlen(input), bytes);
    if (ok)
        for (size_t i = 0; i < 32; i++)
            snprintf(out + i * 2, 3, "%02x", bytes[i]);
    sb_posix_wipe(bytes, sizeof(bytes));
    return ok;
}
static bool refresh_matches(grant *g, const char *token, uint64_t *generation) {
    size_t n = strlen(token);
    if (n < 131 || n > 150 || strncmp(token, g->id, 64) || token[64] != '.')
        return false;
    const char *p = token + 65;
    if (*p < '1' || *p > '9')
        return false;
    uint64_t value = 0;
    size_t digits = 0;
    while (*p >= '0' && *p <= '9') {
        unsigned digit = (unsigned)(*p - '0');
        if (++digits > 20 || value > (UINT64_MAX - digit) / 10)
            return false;
        value = value * 10 + digit;
        ++p;
    }
    if (*p++ != '.' || !hex64(p))
        return false;
    char expected[65];
    if (!refresh_mac(g, value, expected) || !equal(expected, p))
        return false;
    *generation = value;
    return true;
}
static client *find_client(sb_oauth *o, const char *id) {
    for (size_t i = 0; i < o->client_count; ++i)
        if (equal(o->clients[i].id, id))
            return &o->clients[i];
    return NULL;
}
static bool has_redirect(client *c, const char *redirect) {
    for (size_t i = 0; i < c->count; ++i)
        if (!strcmp(c->redirects[i], redirect))
            return true;
    return false;
}
static const sb_oauth_project *project_resource(sb_oauth *o, const sb_oauth_project *p, size_t count,
                                                const char *resource) {
    for (size_t i = 0; i < count; ++i) {
        char expected[512];
        snprintf(expected, sizeof(expected), "%s/projects/%s/mcp", o->origin, p[i].id);
        if (p[i].enabled && !strcmp(resource, expected))
            return &p[i];
    }
    return NULL;
}
static const sb_oauth_project *find_project(const sb_oauth_project *p, size_t count, const char *id,
                                            uint64_t revision) {
    for (size_t i = 0; i < count; ++i)
        if (p[i].enabled && p[i].revision == revision && !strcmp(p[i].id, id))
            return &p[i];
    return NULL;
}
static bool grant_expired(const grant *g, uint64_t t) { return !g->persistent && g->refresh_end <= t; }
/* A durable owner grant follows the project's current operation policy, but
   never follows a reused ID with a different project credential. */
static bool grant_project(const grant *g, const sb_oauth_project *p) {
    return p && p->enabled && !strcmp(g->project, p->id) &&
           (!*g->binding || (hex64(p->binding) && equal(g->binding, p->binding))) &&
           (g->persistent || g->revision == p->revision);
}
static bool grant_current(const grant *g, const sb_oauth_project *p, size_t count) {
    for (size_t i = 0; i < count; ++i)
        if (grant_project(g, &p[i]))
            return true;
    return false;
}
static void failure(sb_oauth *o, const char *reason, const char *project) {
    snprintf(o->last_failure, sizeof(o->last_failure), "%s", reason);
    snprintf(o->failure_project, sizeof(o->failure_project), "%s", project ? project : "");
    o->failure_at = now();
}
static bool token_error(sb_oauth *o, sb_http_response *r, const char *reason, const char *project,
                        const char *description) {
    failure(o, reason, project);
    return error(r, 400, "invalid_grant", description);
}
static bool save(sb_oauth *o) {
    yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
    if (!d)
        return false;
    yyjson_mut_val *root = yyjson_mut_obj(d), *cs = yyjson_mut_arr(d), *gs = yyjson_mut_arr(d);
    yyjson_mut_doc_set_root(d, root);
    yyjson_mut_obj_add_uint(d, root, "version", 3);
    yyjson_mut_obj_add_str(d, root, "issuer", o->origin);
    yyjson_mut_obj_add_val(d, root, "clients", cs);
    yyjson_mut_obj_add_val(d, root, "grants", gs);
    for (size_t i = 0; i < o->client_count; ++i) {
        client *c = &o->clients[i];
        yyjson_mut_val *v = yyjson_mut_obj(d), *rs = yyjson_mut_arr(d);
        yyjson_mut_obj_add_str(d, v, "id", c->id);
        yyjson_mut_obj_add_str(d, v, "name", c->name);
        yyjson_mut_obj_add_uint(d, v, "created", c->created);
        for (size_t j = 0; j < c->count; ++j)
            yyjson_mut_arr_add_str(d, rs, c->redirects[j]);
        yyjson_mut_obj_add_val(d, v, "redirects", rs);
        yyjson_mut_arr_add_val(cs, v);
    }
    for (size_t i = 0; i < o->grant_count; ++i) {
        grant *g = &o->grants[i];
        yyjson_mut_val *v = yyjson_mut_obj(d);
#define S(k) yyjson_mut_obj_add_str(d, v, #k, g->k)
#define N(k) yyjson_mut_obj_add_uint(d, v, #k, g->k)
        S(id);
        S(client);
        S(project);
        S(resource);
        S(scope);
        S(access);
        S(refresh);
        S(previous);
        S(seed);
        S(binding);
        S(revocation_reason);
        N(revision);
        N(issued);
        N(access_end);
        N(refresh_end);
        N(generation);
        yyjson_mut_obj_add_bool(d, v, "revoked", g->revoked);
        yyjson_mut_obj_add_bool(d, v, "persistent", g->persistent);
        yyjson_mut_val *leases = yyjson_mut_arr(d);
        for (size_t j = 0; j < g->lease_count; ++j) {
            access_lease *a = &g->leases[j];
            yyjson_mut_val *entry = yyjson_mut_obj(d);
            yyjson_mut_obj_add_str(d, entry, "access", a->access);
            yyjson_mut_obj_add_uint(d, entry, "issued", a->issued);
            yyjson_mut_obj_add_uint(d, entry, "end", a->end);
            yyjson_mut_arr_add_val(leases, entry);
        }
        yyjson_mut_obj_add_val(d, v, "access_leases", leases);
        yyjson_mut_arr_add_val(gs, v);
#undef S
#undef N
    }
    char *plain = write_doc(d);
    if (!plain) {
        o->failed = true;
        return false;
    }
    size_t bytes = strlen(plain) + 1;
    sb_error e = {0};
    bool ok = bytes <= STATE_CAP && sb_secret_write(o->secrets, "oauth.sealed", plain, bytes, &e);
    sb_posix_wipe(plain, bytes);
    free(plain);
    if (!ok)
        o->failed = true;
    return ok;
}
static bool load(sb_oauth *o, sb_error *error) {
    size_t length = 0;
    bool missing = false;
    char *plain = sb_secret_read(o->secrets, "oauth.sealed", &length, &missing, error);
    if (missing)
        return true;
    if (!plain)
        return false;
    bool ok = length > 1 && length <= STATE_CAP && plain[length - 1] == 0 && !memchr(plain, 0, length - 1);
    yyjson_doc *doc = ok ? yyjson_read(plain, length - 1, 0) : NULL;
    sb_posix_wipe(plain, length);
    free(plain);
    yyjson_val *root = yyjson_doc_get_root(doc), *cs = yyjson_obj_get(root, "clients"),
               *gs = yyjson_obj_get(root, "grants");
    uint64_t version = yyjson_get_uint(yyjson_obj_get(root, "version"));
    ok = doc && version == 3 && yyjson_obj_size(root) == 4 && yyjson_is_arr(cs) &&
         yyjson_arr_size(cs) <= CLIENTS && yyjson_is_arr(gs) && yyjson_arr_size(gs) <= GRANTS;
    if (ok && strcmp(str(root, "issuer"), o->origin)) {
        sb_fail(error, "Stored OAuth issuer differs from public_url; use a separate manager Data directory "
                       "or explicitly recover the old issuer. Existing ciphertext was retained.");
        ok = false;
        goto done;
    }
    if (ok) {
        for (size_t i = 0; i < yyjson_arr_size(cs) && ok; ++i) {
            yyjson_val *v = yyjson_arr_get(cs, i), *rs = yyjson_obj_get(v, "redirects");
            client *c = &o->clients[o->client_count];
            ok = yyjson_obj_size(v) == 4 && hex64(str(v, "id")) && visible(str(v, "name"), 1, 128) &&
                 yyjson_is_uint(yyjson_obj_get(v, "created")) && yyjson_is_arr(rs) &&
                 yyjson_arr_size(rs) > 0 && yyjson_arr_size(rs) <= REDIRECTS && !find_client(o, str(v, "id"));
            if (!ok)
                break;
            strcpy(c->id, str(v, "id"));
            strcpy(c->name, str(v, "name"));
            c->created = yyjson_get_uint(yyjson_obj_get(v, "created"));
            for (size_t j = 0; j < yyjson_arr_size(rs); ++j) {
                const char *uri = sb_string(yyjson_arr_get(rs, j));
                if (!uri || !redirect_valid(uri)) {
                    ok = false;
                    break;
                }
                strcpy(c->redirects[c->count++], uri);
            }
            ++o->client_count;
        }
        for (size_t i = 0; i < yyjson_arr_size(gs) && ok; ++i) {
            yyjson_val *v = yyjson_arr_get(gs, i);
            grant *g = &o->grants[o->grant_count];
            ok = yyjson_obj_size(v) == (version == 3   ? 19u
                                        : version == 2 ? 16u
                                                       : 15u) &&
                 hex64(str(v, "id")) && find_client(o, str(v, "client")) &&
                 sb_identifier(str(v, "project")) && visible(str(v, "resource"), 1, 511) &&
                 scope_valid(str(v, "scope")) && hex64(str(v, "seed")) && hex64(str(v, "access")) &&
                 hex64(str(v, "refresh")) && (!*str(v, "previous") || hex64(str(v, "previous"))) &&
                 yyjson_is_bool(yyjson_obj_get(v, "revoked"));
            if (!ok)
                break;
#define S(k) strcpy(g->k, str(v, #k))
#define N(k)                                                                                                 \
    g->k = yyjson_get_uint(yyjson_obj_get(v, #k));                                                           \
    if (!yyjson_is_uint(yyjson_obj_get(v, #k)))                                                              \
    ok = false
            S(id);
            S(client);
            S(project);
            S(resource);
            S(scope);
            S(access);
            S(refresh);
            S(previous);
            S(seed);
            N(revision);
            N(issued);
            N(access_end);
            N(refresh_end);
            N(generation);
            g->revoked = yyjson_get_bool(yyjson_obj_get(v, "revoked"));
            if (version == 3) {
                ok =
                    ok && yyjson_is_bool(yyjson_obj_get(v, "persistent")) &&
                    (!*str(v, "binding") || hex64(str(v, "binding"))) &&
                    (!*str(v, "revocation_reason") || !strcmp(str(v, "revocation_reason"), "owner_revoked") ||
                     !strcmp(str(v, "revocation_reason"), "client_revoked") ||
                     !strcmp(str(v, "revocation_reason"), "refresh_reused"));
                if (!ok)
                    break;
                g->persistent = yyjson_get_bool(yyjson_obj_get(v, "persistent"));
                S(binding);
                S(revocation_reason);
            }
            if (!g->generation || g->issued > g->access_end || g->access_end - g->issued > 900 ||
                (g->persistent ? (!hex64(g->binding) || g->refresh_end != 0)
                               : g->access_end > g->refresh_end))
                ok = false;
            for (size_t j = 0; j < o->grant_count; ++j)
                if (equal(g->id, o->grants[j].id))
                    ok = false;
            if (version >= 2) {
                yyjson_val *entries = yyjson_obj_get(v, "access_leases");
                ok = ok && yyjson_is_arr(entries) && yyjson_arr_size(entries) <= ACCESS_LEASES;
                for (size_t j = 0; j < yyjson_arr_size(entries) && ok; ++j) {
                    yyjson_val *entry = yyjson_arr_get(entries, j);
                    access_lease *a = &g->leases[j];
                    ok = yyjson_obj_size(entry) == 3 && hex64(str(entry, "access")) &&
                         yyjson_is_uint(yyjson_obj_get(entry, "issued")) &&
                         yyjson_is_uint(yyjson_obj_get(entry, "end"));
                    if (!ok)
                        break;
                    strcpy(a->access, str(entry, "access"));
                    a->issued = yyjson_get_uint(yyjson_obj_get(entry, "issued"));
                    a->end = yyjson_get_uint(yyjson_obj_get(entry, "end"));
                    ok = a->issued <= a->end && (g->persistent || a->end <= g->refresh_end) &&
                         a->end - a->issued <= 900 && !equal(a->access, g->access);
                    for (size_t k = 0; k < j; ++k)
                        if (equal(a->access, g->leases[k].access))
                            ok = false;
                    ++g->lease_count;
                }
            }
            ++o->grant_count;
#undef S
#undef N
        }
    }
done:
    yyjson_doc_free(doc);
    if (!ok && !*error->text)
        sb_fail(error, "OAuth encrypted state is corrupt or foreign; existing ciphertext was retained.");
    return ok;
}
sb_oauth *sb_oauth_new(sb_secrets *secrets, const char *origin, sb_error *error) {
    sb_oauth *o = calloc(1, sizeof(*o));
    if (!o)
        return NULL;
    pthread_mutex_init(&o->lock, NULL);
    o->secrets = secrets;
    if (!secrets || !origin || strlen(origin) >= sizeof(o->origin) || strncmp(origin, "https://", 8)) {
        sb_fail(error, "OAuth requires encrypted credential storage and a configured HTTPS public origin.");
        sb_oauth_free(o);
        return NULL;
    }
    strcpy(o->origin, origin);
    o->enabled = true;
    if (!load(o, error)) {
        sb_oauth_free(o);
        return NULL;
    }
    return o;
}
void sb_oauth_free(sb_oauth *o) {
    if (!o)
        return;
    pthread_mutex_destroy(&o->lock);
    sb_posix_wipe(o, sizeof(*o));
    free(o);
}
static bool registration(sb_oauth *o, const sb_http_request *q, sb_http_response *r) {
    yyjson_doc *d = yyjson_read(q->body, q->length, 0);
    yyjson_val *v = yyjson_doc_get_root(d), *rs = yyjson_obj_get(v, "redirect_uris"),
               *gs = yyjson_obj_get(v, "grant_types"), *ts = yyjson_obj_get(v, "response_types");
    client c = {0};
    bool ok =
        d && yyjson_is_obj(v) && yyjson_is_arr(rs) && yyjson_arr_size(rs) > 0 &&
        yyjson_arr_size(rs) <= REDIRECTS &&
        (!*str(v, "token_endpoint_auth_method") || !strcmp(str(v, "token_endpoint_auth_method"), "none"));
    size_t seen = 0;
    const char *keys[32];
    yyjson_obj_iter it = yyjson_obj_iter_with(v);
    yyjson_val *key;
    while (ok && (key = yyjson_obj_iter_next(&it)) != NULL) {
        const char *name = yyjson_get_str(key);
        if (seen == 32) {
            ok = false;
            break;
        }
        for (size_t j = 0; j < seen; ++j)
            if (!strcmp(keys[j], name))
                ok = false;
        keys[seen++] = name;
    }
    const char *name = *str(v, "client_name") ? str(v, "client_name") : "MCP client";
    ok = ok && visible(name, 1, 128);
    if (ok)
        strcpy(c.name, name);
    for (size_t i = 0; i < yyjson_arr_size(rs) && ok; ++i) {
        const char *uri = sb_string(yyjson_arr_get(rs, i));
        ok = uri && redirect_valid(uri);
        if (ok) {
            for (size_t j = 0; j < c.count; ++j)
                if (!strcmp(c.redirects[j], uri))
                    ok = false;
            if (ok)
                strcpy(c.redirects[c.count++], uri);
        }
    }
    if (gs) {
        ok = ok && yyjson_is_arr(gs) && yyjson_arr_size(gs) > 0 && yyjson_arr_size(gs) <= 2;
        bool code = false;
        for (size_t i = 0; i < yyjson_arr_size(gs) && ok; ++i) {
            const char *g = sb_string(yyjson_arr_get(gs, i));
            if (g && !strcmp(g, "authorization_code"))
                code = true;
            else if (!g || strcmp(g, "refresh_token"))
                ok = false;
        }
        ok = ok && code;
    }
    if (ts)
        ok = ok && yyjson_is_arr(ts) && yyjson_arr_size(ts) == 1 && sb_string(yyjson_arr_get(ts, 0)) &&
             !strcmp(sb_string(yyjson_arr_get(ts, 0)), "code");
    yyjson_doc_free(d);
    if (!ok)
        return error(r, 400, "invalid_client_metadata",
                     "Use public-client PKCE and exact HTTPS or loopback redirect URIs.");
    uint64_t t = now();
    if (t - o->registration_window >= 60) {
        o->registration_window = t;
        o->registrations = 0;
    }
    if (o->registrations >= 4 || o->client_count == CLIENTS)
        return error(r, 429, "temporarily_unavailable",
                     "Registration limit reached. Remove unused clients in ShellBot or retry later.");
    if (!random_id(c.id))
        return error(r, 503, "temporarily_unavailable", "Secure random unavailable.");
    c.created = t;
    o->clients[o->client_count++] = c;
    ++o->registrations;
    if (!save(o))
        return error(r, 503, "temporarily_unavailable", "Credential storage unavailable.");
    yyjson_mut_doc *out = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = yyjson_mut_obj(out), *array = yyjson_mut_arr(out);
    yyjson_mut_doc_set_root(out, root);
    yyjson_mut_obj_add_str(out, root, "client_id", c.id);
    yyjson_mut_obj_add_str(out, root, "client_name", c.name);
    yyjson_mut_obj_add_uint(out, root, "client_id_issued_at", c.created);
    yyjson_mut_obj_add_str(out, root, "token_endpoint_auth_method", "none");
    for (size_t i = 0; i < c.count; ++i)
        yyjson_mut_arr_add_str(out, array, c.redirects[i]);
    yyjson_mut_obj_add_val(out, root, "redirect_uris", array);
    array = yyjson_mut_arr(out);
    yyjson_mut_arr_add_str(out, array, "authorization_code");
    yyjson_mut_arr_add_str(out, array, "refresh_token");
    yyjson_mut_obj_add_val(out, root, "grant_types", array);
    array = yyjson_mut_arr(out);
    yyjson_mut_arr_add_str(out, array, "code");
    yyjson_mut_obj_add_val(out, root, "response_types", array);
    r->status = 201;
    r->body = write_doc(out);
    return true;
}
static bool metadata(sb_oauth *o, const sb_oauth_project *projects, size_t count, const sb_http_request *q,
                     sb_http_response *r) {
    yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
    if (!d)
        return error(r, 503, "temporarily_unavailable", "Allocation failed.");
    yyjson_mut_val *root = yyjson_mut_obj(d), *array;
    yyjson_mut_doc_set_root(d, root);
    if (!strcmp(q->path, "/.well-known/oauth-authorization-server")) {
        yyjson_mut_obj_add_str(d, root, "issuer", o->origin);
        char endpoint[384];
        const char *keys[] = {"authorization_endpoint", "token_endpoint", "registration_endpoint",
                              "revocation_endpoint"},
                   *paths[] = {"/oauth/authorize", "/oauth/token", "/oauth/register", "/oauth/revoke"};
        for (size_t i = 0; i < 4; ++i) {
            snprintf(endpoint, sizeof(endpoint), "%s%s", o->origin, paths[i]);
            yyjson_mut_obj_add_strcpy(d, root, keys[i], endpoint);
        }
        yyjson_mut_obj_add_bool(d, root, "authorization_response_iss_parameter_supported", true);
        const char *names[] = {"response_types_supported",
                               "grant_types_supported",
                               "code_challenge_methods_supported",
                               "token_endpoint_auth_methods_supported",
                               "revocation_endpoint_auth_methods_supported",
                               "scopes_supported"};
        for (size_t i = 0; i < 6; ++i) {
            array = yyjson_mut_arr(d);
            yyjson_mut_arr_add_str(d, array,
                                   i == 0   ? "code"
                                   : i == 1 ? "authorization_code"
                                   : i == 2 ? "S256"
                                   : i == 5 ? "mcp"
                                            : "none");
            if (i == 1)
                yyjson_mut_arr_add_str(d, array, "refresh_token");
            if (i == 5)
                yyjson_mut_arr_add_str(d, array, "offline_access");
            yyjson_mut_obj_add_val(d, root, names[i], array);
        }
    } else {
        const sb_oauth_project *p = NULL;
        const char *prefix = "/.well-known/oauth-protected-resource";
        const char *suffix = q->path + strlen(prefix);
        for (size_t i = 0; i < count; ++i) {
            char path[128];
            snprintf(path, sizeof(path), "/projects/%s/mcp", projects[i].id);
            if (projects[i].enabled && (!strcmp(path, suffix) || (!*suffix && count == 1)))
                p = &projects[i];
        }
        if (!p) {
            yyjson_mut_doc_free(d);
            return error(r, 404, "invalid_target", "Use the selected project's protected-resource metadata.");
        }
        char resource[512];
        snprintf(resource, sizeof(resource), "%s/projects/%s/mcp", o->origin, p->id);
        yyjson_mut_obj_add_strcpy(d, root, "resource", resource);
        array = yyjson_mut_arr(d);
        yyjson_mut_arr_add_str(d, array, o->origin);
        yyjson_mut_obj_add_val(d, root, "authorization_servers", array);
        array = yyjson_mut_arr(d);
        yyjson_mut_arr_add_str(d, array, "mcp");
        yyjson_mut_arr_add_str(d, array, "offline_access");
        yyjson_mut_obj_add_val(d, root, "scopes_supported", array);
        array = yyjson_mut_arr(d);
        yyjson_mut_arr_add_str(d, array, "header");
        yyjson_mut_obj_add_val(d, root, "bearer_methods_supported", array);
    }
    r->status = 200;
    r->body = write_doc(d);
    return true;
}
static const char authorize_js[] =
    "'use strict';const "
    "id=document.body.dataset.request;document.getElementById('number').textContent=id.slice(0,8);async "
    "function poll(){try{const r=await "
    "fetch('/oauth/flow/'+id,{cache:'no-store',credentials:'same-origin'});const d=await "
    "r.json();if(d.redirect_uri){location.replace(d.redirect_uri);return;}document.getElementById('message')."
    "textContent=d.status==='pending'?'请在运行 ShellBot "
    "的本机确认此申请。':d.error_description||'此申请已结束，请返回客户端重新连接。';if(d.status==='pending')"
    "setTimeout(poll,1200);}catch(e){document.getElementById('message').textContent='连接暂不可用，请检查 "
    "ShellBot 是否仍在运行。';setTimeout(poll,2500);}}poll();";
static const char authorize_css[] =
    "body{font:18px system-ui,sans-serif;max-width:680px;margin:12vh "
    "auto;padding:24px;line-height:1.8;color:#243041;background:#f5f7fa}main{background:white;padding:32px;"
    "border-radius:14px}code{font-size:22px}h1{font-size:26px}";
static bool authorize(sb_oauth *o, const sb_oauth_project *projects, size_t count, const sb_http_request *q,
                      sb_http_response *r) {
    parameters p;
    if (!parse(&p, q->query))
        return error(r, 400, "invalid_request", "Malformed or duplicate authorization parameters.");
    const char *id = get(&p, "client_id"), *redirect = get(&p, "redirect_uri"),
               *resource = get(&p, "resource"), *scope = get(&p, "scope"), *state = get(&p, "state"),
               *challenge = get(&p, "code_challenge");
    client *c = find_client(o, id);
    const sb_oauth_project *project = project_resource(o, projects, count, resource);
    if (!c || !has_redirect(c, redirect))
        return error(r, 400, "invalid_request",
                     "Unknown client or unregistered redirect URI. No redirect was performed.");
    if (strcmp(get(&p, "response_type"), "code") || strcmp(get(&p, "code_challenge_method"), "S256") ||
        !challenge_valid(challenge) || !visible(state, 0, 1024))
        return error(r, 400, "invalid_request",
                     "Authorization code with S256 PKCE and a bounded state is required.");
    if (!project)
        return error(r, 400, "invalid_target", "The exact enabled project MCP URL is required as resource.");
    if (!scope_valid(scope))
        return error(r, 400, "invalid_scope", "Only mcp and offline_access are available.");
    flow *f = NULL;
    uint64_t t = now();
    for (size_t i = 0; i < FLOWS; ++i)
        if (o->flows[i].end <= t || o->flows[i].used) {
            f = &o->flows[i];
            break;
        }
    if (!f)
        return error(r, 429, "temporarily_unavailable", "Too many pending authorization requests.");
    memset(f, 0, sizeof(*f));
    char browser[65];
    if (!random_id(f->id) || !random_id(browser) || !hash(browser, f->browser))
        return error(r, 503, "temporarily_unavailable", "Secure random unavailable.");
    strcpy(f->client, id);
    strcpy(f->project, project->id);
    strcpy(f->binding, project->binding);
    strcpy(f->redirect, redirect);
    strcpy(f->resource, resource);
    strcpy(f->scope, strstr(scope, "offline_access") ? "mcp offline_access" : "mcp");
    strcpy(f->state, state);
    strcpy(f->challenge, challenge);
    f->revision = project->revision;
    f->end = t + 300;
    char html[1200], headers[400];
    snprintf(
        html, sizeof(html),
        "<!doctype html><html lang=\"zh-CN\"><meta charset=\"utf-8\"><meta name=\"viewport\" "
        "content=\"width=device-width,initial-scale=1\"><title>ShellBot 客户端授权</title><link "
        "rel=\"stylesheet\" href=\"/oauth/authorize.css\"><body data-request=\"%s\"><main><h1>连接 ShellBot "
        "项目</h1><p>申请编号：<code id=\"number\"></code></p><p "
        "id=\"message\">正在等待本机确认…</p><p>请核对编号、项目和回调地址。只有 ShellBot "
        "的本机所有者可以批准访问。</p></main><script src=\"/oauth/authorize.js\"></script></body></html>",
        f->id);
    snprintf(
        headers, sizeof(headers),
        "Set-Cookie: sb_oauth_flow=%s; Path=/oauth/flow/%s; Max-Age=300; HttpOnly; Secure; SameSite=Lax\r\n",
        browser, f->id);
    sb_posix_wipe(browser, sizeof(browser));
    r->status = 200;
    r->content_type = "text/html";
    r->body = sb_strdup(html);
    r->headers = sb_strdup(headers);
    return true;
}
static bool browser_matches(const char *cookies, const char *expected) {
    if (!cookies || strlen(cookies) > 4096)
        return false;
    char value[65] = {0};
    size_t found = 0;
    while (*cookies) {
        while (*cookies == ' ' || *cookies == ';')
            ++cookies;
        const char *end = strchr(cookies, ';');
        if (!end)
            end = cookies + strlen(cookies);
        if ((size_t)(end - cookies) == 78 && !memcmp(cookies, "sb_oauth_flow=", 14)) {
            memcpy(value, cookies + 14, 64);
            value[64] = 0;
            ++found;
        }
        cookies = end;
    }
    char h[65];
    return found == 1 && hash(value, h) && equal(h, expected);
}
static char *redirect_result(sb_oauth *o, flow *f) {
    char *state = encode(f->state), *issuer = encode(o->origin), *code = encode(f->code);
    size_t cap = strlen(f->redirect) + strlen(f->state) * 3 + strlen(o->origin) * 3 + 256;
    char *out = malloc(cap);
    if (out && state && issuer && code)
        snprintf(out, cap, "%s%c%s=%s&state=%s&iss=%s", f->redirect, strchr(f->redirect, '?') ? '&' : '?',
                 f->decision == 1 ? "code" : "error", f->decision == 1 ? code : "access_denied", state,
                 issuer);
    else {
        free(out);
        out = NULL;
    }
    free(state);
    free(issuer);
    free(code);
    return out;
}
static bool poll_flow(sb_oauth *o, const sb_http_request *q, sb_http_response *r) {
    const char *id = q->path + strlen("/oauth/flow/");
    flow *f = NULL;
    if (hex64(id))
        for (size_t i = 0; i < FLOWS; ++i)
            if (equal(o->flows[i].id, id))
                f = &o->flows[i];
    if (!f || !browser_matches(q->cookie, f->browser))
        return error(r, 403, "access_denied", "This browser does not own the authorization request.");
    if (f->end <= now() || f->used)
        return error(r, 400, "invalid_request",
                     "Authorization expired or already completed. Return to the client and reconnect.");
    yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *v = yyjson_mut_obj(d);
    yyjson_mut_doc_set_root(d, v);
    yyjson_mut_obj_add_str(d, v, "status", f->decision ? "complete" : "pending");
    char *redirect = f->decision ? redirect_result(o, f) : NULL;
    if (redirect)
        yyjson_mut_obj_add_strcpy(d, v, "redirect_uri", redirect);
    free(redirect);
    r->status = 200;
    r->body = write_doc(d);
    return true;
}
static bool access_matches(grant *g, const char *value, uint64_t t, bool live) {
    if (equal(g->access, value) && (!live || (g->issued <= t + 60 && g->access_end > t)))
        return true;
    for (size_t i = 0; i < g->lease_count; ++i)
        if (equal(g->leases[i].access, value) &&
            (!live || (g->leases[i].issued <= t + 60 && g->leases[i].end > t)))
            return true;
    return false;
}
static bool issue(sb_oauth *o, grant *g, sb_http_response *r, const char *response_scope) {
    grant original = *g, draft = *g;
    grant *destination = g;
    g = &draft;
    /* Rotation replaces refresh credentials, not still-valid access leases.
       Keep their original deadlines; saturation rejects renewal without changing
       the current refresh generation or invalidating a usable connection. */
    uint64_t t = now();
    size_t kept = 0;
    for (size_t i = 0; i < g->lease_count; ++i)
        if (g->leases[i].end > t)
            g->leases[kept++] = g->leases[i];
    g->lease_count = kept;
    if (*g->access && g->access_end > t) {
        if (kept == ACCESS_LEASES)
            return error(r, 429, "temporarily_unavailable",
                         "Too many live access credentials. Keep the current access token until it expires.");
        access_lease *a = &g->leases[g->lease_count++];
        strcpy(a->access, g->access);
        a->issued = g->issued;
        a->end = g->access_end;
    }
    char access[65], refresh[151], mac[65];
    if (g->generation == UINT64_MAX)
        return error(r, 400, "invalid_grant", "Refresh generation exhausted; reconnect.");
    ++g->generation;
    if (!random_id(access) || !refresh_mac(g, g->generation, mac) || !hash(access, g->access))
        return error(r, 503, "temporarily_unavailable", "Secure random unavailable.");
    snprintf(refresh, sizeof(refresh), "%.64s.%llu.%.64s", g->id, (unsigned long long)g->generation, mac);
    if (!hash(refresh, g->refresh))
        return error(r, 503, "temporarily_unavailable", "Hash unavailable.");
    g->issued = now();
    g->access_end = g->issued + 900;
    if (!g->persistent && g->access_end > g->refresh_end)
        g->access_end = g->refresh_end;
    yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
    if (!d)
        return error(r, 503, "temporarily_unavailable", "Allocation failed.");
    yyjson_mut_val *v = yyjson_mut_obj(d);
    yyjson_mut_doc_set_root(d, v);
    yyjson_mut_obj_add_str(d, v, "access_token", access);
    yyjson_mut_obj_add_str(d, v, "refresh_token", refresh);
    yyjson_mut_obj_add_str(d, v, "token_type", "Bearer");
    yyjson_mut_obj_add_uint(d, v, "expires_in", g->access_end - g->issued);
    yyjson_mut_obj_add_str(d, v, "scope", response_scope);
    char *body = write_doc(d);
    sb_posix_wipe(access, sizeof(access));
    sb_posix_wipe(refresh, sizeof(refresh));
    if (body)
        *destination = draft;
    if (!body || !save(o)) {
        *destination = original;
        if (body) {
            sb_posix_wipe(body, strlen(body));
            free(body);
        }
        return error(r, 503, "temporarily_unavailable",
                     "Credential storage unavailable; reconnect after recovery.");
    }
    r->status = 200;
    r->body = body;
    return true;
}
static bool token(sb_oauth *o, const sb_oauth_project *projects, size_t count, const sb_http_request *q,
                  sb_http_response *r) {
    parameters p;
    if (!parse(&p, q->body) || q->authorization || *get(&p, "client_secret")) {
        failure(o, "invalid_token_request", NULL);
        return error(r, 400, "invalid_request",
                     "Use a public client_id and form parameters; client secrets are not accepted.");
    }
    const char *id = get(&p, "client_id"), *kind = get(&p, "grant_type"), *resource = get(&p, "resource");
    if (!find_client(o, id)) {
        failure(o, "unknown_client", NULL);
        return error(r, 400, "invalid_client", "Unknown client.");
    }
    if (!strcmp(kind, "authorization_code")) {
        flow *f = NULL;
        for (size_t i = 0; i < FLOWS; ++i)
            if (*o->flows[i].code && equal(o->flows[i].code, get(&p, "code")))
                f = &o->flows[i];
        const sb_oauth_project *project = f ? find_project(projects, count, f->project, f->revision) : NULL;
        if (!f || f->used || f->decision != 1 || f->end <= now() || !equal(f->client, id) ||
            strcmp(f->redirect, get(&p, "redirect_uri")) || strcmp(f->resource, resource) || !project ||
            !equal(f->binding, project->binding) || !pkce(get(&p, "code_verifier"), f->challenge))
            return token_error(
                o, r, "authorization_code_invalid", f ? f->project : NULL,
                "Expired, consumed or mismatched authorization code, project, resource, redirect or PKCE.");
        grant *g = NULL;
        size_t previous_count = o->grant_count;
        for (size_t i = 0; i < o->grant_count; ++i)
            if (o->grants[i].revoked || grant_expired(&o->grants[i], now())) {
                g = &o->grants[i];
                break;
            }
        if (!g) {
            if (o->grant_count == GRANTS)
                return error(r, 429, "temporarily_unavailable", "Remove unused authorizations in ShellBot.");
            g = &o->grants[o->grant_count++];
        }
        grant previous = *g;
        memset(g, 0, sizeof(*g));
        if (!random_id(g->id) || !random_id(g->seed)) {
            *g = previous;
            o->grant_count = previous_count;
            return error(r, 503, "temporarily_unavailable", "Secure random unavailable.");
        }
        strcpy(g->client, id);
        strcpy(g->project, f->project);
        strcpy(g->binding, f->binding);
        strcpy(g->resource, resource);
        strcpy(g->scope, f->scope);
        g->revision = f->revision;
        g->persistent = f->persistent;
        g->refresh_end = g->persistent ? 0 : now() + 30u * 86400u;
        f->used = true;
        bool result = issue(o, g, r, g->scope);
        if (r->status != 200) {
            *g = previous;
            o->grant_count = previous_count;
            f->used = false;
        }
        return result;
    }
    if (!strcmp(kind, "refresh_token")) {
        if (!visible(get(&p, "refresh_token"), 131, 150))
            return token_error(o, r, "refresh_invalid", NULL, "Invalid refresh token.");
        for (size_t i = 0; i < o->grant_count; ++i) {
            grant *g = &o->grants[i];
            uint64_t generation = 0;
            if (!equal(g->client, id) || !refresh_matches(g, get(&p, "refresh_token"), &generation))
                continue;
            if (generation < g->generation) {
                g->revoked = true;
                strcpy(g->revocation_reason, "refresh_reused");
                save(o);
                return token_error(o, r, "refresh_reused", g->project,
                                   "Reused refresh token revoked this connection. Reconnect explicitly.");
            }
            if (generation != g->generation)
                return token_error(o, r, "refresh_invalid", g->project, "Invalid refresh generation.");
            if (g->revoked)
                return token_error(o, r, *g->revocation_reason ? g->revocation_reason : "connection_revoked",
                                   g->project, "Connection revoked. Reconnect explicitly.");
            if (grant_expired(g, now()))
                return token_error(o, r, "authorization_expired", g->project,
                                   "Authorization expired. Reconnect explicitly.");
            if (*resource && strcmp(g->resource, resource))
                return token_error(o, r, "resource_mismatch", g->project,
                                   "Refresh resource differs from the authorized project.");
            if (!grant_current(g, projects, count))
                return token_error(o, r, g->persistent ? "project_unavailable" : "policy_changed", g->project,
                                   "Project disabled, replaced or authorization policy changed.");
            const char *requested = get(&p, "scope");
            if (!scope_matches(requested, g->scope)) {
                failure(o, "scope_mismatch", g->project);
                return error(r, 400, "invalid_scope",
                             "Refresh scope cannot exceed the authorized scope set; order is insignificant.");
            }
            /* RFC 6749 section 6: access may narrow; the rotated refresh
               credential retains the original refresh scope. */
            grant previous = *g;
            strcpy(g->previous, g->refresh);
            bool result = issue(o, g, r,
                                !*requested                           ? g->scope
                                : strstr(requested, "offline_access") ? "mcp offline_access"
                                                                      : "mcp");
            if (r->status != 200)
                *g = previous;
            return result;
        }
        return token_error(o, r, "refresh_unknown", NULL, "Unknown refresh token.");
    }
    return error(r, 400, "unsupported_grant_type",
                 "Only authorization_code and refresh_token are supported.");
}
static bool oauth_revoke(sb_oauth *o, const sb_http_request *q, sb_http_response *r) {
    parameters p;
    if (!parse(&p, q->body) || q->authorization || *get(&p, "client_secret"))
        return error(r, 400, "invalid_request", "Expected public-client form parameters.");
    const char *id = get(&p, "client_id");
    if (!find_client(o, id))
        return error(r, 400, "invalid_client", "Unknown client.");
    char h[65];
    if (!hash(get(&p, "token"), h))
        return error(r, 503, "temporarily_unavailable", "Hash unavailable.");
    for (size_t i = 0; i < o->grant_count; ++i) {
        grant *g = &o->grants[i];
        uint64_t generation = 0;
        if (equal(g->client, id) &&
            (access_matches(g, h, 0, false) || equal(g->refresh, h) || equal(g->previous, h) ||
             (refresh_matches(g, get(&p, "token"), &generation) && generation <= g->generation))) {
            g->revoked = true;
            strcpy(g->revocation_reason, "client_revoked");
        }
    }
    if (!save(o))
        return error(r, 503, "temporarily_unavailable", "Revocation could not be persisted.");
    r->status = 200;
    r->body = sb_strdup("{}");
    return true;
}

bool sb_oauth_http(sb_oauth *o, const sb_oauth_project *projects, size_t count, const sb_http_request *q,
                   sb_http_response *r) {
    memset(r, 0, sizeof(*r));
    r->content_type = "application/json";
    if (!o || !o->enabled)
        return error(r, 404, "not_found",
                     "OAuth requires an enabled manager and a configured HTTPS public origin.");
    pthread_mutex_lock(&o->lock);
    bool result = true;
    if (o->failed) {
        result = error(r, 503, "temporarily_unavailable",
                       "OAuth persistence failed. Existing files were retained; restart after recovery.");
        goto done;
    }
    bool discovery = !strcmp(q->path, "/.well-known/oauth-authorization-server") ||
                     !strncmp(q->path, "/.well-known/oauth-protected-resource",
                              strlen("/.well-known/oauth-protected-resource"));
    bool post = !strcmp(q->path, "/oauth/register") || !strcmp(q->path, "/oauth/token") ||
                !strcmp(q->path, "/oauth/revoke");
    bool query = !strcmp(q->path, "/oauth/authorize");
    if ((post && strcmp(q->method, "POST")) || (!post && strcmp(q->method, "GET"))) {
        result = error(r, 405, "invalid_request", post ? "POST required." : "GET required.");
        goto done;
    }
    if ((!query && q->query) || strchr(q->path, '%') || strchr(q->path, '\\')) {
        result = error(r, 400, "invalid_request", "Unexpected query or noncanonical OAuth path.");
        goto done;
    }
    if (post) {
        const char *expected =
            !strcmp(q->path, "/oauth/register") ? "application/json" : "application/x-www-form-urlencoded";
        size_t n = strlen(expected);
        if (!q->content_type || strncasecmp(q->content_type, expected, n) ||
            (q->content_type[n] && q->content_type[n] != ';')) {
            result = error(r, 415, "invalid_request", "Unexpected OAuth content type.");
            goto done;
        }
    }
    if (discovery)
        result = metadata(o, projects, count, q, r);
    else if (!strcmp(q->path, "/oauth/register"))
        result = registration(o, q, r);
    else if (query)
        result = authorize(o, projects, count, q, r);
    else if (!strcmp(q->path, "/oauth/authorize.js")) {
        r->status = 200;
        r->content_type = "application/javascript";
        r->body = sb_strdup(authorize_js);
    } else if (!strcmp(q->path, "/oauth/authorize.css")) {
        r->status = 200;
        r->content_type = "text/css";
        r->body = sb_strdup(authorize_css);
    } else if (!strncmp(q->path, "/oauth/flow/", 12))
        result = poll_flow(o, q, r);
    else if (!strcmp(q->path, "/oauth/token"))
        result = token(o, projects, count, q, r);
    else if (!strcmp(q->path, "/oauth/revoke"))
        result = oauth_revoke(o, q, r);
    else
        result = error(r, 404, "not_found", "Unknown OAuth endpoint.");
done:
    pthread_mutex_unlock(&o->lock);
    return result;
}
bool sb_oauth_authorized(sb_oauth *o, const sb_oauth_project *project, const char *authorization) {
    if (!o || !o->enabled || !project || !authorization || strncasecmp(authorization, "Bearer ", 7) ||
        strlen(authorization + 7) != 64)
        return false;
    char h[65];
    if (!hash(authorization + 7, h))
        return false;
    bool ok = false;
    uint64_t t = now();
    pthread_mutex_lock(&o->lock);
    if (!o->failed)
        for (size_t i = 0; i < o->grant_count; ++i) {
            grant *g = &o->grants[i];
            if (!g->revoked && grant_project(g, project) && !grant_expired(g, t) &&
                access_matches(g, h, t, true)) {
                ok = true;
                break;
            }
        }
    pthread_mutex_unlock(&o->lock);
    return ok;
}
char *sb_oauth_challenge(sb_oauth *o, const char *path) {
    if (!o || !o->enabled || strncmp(path, "/projects/", 10))
        return NULL;
    const char *end = strchr(path + 10, '/');
    if (!end || strcmp(end, "/mcp") || (size_t)(end - path - 10) > 63)
        return NULL;
    char id[64];
    memcpy(id, path + 10, (size_t)(end - path - 10));
    id[end - path - 10] = 0;
    if (!sb_identifier(id))
        return NULL;
    char header[768];
    snprintf(
        header, sizeof(header),
        "WWW-Authenticate: Bearer "
        "resource_metadata=\"%s/.well-known/oauth-protected-resource/projects/%s/mcp\", scope=\"mcp\"\r\n",
        o->origin, id);
    return sb_strdup(header);
}

void sb_oauth_summary(sb_oauth *o, yyjson_mut_doc *d, yyjson_mut_val *root) {
    bool enabled = o && o->enabled;
    unsigned pending = 0;
    if (o) {
        pthread_mutex_lock(&o->lock);
        for (size_t i = 0; i < FLOWS; ++i)
            if (o->flows[i].end > now() && !o->flows[i].decision && !o->flows[i].used)
                ++pending;
        yyjson_mut_obj_add_bool(d, root, "oauth_storage_available", !o->failed);
        pthread_mutex_unlock(&o->lock);
    }
    yyjson_mut_obj_add_bool(d, root, "oauth_enabled", enabled);
    yyjson_mut_obj_add_uint(d, root, "oauth_pending", pending);
}
char *sb_oauth_owner(sb_oauth *o, const sb_oauth_project *projects, size_t count, yyjson_val *request) {
    if (!o)
        return sb_strdup("{\"error\":\"OAuth 未启用。\"}");
    const char *command = str(request, "command"), *id = str(request, "id");
    pthread_mutex_lock(&o->lock);
    char *result = NULL;
    uint64_t t = now();
    if (strcmp(command, "oauth-status")) {
        bool changed = false;
        if (!strcmp(command, "oauth-approve") || !strcmp(command, "oauth-deny")) {
            for (size_t i = 0; i < FLOWS; ++i) {
                flow *f = &o->flows[i];
                if (equal(f->id, id) && f->end > t && !f->decision && !f->used) {
                    const sb_oauth_project *project = find_project(projects, count, f->project, f->revision);
                    if ((!strcmp(command, "oauth-approve") &&
                         (!project || !equal(f->binding, project->binding))) ||
                        o->failed)
                        break;
                    f->persistent = yyjson_get_bool(yyjson_obj_get(request, "persistent"));
                    f->decision = !strcmp(command, "oauth-approve") ? 1 : 2;
                    f->end = t + 60;
                    if (f->decision == 1 && !random_id(f->code)) {
                        f->decision = 2;
                    }
                    changed = true;
                    break;
                }
            }
        } else if (!strcmp(command, "oauth-revoke")) {
            for (size_t i = 0; i < o->grant_count; ++i)
                if (equal(o->grants[i].id, id)) {
                    o->grants[i].revoked = true;
                    strcpy(o->grants[i].revocation_reason, "owner_revoked");
                    changed = save(o);
                    break;
                }
        } else if (!strcmp(command, "oauth-remove-client")) {
            for (size_t i = 0; i < o->client_count; ++i)
                if (equal(o->clients[i].id, id)) {
                    for (size_t j = 0; j < o->grant_count;) {
                        if (equal(o->grants[j].client, id)) {
                            memmove(&o->grants[j], &o->grants[j + 1],
                                    (o->grant_count - j - 1) * sizeof(grant));
                            --o->grant_count;
                        } else
                            ++j;
                    }
                    for (size_t j = 0; j < FLOWS; ++j)
                        if (equal(o->flows[j].client, id))
                            sb_posix_wipe(&o->flows[j], sizeof(flow));
                    memmove(&o->clients[i], &o->clients[i + 1], (o->client_count - i - 1) * sizeof(client));
                    --o->client_count;
                    changed = save(o);
                    break;
                }
        }
        if (!changed) {
            result = sb_strdup("{\"error\":\"申请已结束、项目配置已变更，或无法保存撤销。请刷新后核对。\"}");
            goto done;
        }
    }
    yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
    if (!d)
        goto done;
    yyjson_mut_val *root = yyjson_mut_obj(d), *pending = yyjson_mut_arr(d), *grants = yyjson_mut_arr(d),
                   *clients = yyjson_mut_arr(d);
    yyjson_mut_doc_set_root(d, root);
    yyjson_mut_obj_add_bool(d, root, "enabled", o->enabled);
    yyjson_mut_obj_add_bool(d, root, "storage_available", !o->failed);
    yyjson_mut_obj_add_str(d, root, "issuer", o->origin);
    for (size_t i = 0; i < FLOWS; ++i) {
        flow *f = &o->flows[i];
        if (f->end <= t || f->decision || f->used)
            continue;
        client *c = find_client(o, f->client);
        yyjson_mut_val *v = yyjson_mut_obj(d);
        yyjson_mut_obj_add_str(d, v, "id", f->id);
        yyjson_mut_obj_add_str(d, v, "project", f->project);
        yyjson_mut_obj_add_str(d, v, "client_name", c ? c->name : "Unknown client");
        yyjson_mut_obj_add_str(d, v, "redirect_uri", f->redirect);
        yyjson_mut_obj_add_str(d, v, "scope", f->scope);
        yyjson_mut_obj_add_bool(d, v, "policy_current",
                                find_project(projects, count, f->project, f->revision) != NULL);
        yyjson_mut_arr_add_val(pending, v);
    }
    for (size_t i = 0; i < o->grant_count; ++i) {
        grant *g = &o->grants[i];
        client *c = find_client(o, g->client);
        yyjson_mut_val *v = yyjson_mut_obj(d);
        yyjson_mut_obj_add_str(d, v, "id", g->id);
        yyjson_mut_obj_add_str(d, v, "project", g->project);
        yyjson_mut_obj_add_str(d, v, "client_name", c ? c->name : "Unknown client");
        yyjson_mut_obj_add_str(d, v, "status",
                               g->revoked                          ? "revoked"
                               : grant_expired(g, t)               ? "expired"
                               : grant_current(g, projects, count) ? "active"
                               : g->persistent                     ? "project_unavailable"
                                                                   : "policy_changed");
        yyjson_mut_obj_add_bool(d, v, "persistent", g->persistent);
        yyjson_mut_obj_add_str(d, v, "revocation_reason", g->revocation_reason);
        yyjson_mut_obj_add_uint(d, v, "expires", g->refresh_end);
        yyjson_mut_arr_add_val(grants, v);
    }
    for (size_t i = 0; i < o->client_count; ++i) {
        client *c = &o->clients[i];
        yyjson_mut_val *v = yyjson_mut_obj(d);
        yyjson_mut_obj_add_str(d, v, "id", c->id);
        yyjson_mut_obj_add_str(d, v, "client_name", c->name);
        yyjson_mut_obj_add_str(d, v, "redirect_uri", c->redirects[0]);
        yyjson_mut_arr_add_val(clients, v);
    }
    yyjson_mut_obj_add_val(d, root, "pending", pending);
    yyjson_mut_obj_add_val(d, root, "connections", grants);
    yyjson_mut_obj_add_val(d, root, "clients", clients);
    yyjson_mut_obj_add_str(d, root, "last_failure", o->last_failure);
    yyjson_mut_obj_add_str(d, root, "failure_project", o->failure_project);
    yyjson_mut_obj_add_uint(d, root, "failure_at", o->failure_at);
    result = write_doc(d);
done:
    pthread_mutex_unlock(&o->lock);
    return result;
}
