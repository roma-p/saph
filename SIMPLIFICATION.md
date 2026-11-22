# Simplification plan

Survey of 2026-11-22 (about 24,000 lines): what can be shared, merged or removed, about 1,500
lines in all. Line numbers are those of that day. Almost everything here is factoring that must
not change a pixel: each batch is checked with `tools/measure/bitexact.sh` (every case of
`tools/test_cases.txt`, CPU and GPU, byte for byte against the commit before), then
`./build.sh`. Rules that still hold: compute in Halide only, C orchestrates; malloc and free in
the same function; no CPU fallback for a GPU problem; parameter names, defaults and limits stay
byte for byte (hosts and presets save them by name).

Order: batch 1, then 2, 3, 4, 5. Tick what is done.

## Batch 1: bugs

- [x] `tools/measure/ms_render.py:36`: the plugin is looked for in `Contents/Darwin` on macOS,
  the bundle has `Contents/MacOS` (`CMakeLists.txt:73`): ms_render.py cannot find it on a Mac.
- [x] `src/effects/ms_glare.c:309-313`: when the style's nodes cannot be allocated, the message
  says the Style is not a glare style; it should say there is not enough memory.
- [x] `tools/format.sh --check`: about 470 lines in 37 files are not formatted (mostly the
  effects added since 2026-11, `ae/plugin.c`, `glare_layout.h`): run `tools/format.sh` once, in
  a commit of its own (no code change), before the other batches.

## Batch 2: one list of effects, named fields (C)

- [x] **One effect list.** The 16 effects are listed by hand in 7 places: `src/common/effect.h:254-270`
  (externs), `src/effects/effects.c:6-24`, `src/ae/plugin.c:63-70` (`effects[]`), `:71-74`
  (enum), `:751-766` (`ENTRY`), `:790-807` (registration), `src/ae/pipl.r:36-51`. One X-macro
  file (`src/effects/effects.def`, `MS_EFFECT(Name, lower, ...)`), included by all of them
  (`pipl.r` already goes through the C preprocessor, `CMakeLists.txt:436`, `:467`); MS_Test
  left out of AE by a column. A new effect: one line instead of six edits. About 70 lines. (Done: `src/effects/list.def`;
  it also fixed the PiPL flags of Glow, JpegDamage, Flicker and TVDamage, which vary over time.)
- [x] **`MsEffect` by field name.** The 17 descriptors are positional
  (`render, NULL, 0, NULL, overlay, ARRAY_SIZE(overlay), "bandpass", NULL`, e.g.
  `ms_bandpass.c:203-219`): designated initialisers plus `MS_CLIPS()`, `MS_PARAMS()`,
  `MS_OVERLAYS()` (pointer and count). About 110 lines.
- [x] **`MsRender` by field name** in `src/ae/plugin.c:562-567` and `tools/perf/bench.c:175-177`,
  `:261-262` (18 fields by position; `src/ofx/plugin.c:333-340` already names them). Safety only.

## Batch 3: the effects' render code (C)

- [x] **Scratch images.** Nine renders do N mallocs, a long out-of-memory test, `ms_image` per
  buffer, `between[]` built with `if (...) between[n++] = ...`, then N frees:
  `ms_tvdamage.c:486-564`, `ms_glare.c:320-391`, `ms_glint.c:257-290`,
  `ms_pixelsort.c:400-445`, `ms_rackdefocus.c:397-432`, `ms_mosaic.c:177-209`,
  `ms_lensflare.c:901-948`, `ms_glow.c:262-328`, `ms_edgeblur.c:127-157`.
  (a) `ms_run` skips NULL entries of `between` as it does for `inputs` (`run.c:182`, `:233`);
  (b) a layout helper (`size_t ms_layout(const size_t *floats, int n, size_t *offsets)`): one
  malloc and one free per render, as `ms_edgeawareblur.c:288-312` already does by hand.
  Distort's `between` (`ms_distort.c:344`) then stops passing unused images. About 130 lines.
  (Done: `ms_parts_size` / `ms_parts`, `ms_mask_between`, `ms_out_of_memory`; the mask value's
  memory is a part too, `ms_mask_free` is gone. Fewer error paths, about 25 lines net.)
