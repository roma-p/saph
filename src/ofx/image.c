// Host image helpers. See image.h.

// clock_gettime: hidden by glibc in strict C11 without it.
#define _POSIX_C_SOURCE 200809L

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <time.h>
#endif

#include "common/half.h"
#include "common/log.h"
#include "common/mutex.h"
#include "engine/engine.h"
#include "ofx/image.h"

// Renders of a session that print where their time goes (see ms_render_with).
#define REPORTED_RENDERS 5000

static double now_ms(void) {
#ifdef _WIN32
    LARGE_INTEGER count, frequency;
    QueryPerformanceCounter(&count);
    QueryPerformanceFrequency(&frequency);
    return (double)count.QuadPart * 1e3 / (double)frequency.QuadPart;
#else
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec * 1e3 + (double)t.tv_nsec / 1e6;
#endif
}

// data struct wrapping ofx image handle: RGBA, in floats or 16-bit floats (the depths the
// plugin declares; the host converts others).
typedef struct {
    OfxPropertySetHandle handle; // handle to the ofx image data structure
    char *data; // pointers to first pixels. typed to char* for p arithmeric (to use with row_bytes)
    int row_bytes;   // row_bytes: row dimension (can be bigger than width, can be neg).
    OfxRectI bounds; // rectangle describing the actual image.
    char depth[16];  // the clip's own depth before the host converts it, e.g. "Half"
    int element;     // bytes of one channel value: 4 (float) or 2 (16-bit float)
} Image;

