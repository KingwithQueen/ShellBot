#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "sb.h"
#include "civetweb.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { sb_host *host; char token[257]; size_t token_length; } http_state;
static volatile LONG stopping;

static BOOL WINAPI stop_handler(DWORD event) {
    if (event == CTRL_C_EVENT || event == CTRL_BREAK_EVENT || event == CTRL_CLOSE_EVENT || event == CTRL_SHUTDOWN_EVENT) {
        InterlockedExchange(&stopping, 1); return TRUE;
    }
    return FALSE;
}

static int reply(struct mg_connection *connection, int status, const char *body, size_t size, const char *extra) {
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
        status == 411 ? "Length Required" : status == 413 ? "Content Too Large" : status == 415 ? "Unsupported Media Type" : "Internal Server Error";
    char header[1024];
    int header_size = snprintf(header, sizeof(header), "HTTP/1.1 %d %s\r\nContent-Type: application/json; charset=utf-8\r\nContent-Length: %zu\r\n"
        "Cache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\n%s\r\n", status, reason, size, extra ? extra : "");
    if (header_size < 0 || (size_t)header_size >= sizeof(header) || size > SB_RESPONSE_LIMIT) return 500;
    char *frame = malloc((size_t)header_size + size);
    if (!frame) return 500;
    memcpy(frame, header, (size_t)header_size);
    if (body && size) memcpy(frame + header_size, body, size);
    mg_write(connection, frame, (size_t)header_size + size);
    free(frame);
    return status;
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

static int handle(struct mg_connection *connection) {
    const struct mg_request_info *info = mg_get_request_info(connection);
    http_state *state = info->user_data;
    static const char *const critical[] = {"Authorization", "Host", "Origin", "Content-Length", "Transfer-Encoding", "Content-Type", "Accept", "MCP-Protocol-Version", "Mcp-Method", "Mcp-Name"};
    for (size_t i = 0; i < sizeof(critical) / sizeof(critical[0]); ++i) {
        unsigned count = 0;
        for (int j = 0; j < info->num_headers; ++j) if (!_stricmp(info->http_headers[j].name, critical[i])) ++count;
        if (count > 1) return reject(connection, 400, "{\"error\":\"Duplicate security or protocol headers.\"}", NULL);
    }
    char host[64], localhost[64], origin[80], local_origin[80];
    snprintf(host, sizeof(host), "127.0.0.1:%u", state->host->config.port);
    snprintf(localhost, sizeof(localhost), "localhost:%u", state->host->config.port);
    snprintf(origin, sizeof(origin), "http://%s", host);
    snprintf(local_origin, sizeof(local_origin), "http://%s", localhost);
    const char *received_host = mg_get_header(connection, "Host");
    const char *received_origin = mg_get_header(connection, "Origin");
    /* An existing local reverse proxy may preserve its public Host. With no
       optional public_url constraint, accept valid authorities/origins without
       guessing a domain. Bearer remains mandatory, cookies and CORS are unused,
       and the listener remains IPv4 loopback only. */
    bool constrained = *state->host->config.public_origin != 0;
    bool host_valid = received_host && (!_stricmp(received_host, host) || !_stricmp(received_host, localhost) ||
        (constrained ? external_host(&state->host->config, received_host) : valid_proxy_authority(received_host)));
    bool origin_valid = !received_origin || !strcmp(received_origin, origin) || !strcmp(received_origin, local_origin) ||
        (constrained ? external_origin(&state->host->config, received_origin) : valid_proxy_origin(received_origin));
    if (!host_valid || !origin_valid)
        return reject(connection, 403, "{\"error\":\"Invalid Host or Origin.\"}", NULL);
    if (!authorized(connection, state))
        return reject(connection, 401, "{\"error\":\"Bearer token required.\"}", "WWW-Authenticate: Bearer realm=\"ShellBot local development\"\r\n");
    if (info->query_string) return reject(connection, 400, "{\"error\":\"Query parameters are not supported.\"}", NULL);
    if (info->local_uri && !strcmp(info->local_uri, "/healthz") && !strcmp(info->request_method, "GET")) {
        const char body[] = "{\"ready\":true,\"version\":\"" SB_VERSION "\"}";
        return reply(connection, 200, body, sizeof(body) - 1, NULL);
    }
    if (!info->local_uri || strcmp(info->local_uri, "/mcp")) return reject(connection, 404, "{\"error\":\"Not found.\"}", NULL);
    if (strcmp(info->request_method, "POST"))
        return reject(connection, 405, "{\"error\":\"SSE and protocol sessions are not supported in this build.\"}", "Allow: POST\r\n");
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
    char *response = sb_dispatch(state->host, NULL, request, length, &context, &response_length, &status);
    free(request);
    int result = reply(connection, status, response, response ? response_length : 0, NULL);
    free(response);
    return result;
}

static int quiet_log(const struct mg_connection *connection, const char *message) {
    (void)connection; (void)message;
    /* Library diagnostics may contain attacker-controlled URLs. */
    return 1;
}

int sb_http_run(sb_host *host, sb_error *error) {
    http_state state = {0};
    state.host = host;
    const char *token = getenv(host->config.token_env);
    size_t length = token ? strlen(token) : 0;
    if (length < 32 || length > 256) {
        sb_fail(error, "HTTP needs a 32..256 byte bearer token in the configured environment variable. Use --generate-token."); return 2;
    }
    for (size_t i = 0; i < length; ++i) if ((unsigned char)token[i] <= 32 || (unsigned char)token[i] >= 127) {
        sb_fail(error, "HTTP token must contain only visible ASCII without spaces."); return 2;
    }
    memcpy(state.token, token, length + 1); state.token_length = length;
    char listen[64];
    snprintf(listen, sizeof(listen), "127.0.0.1:%u", host->config.port);
    const char *options[] = {"listening_ports", listen, "num_threads", "4", "request_timeout_ms", "5000",
        "enable_keep_alive", "yes", "keep_alive_timeout_ms", "2000", "linger_timeout_ms", "5000",
        "tcp_nodelay", "1", "enable_directory_listing", "no", NULL};
    struct mg_callbacks callbacks = {0};
    callbacks.begin_request = handle; callbacks.log_message = quiet_log;
    InterlockedExchange(&stopping, 0);
    if (!SetConsoleCtrlHandler(stop_handler, TRUE)) {
        SecureZeroMemory(state.token, sizeof(state.token)); sb_fail(error, "Cannot install shutdown handler."); return 1;
    }
    mg_init_library(0);
    struct mg_context *context = mg_start(&callbacks, &state, options);
    if (!context) {
        SetConsoleCtrlHandler(stop_handler, FALSE); mg_exit_library();
        SecureZeroMemory(state.token, sizeof(state.token)); sb_fail(error, "Cannot start loopback HTTP listener; check port availability."); return 1;
    }
    fprintf(stderr, "ShellBot %s listening at http://%s/mcp (local development mode; writes require owner approval).\n", SB_VERSION, listen);
    while (!InterlockedCompareExchange(&stopping, 0, 0) && !sb_owner_stopping(host->owner)) Sleep(100);
    mg_stop(context); mg_exit_library();
    SetConsoleCtrlHandler(stop_handler, FALSE);
    SecureZeroMemory(state.token, sizeof(state.token));
    return 0;
}
