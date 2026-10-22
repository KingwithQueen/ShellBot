#include "media.h"
#include "fpdfview.h"
#include "fpdf_text.h"
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
/* PDFium is serialized per process, loaded only from the pinned executable-side
   library. No JavaScript/form environment is created. Source stays in memory. */
typedef struct {
    void(FPDF_CALLCONV *init)(void);
    void(FPDF_CALLCONV *destroy)(void);
    FPDF_DOCUMENT(FPDF_CALLCONV *load)(const void *, size_t, FPDF_BYTESTRING);
    void(FPDF_CALLCONV *close)(FPDF_DOCUMENT);
    int(FPDF_CALLCONV *pages)(FPDF_DOCUMENT);
    FPDF_PAGE(FPDF_CALLCONV *page)(FPDF_DOCUMENT, int);
    void(FPDF_CALLCONV *close_page)(FPDF_PAGE);
    float(FPDF_CALLCONV *width)(FPDF_PAGE), (FPDF_CALLCONV * height)(FPDF_PAGE);
    FPDF_TEXTPAGE(FPDF_CALLCONV *text)(FPDF_PAGE);
    int(FPDF_CALLCONV *chars)(FPDF_TEXTPAGE);
    unsigned int(FPDF_CALLCONV *unicode)(FPDF_TEXTPAGE, int);
    void(FPDF_CALLCONV *close_text)(FPDF_TEXTPAGE);
    FPDF_BITMAP(FPDF_CALLCONV *bitmap)(int, int, int);
    FPDF_BOOL(FPDF_CALLCONV *fill)(FPDF_BITMAP, int, int, int, int, FPDF_DWORD);
    void(FPDF_CALLCONV *render)(FPDF_BITMAP, FPDF_PAGE, int, int, int, int, int, int);
    void *(FPDF_CALLCONV *buffer)(FPDF_BITMAP);
    int(FPDF_CALLCONV *stride)(FPDF_BITMAP);
    void(FPDF_CALLCONV *destroy_bitmap)(FPDF_BITMAP);
    unsigned long(FPDF_CALLCONV *error)(void);
} pdf_api;
static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
static pdf_api api;
static void *module;
static int library_pin = -1;

static bool initialize(sb_error *error) {
    if (module)
        return true;
#ifdef __APPLE__
    const char *base = "libpdfium.dylib",
               *expected = "94e955ba3a27ce93a602f43629fe55ebfa7658def9d367a36a614c9d8a75f088";
#else
    const char *base = "libpdfium.so",
               *expected = "e995a89f11d48605a79a0e44f101ca3a621e5c3ef9ee311a1405e03b44dc5b19";
#endif
    char path[SB_PATH_CAP];
    const char *program = sb_posix_program(), *slash = program ? strrchr(program, '/') : NULL;
    if (!slash || (size_t)(slash - program) + strlen(base) + 2 > sizeof(path)) {
        sb_fail(error, "Executable path unavailable for pinned PDFium.");
        return false;
    }
    snprintf(path, sizeof(path), "%.*s/%s", (int)(slash - program), program, base);
    int file = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    struct stat st, now;
    bool ok = file >= 0 && !fstat(file, &st) && S_ISREG(st.st_mode) && st.st_nlink == 1 && st.st_size > 0 &&
              st.st_size < 16 * 1024 * 1024;
    sb_sha sha;
    sb_sha_init(&sha);
    unsigned char bytes[32768];
    char hash[65];
    while (ok) {
        ssize_t n = read(file, bytes, sizeof(bytes));
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0) {
            ok = false;
            break;
        }
        if (!n)
            break;
        sb_sha_update(&sha, bytes, (size_t)n);
    }
    sb_sha_finish(&sha, hash);
    ok = ok && !strcmp(hash, expected);
    /* Absolute executable-side path only. Pin/recheck the inode around dlopen.
       Integrity checks do not sandbox PDFium or defend against the same uid. */
    void *loaded = ok ? dlopen(path, RTLD_NOW | RTLD_LOCAL) : NULL;
    ok = loaded != NULL;
    ok = ok && !lstat(path, &now) && now.st_dev == st.st_dev && now.st_ino == st.st_ino &&
         now.st_size == st.st_size;