// Gets a clip's image at `time`. Returns 0 if there is none (e.g. the input is not
// connected).
// !! need to call release_image
static int fetch_image(OfxImageEffectHandle effect, const char *clip, OfxTime time, Image *img) {
    OfxImageClipHandle handle;
    void *data = NULL;
    memset(img, 0, sizeof *img);
    snprintf(img->depth, sizeof img->depth, "?");
    if (ms_effect->clipGetHandle(effect, clip, &handle, NULL) != kOfxStatOK)
        return 0;
    OfxPropertySetHandle clip_props;
    char *depth = NULL;
    if (ms_effect->clipGetPropertySet &&
        ms_effect->clipGetPropertySet(handle, &clip_props) == kOfxStatOK &&
        ms_prop->propGetString(clip_props, kOfxImageClipPropUnmappedPixelDepth, 0, &depth) ==
            kOfxStatOK &&
        depth) {
        const size_t prefix = strlen(kOfxBitDepthNone) - strlen("None"); // "OfxBitDepth"
        snprintf(img->depth, sizeof img->depth, "%s",
                 strncmp(depth, kOfxBitDepthNone, prefix) == 0 ? depth + prefix : depth);
    }
    if (ms_effect->clipGetImage(handle, time, NULL, &img->handle) != kOfxStatOK)
        return 0;
    // The image's depth: floats when the host does not say (the plugin's first choice).
    char *pixel_depth = kOfxBitDepthFloat;
    ms_prop->propGetString(img->handle, kOfxImageEffectPropPixelDepth, 0, &pixel_depth);
    img->element = !pixel_depth                                 ? 4
                   : strcmp(pixel_depth, kOfxBitDepthHalf) == 0  ? 2
                   : strcmp(pixel_depth, kOfxBitDepthFloat) == 0 ? 4
                                                                 : 0;
    if (!img->element)
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
static char *pixel(const Image *img, int x, int y) {
    if (!img->data || x < img->bounds.x1 || x >= img->bounds.x2 || y < img->bounds.y1 ||
        y >= img->bounds.y2)
        return NULL;
    return img->data + (ptrdiff_t)(y - img->bounds.y1) * img->row_bytes +
           (ptrdiff_t)(x - img->bounds.x1) * 4 * img->element;
}

// Columns [*a, *b) of row y that both `area` and `img` cover; 0 if none.
static int overlap(const Image *img, OfxRectI area, int y, int *a, int *b) {
    if (!img->data || y < img->bounds.y1 || y >= img->bounds.y2)
        return 0;
    *a = area.x1 > img->bounds.x1 ? area.x1 : img->bounds.x1;
    *b = area.x2 < img->bounds.x2 ? area.x2 : img->bounds.x2;
    return *a < *b;
}

// `count` channel values of `element` bytes (see Image) set to `value`.
static void fill(char *out, size_t count, int element, float value) {
    const uint16_t half = ms_half_from_float(value);
    for (size_t i = 0; i < count; ++i)
        if (element == 2)
            memcpy(out + i * 2, &half, 2);
        else
            memcpy(out + i * 4, &value, 4);
}

// Fills `buffer` with the pixels of `img` covering `area`, packed (bottom row first,
// no padding); `outside` (all four channels) where `img` has no pixels. `buffer` must
// hold width x height x 4 channel values of `area`, in `element` bytes.
static void pack_image(const Image *img, OfxRectI area, float outside, int element,
                       void *buffer) {
    const int width = area.x2 - area.x1, height = area.y2 - area.y1;
    const size_t value = (size_t)element * 4; // bytes of a pixel
    for (int row = 0; row < height; ++row) {
        const int y = area.y1 + row;
        char *out = (char *)buffer + (size_t)row * width * value;
        int a, b;
        if (!overlap(img, area, y, &a, &b)) {
            fill(out, (size_t)width * 4, element, outside);
            continue;
        }
        // outside | copied pixels | outside
        fill(out, (size_t)(a - area.x1) * 4, element, outside);
        memcpy(out + (size_t)(a - area.x1) * value, pixel(img, a, y), (size_t)(b - a) * value);
        fill(out + (size_t)(b - area.x1) * value, (size_t)(area.x2 - b) * 4, element, outside);
    }
}

// Copies the part of a packed buffer covering `area` that falls in `window` into `img`.
static void unpack_image(const void *buffer, OfxRectI area, Image *img, OfxRectI window) {
    const int width = area.x2 - area.x1;
    const size_t value = (size_t)img->element * 4; // bytes of a pixel
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
        memcpy(pixel(img, a, y),
               (const char *)buffer + ((size_t)(y - area.y1) * width + (a - area.x1)) * value,
               (size_t)(b - a) * value);
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

// The pixels of `img` covering `area` as they are in the host's memory, when it has them
// all with rows the kernels can read; NULL otherwise.
static void *view(const Image *img, OfxRectI area, ptrdiff_t *row) {
    if (!img->data || img->row_bytes <= 0 || img->row_bytes % (4 * img->element) != 0 ||
        area.x1 < img->bounds.x1 || area.x2 > img->bounds.x2 || area.y1 < img->bounds.y1 ||
        area.y2 > img->bounds.y2)
        return NULL;
    *row = img->row_bytes / img->element;
    return pixel(img, area.x1, area.y1);
}

OfxStatus ms_render_with(OfxImageEffectHandle effect, OfxPropertySetHandle in_args,
                         const MsClip *clips, const int *connected, int clip_count,
                         MsProcess process, void *user) {
    if (clip_count > MS_MAX_CLIPS)
        return kOfxStatFailed;

    // 1. read request
    OfxTime time;
    OfxRectI window;
    if (ms_prop->propGetDouble(in_args, kOfxPropTime, 0, &time) != kOfxStatOK ||
        ms_prop->propGetIntN(in_args, kOfxImageEffectPropRenderWindow, 4, &window.x1) != kOfxStatOK)
        return kOfxStatFailed;

    // Where the time goes, printed for the first renders of the session: fetching the
    // images, copying them in, the effect, copying the result out, releasing the images.
    double t[6];
    t[0] = now_ms();

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

    // 3. the inputs: read in the host's memory where it covers the area (no copy), else
    //    copied into a packed buffer, `outside` where the image has no pixels. Optional
    //    inputs that are not connected have none.
    //    Every image in the output's depth: the plugin does not take mixed depths.
    const int element = dst.element, half = element == 2;
    const size_t bytes = (size_t)width * height * 4 * element;
    Image images[MS_MAX_CLIPS];
    void *buffers[MS_MAX_CLIPS] = {NULL};
    MsInput inputs[MS_MAX_CLIPS];
    OfxStatus status = kOfxStatOK;
    for (int c = 0; c < clip_count; ++c) {
        inputs[c] = (MsInput){NULL, 0};
        memset(&images[c], 0, sizeof images[c]);
        if ((!connected[c] && clips[c].optional) || status != kOfxStatOK)
            continue;
        fetch_image(effect, clips[c].clip, time, &images[c]);
        if (images[c].data && images[c].element != element)
            status = kOfxStatErrImageFormat;
    }
    t[1] = now_ms();
    char how[MS_MAX_CLIPS][32]; // per input: "in place", "copied", "none"; host depth
    for (int c = 0; c < clip_count; ++c) {
        snprintf(how[c], sizeof how[c], "none");
        if ((!connected[c] && clips[c].optional) || status != kOfxStatOK)
            continue;
        ptrdiff_t row = 0;
        const void *pixels = view(&images[c], area, &row);
        if (pixels) {
            inputs[c] = (MsInput){pixels, row};
            int same = -1; // an earlier input with the same pixels (one image in the host)
            for (int k = 0; k < c && same < 0; ++k)
                if (inputs[k].pixels == pixels && inputs[k].row == row)
                    same = k;
            if (same >= 0)
                snprintf(how[c], sizeof how[c], "same as %s", clips[same].clip);
            else
                snprintf(how[c], sizeof how[c], "in place, host %s", images[c].depth);
            continue;
        }
        snprintf(how[c], sizeof how[c], "copied, host %s", images[c].depth);
        buffers[c] = malloc(bytes);
        if (!buffers[c]) {
            status = kOfxStatErrMemory;
            continue;
        }
        pack_image(&images[c], area, clips[c].outside, element, buffers[c]);
        inputs[c] = (MsInput){buffers[c], (ptrdiff_t)width * 4};
    }

    // 4. the result: written in the host's image when the window is all of it, else into a
    //    packed buffer, then copied to the window.
    ptrdiff_t output_row = 0;
    void *output = NULL, *result = NULL;
    if (window.x1 <= area.x1 && window.x2 >= area.x2 && window.y1 <= area.y1 &&
        window.y2 >= area.y2)
        output = view(&dst, area, &output_row);
    if (!output && status == kOfxStatOK) {
        result = malloc(bytes);
        output = result;
        output_row = (ptrdiff_t)width * 4;
        if (!result)
            status = kOfxStatErrMemory;
    }

    // 5. process; on failure, show the error in the host.
    t[2] = now_ms();
    t[3] = t[4] = t[2];
    if (status == kOfxStatOK) {
        char err[2048] = "";
        const int ok =
            process(inputs, clip_count, half, width, height, output, output_row, err, sizeof err,
                    user);
        t[3] = t[4] = now_ms();
        if (ok) {
            if (result)
                unpack_image(result, area, &dst, window);
            t[4] = now_ms();
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

    // 6. release resources.
    for (int c = 0; c < clip_count; ++c) {
        free(buffers[c]);
        release_image(&images[c]);
    }
    free(result);
    release_image(&dst);
    t[5] = now_ms();

    static int reported;
    static MsMutex lock = MS_MUTEX_INIT;
    ms_mutex_lock(&lock);
    const int number = ++reported;
    ms_mutex_unlock(&lock);
    if (number <= REPORTED_RENDERS) {
        OfxPropertySetHandle props;
        char *name = "?";
        if (ms_effect->getPropertySet(effect, &props) == kOfxStatOK)
            ms_prop->propGetString(props, kOfxPropLabel, 0, &name);
        double kernels = 0, download = 0;
        ms_run_times(&kernels, &download);
        char inputs_line[512] = "";
        size_t used = 0;
        for (int c = 0; c < clip_count && used < sizeof inputs_line; ++c)
            used += (size_t)snprintf(inputs_line + used, sizeof inputs_line - used, "%s%s %s",
                                     c ? ", " : "", clips[c].clip, how[c]);
        ms_log("render %d %s %dx%d: fetch %.1f ms, copy in %.1f, effect %.1f (kernels with "
               "uploads %.1f, download %.1f), copy out %.1f, release %.1f (inputs: %s; output %s, "
               "host %s; window %d %d %d %d, image %d %d %d %d)",
               number, name, width, height, t[1] - t[0], t[2] - t[1], t[3] - t[2], kernels,
               download, t[4] - t[3], t[5] - t[4], inputs_line, result ? "copied" : "in place",
               dst.depth, window.x1, window.y1,
               window.x2, window.y2, area.x1, area.y1, area.x2, area.y2);
    }
    return status;
}
