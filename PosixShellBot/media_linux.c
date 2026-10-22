#include "media.h"
#include <pango/pangocairo.h>
#include <fontconfig/fontconfig.h>
#include <stdlib.h>
#include <stdatomic.h>
static atomic_bool preview_used;
/* Called only after backend requests and owner threads have joined. The
   management GUI has a separate lifetime and never calls this cleanup. */
void sb_preview_shutdown(void) {
    if (atomic_load(&preview_used)) {
        pango_cairo_font_map_set_default(NULL);
        cairo_debug_reset_static_data();
        FcFini();
    }
}

bool sb_office_preview(sb_document *document, unsigned dimension, sb_error *error) {
    atomic_store(&preview_used, true);
    unsigned w = dimension * 3 / 4, h = dimension;
    size_t n = document->text_size;
    if (n > 16384) {
        n = 16384;
        document->extraction_truncated = true;
    }
    while (n < document->text_size && n && ((unsigned char)document->text[n] & 0xc0) == 0x80)
        --n;
    cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, (int)w, (int)h);
    cairo_t *cr = cairo_create(surface);
    bool ok =
        cairo_surface_status(surface) == CAIRO_STATUS_SUCCESS && cairo_status(cr) == CAIRO_STATUS_SUCCESS;
    if (ok) {
        cairo_set_source_rgb(cr, 1, 1, 1);
        cairo_paint(cr);
        cairo_set_source_rgb(cr, .08, .12, .16);
        PangoLayout *layout = pango_cairo_create_layout(cr);
        PangoFontDescription *font = pango_font_description_from_string("sans");
        double size = dimension / 65.0;
        if (size < 8)
            size = 8;
        double margin = w / 30.0;
        pango_font_description_set_absolute_size(font, size * PANGO_SCALE);
        pango_layout_set_font_description(layout, font);
        pango_layout_set_width(layout, (int)((w - 2 * margin) * PANGO_SCALE));
        pango_layout_set_wrap(layout, PANGO_WRAP_WORD_CHAR);
        pango_layout_set_text(layout, "Office content preview / 内容预览（非原始排版）", -1);
        cairo_move_to(cr, margin, margin);
        pango_cairo_show_layout(cr, layout);
        pango_layout_set_text(layout, document->text, (int)n);
        int needed = 0;
        pango_layout_get_pixel_size(layout, NULL, &needed);
        double top = margin + size * 3;
        if (needed > (double)h - margin - top)
            document->extraction_truncated = true;
        cairo_rectangle(cr, margin, top, w - 2 * margin, h - margin - top);
        cairo_clip(cr);
        cairo_move_to(cr, margin, top);
        pango_cairo_show_layout(cr, layout);
        pango_font_description_free(font);
        g_object_unref(layout);
        cairo_surface_flush(surface);
        ok = cairo_status(cr) == CAIRO_STATUS_SUCCESS &&
             cairo_image_surface_get_stride(surface) == (int)w * 4 &&
             sb_platform_bitmap_png(cairo_image_surface_get_data(surface), w, h, &document->image, error);
    }
    cairo_destroy(cr);
    cairo_surface_destroy(surface);
    if (!ok && !*error->text)
        sb_fail(error, "Office text preview raster unavailable.");
    return ok;
}
