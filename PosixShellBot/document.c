#include "media.h"
#include <string.h>
void sb_media_shutdown(void) {
    sb_pdf_shutdown();
    sb_preview_shutdown();
}
bool sb_platform_document(const unsigned char *bytes, size_t size, const char *format, const char *operation,
                          unsigned unit, unsigned dimension, yyjson_mut_doc *doc, sb_document *result,
                          sb_error *error) {
    result->format = format;
    bool pdf = !strcmp(format, "pdf");
    if (size < 22 || (pdf ? memcmp(bytes, "%PDF-", 5) : memcmp(bytes, "PK\3\4", 4))) {
        sb_fail(error, "Document signature does not match PDF or unencrypted Office Open XML.");
        return false;
    }
    bool ok = pdf ? sb_pdf_document(bytes, size, operation, unit, dimension, doc, result, error)
                  : sb_office_document(bytes, size, format, operation, unit, doc, result, error);
    if (ok && !pdf && !strcmp(operation, "document_preview"))
        ok = sb_office_preview(result, dimension, error);
    return ok;
}
