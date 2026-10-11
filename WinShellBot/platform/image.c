#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#include <windows.h>
#include <objbase.h>
#include <wincodec.h>
#include "sb.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void dimensions(UINT width,UINT height,unsigned maximum,UINT *out_width,UINT *out_height) {
    UINT longest=width>height ? width : height;
    if (maximum && longest>maximum) {
        *out_width=(UINT)((uint64_t)width*maximum/longest); *out_height=(UINT)((uint64_t)height*maximum/longest);
        if (!*out_width) *out_width=1; if (!*out_height) *out_height=1;
    } else { *out_width=width; *out_height=height; }
}
static HRESULT encode(IWICImagingFactory *factory,IWICBitmap *bitmap,UINT width,UINT height,
                      unsigned char **output,size_t *size) {
    IWICBitmapScaler *scaler=NULL; IWICFormatConverter *converter=NULL;
    IWICBitmapEncoder *encoder=NULL; IWICBitmapFrameEncode *frame=NULL;
    IPropertyBag2 *properties=NULL; IStream *stream=NULL;
    HRESULT hr=IWICImagingFactory_CreateBitmapScaler(factory,&scaler);
    if (SUCCEEDED(hr)) hr=IWICBitmapScaler_Initialize(scaler,(IWICBitmapSource *)bitmap,width,height,WICBitmapInterpolationModeFant);
    if (SUCCEEDED(hr)) hr=IWICImagingFactory_CreateFormatConverter(factory,&converter);
    if (SUCCEEDED(hr)) hr=IWICFormatConverter_Initialize(converter,(IWICBitmapSource *)scaler,&GUID_WICPixelFormat32bppBGRA,WICBitmapDitherTypeNone,NULL,0,WICBitmapPaletteTypeCustom);
    if (SUCCEEDED(hr)) hr=CreateStreamOnHGlobal(NULL,TRUE,&stream);
    if (SUCCEEDED(hr)) hr=CoCreateInstance(&CLSID_WICPngEncoder,NULL,CLSCTX_INPROC_SERVER,&IID_IWICBitmapEncoder,(void **)&encoder);
    if (SUCCEEDED(hr)) hr=IWICBitmapEncoder_Initialize(encoder,stream,WICBitmapEncoderNoCache);
    if (SUCCEEDED(hr)) hr=IWICBitmapEncoder_CreateNewFrame(encoder,&frame,&properties);
    if (SUCCEEDED(hr)) hr=IWICBitmapFrameEncode_Initialize(frame,properties);
    if (SUCCEEDED(hr)) hr=IWICBitmapFrameEncode_SetSize(frame,width,height);
    WICPixelFormatGUID format=GUID_WICPixelFormat32bppBGRA;
    if (SUCCEEDED(hr)) hr=IWICBitmapFrameEncode_SetPixelFormat(frame,&format);
    if (SUCCEEDED(hr) && !IsEqualGUID(&format,&GUID_WICPixelFormat32bppBGRA)) hr=E_FAIL;
    if (SUCCEEDED(hr)) hr=IWICBitmapFrameEncode_WriteSource(frame,(IWICBitmapSource *)converter,NULL);
    if (SUCCEEDED(hr)) hr=IWICBitmapFrameEncode_Commit(frame);
    if (SUCCEEDED(hr)) hr=IWICBitmapEncoder_Commit(encoder);
    STATSTG stat={0}; HGLOBAL buffer=NULL;
    if (SUCCEEDED(hr)) hr=IStream_Stat(stream,&stat,STATFLAG_NONAME);
    if (SUCCEEDED(hr) && (!stat.cbSize.QuadPart || stat.cbSize.QuadPart>SB_IMAGE_LIMIT)) hr=HRESULT_FROM_WIN32(ERROR_FILE_TOO_LARGE);
    if (SUCCEEDED(hr)) hr=GetHGlobalFromStream(stream,&buffer);
    if (SUCCEEDED(hr)) {
        void *data=GlobalLock(buffer);
        *output=data ? malloc((size_t)stat.cbSize.QuadPart) : NULL;
        if (!*output) hr=E_OUTOFMEMORY;
        else { *size=(size_t)stat.cbSize.QuadPart; memcpy(*output,data,*size); }
        if (data) GlobalUnlock(buffer);
    }
    if (frame) IWICBitmapFrameEncode_Release(frame); if (encoder) IWICBitmapEncoder_Release(encoder);
    if (properties) IPropertyBag2_Release(properties); if (stream) IStream_Release(stream);
    if (converter) IWICFormatConverter_Release(converter); if (scaler) IWICBitmapScaler_Release(scaler);
    return hr;
}
bool sb_platform_image_decode(const unsigned char *bytes,size_t size,unsigned max_dimension,sb_image *image,sb_error *error) {
    HRESULT apartment=CoInitializeEx(NULL,COINIT_MULTITHREADED);
    if (FAILED(apartment) && apartment!=RPC_E_CHANGED_MODE) {
        snprintf(error->text,sizeof(error->text),"Image decoder COM unavailable (0x%08lx).",(unsigned long)apartment); return false;
    }
    IWICImagingFactory *factory=NULL; IWICStream *stream=NULL; IWICBitmapDecoder *decoder=NULL;
    IWICBitmapFrameDecode *frame=NULL; IWICFormatConverter *converter=NULL; IWICBitmap *bitmap=NULL;
    unsigned char *pixels=NULL; UINT width=0,height=0,frames=0; bool png=!strcmp(image->source_mime,"image/png");
    HRESULT hr=CoCreateInstance(&CLSID_WICImagingFactory,NULL,CLSCTX_INPROC_SERVER,&IID_IWICImagingFactory,(void **)&factory);
    if (SUCCEEDED(hr)) hr=IWICImagingFactory_CreateStream(factory,&stream);
    if (SUCCEEDED(hr)) hr=IWICStream_InitializeFromMemory(stream,(BYTE *)bytes,(DWORD)size);
    if (SUCCEEDED(hr)) hr=CoCreateInstance(png ? &CLSID_WICPngDecoder : &CLSID_WICJpegDecoder,NULL,CLSCTX_INPROC_SERVER,&IID_IWICBitmapDecoder,(void **)&decoder);
    if (SUCCEEDED(hr)) hr=IWICBitmapDecoder_Initialize(decoder,(IStream *)stream,WICDecodeMetadataCacheOnDemand);
    if (SUCCEEDED(hr)) hr=IWICBitmapDecoder_GetFrameCount(decoder,&frames);
    if (SUCCEEDED(hr) && frames!=1) hr=E_FAIL;
    if (SUCCEEDED(hr)) hr=IWICBitmapDecoder_GetFrame(decoder,0,&frame);
    if (SUCCEEDED(hr)) hr=IWICBitmapFrameDecode_GetSize(frame,&width,&height);
    if (SUCCEEDED(hr) && (width!=image->source_width || height!=image->source_height)) hr=E_FAIL;
    if (SUCCEEDED(hr)) hr=IWICImagingFactory_CreateFormatConverter(factory,&converter);
    /* Premultiplied alpha prevents transparent colors bleeding into resized edges. */
    if (SUCCEEDED(hr)) hr=IWICFormatConverter_Initialize(converter,(IWICBitmapSource *)frame,&GUID_WICPixelFormat32bppPBGRA,WICBitmapDitherTypeNone,NULL,0,WICBitmapPaletteTypeCustom);
    UINT stride=width*4,capacity=stride*height;
    if (SUCCEEDED(hr)) { pixels=malloc(capacity); if (!pixels) hr=E_OUTOFMEMORY; }
    /* Copy the entire raster, even when preserving originals or making a thumbnail. */
    if (SUCCEEDED(hr)) hr=IWICFormatConverter_CopyPixels(converter,NULL,stride,capacity,pixels);
    if (SUCCEEDED(hr)) {
        image->width=width; image->height=height; image->size=size; image->mime=image->source_mime;
        unsigned maximum=max_dimension ? max_dimension : size>SB_IMAGE_LIMIT ? 1600 : 0;
        dimensions(width,height,maximum,&image->width,&image->height);
        if (size>SB_IMAGE_LIMIT || image->width!=width || image->height!=height) {
            hr=IWICImagingFactory_CreateBitmapFromMemory(factory,width,height,&GUID_WICPixelFormat32bppPBGRA,stride,capacity,pixels,&bitmap);
            if (SUCCEEDED(hr)) {
                do {
                    hr=encode(factory,bitmap,image->width,image->height,&image->encoded,&image->size);
                    if (hr!=HRESULT_FROM_WIN32(ERROR_FILE_TOO_LARGE)) break;
                    UINT longest=image->width>image->height ? image->width : image->height;
                    if (longest<=128) break;
                    maximum=longest/2; if (maximum<128) maximum=128;
                    dimensions(width,height,maximum,&image->width,&image->height);
                } while (true);
            }
            if (SUCCEEDED(hr)) image->mime="image/png";
        }
    }
    if (bitmap) IWICBitmap_Release(bitmap); free(pixels);
    if (converter) IWICFormatConverter_Release(converter); if (frame) IWICBitmapFrameDecode_Release(frame);
    if (decoder) IWICBitmapDecoder_Release(decoder); if (stream) IWICStream_Release(stream);
    if (factory) IWICImagingFactory_Release(factory); if (SUCCEEDED(apartment)) CoUninitialize();
    if (FAILED(hr)) {
        free(image->encoded); image->encoded=NULL;
        snprintf(error->text,sizeof(error->text),"Full PNG/JPEG decode or bounded PNG encoding failed (0x%08lx); no image returned.",(unsigned long)hr);
    }
    return SUCCEEDED(hr);
}
