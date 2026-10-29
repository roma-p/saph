// Host image helpers. See image.h.

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "ofx/image.h"

// data struct wrapping ofx image handle.
// image are considered always RGBA 32 bit flaot.
// (up to the host (dcc) to do the conversion)
typedef struct {
    OfxPropertySetHandle handle; // handle to the ofx image data structure
    char *data; // pointers to first pixels. typed to char* for p arithmeric (to use with row_bytes)
    int row_bytes;   // row_bytes: row dimension (can be bigger than width, can be neg).
    OfxRectI bounds; // rectangle describing the actual image.
} Image;

// Gets a clip's image at `time`. Returns 0 if there is none (e.g. the input is not
// connected).
// !! need to call release_image
static int fetch_image(OfxImageEffectHandle effect, const char *clip, OfxTime time, Image *img) {
    OfxImageClipHandle handle;
    void *data = NULL;
    memset(img, 0, sizeof *img);
    if (ms_effect->clipGetHandle(effect, clip, &handle, NULL) != kOfxStatOK)
        return 0;
    if (ms_effect->clipGetImage(handle, time, NULL, &img->handle) != kOfxStatOK)
        return 0;
    if (ms_prop->propGetPointer(img->handle, kOfxImagePropData, 0, &data) != kOfxStatOK ||
        ms_prop->propGetInt(img->handle, kOfxImagePropRowBytes, 0, &img->row_bytes) != kOfxStatOK ||
        ms_prop->propGetIntN(img->handle, kOfxImagePropBounds, 4, &img->bounds.x1) != kOfxStatOK)
        return 0;
    img->data = data;
    return data != NULL;
}

// Gives the image back to the host.
static void release_image(Image *img) {
    if (img->handle)
        ms_effect->clipReleaseImage(img->handle);
    img->handle = NULL;
}

// RGBA pixel at (x, y), or NULL outside the image.
static float *pixel(const Image *img, int x, int y) {
    if (!img->data || x < img->bounds.x1 || x >= img->bounds.x2 || y < img->bounds.y1 ||
        y >= img->bounds.y2)
        return NULL;
    return (float *)(img->data + (ptrdiff_t)(y - img->bounds.y1) * img->row_bytes) +
           (ptrdiff_t)(x - img->bounds.x1) * 4;
}

// Columns [*a, *b) of row y that both `area` and `img` cover; 0 if none.
static int overlap(const Image *img, OfxRectI area, int y, int *a, int *b) {
    if (!img->data || y < img->bounds.y1 || y >= img->bounds.y2)
        return 0;
    *a = area.x1 > img->bounds.x1 ? area.x1 : img->bounds.x1;
    *b = area.x2 < img->bounds.x2 ? area.x2 : img->bounds.x2;
    return *a < *b;
}

static void fill(float *out, size_t count, float value) {
    for (size_t i = 0; i < count; ++i)
        out[i] = value;
}

// Fills `buffer` with the pixels of `img` covering `area`, packed (bottom row first,
// no padding); `outside` (all four channels) where `img` has no pixels. `buffer` must
// hold width x height x 4 floats of `area`.
static void pack_image(const Image *img, OfxRectI area, float outside, float *buffer) {
    const int width = area.x2 - area.x1, height = area.y2 - area.y1;
    for (int row = 0; row < height; ++row) {
        const int y = area.y1 + row;
        float *out = buffer + (size_t)row * width * 4;
        int a, b;
        if (!overlap(img, area, y, &a, &b)) {
            fill(out, (size_t)width * 4, outside);
            continue;
        }
        // outside | copied pixels | outside
        fill(out, (size_t)(a - area.x1) * 4, outside);
        memcpy(out + (size_t)(a - area.x1) * 4, pixel(img, a, y),
               (size_t)(b - a) * 4 * sizeof(float));
        fill(out + (size_t)(b - area.x1) * 4, (size_t)(area.x2 - b) * 4, outside);
    }
}

// Copies the part of a packed buffer covering `area` that falls in `window` into `img`.
static void unpack_image(const float *buffer, OfxRectI area, Image *img, OfxRectI window) {
    const int width = area.x2 - area.x1;
    // Only where the window, the buffer and the image all overlap.
    OfxRectI copy = window;
    if (copy.x1 < area.x1)
        copy.x1 = area.x1;
    if (copy.x2 > area.x2)
        copy.x2 = area.x2;
    if (copy.y1 < area.y1)
        copy.y1 = area.y1;
    if (copy.y2 > area.y2)
        copy.y2 = area.y2;
    for (int y = copy.y1; y < copy.y2; ++y) {
        int a, b;
        if (!overlap(img, copy, y, &a, &b))
            continue;
        memcpy(pixel(img, a, y), buffer + ((size_t)(y - area.y1) * width + (a - area.x1)) * 4,
               (size_t)(b - a) * 4 * sizeof(float));
    }
}

