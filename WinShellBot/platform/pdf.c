#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "document.h"
#include "fpdfview.h"
#include "fpdf_text.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* PDFium is serialized per process, loaded only from the pinned executable-side
   DLL. No JavaScript/form environment is created. Source stays in memory. */
typedef struct {
    void (FPDF_CALLCONV *init)(void);
    FPDF_DOCUMENT (FPDF_CALLCONV *load)(const void*,size_t,FPDF_BYTESTRING);
    void (FPDF_CALLCONV *close)(FPDF_DOCUMENT);
    int (FPDF_CALLCONV *pages)(FPDF_DOCUMENT);
    FPDF_PAGE (FPDF_CALLCONV *page)(FPDF_DOCUMENT,int);
    void (FPDF_CALLCONV *close_page)(FPDF_PAGE);
    float (FPDF_CALLCONV *width)(FPDF_PAGE), (FPDF_CALLCONV *height)(FPDF_PAGE);
    FPDF_TEXTPAGE (FPDF_CALLCONV *text)(FPDF_PAGE);
    int (FPDF_CALLCONV *chars)(FPDF_TEXTPAGE);
    unsigned int (FPDF_CALLCONV *unicode)(FPDF_TEXTPAGE,int);
    void (FPDF_CALLCONV *close_text)(FPDF_TEXTPAGE);
    FPDF_BITMAP (FPDF_CALLCONV *bitmap)(int,int,int);
    FPDF_BOOL (FPDF_CALLCONV *fill)(FPDF_BITMAP,int,int,int,int,FPDF_DWORD);
    void (FPDF_CALLCONV *render)(FPDF_BITMAP,FPDF_PAGE,int,int,int,int,int,int);
    void* (FPDF_CALLCONV *buffer)(FPDF_BITMAP);
    int (FPDF_CALLCONV *stride)(FPDF_BITMAP);
    void (FPDF_CALLCONV *destroy_bitmap)(FPDF_BITMAP);
    unsigned long (FPDF_CALLCONV *error)(void);
} pdf_api;
static SRWLOCK mutex=SRWLOCK_INIT;static pdf_api api;static HMODULE module;static HANDLE dll_pin;

