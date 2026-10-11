#include "sb.h"
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

bool sb_document_tool(const char *name) {
    return name && (!strcmp(name,"document_info") || !strcmp(name,"document_read") || !strcmp(name,"document_preview"));
}
static bool integer(yyjson_val *args,const char *key,uint64_t fallback,uint64_t low,uint64_t high,uint64_t *out) {
    yyjson_val *v=yyjson_obj_get(args,key); *out=fallback;
    return !v || (sb_integer(v,out) && *out>=low && *out<=high);
}
yyjson_mut_val *sb_document_call(sb_file *file,const char *path,const char *name,yyjson_val *args,
                                yyjson_mut_doc *doc,sb_image *image,sb_error *error) {
    const char *extension=strrchr(path,'.'); char format[8]={0};
    if (extension && strlen(extension)<sizeof(format))
        for(size_t i=1;extension[i];++i) format[i-1]=(char)tolower((unsigned char)extension[i]);
    if (strcmp(format,"pdf") && strcmp(format,"docx") && strcmp(format,"xlsx") && strcmp(format,"pptx")) {
        sb_fail(error,"Supported documents: PDF, DOCX, XLSX and PPTX. Convert legacy DOC/XLS/PPT or macro-enabled files to a supported format first."); return NULL;
    }
    uint64_t size=sb_platform_size(file),unit,offset,limit,dimension;
    if (!size || size>SB_DOCUMENT_INPUT_LIMIT) { sb_fail(error,"Document is empty or exceeds the 32 MiB source budget.");return NULL; }
    if (!integer(args,"unit",1,1,4096,&unit) || !integer(args,"offset",0,0,SB_DOCUMENT_TEXT_LIMIT,&offset) ||
        !integer(args,"max_bytes",SB_TEXT_LIMIT,4,SB_TEXT_LIMIT,&limit) || !integer(args,"max_dimension",1600,128,4096,&dimension)) {
        sb_fail(error,"Invalid document unit, text offset, output budget or preview dimensions.");return NULL;
    }
    unsigned char *bytes=malloc((size_t)size); size_t got=0; sb_document document={0};
    if (!bytes) { sb_fail(error,"Out of memory.");return NULL; }
    bool ok=sb_platform_read(file,0,bytes,(size_t)size,&got,error) && got==size;
    if(ok) ok=sb_platform_document(bytes,got,format,name,(unsigned)unit,(unsigned)dimension,doc,&document,error);
    char source_hash[65]; if(ok)ok=sb_platform_sha256(bytes,got,source_hash,error);
    free(bytes); yyjson_mut_val *data=NULL;
    if (!ok) goto done;
    data=yyjson_mut_obj(doc);
    yyjson_mut_obj_add_strcpy(doc,data,"format",document.format);
    yyjson_mut_obj_add_strcpy(doc,data,"source_sha256",source_hash);
    yyjson_mut_obj_add_uint(doc,data,"source_size",size);
    yyjson_mut_obj_add_uint(doc,data,"unit_count",document.unit_count);
    yyjson_mut_obj_add_str(doc,data,"unit_kind",document.unit_kind);
    yyjson_mut_obj_add_bool(doc,data,"source_modified",false);
    yyjson_mut_obj_add_bool(doc,data,"macros_executed",false);
    yyjson_mut_obj_add_bool(doc,data,"external_resources_loaded",false);
    if (!strcmp(name,"document_info")) {
        yyjson_mut_obj_add_val(doc,data,"units",document.units);
        yyjson_mut_obj_add_bool(doc,data,"units_truncated",document.unit_count>128);
        yyjson_mut_obj_add_str(doc,data,"preview_kind",!strcmp(format,"pdf") ? "pdf_page" : "structured_text");
    } else {
        yyjson_mut_obj_add_uint(doc,data,"unit",unit);
        yyjson_mut_obj_add_strcpy(doc,data,"unit_name",document.unit_name);
        yyjson_mut_obj_add_bool(doc,data,"extraction_truncated",document.extraction_truncated);
        if (!strcmp(name,"document_read")) {
            if(offset>document.text_size || (offset<document.text_size && ((unsigned char)document.text[offset]&0xc0)==0x80)) {
                sb_fail(error,"Text offset is outside the extracted unit or splits a UTF-8 character.");data=NULL;goto done;
            }
            size_t n=document.text_size-(size_t)offset;if(n>limit)n=(size_t)limit;
            while(n && offset+n<document.text_size && ((unsigned char)document.text[offset+n]&0xc0)==0x80)--n;
            yyjson_mut_obj_add_strncpy(doc,data,"text",document.text+offset,n);
            yyjson_mut_obj_add_str(doc,data,"encoding","utf-8");
            yyjson_mut_obj_add_uint(doc,data,"text_size",document.text_size);
            yyjson_mut_obj_add_uint(doc,data,"offset",offset);
            yyjson_mut_obj_add_uint(doc,data,"next_offset",offset+n);
            yyjson_mut_obj_add_bool(doc,data,"truncated",offset+n<document.text_size);
            yyjson_mut_obj_add_bool(doc,data,"has_more",offset+n<document.text_size);
            yyjson_mut_obj_add_bool(doc,data,"formulas_recalculated",false);
            yyjson_mut_obj_add_str(doc,data,"text_order",!strcmp(format,"pdf") ? "PDF text object order; no OCR" : "document XML order; cell references and stored values for worksheets");
        } else {
            char hash[65];if(!sb_platform_sha256(document.image.encoded,document.image.size,hash,error)){data=NULL;goto done;}
            *image=document.image;document.image.encoded=NULL;
            yyjson_mut_obj_add_str(doc,data,"mime_type","image/png");
            yyjson_mut_obj_add_uint(doc,data,"width",image->width);yyjson_mut_obj_add_uint(doc,data,"height",image->height);
            yyjson_mut_obj_add_uint(doc,data,"size",image->size);yyjson_mut_obj_add_strcpy(doc,data,"sha256",hash);
            yyjson_mut_obj_add_str(doc,data,"preview_kind",!strcmp(format,"pdf") ? "pdf_page" : "structured_text");
            yyjson_mut_obj_add_bool(doc,data,"original_layout",!strcmp(format,"pdf"));
            yyjson_mut_obj_add_bool(doc,data,"content_preview_truncated",document.extraction_truncated);
        }
    }
done:
    free(document.text);free(document.image.encoded);
    if(!data && !*error->text)sb_fail(error,"Document read or processing failed; no content returned.");
    return data;
}
