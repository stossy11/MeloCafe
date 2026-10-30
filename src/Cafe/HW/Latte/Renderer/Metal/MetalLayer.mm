#include "Cafe/HW/Latte/Renderer/Metal/MetalLayer.h"

#include <TargetConditionals.h>
#include <QuartzCore/CAMetalLayer.h>
#if TARGET_OS_IPHONE
#include <UIKit/UIKit.h>
#else
#include <AppKit/AppKit.h>
#endif

void* CreateMetalLayer(void* handle, float& scaleX, float& scaleY)
{
#if TARGET_OS_IPHONE
    CGFloat screenScale = [UIScreen mainScreen].scale;
    scaleX = (float)screenScale;
    scaleY = (float)screenScale;

    return (__bridge void*)handle;
#else
    NSView* view = (__bridge NSView*)handle;
    CAMetalLayer* layer = [CAMetalLayer layer];
    view.wantsLayer = YES;
    view.layer = layer;
    const CGFloat screenScale = view.window ? view.window.backingScaleFactor : NSScreen.mainScreen.backingScaleFactor;
    scaleX = scaleY = (float)screenScale;
    layer.contentsScale = screenScale;
    layer.drawableSize = CGSizeMake(view.bounds.size.width * screenScale, view.bounds.size.height * screenScale);
    return (__bridge void*)layer;
#endif
}
