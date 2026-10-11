#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <objbase.h>
#include "document.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool text_preview(sb_document *document,unsigned dimension,sb_error *error) {
    unsigned height=dimension,width=dimension*3/4;
    BITMAPINFO info={0};info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth=(LONG)width;info.bmiHeader.biHeight=-(LONG)height;
    info.bmiHeader.biPlanes=1;info.bmiHeader.biBitCount=32;info.bmiHeader.biCompression=BI_RGB;
    HDC dc=CreateCompatibleDC(NULL);void *pixels=NULL;
    HBITMAP bitmap=dc ? CreateDIBSection(dc,&info,DIB_RGB_COLORS,&pixels,NULL,0) : NULL;
    if(!bitmap){if(dc)DeleteDC(dc);sb_fail(error,"Office text preview raster unavailable.");return false;}
    HGDIOBJ old=SelectObject(dc,bitmap);RECT rect={0,0,(LONG)width,(LONG)height};
    FillRect(dc,&rect,(HBRUSH)GetStockObject(WHITE_BRUSH));SetBkMode(dc,TRANSPARENT);SetTextColor(dc,RGB(20,30,40));
    int font_size=(int)dimension/65;if(font_size<8)font_size=8;
    HFONT font=CreateFontW(-font_size,0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,
                          CLIP_DEFAULT_PRECIS,ANTIALIASED_QUALITY,DEFAULT_PITCH,L"Microsoft YaHei");
    HGDIOBJ old_font=font ? SelectObject(dc,font) : NULL;
    int margin=(int)width/30;rect.left=margin;rect.right-=margin;rect.top=margin;
    const wchar_t title[]=L"Office content preview / 内容预览（非原始排版）";
    DrawTextW(dc,title,-1,&rect,DT_TOP|DT_SINGLELINE|DT_NOPREFIX);
    rect.top+=font_size*3;rect.bottom-=margin;
    size_t preview_size=document->text_size;if(preview_size>16384){preview_size=16384;document->extraction_truncated=true;}
    while(preview_size<document->text_size && preview_size && ((unsigned char)document->text[preview_size]&0xc0)==0x80)--preview_size;
    int count=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,document->text,(int)preview_size,NULL,0);
    wchar_t *text=count>=0 ? calloc((size_t)count+1,sizeof(wchar_t)) : NULL;
    bool ok=text!=NULL;
    if(ok && count)ok=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,document->text,(int)preview_size,text,count)==count;
    if(ok){RECT measured=rect;int needed=DrawTextW(dc,text,count,&measured,DT_TOP|DT_WORDBREAK|DT_EXPANDTABS|DT_NOPREFIX|DT_CALCRECT);
        if(needed>rect.bottom-rect.top)document->extraction_truncated=true;
        DrawTextW(dc,text,count,&rect,DT_TOP|DT_WORDBREAK|DT_EXPANDTABS|DT_NOPREFIX);
        GdiFlush();unsigned char *bytes=pixels;for(size_t i=0;i<(size_t)width*height;++i)bytes[i*4+3]=255;
        ok=sb_platform_bitmap_png(bytes,width,height,&document->image,error);
    }
    free(text);if(old_font)SelectObject(dc,old_font);if(font)DeleteObject(font);SelectObject(dc,old);DeleteObject(bitmap);DeleteDC(dc);
    if(!ok && !*error->text)sb_fail(error,"Office text preview failed.");return ok;
}
bool sb_platform_document(const unsigned char *bytes,size_t size,const char *format,const char *operation,
                           unsigned unit,unsigned max_dimension,yyjson_mut_doc *doc,sb_document *document,sb_error *error) {
    document->format=format;
    bool pdf=!strcmp(format,"pdf");
    if((pdf && (size<8 || memcmp(bytes,"%PDF-",5))) || (!pdf && (size<22 || memcmp(bytes,"PK\3\4",4)))) {
        sb_fail(error,"Document content does not match its supported format; encrypted Office files require conversion to an unencrypted copy.");return false;
    }
    HRESULT apartment=CoInitializeEx(NULL,COINIT_MULTITHREADED);
    if(FAILED(apartment) && apartment!=RPC_E_CHANGED_MODE){sb_fail(error,"Document COM backend unavailable.");return false;}
    bool ok=pdf ? sb_pdf_document(bytes,size,operation,unit,max_dimension,doc,document,error) :
                  sb_office_document(bytes,size,format,operation,unit,doc,document,error);
    if(ok && !pdf && !strcmp(operation,"document_preview"))ok=text_preview(document,max_dimension,error);
    if(SUCCEEDED(apartment))CoUninitialize();return ok;
}
