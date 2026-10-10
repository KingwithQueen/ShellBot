#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "sb.h"
#include "civetweb.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "owner_ui.h"

struct sb_http_server {
    const sb_config *config;
    sb_host *host;
    sb_http_routes routes;
    void *user;
    struct mg_context *listener;
    char token[257];
    size_t token_length;
};
typedef struct sb_http_server http_state;
static volatile LONG stopping;

static BOOL WINAPI stop_handler(DWORD event) {
    if (event == CTRL_C_EVENT || event == CTRL_BREAK_EVENT || event == CTRL_CLOSE_EVENT || event == CTRL_SHUTDOWN_EVENT) {
        InterlockedExchange(&stopping, 1); return TRUE;
    }
    return FALSE;
}

static int reply_typed(struct mg_connection *connection, int status, const char *body, size_t size, const char *extra, const char *type) {
    /* Complete a bounded incoming body before closing an error response on
       Windows. Unread socket data may otherwise discard the outgoing reply. */
    const struct mg_request_info *info = mg_get_request_info(connection);
    if (info->content_length >= 0 && info->content_length <= 2 * SB_REQUEST_LIMIT &&
        !mg_get_header(connection, "Transfer-Encoding")) {
        char discard[4096];
        size_t drained = 0;
        uint64_t deadline = sb_platform_millis() + 5000;
        int got;
        while (drained < 2 * SB_REQUEST_LIMIT && sb_platform_millis() < deadline &&
            (got = mg_read(connection, discard, sizeof(discard))) > 0) drained += (size_t)got;
    }
    const char *reason = status == 200 ? "OK" : status == 202 ? "Accepted" : status == 400 ? "Bad Request" :
        status == 401 ? "Unauthorized" : status == 403 ? "Forbidden" : status == 404 ? "Not Found" :
        status == 405 ? "Method Not Allowed" : status == 406 ? "Not Acceptable" :
        status == 411 ? "Length Required" : status == 413 ? "Content Too Large" : status == 415 ? "Unsupported Media Type" :
        status == 429 ? "Too Many Requests" : status == 503 ? "Service Unavailable" : "Internal Server Error";
    char header[1024];
    int header_size = snprintf(header, sizeof(header), "HTTP/1.1 %d %s\r\nContent-Type: %s; charset=utf-8\r\nContent-Length: %zu\r\n"
        "Cache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\n%s\r\n", status, reason, type, size, extra ? extra : "");
    if (header_size < 0 || (size_t)header_size >= sizeof(header) || size > SB_RESPONSE_LIMIT) return 500;
    char *frame = malloc((size_t)header_size + size);
    if (!frame) return 500;
    memcpy(frame, header, (size_t)header_size);
    if (body && size) memcpy(frame + header_size, body, size);
    mg_write(connection, frame, (size_t)header_size + size);
    free(frame);
    return status;
}
static int reply(struct mg_connection *connection, int status, const char *body, size_t size, const char *extra) {
    return reply_typed(connection,status,body,size,extra,"application/json");
}

static int reject(struct mg_connection *connection, int status, const char *message, const char *extra) {
    return reply(connection, status, message, strlen(message), extra);
}

static bool authorized(struct mg_connection *connection, const http_state *state) {
    const char *authorization = mg_get_header(connection, "Authorization");
    if (!authorization || _strnicmp(authorization, "Bearer ", 7)) return false;
    const char *token = authorization + 7;
    if (strlen(token) != state->token_length) return false;
    volatile unsigned difference = 0;
    for (size_t i = 0; i < state->token_length; ++i) difference |= (unsigned)((unsigned char)token[i] ^ (unsigned char)state->token[i]);
    return difference == 0;
}

static bool external_host(const sb_config *config, const char *value) {
    if (!value || !*config->public_authority) return false;
    if (!_stricmp(value, config->public_authority)) return true;
    if (!strchr(config->public_authority, ':')) {
        char with_port[272];
        snprintf(with_port, sizeof(with_port), "%s:%s", config->public_authority, config->public_default_port);
        return !_stricmp(value, with_port);
    }
    return false;
}

static bool external_origin(const sb_config *config, const char *value) {
    if (!value || !*config->public_origin) return false;
    if (!_stricmp(value, config->public_origin)) return true;
    if (!strchr(config->public_authority, ':')) {
        char with_port[328];
        snprintf(with_port, sizeof(with_port), "%s:%s", config->public_origin, config->public_default_port);
        return !_stricmp(value, with_port);
    }
    return false;
}
static bool valid_proxy_authority(const char *host) {
    if (!host || !*host || strlen(host) >= 264) return false;
    char origin[280]; snprintf(origin, sizeof(origin), "http://%s", host);
    sb_config parsed = {0}; sb_error error = {0};
    return !strchr(host, '/') && sb_config_public_url(&parsed, origin, &error);
}
static bool valid_proxy_origin(const char *origin) {
    sb_config parsed = {0}; sb_error error = {0};
    return origin && sb_config_public_url(&parsed, origin, &error) && external_origin(&parsed, origin);
}

