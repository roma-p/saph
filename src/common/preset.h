// MillSaph's presets: one JSON file per preset, in a folder per effect next to the plugin
// (the bundle's Contents/Resources/presets/<folder>/, MsEffect.presets):
//   {"name": "Comic Edges", "tags": ["Blurred", ...], "params": {"blur_width": 0.088, ...}}
// values as the host's parameters hold them: WIDTH and HEIGHT as fractions of the frame,
// CHOICE as indices, BOOL 0 or 1, RGB as [r, g, b], POINT as [x, y] fractions of the frame
// (y from the bottom), STRING as text. Each has a thumbnail beside it (<file>.png).
// Applying one sets every parameter to its default, then the preset's.

#pragma once

#include <stddef.h>

#include "common/effect.h"

#ifdef __cplusplus
extern "C" {
#endif

// The folder of the presets `folder` (MsEffect.presets), with a final separator, into `out`;
// 0 if there is none.
int ms_presets_dir(const char *folder, char *out, size_t size);

// Applies the preset `text` (a preset file's contents) to the parameters of `e` through
// `host`. Parameters the effect does not have are skipped. Returns 0 with the reason in
// `err` if the text is not a preset.
int ms_apply_preset(const MsEffect *e, MsHost *host, const char *text, size_t length, char *err,
                    size_t err_size);

// Load Preset: the preset window (millsaph_preset_browser, next to the plugin) on the
// effect's presets, run and waited for; the preset picked there applied. Returns 1 when
// done or cancelled, 0 with the reason in `err`.
int ms_load_preset(const MsEffect *e, MsHost *host, char *err, size_t err_size);

// The name of the button every effect with presets gets (OFX, After Effects).
#define MS_LOAD_PRESET "load_preset"

#ifdef __cplusplus
}
#endif
