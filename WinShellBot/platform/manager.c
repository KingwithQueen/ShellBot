#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <sddl.h>
#include "sb_projects.h"
#include "execution.h"
#include "manager.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

/* No HTTP or elevation in M2. All owner calls are authenticated locally;
   a worker receives exactly one immutable project over its inherited handle. */
#define WORKER_IO_MS 8000
typedef struct {
    CRITICAL_SECTION lock;
    const sb_project *project;
    char *snapshot;
    HANDLE pipe, process, job;
    DWORD pid, exit_code;
    char instance[33];
    char data_identity[50];
    const char *state;
    sb_root *data_root;
} worker;
typedef struct {
    sb_project_registry registry;
    worker workers[SB_MAX_PROJECTS];
    size_t initialized;
    sb_lease *lease;
    sb_root *data_root;
    sb_audit *audit;
    sb_owner *owner;
    HANDLE stop;
} manager;

static wchar_t *wide(const char *s) {
    int n = s ? MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s, -1, NULL, 0) : 0;
    wchar_t *result = n ? calloc((size_t)n, sizeof(wchar_t)) : NULL;
    if (result && !MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s, -1, result, n)) { free(result); result = NULL; }
    return result;
}
static bool ordinary(void) {
    HANDLE token = NULL; TOKEN_ELEVATION elevation = {0}; DWORD n;
    bool ok = OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token) && sb_win_owner_token_allowed(token) &&
        GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &n) && !elevation.TokenIsElevated;
    if (token) CloseHandle(token);
    return ok;
}
static bool same_executable(HANDLE process) {
    wchar_t mine[SB_PATH_CAP], theirs[SB_PATH_CAP]; DWORD size = SB_PATH_CAP;
    return GetModuleFileNameW(NULL, mine, SB_PATH_CAP) && QueryFullProcessImageNameW(process, 0, theirs, &size) && !_wcsicmp(mine, theirs);
}
static PSECURITY_DESCRIPTOR security(void) {
    HANDLE token = NULL; DWORD size = 0; void *user = NULL; LPWSTR sid = NULL;
    PSECURITY_DESCRIPTOR descriptor = NULL;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return NULL;
    GetTokenInformation(token, TokenUser, NULL, 0, &size);
    user = size ? malloc(size) : NULL;
    if (user && GetTokenInformation(token, TokenUser, user, size, &size) && ConvertSidToStringSidW(((TOKEN_USER *)user)->User.Sid, &sid)) {
        wchar_t sddl[256]; swprintf(sddl, 256, L"D:P(A;;GA;;;SY)(A;;GA;;;%ls)", sid);
        ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl, SDDL_REVISION_1, &descriptor, NULL);
    }
    if (sid) LocalFree(sid);
    free(user); CloseHandle(token); return descriptor;
}
static bool io(HANDLE pipe, void *buffer, DWORD size, bool writing, bool asynchronous, uint64_t deadline) {
    DWORD offset = 0;
    while (offset < size) {
        DWORD got = 0; OVERLAPPED ov = {0};
        if (asynchronous) {
            if (sb_platform_millis() >= deadline) return false;
            ov.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
            if (!ov.hEvent) return false;
        }
        BOOL ok = writing ? WriteFile(pipe, (char *)buffer + offset, size - offset, &got, asynchronous ? &ov : NULL) :
            ReadFile(pipe, (char *)buffer + offset, size - offset, &got, asynchronous ? &ov : NULL);
        if (!ok && asynchronous && GetLastError() == ERROR_IO_PENDING) {
            uint64_t now = sb_platform_millis();
            DWORD remaining = now < deadline ? (DWORD)(deadline - now) : 0;
            if (WaitForSingleObject(ov.hEvent, remaining) == WAIT_OBJECT_0) ok = GetOverlappedResult(pipe, &ov, &got, FALSE);
            else { CancelIoEx(pipe, &ov); GetOverlappedResult(pipe, &ov, &got, TRUE); ok = FALSE; }
        }
        if (ov.hEvent) CloseHandle(ov.hEvent);
        if (!ok || !got) return false;
        offset += got;
    }
    return true;
}
static bool send_frame(HANDLE pipe, const char *json, bool async, uint64_t deadline) {
    size_t length = json ? strlen(json) : 0;
    if (!length || length > SB_RESPONSE_LIMIT) return false;
    uint32_t size = (uint32_t)length;
    return io(pipe, &size, sizeof(size), true, async, deadline) && io(pipe, (void *)json, size, true, async, deadline);
}
static char *receive_frame(HANDLE pipe, bool async, uint64_t deadline, unsigned maximum) {
    uint32_t length = 0;
    if (!io(pipe, &length, sizeof(length), false, async, deadline) || !length || length > maximum) return NULL;
    char *result = calloc((size_t)length + 1, 1);
    if (result && !io(pipe, result, length, false, async, deadline)) { free(result); result = NULL; }
    return result;
}
static char *error_json(const char *message) {
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL); if (!doc) return NULL;
    yyjson_mut_val *root = yyjson_mut_obj(doc); yyjson_mut_doc_set_root(doc, root);
    yyjson_mut_obj_add_strcpy(doc, root, "error", message);
    char *json = yyjson_mut_write(doc, 0, NULL); yyjson_mut_doc_free(doc); return json;
}
static void reap(worker *w, const char *state) {
    /* Every handle here came from our own CreateProcess/Job allocation. */
    if (w->job) TerminateJobObject(w->job, 125);
    if (w->process) {
        WaitForSingleObject(w->process, 3000);
        GetExitCodeProcess(w->process, &w->exit_code); CloseHandle(w->process);
    }
    if (w->pipe) CloseHandle(w->pipe);
    if (w->job) CloseHandle(w->job);
    w->process = w->pipe = w->job = NULL; w->state = state;
}
static void refresh(manager *m, worker *w) {
    if (w->process && WaitForSingleObject(w->process, 0) != WAIT_TIMEOUT) {
        reap(w, "failed");
        sb_audit_project(m->audit, "project_failed", w->project->id, w->project->policy_revision, w->pid);
    }
}
static wchar_t *environment(const sb_project_registry *registry) {
    static const wchar_t *names[] = {L"ALLUSERSPROFILE",L"APPDATA",L"COMPUTERNAME",L"ComSpec",L"HOMEDRIVE",L"HOMEPATH",L"LOCALAPPDATA",L"PATH",L"PATHEXT",L"ProgramData",L"ProgramFiles",L"ProgramFiles(x86)",L"SystemDrive",L"SystemRoot",L"TEMP",L"TMP",L"USERDOMAIN",L"USERNAME",L"USERPROFILE",L"windir"};
    wchar_t *block = calloc(32768, sizeof(wchar_t)); size_t used = 0;
    if (!block) return NULL;
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
        bool excluded = false;
        for (size_t j = 0; j < registry->project_count; ++j) {
            wchar_t *key = wide(registry->projects[j].token_env);
            if (!key) { free(block); return NULL; }
            if (!_wcsicmp(key, names[i])) excluded = true;
            free(key);
        }
        if (excluded) continue;
        DWORD n = GetEnvironmentVariableW(names[i], NULL, 0); if (!n) continue;
        size_t key_length = wcslen(names[i]);
        if (n + key_length + 2 >= 32768 - used) { free(block); return NULL; }
        memcpy(block + used, names[i], key_length * sizeof(wchar_t)); used += key_length; block[used++] = L'=';
        DWORD got = GetEnvironmentVariableW(names[i], block + used, n);
        if (!got || got >= n) { free(block); return NULL; }
        used += got + 1;
    }
    return block;
}
static bool start_worker(manager *m, worker *w, sb_error *error) {
    refresh(m, w);
    if (w->process) return true;
    if (!w->project->enabled) { sb_fail(error, "Project is disabled."); return false; }
    if (!sb_audit_project(m->audit, "project_start_intent", w->project->id, w->project->policy_revision, w->pid)) {
        sb_fail(error, "Manager audit unavailable; project startup blocked."); return false;
    }
    HANDLE child = INVALID_HANDLE_VALUE;
    PSECURITY_DESCRIPTOR descriptor = security();
    unsigned char random[16]; char hex[33]; wchar_t name[128], exe[SB_PATH_CAP], line[SB_PATH_CAP];
    bool ok = descriptor && sb_platform_random(random, sizeof(random), error) && GetModuleFileNameW(NULL, exe, SB_PATH_CAP);
    for (size_t i = 0; ok && i < sizeof(random); ++i) snprintf(hex + i * 2, 3, "%02x", random[i]);
    if (ok) {
        swprintf(name, 128, L"\\\\.\\pipe\\ShellBot-worker-%lu-%hs", GetCurrentProcessId(), hex);
        SECURITY_ATTRIBUTES sa = {sizeof(sa), descriptor, FALSE};
        w->pipe = CreateNamedPipeW(name, PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, 65536, 65536, 0, &sa);
        if (w->pipe == INVALID_HANDLE_VALUE) w->pipe = NULL;
        child = w->pipe ? CreateFileW(name, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING,
            SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, NULL) : INVALID_HANDLE_VALUE;
        ok = w->pipe && child != INVALID_HANDLE_VALUE && SetHandleInformation(child, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
    }
    if (descriptor) LocalFree(descriptor);
    SIZE_T attr_size = 0; STARTUPINFOEXW startup = {0}; PROCESS_INFORMATION process = {0};
    startup.StartupInfo.cb = sizeof(startup);
    InitializeProcThreadAttributeList(NULL, 1, 0, &attr_size);
    startup.lpAttributeList = attr_size ? malloc(attr_size) : NULL;
    bool initialized = startup.lpAttributeList && InitializeProcThreadAttributeList(startup.lpAttributeList, 1, 0, &attr_size);
    wchar_t *env = ok ? environment(&m->registry) : NULL;
    if (ok && initialized && env) {
        ok = UpdateProcThreadAttribute(startup.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, &child, sizeof(child), NULL, NULL) != FALSE;
        swprintf(line, SB_PATH_CAP, L"\"%ls\" --project-worker %llu %lu", exe, (unsigned long long)(uintptr_t)child, GetCurrentProcessId());
        w->job = CreateJobObjectW(NULL, NULL);
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits = {0};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_ACTIVE_PROCESS | JOB_OBJECT_LIMIT_JOB_MEMORY;
        limits.BasicLimitInformation.ActiveProcessLimit = 1024;
        limits.JobMemoryLimit = (SIZE_T)w->project->limits.worker_memory_mb * 1024 * 1024;
        ok = ok && w->job && SetInformationJobObject(w->job, JobObjectExtendedLimitInformation, &limits, sizeof(limits)) &&
            CreateProcessW(exe, line, NULL, NULL, TRUE, CREATE_SUSPENDED | CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT | EXTENDED_STARTUPINFO_PRESENT,
                env, NULL, &startup.StartupInfo, &process);
        if (ok) {
            w->process = process.hProcess; w->pid = process.dwProcessId;
            ok = AssignProcessToJobObject(w->job, w->process) && ResumeThread(process.hThread) != (DWORD)-1;
            if (!ok) TerminateProcess(w->process, 125); /* Own suspended child, not a searched PID. */
        }
    } else ok = false;
    if (process.hThread) CloseHandle(process.hThread);
    if (child != INVALID_HANDLE_VALUE) CloseHandle(child);
    if (initialized) DeleteProcThreadAttributeList(startup.lpAttributeList);
    free(startup.lpAttributeList); free(env);
    uint64_t deadline = sb_platform_millis() + WORKER_IO_MS;
    char *greeting = ok && send_frame(w->pipe, w->snapshot, true, deadline) && send_frame(w->pipe, w->data_identity, true, deadline) ?
        receive_frame(w->pipe, true, deadline, 4096) : NULL;
    yyjson_doc *reply = greeting ? yyjson_read(greeting, strlen(greeting), 0) : NULL;
    yyjson_val *root = yyjson_doc_get_root(reply);
    const char *id = sb_string(yyjson_obj_get(root, "project_id")), *instance = sb_string(yyjson_obj_get(root, "host_instance_id"));
    uint64_t revision = 0, pid = 0;
    ok = reply && yyjson_is_true(yyjson_obj_get(root, "ready")) && id && !strcmp(id, w->project->id) &&
        sb_integer(yyjson_obj_get(root, "policy_revision"), &revision) && revision == w->project->policy_revision &&
        sb_integer(yyjson_obj_get(root, "pid"), &pid) && pid == w->pid && instance && strlen(instance) == 32 &&
        strspn(instance, "0123456789abcdef") == 32;
    if (ok) strcpy(w->instance, instance);
    yyjson_doc_free(reply); free(greeting);
    if (!ok) { reap(w, "failed"); sb_fail(error, "Worker startup failed; no elevation or automatic restart attempted."); return false; }
    w->state = "running"; w->exit_code = STILL_ACTIVE;
    sb_audit_project(m->audit, "project_started", w->project->id, w->project->policy_revision, w->pid);
    return true;
}
static yyjson_mut_val *status(manager *m, worker *w, yyjson_mut_doc *doc) {
    refresh(m, w);
    yyjson_mut_val *value = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_str(doc, value, "project_id", w->project->id);
    yyjson_mut_obj_add_uint(doc, value, "policy_revision", w->project->policy_revision);
    yyjson_mut_obj_add_str(doc, value, "state", w->state);
    yyjson_mut_obj_add_uint(doc, value, "worker_pid", w->pid);
    yyjson_mut_obj_add_uint(doc, value, "exit_code", w->exit_code);
    yyjson_mut_obj_add_strcpy(doc, value, "host_instance_id", w->instance);
    yyjson_mut_obj_add_bool(doc, value, "enabled", w->project->enabled);
    yyjson_mut_obj_add_bool(doc, value, "automatic_restart", false);
    yyjson_mut_obj_add_bool(doc, value, "administrator_backend", false);
    return value;
}
static char *manager_request(void *context, const char *request, size_t length) {
    manager *m = context;
    yyjson_doc *input = yyjson_read(request, length, 0);
    yyjson_val *root = yyjson_doc_get_root(input);
    static const char *const keys[] = {"command", "id", "request"};
    const char *command = sb_string(yyjson_obj_get(root, "command")), *id = sb_string(yyjson_obj_get(root, "id"));
    yyjson_val *payload = yyjson_obj_get(root, "request");
    char *json = NULL;
    if (!sb_config_keys(root, keys, 3) || !command || (yyjson_obj_get(root, "id") && !sb_identifier(id)) ||
        (payload && !yyjson_is_obj(payload))) { json = error_json("Invalid manager request."); goto done; }
    if (!strcmp(command, "manager-stop") && !id && !payload) {
        json = sb_strdup("{\"status\":\"stopping\"}"); goto done;
    }
    if (WaitForSingleObject(m->stop, 0) == WAIT_OBJECT_0) { json = error_json("Manager is stopping."); goto done; }
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    if (!doc) goto done;
    yyjson_mut_val *result = yyjson_mut_obj(doc); yyjson_mut_doc_set_root(doc, result);
    if (!strcmp(command, "project-status") && !id && !payload) {
        yyjson_mut_val *array = yyjson_mut_arr(doc);
        for (size_t i = 0; i < m->registry.project_count; ++i) {
            worker *w = &m->workers[i];
            if (TryEnterCriticalSection(&w->lock)) { yyjson_mut_arr_add_val(array, status(m, w, doc)); LeaveCriticalSection(&w->lock); }
            else {
                yyjson_mut_val *busy = yyjson_mut_obj(doc); yyjson_mut_obj_add_str(doc, busy, "project_id", w->project->id);
                yyjson_mut_obj_add_str(doc, busy, "state", "busy"); yyjson_mut_arr_add_val(array, busy);
            }
        }
        yyjson_mut_obj_add_val(doc, result, "projects", array);
        yyjson_mut_obj_add_uint(doc, result, "manager_pid", GetCurrentProcessId());
        yyjson_mut_obj_add_bool(doc, result, "elevated", false);
        yyjson_mut_obj_add_uint(doc, result, "owner_workers", 4);
        yyjson_mut_obj_add_bool(doc, result, "http_gateway", false);
    } else {
        worker *w = NULL;
        for (size_t i = 0; id && i < m->registry.project_count; ++i) if (!strcmp(id, m->workers[i].project->id)) w = &m->workers[i];
        const char *failure = NULL;
        if (!w) failure = "Unknown project id.";
        else if (!TryEnterCriticalSection(&w->lock)) { failure = "Project has an in-flight request; retry management after it completes."; w = NULL; }
        else {
            sb_error error = {0}; refresh(m, w);
            if (!strcmp(command, "project-start") && !payload) {
                if (!start_worker(m, w, &error)) failure = error.text;
                else yyjson_mut_obj_add_val(doc, result, "project", status(m, w, doc));
            } else if (!strcmp(command, "project-stop") && !payload) {
                reap(w, "stopped");
                sb_audit_project(m->audit, "project_stopped", w->project->id, w->project->policy_revision, w->pid);
                yyjson_mut_obj_add_val(doc, result, "project", status(m, w, doc));
            } else if (!strcmp(command, "project-status") && !payload) yyjson_mut_obj_add_val(doc, result, "project", status(m, w, doc));
            else if ((!strcmp(command, "project-request") || !strcmp(command, "project-owner")) && payload) {
                if (!w->process) failure = "Project is not running; explicit owner start is required.";
                else {
                    yyjson_mut_val *envelope = yyjson_mut_obj(doc);
                    yyjson_mut_obj_add_str(doc, envelope, "kind", !strcmp(command, "project-owner") ? "owner" : "mcp");
                    yyjson_mut_obj_add_val(doc, envelope, "request", yyjson_val_mut_copy(doc, payload));
                    char *wire = yyjson_mut_val_write(envelope, 0, NULL);
                    uint64_t deadline = sb_platform_millis() + WORKER_IO_MS;
                    char *response = wire && send_frame(w->pipe, wire, true, deadline) ? receive_frame(w->pipe, true, deadline, SB_RESPONSE_LIMIT - 1024) : NULL;
                    free(wire);
                    yyjson_doc *reply = response ? yyjson_read(response, strlen(response), 0) : NULL;
                    if (reply) {
                        yyjson_mut_obj_add_str(doc, result, "project_id", w->project->id);
                        yyjson_mut_obj_add_val(doc, result, "response", yyjson_val_mut_copy(doc, yyjson_doc_get_root(reply)));
                    } else {
                        reap(w, "failed"); failure = "Worker disconnected or exceeded its deadline; review possible side effects before any new request. No automatic replay.";
                        sb_audit_project(m->audit, "project_failed", w->project->id, w->project->policy_revision, w->pid);
                    }
                    yyjson_doc_free(reply); free(response);
                }
            } else failure = "Unknown manager command or invalid arguments.";
            if (failure) yyjson_mut_obj_add_strcpy(doc, result, "error", failure);
            LeaveCriticalSection(&w->lock);
        }
        if (!w && failure) yyjson_mut_obj_add_strcpy(doc, result, "error", failure);
    }
    json = yyjson_mut_write(doc, 0, NULL); yyjson_mut_doc_free(doc);
done:
    yyjson_doc_free(input); return json;
}
static bool preflight(manager *m, const char *data_dir, const char *bytes, size_t length, sb_error *error) {
    if (!ordinary()) { sb_fail(error, "Manager requires a normal, unelevated owner token; it never requests UAC."); return false; }
    if (!sb_project_path_valid(data_dir) || !sb_projects_parse(&m->registry, bytes, length, error)) return false;
    m->lease = sb_platform_lease(data_dir, error); if (!m->lease) return false;
    m->data_root = sb_platform_root(data_dir, error); if (!m->data_root) return false;
    m->audit = sb_audit_new(m->lease, SB_AUDIT_LIMIT, error); if (!m->audit) return false;
    yyjson_doc *source = yyjson_read(bytes, length, 0);
    yyjson_val *array = yyjson_obj_get(yyjson_doc_get_root(source), "projects");
    bool ok = false;
    for (size_t i = 0; i < m->registry.project_count; ++i) {
        worker *w = &m->workers[i]; w->project = &m->registry.projects[i]; w->state = "stopped";
        InitializeCriticalSection(&w->lock); ++m->initialized;
        /* Physical Data comparison includes junction/UNC aliases; roots stay pinned. */
        sb_lease *probe = sb_platform_lease(w->project->data_dir, error);
        if (!probe) goto done;
        w->data_root = sb_platform_root(w->project->data_dir, error); sb_platform_lease_close(probe);
        if (!w->data_root) goto done;
        if (!sb_platform_root_identity(w->data_root, w->data_identity, error)) goto done;
        if (sb_platform_roots_overlap(m->data_root, w->data_root)) goto overlap;
        for (size_t j = 0; j < i; ++j) if (sb_platform_roots_overlap(m->workers[j].data_root, w->data_root)) goto overlap;
        yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL); if (!doc) goto done;
        yyjson_mut_val *root = yyjson_mut_obj(doc), *projects = yyjson_mut_arr(doc); yyjson_mut_doc_set_root(doc, root);
        yyjson_mut_obj_add_uint(doc, root, "format_version", SB_PROJECT_FORMAT);
        yyjson_mut_arr_add_val(projects, yyjson_val_mut_copy(doc, yyjson_arr_get(array, i)));
        yyjson_mut_obj_add_val(doc, root, "projects", projects);
        w->snapshot = yyjson_mut_write(doc, 0, NULL); yyjson_mut_doc_free(doc);
        if (!w->snapshot) goto done;
    }
    /* Configured-directory projects must not expose manager/project state,
       including aliases. Account-trust projects retain their declared scope. */
    for (size_t i = 0; i < m->registry.project_count; ++i) {
        const sb_project *project = &m->registry.projects[i];
        if (project->policy.file_scope != SB_FILES_CONFIGURED) continue;
        for (size_t j = 0; j < project->directory_count; ++j) {
            sb_root *directory = sb_platform_root(project->directories[j].path, error);
            if (!directory) goto done;
            bool conflict = sb_platform_roots_overlap(directory, m->data_root);
            for (size_t k = 0; !conflict && k < m->registry.project_count; ++k)
                conflict = sb_platform_roots_overlap(directory, m->workers[k].data_root);
            sb_platform_root_close(directory);
            if (conflict) { sb_fail(error, "Configured project directories overlap protected manager/project Data."); goto done; }
        }
    }
    ok = true; goto done;
overlap:
    sb_fail(error, "Physical manager/project Data directories overlap; no workers started.");
done:
    yyjson_doc_free(source); return ok;
}
static int run_manager(const char *source, const char *data) {
    sb_error error = {0}; size_t length = 0; char *bytes = sb_platform_load_config(source, &length, &error);
    manager *m = calloc(1, sizeof(*m)); int code = 2;
    if (!m || !bytes || !preflight(m, data, bytes, length, &error)) goto done;
    m->stop = CreateEventW(NULL, TRUE, FALSE, NULL); if (!m->stop) goto done;
    m->owner = sb_owner_start_handler(data, manager_request, m, &error); if (!m->owner) goto done;
    puts("{\"manager_ready\":true,\"elevated\":false,\"http_gateway\":false}"); fflush(stdout);
    /* Explicit startup per project. The manager never replays failed work. */
    while (WaitForSingleObject(m->stop, 100) == WAIT_TIMEOUT && !sb_owner_stopping(m->owner)) {
        for (size_t i = 0; i < m->registry.project_count; ++i) {
            worker *w = &m->workers[i];
            if (TryEnterCriticalSection(&w->lock)) { refresh(m, w); LeaveCriticalSection(&w->lock); }
        }
    }
    SetEvent(m->stop); code = 0;
done:
    free(bytes);
    if (m) {
        sb_owner_stop(m->owner);
        for (size_t i = 0; i < m->initialized; ++i) {
            if (m->workers[i].process)
                sb_audit_project(m->audit, "project_stopped", m->workers[i].project->id, m->workers[i].project->policy_revision, m->workers[i].pid);
            reap(&m->workers[i], "stopped"); free(m->workers[i].snapshot);
            sb_platform_root_close(m->workers[i].data_root); DeleteCriticalSection(&m->workers[i].lock);
        }
        if (m->stop) CloseHandle(m->stop);
        sb_audit_free(m->audit); sb_platform_root_close(m->data_root); sb_platform_lease_close(m->lease);
        sb_projects_free(&m->registry); free(m);
    }
    if (code) fprintf(stderr, "ShellBot: %s\n", *error.text ? error.text : "Manager startup failed.");
    return code;
}