static int handle_owner(struct mg_connection *connection, http_state *state) {
    const struct mg_request_info *info=mg_get_request_info(connection);
    if (!state->routes.owner_enabled || !state->routes.owner_enabled(state->user)) return reject(connection,404,"{\"error\":\"Owner route disabled.\"}",NULL);
    const char *origin=mg_get_header(connection,"Origin"), *host=mg_get_header(connection,"Host");
    char http[280],https[280]; snprintf(http,sizeof(http),"http://%s",host); snprintf(https,sizeof(https),"https://%s",host);
    if (origin && (_stricmp(origin,http) && _stricmp(origin,https)) && !external_origin(state->config,origin))
        return reject(connection,403,"{\"error\":\"Owner requests require the same origin.\"}",NULL);
    const char *security="Referrer-Policy: no-referrer\r\nContent-Security-Policy: default-src 'none'; script-src 'self'; style-src 'self'; connect-src 'self'; form-action 'self'; frame-ancestors 'none'; base-uri 'none'\r\nX-Frame-Options: DENY\r\n";
    if (strcmp(info->local_uri,"/owner/api")) {
        if (strcmp(info->request_method,"GET")) return reject(connection,405,"{\"error\":\"GET required.\"}","Allow: GET\r\n");
        bool script=!strcmp(info->local_uri,"/owner.js");
        bool style=!strcmp(info->local_uri,"/owner.css");
        return reply_typed(connection,200,style ? sb_owner_css : script ? sb_owner_js : sb_owner_html,style ? sizeof(sb_owner_css)-1 : script ? sizeof(sb_owner_js)-1 : sizeof(sb_owner_html)-1,security,
            style ? "text/css" : script ? "application/javascript" : "text/html");
    }
    /* Authenticate before reading the body or looking up a project. */
    if (!state->routes.owner_authorized || !state->routes.owner_authorized(state->user,mg_get_header(connection,"Authorization")))
        return reject(connection,401,"{\"error\":\"Independent owner bearer required.\"}","WWW-Authenticate: Bearer realm=\"ShellBot owner\"\r\n");
    if (strcmp(info->request_method,"POST")) return reject(connection,405,"{\"error\":\"POST required.\"}","Allow: POST\r\n");
    const char *type=mg_get_header(connection,"Content-Type");
    if (!type || _strnicmp(type,"application/json",16) || (type[16] && type[16]!=';')) return reject(connection,415,"{\"error\":\"application/json required.\"}",NULL);
    const char *accept=mg_get_header(connection,"Accept");
    if (!accept || !strstr(accept,"application/json")) return reject(connection,406,"{\"error\":\"Accept application/json required.\"}",NULL);
    if (mg_get_header(connection,"Transfer-Encoding") || info->content_length<0) return reject(connection,411,"{\"error\":\"Content-Length required.\"}",NULL);
    if (info->content_length>65536) return reject(connection,413,"{\"error\":\"Owner request exceeds 64 KiB.\"}",NULL);
    size_t size=(size_t)info->content_length,read=0; char *body=malloc(size+1);
    if (!body) return reject(connection,500,"{\"error\":\"Allocation failed.\"}",NULL);
    while (read<size) { int got=mg_read(connection,body+read,size-read); if (got<=0) break; read+=(size_t)got; }
    if (read!=size) { free(body); return reject(connection,400,"{\"error\":\"Incomplete request.\"}",NULL); }
    body[size]=0; int status=500; char *response=state->routes.owner_call(state->user,body,size,&status); free(body);
    int result=reply(connection,status,response,response ? strlen(response) : 0,security); free(response); return result;
}

