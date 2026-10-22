#include "media.h"
#import <Cocoa/Cocoa.h>
#import <CoreText/CoreText.h>
#include <stdlib.h>
void sb_preview_shutdown(void) {}

bool sb_office_preview(sb_document *document, unsigned dimension, sb_error *error) {
    @autoreleasepool {
        unsigned w = dimension * 3 / 4, h = dimension;
        size_t n = document->text_size;
        if (n > 16384) {
            n = 16384;
            document->extraction_truncated = true;
        }
        while (n < document->text_size && n && ((unsigned char)document->text[n] & 0xc0) == 0x80)
            --n;
        NSString *text = [[NSString alloc] initWithBytes:document->text
                                                  length:n
                                                encoding:NSUTF8StringEncoding];
        unsigned char *pixels = calloc((size_t)w * h, 4);
        CGColorSpaceRef space = CGColorSpaceCreateDeviceRGB();
        CGContextRef ctx =
            pixels && space
                ? CGBitmapContextCreate(pixels, w, h, 8, w * 4, space,
                                        kCGImageAlphaPremultipliedFirst | kCGBitmapByteOrder32Little)
                : NULL;
        bool ok = ctx && text;
        if (ok) {
            CGContextSetRGBFillColor(ctx, 1, 1, 1, 1);
            CGContextFillRect(ctx, CGRectMake(0, 0, w, h));
            double size = dimension / 65.0;
            if (size < 8)
                size = 8;
            double margin = w / 30.0;
            NSFont *font = [NSFont systemFontOfSize:size];
            NSDictionary *attributes = @{
                NSFontAttributeName : font,
                NSForegroundColorAttributeName : [NSColor colorWithCalibratedRed:.08
                                                                           green:.12
                                                                            blue:.16
                                                                           alpha:1]
            };
            NSAttributedString *title =
                [[NSAttributedString alloc] initWithString:@"Office content preview / 内容预览（非原始排版）"
                                                attributes:attributes];
            CTLineRef line = CTLineCreateWithAttributedString((__bridge CFAttributedStringRef)title);
            CGContextSetTextPosition(ctx, margin, h - margin - size);
            CTLineDraw(line, ctx);
            CFRelease(line);
            NSAttributedString *body = [[NSAttributedString alloc] initWithString:text attributes:attributes];
            CTFramesetterRef setter =
                CTFramesetterCreateWithAttributedString((__bridge CFAttributedStringRef)body);
            CGMutablePathRef path = CGPathCreateMutable();
            CGPathAddRect(path, NULL, CGRectMake(margin, margin, w - 2 * margin, h - 2 * margin - size * 3));
            CTFrameRef frame = CTFramesetterCreateFrame(setter, CFRangeMake(0, 0), path, NULL);
            CFRange visible = CTFrameGetVisibleStringRange(frame);
            if (visible.length < (CFIndex)text.length)
                document->extraction_truncated = true;
            CTFrameDraw(frame, ctx);
            CFRelease(frame);
            CGPathRelease(path);
            CFRelease(setter);
            ok = sb_platform_bitmap_png(pixels, w, h, &document->image, error);
        }
        if (ctx)
            CGContextRelease(ctx);
        if (space)
            CGColorSpaceRelease(space);
        free(pixels);
        if (!ok && !*error->text)
            sb_fail(error, "Office text preview raster unavailable.");
        return ok;
    }
}