- [x] **Parameter macros.**
  - BandPass, EdgeAwareBlur, Distort spell out Rel X / Rel Y (`ms_bandpass.c:59-74`,
    `ms_edgeawareblur.c:59-74`, `ms_distort.c:101-116`): `MS_RELATIVE(..., 1.0)`.
  - Blur Mask copied 7 times (bandpass:95, distort:136, mosaic:91, rackdefocus:245,
    tvdamage:293, jpegdamage:193, pixelsort:210): `MS_PARAM_BLUR_MASK(def, dmax)` in `mask.h`.
  - RGB triplets by hand (glow:83-102, glint:69-95, glare:90-111, tvdamage:248-259,
    flickermatch:64-69): `MS_COLOUR_DEF(..., default)`.
  - Widths from 0, unbounded, about 25 times: `MS_WIDTH(id, n, l, h, d, dmax)`.
  - Atmosphere's 5 parameters in glow.c:179-208 and lensflare.c:337-371:
    `MS_ATMOSPHERE_PARAMS(first, amp_hint)` in `atmosphere.h`.
  About 200 lines. (Done: 65 entries by `MS_WIDTH`, `MS_RELATIVE`, `MS_FRACTION`, `MS_COLOUR(_OF)`,
  `MS_PARAM_BLUR_MASK`, and the Atmosphere's; every effect's description dumped and compared
  field by field: only LensFlare's Seed and Speed limits, 0 / 0 instead of +-MS_UNBOUNDED,
  the same for both hosts.)
- [x] **Overlays**: ellipse + arrow X + arrow Y repeated (bandpass:176-201,
  edgeawareblur:459-478, glow:347-390, glare:397-408, rackdefocus:443-454,
  lensflare:1000-1012): one `MS_OVERLAY_ELLIPSE_ARROWS(...)`. About 70 lines.
- [ ] **"An input plugged into the Source stands for it"**, 9 times (glow:279-284, glint:273-275,
  glare:356-358, edgeblur:139-142, lensflare:926-928, flickermatch:106-108,
  distort:323-333, edgeawareblur:335-339, mask_input.c:27-29): `ms_input_or(r, clip, &img,
  &source)`, plus `ms_render_input()` / `ms_render_output()` for the 34 `ms_image_rgba` lines.
  About 50 lines.
- [ ] **Whole pixels + fraction** of a width, 6 copies (edgeblur:116-121, glow:306-313,
  glint:214-218 and 245-247, glare:253-257, lensflare:490-494, rackdefocus:357-359 with 1e5):
  `ms_whole_px()`. About 30 lines.
- [ ] **Glint and Glare** make the same 13-argument `MS_K_GLINT_SOURCE` call (glint:183-191,
  glare:206-213) from the same 10 fields: one shared struct and call. About 30 lines. Related:
  Glare uses `MS_K_RACK_DEFOCUS_MULTIPLY`, TVDamage `MS_K_PIXEL_SORT_DOWN`: generic names
  (`fft_multiply`, `downsample`) would show they are shared.
- [ ] **Inputs de-duplicated by hand** before `ms_run`, which already drops NULL and repeated
  images (`run.c:247-260`): glow:319-326, lensflare:935-937. About 8 lines.
- [ ] **Two structs in the older effects** (Settings, then Job): Distort (`Settings` 182-193,
  `render` 356-389, `process` 295-352) and EdgeAwareBlur (134-140, 430-447, 282-410): fill
  Job directly, the mask in `MsMask`. About 60 lines.
- [ ] **Dead fields**: Glare's `pad` always 0 (glare:322, passed at 232, 244); RackDefocus
  `j.pad = j.half` (rackdefocus:393); unused `<string.h>` (distort), `<stdio.h>` (jpegdamage).
- [ ] **Small repeats**: text-or-default (glare:265, lensflare:955): `ms_param_text()`;
  `hue = v - floor(v)` 3 times; the "not enough memory" message 13 times: `ms_fail_memory(r)`;
  LensFlare's Job copies `MsAtmosphere`'s fields one by one (425-427, 723-730), Glow embeds it.
- [ ] Glare's `render()` (124 lines, glare:259-382): split as LensFlare's `build_rows`.

## Batch 4: Halide kernels

