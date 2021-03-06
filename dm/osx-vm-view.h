
#include "config.h"
#import <AppKit/AppKit.h>

#include "qemu_glue.h"
#include "timer.h"

@interface UXENVirtualMachineView : NSView
{
    CGDataProviderRef dataProviderRef;

    uint8_t *rawbitmap;
    int bitmap_width;
    int bitmap_height;

    int lastx, lasty;
    NSUInteger lastflags;

    QEMUTimer *capslock_timer;

    NSTrackingArea *trackingArea;
    NSCursor *cursor;
    BOOL cursor_in_vm;
}

- (void)setBackingStoreBitmap: (void *)bitmap
                        width: (int)width
                       height: (int)height;

- (void *)getBackingStoreBitmap;

- (void)setCursor: (NSCursor *)new_cursor;

@end