static int handle_bound(struct mg_connection *connection, http_state *state, void *binding, bool health) {
    const struct mg_request_info *info = mg_get_request_info(connection);
    if (info->query_string) return reject(connection, 400, "{\"error\":\"Query parameters are not supported.\"}", NULL);
    if (health && !strcmp(info->request_method, "GET")) {
        const char body[] = "{\"ready\":true,\"version\":\"" SB_VERSION "\"}";
        return reply(connection, 200, body, sizeof(body) - 1, NULL);
    }
    if (health || strcmp(info->request_method, "POST"))
        return reject(connection, 405, "{\"error\":\"This route does not support the requested method or SSE sessions.\"}", health ? "Allow: GET\r\n" : "Allow: POST\r\n");
    const char *version = mg_get_header(connection, "MCP-Protocol-Version");
    const char *content_type = mg_get_header(connection, "Content-Type");
    if (!content_type || _strnicmp(content_type, "application/json", 16) || (content_type[16] && content_type[16] != ';'))
        return reject(connection, 415, "{\"error\":\"application/json required.\"}", NULL);
    const char *accept = mg_get_header(connection, "Accept");
    if (!accept || !strstr(accept, "application/json"))
        return reject(connection, 406, "{\"error\":\"Accept application/json required.\"}", NULL);
    if (mg_get_header(connection, "Transfer-Encoding") || info->content_length < 0)
        return reject(connection, 411, "{\"error\":\"Content-Length required; chunked requests are not supported yet.\"}", NULL);
    if (info->content_length > SB_REQUEST_LIMIT)
        return reject(connection, 413, "{\"error\":\"Request exceeds 1 MiB.\"}", NULL);
    size_t length = (size_t)info->content_length;
    char *request = malloc(length + 1);
    if (!request) return reject(connection, 500, "{\"error\":\"Out of memory.\"}", NULL);
    size_t read = 0;
    uint64_t deadline = sb_platform_millis() + 10000;
    while (read < length && sb_platform_millis() < deadline) {
        int got = mg_read(connection, request + read, length - read);
        if (got <= 0) break;
        read += (size_t)got;
    }
    if (read != length) { free(request); return reject(connection, 400, "{\"error\":\"Incomplete request.\"}", NULL); }
    size_t response_length;
    int status;
    const sb_request_context context = {.actor = "http-development", .version = version,
        .method = mg_get_header(connection, "Mcp-Method"), .name = mg_get_header(connection, "Mcp-Name")};
    char *response = state->routes.call ? state->routes.call(state->user, binding, request, length, &context, &response_length, &status) :
        sb_dispatch(state->host, NULL, request, length, &context, &response_length, &status);
    free(request);
    int result = reply(connection, status, response, response ? response_length : 0, NULL);
    free(response);
    return result;
}

static int handle(struct mg_connection *connection) {
    const struct mg_request_info *info = mg_get_request_info(connection);
    http_state *state = info->user_data;
    const sb_config *config = state->config;
    static const char *const critical[] = {"Authorization", "Host", "Origin", "Content-Length", "Transfer-Encoding", "Content-Type", "Accept", "MCP-Protocol-Version", "Mcp-Method", "Mcp-Name"};
    for (size_t i = 0; i < sizeof(critical) / sizeof(critical[0]); ++i) {
        unsigned count = 0;
        for (int j = 0; j < info->num_headers; ++j) if (!_stricmp(info->http_headers[j].name, critical[i])) ++count;
        if (count > 1) return reject(connection, 400, "{\"error\":\"Duplicate security or protocol headers.\"}", NULL);
    }
    char host[64], localhost[64], origin[80], local_origin[80];
    snprintf(host, sizeof(host), "127.0.0.1:%u", config->port);
    snprintf(localhost, sizeof(localhost), "localhost:%u", config->port);
    snprintf(origin, sizeof(origin), "http://%s", host);
    snprintf(local_origin, sizeof(local_origin), "http://%s", localhost);
    const char *received_host = mg_get_header(connection, "Host"), *received_origin = mg_get_header(connection, "Origin");
    /* Optional public_url constrains an existing proxy's Host/Origin. No DNS,
       cookies, CORS, forwarded credential headers, or tunnel management. */
    bool constrained = *config->public_origin != 0;
    bool host_valid = received_host && (!_stricmp(received_host, host) || !_stricmp(received_host, localhost) ||
        (constrained ? external_host(config, received_host) : valid_proxy_authority(received_host)));
    bool origin_valid = !received_origin || !strcmp(received_origin, origin) || !strcmp(received_origin, local_origin) ||
        (constrained ? external_origin(config, received_origin) : valid_proxy_origin(received_origin));
    if (!host_valid || !origin_valid) return reject(connection, 403, "{\"error\":\"Invalid Host or Origin.\"}", NULL);
    void *binding = NULL; bool health = false;
    if (state->routes.open) {
        /* decode_url=no preserves percent escapes; reject rewritten/absolute
           targets too, so only one canonical project route can authenticate. */
        if (!info->request_uri || !info->local_uri_raw || !info->local_uri ||
            strcmp(info->request_uri, info->local_uri_raw) || strcmp(info->local_uri_raw, info->local_uri) ||
            info->query_string || strchr(info->request_uri, '%') || strchr(info->request_uri, '\\'))
            return reject(connection, 400, "{\"error\":\"A canonical project route without query parameters is required.\"}", NULL);
        if (!strcmp(info->local_uri,"/owner") || !strcmp(info->local_uri,"/owner.js") || !strcmp(info->local_uri,"/owner.css") || !strcmp(info->local_uri,"/owner/api")) return handle_owner(connection,state);
        int status = 500;
        binding = state->routes.open(state->user, info->local_uri, mg_get_header(connection, "Authorization"), &health, &status);
        if (!binding) {
            const char *body = status == 401 ? "{\"error\":\"Project bearer token required.\"}" :
                status == 404 ? "{\"error\":\"Project route not found.\"}" :
                status == 429 ? "{\"error\":\"Project has an in-flight request. No request was queued.\"}" :
                "{\"error\":\"Project is unavailable; explicit owner action is required.\"}";
            return reject(connection, status, body, status == 401 ? "WWW-Authenticate: Bearer realm=\"ShellBot project\"\r\n" : NULL);
        }
    } else {
        if (!authorized(connection, state)) return reject(connection, 401, "{\"error\":\"Bearer token required.\"}", "WWW-Authenticate: Bearer realm=\"ShellBot local development\"\r\n");
        health = info->local_uri && !strcmp(info->local_uri, "/healthz");
        if (!health && (!info->local_uri || strcmp(info->local_uri, "/mcp"))) return reject(connection, 404, "{\"error\":\"Not found.\"}", NULL);
    }
    int result = handle_bound(connection, state, binding, health);
    if (binding) state->routes.close(state->user, binding);
    return result;
}