int sb_project_worker(int argc, wchar_t **argv) {
    if (argc != 4 || !ordinary()) return 2;
    wchar_t *end = NULL; unsigned long long value = _wcstoui64(argv[2], &end, 10);
    if (!value || !end || *end) return 2;
    HANDLE pipe = (HANDLE)(uintptr_t)value;
    unsigned long expected = wcstoul(argv[3], &end, 10); ULONG server = 0;
    if (!expected || !end || *end || !GetNamedPipeServerProcessId(pipe, &server) || server != expected) return 2;
    HANDLE parent = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, server);
    if (!parent || WaitForSingleObject(parent, 0) != WAIT_TIMEOUT || !same_executable(parent)) { if (parent) CloseHandle(parent); return 2; }
    CloseHandle(parent);
    if (!SetHandleInformation(pipe, HANDLE_FLAG_INHERIT, 0)) return 2;
    sb_error error = {0}; sb_project_registry registry = {0}; sb_host host = {0}; int code = 2;
    char *bytes = receive_frame(pipe, false, 0, SB_TEXT_LIMIT);
    char *data_identity = bytes ? receive_frame(pipe, false, 0, 64) : NULL;
    if (!bytes || !sb_projects_parse(&registry, bytes, strlen(bytes), &error) || registry.project_count != 1) goto done;
    host.project = &registry.projects[0]; const sb_project *project = host.project;
    sb_config_defaults(&host.config); host.started_ms = sb_platform_millis();
    host.config.data_dir = sb_strdup(project->data_dir); strcpy(host.config.token_env, project->token_env);
    host.config.limits = project->limits; host.config.approval_ttl_seconds = project->approval_ttl_seconds; host.config.audit_max_bytes = project->audit_max_bytes;
    host.config.execution_profile = SB_EXEC_USER;
    /* Legacy administrator stays unavailable until shared, project-authorized M5. */
    bool command_candidate = project->enabled && project->policy.execution_enabled && project->policy.default_identity == SB_ID_CURRENT_USER;
    if (command_candidate) {
        sb_request_context ctx = {.actor = "manager-owner", .project = project, .policy_revision = project->policy_revision};
        const sb_project_directory *cwd = sb_project_directory_find(project, project->default_workspace_id);
        sb_project_operation op = {.tool = "task_start", .directory = cwd, .identity = SB_ID_CURRENT_USER};
        host.config.execution_enabled = sb_project_evaluate(&ctx, &op).decision == SB_DECISION_ALLOW;
    }
    host.lease = host.config.data_dir && data_identity ? sb_platform_lease_bound(host.config.data_dir, data_identity, &error) : NULL;
    if (!host.lease) goto done;
    for (size_t i = 0; i < project->directory_count; ++i) {
        sb_workspace *workspace = &host.config.workspaces[host.config.workspace_count];
        workspace->root = sb_platform_root(project->directories[i].path, &error);
        if (!workspace->root) goto done;
        strcpy(workspace->id, project->directories[i].id); workspace->read_only = project->directories[i].read_only; ++host.config.workspace_count;
        for (size_t j = 0; j < i; ++j) if (sb_platform_roots_overlap(host.config.workspaces[j].root, workspace->root)) {
            sb_fail(&error, "Physical project directory aliases overlap."); goto done;
        }
    }
    host.execution = sb_execution_new(&host.config, &error); if (!host.execution) goto done;
    host.audit = sb_audit_new(host.lease, host.config.audit_max_bytes, &error); if (!host.audit) goto done;
    host.policy = sb_policy_new(host.config.approval_ttl_seconds, host.audit, &error); if (!host.policy) goto done;
    sb_policy_limit(host.policy, host.config.limits.approval_records);
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL); if (!doc) goto done;
    yyjson_mut_val *ready = yyjson_mut_obj(doc); yyjson_mut_doc_set_root(doc, ready);
    yyjson_mut_obj_add_bool(doc, ready, "ready", true); yyjson_mut_obj_add_str(doc, ready, "project_id", project->id);
    yyjson_mut_obj_add_uint(doc, ready, "policy_revision", project->policy_revision); yyjson_mut_obj_add_uint(doc, ready, "pid", GetCurrentProcessId());
    yyjson_mut_obj_add_str(doc, ready, "host_instance_id", sb_execution_instance(host.execution));
    char *greeting = yyjson_mut_write(doc, 0, NULL); yyjson_mut_doc_free(doc);
    bool sent = send_frame(pipe, greeting, false, 0); free(greeting); if (!sent) goto done;
    sb_protocol protocol = {0}; code = 0;
    for (;;) {
        char *envelope = receive_frame(pipe, false, 0, SB_REQUEST_LIMIT); if (!envelope) break;
        yyjson_doc *input = yyjson_read(envelope, strlen(envelope), 0);
        yyjson_val *root = yyjson_doc_get_root(input), *request = yyjson_obj_get(root, "request");
        static const char *const keys[] = {"kind", "request"};
        const char *kind = sb_string(yyjson_obj_get(root, "kind"));
        char *wire = yyjson_val_write(request, 0, NULL), *response = NULL;
        if (!wire || !kind || !yyjson_is_obj(request) || !sb_config_keys(root, keys, 2)) response = error_json("Invalid worker envelope.");
        else if (!strcmp(kind, "owner")) response = sb_policy_owner_request(host.policy, wire, strlen(wire));
        else if (!strcmp(kind, "mcp")) {
            sb_request_context ctx = {.actor = "manager-owner", .project = project, .policy_revision = project->policy_revision};
            size_t response_length = 0; int http_status = 0;
            response = sb_dispatch(&host, &protocol, wire, strlen(wire), &ctx, &response_length, &http_status);
            if (!response && http_status == 202) response = sb_strdup("{\"notification\":true}");
        } else response = error_json("Unknown worker envelope kind.");
        free(wire); yyjson_doc_free(input); free(envelope);
        sent = send_frame(pipe, response, false, 0); free(response); if (!sent) break;
    }
