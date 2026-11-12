/* The fullscreen content shape for widescreen auto, called on SDL's main thread.
 * A notched Mac's desktop mode includes pixels above its fullscreen content.
 * NSScreen's safe area removes those pixels without subtracting the Dock.
 */
#import <AppKit/AppKit.h>

int host_macos_display_size(int *w, int *h)
{
    @autoreleasepool {
        /* The first screen owns the menu bar, matching SDL's primary display;
         * mainScreen instead follows the window with keyboard focus. */
        NSScreen *screen = NSScreen.screens.firstObject;
        if (!screen)
            return 0;
        NSRect frame = screen.frame;
        NSEdgeInsets safe = NSEdgeInsetsMake(0, 0, 0, 0);
        if (@available(macOS 12.0, *))
            safe = screen.safeAreaInsets;
        CGFloat width = frame.size.width - safe.left - safe.right;
        CGFloat height = frame.size.height - safe.top - safe.bottom;
        if (width <= 0 || height <= 0)
            return 0;
        *w = (int)(width + 0.5);
        *h = (int)(height + 0.5);
        return *w > 0 && *h > 0;
    }
}
