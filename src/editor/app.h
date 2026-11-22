// The lens editor's state (App) and what its files share: main.cpp (the window: its panels,
// menus and tool bar), changes.cpp (the elements' paths and the changes to the lens),
// properties.cpp (the selected element's properties).

#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include <GLFW/glfw3.h>

#include "lens.h"
#include "preview.h"

namespace editor {

using lens::Json;

// An element's place: indices from the elements' array down through containers' children; a
// first index -1: the lens shadow's array instead.
using Path = std::vector<int>;

struct Thumb {
    GLuint texture = 0;
    int w = 0, h = 0;
    bool used = false;
};

// A lens preset of the dock: MillSaph's own (compiled in), then the bundle's preset files
// (presets/lensflare/*.json: the converted lenses too), by name.
struct LensPreset {
    std::string name, text;
};

// The file dialog's action.
enum FileAction { FILE_NONE, FILE_OPEN, FILE_INSERT, FILE_SAVE_AS };

struct App {
    std::vector<LensPreset> presets;
    lens::Lens lens;
    std::string out_path, status, hint;
    std::vector<Path> selected; // the last one: the one the properties show
    PreviewSettings settings;
    Preview preview;
    std::vector<uint8_t> pixels;
    GLuint texture = 0;
    int tex_w = 0, tex_h = 0;
    bool dirty = true, saved = false, quit = false, busy = false;
    // undo: the lens's texts before each change; `committed`: the lens as last recorded
    std::vector<std::string> undo, redo;
    std::string committed;
    bool changed = false;
    // the preview's own switches
    bool solo = false, preview_shadow = false, show_presets = true;
    double identify_until = 0;
    // the thumbnails (elements, palette, presets), by the text they were rendered from
    std::map<std::string, Thumb> thumbs;
    int thumbs_budget = 2;
    FileAction file_action = FILE_NONE;
    char file_path[1024] = "lens.json";
    std::string lens_path;
    char search[128] = "";
    int columns = 2;
    int gradient_stop = -1;
    bool screenshot = false;
    // a drop in the tree, done after the frame (the tree is being walked)
    bool move_pending = false, move_into = false;
    Path move_target;
    bool tree_focused = false, scroll_to_selection = false;
    Path last_primary;
};

// main.cpp
double now();
GLuint element_thumb(App &app, const Json &e, int w, int h);
void centered_title(const char *text);
bool reset_button(const char *id, float size, const char *tip);

// changes.cpp
Json *list_of(App &app, const Path &path);
Json *element_at(App &app, const Path &path);
void touch(App &app);
bool find(Json &list, const std::string &mark, Path &path);
void remove_selected(App &app);
void duplicate_selected(App &app);
void group_selected(App &app);
void move_selected(App &app, const Path &target, bool into);
void add_element(App &app, const std::string &uitype, const std::string &label);
void load_lens(App &app, const std::string &text, bool insert);

// properties.cpp
void properties_panel(App &app);

} // namespace editor