done:
    free(bytes); free(data_identity); sb_policy_free(host.policy); sb_audit_free(host.audit); sb_execution_free(host.execution);
    sb_config_free(&host.config); sb_platform_lease_close(host.lease); sb_projects_free(&registry); CloseHandle(pipe);
    return code;
}

bool sb_manager_option(const char *option) {
    static const char *const names[] = {"--manager", "--manager-stop", "--project-create", "--project-configure", "--project-start",
        "--project-stop", "--project-status", "--project-request", "--project-owner"};
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i) if (!strcmp(option, names[i])) return true;
    return false;
}
int sb_manager_cli(int argc, char **argv) {
    const char *mode = argv[1], *id = NULL, *registry = NULL, *data = NULL, *definition = NULL, *output = NULL, *request_file = NULL;
    bool editing = !strcmp(mode, "--project-create") || !strcmp(mode, "--project-configure");
    bool payload = !strcmp(mode, "--project-request") || !strcmp(mode, "--project-owner");
    int index = 2;
    if (strcmp(mode, "--manager") && strcmp(mode, "--manager-stop")) {
        if (index < argc && strncmp(argv[index], "--", 2)) id = argv[index++];
        if ((!id && strcmp(mode, "--project-status")) || (id && !sb_identifier(id))) goto invalid;
    }
    for (; index < argc; ++index) {
        const char **slot = !strcmp(argv[index], "--projects") ? &registry : !strcmp(argv[index], "--manager-data-dir") ? &data :
            !strcmp(argv[index], "--project-config") ? &definition : !strcmp(argv[index], "--output") ? &output :
            !strcmp(argv[index], "--request") ? &request_file : NULL;
        if (!slot || *slot || index + 1 >= argc || !*argv[index + 1]) goto invalid;
        *slot = argv[++index];
    }
    sb_error error = {0}; int code = 2; char *bytes = NULL, *project = NULL, *json = NULL, *backup = NULL;
    size_t length = 0, project_length = 0;
    if (editing) {
        if (!definition || !output || data || request_file || !sb_project_path_valid(output) || (!strcmp(mode, "--project-configure") && !registry)) goto invalid;
        if (registry) { bytes = sb_platform_load_config(registry, &length, &error); if (!bytes) goto done; }
        project = sb_platform_load_config(definition, &project_length, &error); if (!project) goto done;
        json = sb_projects_edit(bytes, length, project, project_length, id, !strcmp(mode, "--project-configure"), &error); if (!json) goto done;
        size_t capacity = strlen(output) + sizeof(".source-backup.json"); backup = malloc(capacity); if (!backup) goto done;
        snprintf(backup, capacity, "%s.source-backup.json", output);
        if (!sb_platform_export_config(output, backup, json, strlen(json), bytes ? bytes : project, bytes ? length : project_length, &error)) goto done;
        puts("{\"exported\":true,\"source_backup_saved\":true,\"running_configuration_changed\":false}"); code = 0;
    } else if (!strcmp(mode, "--manager")) {
        if (!registry || !data || definition || output || request_file) goto invalid;
        code = run_manager(registry, data);
    } else {
        if (!data || !sb_project_path_valid(data) || registry || definition || output || (payload != (request_file != NULL))) goto invalid;
        yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL); if (!doc) goto done;
        yyjson_mut_val *root = yyjson_mut_obj(doc); yyjson_mut_doc_set_root(doc, root);
        yyjson_mut_obj_add_str(doc, root, "command", mode + 2); if (id) yyjson_mut_obj_add_str(doc, root, "id", id);
        if (request_file) {
            project = sb_platform_load_config(request_file, &project_length, &error);
            yyjson_doc *input = project ? yyjson_read(project, project_length, 0) : NULL;
            if (!input || !yyjson_is_obj(yyjson_doc_get_root(input))) { yyjson_doc_free(input); yyjson_mut_doc_free(doc); sb_fail(&error, "Request file must contain a bounded JSON object."); goto done; }
            yyjson_mut_obj_add_val(doc, root, "request", yyjson_val_mut_copy(doc, yyjson_doc_get_root(input))); yyjson_doc_free(input);
        }
        json = yyjson_mut_write(doc, 0, NULL); yyjson_mut_doc_free(doc); if (!json) goto done;
        bytes = sb_owner_exchange(data, json, strlen(json), &error); if (!bytes) goto done;
        yyjson_doc *reply = yyjson_read(bytes, strlen(bytes), 0);
        if (reply) { puts(bytes); code = yyjson_obj_get(yyjson_doc_get_root(reply), "error") ? 2 : 0; }
        yyjson_doc_free(reply);
    }
done:
    if (code && strcmp(mode, "--manager")) fprintf(stderr, "ShellBot: %s\n", *error.text ? error.text : "Command failed.");
    free(bytes); free(project); free(json); free(backup); return code;
invalid:
    fputs("ShellBot: Invalid project management arguments. Use --help.\n", stderr); return 2;
}
