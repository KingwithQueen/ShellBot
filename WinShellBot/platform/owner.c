#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <sddl.h>
#include "sb.h"
#include "execution.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

struct sb_owner { HANDLE pipe, stop, thread; void *user; sb_policy *policy; bool http; };

static void *token_user(HANDLE token) {
    DWORD size = 0;
    GetTokenInformation(token, TokenUser, NULL, 0, &size);
    void *user = size ? malloc(size) : NULL;
    if (user && !GetTokenInformation(token, TokenUser, user, size, &size)) { free(user); user = NULL; }
    return user;
}
static void *current_user(void) {
    HANDLE token = NULL;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return NULL;
    void *user = token_user(token); CloseHandle(token); return user;
}
static bool pipe_name(const char *data_dir, wchar_t name[128], sb_error *error) {
    int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, data_dir, -1, NULL, 0);
    wchar_t *path = size > 0 ? calloc((size_t)size, sizeof(*path)) : NULL;
    if (!path) { sb_fail(error, "Cannot allocate owner channel path."); return false; }
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, data_dir, -1, path, size);
    HANDLE directory = CreateFileW(path, FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE,
        NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
    free(path);
    wchar_t canonical[SB_PATH_CAP];
    DWORD length = directory != INVALID_HANDLE_VALUE ? GetFinalPathNameByHandleW(directory, canonical,
        SB_PATH_CAP, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS) : 0;
    if (directory != INVALID_HANDLE_VALUE) CloseHandle(directory);
    if (!length || length >= SB_PATH_CAP) { sb_fail(error, "Owner commands need an existing data directory of a running host."); return false; }
    while (length && canonical[length - 1] == L'\\') canonical[--length] = 0;
    char hash[65];
    if (!sb_platform_sha256(canonical, length * sizeof(wchar_t), hash, error)) return false;
    swprintf(name, 128, L"\\\\.\\pipe\\ShellBot-owner-%hs", hash);
    return true;
}