- [ ] **Schedules.** Three identical `tiles()` (`glare_generator.cpp:43-50`,
  `rack_defocus_generator.cpp:46-53`, `lensflare_generator.cpp:35-44`), the same schedule
  inline about 20 times (blur:42-46, edge_blur:88-92, glow:75-84 twice, glint:70-79,
  151-161, 209-218, jpeg_damage:350, 364, pixel_sort:353-358, 427-432, test:20-24 a copy of
  `ms::schedule_rgba`, convert:22-30), the scalar 2D one about 15 times (mask:37-40,
  edge_blur:57-60, glare:118-121, jpeg, distort...). Extend `ms::schedule_rgba` (channels,
  float4 or unrolled c, tile size, compute_root), add `ms::schedule_2d`. Each site keeps its
  tile size and vectorisation: no render nor speed change. About 120 lines.
- [ ] **Glare family finish**: screen over the Source x opacity, or under, + Affect Alpha,
  identical in glint:192-203 and glare:310-319; Hue Shift, Saturation, blur chain in
  glare:298-308, lensflare:535-545, glint:180-191: `ms::toned_glare()`, `ms::screen_over()`
  in `hue.h`. About 35 lines.
- [ ] **`blur3_subpixel` with one radius**: 9 calls of 10 pass `(n, f, n, n, f, n)`
  (edge_blur:42, 78; glare:305; glint:61, 187; lensflare:349, 361, 541; rack_defocus:222).
- [ ] **Glow / Glint pre-threshold source** (glow:31-37, glint:53-58):
  `ms::from_alpha_masked()` in `threshold.h`. About 12 lines.
- [ ] **Mask input**: `mask_value.dim(0).set_stride(Expr())` 9 times, the mix
  `s + (e - s) * m` 5 times: `ms::optional_mask()`, `ms::masked()` in `mask.h` (BandPass's
  `s*(1-m) + e*m` rounds differently: leave it).
- [ ] **Layout**: `ms::interleaved(b, n = 4)` for fft:57-60, rack_defocus:259-260, 296-301,
  338, 366, convert:15-18; `u32()` once in `shared.h` (convert:20, noise.h:17).
- [ ] **Luma and bilinear** (luma in bandpass:55, edge_blur:40, flicker_match:55, glare:302,
  glint:54, jpeg:297, rack_defocus:109, hue.h:27-28, lensflare:46, tv_damage:44, mask.h:12;
  bilinear in tv_damage:55-67, glare:287-294, rack_defocus:90-96, lensflare:231-232,
  distort:296): shared helpers only if each site keeps its arithmetic.
  `(5r+8g+3b)/16` and `0.3125r+0.5g+0.1875b` are not the same float, nor `lerp` and
  `a*(1-f)+b*f`: two helpers, or re-check the measured matches.