#define PROC(field, name)                                                                                    \
    do {                                                                                                     \
        void *p = loaded ? dlsym(loaded, name) : NULL;                                                       \
        if (!p)                                                                                              \
            ok = false;                                                                                      \
        memcpy(&api.field, &p, sizeof(p));                                                                   \
    } while (0)
    PROC(init, "FPDF_InitLibrary");
    PROC(destroy, "FPDF_DestroyLibrary");
    PROC(load, "FPDF_LoadMemDocument64");
    PROC(close, "FPDF_CloseDocument");
    PROC(pages, "FPDF_GetPageCount");
    PROC(page, "FPDF_LoadPage");
    PROC(close_page, "FPDF_ClosePage");
    PROC(width, "FPDF_GetPageWidthF");
    PROC(height, "FPDF_GetPageHeightF");
    PROC(text, "FPDFText_LoadPage");
    PROC(chars, "FPDFText_CountChars");
    PROC(unicode, "FPDFText_GetUnicode");
    PROC(close_text, "FPDFText_ClosePage");
    PROC(bitmap, "FPDFBitmap_Create");
    PROC(fill, "FPDFBitmap_FillRect");
    PROC(render, "FPDF_RenderPageBitmap");
    PROC(buffer, "FPDFBitmap_GetBuffer");
    PROC(stride, "FPDFBitmap_GetStride");
    PROC(destroy_bitmap, "FPDFBitmap_Destroy");
    PROC(error, "FPDF_GetLastError");
