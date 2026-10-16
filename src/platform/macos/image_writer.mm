// JPEG/PNG encoding with ImageIO for D3DXSaveSurfaceToFile (see image_writer.h).
#import <CoreFoundation/CoreFoundation.h>
#import <CoreGraphics/CoreGraphics.h>
#import <ImageIO/ImageIO.h>

#include <string.h>

#include "image_writer.h"

int Mac_WriteImage(const char *path, int kind, const uint8_t *rgba, int width, int height, int stride, int hasAlpha,
                   float quality)
{
    if (!path || !rgba || width <= 0 || height <= 0 || stride < width * 4)
        return -1;
    @autoreleasepool
    {
        CFStringRef type = kind == MAC_IMAGE_PNG ? CFSTR("public.png") : CFSTR("public.jpeg");
        const bool alpha = hasAlpha && kind == MAC_IMAGE_PNG;
        CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
        CGDataProviderRef dp = CGDataProviderCreateWithData(nullptr, rgba, (size_t)stride * (size_t)height, nullptr);
        const CGBitmapInfo info = (CGBitmapInfo)kCGBitmapByteOrderDefault |
                                  (CGBitmapInfo)(alpha ? kCGImageAlphaLast : kCGImageAlphaNoneSkipLast);
        CGImageRef img = (cs && dp) ? CGImageCreate((size_t)width, (size_t)height, 8, 32, (size_t)stride, cs, info, dp,
                                                    nullptr, false, kCGRenderingIntentDefault)
                                    : nullptr;
        CFURLRef url = CFURLCreateFromFileSystemRepresentation(nullptr, (const UInt8 *)path, (CFIndex)strlen(path), false);
        CGImageDestinationRef dst = url ? CGImageDestinationCreateWithURL(url, type, 1, nullptr) : nullptr;
        int rc = -1;
        if (dst && img)
        {
            float q = quality < 0.0f ? 0.0f : quality > 1.0f ? 1.0f : quality;
            CFNumberRef qn = CFNumberCreate(nullptr, kCFNumberFloatType, &q);
            const void *keys[] = {kCGImageDestinationLossyCompressionQuality};
            const void *vals[] = {qn};
            CFDictionaryRef props = CFDictionaryCreate(nullptr, keys, vals, 1, &kCFTypeDictionaryKeyCallBacks,
                                                       &kCFTypeDictionaryValueCallBacks);
            CGImageDestinationAddImage(dst, img, props);
            rc = CGImageDestinationFinalize(dst) ? 0 : -1;
            CFRelease(props);
            CFRelease(qn);
        }
        if (dst)
            CFRelease(dst);
        if (url)
            CFRelease(url);
        if (img)
            CGImageRelease(img);
        if (dp)
            CGDataProviderRelease(dp);
        if (cs)
            CGColorSpaceRelease(cs);
        return rc;
    }
}