- [ ] Central-difference gradient (edge_blur:45-51, distort:82-89): one helper.
- [ ] BandPass's GPU "windows" version never runs with the profiles of `profile.c` (staged
  for boxes up to 200 x 100, prefix from 24, `ms_bandpass.c:131-137`): build it CPU only,
  once CUDA / D3D12 profiles are measured (bench.c's `ms_profile_set` can still reach it).

Not worth it: merging `glint_combine` and `glint_combine_blur` (the GPU prefix sums would stay
compute_root); `glare_half` vs `pixel_sort_down` (they differ on odd sizes); `dt_generator.cpp`
is already well factored.

## Batch 5: adapters, editor, tools, build

- [ ] **Editor windows**: `src/editor/main.cpp:1702-1787` and `browser_main.cpp:286-370` repeat
  the GLFW / ImGui setup, loop and shutdown; `screenshot()` (main:1642-1654,
  browser:270-282) and `read_file()` (main:103-108, browser:62-67) are copies:
  `editor/window.{h,cpp}` (`run_window`, `screenshot`, `read_file`, `style`, `fonts`). About 90 lines.
- [ ] **`editor/main.cpp` (1787 lines)**: tree and selection (118-352) to `tree.cpp`, properties
  (654-1156) to `properties.cpp`; "New lens" twice (1373-1376, 1431-1434), "Save or Save As"
  three times (1381-1385, 1443-1447, 1557): `new_lens()`, `save_or_ask()`;
  `open_file_dialog(app, 1|2|3)`: an enum; `load_presets` (1260-1263) re-reads a file
  inline instead of `read_file`. About 40 lines.
- [x] **`test_host` built three ways**: CMake (`CMakeLists.txt:408-413`), and by hand with `cc`
  in `tools/perf/perf.sh:23-24` and `tools/sanitize.sh:19-23`: use `build/test_host`
  (CMake's sanitizer flags already apply to it).
- [x] **The plugin's path per system** in 4 scripts (`sanitize.sh:18-21`, `perf.sh:15-18`,
  `bitexact.sh:20`, `ms_render.py`): one `tools/plugin_path.sh` (ms_render.py keeps its own constant, the only Python one).
- [ ] **`src/ofx/image.c` `ms_render_with` (168 lines, 191-358)**: `fetch_inputs()`,
  `log_render()`; the label asked of the host twice (309-312, 334-337) is `e->label`; the
  "same pixels as an earlier input" loop (252-255) is `ms_same_input()`. About 20 lines.
- [ ] **`src/ofx/plugin.c` parameters**: `host_get_value` (386-410) and `host_set_value`
  (419-458) each parse `_y`, `_green`, `_blue`; the Choice/Boolean/Integer test twice there
  and in `test_host.c:186-189`; `get_rgb/point/double/int/text` (177-235) near copies:
  `split_part()`, `is_int_type()`, `param_handle()`. About 35 lines.
- [ ] **Rules shared by OFX and AE**: which types have a control (`ae/plugin.c:80-82`,
  `ofx/plugin.c:51-52`, 74, 259-261, `bench.c:207-213`) and widths / points to pixels
  (`ofx/plugin.c:265-275`, `ae/plugin.c:383-391`, `bench.c:99-104`, 248-252): inline helpers
  in `common/effect.h`. About 25 lines.
- [ ] **OFX entry points** (`ofx/plugin.c:542-576`): 32 by hand twice, and their names: one
  `MS_REPEAT32()`. About 15 lines.
- [ ] **CMake**: the editor and browser targets (355-386) repeat ImGui's sources, folders and
  signing: `millsaph_bundle_tool()`; `${bundle}` instead of the path (359-360, 375-376);
  `find_package(OpenGL)` twice (322, 362), `enable_language(CXX)` twice (57, 347); the
  "benchmark" heading (340-342) is above the editor; AE's "copy the editor" block twice
  (454-458, 487-491). About 25 lines.
- [ ] **`tools/perf/bench.c`**: the 145 cases (619-763) in a text file like `test_cases.txt`;
  worst / mean difference twice (297-300, 798-803); size arguments parsed twice (510-515,
  598-603).
- [ ] **`tools/measure/*.py`**: one `load_jobs()` (ms_render:66-70, scores:41, sheet:52,
  report:26, nuke_measure:326), `frame_pairs()` (scores:45-50, sheet:58-61), `argparse`
  instead of `argv.index("--x")` and the `print(open(__file__))` help (5 copies). About 40 lines.
- [ ] Small: `install_mac.sh` / `install_linux.sh` into one; typo "pacakge.py" in `build.sh:6`;
  `sanitize.sh:13` passes `-D` to cmake (a separate build folder, but outside "build.sh only").
  Local, ignored files to delete: `build_log.txt`, `millsaph_perf.zip`, `build-sanitize/`.

## To decide (not factoring)

- **EdgeAwareBlur's largest mask value** is computed on the CPU before the GPU runs, with a CPU
  half-to-float copy (`ms_edgeawareblur.c:342-361`): run `DT_LARGEST` on the chosen API and read
  back one float, as Distort does for its largest LOD (`ms_distort.c:270-276`).
- **Glare's spots on Metal and Direct3D 12** run on the CPU (no atomics there in Halide 21,
  `glare_generator.cpp:160-246`). No simple version without atomics: a spot can be any size,
  so a gather has no bounded window. Options, all real work: sums per tile then a merge pass;
  integer atomics if Halide's Metal codegen has them. First measure what the CPU stage costs.
  Note: float atomics on CUDA make the sums' order vary (last ulps between runs).
- **PixelSort's Mask Use** (`ms_pixelsort.c:209`) does nothing (always 0.0, line 410): hide it
  rather than remove it (saved scripts, presets).
- **Always varying over time**: JpegDamage (`ms_jpegdamage.c:332`, although nothing moves when
  Jitter Frames is 0) and LensFlare (`ms_lensflare.c:1036`): a `varying()` as Glow's would let
  hosts cache. LensFlare: list everything that depends on time first.

Not kept: Flicker's gains in C instead of its kernel (computing in C); merging `test_host` and
`bench.c` (they share little).
