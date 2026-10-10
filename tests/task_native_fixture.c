/* Test-only child: performs fixed operations in an isolated test workspace. */
#define _CRT_SECURE_NO_WARNINGS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>
#include <string.h>
#include <fcntl.h>
#include <io.h>

static void json_string(const char *value) {
    const unsigned char *p = (const unsigned char *)value;
    putchar('"');
    while (*p) {
        if (*p == '"' || *p == '\\') { putchar('\\'); putchar(*p); }
        else if (*p < 32) { printf("\\u%04x", (unsigned int)*p); }
        else { putchar(*p); }
        ++p;
    }
    putchar('"');
}

static int identity(void) {
    HANDLE token = NULL;
    TOKEN_ELEVATION elevation;
    DWORD returned = 0, capacity = GetCurrentDirectoryW(0, NULL);
    wchar_t *directory = NULL;
    char *utf8 = NULL;
    int length, result = 2;
    if (!capacity || !OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return 2;
    if (!GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &returned)) goto done;
    directory = calloc(capacity, sizeof(*directory));
    if (!directory || !GetCurrentDirectoryW(capacity, directory)) goto done;
    length = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, directory, -1, NULL, 0, NULL, NULL);
    if (length <= 0) goto done;
    utf8 = malloc((size_t)length);
    if (!utf8 || !WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, directory, -1, utf8, length, NULL, NULL)) goto done;
    printf("{\"elevated\":%s,\"pid\":%lu,\"token_inherited\":%s,\"cwd\":",
           elevation.TokenIsElevated ? "true" : "false", GetCurrentProcessId(),
           GetEnvironmentVariableW(L"SHELLBOT_TOKEN", NULL, 0) ? "true" : "false");
    json_string(utf8);
    puts("}");
    result = ferror(stdout) ? 3 : 0;
done:
    free(utf8);
    free(directory);
    CloseHandle(token);
    return result;
}

int wmain(int argc, wchar_t **argv) {
    if (argc != 2) return 2;
    if (_setmode(_fileno(stdin), _O_BINARY) == -1 || _setmode(_fileno(stdout), _O_BINARY) == -1) return 2;
    if (!wcscmp(argv[1], L"identity")) return identity();
    if (!wcscmp(argv[1], L"append")) {
        HANDLE file = CreateFileW(L"once.txt", FILE_APPEND_DATA, FILE_SHARE_READ, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        DWORD written = 0;
        BOOL ok;
        if (file == INVALID_HANDLE_VALUE) return 2;
        ok = WriteFile(file, "ONE", 3, &written, NULL);
        CloseHandle(file);
        if (!ok || written != 3) return 3;
        return fwrite("done", 1, 4, stdout) == 4 ? 0 : 3;
    }
    if (!wcscmp(argv[1], L"stdin")) {
        char buffer[4096];
        size_t length;
        while ((length = fread(buffer, 1, sizeof(buffer), stdin)) != 0) {
            if (fwrite(buffer, 1, length, stdout) != length) return 3;
        }
        return ferror(stdin) || ferror(stdout) ? 3 : 0;
    }
    if (!wcscmp(argv[1], L"escaped")) {
        char bytes[16384];
        memset(bytes, 1, sizeof(bytes));
        return fwrite(bytes, 1, sizeof(bytes), stdout) == sizeof(bytes) ? 0 : 3;
    }
    if (!wcscmp(argv[1], L"wait")) { Sleep(60000); return 0; }
    return 2;
}
