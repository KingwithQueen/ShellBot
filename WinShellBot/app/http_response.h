#ifndef SHELLBOT_HTTP_RESPONSE_H
#define SHELLBOT_HTTP_RESPONSE_H

static bool close_requested(struct mg_connection *connection) {
    const struct mg_request_info *info=mg_get_request_info(connection);
    const char *value=mg_get_header(connection,"Connection");
    if (!info->http_version || strcmp(info->http_version,"1.1")) return true;
    while (value && *value) {
        while (*value==' ' || *value=='\t' || *value==',') ++value;
        const char *start=value; while (*value && *value!=',') ++value;
        const char *end=value; while (end>start && (end[-1]==' ' || end[-1]=='\t')) --end;
        if (end-start==5 && !_strnicmp(start,"close",5)) return true;
    }
    return false;
}

static int reply_typed(struct mg_connection *connection, int status, const char *body, size_t size, const char *extra, const char *type) {
    /* Drain only a reusable request here. An early rejection must reach the
       peer before waiting for a body it may never send; socket close performs
       its own bounded drain after publishing FIN. */
    const struct mg_request_info *info = mg_get_request_info(connection);
    bool closing=status>=400 || close_requested(connection);
    if (closing) mg_disable_keep_alive(connection);
    if (!closing && info->content_length >= 0 && info->content_length <= 2 * SB_REQUEST_LIMIT &&
        !mg_get_header(connection, "Transfer-Encoding")) {
        char discard[4096];
        size_t drained = 0;
        uint64_t deadline = sb_platform_millis() + 5000;
        int got;
        while (drained < 2 * SB_REQUEST_LIMIT && sb_platform_millis() < deadline &&
            (got = mg_read(connection, discard, sizeof(discard))) > 0) drained += (size_t)got;
    }
    const char *reason = status == 200 ? "OK" : status == 201 ? "Created" : status == 202 ? "Accepted" : status == 400 ? "Bad Request" :
        status == 401 ? "Unauthorized" : status == 403 ? "Forbidden" : status == 404 ? "Not Found" :
        status == 405 ? "Method Not Allowed" : status == 406 ? "Not Acceptable" :
        status == 411 ? "Length Required" : status == 413 ? "Content Too Large" : status == 415 ? "Unsupported Media Type" :
        status == 429 ? "Too Many Requests" : status == 503 ? "Service Unavailable" : "Internal Server Error";
    char header[1024];
    int header_size = snprintf(header, sizeof(header), "HTTP/1.1 %d %s\r\nContent-Type: %s; charset=utf-8\r\nContent-Length: %zu\r\n"
        "Cache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\n%s%s\r\n", status, reason, type, size,
        closing ? "Connection: close\r\n" : "",extra ? extra : "");
    if (header_size < 0 || (size_t)header_size >= sizeof(header) || size > SB_RESPONSE_LIMIT || (!body && size)) { mg_disable_keep_alive(connection); return 500; }
    char *frame = malloc((size_t)header_size + size);
    if (!frame) { mg_disable_keep_alive(connection); return 500; }
    memcpy(frame, header, (size_t)header_size);
    if (body && size) memcpy(frame + header_size, body, size);
    size_t sent=0,total=(size_t)header_size+size;
    while (sent<total) {
        int wrote=mg_write(connection,frame+sent,total-sent);
        if (wrote<=0 || (size_t)wrote>total-sent) break;
        sent+=(size_t)wrote;
    }
    free(frame);
    if (sent!=total) { mg_disable_keep_alive(connection); return 500; }
    return status;
}
static int reply(struct mg_connection *connection, int status, const char *body, size_t size, const char *extra) {
    return reply_typed(connection,status,body,size,extra,"application/json");
}

static int reject(struct mg_connection *connection, int status, const char *message, const char *extra) {
    return reply(connection, status, message, strlen(message), extra);
}


#endif
