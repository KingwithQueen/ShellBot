#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#include <windows.h>
#include <objbase.h>
#include <shlwapi.h>
#include <msopc.h>
#include <xmllite.h>
#include "document.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PART_LIMIT (8u*1024u*1024u)
#define UNIT_LIMIT 256
#define EVENT_LIMIT 1000000
typedef struct {char *value;size_t size,capacity;bool truncated;} text_buffer;
typedef struct {wchar_t id[128];char name[256];} office_unit;
typedef struct {
    const char *format;office_unit units[UNIT_LIMIT];unsigned count;
    char **shared;size_t shared_count,shared_bytes;
    text_buffer text,cell;char cell_type[16],reference[32];
    unsigned capture_depth,skip_depth,cell_depth;bool in_shared;
} office_context;

static unsigned le16(const unsigned char *p){return (unsigned)p[0]|((unsigned)p[1]<<8);}
static uint32_t le32(const unsigned char *p){return (uint32_t)p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24);}
/* Enforce expansion before handing the ZIP to the system OPC decoder. We do
   not extract paths to disk. OPC subsequently validates CRCs and relationships. */
static bool zip_budget(const unsigned char *p,size_t size) {
    size_t at=size>=22 ? size-22 : 0,minimum=size>65557 ? size-65557 : 0;
    for(;;){if(size-at>=22 && le32(p+at)==0x06054b50 && (size_t)le16(p+at+20)==size-at-22)break;
        if(at==minimum)return false;--at;}
    unsigned count=le16(p+at+10);uint32_t directory=le32(p+at+16),length=le32(p+at+12);
    if(le16(p+at+4)||le16(p+at+6)||!count || count>2048 || le16(p+at+8)!=count || directory>at || length!=at-directory)return false;
    size_t pos=directory;uint64_t expanded=0;
    for(unsigned i=0;i<count;++i){
        if(at-pos<46 || le32(p+pos)!=0x02014b50)return false;
        unsigned flags=le16(p+pos+8),method=le16(p+pos+10),n=le16(p+pos+28),extra=le16(p+pos+30),comment=le16(p+pos+32);
        uint32_t output=le32(p+pos+24),input=le32(p+pos+20),local=le32(p+pos+42);
        if(flags&~0x080eu || flags&1 || (method!=0 && method!=8) || le16(p+pos+34) || !n || n>1024 ||
            (size_t)n+extra+comment>at-pos-46 || output>PART_LIMIT || input>size || local>directory || directory-local<30)return false;
        const unsigned char *name=p+pos+46;
        if(memchr(name,0,n)||name[0]=='/'||memchr(name,'\\',n)||memchr(name,':',n))return false;
        char path[1025];memcpy(path,name,n);path[n]=0;
        if(strstr(path,"../") || strstr(path,"/..") || !strcmp(path,".."))return false;
        const char *base=strrchr(path,'/');base=base ? base+1 : path;
        if(!_stricmp(base,"vbaProject.bin"))return false;
        if(le32(p+local)!=0x04034b50 || le16(p+local+6)!=flags || le16(p+local+8)!=method || le16(p+local+26)!=n)return false;
        size_t header=30u+n+le16(p+local+28);
        if(header>directory-local || memcmp(p+local+30,name,n) || input>directory-local-header)return false;
        if(!(flags&8) && (le32(p+local+14)!=le32(p+pos+16)||le32(p+local+18)!=input||le32(p+local+22)!=output))return false;
        expanded+=output;if(expanded>64u*1024u*1024u)return false;
        pos+=46u+n+extra+comment;
    }
    return pos==at;
}
static bool append(text_buffer *buffer,const char *value,size_t size) {
    size_t n=size;
    if(n>SB_DOCUMENT_TEXT_LIMIT-buffer->size){n=SB_DOCUMENT_TEXT_LIMIT-buffer->size;buffer->truncated=true;
        while(n && n<size && ((unsigned char)value[n]&0xc0)==0x80)--n;}
    if(buffer->size+n+1>buffer->capacity){size_t capacity=buffer->capacity ? buffer->capacity : 256;
        while(capacity<buffer->size+n+1){capacity*=2;if(capacity>SB_DOCUMENT_TEXT_LIMIT+1)capacity=SB_DOCUMENT_TEXT_LIMIT+1;}
        char *next=realloc(buffer->value,capacity);if(!next)return false;buffer->value=next;buffer->capacity=capacity;}
    memcpy(buffer->value+buffer->size,value,n);buffer->size+=n;buffer->value[buffer->size]=0;return true;
}
static bool wide_append(text_buffer *buffer,const wchar_t *value,UINT size) {
    if(!size)return append(buffer,"",0);
    if(size>PART_LIMIT)return false;
    int n=WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,value,(int)size,NULL,0,NULL,NULL);
    char *text=n>0 ? malloc((size_t)n) : NULL;
    bool ok=text && WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,value,(int)size,text,n,NULL,NULL)==n && !memchr(text,0,(size_t)n);
    if(ok)ok=append(buffer,text,(size_t)n);free(text);return ok;
}
static bool utf8(const wchar_t *value,UINT length,char *out,size_t capacity) {
    if(!length){out[0]=0;return true;}
    int n=WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,value,(int)length,out,(int)capacity-1,NULL,NULL);
    if(n<1)return false;out[n]=0;return true;
}
static bool attribute(IXmlReader *reader,const wchar_t *name,const wchar_t *space,wchar_t *out,size_t capacity) {
    out[0]=0;HRESULT hr=IXmlReader_MoveToAttributeByName(reader,name,space);
    if(hr==S_FALSE)return true;if(FAILED(hr))return false;
    const wchar_t *value=NULL;UINT size=0;hr=IXmlReader_GetValue(reader,&value,&size);
    bool ok=SUCCEEDED(hr) && size<capacity;
    if(ok){memcpy(out,value,(size_t)size*sizeof(wchar_t));out[size]=0;}
    IXmlReader_MoveToElement(reader);return ok;
}
static bool namespace_is(const wchar_t *space,const char *kind) {
    const wchar_t *expected=!strcmp(kind,"docx") ? L"http://schemas.openxmlformats.org/wordprocessingml/2006/main" :
        !strcmp(kind,"xlsx") ? L"http://schemas.openxmlformats.org/spreadsheetml/2006/main" : L"http://schemas.openxmlformats.org/presentationml/2006/main";
    const wchar_t *strict=!strcmp(kind,"docx") ? L"http://purl.oclc.org/ooxml/wordprocessingml/main" :
        !strcmp(kind,"xlsx") ? L"http://purl.oclc.org/ooxml/spreadsheetml/main" : L"http://purl.oclc.org/ooxml/presentationml/main";
    return !wcscmp(space,expected)||!wcscmp(space,strict);
}
static bool drawing(const wchar_t *space){return !wcscmp(space,L"http://schemas.openxmlformats.org/drawingml/2006/main") || !wcscmp(space,L"http://purl.oclc.org/ooxml/drawingml/main");}
static bool relationship_id(IXmlReader *reader,wchar_t out[128]) {
    if(!attribute(reader,L"id",L"http://schemas.openxmlformats.org/officeDocument/2006/relationships",out,128))return false;
    if(!*out && !attribute(reader,L"id",L"http://purl.oclc.org/ooxml/officeDocument/relationships",out,128))return false;
    return *out!=0;
}
static bool cell_finish(office_context *context) {
    bool ok=append(&context->text,context->reference,strlen(context->reference)) && append(&context->text,"\t",1);
    if(ok && !strcmp(context->cell_type,"s")){
        const char *value=context->cell.value ? context->cell.value : "";size_t index=0;
        if(!*value)return false;
        for(const char *p=value;*p;++p){if(*p<'0'||*p>'9'||index>32768)return false;index=index*10+(unsigned)(*p-'0');}
        if(index>=context->shared_count)return false;
        ok=append(&context->text,context->shared[index],strlen(context->shared[index]));
    }else if(ok)ok=append(&context->text,context->cell.value ? context->cell.value : "",context->cell.size);
    context->text.truncated|=context->cell.truncated;context->cell.size=0;context->cell.truncated=false;
    if(context->cell.value)context->cell.value[0]=0;
    return ok && append(&context->text,"\n",1);
}
/* mode: main metadata, text body, shared strings, worksheet cells. */
static bool xml_part(IOpcPart *part,office_context *context,unsigned mode,bool keep_text) {
    IStream *stream=NULL;IXmlReader *reader=NULL;STATSTG stat={0};
    HRESULT hr=IOpcPart_GetContentStream(part,&stream);
    if(SUCCEEDED(hr))hr=IStream_Stat(stream,&stat,STATFLAG_NONAME);
    if(SUCCEEDED(hr) && (!stat.cbSize.QuadPart || stat.cbSize.QuadPart>PART_LIMIT))hr=E_FAIL;
    if(SUCCEEDED(hr))hr=CreateXmlReader(&IID_IXmlReader,(void**)&reader,NULL);
    if(SUCCEEDED(hr))hr=IXmlReader_SetProperty(reader,XmlReaderProperty_DtdProcessing,DtdProcessing_Prohibit);
    if(SUCCEEDED(hr))hr=IXmlReader_SetProperty(reader,XmlReaderProperty_XmlResolver,0);
    if(SUCCEEDED(hr))hr=IXmlReader_SetProperty(reader,XmlReaderProperty_MaxElementDepth,64);
    if(SUCCEEDED(hr))hr=IXmlReader_SetInput(reader,(IUnknown*)stream);
    bool ok=SUCCEEDED(hr),root=false;unsigned events=0;
    while(ok){XmlNodeType type;hr=IXmlReader_Read(reader,&type);if(hr==S_FALSE)break;if(FAILED(hr)){ok=false;break;}
        if(++events>EVENT_LIMIT){ok=false;break;}
        UINT depth=0;IXmlReader_GetDepth(reader,&depth);
        /* XmlLite reports end depth before processing the closing node. */
        if(type==XmlNodeType_EndElement && depth)--depth;
        const wchar_t *local=L"",*space=L"";UINT length=0;
        if(type==XmlNodeType_Element || type==XmlNodeType_EndElement){
            if(FAILED(IXmlReader_GetLocalName(reader,&local,&length))||FAILED(IXmlReader_GetNamespaceUri(reader,&space,&length))){ok=false;break;}}
        bool empty=type==XmlNodeType_Element && IXmlReader_IsEmptyElement(reader);
        bool start=type==XmlNodeType_Element,end=type==XmlNodeType_EndElement||empty;
        if(start && !root){
            const wchar_t *expected=mode==2 ? L"sst" : mode==3 ? L"worksheet" :
                !strcmp(context->format,"docx") ? L"document" : !strcmp(context->format,"xlsx") ? L"workbook" : mode==0 ? L"presentation" : L"sld";
            ok=depth==0 && !wcscmp(local,expected) && namespace_is(space,context->format);root=true;
            if(!ok)break;
        }
        if(start && namespace_is(space,context->format) && !wcscmp(local,L"del"))context->skip_depth=depth+1;
        if(start && !wcscmp(local,L"rPh") && namespace_is(space,"xlsx"))context->skip_depth=depth+1;
        if(context->skip_depth){if(end && depth+1==context->skip_depth)context->skip_depth=0;continue;}
        if(mode==0 && start && namespace_is(space,context->format) &&
            ((!strcmp(context->format,"xlsx") && !wcscmp(local,L"sheet")) || (!strcmp(context->format,"pptx") && !wcscmp(local,L"sldId")))){
            if(context->count==UNIT_LIMIT){ok=false;break;}
            office_unit *unit=&context->units[context->count];wchar_t label[128];
            ok=relationship_id(reader,unit->id);
            if(!strcmp(context->format,"xlsx"))ok=ok && attribute(reader,L"name",NULL,label,128) && *label && utf8(label,(UINT)wcslen(label),unit->name,sizeof(unit->name));
            else snprintf(unit->name,sizeof(unit->name),"Slide %u",context->count+1);
            for(unsigned i=0;ok && i<context->count;++i)if(!wcscmp(unit->id,context->units[i].id))ok=false;
            if(ok)++context->count;
        }
        if(mode==2 && start && namespace_is(space,"xlsx") && !wcscmp(local,L"si")){
            if(context->in_shared){ok=false;break;}context->in_shared=true;
            context->cell.size=0;context->cell.truncated=false;if(context->cell.value)context->cell.value[0]=0;
        }
        if(mode==3 && start && namespace_is(space,"xlsx") && !wcscmp(local,L"c")){
            wchar_t reference[32],cell_type[16];
            ok=!context->cell_depth && attribute(reader,L"r",NULL,reference,32) && *reference &&
                attribute(reader,L"t",NULL,cell_type,16) && utf8(reference,(UINT)wcslen(reference),context->reference,sizeof(context->reference)) &&
                utf8(cell_type,(UINT)wcslen(cell_type),context->cell_type,sizeof(context->cell_type));
            size_t i=0;while(context->reference[i]>='A'&&context->reference[i]<='Z')++i;
            if(!i || i>3 || context->reference[i]<'1'||context->reference[i]>'9')ok=false;
            unsigned column=0,row=0;for(size_t j=0;j<i;++j)column=column*26+(unsigned)(context->reference[j]-'A'+1);
            for(size_t j=i;context->reference[j]>='0'&&context->reference[j]<='9';++j){if(row>1048576){ok=false;break;}row=row*10+(unsigned)(context->reference[j]-'0');}
            if(column>16384 || !row || row>1048576)ok=false;
            while(context->reference[i]>='0'&&context->reference[i]<='9')++i;if(context->reference[i])ok=false;
            context->cell_depth=depth+1;
            context->cell.size=0;context->cell.truncated=false;if(context->cell.value)context->cell.value[0]=0;
        }
        bool word=namespace_is(space,"docx"),sheet=namespace_is(space,"xlsx"),draw=drawing(space);
        bool text_element=(mode==2 && context->in_shared && sheet && !wcscmp(local,L"t")) ||
            (mode==3 && context->cell_depth && sheet && ((!wcscmp(local,L"v")) || (!wcscmp(local,L"t") && !strcmp(context->cell_type,"inlineStr")))) ||
            (keep_text && ((word||draw) && !wcscmp(local,L"t")));
        if(start && text_element)context->capture_depth=depth+1;
        if((type==XmlNodeType_Text||type==XmlNodeType_CDATA||type==XmlNodeType_Whitespace) && context->capture_depth){
            const wchar_t *value=NULL;UINT n=0;ok=SUCCEEDED(IXmlReader_GetValue(reader,&value,&n)) &&
                wide_append(mode>=2 ? &context->cell : &context->text,value,n);
        }
        if(keep_text && start && word && (!wcscmp(local,L"tab")||!wcscmp(local,L"br")||!wcscmp(local,L"cr")))
            ok=ok && append(&context->text,!wcscmp(local,L"tab") ? "\t" : "\n",1);
        if(keep_text && end && (word||draw) && !wcscmp(local,L"p"))ok=ok && append(&context->text,"\n",1);
        if(keep_text && end && word && !wcscmp(local,L"tc"))ok=ok && append(&context->text,"\t",1);
        if(end && depth+1==context->capture_depth)context->capture_depth=0;
        if(mode==2 && end && sheet && !wcscmp(local,L"si")){
            size_t size=context->cell.size;
            if(!context->in_shared || context->shared_count>=32768 || context->cell.truncated || size>4u*1024u*1024u-context->shared_bytes){ok=false;break;}
            char **list=realloc(context->shared,(context->shared_count+1)*sizeof(char*));if(!list){ok=false;break;}context->shared=list;
            list[context->shared_count]=sb_strdup(context->cell.value ? context->cell.value : "");if(!list[context->shared_count]){ok=false;break;}
            ++context->shared_count;context->shared_bytes+=size;context->in_shared=false;
        }
        if(mode==3 && end && sheet && !wcscmp(local,L"c")){ok=ok && cell_finish(context);context->cell_depth=0;}
    }
    if(reader)IXmlReader_Release(reader);if(stream)IStream_Release(stream);
    return ok && hr==S_FALSE && root && !context->capture_depth && !context->cell_depth && !context->in_shared;
}
static bool type_is(const wchar_t *type,const wchar_t *suffix) {
    wchar_t first[192],second[192];swprintf(first,192,L"http://schemas.openxmlformats.org/officeDocument/2006/relationships/%ls",suffix);
    swprintf(second,192,L"http://purl.oclc.org/ooxml/officeDocument/relationships/%ls",suffix);
    return !wcscmp(type,first)||!wcscmp(type,second);
}
static IOpcPart *related_part(IOpcPartSet *parts,IOpcRelationship *relation,const wchar_t *kind) {
    IOpcUri *source=NULL;IUri *target=NULL;IOpcPartUri *name=NULL;IOpcPart *part=NULL;
    OPC_URI_TARGET_MODE mode;wchar_t *type=NULL;
    HRESULT hr=IOpcRelationship_GetTargetMode(relation,&mode);
    if(SUCCEEDED(hr) && mode!=OPC_URI_TARGET_MODE_INTERNAL)hr=E_FAIL;
    if(SUCCEEDED(hr))hr=IOpcRelationship_GetRelationshipType(relation,&type);
    if(SUCCEEDED(hr) && !type_is(type,kind))hr=E_FAIL;
    if(SUCCEEDED(hr))hr=IOpcRelationship_GetSourceUri(relation,&source);
    if(SUCCEEDED(hr))hr=IOpcRelationship_GetTargetUri(relation,&target);
    if(SUCCEEDED(hr))hr=IOpcUri_CombinePartUri(source,target,&name);
    if(SUCCEEDED(hr))hr=IOpcPartSet_GetPart(parts,name,&part);
    if(type)CoTaskMemFree(type);if(name)IOpcPartUri_Release(name);if(target)IUri_Release(target);if(source)IOpcUri_Release(source);
    if(FAILED(hr) && part){IOpcPart_Release(part);part=NULL;}return part;
}
static IOpcPart *find_relation(IOpcPartSet *parts,IOpcRelationshipSet *set,const wchar_t *kind,bool *valid) {
    IOpcRelationshipEnumerator *enumerator=NULL;HRESULT hr=IOpcRelationshipSet_GetEnumerator(set,&enumerator);
    IOpcPart *selected=NULL;BOOL more=FALSE;*valid=true;
    while(SUCCEEDED(hr) && SUCCEEDED(hr=IOpcRelationshipEnumerator_MoveNext(enumerator,&more)) && more){
        IOpcRelationship *relation=NULL;wchar_t *type=NULL;hr=IOpcRelationshipEnumerator_GetCurrent(enumerator,&relation);
        if(SUCCEEDED(hr))hr=IOpcRelationship_GetRelationshipType(relation,&type);
        if(SUCCEEDED(hr) && type_is(type,kind)){
            if(selected)*valid=false;else {selected=related_part(parts,relation,kind);if(!selected)*valid=false;}}
        if(type)CoTaskMemFree(type);if(relation)IOpcRelationship_Release(relation);
    }
    if(FAILED(hr))*valid=false;if(enumerator)IOpcRelationshipEnumerator_Release(enumerator);
    if(!*valid && selected){IOpcPart_Release(selected);selected=NULL;}return selected;
}
bool sb_office_document(const unsigned char *bytes,size_t size,const char *format,const char *operation,
                        unsigned unit,yyjson_mut_doc *doc,sb_document *document,sb_error *error) {
    if(!zip_budget(bytes,size)){sb_fail(error,"Office ZIP validation, macro check or 2048-part/8 MiB part/64 MiB expansion budget failed; ZIP64 is unsupported.");return false;}
    IOpcFactory *factory=NULL;IOpcPackage *package=NULL;IOpcPartSet *parts=NULL;IOpcRelationshipSet *roots=NULL,*relationships=NULL;
    IOpcPart *main=NULL,*body=NULL,*shared=NULL;IStream *stream=SHCreateMemStream(bytes,(UINT)size);office_context context={0};context.format=format;
    HRESULT hr=stream ? CoCreateInstance(&CLSID_OpcFactory,NULL,CLSCTX_INPROC_SERVER,&IID_IOpcFactory,(void**)&factory) : E_OUTOFMEMORY;
    if(SUCCEEDED(hr))hr=IOpcFactory_ReadPackageFromStream(factory,stream,OPC_VALIDATE_ON_LOAD,&package);
    if(SUCCEEDED(hr))hr=IOpcPackage_GetPartSet(package,&parts);
    if(SUCCEEDED(hr))hr=IOpcPackage_GetRelationshipSet(package,&roots);
    bool valid=true,ok=SUCCEEDED(hr);
    if(ok){main=find_relation(parts,roots,L"officeDocument",&valid);ok=main && valid;}
    wchar_t *content_type=NULL;
    if(ok)ok=SUCCEEDED(IOpcPart_GetContentType(main,&content_type));
    const wchar_t *expected=!strcmp(format,"docx") ? L"application/vnd.openxmlformats-officedocument.wordprocessingml.document.main+xml" :
        !strcmp(format,"xlsx") ? L"application/vnd.openxmlformats-officedocument.spreadsheetml.sheet.main+xml" : L"application/vnd.openxmlformats-officedocument.presentationml.presentation.main+xml";
    if(ok)ok=!wcscmp(content_type,expected);if(content_type)CoTaskMemFree(content_type);
    bool word=!strcmp(format,"docx"),info=!strcmp(operation,"document_info");
    if(ok)ok=xml_part(main,&context,0,word && !info);
    if(word){context.count=1;strcpy(context.units[0].name,"Document text");}
    document->unit_count=context.count;document->unit_kind=word ? "document" : !strcmp(format,"xlsx") ? "worksheet" : "slide";
    if(ok && !context.count)ok=false;
    if(ok && info){document->units=yyjson_mut_arr(doc);
        for(unsigned i=0;i<context.count && i<128;++i){yyjson_mut_val *item=yyjson_mut_obj(doc);
            yyjson_mut_obj_add_uint(doc,item,"unit",i+1);yyjson_mut_obj_add_strcpy(doc,item,"name",context.units[i].name);yyjson_mut_arr_add_val(document->units,item);}}
    else if(ok){
        if(unit>context.count){ok=false;sb_fail(error,"Office document unit is outside the document.");}
        else strcpy(document->unit_name,context.units[unit-1].name);
        if(ok && !word){ok=SUCCEEDED(IOpcPart_GetRelationshipSet(main,&relationships));
            if(ok && !strcmp(format,"xlsx")){
                shared=find_relation(parts,relationships,L"sharedStrings",&valid);ok=valid;
                if(ok && shared)ok=xml_part(shared,&context,2,false);
            }
            IOpcRelationship *relation=NULL;
            if(ok)ok=SUCCEEDED(IOpcRelationshipSet_GetRelationship(relationships,context.units[unit-1].id,&relation));
            if(ok){body=related_part(parts,relation,!strcmp(format,"xlsx") ? L"worksheet" : L"slide");ok=body!=NULL;}
            if(relation)IOpcRelationship_Release(relation);
            if(ok)ok=xml_part(body,&context,!strcmp(format,"xlsx") ? 3 : 1,true);
        }
        if(ok){ok=append(&context.text,"",0);if(ok){document->text=context.text.value;document->text_size=context.text.size;
            document->extraction_truncated=context.text.truncated;context.text.value=NULL;}}
    }
    free(context.text.value);free(context.cell.value);for(size_t i=0;i<context.shared_count;++i)free(context.shared[i]);free(context.shared);
    if(shared)IOpcPart_Release(shared);if(body)IOpcPart_Release(body);if(relationships)IOpcRelationshipSet_Release(relationships);
    if(main)IOpcPart_Release(main);if(roots)IOpcRelationshipSet_Release(roots);if(parts)IOpcPartSet_Release(parts);
    if(package)IOpcPackage_Release(package);if(factory)IOpcFactory_Release(factory);if(stream)IStream_Release(stream);
    if(!ok && !*error->text)sb_fail(error,"Office package, relationships, XML or extraction budget is invalid; no content returned.");return ok;
}
