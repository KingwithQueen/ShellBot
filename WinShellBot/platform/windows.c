#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <winternl.h>
#include <bcrypt.h>
#include <shlobj.h>
#include <sddl.h>
#include "sb.h"
#include "sb_projects.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

struct sb_root { HANDLE handle; wchar_t *path; };
struct sb_file { HANDLE handle; wchar_t *path; uint64_t size; };
struct sb_lease { HANDLE handle; sb_root *root; SRWLOCK backup_lock; unsigned backups; };
struct sb_journal { HANDLE handle; };
struct sb_mutex { SRWLOCK lock; };
struct sb_create { sb_file *parent; wchar_t *leaf; char *target; char identity[96]; };
struct sb_modify { sb_create *location; HANDLE handle; char *original; size_t length; char identity[96]; bool backed_up; };

static wchar_t *wide(const char *text) {
    if (!text || strlen(text) >= SB_PATH_CAP) return NULL;
    int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, NULL, 0);
    if (!size) return NULL;
    wchar_t *value = malloc((size_t)size * sizeof(*value));
    if (value && !MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, value, size)) {
        free(value); return NULL;
    }
    return value;
}

static char *utf8(const wchar_t *text) {
    int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text, -1, NULL, 0, NULL, NULL);
    if (!size) return NULL;
    char *value = malloc((size_t)size);
    if (value && !WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text, -1, value, size, NULL, NULL)) {
        free(value); return NULL;
    }
    return value;
}