/* Every pending IO waits for both completion and stop, with a bounded timeout. */
static bool finish(HANDLE handle, OVERLAPPED *overlapped, HANDLE stop, DWORD timeout, DWORD *transferred) {
    HANDLE events[2] = {overlapped->hEvent, stop};
    DWORD waited = WaitForMultipleObjects(stop ? 2 : 1, events, FALSE, timeout);
    if (waited != WAIT_OBJECT_0) {
        CancelIoEx(handle, overlapped);
        GetOverlappedResult(handle, overlapped, transferred, TRUE);
        return false;
    }
    return GetOverlappedResult(handle, overlapped, transferred, FALSE) != FALSE;
}
static bool transfer(HANDLE pipe, HANDLE stop, void *buffer, DWORD length, bool write) {
    DWORD offset = 0;
    uint64_t deadline = sb_platform_millis() + 5000;
    while (offset < length) {
        uint64_t now = sb_platform_millis();
        if (now >= deadline || (stop && WaitForSingleObject(stop, 0) == WAIT_OBJECT_0)) return false;
        OVERLAPPED overlapped = {0}; overlapped.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
        if (!overlapped.hEvent) return false;
        DWORD got = 0;
        BOOL ok = write ? WriteFile(pipe, (char *)buffer + offset, length - offset, &got, &overlapped) :
            ReadFile(pipe, (char *)buffer + offset, length - offset, &got, &overlapped);
        if (!ok && GetLastError() == ERROR_IO_PENDING) ok = finish(pipe, &overlapped, stop, (DWORD)(deadline - now), &got);
        CloseHandle(overlapped.hEvent);
        if (!ok || !got) return false;
        offset += got;
    }
    return true;
}
static bool owner_peer(sb_owner *owner) {
    if (!ImpersonateNamedPipeClient(owner->pipe)) return false;
    HANDLE token = NULL; void *user = NULL;
    if (OpenThreadToken(GetCurrentThread(), TOKEN_QUERY, TRUE, &token)) {
        if (sb_win_owner_token_allowed(token)) user = token_user(token);
        CloseHandle(token);
    }
    bool same = user && EqualSid(((TOKEN_USER *)user)->User.Sid, ((TOKEN_USER *)owner->user)->User.Sid);
    free(user);
    if (!RevertToSelf()) TerminateProcess(GetCurrentProcess(), 3); /* Never execute while impersonating a caller. */
    return same;
}
static DWORD WINAPI serve(void *parameter) {
    sb_owner *owner = parameter;
    while (WaitForSingleObject(owner->stop, 0) != WAIT_OBJECT_0) {
        OVERLAPPED overlapped = {0}; overlapped.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
        if (!overlapped.hEvent) break;
        BOOL connected = ConnectNamedPipe(owner->pipe, &overlapped);
        DWORD transferred = 0, error = connected ? ERROR_SUCCESS : GetLastError();
        if (!connected && error == ERROR_PIPE_CONNECTED) connected = TRUE;
        else if (!connected && error == ERROR_IO_PENDING) {
            HANDLE events[] = {overlapped.hEvent, owner->stop};
            DWORD waited = WaitForMultipleObjects(2, events, FALSE, INFINITE);
            if (waited == WAIT_OBJECT_0) connected = GetOverlappedResult(owner->pipe, &overlapped, &transferred, FALSE);
            else { CancelIoEx(owner->pipe, &overlapped); GetOverlappedResult(owner->pipe, &overlapped, &transferred, TRUE); }
        }
        CloseHandle(overlapped.hEvent);
        if (connected && WaitForSingleObject(owner->stop, 0) != WAIT_OBJECT_0) {
            uint32_t length = 0;
            if (transfer(owner->pipe, owner->stop, &length, sizeof(length), false) && length && length <= 4096) {
                char request[4096];
                if (transfer(owner->pipe, owner->stop, request, length, false) && owner_peer(owner)) {
                    yyjson_doc *input = yyjson_read(request, length, 0);
                    yyjson_val *root = yyjson_doc_get_root(input);
                    const char *command = sb_string(yyjson_obj_get(root, "command"));
                    bool stop_request = yyjson_is_obj(root) && yyjson_obj_size(root) == 1 && command && !strcmp(command, "stop");
                    char *response = stop_request ? sb_strdup(owner->http ? "{\"status\":\"stopping\"}" :
                        "{\"error\":\"For stdio, close the client input stream to stop.\"}") : sb_policy_owner_request(owner->policy, request, length);
                    yyjson_doc_free(input);
                    if (response && strlen(response) <= SB_RESPONSE_LIMIT) {
                        uint32_t size = (uint32_t)strlen(response);
                        if (transfer(owner->pipe, owner->stop, &size, sizeof(size), true))
                            transfer(owner->pipe, owner->stop, response, size, true);
                        /* Client acknowledgement ensures it received the frame before disconnect.
                           FlushFileBuffers would wait indefinitely on a stalled client. */
                        unsigned char ack;
                        transfer(owner->pipe, owner->stop, &ack, 1, false);
                        if (stop_request && owner->http) SetEvent(owner->stop);
                    }
                    free(response);
                }
            }
        }
        DisconnectNamedPipe(owner->pipe);
    }
    return 0;
}
sb_owner *sb_owner_start(sb_host *host, sb_error *error) {
    wchar_t name[128];
    if (!pipe_name(host->config.data_dir, name, error)) return NULL;
    sb_owner *owner = calloc(1, sizeof(*owner));
    if (!owner) { sb_fail(error, "Cannot allocate owner channel."); return NULL; }
    owner->pipe = INVALID_HANDLE_VALUE; owner->policy = host->policy; owner->user = current_user(); owner->http = host->http_mode;
    LPWSTR sid = NULL; PSECURITY_DESCRIPTOR descriptor = NULL;
    wchar_t sddl[256];
    bool secured = owner->user && ConvertSidToStringSidW(((TOKEN_USER *)owner->user)->User.Sid, &sid);
    if (secured) {
        swprintf(sddl, 256, L"D:P(A;;GA;;;SY)(A;;GA;;;%ls)", sid);
        secured = ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl, SDDL_REVISION_1, &descriptor, NULL) != FALSE;
    }
    if (sid) LocalFree(sid);
    if (secured) {
        SECURITY_ATTRIBUTES attributes = {sizeof(attributes), descriptor, FALSE};
        owner->pipe = CreateNamedPipeW(name, PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, 4096, 4096, 0, &attributes);
    }
    if (descriptor) LocalFree(descriptor);
    owner->stop = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (owner->pipe != INVALID_HANDLE_VALUE && owner->stop) owner->thread = CreateThread(NULL, 0, serve, owner, 0, NULL);
    if (!owner->thread) { sb_owner_stop(owner); sb_fail(error, "Cannot start the protected local owner channel."); return NULL; }
    return owner;
}
void sb_owner_stop(sb_owner *owner) {
    if (!owner) return;
    if (owner->stop) SetEvent(owner->stop);
    if (owner->thread) { WaitForSingleObject(owner->thread, INFINITE); CloseHandle(owner->thread); }
    if (owner->pipe != INVALID_HANDLE_VALUE) CloseHandle(owner->pipe);
    if (owner->stop) CloseHandle(owner->stop);
    free(owner->user); free(owner);
}
bool sb_owner_stopping(sb_owner *owner) { return owner && WaitForSingleObject(owner->stop, 0) == WAIT_OBJECT_0; }
int sb_owner_command(const char *data_dir, const char *command, const char *id, sb_error *error) {
    wchar_t name[128];
    if (!pipe_name(data_dir, name, error)) return 2;
    if (!WaitNamedPipeW(name, 3000)) { sb_fail(error, "No available owner channel. Start ShellBot with the same data directory and Windows account."); return 2; }
    HANDLE pipe = CreateFileW(name, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING,
        FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, NULL);
    if (pipe == INVALID_HANDLE_VALUE) { sb_fail(error, "Cannot connect to owner channel."); return 2; }
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    if (!doc) { CloseHandle(pipe); sb_fail(error, "Out of memory."); return 2; }
    yyjson_mut_val *root = yyjson_mut_obj(doc); yyjson_mut_doc_set_root(doc, root);
    yyjson_mut_obj_add_str(doc, root, "command", command);
    if (id) yyjson_mut_obj_add_str(doc, root, "id", id);
    char *request = yyjson_mut_write(doc, 0, NULL); yyjson_mut_doc_free(doc);
    uint32_t size = request ? (uint32_t)strlen(request) : 0;
    bool ok = size && size <= 4096 && transfer(pipe, NULL, &size, sizeof(size), true) && transfer(pipe, NULL, request, size, true);
    free(request);
    uint32_t length = 0;
    ok = ok && transfer(pipe, NULL, &length, sizeof(length), false) && length && length <= SB_RESPONSE_LIMIT;
    char *response = ok ? malloc((size_t)length + 1) : NULL;
    ok = response && transfer(pipe, NULL, response, length, false);
    unsigned char ack = 1;
    if (ok) transfer(pipe, NULL, &ack, 1, true);
    CloseHandle(pipe);
    int result = 2;
    if (ok) {
        response[length] = 0;
        yyjson_doc *reply = yyjson_read(response, length, 0);
        if (reply) { puts(response); result = yyjson_obj_get(yyjson_doc_get_root(reply), "error") ? 2 : 0; }
        yyjson_doc_free(reply);
    }
    free(response);
    if (!ok) sb_fail(error, "Owner channel request failed or timed out.");
    return result;
}
