/* 显式启动时的 D3D11 Present 前采集；从未附着已有进程，不操作输入或安全桌面。 */
#define COBJMACROS
#include "graphics.h"
#include "detours.h"
#include <d3d11.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <stdio.h>
typedef HRESULT (WINAPI *create_fn)(IDXGIAdapter*,D3D_DRIVER_TYPE,HMODULE,UINT,const D3D_FEATURE_LEVEL*,UINT,UINT,const DXGI_SWAP_CHAIN_DESC*,IDXGISwapChain**,ID3D11Device**,D3D_FEATURE_LEVEL*,ID3D11DeviceContext**);
typedef HRESULT (STDMETHODCALLTYPE *present_fn)(IDXGISwapChain*,UINT,UINT);
static create_fn original_create=D3D11CreateDeviceAndSwapChain;static present_fn original_present;
static IDXGISwapChain *watched;static HWND watched_window;static HANDLE mapping;static sg_frame *shared;
static ID3D11Texture2D *staging;static ID3D11Device *staging_device;static D3D11_TEXTURE2D_DESC staging_desc;
static wchar_t nonce[33];static sg_identity launcher,self;static DWORD capacity=SG_MAX_FRAME_BYTES;static SRWLOCK install_lock=SRWLOCK_INIT;static volatile LONG copying;
static wchar_t status_file[32768];
static void diagnostic(const char *phase,HRESULT hr,DWORD native,HWND window){DWORD previous=GetLastError();if(*status_file){HANDLE file=CreateFileW(status_file,FILE_APPEND_DATA,FILE_SHARE_READ|FILE_SHARE_WRITE,NULL,OPEN_ALWAYS,FILE_ATTRIBUTE_NORMAL,NULL);if(file!=INVALID_HANDLE_VALUE){char text[384];int length=snprintf(text,sizeof(text),"{\"phase\":\"%s\",\"hresult\":%lu,\"win32_error\":%lu,\"hwnd\":%llu,\"pid\":%lu,\"tick_ms\":%llu}\n",phase,(DWORD)hr,native,(unsigned long long)(UINT_PTR)window,GetCurrentProcessId(),GetTickCount64());DWORD written;if(length>0)WriteFile(file,text,(DWORD)length,&written,NULL);CloseHandle(file);}}SetLastError(previous);}
static bool mapping_start(void){
    wchar_t name[128];PSECURITY_DESCRIPTOR descriptor=NULL;if(!sg_named(name,L"Frame",self.pid,nonce) || !sg_private_security(&descriptor))return false;SECURITY_ATTRIBUTES attributes={sizeof(attributes),descriptor,FALSE};
    DWORD bytes=(DWORD)sizeof(sg_frame)+capacity;SetLastError(0);mapping=CreateFileMappingW(INVALID_HANDLE_VALUE,&attributes,PAGE_READWRITE|SEC_RESERVE,0,bytes,name);DWORD code=GetLastError();LocalFree(descriptor);
    if(!mapping || code==ERROR_ALREADY_EXISTS){if(mapping)CloseHandle(mapping);mapping=NULL;return false;}
    shared=MapViewOfFile(mapping,FILE_MAP_WRITE,0,0,bytes);if(!shared || !VirtualAlloc(shared,sizeof(*shared),MEM_COMMIT,PAGE_READWRITE)){if(shared)UnmapViewOfFile(shared);shared=NULL;CloseHandle(mapping);mapping=NULL;return false;}
    memset(shared,0,sizeof(*shared));shared->magic=SG_MAGIC;shared->version=SG_PROTOCOL;shared->header_bytes=sizeof(*shared);shared->capacity=capacity;shared->pid=self.pid;shared->session=self.session;shared->created=self.created;shared->launcher_pid=launcher.pid;shared->launcher_created=launcher.created;shared->copy_hresult=(uint32_t)E_PENDING;shared->present_hresult=E_PENDING;shared->phase=1;return true;
}
static HRESULT copy_frame(IDXGISwapChain *chain){
    ID3D11Device *device=NULL;ID3D11DeviceContext *context=NULL;ID3D11Texture2D *source=NULL;D3D11_TEXTURE2D_DESC desc={0};D3D11_MAPPED_SUBRESOURCE map={0};bool mapped=false;HRESULT hr=E_ACCESSDENIED;
    if(!sg_default_desktop() || !IsWindow(watched_window))goto complete;
    DWORD pid=0;GetWindowThreadProcessId(watched_window,&pid);if(pid!=GetCurrentProcessId())goto complete;
    hr=IDXGISwapChain_GetDevice(chain,&IID_ID3D11Device,(void**)&device);if(SUCCEEDED(hr)){ID3D11Device_GetImmediateContext(device,&context);hr=IDXGISwapChain_GetBuffer(chain,0,&IID_ID3D11Texture2D,(void**)&source);}
    if(SUCCEEDED(hr)){
        ID3D11Texture2D_GetDesc(source,&desc);
        bool format=desc.Format==DXGI_FORMAT_B8G8R8A8_UNORM || desc.Format==DXGI_FORMAT_B8G8R8A8_UNORM_SRGB || desc.Format==DXGI_FORMAT_R8G8B8A8_UNORM || desc.Format==DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
        if(!desc.Width || !desc.Height || desc.Width>SG_MAX_DIMENSION || desc.Height>SG_MAX_DIMENSION || (uint64_t)desc.Width*desc.Height*4>capacity || !format || desc.SampleDesc.Count!=1 || desc.ArraySize!=1 || desc.MipLevels!=1)hr=DXGI_ERROR_UNSUPPORTED;
    }
    if(SUCCEEDED(hr) && (staging_device!=device || staging_desc.Width!=desc.Width || staging_desc.Height!=desc.Height || staging_desc.Format!=desc.Format)){
        if(staging)ID3D11Texture2D_Release(staging);staging=NULL;if(staging_device)ID3D11Device_Release(staging_device);staging_device=device;ID3D11Device_AddRef(device);staging_desc=desc;
    }
    if(SUCCEEDED(hr) && !staging){D3D11_TEXTURE2D_DESC copy=desc;copy.Usage=D3D11_USAGE_STAGING;copy.BindFlags=0;copy.CPUAccessFlags=D3D11_CPU_ACCESS_READ;copy.MiscFlags=0;hr=ID3D11Device_CreateTexture2D(device,&copy,NULL,&staging);}
    if(SUCCEEDED(hr)){ID3D11DeviceContext_CopyResource(context,(ID3D11Resource*)staging,(ID3D11Resource*)source);hr=ID3D11DeviceContext_Map(context,(ID3D11Resource*)staging,0,D3D11_MAP_READ,0,&map);mapped=SUCCEEDED(hr);}
    if(SUCCEEDED(hr) && !VirtualAlloc(shared,sizeof(*shared)+(SIZE_T)desc.Width*desc.Height*4,MEM_COMMIT,PAGE_READWRITE))hr=E_OUTOFMEMORY;
complete:
    InterlockedIncrement(&shared->sequence);shared->phase=SUCCEEDED(hr)?6:7;shared->copy_hresult=(uint32_t)hr;++shared->frame;shared->tick_ms=GetTickCount64();LARGE_INTEGER stamp,frequency;QueryPerformanceCounter(&stamp);QueryPerformanceFrequency(&frequency);shared->qpc=stamp.QuadPart;shared->qpc_frequency=frequency.QuadPart;
    shared->hwnd=(uint64_t)(UINT_PTR)watched_window;shared->root_hwnd=(uint64_t)(UINT_PTR)GetAncestor(watched_window,GA_ROOT);shared->width=desc.Width;shared->height=desc.Height;shared->stride=desc.Width*4;shared->format=87;shared->present_hresult=E_PENDING;shared->present_frame=0;
    if(SUCCEEDED(hr)){
        BYTE *pixels=(BYTE*)(shared+1);bool rgba=desc.Format==DXGI_FORMAT_R8G8B8A8_UNORM || desc.Format==DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
        for(UINT y=0;y<desc.Height;++y){const BYTE *row=(const BYTE*)map.pData+(SIZE_T)y*map.RowPitch;BYTE *out=pixels+(SIZE_T)y*shared->stride;if(!rgba)memcpy(out,row,shared->stride);else for(UINT x=0;x<desc.Width;++x){out[x*4]=row[x*4+2];out[x*4+1]=row[x*4+1];out[x*4+2]=row[x*4];out[x*4+3]=row[x*4+3];}}
    }
    MemoryBarrier();InterlockedIncrement(&shared->sequence);
    if(mapped)ID3D11DeviceContext_Unmap(context,(ID3D11Resource*)staging,0);if(source)ID3D11Texture2D_Release(source);if(context)ID3D11DeviceContext_Release(context);if(device)ID3D11Device_Release(device);return hr;
}
static HRESULT STDMETHODCALLTYPE present(IDXGISwapChain *chain,UINT interval,UINT flags){
    DWORD last=GetLastError();bool copied=false;
    if(chain==watched && shared && !(flags&DXGI_PRESENT_TEST) && InterlockedCompareExchange(&copying,1,0)==0){copy_frame(chain);copied=true;}
    SetLastError(last);HRESULT hr=original_present(chain,interval,flags);
    if(copied){InterlockedIncrement(&shared->sequence);shared->present_hresult=hr;shared->present_frame=shared->frame;MemoryBarrier();InterlockedIncrement(&shared->sequence);InterlockedExchange(&copying,0);}return hr;
}
static HRESULT WINAPI create(IDXGIAdapter *adapter,D3D_DRIVER_TYPE driver,HMODULE software,UINT flags,const D3D_FEATURE_LEVEL *levels,UINT count,UINT sdk,const DXGI_SWAP_CHAIN_DESC *description,IDXGISwapChain **chain,ID3D11Device **device,D3D_FEATURE_LEVEL *level,ID3D11DeviceContext **context){
    InterlockedIncrement(&shared->create_calls);InterlockedExchange(&shared->phase,3);diagnostic("D3D11CreateDeviceAndSwapChain.enter",S_OK,0,description?description->OutputWindow:NULL);
    HRESULT hr=original_create(adapter,driver,software,flags,levels,count,sdk,description,chain,device,level,context);
    shared->diagnostic_hresult=(uint32_t)hr;diagnostic("D3D11CreateDeviceAndSwapChain.return",hr,0,description?description->OutputWindow:NULL);
    if(SUCCEEDED(hr) && chain && *chain && description && description->OutputWindow && sg_default_desktop()){
        AcquireSRWLockExclusive(&install_lock);
        DWORD pid=0;GetWindowThreadProcessId(description->OutputWindow,&pid);
        if(!original_present && pid==GetCurrentProcessId()){
            void **table=*(void***)(*chain);memcpy(&original_present,&table[8],sizeof(original_present));present_fn function=present;PVOID hook=NULL;memcpy(&hook,&function,sizeof(hook));watched=*chain;watched_window=description->OutputWindow;
            LONG result=DetourTransactionBegin();if(result==NO_ERROR)result=DetourUpdateThread(GetCurrentThread());if(result==NO_ERROR)result=DetourAttach((PVOID*)&original_present,hook);if(result==NO_ERROR)result=DetourTransactionCommit();else DetourTransactionAbort();
            diagnostic("Present.hook_install",HRESULT_FROM_WIN32(result),(DWORD)result,watched_window);InterlockedExchange(&shared->hook_error,result);InterlockedExchange(&shared->phase,result==NO_ERROR?5:7);
            if(result!=NO_ERROR){original_present=NULL;watched=NULL;InterlockedIncrement(&shared->sequence);shared->copy_hresult=HRESULT_FROM_WIN32(result);shared->closed=1;InterlockedIncrement(&shared->sequence);}
        }
        ReleaseSRWLockExclusive(&install_lock);
    }return hr;
}
__declspec(dllexport) void WINAPI ShellBotGraphicsOrdinal(void){}
BOOL WINAPI DllMain(HINSTANCE instance,DWORD reason,LPVOID reserved){
    (void)reserved;
    if(reason==DLL_PROCESS_ATTACH){
        /* 避免 D3D 的小栈线程在 DLL_THREAD_ATTACH 时预留长路径数组。 */
        static wchar_t image[32768],expected[32768],owner_path[32768],module[32768];wchar_t value[32],*end=NULL;DWORD size=32768;
        GetEnvironmentVariableW(L"SHELLBOT_GRAPHICS_STATUS",status_file,32768);diagnostic("DLL.attach",S_OK,0,NULL);
        if(DetourIsHelperProcess() || !sg_default_desktop() || !sg_identity_read(GetCurrentProcess(),&self,true)){diagnostic("DLL.account_or_desktop_rejected",E_ACCESSDENIED,0,NULL);return FALSE;}
        DWORD n=GetModuleFileNameW(NULL,image,32768);if(!n || n>=32768 || GetEnvironmentVariableW(L"SHELLBOT_GRAPHICS_TARGET",expected,32768)!=n || _wcsicmp(image,expected) || GetEnvironmentVariableW(L"SHELLBOT_GRAPHICS_NONCE",nonce,33)!=32 || !sg_valid_nonce(nonce))return FALSE;
        if(!GetEnvironmentVariableW(L"SHELLBOT_GRAPHICS_OWNER_PID",value,32))return FALSE;launcher.pid=wcstoul(value,&end,10);if(!launcher.pid || !end || *end)return FALSE;
        if(!GetEnvironmentVariableW(L"SHELLBOT_GRAPHICS_OWNER_CREATED",value,32))return FALSE;launcher.created=_wcstoui64(value,&end,10);if(!launcher.created || !end || *end)return FALSE;
        if(!GetEnvironmentVariableW(L"SHELLBOT_GRAPHICS_CAPACITY",value,32))return FALSE;unsigned long limit=wcstoul(value,&end,10);if(!end || *end || limit<4 || limit>SG_MAX_FRAME_BYTES)return FALSE;capacity=limit;
        HANDLE owner=OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION|SYNCHRONIZE,FALSE,launcher.pid);sg_identity actual={0};bool ok=owner && sg_identity_read(owner,&actual,true) && actual.created==launcher.created && QueryFullProcessImageNameW(owner,0,owner_path,&size) && WaitForSingleObject(owner,0)==WAIT_TIMEOUT;
        if(owner)CloseHandle(owner);if(!ok)return FALSE;launcher=actual;
        if(!GetModuleFileNameW(instance,module,32768))return FALSE;wchar_t *slash=wcsrchr(module,L'\\');if(!slash || wcscpy_s(slash+1,32768-(size_t)(slash+1-module),L"ShellBotGraphics.exe") || _wcsicmp(module,owner_path))return FALSE;
        if(!mapping_start()){DWORD code=GetLastError();diagnostic("DLL.mapping_failed",HRESULT_FROM_WIN32(code),code,NULL);return FALSE;}
        if(!DetourRestoreAfterWith()){diagnostic("DLL.restore_imports_failed",E_FAIL,GetLastError(),NULL);return FALSE;}
        /* Debug 的增量链接可在函数符号前再加 ILT 跳板，必须拦真正的系统导出。 */
        HMODULE d3d=GetModuleHandleW(L"d3d11.dll");FARPROC exported=d3d?GetProcAddress(d3d,"D3D11CreateDeviceAndSwapChain"):NULL;if(!exported){diagnostic("DLL.d3d11_export_unavailable",E_FAIL,GetLastError(),NULL);return FALSE;}PVOID symbol=NULL,address=NULL;memcpy(&symbol,&original_create,sizeof(symbol));memcpy(&address,&exported,sizeof(address));if(symbol!=address)diagnostic("DLL.create_symbol_not_export",S_FALSE,0,NULL);if(DetourCodeFromPointer(symbol,NULL)!=address)diagnostic("DLL.detours_symbol_resolution_not_export",S_FALSE,0,NULL);memcpy(&original_create,&exported,sizeof(original_create));
        create_fn function=create;PVOID hook=NULL;memcpy(&hook,&function,sizeof(hook));LONG result=DetourTransactionBegin();if(result==NO_ERROR)result=DetourUpdateThread(GetCurrentThread());if(result==NO_ERROR)result=DetourAttach((PVOID*)&original_create,hook);if(result==NO_ERROR)result=DetourTransactionCommit();else DetourTransactionAbort();InterlockedExchange(&shared->hook_error,result);InterlockedExchange(&shared->phase,result==NO_ERROR?2:7);diagnostic("DLL.create_hook_install",HRESULT_FROM_WIN32(result),(DWORD)result,NULL);return result==NO_ERROR;
    }
    if(reason==DLL_PROCESS_DETACH && shared){InterlockedIncrement(&shared->sequence);shared->closed=1;MemoryBarrier();InterlockedIncrement(&shared->sequence);}return TRUE;
}