static int quiet_log(const struct mg_connection *connection, const char *message) {
    (void)connection; (void)message;
    /* Library diagnostics may contain attacker-controlled URLs. */
    return 1;
}

static bool listen_http(http_state *state, sb_error *error) {
    char listen[64];
    snprintf(listen, sizeof(listen), "127.0.0.1:%u", state->config->port);
    const char *options[] = {"listening_ports", listen, "num_threads", "4", "request_timeout_ms", "5000",
        "connection_queue", "4", "decode_url", state->routes.open ? "no" : "yes",
        "enable_keep_alive", "yes", "keep_alive_timeout_ms", "2000", "linger_timeout_ms", "5000",
        "tcp_nodelay", "1", "enable_directory_listing", "no", NULL};
    struct mg_callbacks callbacks = {0};
    callbacks.begin_request = handle; callbacks.log_message = quiet_log;
    InterlockedExchange(&stopping, 0);
    if (!SetConsoleCtrlHandler(stop_handler, TRUE)) {
        sb_fail(error, "Cannot install shutdown handler."); return false;
    }
    mg_init_library(0);
    state->listener = mg_start(&callbacks, state, options);
    if (!state->listener) {
        SetConsoleCtrlHandler(stop_handler, FALSE); mg_exit_library();
        sb_fail(error, "Cannot start loopback HTTP listener; check port availability."); return false;
    }
    return true;
}

sb_http_server *sb_http_start(const sb_config *config, const sb_http_routes *routes, void *context, sb_error *error) {
    if (!config || !routes || !routes->open || !routes->call || !routes->close) return NULL;
    http_state *state = calloc(1, sizeof(*state));
    if (!state) { sb_fail(error, "Cannot allocate HTTP gateway."); return NULL; }
    state->config = config; state->routes = *routes; state->user = context;
    if (!listen_http(state, error)) { free(state); return NULL; }
    return state;
}
bool sb_http_stopping(void) { return InterlockedCompareExchange(&stopping, 0, 0) != 0; }
void sb_http_stop(sb_http_server *state) {
    if (!state) return;
    mg_stop(state->listener); mg_exit_library(); SetConsoleCtrlHandler(stop_handler, FALSE);
    SecureZeroMemory(state, sizeof(*state)); free(state);
}

int sb_http_run(sb_host *host, sb_error *error) {
    http_state *state = calloc(1, sizeof(*state));
    if (!state) { sb_fail(error, "Cannot allocate HTTP host."); return 2; }
    state->host = host; state->config = &host->config;
    const char *token = getenv(host->config.token_env);
    size_t length = token ? strlen(token) : 0;
    if (length < 32 || length > 256) {
        sb_fail(error, "HTTP needs a 32..256 byte bearer token in the configured environment variable. Use --generate-token."); free(state); return 2;
    }
    for (size_t i = 0; i < length; ++i) if ((unsigned char)token[i] <= 32 || (unsigned char)token[i] >= 127) {
        sb_fail(error, "HTTP token must contain only visible ASCII without spaces."); free(state); return 2;
    }
    memcpy(state->token, token, length + 1); state->token_length = length;
    if (!listen_http(state, error)) { SecureZeroMemory(state, sizeof(*state)); free(state); return 1; }
    char listen[64]; snprintf(listen, sizeof(listen), "127.0.0.1:%u", host->config.port);
    fprintf(stderr, "ShellBot %s listening at http://%s/mcp (local development mode; writes require owner approval).\n", SB_VERSION, listen);
    while (!sb_http_stopping() && !sb_owner_stopping(host->owner)) Sleep(100);
    sb_http_stop(state);
    return 0;
}
