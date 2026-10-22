#include "posix.h"
#include "sb_projects.h"
#include "credentials.h"
#include "civetweb.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
int sb_stdio_run(sb_host *h) {
    char *line = malloc(SB_REQUEST_LIMIT + 1);
    if (!line)
        return 2;
    sb_protocol protocol = {0};
    int result = 0;
    while (!sb_posix_signal) {
        size_t size = 0;
        bool overflow = false;
        int c;
        while ((c = fgetc(stdin)) != EOF && c != '\n') {
            if (size < SB_REQUEST_LIMIT)
                line[size++] = (char)c;
            else
                overflow = true;
        }
        if (c == EOF && !size && !overflow)
            break;
        if (size && line[size - 1] == '\r')
            size--;
        if (!size && !overflow)
            continue;
        sb_request_context ctx = {.actor = "stdio",
                                  .project = h->project,
                                  .policy_revision = h->project ? h->project->policy_revision : 0};
        size_t length;
        int status;
        char *reply =
            sb_dispatch(h, &protocol, line, overflow ? SB_REQUEST_LIMIT + 1 : size, &ctx, &length, &status);
        if (reply) {
            bool ok =
                fwrite(reply, 1, length, stdout) == length && fputc('\n', stdout) != EOF && !fflush(stdout);
            free(reply);
            if (!ok) {
                result = 1;
                break;
            }
        } else if (status != 202) {
            result = 1;
            break;
        }
        if (c == EOF)
            break;
    }
    if (ferror(stdin) && !sb_posix_signal)
        result = 1;
    free(line);
    return result;
}

