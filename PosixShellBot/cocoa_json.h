#ifndef SB_COCOA_JSON_H
#define SB_COCOA_JSON_H
#import <Foundation/Foundation.h>

/* C comparisons box as integers on Intel Mac; request fields require JSON booleans. */
static inline NSNumber *sb_macos_json_bool(BOOL value) {
    return [NSNumber numberWithBool:value];
}
#endif