static bool initialize(sb_error *error) {
    if(module)return true;
    wchar_t path[SB_PATH_CAP];DWORD n=GetModuleFileNameW(NULL,path,SB_PATH_CAP);
    if(!n || n>=SB_PATH_CAP)return false;
    wchar_t *slash=wcsrchr(path,L'\\');if(!slash || (size_t)(slash-path)+12>=SB_PATH_CAP)return false;
    wcscpy(slash+1,L"pdfium.dll");
    HANDLE file=CreateFileW(path,GENERIC_READ,FILE_SHARE_READ,NULL,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,NULL);
    BY_HANDLE_FILE_INFORMATION info={0};LARGE_INTEGER size={0};DWORD got=0;unsigned char *bytes=NULL;char hash[65]={0};
    bool ok=file!=INVALID_HANDLE_VALUE && GetFileInformationByHandle(file,&info) &&
        !(info.dwFileAttributes&(FILE_ATTRIBUTE_REPARSE_POINT|FILE_ATTRIBUTE_DIRECTORY)) && info.nNumberOfLinks==1 &&
        GetFileSizeEx(file,&size) && size.QuadPart>0 && size.QuadPart<16*1024*1024;
    if(ok){bytes=malloc((size_t)size.QuadPart);ok=bytes && ReadFile(file,bytes,(DWORD)size.QuadPart,&got,NULL) && got==size.QuadPart && sb_platform_sha256(bytes,got,hash,error);}
    free(bytes);
#ifdef _WIN64
    const char *expected="1f2aee4286b8cf0413ab4562b7121aa454f9cfd9afd6ca8609bcef80927b3f80";
#else
    const char *expected="095f17644e3dc324ac7800d5f1bbdc38bcfabd1f52045617d8050901ae9544b3";
#endif
    ok=ok && !strcmp(hash,expected);
    HMODULE loaded=ok ? LoadLibraryExW(path,NULL,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_SYSTEM32) : NULL;
    ok=loaded!=NULL;
#define PROC(field,name) do { FARPROC p=loaded ? GetProcAddress(loaded,name) : NULL; if(!p)ok=false;memcpy(&api.field,&p,sizeof(p)); } while(0)
    PROC(init,"FPDF_InitLibrary");PROC(load,"FPDF_LoadMemDocument64");PROC(close,"FPDF_CloseDocument");
    PROC(pages,"FPDF_GetPageCount");PROC(page,"FPDF_LoadPage");PROC(close_page,"FPDF_ClosePage");
    PROC(width,"FPDF_GetPageWidthF");PROC(height,"FPDF_GetPageHeightF");PROC(text,"FPDFText_LoadPage");
    PROC(chars,"FPDFText_CountChars");PROC(unicode,"FPDFText_GetUnicode");PROC(close_text,"FPDFText_ClosePage");
    PROC(bitmap,"FPDFBitmap_Create");PROC(fill,"FPDFBitmap_FillRect");PROC(render,"FPDF_RenderPageBitmap");
    PROC(buffer,"FPDFBitmap_GetBuffer");PROC(stride,"FPDFBitmap_GetStride");PROC(destroy_bitmap,"FPDFBitmap_Destroy");PROC(error,"FPDF_GetLastError");
#undef PROC
    if(ok){api.init();module=loaded;dll_pin=file;}
    else {if(loaded)FreeLibrary(loaded);if(file!=INVALID_HANDLE_VALUE)CloseHandle(file);memset(&api,0,sizeof(api));sb_fail(error,"Pinned PDFium 8086 backend unavailable or checksum mismatch; no PDF content processed.");}
    return ok;
}
static bool extracted(FPDF_PAGE page,sb_document *document,sb_error *error) {
    FPDF_TEXTPAGE text=api.text(page);if(!text){sb_fail(error,"PDF text page unavailable.");return false;}
    int count=api.chars(text);bool ok=count>=0;size_t used=0;
    document->text=malloc(SB_DOCUMENT_TEXT_LIMIT+1);
    if(!document->text)ok=false;
    /* Bound parser iteration independently of output (unmapped glyphs count). */
    if(count>(int)SB_DOCUMENT_TEXT_LIMIT){count=(int)SB_DOCUMENT_TEXT_LIMIT;document->extraction_truncated=true;}
    for(int i=0;ok && i<count;++i){unsigned c=api.unicode(text,i);unsigned char value[4];size_t n;
        if(!c)continue;
        if(c>0x10ffff || (c>=0xd800 && c<=0xdfff))c=0xfffd;
        if(c<0x80){value[0]=(unsigned char)c;n=1;}
        else if(c<0x800){value[0]=(unsigned char)(0xc0|(c>>6));value[1]=(unsigned char)(0x80|(c&63));n=2;}
        else if(c<0x10000){value[0]=(unsigned char)(0xe0|(c>>12));value[1]=(unsigned char)(0x80|((c>>6)&63));value[2]=(unsigned char)(0x80|(c&63));n=3;}
        else{value[0]=(unsigned char)(0xf0|(c>>18));value[1]=(unsigned char)(0x80|((c>>12)&63));value[2]=(unsigned char)(0x80|((c>>6)&63));value[3]=(unsigned char)(0x80|(c&63));n=4;}
        if(n>SB_DOCUMENT_TEXT_LIMIT-used){document->extraction_truncated=true;break;}
        memcpy(document->text+used,value,n);used+=n;
    }
    api.close_text(text);if(ok){document->text[used]=0;document->text_size=used;}else sb_fail(error,"PDF text extraction failed.");return ok;
}
bool sb_pdf_document(const unsigned char *bytes,size_t size,const char *operation,unsigned unit,
                     unsigned dimension,yyjson_mut_doc *doc,sb_document *document,sb_error *error) {
    AcquireSRWLockExclusive(&mutex);bool ok=initialize(error);FPDF_DOCUMENT pdf=NULL;FPDF_PAGE page=NULL;
    if(ok){pdf=api.load(bytes,size,NULL);ok=pdf!=NULL;
        if(!ok)snprintf(error->text,sizeof(error->text),"PDF could not be opened (PDFium error %lu); encrypted/password-protected or corrupt documents are not processed.",api.error());}
    int count=ok ? api.pages(pdf) : 0;
    if(ok && (count<1 || count>4096)){ok=false;sb_fail(error,"PDF page count must be within 1..4096.");}
    document->unit_kind="page";document->unit_count=(unsigned)count;
    if(ok && !strcmp(operation,"document_info")){
        document->units=yyjson_mut_arr(doc);
        for(unsigned i=0;i<document->unit_count && i<128;++i){yyjson_mut_val *item=yyjson_mut_obj(doc);
            yyjson_mut_obj_add_uint(doc,item,"unit",i+1);yyjson_mut_arr_add_val(document->units,item);}
    }else if(ok){
        if(unit>document->unit_count){ok=false;sb_fail(error,"PDF page is outside the document.");}
        else {page=api.page(pdf,(int)unit-1);ok=page!=NULL;snprintf(document->unit_name,sizeof(document->unit_name),"Page %u",unit);}
        if(ok && !strcmp(operation,"document_read"))ok=extracted(page,document,error);
        else if(ok){
            double w=api.width(page),h=api.height(page),longest=w>h ? w : h;
            ok=isfinite(w) && isfinite(h) && w>0 && h>0 && w<1000000 && h<1000000;
            unsigned width=ok ? (unsigned)(w/longest*dimension) : 0,height=ok ? (unsigned)(h/longest*dimension) : 0;
            if(!width)width=1;if(!height)height=1;
            FPDF_BITMAP bitmap=ok ? api.bitmap((int)width,(int)height,1) : NULL;ok=bitmap!=NULL;
            if(ok){ok=api.fill(bitmap,0,0,(int)width,(int)height,0xffffffff) && api.stride(bitmap)==(int)width*4;
                if(ok){api.render(bitmap,page,0,0,(int)width,(int)height,0,FPDF_ANNOT);ok=sb_platform_bitmap_png(api.buffer(bitmap),width,height,&document->image,error);}
                api.destroy_bitmap(bitmap);}
        }
    }
    if(page)api.close_page(page);if(pdf)api.close(pdf);ReleaseSRWLockExclusive(&mutex);
    if(!ok && !*error->text)sb_fail(error,"PDF page processing failed; no content returned.");return ok;
}
