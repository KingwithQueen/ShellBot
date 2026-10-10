#include "sb.h"
#include <fcntl.h>
#include <io.h>
#include <stdio.h>
#include <stdlib.h>

int sb_stdio_run(sb_host *host) {
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
    char *line = malloc(SB_REQUEST_LIMIT + 1u);
    if (!line) return 1;
    sb_protocol protocol = {0};
    int result = 0;
    for (;;) {
        size_t size = 0;
        bool overflow = false;
        int c;
        while ((c = fgetc(stdin)) != EOF && c != '\n') {
            if (size < SB_REQUEST_LIMIT) line[size++] = (char)c;
            else overflow = true;
        }
        if (c == EOF && !size && !overflow) break;
        if (size && line[size - 1] == '\r') --size;
        if (!size && !overflow) continue;
        size_t response_length;
        int status;
        const sb_request_context context = {.actor = "stdio"};
        char *response = sb_dispatch(host, &protocol, line, overflow ? SB_REQUEST_LIMIT + 1u : size, &context, &response_length, &status);
        if (response) {
            bool written = fwrite(response, 1, response_length, stdout) == response_length && fputc('\n', stdout) != EOF && fflush(stdout) == 0;
            free(response);
            if (!written) { result = 1; break; }
        } else if (status != 202) { result = 1; break; }
        if (c == EOF) break;
    }
    if (ferror(stdin)) result = 1;
    free(line);
    return result;
}