struct sb_http_server {
    sb_config config;
    sb_http_routes routes;
    void *context;
    struct mg_context *native;
    char error[256];
};
static int send_full(struct mg_connection *c, int status, const char *body, const char *type,
                     const char *headers) {
    mg_disable_keep_alive(c);
    size_t n = body ? strlen(body) : 0;
    mg_printf(
        c,
        "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\nCache-Control: "
        "no-store\r\nX-Content-Type-Options: nosniff\r\nReferrer-Policy: "
        "no-referrer\r\nContent-Security-Policy: default-src 'none'; script-src 'self'; style-src 'self'; "
        "connect-src 'self'; frame-ancestors 'none'; base-uri 'none'\r\nConnection: close\r\n%s\r\n",
        status,
        status == 200   ? "OK"
        : status == 201 ? "Created"
        : status == 202 ? "Accepted"
                        : "Request rejected",
        type ? type : "application/json; charset=utf-8", n, headers ? headers : "");
    if (n)
        mg_write(c, body, n);
    return status;
}
static int send_reply(struct mg_connection *c, int status, const char *body) {
    return send_full(c, status, body, NULL, NULL);
}
static int unauthorized(struct mg_connection *c, sb_http_server *s, const char *path) {
    char *header = s->routes.challenge ? s->routes.challenge(s->context, path) : NULL;
    int status = send_full(c, 401, "{\"error\":\"Bearer authentication required\"}", NULL, header);
    free(header);
    return status;
}
bool sb_posix_secret(const char *a, const char *b) {
    if (!a || !b || strlen(a) != strlen(b))
        return false;
    unsigned char diff = 0;
    for (size_t i = 0; b[i]; i++)
        diff |= (unsigned char)(a[i] ^ b[i]);
    return !diff;
}
char *sb_posix_bearer(const char *name, sb_error *e) {
    const char *token = getenv(name);
    if (!token || strlen(token) < 32 || strlen(token) > 128 ||
        strspn(token, "0123456789abcdefABCDEF") != strlen(token)) {
        sb_fail(e,
                "HTTP requires a distinct 32..128 character hexadecimal bearer in each project's token_env.");
        return NULL;
    }
    size_t n = strlen(token) + 8;
    char *b = malloc(n);
    if (b)
        snprintf(b, n, "Bearer %s", token);
    return b;
}
static int request(struct mg_connection *c) {
    const struct mg_request_info *r = mg_get_request_info(c);
    sb_http_server *s = mg_get_user_data(mg_get_context(c));
    const char *host = mg_get_header(c, "Host");
    char local[64], name[64];
    snprintf(local, sizeof(local), "127.0.0.1:%u", s->config.port);
    snprintf(name, sizeof(name), "localhost:%u", s->config.port);
    if (!host || (strcmp(host, local) && strcmp(host, name) &&
                  (!*s->config.public_authority || strcmp(host, s->config.public_authority))))
        return send_reply(c, 403, "{\"error\":\"Invalid host\"}");
    unsigned auth_count = 0;
    for (int i = 0; i < r->num_headers; i++)
        if (!strcasecmp(r->http_headers[i].name, "Authorization"))
            auth_count++;
    const char *path = r->local_uri;
    bool auxiliary = !strncmp(path, "/oauth/", 7) || !strncmp(path, "/.well-known/", 13);
    if (auxiliary && s->routes.auxiliary) {
        const char *origin = mg_get_header(c, "Origin");
        if ((origin && (!*s->config.public_origin || strcmp(origin, s->config.public_origin))) ||
            auth_count > 1)
            return send_reply(c, 403, "{\"error\":\"Invalid OAuth origin or authentication headers\"}");
        if (r->content_length > 16384 || mg_get_header(c, "Transfer-Encoding") ||
            (!strcmp(r->request_method, "POST") && r->content_length < 0))
            return send_reply(c, 413, "{\"error\":\"Bounded OAuth Content-Length required\"}");
        size_t n = r->content_length > 0 ? (size_t)r->content_length : 0, at = 0;
        char *bytes = malloc(n + 1);
        if (!bytes)
            return send_reply(c, 500, NULL);
        while (at < n) {
            int got = mg_read(c, bytes + at, n - at);
            if (got <= 0) {
                free(bytes);
                return send_reply(c, 400, NULL);
            }
            at += (size_t)got;
        }
        bytes[n] = 0;
        if (memchr(bytes, 0, n)) {
            free(bytes);
            return send_reply(c, 400, "{\"error\":\"Embedded NUL in OAuth body\"}");
        }
        sb_http_request request = {.method = r->request_method,
                                   .path = path,
                                   .query = r->query_string,
                                   .content_type = mg_get_header(c, "Content-Type"),
                                   .authorization = mg_get_header(c, "Authorization"),
                                   .cookie = mg_get_header(c, "Cookie"),
                                   .origin = origin,
                                   .body = bytes,
                                   .length = n};
        sb_http_response response = {0};
        bool handled = s->routes.auxiliary(s->context, &request, &response);
        int status =
            handled ? send_full(c, response.status, response.body, response.content_type, response.headers)
                    : send_reply(c, 404, NULL);
        if (response.body) {
            sb_posix_wipe(response.body, strlen(response.body));
            free(response.body);
        }
        free(response.headers);
        sb_posix_wipe(bytes, n);
        free(bytes);
        return status;
    }
    if (mg_get_header(c, "Origin") || r->query_string)
        return send_reply(c, 403, "{\"error\":\"Invalid origin or query\"}");
    if (!strcmp(path, "/healthz") && !strcmp(r->request_method, "GET"))
        return send_reply(c, 200, "{\"status\":\"ok\",\"platform_backend\":\"posix-phase3\"}");
    if (auth_count != 1)
        return unauthorized(c, s, path);
    bool health = false;
    int status = 401;
    void *binding = s->routes.open(s->context, path, mg_get_header(c, "Authorization"), &health, &status);
    if (!binding)
        return status == 401
                   ? unauthorized(c, s, path)
                   : send_reply(c, status, "{\"error\":\"Project unavailable or authentication failed\"}");
    char *reply = NULL, *bytes = NULL;
    int result = 0;
    if (strcmp(r->request_method, "POST")) {
        result = send_reply(c, 405, "{\"error\":\"POST required\"}");
        goto done;
    }
    const char *type = mg_get_header(c, "Content-Type");
    if (!type || strncasecmp(type, "application/json", 16) || (type[16] && type[16] != ';')) {
        result = send_reply(c, 415, "{\"error\":\"application/json required\"}");
        goto done;
    }
    if (r->content_length < 0 || r->content_length > SB_REQUEST_LIMIT ||
        mg_get_header(c, "Transfer-Encoding")) {
        result = send_reply(c, 413, "{\"error\":\"A bounded Content-Length is required\"}");
        goto done;
    }
    size_t n = (size_t)r->content_length, at = 0;
    bytes = malloc(n + 1);
    if (!bytes) {
        result = send_reply(c, 500, NULL);
        goto done;
    }
    while (at < n) {
        int got = mg_read(c, bytes + at, n - at);
        if (got <= 0) {
            result = send_reply(c, 400, "{\"error\":\"Incomplete request\"}");
            goto done;
        }
        at += (size_t)got;
    }
    bytes[n] = 0;
    sb_request_context ctx = {.actor = "http-bearer",
                              .version = mg_get_header(c, "MCP-Protocol-Version"),
                              .method = mg_get_header(c, "MCP-Method"),
                              .name = mg_get_header(c, "MCP-Name")};
    size_t length = 0;
    reply = s->routes.call(s->context, binding, bytes, n, &ctx, &length, &status);
    result = send_reply(c, status, reply);
done:
    s->routes.close(s->context, binding);
    free(reply);
    free(bytes);
    return result;
}
static int log_message(const struct mg_connection *c, const char *message) {
    if (c) {
        sb_http_server *s = mg_get_user_data(mg_get_context(c));
        if (s)
            snprintf(s->error, sizeof(s->error), "%s", message);
    }
    return 1;
}
sb_http_server *sb_http_start(const sb_config *config, const sb_http_routes *routes, void *context,
                              sb_error *e) {
    sb_http_server *s = calloc(1, sizeof(*s));
    if (!s)
        return NULL;
    s->config = *config;
    s->routes = *routes;
    s->context = context;
    char address[64];
    snprintf(address, sizeof(address), "127.0.0.1:%u", config->port);
    const char *options[] = {"listening_ports",
                             address,
                             "num_threads",
                             "4",
                             "request_timeout_ms",
                             "5000",
                             "enable_keep_alive",
                             "no",
                             "max_request_size",
                             "16384",
                             NULL};
    struct mg_callbacks callbacks = {.begin_request = request, .log_message = log_message};
    mg_init_library(0);
    s->native = mg_start(&callbacks, s, options);
    if (!s->native) {
        snprintf(e->text, sizeof(e->text), "Cannot bind loopback HTTP on port %u: %.160s", config->port,
                 *s->error ? s->error : "native listener initialization failed");
        mg_exit_library();
        free(s);
        return NULL;
    }
    fprintf(stderr, "ShellBot: listening on %s\n", address);
    return s;
}
bool sb_http_stopping(void) { return sb_posix_signal != 0; }
void sb_http_stop(sb_http_server *s) {
    if (s) {
        mg_stop(s->native);
        mg_exit_library();
        free(s);
    }
}
typedef struct {
    sb_host *host;
    char *authorization;
    pthread_mutex_t mutex;
} single;
static void *single_open(void *context, const char *path, const char *authorization, bool *health,
                         int *status) {
    single *s = context;
    char endpoint[100];
    snprintf(endpoint, sizeof(endpoint), "/projects/%s/mcp", s->host->project ? s->host->project->id : "");
    (void)health;
    if (strcmp(path, "/mcp") && (!s->host->project || strcmp(path, endpoint))) {
        *status = 404;
        return NULL;
    }
    if (!sb_posix_secret(authorization, s->authorization)) {
        *status = 401;
        return NULL;
    }
    pthread_mutex_lock(&s->mutex);
    return s;
}
static char *single_call(void *context, void *binding, const char *bytes, size_t n,
                         const sb_request_context *ctx, size_t *length, int *status) {
    (void)context;
    single *s = binding;
    sb_request_context trusted = *ctx;
    trusted.project = s->host->project;
    trusted.policy_revision = s->host->project ? s->host->project->policy_revision : 0;
    return sb_dispatch(s->host, NULL, bytes, n, &trusted, length, status);
}
static void single_close(void *context, void *binding) {
    (void)context;
    single *s = binding;
    pthread_mutex_unlock(&s->mutex);
}
int sb_http_run(sb_host *h, sb_error *e) {
    single s = {.host = h};
    s.authorization = sb_posix_bearer(h->config.token_env, e);
    if (!s.authorization)
        return 2;
    pthread_mutex_init(&s.mutex, NULL);
    sb_http_routes routes = {.open = single_open, .call = single_call, .close = single_close};
    sb_http_server *server = sb_http_start(&h->config, &routes, &s, e);
    if (server) {
        while (!sb_posix_signal && !sb_owner_stopping(h->owner)) {
            struct timespec pause = {.tv_nsec = 100000000};
            nanosleep(&pause, NULL);
        }
        sb_http_stop(server);
    }
    pthread_mutex_destroy(&s.mutex);
    memset(s.authorization, 0, strlen(s.authorization));
    free(s.authorization);
    return server ? 0 : 2;
}
