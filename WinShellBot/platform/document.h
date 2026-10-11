#ifndef SB_WINDOWS_DOCUMENT_H
#define SB_WINDOWS_DOCUMENT_H
#include "sb.h"
bool sb_pdf_document(const unsigned char *bytes,size_t size,const char *operation,unsigned unit,
                     unsigned dimension,yyjson_mut_doc *doc,sb_document *document,sb_error *error);
bool sb_office_document(const unsigned char *bytes,size_t size,const char *format,const char *operation,
                        unsigned unit,yyjson_mut_doc *doc,sb_document *document,sb_error *error);
#endif