static void define_clip(OfxImageEffectHandle effect, const char *clip, const char *label,
                        int optional, int mask) {
    OfxPropertySetHandle props;
    if (ms_effect->clipDefine(effect, clip, &props) != kOfxStatOK)
        return;
    if (label)
        ms_prop->propSetString(props, kOfxPropLabel, 0, label);
    ms_prop->propSetString(props, kOfxImageEffectPropSupportedComponents, 0,
                           kOfxImageComponentRGBA);
    ms_prop->propSetInt(props, kOfxImageEffectPropSupportsTiles, 0, 0);
    ms_prop->propSetInt(props, kOfxImageClipPropOptional, 0, optional);
    ms_prop->propSetInt(props, kOfxImageClipPropIsMask, 0, mask);
}

OfxStatus ms_define_clips(OfxImageEffectHandle effect, OfxPropertySetHandle in_args,
                          const MsClip *clips, int clip_count) {
    char *context = "";
    ms_prop->propGetString(in_args, kOfxImageEffectPropContext, 0, &context);
    const int filter = strcmp(context, kOfxImageEffectContextFilter) == 0;
    for (int c = 0; c < clip_count; ++c) {
        const int source = strcmp(clips[c].clip, kOfxImageEffectSimpleSourceClipName) == 0;
        if (filter && !source)
            continue; // a filter has only Source and Output
        define_clip(effect, clips[c].clip, clips[c].label, clips[c].optional, clips[c].mask);
    }
    define_clip(effect, kOfxImageEffectOutputClipName, NULL, 0, 0);
    return kOfxStatOK;
}

OfxStatus ms_render_with(OfxImageEffectHandle effect, OfxPropertySetHandle in_args,
                         const MsClip *clips, int clip_count, MsProcess process, void *user) {
    if (clip_count > MS_MAX_CLIPS)
        return kOfxStatFailed;

    // 1. read request
    OfxTime time;
    OfxRectI window;
    if (ms_prop->propGetDouble(in_args, kOfxPropTime, 0, &time) != kOfxStatOK ||
        ms_prop->propGetIntN(in_args, kOfxImageEffectPropRenderWindow, 4, &window.x1) != kOfxStatOK)
        return kOfxStatFailed;

    // 2. fetch the output image: everything is computed at its size
    Image dst;
    if (!fetch_image(effect, kOfxImageEffectOutputClipName, time, &dst)) {
        release_image(&dst);
        return kOfxStatFailed;
    }
    const OfxRectI area = dst.bounds;
    const int width = area.x2 - area.x1, height = area.y2 - area.y1;
    if (width <= 0 || height <= 0) {
        release_image(&dst);
        return kOfxStatOK;
    }

    // 3. one packed buffer per input, plus one for the result. An input with no image
    //    (not connected, or not defined in this context) is filled with its `outside` value.
    const size_t bytes = (size_t)width * height * 4 * sizeof(float);
    float *buffers[MS_MAX_CLIPS] = {NULL};
    MsInput inputs[MS_MAX_CLIPS];
    float *output = malloc(bytes);
    OfxStatus status = output ? kOfxStatOK : kOfxStatErrMemory;
    for (int c = 0; c < clip_count && status == kOfxStatOK; ++c) {
        buffers[c] = malloc(bytes);
        if (!buffers[c]) {
            status = kOfxStatErrMemory;
            break;
        }
        Image img;
        fetch_image(effect, clips[c].clip, time, &img);
        pack_image(&img, area, clips[c].outside, buffers[c]);
        release_image(&img);
        inputs[c].name = clips[c].name;
        inputs[c].pixels = buffers[c];
    }

    // 4. process.
    //  if success, unpack the result to dst
    //  otherwise show the error in the host.
    if (status == kOfxStatOK) {
        char err[2048] = "";
        if (process(inputs, clip_count, width, height, output, err, sizeof err, user)) {
            unpack_image(output, area, &dst, window);
        } else {
            OfxPropertySetHandle props;
            char *name = "MillSaph";
            ms_effect->getPropertySet(effect, &props);
            ms_prop->propGetString(props, kOfxPropLabel, 0, &name);
            if (ms_message)
                ms_message->message(effect, kOfxMessageError, NULL, "%s: %s", name, err);
            status = kOfxStatFailed;
        }
    }

    // 5. release resources.
    for (int c = 0; c < clip_count; ++c)
        free(buffers[c]);
    free(output);
    release_image(&dst);
    return status;
}
