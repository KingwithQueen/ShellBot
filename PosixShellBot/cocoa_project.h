#import <Cocoa/Cocoa.h>
#include "project_form.h"
NSScrollView *sb_macos_form_scroll(NSStackView *panel);
void sb_macos_project_editor(NSWindow *parent, yyjson_val *project, yyjson_val *configuration,
    yyjson_val *settings, const char *directory, const char *expected, void (^submit)(NSDictionary *));
