// Host image helpers. See image.h.

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "image.h"

int ms_fetch_image(OfxImageEffectHandle effect, const char* clip, OfxTime time, MsImage* img) {
    OfxImageClipHandle handle;
    void* data = NULL;
    memset(img, 0, sizeof *img);
    if (ms_effect->clipGetHandle(effect, clip, &handle, NULL) != kOfxStatOK) return 0;
    if (ms_effect->clipGetImage(handle, time, NULL, &img->handle) != kOfxStatOK) return 0;
    if (ms_prop->propGetPointer(img->handle, kOfxImagePropData, 0, &data) != kOfxStatOK ||
        ms_prop->propGetInt(img->handle, kOfxImagePropRowBytes, 0, &img->row_bytes) != kOfxStatOK ||
        ms_prop->propGetIntN(img->handle, kOfxImagePropBounds, 4, &img->bounds.x1) != kOfxStatOK)
        return 0;
    img->data = data;
    return data != NULL;
}

void ms_release_image(MsImage* img) {
    if (img->handle) ms_effect->clipReleaseImage(img->handle);
    img->handle = NULL;
}

float* ms_pixel(const MsImage* img, int x, int y) {
    if (!img->data || x < img->bounds.x1 || x >= img->bounds.x2 || y < img->bounds.y1 || y >= img->bounds.y2)
        return NULL;
    return (float*)(img->data + (ptrdiff_t)(y - img->bounds.y1) * img->row_bytes) + (x - img->bounds.x1) * 4;
}

// Columns [*a, *b) of row y that both `area` and `img` cover; 0 if none.
static int overlap(const MsImage* img, OfxRectI area, int y, int* a, int* b) {
    if (!img->data || y < img->bounds.y1 || y >= img->bounds.y2) return 0;
    *a = area.x1 > img->bounds.x1 ? area.x1 : img->bounds.x1;
    *b = area.x2 < img->bounds.x2 ? area.x2 : img->bounds.x2;
    return *a < *b;
}

void ms_pack_image(const MsImage* img, OfxRectI area, float* buffer) {
    const int width = area.x2 - area.x1, height = area.y2 - area.y1;
    for (int row = 0; row < height; ++row) {
        const int y = area.y1 + row;
        float* out = buffer + (size_t)row * width * 4;
        int a, b;
        if (!overlap(img, area, y, &a, &b)) {
            memset(out, 0, (size_t)width * 4 * sizeof(float));
            continue;
        }
        // black | copied pixels | black
        memset(out, 0, (size_t)(a - area.x1) * 4 * sizeof(float));
        memcpy(out + (size_t)(a - area.x1) * 4, ms_pixel(img, a, y), (size_t)(b - a) * 4 * sizeof(float));
        memset(out + (size_t)(b - area.x1) * 4, 0, (size_t)(area.x2 - b) * 4 * sizeof(float));
    }
}

void ms_unpack_image(const float* buffer, OfxRectI area, MsImage* img, OfxRectI window) {
    const int width = area.x2 - area.x1;
    // Only where the window, the buffer and the image all overlap.
    OfxRectI copy = window;
    if (copy.x1 < area.x1) copy.x1 = area.x1;
    if (copy.x2 > area.x2) copy.x2 = area.x2;
    if (copy.y1 < area.y1) copy.y1 = area.y1;
    if (copy.y2 > area.y2) copy.y2 = area.y2;
    for (int y = copy.y1; y < copy.y2; ++y) {
        int a, b;
        if (!overlap(img, copy, y, &a, &b)) continue;
        memcpy(ms_pixel(img, a, y), buffer + ((size_t)(y - area.y1) * width + (a - area.x1)) * 4,
               (size_t)(b - a) * 4 * sizeof(float));
    }
}

OfxStatus ms_render_shader(
        OfxImageEffectHandle effect, OfxPropertySetHandle in_args, const char* shader,
        const MsUniform* uniforms, int uniform_count) {
    
    // 1. read request
    OfxTime time;
    OfxRectI window;
    if (ms_prop->propGetDouble(in_args, kOfxPropTime, 0, &time) != kOfxStatOK ||
        ms_prop->propGetIntN(in_args, kOfxImageEffectPropRenderWindow, 4, &window.x1) != kOfxStatOK)
        return kOfxStatFailed;

    //2. fetch both src and dst images
    MsImage src, dst;
    if (!ms_fetch_image(effect, kOfxImageEffectOutputClipName, time, &dst)) {
        ms_release_image(&dst);
        return kOfxStatFailed;
    }
    ms_fetch_image(effect, kOfxImageEffectSimpleSourceClipName, time, &src);  // not connected: black

    // 3. crop image to its actual payload, and pack it to a buffer
    const OfxRectI area = dst.bounds;
    const int width = area.x2 - area.x1, height = area.y2 - area.y1;
    if (width <= 0 || height <= 0) {
        ms_release_image(&src);
        ms_release_image(&dst);
        return kOfxStatOK;
    }
    float* buffer = malloc((size_t)width * height * 4 * sizeof(float));
    if (!buffer) {
        ms_release_image(&src);
        ms_release_image(&dst);
        return kOfxStatErrMemory;
    }
    ms_pack_image(&src, area, buffer);
    ms_release_image(&src);

    // 4. process the shader on the buffer.
    //  if sux, unpack image to dst
    //  otherwise document error.
    char err[1024];
    OfxStatus status = kOfxStatOK;
    if (ms_gpu_run(shader, buffer, width, height, uniforms, uniform_count, buffer, err, sizeof err)) {
        ms_unpack_image(buffer, area, &dst, window);
    } else {
        OfxPropertySetHandle props;
        char* name = "MillSaph";
        ms_effect->getPropertySet(effect, &props);
        ms_prop->propGetString(props, kOfxPropLabel, 0, &name);
        if (ms_message) ms_message->message(effect, kOfxMessageError, NULL, "%s: %s", name, err);
        status = kOfxStatFailed;
    }

    // 5. release resources.
    free(buffer);
    ms_release_image(&dst);
    return status;
}
