#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#include <windows.h>
#include <roapi.h>
#include <winstring.h>
#include <d3d11.h>
#include <dxgi.h>
#include <dwmapi.h>
#include <windows.graphics.capture.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <wincodec.h>
#include "window_capture.h"
#include "uia.h"
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

typedef __x_ABI_CWindows_CGraphics_CCapture_CIGraphicsCaptureItem capture_item;
typedef __x_ABI_CWindows_CGraphics_CCapture_CIDirect3D11CaptureFramePoolStatics2 pool_factory;
typedef __x_ABI_CWindows_CGraphics_CCapture_CIDirect3D11CaptureFramePool frame_pool;
typedef __x_ABI_CWindows_CGraphics_CCapture_CIDirect3D11CaptureFrame capture_frame;
typedef __x_ABI_CWindows_CGraphics_CCapture_CIGraphicsCaptureSession capture_session;
typedef __x_ABI_CWindows_CGraphics_CCapture_CIGraphicsCaptureSession2 capture_session2;
typedef __x_ABI_CWindows_CGraphics_CDirectX_CDirect3D11_CIDirect3DDevice winrt_device;
typedef __x_ABI_CWindows_CGraphics_CDirectX_CDirect3D11_CIDirect3DSurface winrt_surface;
typedef struct __x_ABI_CWindows_CGraphics_CSizeInt32 capture_size;
/* SDK 的两个 interop 接口只有 C++ 声明；这里逐项声明文档规定的 IUnknown ABI。 */
typedef struct item_interop item_interop;
typedef struct {HRESULT (STDMETHODCALLTYPE *QueryInterface)(item_interop*,REFIID,void**);ULONG (STDMETHODCALLTYPE *AddRef)(item_interop*);ULONG (STDMETHODCALLTYPE *Release)(item_interop*);HRESULT (STDMETHODCALLTYPE *CreateForWindow)(item_interop*,HWND,REFIID,void**);HRESULT (STDMETHODCALLTYPE *CreateForMonitor)(item_interop*,HMONITOR,REFIID,void**);} item_interop_vtbl;
struct item_interop {const item_interop_vtbl *lpVtbl;};
typedef struct dxgi_access dxgi_access;
typedef struct {HRESULT (STDMETHODCALLTYPE *QueryInterface)(dxgi_access*,REFIID,void**);ULONG (STDMETHODCALLTYPE *AddRef)(dxgi_access*);ULONG (STDMETHODCALLTYPE *Release)(dxgi_access*);HRESULT (STDMETHODCALLTYPE *GetInterface)(dxgi_access*,REFIID,void**);} dxgi_access_vtbl;
struct dxgi_access {const dxgi_access_vtbl *lpVtbl;};
static const IID item_interop_id={0x3628e81b,0x3cac,0x4c60,{0xb7,0xf4,0x23,0xce,0x0e,0x0c,0x33,0x56}};
static const IID item_id={0x79c3f95b,0x31f7,0x4ec2,{0xa4,0x64,0x63,0x2e,0xf5,0xd3,0x07,0x60}};
static const IID pool_factory_id={0x589b103f,0x6bbc,0x5df5,{0xa9,0x91,0x02,0xe2,0x8b,0x3b,0x66,0xd5}};
static const IID session2_id={0x2c39ae40,0x7d2e,0x5044,{0x80,0x4e,0x8b,0x67,0x99,0xd4,0xcf,0x9e}};
static const IID dxgi_access_id={0xa9b3d012,0x3df2,0x4ee3,{0xb8,0xd1,0x86,0x95,0xf4,0x57,0xd3,0xc1}};
static const IID closable_id={0x30d5a829,0x7fa4,0x4026,{0x83,0xbb,0xd7,0x5b,0xae,0x4e,0xa9,0x9e}};
#define RELEASE(p) do{if(p)IUnknown_Release((IUnknown*)(p));}while(0)
typedef struct {HWND window;DWORD pid;FILETIME created;LONG epoch;uint64_t deadline;unsigned maximum;const char *backend,*stage,*error;HRESULT hr;DWORD native;bool captured,cursor_disabled,region_known,background;DWORD signature;RECT region;unsigned width,height;uint64_t png_bytes;bool all_black,uniform;const char *device;INT64 frame_time;} capture_context;
static bool permitted(capture_context *c){return !IsIconic(c->window) && sb_win_application_guard(c->window,c->pid,c->created,c->epoch,c->deadline,false,false,c->background,c->signature);}
static bool dimensions(unsigned w,unsigned h){return w && h && w<=32768 && h<=32768 && (uint64_t)w*h<=SB_IMAGE_PIXEL_LIMIT;}
static HRESULT factory(const wchar_t *name,REFIID iid,void **out){HSTRING text=NULL;HRESULT hr=WindowsCreateString(name,(UINT32)wcslen(name),&text);if(SUCCEEDED(hr))hr=RoGetActivationFactory(text,iid,out);if(text)WindowsDeleteString(text);return hr;}
static void close_object(void *value){__x_ABI_CWindows_CFoundation_CIClosable *close=NULL;if(value && SUCCEEDED(IUnknown_QueryInterface((IUnknown*)value,&closable_id,(void**)&close))){close->lpVtbl->Close(close);RELEASE(close);}}
static void rectangle(yyjson_mut_doc *doc,yyjson_mut_val *parent,const char *key,RECT r){yyjson_mut_val *v=yyjson_mut_obj(doc);yyjson_mut_obj_add_int(doc,v,"x",r.left);yyjson_mut_obj_add_int(doc,v,"y",r.top);yyjson_mut_obj_add_int(doc,v,"width",(int64_t)r.right-r.left);yyjson_mut_obj_add_int(doc,v,"height",(int64_t)r.bottom-r.top);yyjson_mut_obj_add_val(doc,parent,key,v);}
static char *encode_pixels(capture_context *c,const BYTE *pixels,unsigned stride,unsigned w,unsigned h){
    IWICImagingFactory *wic=NULL;IWICBitmap *bitmap=NULL;IWICBitmapScaler *scaler=NULL;IWICFormatConverter *converter=NULL;IWICBitmapEncoder *encoder=NULL;IWICBitmapFrameEncode *frame=NULL;IPropertyBag2 *properties=NULL;IStream *stream=NULL;char *png=NULL;
    c->captured=true;c->all_black=true;c->uniform=true;BYTE first[3]={pixels[0],pixels[1],pixels[2]};
    for(unsigned y=0;y<h;++y){const BYTE *row=pixels+(size_t)y*stride;for(unsigned x=0;x<w;++x){const BYTE *p=row+x*4;if(p[0] || p[1] || p[2])c->all_black=false;if(memcmp(first,p,3))c->uniform=false;}}
    double scale=(w>h?w:h)/(double)c->maximum;if(scale<1)scale=1;c->width=(unsigned)(w/scale);c->height=(unsigned)(h/scale);if(!c->width)c->width=1;if(!c->height)c->height=1;
    c->stage="WIC.factory";HRESULT hr=CoCreateInstance(&CLSID_WICImagingFactory,NULL,CLSCTX_INPROC_SERVER,&IID_IWICImagingFactory,(void**)&wic);
#define STEP(label,expression) if(SUCCEEDED(hr)){c->stage=label;hr=(expression);}
    STEP("WIC.bitmap",IWICImagingFactory_CreateBitmapFromMemory(wic,w,h,&GUID_WICPixelFormat32bppBGRA,stride,stride*h,(BYTE*)pixels,&bitmap));
    IWICBitmapSource *source=(IWICBitmapSource*)bitmap;
    if(SUCCEEDED(hr) && (w!=c->width || h!=c->height)){STEP("WIC.scaler",IWICImagingFactory_CreateBitmapScaler(wic,&scaler));STEP("WIC.scale",IWICBitmapScaler_Initialize(scaler,source,c->width,c->height,WICBitmapInterpolationModeFant));source=(IWICBitmapSource*)scaler;}
    STEP("WIC.converter",IWICImagingFactory_CreateFormatConverter(wic,&converter));
    STEP("WIC.BGR",IWICFormatConverter_Initialize(converter,source,&GUID_WICPixelFormat24bppBGR,WICBitmapDitherTypeNone,NULL,0,WICBitmapPaletteTypeCustom));
    STEP("WIC.stream",CreateStreamOnHGlobal(NULL,TRUE,&stream));STEP("WIC.encoder",IWICImagingFactory_CreateEncoder(wic,&GUID_ContainerFormatPng,NULL,&encoder));
    STEP("WIC.initialize",IWICBitmapEncoder_Initialize(encoder,stream,WICBitmapEncoderNoCache));STEP("WIC.frame",IWICBitmapEncoder_CreateNewFrame(encoder,&frame,&properties));STEP("WIC.frame_initialize",IWICBitmapFrameEncode_Initialize(frame,properties));
    STEP("WIC.size",IWICBitmapFrameEncode_SetSize(frame,c->width,c->height));WICPixelFormatGUID format=GUID_WICPixelFormat24bppBGR;
    STEP("WIC.format",IWICBitmapFrameEncode_SetPixelFormat(frame,&format));if(SUCCEEDED(hr) && !IsEqualGUID(&format,&GUID_WICPixelFormat24bppBGR))hr=E_FAIL;
    STEP("WIC.pixels",IWICBitmapFrameEncode_WriteSource(frame,(IWICBitmapSource*)converter,NULL));STEP("WIC.commit_frame",IWICBitmapFrameEncode_Commit(frame));STEP("WIC.commit",IWICBitmapEncoder_Commit(encoder));
    STATSTG stat={0};HGLOBAL buffer=NULL;STEP("WIC.encoded_size",IStream_Stat(stream,&stat,STATFLAG_NONAME));
    if(SUCCEEDED(hr)){c->png_bytes=stat.cbSize.QuadPart;if(!c->png_bytes || c->png_bytes>SB_IMAGE_LIMIT){c->error="png_budget_exceeded";hr=HRESULT_FROM_WIN32(ERROR_FILE_TOO_LARGE);}}
    STEP("WIC.buffer",GetHGlobalFromStream(stream,&buffer));
    if(SUCCEEDED(hr)){void *data=GlobalLock(buffer);if(data){png=sb_base64(data,(size_t)c->png_bytes);GlobalUnlock(buffer);}if(!png)hr=E_OUTOFMEMORY;}
    if(FAILED(hr))c->hr=hr;
#undef STEP
    RELEASE(properties);RELEASE(frame);RELEASE(encoder);RELEASE(stream);RELEASE(converter);RELEASE(scaler);RELEASE(bitmap);RELEASE(wic);return png;
}
static char *print_window(capture_context *c){
    RECT physical={0},r={0};c->stage="PrintWindow.bounds";if(!GetWindowRect(c->window,&physical)){c->hr=E_INVALIDARG;return NULL;}
    /* 老式应用按自身 DPI 单位处理 WM_PRINT；使用同一上下文取得尺寸，避免物理大画布上只画出左上角。 */
    c->stage="PrintWindow.dpi_context";DPI_AWARENESS_CONTEXT target=GetWindowDpiAwarenessContext(c->window),previous=target?SetThreadDpiAwarenessContext(target):NULL;if(!previous){c->hr=E_FAIL;return NULL;}
    if(!GetWindowRect(c->window,&r) || !dimensions((unsigned)(r.right-r.left),(unsigned)(r.bottom-r.top))){SetThreadDpiAwarenessContext(previous);c->hr=E_INVALIDARG;return NULL;}
    unsigned w=(unsigned)(r.right-r.left),h=(unsigned)(r.bottom-r.top);BITMAPINFO info={0};info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);info.bmiHeader.biWidth=(LONG)w;info.bmiHeader.biHeight=-(LONG)h;info.bmiHeader.biPlanes=1;info.bmiHeader.biBitCount=32;
    c->stage="PrintWindow.memory_bitmap";HDC dc=CreateCompatibleDC(NULL);BYTE *pixels=NULL;HBITMAP bitmap=dc?CreateDIBSection(dc,&info,DIB_RGB_COLORS,(void**)&pixels,NULL,0):NULL;HGDIOBJ old=bitmap?SelectObject(dc,bitmap):NULL;char *png=NULL;
    if(old && old!=HGDI_ERROR && permitted(c)){memset(pixels,0,(size_t)w*h*4);c->stage="PrintWindow";SetLastError(0);BOOL ok=PrintWindow(c->window,dc,2u);c->native=ok?0:GetLastError();c->hr=ok?S_OK:E_FAIL;
        if(ok){c->stage="PrintWindow.GdiFlush";if(!GdiFlush()){c->native=GetLastError();c->hr=E_FAIL;}else if(permitted(c)){c->region=physical;c->region_known=true;c->cursor_disabled=true;png=encode_pixels(c,pixels,w*4,w,h);}else c->hr=E_ABORT;}
    }else c->hr=HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED);
    if(old && old!=HGDI_ERROR)SelectObject(dc,old);if(bitmap)DeleteObject(bitmap);if(dc)DeleteDC(dc);SetThreadDpiAwarenessContext(previous);return png;
}
static char *wgc_window(capture_context *c){
    item_interop *interop=NULL;capture_item *item=NULL;pool_factory *statics=NULL;frame_pool *pool=NULL;capture_session *session=NULL;capture_session2 *cursor=NULL;capture_frame *frame=NULL;
    ID3D11Device *device=NULL;ID3D11DeviceContext *commands=NULL;IDXGIDevice *dxgi=NULL;IInspectable *runtime=NULL;winrt_surface *surface=NULL;dxgi_access *access=NULL;ID3D11Texture2D *texture=NULL,*staging=NULL;char *png=NULL;bool mapped=false;
    HRESULT hr=S_OK;capture_size size={0};D3D11_MAPPED_SUBRESOURCE map={0};
#define STEP(label,expression) if(SUCCEEDED(hr)){c->stage=label;hr=(expression);}
    STEP("WGC.item_factory",factory(L"Windows.Graphics.Capture.GraphicsCaptureItem",&item_interop_id,(void**)&interop));
    STEP("WGC.CreateForWindow",interop->lpVtbl->CreateForWindow(interop,c->window,&item_id,(void**)&item));STEP("WGC.item_size",item->lpVtbl->get_Size(item,&size));
    if(SUCCEEDED(hr) && !dimensions((unsigned)size.Width,(unsigned)size.Height))hr=E_INVALIDARG;
    if(SUCCEEDED(hr)){c->stage="D3D11.hardware";hr=D3D11CreateDevice(NULL,D3D_DRIVER_TYPE_HARDWARE,NULL,D3D11_CREATE_DEVICE_BGRA_SUPPORT,NULL,0,D3D11_SDK_VERSION,&device,NULL,&commands);c->device="hardware";
        if(FAILED(hr)){RELEASE(commands);commands=NULL;RELEASE(device);device=NULL;c->stage="D3D11.WARP";hr=D3D11CreateDevice(NULL,D3D_DRIVER_TYPE_WARP,NULL,D3D11_CREATE_DEVICE_BGRA_SUPPORT,NULL,0,D3D11_SDK_VERSION,&device,NULL,&commands);c->device="warp";}}
    STEP("D3D11.DXGI",ID3D11Device_QueryInterface(device,&IID_IDXGIDevice,(void**)&dxgi));STEP("D3D11.WinRT",CreateDirect3D11DeviceFromDXGIDevice(dxgi,&runtime));
    STEP("WGC.pool_factory",factory(L"Windows.Graphics.Capture.Direct3D11CaptureFramePool",&pool_factory_id,(void**)&statics));
    STEP("WGC.free_threaded_pool",statics->lpVtbl->CreateFreeThreaded(statics,(winrt_device*)runtime,(enum __x_ABI_CWindows_CGraphics_CDirectX_CDirectXPixelFormat)87,2,size,&pool));
    STEP("WGC.session",pool->lpVtbl->CreateCaptureSession(pool,item,&session));
    if(SUCCEEDED(hr) && SUCCEEDED(session->lpVtbl->QueryInterface(session,&session2_id,(void**)&cursor)))c->cursor_disabled=SUCCEEDED(cursor->lpVtbl->put_IsCursorCaptureEnabled(cursor,FALSE));
    if(SUCCEEDED(hr) && !permitted(c))hr=E_ACCESSDENIED;
    STEP("WGC.StartCapture",session->lpVtbl->StartCapture(session));
    if(SUCCEEDED(hr)){c->stage="WGC.wait_frame";uint64_t until=sb_platform_millis()+1200;if(until>c->deadline)until=c->deadline;
        while(sb_platform_millis()<until && permitted(c)){hr=pool->lpVtbl->TryGetNextFrame(pool,&frame);if(FAILED(hr) || frame)break;Sleep(5);}if(SUCCEEDED(hr) && !frame){c->error="wgc_no_frame";hr=HRESULT_FROM_WIN32(WAIT_TIMEOUT);}}
    capture_size actual={0};STEP("WGC.frame_size",frame->lpVtbl->get_ContentSize(frame,&actual));
    if(SUCCEEDED(hr) && (!dimensions((unsigned)actual.Width,(unsigned)actual.Height) || actual.Width!=size.Width || actual.Height!=size.Height)){c->error="window_content_size_changed";hr=E_ABORT;}
    if(SUCCEEDED(hr)){struct __x_ABI_CWindows_CFoundation_CTimeSpan stamp={0};if(SUCCEEDED(frame->lpVtbl->get_SystemRelativeTime(frame,&stamp)))c->frame_time=stamp.Duration;}
    STEP("WGC.surface",frame->lpVtbl->get_Surface(frame,&surface));STEP("WGC.DXGI_surface",surface->lpVtbl->QueryInterface(surface,&dxgi_access_id,(void**)&access));STEP("WGC.texture",access->lpVtbl->GetInterface(access,&IID_ID3D11Texture2D,(void**)&texture));
    D3D11_TEXTURE2D_DESC desc={0};if(SUCCEEDED(hr)){ID3D11Texture2D_GetDesc(texture,&desc);if(desc.Width<(UINT)actual.Width || desc.Height<(UINT)actual.Height || desc.Format!=DXGI_FORMAT_B8G8R8A8_UNORM)hr=E_INVALIDARG;desc.Usage=D3D11_USAGE_STAGING;desc.BindFlags=0;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;desc.MiscFlags=0;}
    STEP("D3D11.staging",ID3D11Device_CreateTexture2D(device,&desc,NULL,&staging));
    if(SUCCEEDED(hr)){ID3D11DeviceContext_CopyResource(commands,(ID3D11Resource*)staging,(ID3D11Resource*)texture);c->stage="D3D11.map";hr=ID3D11DeviceContext_Map(commands,(ID3D11Resource*)staging,0,D3D11_MAP_READ,0,&map);mapped=SUCCEEDED(hr);}
    if(SUCCEEDED(hr) && permitted(c)){RECT bounds={0};if(SUCCEEDED(DwmGetWindowAttribute(c->window,DWMWA_EXTENDED_FRAME_BOUNDS,&bounds,sizeof(bounds))) && bounds.right-bounds.left==actual.Width && bounds.bottom-bounds.top==actual.Height){c->region=bounds;c->region_known=true;}png=encode_pixels(c,map.pData,map.RowPitch,(unsigned)actual.Width,(unsigned)actual.Height);}
    if(FAILED(hr))c->hr=hr;
#undef STEP
    if(mapped)ID3D11DeviceContext_Unmap(commands,(ID3D11Resource*)staging,0);RELEASE(staging);RELEASE(texture);RELEASE(access);RELEASE(surface);close_object(frame);RELEASE(frame);close_object(session);RELEASE(cursor);RELEASE(session);close_object(pool);RELEASE(pool);RELEASE(statics);RELEASE(runtime);RELEASE(dxgi);RELEASE(commands);RELEASE(device);RELEASE(item);RELEASE(interop);return png;
}
static char *capture_request(yyjson_val *args){
    capture_context c={0};c.background=yyjson_get_bool(yyjson_obj_get(args,"application_background"));c.signature=(DWORD)yyjson_get_uint(yyjson_obj_get(args,"session_signature"));c.backend=sb_string(yyjson_obj_get(args,"backend"));c.stage="validate_request";c.hr=E_INVALIDARG;uint64_t hwnd=0,pid=0,low=0,high=0,maximum=0,deadline=0;
    bool valid=c.backend && (!strcmp(c.backend,"wgc") || !strcmp(c.backend,"printwindow")) && sb_integer(yyjson_obj_get(args,"hwnd"),&hwnd) && hwnd && hwnd<=(uint64_t)UINTPTR_MAX && sb_integer(yyjson_obj_get(args,"pid"),&pid) && pid && pid<=MAXDWORD && sb_integer(yyjson_obj_get(args,"created_low"),&low) && low<=MAXDWORD && sb_integer(yyjson_obj_get(args,"created_high"),&high) && high<=MAXDWORD && sb_integer(yyjson_obj_get(args,"max_dimension"),&maximum) && maximum>=200 && maximum<=SB_IMAGE_DIMENSION && sb_integer(yyjson_obj_get(args,"deadline"),&deadline) && deadline>sb_platform_millis() && deadline-sb_platform_millis()<=5000;
    c.window=(HWND)(uintptr_t)hwnd;c.pid=(DWORD)pid;c.created.dwLowDateTime=(DWORD)low;c.created.dwHighDateTime=(DWORD)high;c.maximum=(unsigned)maximum;c.deadline=deadline;char *png=NULL;
    if(valid && permitted(&c)){c.hr=S_OK;png=!strcmp(c.backend,"wgc")?wgc_window(&c):print_window(&c);}else c.error="window_capture_guard_unavailable";
    if(png && !permitted(&c)){free(png);png=NULL;c.error="window_capture_guard_changed";c.hr=E_ABORT;}
    yyjson_mut_doc *doc=yyjson_mut_doc_new(NULL);if(!doc){free(png);return NULL;}yyjson_mut_val *r=yyjson_mut_obj(doc);
    yyjson_mut_obj_add_str(doc,r,"backend",c.backend?c.backend:"invalid");yyjson_mut_obj_add_str(doc,r,"stage",png?"complete":c.stage);yyjson_mut_obj_add_uint(doc,r,"hresult",(uint32_t)c.hr);yyjson_mut_obj_add_uint(doc,r,"win32_error",c.native);
    yyjson_mut_obj_add_bool(doc,r,"pixels_captured",c.captured);yyjson_mut_obj_add_bool(doc,r,"cursor_capture_disabled",c.cursor_disabled);yyjson_mut_obj_add_bool(doc,r,"alpha_preserved",false);yyjson_mut_obj_add_bool(doc,r,"all_black",c.all_black);yyjson_mut_obj_add_bool(doc,r,"uniform",c.uniform);yyjson_mut_obj_add_bool(doc,r,"application_paint_verified",false);
    yyjson_mut_obj_add_uint(doc,r,"width",c.width);yyjson_mut_obj_add_uint(doc,r,"height",c.height);yyjson_mut_obj_add_uint(doc,r,"encoded_bytes",c.png_bytes);yyjson_mut_obj_add_bool(doc,r,"physical_mapping_known",c.region_known);
    if(c.region_known)rectangle(doc,r,"physical_region",c.region);else yyjson_mut_obj_add_null(doc,r,"physical_region");
    if(c.device)yyjson_mut_obj_add_str(doc,r,"render_device",c.device);if(c.frame_time)yyjson_mut_obj_add_sint(doc,r,"frame_system_relative_100ns",c.frame_time);
    if(png)yyjson_mut_obj_add_strcpy(doc,r,"png_base64",png);else yyjson_mut_obj_add_str(doc,r,"error",c.error?c.error:c.captured?"window_png_encoding_failed":"window_capture_failed");
    char *wire=yyjson_mut_val_write(r,0,NULL);free(png);yyjson_mut_doc_free(doc);return wire;
}
int sb_win_capture_worker(void){
    if(!SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2))return 2;
    HRESULT hr=RoInitialize(RO_INIT_MULTITHREADED);if(FAILED(hr))return 2;
    char input[8192];DWORD got=0;size_t used=0;HANDLE in=GetStdHandle(STD_INPUT_HANDLE);while(used<sizeof(input)-1 && ReadFile(in,input+used,(DWORD)(sizeof(input)-1-used),&got,NULL) && got)used+=got;
    yyjson_doc *doc=used && used<sizeof(input)-1?yyjson_read(input,used,0):NULL;char *wire=doc?capture_request(yyjson_doc_get_root(doc)):NULL;yyjson_doc_free(doc);bool ok=false;
    if(wire){DWORD written=0;size_t length=strlen(wire);ok=length<=SB_RESPONSE_LIMIT && WriteFile(GetStdHandle(STD_OUTPUT_HANDLE),wire,(DWORD)length,&written,NULL) && written==length;free(wire);}RoUninitialize();return ok?0:2;
}
