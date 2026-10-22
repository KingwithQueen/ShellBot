#ifndef SB_POSIX_MEDIA_H
#define SB_POSIX_MEDIA_H
#include "posix.h"
void sb_media_shutdown(void);
void sb_pdf_shutdown(void);
void sb_preview_shutdown(void);
bool sb_media_png(const unsigned char *pixels, unsigned w, unsigned h, bool bgra, sb_image *image,
                  sb_error *error);
bool sb_pdf_document(const unsigned char *, size_t, const char *, unsigned, unsigned, yyjson_mut_doc *,
                     sb_document *, sb_error *);
bool sb_office_document(const unsigned char *, size_t, const char *, const char *, unsigned, yyjson_mut_doc *,
                        sb_document *, sb_error *);
bool sb_office_preview(sb_document *, unsigned, sb_error *);
#endif
