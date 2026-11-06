/* 独立显式启动采集协议，不授予普通桌面租约注入能力。 */
#ifndef SHELLBOT_GRAPHICS_H
#define SHELLBOT_GRAPHICS_H
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <stdbool.h>
#define SG_MAGIC 0x31474753u
#define SG_PROTOCOL 1u
#define SG_MAX_FRAME_BYTES (256u*1024u*1024u)
#define SG_MAX_DIMENSION 16384u
typedef struct {DWORD pid,session,integrity;uint64_t created;} sg_identity;
typedef struct {
    uint32_t magic,version,header_bytes,capacity,pid,session;
    uint64_t created,launcher_created;
    uint32_t launcher_pid,reserved;
    volatile LONG phase,create_calls,hook_error;
    uint32_t diagnostic_hresult;
    volatile LONG sequence,closed;
    uint64_t frame,tick_ms,qpc,qpc_frequency,hwnd,root_hwnd;
    uint32_t width,height,stride,copy_hresult;
    volatile LONG present_hresult;
    uint32_t format;
    uint64_t present_frame;
} sg_frame;
bool sg_identity_read(HANDLE process,sg_identity *value,bool normal);
bool sg_same_user(HANDLE process);
bool sg_same_identity(HANDLE process,const sg_identity *expected,bool normal);
bool sg_default_desktop(void);
bool sg_private_security(PSECURITY_DESCRIPTOR *descriptor);
bool sg_private_file(HANDLE file);
bool sg_nonce(wchar_t value[33]);
bool sg_valid_nonce(const wchar_t *value);
bool sg_named(wchar_t out[128],const wchar_t *kind,DWORD pid,const wchar_t *nonce);
#endif