static wchar_t *final_path(HANDLE handle) {
    DWORD length = GetFinalPathNameByHandleW(handle, NULL, 0, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (!length || length >= SB_PATH_CAP) return NULL;
    wchar_t *path = calloc((size_t)length + 1, sizeof(*path));
    if (!path) return NULL;
    DWORD got = GetFinalPathNameByHandleW(handle, path, length + 1, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (!got || got > length) { free(path); return NULL; }
    while (got && path[got - 1] == L'\\') path[--got] = 0;
    return path;
}

char *sb_platform_directory_path(sb_file *directory, sb_error *error) {
    char *path = directory ? utf8(directory->path) : NULL;
    if (!path) sb_fail(error, "Cannot resolve the actual task working directory.");
    return path;
}

uint64_t sb_platform_millis(void) { return GetTickCount64(); }
uint64_t sb_platform_unix_ms(void) {
    FILETIME time; GetSystemTimeAsFileTime(&time);
    uint64_t ticks = ((uint64_t)time.dwHighDateTime << 32) | time.dwLowDateTime;
    return (ticks - UINT64_C(116444736000000000)) / 10000;
}

void sb_platform_info(char *system, size_t capacity, char *arch, size_t arch_capacity) {
    typedef LONG (WINAPI *version_fn)(OSVERSIONINFOW *);
    OSVERSIONINFOW version = {0};
    version.dwOSVersionInfoSize = sizeof(version);
    HMODULE module = GetModuleHandleW(L"ntdll.dll");
    FARPROC address = module ? GetProcAddress(module, "RtlGetVersion") : NULL;
    version_fn get_version = NULL;
    _Static_assert(sizeof(get_version) == sizeof(address), "Windows function pointer size");
    memcpy(&get_version, &address, sizeof(get_version));
    if (get_version && get_version(&version) == 0)
        snprintf(system, capacity, "Windows %lu.%lu (build %lu)", version.dwMajorVersion, version.dwMinorVersion, version.dwBuildNumber);
    else snprintf(system, capacity, "Windows");
    SYSTEM_INFO info;
    GetNativeSystemInfo(&info);
    snprintf(arch, arch_capacity, "%s", info.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_AMD64 ? "x64" :
        info.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_ARM64 ? "arm64" : "x86");
}

sb_root *sb_platform_root(const char *path, sb_error *error) {
    wchar_t *directory = wide(path);
    if (!directory) { sb_fail(error, "Invalid UTF-8 root path."); return NULL; }
    /* Require an absolute disk or UNC path. No cwd-dependent permissions. */
    bool absolute = wcslen(directory) >= 3 && ((directory[1] == L':' && (directory[2] == L'\\' || directory[2] == L'/')) ||
        (directory[0] == L'\\' && directory[1] == L'\\'));
    if (!absolute) { free(directory); sb_fail(error, "Workspace roots must be absolute paths."); return NULL; }
    HANDLE handle = CreateFileW(directory, FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
    free(directory);
    if (handle == INVALID_HANDLE_VALUE) { sb_fail(error, "Cannot open workspace directory."); return NULL; }
    BY_HANDLE_FILE_INFORMATION info = {0};
    if (GetFileType(handle) != FILE_TYPE_DISK || !GetFileInformationByHandle(handle, &info) || !(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
        CloseHandle(handle); sb_fail(error, "Workspace root must be a disk directory."); return NULL;
    }
    sb_root *root = calloc(1, sizeof(*root));
    if (!root) { CloseHandle(handle); sb_fail(error, "Out of memory."); return NULL; }
    root->handle = handle; root->path = final_path(handle);
    if (!root->path) { sb_platform_root_close(root); sb_fail(error, "Cannot resolve workspace path."); return NULL; }
    return root;
}

void sb_platform_root_close(sb_root *root) {
    if (root) { CloseHandle(root->handle); free(root->path); free(root); }
}

static bool relative_path(wchar_t *path) {
    if (*path == L'\\' || *path == L'/') return false;
    wchar_t *component = path;
    for (wchar_t *p = path;; ++p) {
        if (*p == L'/') *p = L'\\';
        if (*p && (*p < 32 || wcschr(L":*?\"<>|", *p))) return false;
        if (!*p || *p == L'\\') {
            size_t length = (size_t)(p - component);
            if (length == 2 && component[0] == L'.' && component[1] == L'.') return false;
            if (length > 1 && (p[-1] == L'.' || p[-1] == L' ')) return false;
            if (!*p) break;
            component = p + 1;
        }
    }
    /* Extended-length Win32 paths do not resolve '.' automatically. */
    wchar_t *source = path, *destination = path;
    while (*source) {
        while (*source == L'\\') ++source;
        wchar_t *start = source;
        while (*source && *source != L'\\') ++source;
        size_t length = (size_t)(source - start);
        if (length && !(length == 1 && start[0] == L'.')) {
            if (destination != path) *destination++ = L'\\';
            memmove(destination, start, length * sizeof(*destination));
            destination += length;
        }
    }
    *destination = 0;
    return true;
}

sb_file *sb_platform_open(sb_root *root, const char *relative, bool directory, sb_error *error) {
    wchar_t *suffix = wide(relative);
    if (!suffix || !relative_path(suffix)) { free(suffix); sb_fail(error, "Path must be relative without parent traversal, streams, or wildcards."); return NULL; }
    size_t prefix = wcslen(root->path), size = prefix + wcslen(suffix) + 2;
    if (size >= SB_PATH_CAP) { free(suffix); sb_fail(error, "Path is too long."); return NULL; }
    wchar_t *path = calloc(size, sizeof(*path));
    if (!path) { free(suffix); sb_fail(error, "Out of memory."); return NULL; }
    swprintf(path, size, L"%ls\\%ls", root->path, suffix);
    free(suffix);
    HANDLE handle = CreateFileW(path, directory ? FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES : GENERIC_READ,
        FILE_SHARE_READ | (directory ? FILE_SHARE_WRITE : 0), NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
    free(path);
    if (handle == INVALID_HANDLE_VALUE) { sb_fail(error, "Cannot open path for reading."); return NULL; }
    wchar_t *actual = final_path(handle);
    BY_HANDLE_FILE_INFORMATION info = {0};
    bool valid = actual && wcslen(actual) >= prefix &&
        /* Normalized handle paths retain stored casing. Exact comparison also
           protects roots beneath Windows case-sensitive directories. */
        CompareStringOrdinal(actual, (int)prefix, root->path, (int)prefix, FALSE) == CSTR_EQUAL &&
        (actual[prefix] == 0 || actual[prefix] == L'\\') && GetFileType(handle) == FILE_TYPE_DISK &&
        GetFileInformationByHandle(handle, &info) &&
        ((info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) == directory;
    if (!valid) { free(actual); CloseHandle(handle); sb_fail(error, "Resolved path is outside the workspace or has the wrong file type."); return NULL; }
    sb_file *file = calloc(1, sizeof(*file));
    if (!file) { free(actual); CloseHandle(handle); sb_fail(error, "Out of memory."); return NULL; }
    file->handle = handle; file->path = actual;
    file->size = ((uint64_t)info.nFileSizeHigh << 32) | info.nFileSizeLow;
    return file;
}

void sb_platform_close(sb_file *file) {
    if (file) { CloseHandle(file->handle); free(file->path); free(file); }
}

uint64_t sb_platform_size(sb_file *file) { return file->size; }

bool sb_platform_read(sb_file *file, uint64_t offset, unsigned char *buffer,
                      size_t capacity, size_t *length, sb_error *error) {
    if (offset > file->size || capacity > UINT32_MAX) { sb_fail(error, "Invalid read range."); return false; }
    LARGE_INTEGER position;
    position.QuadPart = (LONGLONG)offset;
    DWORD got = 0;
    if (!SetFilePointerEx(file->handle, position, NULL, FILE_BEGIN) || !ReadFile(file->handle, buffer, (DWORD)capacity, &got, NULL)) {
        sb_fail(error, "File read failed."); return false;
    }
    *length = got;
    return true;
}

bool sb_platform_list(sb_file *directory, size_t offset, size_t limit,
                      sb_entry **entries, size_t *count, bool *more, sb_error *error) {
    *entries = NULL; *count = 0; *more = false;
    size_t size = wcslen(directory->path) + 3;
    wchar_t *pattern = calloc(size, sizeof(*pattern));
    sb_entry *items = calloc(limit, sizeof(*items));
    if (!pattern || !items) { free(pattern); free(items); sb_fail(error, "Out of memory."); return false; }
    swprintf(pattern, size, L"%ls\\*", directory->path);
    WIN32_FIND_DATAW data;
    HANDLE search = FindFirstFileW(pattern, &data);
    free(pattern);
    if (search == INVALID_HANDLE_VALUE) {
        free(items);
        if (GetLastError() == ERROR_FILE_NOT_FOUND) return true;
        sb_fail(error, "Directory enumeration failed."); return false;
    }
    size_t index = 0;
    DWORD last = ERROR_SUCCESS;
    do {
        if (!wcscmp(data.cFileName, L".") || !wcscmp(data.cFileName, L"..")) continue;
        if (index++ < offset) continue;
        if (*count == limit) { *more = true; break; }
        sb_entry *entry = &items[*count];
        entry->name = utf8(data.cFileName);
        if (!entry->name) { last = ERROR_NO_UNICODE_TRANSLATION; break; }
        entry->size = ((uint64_t)data.nFileSizeHigh << 32) | data.nFileSizeLow;
        entry->directory = (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        entry->reparse = (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
        ++*count;
    } while (FindNextFileW(search, &data));
    if (!*more && last == ERROR_SUCCESS) last = GetLastError();
    FindClose(search);
    if (last != ERROR_SUCCESS && last != ERROR_NO_MORE_FILES) {
        sb_platform_entries_free(items, *count); *count = 0; sb_fail(error, "Directory enumeration failed."); return false;
    }
    *entries = items;
    return true;
}

void sb_platform_entries_free(sb_entry *entries, size_t count) {
    for (size_t i = 0; i < count; ++i) free(entries[i].name);
    free(entries);
}

char *sb_platform_default_data_dir(sb_error *error) {
    PWSTR local = NULL;
    if (FAILED(SHGetKnownFolderPath(&FOLDERID_LocalAppData, 0, NULL, &local))) {
        sb_fail(error, "Cannot locate local application data."); return NULL;
    }
    size_t size = wcslen(local) + 10;
    wchar_t *path = calloc(size, sizeof(*path));
    if (path) swprintf(path, size, L"%ls\\ShellBot", local);
    CoTaskMemFree(local);
    char *result = path ? utf8(path) : NULL;
    free(path);
    if (!result) sb_fail(error, "Cannot allocate data path.");
    return result;
}

sb_lease *sb_platform_lease(const char *directory, sb_error *error) {
    wchar_t *path = wide(directory);
    if (!path) { sb_fail(error, "Invalid data directory."); return NULL; }
    int result = SHCreateDirectoryExW(NULL, path, NULL);
    if (result != ERROR_SUCCESS && result != ERROR_ALREADY_EXISTS && result != ERROR_FILE_EXISTS) {
        free(path); sb_fail(error, "Cannot create data directory. Use an absolute path."); return NULL;
    }
    free(path);
    sb_root *root = sb_platform_root(directory, error);
    if (!root) return NULL;
    size_t size = wcslen(root->path) + 20;
    wchar_t *lock = calloc(size, sizeof(*lock));
    if (lock) swprintf(lock, size, L"%ls\\.shellbot.lock", root->path);
    if (!lock) { sb_platform_root_close(root); sb_fail(error, "Out of memory."); return NULL; }
    HANDLE handle = CreateFileW(lock, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    free(lock);
    if (handle == INVALID_HANDLE_VALUE) { sb_platform_root_close(root); sb_fail(error, "Data directory is locked by another instance or cannot be accessed."); return NULL; }
    sb_lease *lease = calloc(1, sizeof(*lease));
    if (!lease) { CloseHandle(handle); sb_platform_root_close(root); sb_fail(error, "Out of memory."); return NULL; }
    lease->handle = handle; lease->root = root; InitializeSRWLock(&lease->backup_lock);
    return lease;
}

void sb_platform_lease_close(sb_lease *lease) {
    if (lease) { CloseHandle(lease->handle); sb_platform_root_close(lease->root); free(lease); }
}

sb_journal *sb_platform_journal_open(sb_lease *lease, const char *filename, sb_error *error) {
    wchar_t *name = wide(filename);
    if (!lease || !name || !*name || wcspbrk(name, L"\\/:*?\"<>|")) {
        free(name); sb_fail(error, "Invalid audit journal name."); return NULL;
    }
    HANDLE token = NULL; DWORD size = 0; void *user = NULL;
    LPWSTR sid = NULL; PSECURITY_DESCRIPTOR descriptor = NULL;
    bool secured = OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token) != FALSE;
    if (secured) { GetTokenInformation(token, TokenUser, NULL, 0, &size); user = size ? malloc(size) : NULL; }
    secured = secured && user && GetTokenInformation(token, TokenUser, user, size, &size) &&
        ConvertSidToStringSidW(((TOKEN_USER *)user)->User.Sid, &sid);
    if (token) CloseHandle(token);
    wchar_t sddl[256];
    if (secured) {
        swprintf(sddl, 256, L"D:P(A;;FA;;;SY)(A;;FA;;;%ls)", sid);
        secured = ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl, SDDL_REVISION_1, &descriptor, NULL) != FALSE;
    }
    if (sid) LocalFree(sid);
    free(user);
    typedef NTSTATUS (NTAPI *create_fn)(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, PIO_STATUS_BLOCK,
        PLARGE_INTEGER, ULONG, ULONG, ULONG, ULONG, PVOID, ULONG);
    HMODULE module = GetModuleHandleW(L"ntdll.dll");
    FARPROC address = module ? GetProcAddress(module, "NtCreateFile") : NULL;
    create_fn create = NULL;
    _Static_assert(sizeof(create) == sizeof(address), "Native function pointer size");
    memcpy(&create, &address, sizeof(create));
    UNICODE_STRING leaf = {0}; leaf.Buffer = name; leaf.Length = (USHORT)(wcslen(name) * sizeof(wchar_t)); leaf.MaximumLength = leaf.Length;
    OBJECT_ATTRIBUTES attributes = {0}; attributes.Length = sizeof(attributes);
    attributes.RootDirectory = lease->root->handle; attributes.ObjectName = &leaf; attributes.SecurityDescriptor = descriptor;
    IO_STATUS_BLOCK io = {0}; HANDLE handle = NULL;
    /* Create-new relative to the pinned directory, never open a pre-existing link.
       Only readers may share the handle. Protected DACL excludes inherited grants. */
    NTSTATUS status = secured && create ? create(&handle, GENERIC_WRITE | SYNCHRONIZE, &attributes, &io, NULL,
        FILE_ATTRIBUTE_NORMAL, FILE_SHARE_READ, 2, 0x00200060, NULL, 0) : (NTSTATUS)-1;
    if (descriptor) LocalFree(descriptor);
    free(name);
    if (status < 0) { sb_fail(error, "Cannot create a protected audit journal."); return NULL; }
    /* Verify the file system retained the protected, nonempty DACL. FAT is not a
       valid storage target for these private audit records. */
    size = 0; GetKernelObjectSecurity(handle, DACL_SECURITY_INFORMATION, NULL, 0, &size);
    descriptor = size ? malloc(size) : NULL;
    SECURITY_DESCRIPTOR_CONTROL control = 0; DWORD revision = 0;
    PACL acl = NULL; BOOL present = FALSE, defaulted = FALSE;
    bool valid = GetFileType(handle) == FILE_TYPE_DISK && descriptor &&
        GetKernelObjectSecurity(handle, DACL_SECURITY_INFORMATION, descriptor, size, &size) &&
        GetSecurityDescriptorControl(descriptor, &control, &revision) && (control & SE_DACL_PROTECTED) &&
        GetSecurityDescriptorDacl(descriptor, &present, &acl, &defaulted) && present && acl && acl->AceCount > 0;
    free(descriptor);
    sb_journal *journal = valid ? malloc(sizeof(*journal)) : NULL;
    if (!journal) { CloseHandle(handle); sb_fail(error, "Audit storage requires a protected file ACL."); return NULL; }
    journal->handle = handle; return journal;
}

bool sb_platform_journal_append(sb_journal *journal, const char *line, size_t length, sb_error *error) {
    DWORD written = 0;
    bool ok = journal && length <= UINT32_MAX && WriteFile(journal->handle, line, (DWORD)length, &written, NULL) &&
        written == length && FlushFileBuffers(journal->handle);
    if (!ok) sb_fail(error, "Cannot write and flush audit record.");
    return ok;
}

void sb_platform_journal_close(sb_journal *journal) {
    if (journal) { CloseHandle(journal->handle); free(journal); }
}

bool sb_platform_random(unsigned char *buffer, size_t size, sb_error *error) {
    if (size > UINT32_MAX || BCryptGenRandom(NULL, buffer, (ULONG)size, BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) {
        sb_fail(error, "System random generation failed."); return false;
    }
    return true;
}

char *sb_platform_load_config(const char *path, size_t *length, sb_error *error) {
    wchar_t *name = wide(path);
    HANDLE handle = name ? CreateFileW(name, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL) : INVALID_HANDLE_VALUE;
    free(name);
    if (handle == INVALID_HANDLE_VALUE) { sb_fail(error, "Cannot open configuration file."); return NULL; }
    LARGE_INTEGER size;
    char *buffer = NULL;
    if (GetFileType(handle) == FILE_TYPE_DISK && GetFileSizeEx(handle, &size) && size.QuadPart > 0 && size.QuadPart <= SB_TEXT_LIMIT) {
        buffer = malloc((size_t)size.QuadPart);
        DWORD got = 0;
        if (buffer && ReadFile(handle, buffer, (DWORD)size.QuadPart, &got, NULL) && got == size.QuadPart) *length = got;
        else { free(buffer); buffer = NULL; }
    }
    CloseHandle(handle);
    if (!buffer) sb_fail(error, "Configuration must be a readable disk file between 1 byte and 64 KiB.");
    return buffer;
}

static void discard_export(HANDLE handle) {
    FILE_DISPOSITION_INFO disposition = {TRUE};
    SetFileInformationByHandle(handle, FileDispositionInfo, &disposition, sizeof(disposition));
}

bool sb_platform_export_config(const char *output, const char *backup,
                               const char *json, size_t json_length,
                               const char *original, size_t original_length, sb_error *error) {
    if (!sb_project_path_valid(output) || !sb_project_path_valid(backup) ||
        !json || !original || !json_length || !original_length || json_length > SB_TEXT_LIMIT || original_length > SB_TEXT_LIMIT) {
        sb_fail(error, "Configuration export needs new absolute file paths and bounded JSON."); return false;
    }
    wchar_t *output_name = wide(output), *backup_name = wide(backup);
    HANDLE token = NULL; DWORD size = 0; void *user = NULL;
    LPWSTR sid = NULL; PSECURITY_DESCRIPTOR descriptor = NULL;
    bool secured = OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token) != FALSE;
    if (secured) { GetTokenInformation(token, TokenUser, NULL, 0, &size); user = size ? malloc(size) : NULL; }
    secured = secured && user && GetTokenInformation(token, TokenUser, user, size, &size) &&
        ConvertSidToStringSidW(((TOKEN_USER *)user)->User.Sid, &sid);
    if (token) CloseHandle(token);
    wchar_t sddl[256];
    if (secured) {
        swprintf(sddl, 256, L"D:P(A;;FA;;;SY)(A;;FA;;;%ls)", sid);
        secured = ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl, SDDL_REVISION_1, &descriptor, NULL) != FALSE;
    }
    if (sid) LocalFree(sid);
    free(user);
    SECURITY_ATTRIBUTES attributes = {sizeof(attributes), descriptor, FALSE};
    HANDLE out = secured && output_name ? CreateFileW(output_name, GENERIC_WRITE | DELETE, 0, &attributes,
        CREATE_NEW, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, NULL) : INVALID_HANDLE_VALUE;
    HANDLE old = out != INVALID_HANDLE_VALUE && backup_name ? CreateFileW(backup_name, GENERIC_WRITE | DELETE, 0, &attributes,
        CREATE_NEW, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, NULL) : INVALID_HANDLE_VALUE;
    if (descriptor) LocalFree(descriptor);
    free(output_name); free(backup_name);
    DWORD written = 0;
    bool backup_saved = old != INVALID_HANDLE_VALUE && WriteFile(old, original, (DWORD)original_length, &written, NULL) &&
        written == original_length && FlushFileBuffers(old);
    bool ok = backup_saved && WriteFile(out, json, (DWORD)json_length, &written, NULL) && written == json_length && FlushFileBuffers(out);
    /* Delete only files created by this call, through their still-exclusive
       handles. A completed backup is retained if output writing fails. */
    if (old != INVALID_HANDLE_VALUE) { if (!backup_saved) discard_export(old); CloseHandle(old); }
    if (out != INVALID_HANDLE_VALUE) { if (!ok) discard_export(out); CloseHandle(out); }
    if (!ok) sb_fail(error, backup_saved ? "Export failed; the completed legacy backup was retained." :
        "Export refused or failed. Output and backup must both be new files in existing directories.");
    return ok;
}

sb_mutex *sb_platform_mutex_new(void) {
    sb_mutex *mutex = malloc(sizeof(*mutex));
    if (mutex) InitializeSRWLock(&mutex->lock);
    return mutex;
}
void sb_platform_mutex_free(sb_mutex *mutex) { free(mutex); }
void sb_platform_mutex_lock(sb_mutex *mutex) { AcquireSRWLockExclusive(&mutex->lock); }
void sb_platform_mutex_unlock(sb_mutex *mutex) { ReleaseSRWLockExclusive(&mutex->lock); }

bool sb_platform_sha256(const void *bytes, size_t length, char hex[65], sb_error *error) {
    BCRYPT_ALG_HANDLE algorithm = NULL;
    BCRYPT_HASH_HANDLE hash = NULL;
    unsigned char digest[32];
    bool ok = length <= UINT32_MAX &&
        BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, NULL, 0) == 0 &&
        BCryptCreateHash(algorithm, &hash, NULL, 0, NULL, 0, 0) == 0 &&
        BCryptHashData(hash, (PUCHAR)bytes, (ULONG)length, 0) == 0 &&
        BCryptFinishHash(hash, digest, sizeof(digest), 0) == 0;
    if (hash) BCryptDestroyHash(hash);
    if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
    if (!ok) { sb_fail(error, "System SHA-256 failed."); return false; }
    for (size_t i = 0; i < sizeof(digest); ++i) snprintf(hex + i * 2, 3, "%02x", digest[i]);
    return true;
}

sb_create *sb_platform_prepare_create(sb_root *root, const char *relative, sb_error *error) {
    size_t relative_length = strlen(relative);
    if (!relative_length || relative[relative_length - 1] == '/' || relative[relative_length - 1] == '\\') {
        sb_fail(error, "Creation requires a file name, not a directory path."); return NULL;
    }
    wchar_t *suffix = wide(relative);
    if (!suffix || !relative_path(suffix) || !*suffix) {
        free(suffix); sb_fail(error, "Creation requires a relative file path without traversal, streams, or wildcards."); return NULL;
    }
    wchar_t *separator = wcsrchr(suffix, L'\\');
    wchar_t *leaf = _wcsdup(separator ? separator + 1 : suffix);
    if (separator) *separator = 0;
    char *parent_path = separator ? utf8(suffix) : sb_strdup(".");
    free(suffix);
    if (!leaf || !parent_path || wcslen(leaf) > 255 || (wcslen(leaf) && leaf[wcslen(leaf) - 1] == L' ')) {
        free(leaf); free(parent_path); sb_fail(error, "Invalid file name or allocation failure."); return NULL;
    }
    sb_file *parent = sb_platform_open(root, parent_path, true, error);
    free(parent_path);
    if (!parent) { free(leaf); return NULL; }
    FILE_ID_INFO info;
    size_t size = wcslen(parent->path) + wcslen(leaf) + 2;
    wchar_t *target = size < SB_PATH_CAP ? calloc(size, sizeof(*target)) : NULL;
    sb_create *plan = calloc(1, sizeof(*plan));
    if (!target || !plan || !GetFileInformationByHandleEx(parent->handle, FileIdInfo, &info, sizeof(info))) {
        free(target); free(plan); free(leaf); sb_platform_close(parent);
        sb_fail(error, "Cannot prepare anchored file creation."); return NULL;
    }
    swprintf(target, size, L"%ls\\%ls", parent->path, leaf);
    plan->parent = parent; plan->leaf = leaf; plan->target = utf8(target);
    free(target);
    snprintf(plan->identity, sizeof(plan->identity), "%016llx:", (unsigned long long)info.VolumeSerialNumber);
    for (size_t i = 0; i < sizeof(info.FileId.Identifier); ++i)
        snprintf(plan->identity + 17 + i * 2, 3, "%02x", info.FileId.Identifier[i]);
    if (!plan->target) { sb_platform_create_close(plan); sb_fail(error, "Cannot encode target path."); return NULL; }
    return plan;
}
const char *sb_platform_create_target(sb_create *plan) { return plan->target; }
const char *sb_platform_create_identity(sb_create *plan) { return plan->identity; }
void sb_platform_create_close(sb_create *plan) {
    if (plan) { sb_platform_close(plan->parent); free(plan->leaf); free(plan->target); free(plan); }
}

bool sb_platform_create_text(sb_create *plan, const char *text, size_t length, sb_error *error) {
    /* Native relative opens anchor all mutations to the already validated directory.
       A lexical path/reparse change cannot redirect these operations elsewhere. */
    typedef NTSTATUS (NTAPI *create_fn)(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, PIO_STATUS_BLOCK,
        PLARGE_INTEGER, ULONG, ULONG, ULONG, ULONG, PVOID, ULONG);
    typedef NTSTATUS (NTAPI *set_fn)(HANDLE, PIO_STATUS_BLOCK, PVOID, ULONG, FILE_INFORMATION_CLASS);
    typedef struct { BOOLEAN replace; HANDLE directory; ULONG length; WCHAR name[1]; } rename_info;
    HMODULE module = GetModuleHandleW(L"ntdll.dll");
    FARPROC create_address = module ? GetProcAddress(module, "NtCreateFile") : NULL;
    FARPROC set_address = module ? GetProcAddress(module, "NtSetInformationFile") : NULL;
    create_fn create = NULL; set_fn set = NULL;
    _Static_assert(sizeof(create) == sizeof(create_address), "Native function pointer size");
    _Static_assert(sizeof(set) == sizeof(set_address), "Native function pointer size");
    memcpy(&create, &create_address, sizeof(create)); memcpy(&set, &set_address, sizeof(set));
    if (!create || !set || length > SB_TEXT_LIMIT) { sb_fail(error, "Native anchored creation is unavailable."); return false; }
    unsigned char random[16];
    if (!sb_platform_random(random, sizeof(random), error)) return false;
    wchar_t temporary[64];
    wcscpy(temporary, L".shellbot-");
    for (size_t i = 0; i < sizeof(random); ++i) swprintf(temporary + 9 + i * 2, 3, L"%02x", random[i]);
    wcscat(temporary, L".tmp");
    UNICODE_STRING name = {0};
    name.Buffer = temporary; name.Length = (USHORT)(wcslen(temporary) * sizeof(wchar_t)); name.MaximumLength = name.Length;
    OBJECT_ATTRIBUTES attributes = {0};
    attributes.Length = sizeof(attributes); attributes.RootDirectory = plan->parent->handle; attributes.ObjectName = &name;
    IO_STATUS_BLOCK io = {0}; HANDLE file = NULL;
    /* FILE_CREATE=2, NON_DIRECTORY=0x40, SYNCHRONOUS_NONALERT=0x20,
       OPEN_REPARSE_POINT=0x00200000, FileRenameInformation=10 (documented NT ABI). */
    NTSTATUS status = create(&file, GENERIC_WRITE | DELETE | SYNCHRONIZE, &attributes, &io, NULL,
        FILE_ATTRIBUTE_NORMAL, 0, 2, 0x00200060, NULL, 0);
    if (status < 0) { sb_fail(error, "Cannot create a temporary file in the approved parent directory."); return false; }
    DWORD written = 0;
    bool ok = GetFileType(file) == FILE_TYPE_DISK && WriteFile(file, text, (DWORD)length, &written, NULL) &&
        written == length && FlushFileBuffers(file);
    size_t rename_size = sizeof(rename_info) + wcslen(plan->leaf) * sizeof(wchar_t);
    rename_info *rename = ok ? calloc(1, rename_size) : NULL;
    if (rename) {
        rename->replace = FALSE; rename->directory = plan->parent->handle;
        rename->length = (ULONG)(wcslen(plan->leaf) * sizeof(wchar_t));
        memcpy(rename->name, plan->leaf, rename->length);
        ok = set(file, &io, rename, (ULONG)rename_size, (FILE_INFORMATION_CLASS)10) >= 0;
    } else ok = false;
    free(rename);
    if (!ok) {
        /* Delete only our newly created temporary file, through its own handle. */
        FILE_DISPOSITION_INFO disposition = {TRUE};
        SetFileInformationByHandle(file, FileDispositionInfo, &disposition, sizeof(disposition));
        sb_fail(error, "Create-only write failed. The destination may already exist or access was denied.");
    }
    CloseHandle(file);
    return ok;
}

sb_modify *sb_platform_prepare_modify(sb_root *root, const char *relative, sb_error *error) {
    sb_create *location = sb_platform_prepare_create(root, relative, error);
    if (!location) return NULL;
    typedef NTSTATUS (NTAPI *create_fn)(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, PIO_STATUS_BLOCK,
        PLARGE_INTEGER, ULONG, ULONG, ULONG, ULONG, PVOID, ULONG);
    HMODULE module = GetModuleHandleW(L"ntdll.dll");
    FARPROC address = module ? GetProcAddress(module, "NtCreateFile") : NULL;
    create_fn create = NULL;
    _Static_assert(sizeof(create) == sizeof(address), "Native function pointer size");
    memcpy(&create, &address, sizeof(create));
    UNICODE_STRING name = {0}; name.Buffer = location->leaf;
    name.Length = (USHORT)(wcslen(name.Buffer) * sizeof(wchar_t)); name.MaximumLength = name.Length;
    OBJECT_ATTRIBUTES attributes = {0}; attributes.Length = sizeof(attributes);
    attributes.RootDirectory = location->parent->handle; attributes.ObjectName = &name;
    IO_STATUS_BLOCK io = {0}; HANDLE handle = NULL;
    /* Open-existing relative to the pinned parent, do not follow the leaf reparse
       point, and deny shared reads/writes/deletes while checking and editing. */
    NTSTATUS status = create ? create(&handle, GENERIC_READ | GENERIC_WRITE | SYNCHRONIZE,
        &attributes, &io, NULL, 0, 0, 1, 0x00200060, NULL, 0) : (NTSTATUS)-1;
    if (status < 0) { sb_platform_create_close(location); sb_fail(error, "Cannot exclusively open an existing writable file."); return NULL; }
    BY_HANDLE_FILE_INFORMATION basic = {0}; FILE_ID_INFO info = {0};
    wchar_t *actual = final_path(handle); wchar_t *expected = wide(location->target);
    bool valid = actual && expected && !wcscmp(actual, expected) && GetFileType(handle) == FILE_TYPE_DISK &&
        GetFileInformationByHandle(handle, &basic) && GetFileInformationByHandleEx(handle, FileIdInfo, &info, sizeof(info)) &&
        !(basic.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_READONLY)) &&
        basic.nNumberOfLinks == 1 && !basic.nFileSizeHigh && basic.nFileSizeLow <= SB_TEXT_LIMIT;
    free(actual); free(expected);
    sb_modify *plan = valid ? calloc(1, sizeof(*plan)) : NULL;
    if (!plan) { CloseHandle(handle); sb_platform_create_close(location); sb_fail(error, "Modification requires a regular single-link file of at most 64 KiB."); return NULL; }
    plan->location = location; plan->handle = handle; plan->length = basic.nFileSizeLow;
    plan->original = calloc(plan->length + 1, 1);
    DWORD got = 0;
    if (!plan->original || !ReadFile(handle, plan->original, (DWORD)plan->length, &got, NULL) || got != plan->length) {
        sb_platform_modify_close(plan); sb_fail(error, "Cannot read the complete original file."); return NULL;
    }
    snprintf(plan->identity, sizeof(plan->identity), "%016llx:", (unsigned long long)info.VolumeSerialNumber);
    for (size_t i = 0; i < sizeof(info.FileId.Identifier); ++i)
        snprintf(plan->identity + 17 + i * 2, 3, "%02x", info.FileId.Identifier[i]);
    return plan;
}
const char *sb_platform_modify_target(sb_modify *plan) { return plan->location->target; }
const char *sb_platform_modify_parent(sb_modify *plan) { return plan->location->identity; }
const char *sb_platform_modify_identity(sb_modify *plan) { return plan->identity; }
const char *sb_platform_modify_original(sb_modify *plan, size_t *length) { *length = plan->length; return plan->original; }
void sb_platform_modify_close(sb_modify *plan) {
    if (plan) { CloseHandle(plan->handle); sb_platform_create_close(plan->location); free(plan->original); free(plan); }
}
static bool replace_locked_bytes(HANDLE file, const char *text, size_t length) {
    LARGE_INTEGER position; position.QuadPart = 0;
    DWORD written = 0;
    return SetFilePointerEx(file, position, NULL, FILE_BEGIN) && WriteFile(file, text, (DWORD)length, &written, NULL) &&
        written == length && SetEndOfFile(file) && FlushFileBuffers(file);
}
bool sb_platform_modify_backup(sb_modify *plan, sb_lease *lease, char backup_id[33], sb_error *error) {
    backup_id[0] = 0;
    AcquireSRWLockExclusive(&lease->backup_lock);
    bool available = lease->backups < 64;
    if (available) ++lease->backups;
    ReleaseSRWLockExclusive(&lease->backup_lock);
    if (!available) { sb_fail(error, "This instance reached its 64-backup limit; modification was not started."); return false; }
    unsigned char random[16]; char id[33], filename[64];
    if (!sb_platform_random(random, sizeof(random), error)) return false;
    for (size_t i = 0; i < sizeof(random); ++i) snprintf(id + i * 2, 3, "%02x", random[i]);
    snprintf(filename, sizeof(filename), "backup-%s.bin", id);
    /* The existing anchored journal primitive also creates binary backups with a
       protected DACL restricted to the current owner and SYSTEM. */
    sb_journal *backup = sb_platform_journal_open(lease, filename, error);
    if (!backup) return false;
    bool saved = sb_platform_journal_append(backup, plan->original, plan->length, error);
    sb_platform_journal_close(backup);
    if (!saved) { sb_fail(error, "Cannot flush the private backup; original file was not modified."); return false; }
    memcpy(backup_id, id, sizeof(id));
    plan->backed_up = true;
    return true;
}
bool sb_platform_modify_text(sb_modify *plan, const char *text, size_t length, sb_error *error) {
    if (!plan->backed_up || length > SB_TEXT_LIMIT) { sb_fail(error, "Modification requires a flushed backup and text within 64 KiB."); return false; }
    if (replace_locked_bytes(plan->handle, text, length)) return true;
    bool restored = replace_locked_bytes(plan->handle, plan->original, plan->length);
    sb_fail(error, restored ? "Write failed; original bytes were restored and the private backup was retained." :
        "Write and rollback failed; restore original bytes from the reported private backup.");
    return false;
}