#undef PROC
    if (ok) {
        api.init();
        module = loaded;
        library_pin = file;
    } else {
        if (loaded)
            dlclose(loaded);
        if (file >= 0)
            close(file);
        memset(&api, 0, sizeof(api));
        sb_fail(error,
                "Pinned PDFium 8086 backend unavailable or checksum mismatch; no PDF content processed.");
    }
    return ok;
}
void sb_pdf_shutdown(void) {
    pthread_mutex_lock(&mutex);
    if (module) {
        api.destroy();
        dlclose(module);
        close(library_pin);
        module = NULL;
        library_pin = -1;
        memset(&api, 0, sizeof(api));
    }
    pthread_mutex_unlock(&mutex);
}
static bool extracted(FPDF_PAGE page, sb_document *document, sb_error *error) {
    FPDF_TEXTPAGE text = api.text(page);
    if (!text) {
        sb_fail(error, "PDF text page unavailable.");
        return false;
    }
    int count = api.chars(text);
    bool ok = count >= 0;
    size_t used = 0;
    document->text = malloc(SB_DOCUMENT_TEXT_LIMIT + 1);
    if (!document->text)
        ok = false;
    /* Bound parser iteration independently of output (unmapped glyphs count). */
    if (count > 1048576) {
        count = 1048576;
        document->extraction_truncated = true;
    }
    for (int i = 0; ok && i < count; ++i) {
        unsigned c = api.unicode(text, i);
        unsigned char value[4];
        size_t n;
        if (!c)
            continue;
        if (c > 0x10ffff || (c >= 0xd800 && c <= 0xdfff))
            c = 0xfffd;
        if (c < 0x80) {
            value[0] = (unsigned char)c;
            n = 1;
        } else if (c < 0x800) {
            value[0] = (unsigned char)(0xc0 | (c >> 6));
            value[1] = (unsigned char)(0x80 | (c & 63));
            n = 2;
        } else if (c < 0x10000) {
            value[0] = (unsigned char)(0xe0 | (c >> 12));
            value[1] = (unsigned char)(0x80 | ((c >> 6) & 63));
            value[2] = (unsigned char)(0x80 | (c & 63));
            n = 3;
        } else {
            value[0] = (unsigned char)(0xf0 | (c >> 18));
            value[1] = (unsigned char)(0x80 | ((c >> 12) & 63));
            value[2] = (unsigned char)(0x80 | ((c >> 6) & 63));
            value[3] = (unsigned char)(0x80 | (c & 63));
            n = 4;
        }
        if (n > SB_DOCUMENT_TEXT_LIMIT - used) {
            document->extraction_truncated = true;
            break;
        }
        memcpy(document->text + used, value, n);
        used += n;
    }
    api.close_text(text);
    if (ok) {
        document->text[used] = 0;
        document->text_size = used;
    } else
        sb_fail(error, "PDF text extraction failed.");
    return ok;
}
bool sb_pdf_document(const unsigned char *bytes, size_t size, const char *operation, unsigned unit,
                     unsigned dimension, yyjson_mut_doc *doc, sb_document *document, sb_error *error) {
    pthread_mutex_lock(&mutex);
    bool ok = initialize(error);
    FPDF_DOCUMENT pdf = NULL;
    FPDF_PAGE page = NULL;
    if (ok) {
        pdf = api.load(bytes, size, NULL);
        ok = pdf != NULL;
        if (!ok)
            snprintf(error->text, sizeof(error->text),
                     "PDF could not be opened (PDFium error %lu); encrypted/password-protected or corrupt "
                     "documents are not processed.",
                     api.error());
    }
    int count = ok ? api.pages(pdf) : 0;
    if (ok && (count < 1 || count > 4096)) {
        ok = false;
        sb_fail(error, "PDF page count must be within 1..4096.");
    }
    document->unit_kind = "page";
    document->unit_count = (unsigned)count;
    if (ok && !strcmp(operation, "document_info")) {
        document->units = yyjson_mut_arr(doc);
        for (unsigned i = 0; i < document->unit_count && i < 128; ++i) {
            yyjson_mut_val *item = yyjson_mut_obj(doc);
            yyjson_mut_obj_add_uint(doc, item, "unit", i + 1);
            yyjson_mut_arr_add_val(document->units, item);
        }
    } else if (ok) {
        if (unit > document->unit_count) {
            ok = false;
            sb_fail(error, "PDF page is outside the document.");
        } else {
            page = api.page(pdf, (int)unit - 1);
            ok = page != NULL;
            snprintf(document->unit_name, sizeof(document->unit_name), "Page %u", unit);
        }
        if (ok && !strcmp(operation, "document_read"))
            ok = extracted(page, document, error);
        else if (ok) {
            double w = api.width(page), h = api.height(page), longest = w > h ? w : h;
            ok = isfinite(w) && isfinite(h) && w > 0 && h > 0 && w < 1000000 && h < 1000000;
            unsigned width = ok ? (unsigned)(w / longest * dimension) : 0,
                     height = ok ? (unsigned)(h / longest * dimension) : 0;
            if (!width)
                width = 1;
            if (!height)
                height = 1;
            FPDF_BITMAP bitmap = ok ? api.bitmap((int)width, (int)height, 1) : NULL;
            ok = bitmap != NULL;
            if (ok) {
                ok = api.fill(bitmap, 0, 0, (int)width, (int)height, 0xffffffff) &&
                     api.stride(bitmap) == (int)width * 4;
                if (ok) {
                    api.render(bitmap, page, 0, 0, (int)width, (int)height, 0, FPDF_ANNOT);
                    ok = sb_platform_bitmap_png(api.buffer(bitmap), width, height, &document->image, error);
                }
                api.destroy_bitmap(bitmap);
            }
        }
    }
    if (page)
        api.close_page(page);
    if (pdf)
        api.close(pdf);
    pthread_mutex_unlock(&mutex);
    if (!ok && !*error->text)
        sb_fail(error, "PDF page processing failed; no content returned.");
    return ok;
}
