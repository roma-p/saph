// MillSaph's lens editor (millsaph_lens_editor): edits a Lens Flare lens with a live preview
// rendered by MS_LensFlare. MS_LensFlare's Edit Lens button runs it and waits:
//   millsaph_lens_editor --in lens.json --out edited.json
// Exit code 0: saved (the lens in --out), 1: cancelled. --screenshot file.ppm renders the
// window once into a file and exits (tests); --select N selects the Nth element first.
//
// The window: menus and a tool bar, the element palette (a button per element kind), then
// the presets (left, can be hidden), the preview, the elements' tree and the selected
// element's properties in groups; the hint of what is under the mouse and OK / Cancel at the
// bottom.

#include <algorithm>
#include <cctype>
#include <functional>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include <GLFW/glfw3.h>

#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"

#include "common/effect.h"
#include "engine/engine.h"

#include "gradient.h"
#include "lens.h"
#include "preview.h"
#include "lensflare_presets.h" // (generated from presets/lensflare)

using lens::Json;

namespace {

// An element's place: indices from the elements' array down through containers' children; a
// first index -1: the lens shadow's array instead.
using Path = std::vector<int>;

struct Thumb {
    GLuint texture = 0;
    int w = 0, h = 0;
    bool used = false;
};

struct App {
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
    // file dialog: 1 open, 2 insert, 3 save as
    int file_action = 0;
    char file_path[1024] = "lens.json";
    std::string lens_path;
    char search[128] = "";
    int columns = 2;
    int gradient_stop = -1;
    bool screenshot = false;
    // a drop in the tree, done after the frame (the tree is being walked)
    bool move_pending = false, move_into = false;
    Path move_target;
    bool tree_focused = false;
    Path last_primary;
};

double now() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

std::string read_file(const std::string &path) {
    std::ifstream f(path, std::ios::binary);
    std::stringstream s;
    s << f.rdbuf();
    return s.str();
}

bool write_file(const std::string &path, const std::string &text) {
    std::ofstream f(path, std::ios::binary);
    f << text;
    return (bool)f;
}

// ------------------------------------------------------------------ paths and changes

Json *list_of(App &app, const Path &path) {
    if (path.empty())
        return nullptr;
    const bool shadow = path[0] == -1;
    Json *list = shadow ? &app.lens.shadow() : &app.lens.elements();
    for (size_t i = shadow ? 1 : 0; i + 1 < path.size(); ++i) {
        if (path[i] < 0 || path[i] >= (int)list->size())
            return nullptr;
        Json &e = (*list)[path[i]];
        if (!e.is_object() || !e.contains("children") || !e["children"].is_array())
            return nullptr;
        list = &e["children"];
    }
    return list;
}

Json *element_at(App &app, const Path &path) {
    Json *list = list_of(app, path);
    if (!list || path.size() < (path[0] == -1 ? 2u : 1u) || path.back() < 0 || path.back() >= (int)list->size())
        return nullptr;
    return &(*list)[path.back()];
}

void touch(App &app) {
    app.changed = true;
    app.dirty = true;
}

// Markers put on elements while the tree changes, so that they are found again.
const char *const MARK = "__millsaph_mark";

void strip(Json &list, const char *key) {
    if (!list.is_array())
        return;
    for (Json &e : list)
        if (e.is_object()) {
            e.erase(key);
            if (e.contains("children"))
                strip(e["children"], key);
        }
}

bool find(Json &list, const std::string &mark, Path &path) {
    if (!list.is_array())
        return false;
    for (int i = 0; i < (int)list.size(); ++i) {
        Json &e = list[i];
        if (!e.is_object())
            continue;
        path.push_back(i);
        if (e.value(MARK, std::string()) == mark)
            return true;
        if (e.contains("children") && find(e["children"], mark, path))
            return true;
        path.pop_back();
    }
    return false;
}

// The selection marked "sel" (its order kept), the markers removed after.
void mark_selection(App &app) {
    int k = 0;
    for (const Path &p : app.selected)
        if (Json *e = element_at(app, p))
            (*e)[MARK] = "sel" + std::to_string(k++);
}

void reselect(App &app, int count) {
    app.selected.clear();
    for (int k = 0; k < count; ++k) {
        Path p;
        if (find(app.lens.elements(), "sel" + std::to_string(k), p))
            app.selected.push_back(p);
        else if (Path q{-1}; find(app.lens.shadow(), "sel" + std::to_string(k), q))
            app.selected.push_back(q);
    }
    strip(app.lens.elements(), MARK);
    strip(app.lens.shadow(), MARK);
}

// The marked elements taken out of the tree (in its order).
void take_marked(Json &list, const std::string &prefix, std::vector<Json> &out) {
    if (!list.is_array())
        return;
    for (int i = 0; i < (int)list.size();) {
        Json &e = list[i];
        if (e.is_object() && e.value(MARK, std::string()).rfind(prefix, 0) == 0) {
            out.push_back(e);
            list.erase(list.begin() + i);
            continue;
        }
        if (e.is_object() && e.contains("children"))
            take_marked(e["children"], prefix, out);
        ++i;
    }
}

void remove_selected(App &app) {
    if (app.selected.empty())
        return;
    mark_selection(app);
    std::vector<Json> gone;
    take_marked(app.lens.elements(), "sel", gone);
    take_marked(app.lens.shadow(), "sel", gone);
    app.selected.clear();
    touch(app);
}

void duplicate_selected(App &app) {
    const int n = (int)app.selected.size();
    if (!n)
        return;
    mark_selection(app);
    // each copy after its element, the copies selected
    std::function<void(Json &)> walk = [&](Json &list) {
        for (int i = 0; i < (int)list.size(); ++i) {
            Json &e = list[i];
            if (!e.is_object())
                continue;
            const std::string m = e.value(MARK, std::string());
            if (m.rfind("sel", 0) == 0) {
                Json copy = e;
                e.erase(MARK);
                list.insert(list.begin() + i + 1, copy);
                ++i;
            } else if (e.contains("children")) {
                walk(e["children"]);
            }
        }
    };
    walk(app.lens.elements());
    reselect(app, n);
    touch(app);
}

int count_kind(App &app, const std::string &label) {
    int n = 0;
    std::function<void(const Json &)> walk = [&](const Json &list) {
        for (const Json &e : list)
            if (e.is_object()) {
                if (e.value("name", "").rfind(label + " ", 0) == 0)
                    ++n;
                if (e.contains("children") && e["children"].is_array())
                    walk(e["children"]);
            }
    };
    walk(app.lens.elements());
    return n;
}

void group_selected(App &app) {
    if (app.selected.empty() || app.selected[0][0] == -1)
        return;
    // the group where the first selected element (in the tree's order) was
    std::vector<Path> sel = app.selected;
    std::sort(sel.begin(), sel.end());
    Json *first = element_at(app, sel[0]);
    if (!first)
        return;
    mark_selection(app);
    Json group = lens::new_element("Group", count_kind(app, "Group") + 1);
    group[MARK] = "group";
    Json *list = list_of(app, sel[0]);
    list->insert(list->begin() + sel[0].back(), group);
    std::vector<Json> moved;
    take_marked(app.lens.elements(), "sel", moved);
    Path where;
    if (find(app.lens.elements(), "group", where)) {
        Json *g = element_at(app, where);
        for (Json &m : moved) {
            m.erase(MARK);
            (*g)["children"].push_back(m);
        }
        g->erase(MARK);
        app.selected = {where};
    }
    strip(app.lens.elements(), MARK);
    touch(app);
}

// The selection moved before `target` (into it: at the end of a group's children).
void move_selected(App &app, const Path &target, bool into) {
    for (const Path &p : app.selected)
        if (p[0] == -1 || (target.size() >= p.size() && std::equal(p.begin(), p.end(), target.begin())))
            return; // (not into itself)
    const int n = (int)app.selected.size();
    Json *t = target.empty() ? nullptr : element_at(app, target);
    mark_selection(app);
    if (t)
        (*t)[MARK] = "target";
    std::vector<Json> moved;
    take_marked(app.lens.elements(), "sel", moved);
    Path where;
    Json *list = &app.lens.elements();
    int at = (int)list->size();
    if (t && find(app.lens.elements(), "target", where)) {
        Json *e = element_at(app, where);
        if (into) {
            list = &(*e)["children"];
            at = (int)list->size();
        } else {
            list = list_of(app, where);
            at = where.back();
        }
    }
    for (Json &m : moved)
        list->insert(list->begin() + at++, m);
    reselect(app, n);
    touch(app);
}

void add_element(App &app, const std::string &uitype, const std::string &label) {
    Json e = lens::new_element(uitype, count_kind(app, label) + 1);
    app.lens.elements().push_back(e);
    app.selected = {Path{(int)app.lens.elements().size() - 1}};
    touch(app);
}

void load_lens(App &app, const std::string &text, bool insert) {
    std::string err;
    lens::Lens next;
    if (!lens::parse(text, next, err)) {
        app.status = "Cannot read the lens: " + err;
        return;
    }
    if (insert) {
        for (const Json &e : next.elements())
            app.lens.elements().push_back(e);
    } else {
        app.lens = next;
        app.selected.clear();
    }
    touch(app);
}

// The lens the preview shows: the whole lens, the selection alone (Solo Selection,
// Identify), or the lens shadow element as an element.
std::string preview_text(App &app) {
    const bool solo = app.solo || now() < app.identify_until;
    if (!solo && !app.preview_shadow)
        return lens::write(app.lens);
    lens::Lens copy;
    copy.prefix = app.lens.prefix;
    copy.doc = Json::object();
    Json &root = app.lens.root();
    if (root.contains("images"))
        copy.doc["images"] = root["images"];
    Json list = Json::array();
    if (app.preview_shadow) {
        for (Json e : app.lens.shadow())
            if (e.is_object()) {
                e["disabled"] = false;
                list.push_back(e);
            }
    } else {
        for (const Path &p : app.selected)
            if (Json *e = element_at(app, p)) {
                Json c = *e;
                c["disabled"] = false;
                list.push_back(c);
            }
    }
    copy.doc["elements"] = list;
    return lens::write(copy);
}

// ------------------------------------------------------------------ thumbnails

GLuint upload(GLuint texture, const std::vector<uint8_t> &px, int w, int h) {
    if (!texture)
        glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
    return texture;
}

// The thumbnail of one element: alone, at the frame's center, its size and brightness
// brought to the thumbnail's.
std::string element_thumb_text(App &app, const Json &e) {
    Json c = e;
    c["disabled"] = false;
    Json doc = Json::object();
    Json &root = app.lens.root();
    if (root.contains("images"))
        doc["images"] = root["images"];
    doc["elements"] = Json::array({c});
    return doc.dump();
}

// A thumbnail's texture (0 until rendered; a few rendered per frame).
GLuint thumb(App &app, const std::string &text, int w, int h, double scale_widths, double brightness,
             bool spread = false) {
    char key_tail[64];
    std::snprintf(key_tail, sizeof key_tail, "|%dx%d|%.4g|%.4g|%d", w, h, scale_widths, brightness, (int)spread);
    const std::string key = text + key_tail;
    auto it = app.thumbs.find(key);
    if (it != app.thumbs.end()) {
        it->second.used = true;
        return it->second.texture;
    }
    if (app.thumbs_budget <= 0) {
        app.busy = true; // (more frames until they are all there)
        return 0;
    }
    --app.thumbs_budget;
    PreviewSettings s;
    // one element: at the frame's center; several (groups, Multi Spot): along an axis
    s.hotspot[0] = spread ? 0.78 : 0.5, s.pivot[0] = spread ? 0.4 : 0.5;
    s.hotspot[1] = spread ? 0.62 : 0.5, s.pivot[1] = spread ? 0.45 : 0.5;
    s.scale_widths = scale_widths;
    s.brightness = brightness;
    std::vector<uint8_t> px;
    std::string err;
    Thumb t;
    if (app.preview.render(text, s, w, h, px, err))
        t.texture = upload(0, px, w, h);
    t.w = w, t.h = h, t.used = true;
    app.thumbs[key] = t;
    return t.texture;
}

GLuint element_thumb(App &app, const Json &e, int w, int h) {
    const Json *size = lens::value(e, "size"), *bright = lens::value(e, "brightness");
    const double sz = size && size->is_number() ? std::fabs(size->get<double>()) : 1.0;
    const double br = bright && bright->is_number() ? std::fabs(bright->get<double>()) : 1.0;
    // a group: its elements where they are around the pivot (unscaled)
    if (lens::is_group(e) || lens::is_generator(e))
        return thumb(app, element_thumb_text(app, e), w, h, 0.25, 1.0, true);
    return thumb(app, element_thumb_text(app, e), w, h, std::clamp(0.07 / std::fmax(sz, 0.02), 0.02, 4.0),
                 std::clamp(0.3 / std::fmax(br, 0.01), 0.1, 20.0));
}

void drop_unused_thumbs(App &app) {
    for (auto it = app.thumbs.begin(); it != app.thumbs.end();) {
        if (!it->second.used) {
            if (it->second.texture)
                glDeleteTextures(1, &it->second.texture);
            it = app.thumbs.erase(it);
        } else {
            it->second.used = false;
            ++it;
        }
    }
}

// ------------------------------------------------------------------ small widgets

void centered_title(const char *text) {
    const float w = ImGui::GetContentRegionAvail().x, t = ImGui::CalcTextSize(text).x;
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, (w - t) * 0.5f));
    ImGui::TextUnformatted(text);
}

// A reset button (a round arrow) of `size`; true when clicked.
bool reset_button(const char *id, float size, const char *tip) {
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const bool clicked = ImGui::InvisibleButton(id, ImVec2(size, size));
    const bool hot = ImGui::IsItemHovered();
    ImDrawList *d = ImGui::GetWindowDrawList();
    const ImU32 col = hot ? IM_COL32(255, 255, 255, 255) : IM_COL32(190, 190, 190, 255);
    const ImVec2 c(p.x + size * 0.5f, p.y + size * 0.55f);
    const float r = size * 0.3f;
    d->PathArcTo(c, r, -2.6f, 1.9f, 16);
    d->PathStroke(col, 0, 1.6f);
    const ImVec2 tip_at(c.x + r * std::cos(-2.6f), c.y + r * std::sin(-2.6f));
    d->AddTriangleFilled(ImVec2(tip_at.x - 4, tip_at.y - 3), ImVec2(tip_at.x + 3, tip_at.y - 3),
                         ImVec2(tip_at.x - 1, tip_at.y + 4), col);
    if (hot && tip)
        ImGui::SetTooltip("%s", tip);
    return clicked;
}

// ------------------------------------------------------------------ the elements' tree

void select_row(App &app, const Path &path) {
    const ImGuiIO &io = ImGui::GetIO();
    if (io.KeyCtrl || io.KeyShift) {
        auto it = std::find(app.selected.begin(), app.selected.end(), path);
        if (it != app.selected.end() && app.selected.size() > 1)
            app.selected.erase(it);
        else if (it == app.selected.end())
            app.selected.push_back(path);
    } else {
        app.selected = {path};
    }
}

const float ROW_H = 34.0f, THUMB_W = 48.0f, THUMB_H = 27.0f;

void tree(App &app, Json &list, Path &path, int depth) {
    for (int i = 0; i < (int)list.size(); ++i) {
        Json &e = list[i];
        if (!e.is_object())
            continue;
        path.push_back(i);
        ImGui::PushID(i);
        const bool group = lens::is_group(e);
        std::string name = e.contains("name") && e["name"].is_string() ? e["name"].get<std::string>() : "";
        if (name.empty())
            name = lens::kind_label(e);
        const bool selected = std::find(app.selected.begin(), app.selected.end(), path) != app.selected.end();
        ImGuiStorage *store = ImGui::GetStateStorage();
        const ImGuiID open_id = ImGui::GetID("open");
        bool open = store->GetBool(open_id, false);

        const ImVec2 row = ImGui::GetCursorScreenPos();
        if (ImGui::Selectable("##row", selected, ImGuiSelectableFlags_AllowOverlap, ImVec2(0, ROW_H)))
            select_row(app, path);
        if (ImGui::IsItemHovered())
            app.hint = "Shift- or Control-click to select several elements and edit them together. "
                       "Drag to reorder (onto a group: into it).";
        if (ImGui::BeginDragDropSource()) {
            if (!selected)
                app.selected = {path};
            ImGui::SetDragDropPayload("ELEMENTS", "", 1);
            ImGui::Text("%s%s", name.c_str(), app.selected.size() > 1 ? " ..." : "");
            ImGui::EndDragDropSource();
        }
        if (ImGui::BeginDragDropTarget()) {
            if (ImGui::AcceptDragDropPayload("ELEMENTS")) {
                app.move_pending = true;
                app.move_target = path;
                app.move_into = group;
            }
            ImGui::EndDragDropTarget();
        }
        // over the row: the arrow (groups), the checkbox, the thumbnail, the name
        const float x0 = row.x + 4 + depth * 20.0f, cy = row.y + ROW_H * 0.5f;
        ImGui::SetCursorScreenPos(ImVec2(x0, cy - 9));
        if (group) {
            if (ImGui::ArrowButton("##open", open ? ImGuiDir_Down : ImGuiDir_Right)) {
                open = !open;
                store->SetBool(open_id, open);
            }
        }
        ImGui::SetCursorScreenPos(ImVec2(x0 + 24, cy - ImGui::GetFrameHeight() * 0.5f));
        bool on = !(e.contains("disabled") && e["disabled"].is_boolean() && e["disabled"].get<bool>());
        if (ImGui::Checkbox("##on", &on)) {
            e["disabled"] = !on;
            touch(app);
        }
        const ImVec2 tp(x0 + 52, cy - THUMB_H * 0.5f);
        ImDrawList *d = ImGui::GetWindowDrawList();
        d->AddRectFilled(tp, ImVec2(tp.x + THUMB_W, tp.y + THUMB_H), IM_COL32(0, 0, 0, 255));
        if (GLuint t = element_thumb(app, e, 96, 54))
            d->AddImage((ImTextureID)(intptr_t)t, tp, ImVec2(tp.x + THUMB_W, tp.y + THUMB_H));
        d->AddText(ImVec2(tp.x + THUMB_W + 8, cy - ImGui::GetTextLineHeight() * 0.5f),
                   ImGui::GetColorU32(on ? ImGuiCol_Text : ImGuiCol_TextDisabled), name.c_str());
        ImGui::SetCursorScreenPos(ImVec2(row.x, row.y + ROW_H + ImGui::GetStyle().ItemSpacing.y));
        ImGui::Dummy(ImVec2(0, 0));
        if (group && open) {
            if (!e.contains("children") || !e["children"].is_array())
                e["children"] = Json::array();
            tree(app, e["children"], path, depth + 1);
        }
        ImGui::PopID();
        path.pop_back();
    }
}

Json new_shadow() {
    Json s = lens::new_element("Polygon", 1);
    s["name"] = "New Shadow";
    s["uitype"] = "Shadow";
    s["center"] = 0.0;
    s["size"] = 0.5;
    s["roundness"] = 0.83;
    s["blur_gradient"] = 0.04;
    s["brightness"] = 1.0;
    return s;
}

void elements_panel(App &app) {
    centered_title("Elements");
    ImGui::Separator();
    const float bottom = ImGui::GetFrameHeightWithSpacing() * 4 + ImGui::GetTextLineHeightWithSpacing() + 8;
    ImGui::BeginChild("tree", ImVec2(0, -bottom));
    app.tree_focused = ImGui::IsWindowFocused();
    Path path;
    tree(app, app.lens.elements(), path, 0);
    // dropped under the rows: at the end
    ImGui::Dummy(ImVec2(ImGui::GetContentRegionAvail().x, std::max(20.0f, ImGui::GetContentRegionAvail().y)));
    if (ImGui::BeginDragDropTarget()) {
        if (ImGui::AcceptDragDropPayload("ELEMENTS")) {
            app.move_pending = true;
            app.move_target.clear();
            app.move_into = false;
        }
        ImGui::EndDragDropTarget();
    }
    ImGui::EndChild();

    ImGui::Separator();
    centered_title("Lens Shadow Element");
    const float w = ImGui::GetContentRegionAvail().x;
    if (ImGui::Button("Shadow Parameters", ImVec2(w, 0))) {
        if (app.lens.shadow().empty()) {
            app.lens.shadow().push_back(new_shadow());
            touch(app);
        }
        app.selected = {Path{-1, 0}};
    }
    if (ImGui::IsItemHovered())
        app.hint = "The lens shadow element: the shape the elements' shadows take.";
    if (ImGui::Selectable("Preview Shadow Element", &app.preview_shadow, 0, ImVec2(w, 0)))
        app.dirty = true;
    ImGui::Spacing();
    const bool any = !app.selected.empty();
    ImGui::BeginDisabled(!any);
    if (ImGui::Button("Remove"))
        remove_selected(app);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Remove selected element(s)  (Delete)");
    ImGui::SameLine();
    if (ImGui::Button("Duplicate"))
        duplicate_selected(app);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Duplicate selected element(s)  (Ctrl+D)");
    ImGui::SameLine();
    if (ImGui::Button("Group"))
        group_selected(app);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Group selected elements together  (Ctrl+G)");
    ImGui::EndDisabled();
}

// ------------------------------------------------------------------ the properties

double number(const Json &e, const char *key, double def) {
    const Json *v = lens::value(e, key);
    return v && v->is_number() ? v->get<double>() : v && v->is_boolean() ? (v->get<bool>() ? 1 : 0) : def;
}

const float LABEL_W = 150.0f, VALUE_W = 70.0f;

// A row's label, right-aligned in its column (greyed when it does nothing).
void row_label(const char *label, bool grey) {
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::AlignTextToFramePadding();
    const float w = ImGui::GetContentRegionAvail().x, t = ImGui::CalcTextSize(label).x;
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, w - t));
    if (grey)
        ImGui::TextDisabled("%s", label);
    else
        ImGui::TextUnformatted(label);
}

std::vector<std::string> split(const char *choices) {
    std::vector<std::string> items;
    for (const char *p = choices; p && *p;) {
        const char *bar = std::strchr(p, '|');
        items.emplace_back(p, bar ? (size_t)(bar - p) : std::strlen(p));
        p = bar ? bar + 1 : p + std::strlen(p);
    }
    return items;
}

std::string title_case(std::string s) {
    for (size_t i = 0; i < s.size(); ++i)
        if (i == 0 || s[i - 1] == ' ')
            s[i] = (char)std::toupper((unsigned char)s[i]);
    return s;
}

// One field's row: the value box and the slider (numbers), or its control. True when changed.
bool edit_field(Json &e, const lens::Field &f, bool grey, bool relative) {
    const char *key = f.name;
    bool changed = false;
    Json now;
    row_label(f.label, grey);
    ImGui::PushID(key);
    ImGui::BeginDisabled(grey);
    ImGui::TableSetColumnIndex(1);
    const lens::Mode mode = relative ? lens::mode(e, key) : lens::Mode::Overwrite;
    const char *fmt = mode == lens::Mode::Scale ? "x%.3f" : mode == lens::Mode::Offset ? "%+.3f" : "%.3f";
    switch (f.kind) {
    case lens::Kind::Number:
    case lens::Kind::Integer: {
        const bool integer = f.kind == lens::Kind::Integer;
        float v = (float)number(e, key, f.def);
        const float speed = (float)(f.max - f.min) / 400.0f;
        ImGui::SetNextItemWidth(VALUE_W);
        bool c = integer ? false : ImGui::DragFloat("##v", &v, speed, 0, 0, fmt);
        int iv = (int)std::lround(v);
        if (integer) {
            c = ImGui::DragInt("##v", &iv, 0.1f);
            v = (float)iv;
        }
        ImGui::TableSetColumnIndex(2);
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (integer) {
            c |= ImGui::SliderInt("##s", &iv, (int)f.min, (int)f.max, "");
            v = (float)iv;
        } else {
            c |= ImGui::SliderFloat("##s", &v, (float)f.min, (float)f.max, "");
        }
        if (c) {
            now = integer ? Json((int)v) : Json((double)v);
            changed = true;
        }
        break;
    }
    case lens::Kind::Bool: {
        bool v = number(e, key, f.def) != 0;
        if (ImGui::Checkbox("##v", &v)) {
            now = v ? 1 : 0;
            changed = true;
        }
        break;
    }
    case lens::Kind::Flag:
    case lens::Kind::Choice:
    case lens::Kind::Shape: {
        const std::vector<std::string> items = split(f.choices);
        int at = 0;
        if (f.kind == lens::Kind::Flag)
            at = number(e, key, f.def) != 0 ? 1 : 0;
        else if (f.kind == lens::Kind::Shape)
            at = number(e, "n_sides", 0) > 0 ? 1 : 0;
        else if (const Json *v = lens::value(e, key); v && v->is_string())
            for (int k = 0; k < (int)items.size(); ++k)
                if (items[k] == v->get<std::string>())
                    at = k;
        ImGui::SetNextItemWidth(VALUE_W + 60);
        if (ImGui::BeginCombo("##v", title_case(items[at]).c_str())) {
            for (int k = 0; k < (int)items.size(); ++k)
                if (ImGui::Selectable(title_case(items[k]).c_str(), k == at) && k != at) {
                    if (f.kind == lens::Kind::Flag)
                        now = k;
                    else if (f.kind == lens::Kind::Shape) {
                        lens::set_field(e, "n_sides", k ? 6 : 0);
                        now = k ? "polygon" : "circle";
                    } else
                        now = items[k];
                    changed = true;
                }
            ImGui::EndCombo();
        }
        break;
    }
    case lens::Kind::Color: {
        float c[3] = {1, 1, 1};
        if (const Json *v = lens::value(e, key); v && v->is_array())
            for (int k = 0; k < 3 && k < (int)v->size(); ++k)
                c[k] = (*v)[k].get<float>();
        if (ImGui::ColorEdit3("##v", c, ImGuiColorEditFlags_Float | ImGuiColorEditFlags_HDR |
                                            ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_PickerHueWheel)) {
            now = {c[0], c[1], c[2]};
            changed = true;
        }
        break;
    }
    case lens::Kind::Pair: {
        float v[2] = {(float)f.def, (float)f.def};
        if (const Json *j = lens::value(e, key); j && j->is_array())
            for (int k = 0; k < 2 && k < (int)j->size(); ++k)
                v[k] = (*j)[k].get<float>();
        ImGui::TableSetColumnIndex(2);
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::SliderFloat2("##v", v, (float)f.min, (float)f.max, "%.3f")) {
            now = {v[0], v[1]};
            changed = true;
        }
        break;
    }
    case lens::Kind::Text:
        break;
    }
    ImGui::EndDisabled();
    ImGui::PopID();
    if (changed)
        lens::set_field(e, key, now);
    return changed;
}

// Texture Overlay's image: none, one of MillSaph's generated textures, or one in the lens.
bool edit_image(Json &e) {
    bool changed = false;
    const bool has = e.contains("image") && e["image"].is_object();
    const bool generated = has && e["image"].contains("generate");
    const char *now = !has ? "None" : generated ? "Generated" : "In the lens";
    row_label("Image", false);
    ImGui::TableSetColumnIndex(1);
    ImGui::SetNextItemWidth(VALUE_W + 60);
    if (ImGui::BeginCombo("##image", now)) {
        if (ImGui::Selectable("None", !has)) {
            e.erase("image");
            changed = true;
        }
        if (ImGui::Selectable("Generated", generated)) {
            e["image"] = {{"generate", "dirt"}, {"seed", 1}};
            changed = true;
        }
        ImGui::EndCombo();
    }
    if (has && !generated) {
        ImGui::TableSetColumnIndex(2);
        ImGui::TextDisabled("%d x %d texels", (int)number(e["image"], "xdim", 0), (int)number(e["image"], "ydim", 0));
    }
    if (has && generated) {
        Json &im = e["image"];
        static const char *const kinds[] = {"dirt", "glint", "ramp", "iris"};
        const std::string kind = im.value("generate", "dirt");
        row_label("Kind", false);
        ImGui::TableSetColumnIndex(1);
        ImGui::SetNextItemWidth(VALUE_W + 60);
        if (ImGui::BeginCombo("##kind", title_case(kind).c_str())) {
            for (const char *k : kinds)
                if (ImGui::Selectable(title_case(k).c_str(), kind == k)) {
                    im["generate"] = k;
                    changed = true;
                }
            ImGui::EndCombo();
        }
        struct Knob {
            const char *key, *label;
            double def, min, max;
            bool integer;
        };
        const Knob knobs[] = {
            {"seed", "Seed", 0, 0, 100, true},
            {"amount", "Amount", kind == "dirt" ? 0.35 : kind == "ramp" ? 1.0 : 0.5, 0, 1, false},
            {"scale", "Scale", 1, 0.1, 4, false},
            {"detail", "Detail", 0.5, 0, 1, false},
        };
        for (const Knob &k : knobs) {
            row_label(k.label, false);
            ImGui::PushID(k.key);
            ImGui::TableSetColumnIndex(1);
            ImGui::SetNextItemWidth(VALUE_W);
            float v = (float)number(im, k.key, k.def);
            bool c = ImGui::DragFloat("##v", &v, (float)(k.max - k.min) / 400.0f, 0, 0, k.integer ? "%.0f" : "%.3f");
            ImGui::TableSetColumnIndex(2);
            ImGui::SetNextItemWidth(-FLT_MIN);
            c |= ImGui::SliderFloat("##s", &v, (float)k.min, (float)k.max, "");
            if (c) {
                im[k.key] = k.integer ? Json((int)std::lround(v)) : Json((double)v);
                changed = true;
            }
            ImGui::PopID();
        }
        if (kind == "iris" || kind == "ramp") {
            row_label(kind == "iris" ? "Blades" : "Linear", false);
            ImGui::TableSetColumnIndex(1);
            ImGui::SetNextItemWidth(VALUE_W);
            int sides = (int)number(im, "sides", kind == "iris" ? 6 : 0);
            if (ImGui::DragInt("##sides", &sides, 0.1f, 0, kind == "iris" ? 12 : 1)) {
                im["sides"] = sides;
                changed = true;
            }
        }
    }
    return changed;
}

// A group header's open state, kept in the element (view_properties, as the lens files).
bool group_collapsed(const Json &e, const std::string &group, bool def) {
    if (e.contains("view_properties") && e["view_properties"].is_object()) {
        const Json &v = e["view_properties"];
        if (v.contains("ElementView") && v["ElementView"].contains("groups_collapsed") &&
            v["ElementView"]["groups_collapsed"].contains(group) &&
            v["ElementView"]["groups_collapsed"][group].is_boolean())
            return v["ElementView"]["groups_collapsed"][group].get<bool>();
    }
    return def;
}

bool begin_table() {
    if (!ImGui::BeginTable("rows", 3, ImGuiTableFlags_SizingFixedFit))
        return false;
    ImGui::TableSetupColumn("label", ImGuiTableColumnFlags_WidthFixed, LABEL_W);
    ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthFixed, VALUE_W);
    ImGui::TableSetupColumn("slider", ImGuiTableColumnFlags_WidthStretch);
    return true;
}

// A group's header: its triangle, its name (dim when the group does nothing) and its reset
// button. True when open.
bool group_header(App &app, Json &e, const std::string &group, const std::string &title, bool dim,
                  bool def_collapsed, bool &reset) {
    const bool collapsed = group_collapsed(e, group, def_collapsed);
    ImGui::SetNextItemOpen(!collapsed, ImGuiCond_Always);
    if (dim)
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0, 0, 0, 0));
    const bool open = ImGui::CollapsingHeader(title.c_str(), ImGuiTreeNodeFlags_AllowOverlap);
    ImGui::PopStyleColor(dim ? 2 : 1);
    if (open == collapsed) { // toggled: kept in the lens, not an undo step
        e["view_properties"]["ElementView"]["groups_collapsed"][group] = !open;
        if (!app.changed)
            app.committed = lens::write(app.lens);
    }
    ImGui::SameLine();
    ImGui::SetCursorPosX(ImGui::GetWindowContentRegionMax().x - 20);
    reset = reset_button(("reset" + group).c_str(), 18, ("Reset " + title).c_str());
    return open;
}

// Whether a field is at its default (left out, or its default's value).
bool at_default(const Json &e, const lens::Field &f) {
    const Json *v = lens::value(e, f.name);
    if (!v || v->is_null())
        return true;
    if (v->is_number() || v->is_boolean())
        return std::fabs(number(e, f.name, f.def) - f.def) < 1e-9;
    if (f.kind == lens::Kind::Color && v->is_array()) {
        for (const Json &c : *v)
            if (!c.is_number() || c.get<double>() != f.def)
                return false;
        return true;
    }
    return false;
}

void reset_field(Json &e, const lens::Field &f) {
    if (e.value("type", "") == "container") {
        if (f.kind == lens::Kind::Color)
            lens::set_field(e, f.name, Json{f.def, f.def, f.def});
        else if (f.kind == lens::Kind::Pair)
            lens::set_field(e, f.name, Json{f.def, f.def});
        else if (f.kind == lens::Kind::Number || f.kind == lens::Kind::Integer || f.kind == lens::Kind::Bool ||
                 f.kind == lens::Kind::Flag)
            lens::set_field(e, f.name, f.def);
        e.erase(f.name);
    } else {
        e.erase(f.name);
    }
    if (f.kind == lens::Kind::Shape)
        e.erase("n_sides");
}

// The fields of `group` for this element.
std::vector<const lens::Field *> group_fields(const Json &e, const std::string &group) {
    std::vector<const lens::Field *> out;
    const std::string type = lens::engine_type(e);
    const bool gen = lens::is_generator(e), grp = lens::is_group(e);
    for (const lens::Field &f : lens::fields()) {
        if (group != f.group)
            continue;
        const bool gen_field = !std::strcmp(f.types, "generator");
        const bool show = gen_field ? gen : grp ? group != "Options" : lens::field_for(f, type);
        if (show)
            out.push_back(&f);
    }
    return out;
}

void properties_panel(App &app) {
    centered_title("Properties");
    ImGui::Separator();
    Json *e = app.selected.empty() ? nullptr : element_at(app, app.selected.back());
    if (!e) {
        ImGui::TextDisabled("Select an element.");
        return;
    }
    const Json before = *e;
    const bool shadow = app.selected.back()[0] == -1;
    const bool grp = lens::is_group(*e);
    const std::string type = lens::engine_type(*e);
    const std::string kind = shadow ? "Shadow" : lens::kind_label(*e);
    bool changed = false, gradient_changed = false;
    ImGui::BeginChild("fields", ImVec2(0, -ImGui::GetFrameHeightWithSpacing() - 4));

    // Identity
    bool reset = false;
    if (group_header(app, *e, "Identity", "Identity", false, false, reset)) {
        const float w = ImGui::GetContentRegionAvail().x;
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 150 + (w - 150 - 80) * 0.5f);
        if (ImGui::Button("Identify", ImVec2(80, 0)))
            app.identify_until = now() + 0.6;
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Show this element alone in the preview for a moment");
        const ImVec2 p = ImGui::GetCursorScreenPos();
        ImDrawList *d = ImGui::GetWindowDrawList();
        d->AddRectFilled(p, ImVec2(p.x + 128, p.y + 72), IM_COL32(0, 0, 0, 255));
        if (GLuint t = element_thumb(app, *e, 192, 108))
            d->AddImage((ImTextureID)(intptr_t)t, p, ImVec2(p.x + 128, p.y + 72));
        ImGui::SetCursorScreenPos(ImVec2(p.x + 140, p.y + 10));
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Name:");
        ImGui::SetCursorScreenPos(ImVec2(p.x + 190, p.y + 10));
        char buf[256];
        std::snprintf(buf, sizeof buf, "%s", e->value("name", std::string()).c_str());
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::InputText("##name", buf, sizeof buf)) {
            (*e)["name"] = std::string(buf);
            changed = true;
        }
        ImGui::SetCursorScreenPos(ImVec2(p.x + 140, p.y + 44));
        ImGui::TextUnformatted("Type:");
        ImGui::SetCursorScreenPos(ImVec2(p.x + 190, p.y + 44));
        ImGui::TextUnformatted(kind.c_str());
        ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + 80));
        ImGui::Dummy(ImVec2(0, 0));
    }

    for (const char *group_name : lens::groups()) {
        const std::string group = group_name;
        std::vector<const lens::Field *> list = group_fields(*e, group);
        const bool stops = group == "Color and Brightness" && lens::has_gradient(*e);
        const bool image = group == "Texture Overlay" && !grp;
        if (list.empty() && !stops && !image)
            continue;
        if (shadow && (group == "Animation" || group == "Triggers" || group == "Shadow"))
            continue;
        std::string title = group == "Options" ? (shadow ? "Shadow" : kind) + " Options" : group;
        // dim: no effect (all at their defaults, no image)
        bool dim = group != "Color and Brightness" && group != "Position" && group != "Size" && group != "Options";
        for (const lens::Field *f : list)
            dim &= at_default(*e, *f);
        if (image && e->contains("image"))
            dim = false;
        const bool def_collapsed = !(group == "Position" || group == "Size" || group == "Color and Brightness" ||
                                     group == "Options");
        ImGui::PushID(group_name);
        const bool open = group_header(app, *e, group, title, dim, def_collapsed, reset);
        if (reset) {
            for (const lens::Field *f : list)
                reset_field(*e, *f);
            if (image)
                e->erase("image");
            changed = true;
        }
        if (open && begin_table()) {
            if (image)
                changed |= edit_image(*e);
            for (const lens::Field *f : list) {
                bool grey = lens::field_grey(*f, type);
                // Sides, Roundness, Randomize Sides: for polygons
                if ((!std::strcmp(f->name, "roundness") || !std::strcmp(f->name, "random_side_lengths") ||
                     !std::strcmp(f->name, "n_sides")) &&
                    number(*e, "n_sides", 0) <= 0)
                    grey = true;
                changed |= edit_field(*e, *f, grey, grp || lens::is_generator(*e));
                if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
                    reset_field(*e, *f);
                    changed = true;
                }
                // the gradient after the colour
                if (stops && (!std::strcmp(f->name, "tint") || !std::strcmp(f->name, "color"))) {
                    row_label("Gradient:", false);
                    ImGui::EndTable();
                    gradient_changed |= gradient_edit(*e, app.gradient_stop, app.hint);
                    if (!begin_table())
                        break;
                }
            }
            ImGui::EndTable();
        }
        ImGui::PopID();
    }
    ImGui::EndChild();
    if (reset_button("resetall", 22, "Reset All Properties to Default Values")) {
        const std::string name = e->value("name", std::string());
        for (const lens::Field &f : lens::fields())
            if (e->contains(f.name) || lens::value(*e, f.name))
                reset_field(*e, f);
        e->erase("image");
        (*e)["name"] = name;
        changed = true;
    }
    if (changed || gradient_changed) {
        // the generators made again; the other selected elements given the same changes
        bool regenerate = false;
        Json diff = Json::object();
        for (auto it = e->begin(); it != e->end(); ++it)
            if (it.key() != "children" && it.key() != "name" && it.key() != "view_properties" &&
                (!before.contains(it.key()) || before[it.key()] != it.value())) {
                diff[it.key()] = it.value();
                regenerate |= lens::generator_field(it.key());
            }
        if (lens::is_generator(*e) && (regenerate || diff.contains("center") || diff.contains("size") ||
                                       diff.contains("brightness") || diff.contains("rotate") ||
                                       diff.contains("tint") || diff.contains("color") || diff.contains("rand_seed")))
            lens::generate(*e);
        Json gone = Json::array();
        for (auto it = before.begin(); it != before.end(); ++it)
            if (!e->contains(it.key()))
                gone.push_back(it.key());
        for (size_t k = 0; k + 1 < app.selected.size(); ++k)
            if (Json *o = element_at(app, app.selected[k]); o && o != e) {
                for (auto it = diff.begin(); it != diff.end(); ++it)
                    lens::set_field(*o, it.key(), it.value());
                for (const Json &g : gone)
                    o->erase(g.get<std::string>());
                if (lens::is_generator(*o))
                    lens::generate(*o);
            }
        touch(app);
    }
}

// ------------------------------------------------------------------ the preview

void preview_panel(App &app) {
    ImVec2 room = ImGui::GetContentRegionAvail();
    room.y -= ImGui::GetFrameHeightWithSpacing() * 2 + 6;
    int w = (int)room.x, h = (int)(room.x * 9 / 16);
    if (h > room.y)
        h = (int)room.y, w = h * 16 / 9;
    w = std::max(w, 64), h = std::max(h, 36);
    const bool identify = now() < app.identify_until;
    static bool was_identify = false;
    if (identify != was_identify)
        app.dirty = true;
    was_identify = identify;
    if (identify)
        app.busy = true;
    if (app.dirty || w != app.tex_w || h != app.tex_h) {
        std::string err;
        if (app.preview.render(preview_text(app), app.settings, w, h, app.pixels, err)) {
            app.texture = upload(app.texture, app.pixels, w, h);
            app.tex_w = w, app.tex_h = h;
            app.status = std::string("Preview on ") + app.preview.api();
        } else {
            app.status = "Preview failed: " + err;
        }
        app.dirty = false;
    }
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, (room.x - w) * 0.5f));
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + std::max(0.0f, (room.y - h) * 0.5f));
    const ImVec2 at = ImGui::GetCursorScreenPos();
    ImGui::Image((ImTextureID)(intptr_t)app.texture, ImVec2((float)w, (float)h));
    // drag on it: left the hotspot, right the pivot
    if (ImGui::IsItemHovered()) {
        app.hint = "Left-drag: the hotspot. Right-drag: the pivot. (The preview's only.)";
        const ImVec2 m = ImGui::GetMousePos();
        const double fx = (m.x - at.x) / w, fy = 1.0 - (m.y - at.y) / h;
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            app.settings.hotspot[0] = fx, app.settings.hotspot[1] = fy;
            app.dirty = true;
        } else if (ImGui::IsMouseDown(ImGuiMouseButton_Right)) {
            app.settings.pivot[0] = fx, app.settings.pivot[1] = fy;
            app.dirty = true;
        }
    }
    ImDrawList *draw = ImGui::GetWindowDrawList();
    const ImVec2 hot(at.x + (float)app.settings.hotspot[0] * w, at.y + (float)(1 - app.settings.hotspot[1]) * h);
    const ImVec2 piv(at.x + (float)app.settings.pivot[0] * w, at.y + (float)(1 - app.settings.pivot[1]) * h);
    draw->AddLine(hot, piv, IM_COL32(255, 255, 255, 60));
    draw->AddCircle(hot, 6, IM_COL32(255, 220, 120, 200), 0, 1.5f);
    draw->AddCircle(piv, 4, IM_COL32(120, 200, 255, 200), 0, 1.5f);
    ImGui::SetCursorPosY(ImGui::GetWindowContentRegionMax().y - ImGui::GetFrameHeightWithSpacing() * 2);

    // the preview's settings (none of them in the lens)
    float sw = (float)app.settings.scale_widths, br = (float)app.settings.brightness, ga = (float)app.settings.gamma;
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Scale Widths:");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(120);
    if (ImGui::SliderFloat("##sw", &sw, 0.02f, 1.0f))
        app.settings.scale_widths = sw, app.dirty = true;
    ImGui::SameLine();
    ImGui::TextUnformatted("Background:");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(90);
    if (ImGui::Combo("##bg", &app.settings.background, "Black\0Grey\0Dusk\0"))
        app.dirty = true;
    ImGui::SameLine();
    ImGui::TextDisabled("%d x %d", w, h);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", app.status.c_str());

    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Gamma:");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(70);
    if (ImGui::DragFloat("##gamma", &ga, 0.01f, 0.2f, 4.0f, "%.3f"))
        app.settings.gamma = ga, app.dirty = true;
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Preview Gamma (doesn't change the flare)");
    ImGui::SameLine();
    ImGui::TextUnformatted("Brightness:");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(70);
    if (ImGui::DragFloat("##bright", &br, 0.01f, 0.0f, 10.0f, "%.3f"))
        app.settings.brightness = br, app.dirty = true;
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Preview Brightness (doesn't change the flare)");
    ImGui::SameLine(0, 30);
    if (ImGui::Checkbox("Solo Selection", &app.solo))
        app.dirty = true;
}

// ------------------------------------------------------------------ the presets

void presets_panel(App &app) {
    centered_title("Flare Presets");
    ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - 18);
    if (ImGui::SmallButton("x"))
        app.show_presets = false;
    ImGui::Separator();
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputTextWithHint("##search", "Search presets", app.search, sizeof app.search);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Columns:");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(60);
    ImGui::DragInt("##columns", &app.columns, 0.05f, 1, 4);
    ImGui::Separator();
    ImGui::BeginChild("grid");
    const float spacing = ImGui::GetStyle().ItemSpacing.x;
    const float tw = (ImGui::GetContentRegionAvail().x - spacing * (app.columns - 1)) / app.columns;
    const float th = tw * 9 / 16;
    int shown = 0;
    std::string needle = app.search;
    std::transform(needle.begin(), needle.end(), needle.begin(), ::tolower);
    for (int i = 0; i < MS_LENSFLARE_PRESETS; ++i) {
        std::string name = ms_lensflare_preset_menu[i + 1], low = name;
        std::transform(low.begin(), low.end(), low.begin(), ::tolower);
        if (!needle.empty() && low.find(needle) == std::string::npos)
            continue;
        if (shown++ % app.columns)
            ImGui::SameLine();
        ImGui::BeginGroup();
        ImGui::PushID(i);
        const ImVec2 p = ImGui::GetCursorScreenPos();
        if (ImGui::InvisibleButton("preset", ImVec2(tw, th))) {
            load_lens(app, ms_lensflare_preset_texts[i], false);
        }
        const bool hot = ImGui::IsItemHovered();
        if (hot)
            app.hint = "Click to load the preset, replacing all the current elements.";
        ImDrawList *d = ImGui::GetWindowDrawList();
        d->AddRectFilled(p, ImVec2(p.x + tw, p.y + th), IM_COL32(0, 0, 0, 255), 4);
        PreviewSettings s;
        if (GLuint t = thumb(app, ms_lensflare_preset_texts[i], 256, 144, s.scale_widths, 1.0))
            d->AddImageRounded((ImTextureID)(intptr_t)t, p, ImVec2(p.x + tw, p.y + th), ImVec2(0, 0),
                               ImVec2(1, 1), IM_COL32_WHITE, 4);
        if (hot)
            d->AddRect(p, ImVec2(p.x + tw, p.y + th), IM_COL32(80, 150, 255, 255), 4, 0, 2);
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + tw);
        ImGui::TextUnformatted(name.c_str());
        ImGui::PopTextWrapPos();
        ImGui::Spacing();
        ImGui::PopID();
        ImGui::EndGroup();
    }
    ImGui::EndChild();
}

// ------------------------------------------------------------------ the window

void do_undo(App &app, bool redo) {
    std::vector<std::string> &from = redo ? app.redo : app.undo, &to = redo ? app.undo : app.redo;
    if (from.empty())
        return;
    to.push_back(app.committed);
    app.committed = from.back();
    from.pop_back();
    std::string err;
    lens::parse(app.committed, app.lens, err);
    app.selected.erase(std::remove_if(app.selected.begin(), app.selected.end(),
                                      [&](const Path &p) { return !element_at(app, p); }),
                       app.selected.end());
    app.dirty = true;
    app.changed = false;
}

void save(App &app, const std::string &path) {
    const bool done = write_file(path, lens::write(app.lens));
    app.status = done ? "Saved " + path : "Cannot save " + path;
    if (done)
        app.lens_path = path;
}

void open_file_dialog(App &app, int action) {
    app.file_action = action;
    std::snprintf(app.file_path, sizeof app.file_path, "%s", app.lens_path.empty() ? "lens.json" : app.lens_path.c_str());
}

void menus(App &app) {
    if (!ImGui::BeginMenuBar())
        return;
    if (ImGui::BeginMenu("File")) {
        if (ImGui::MenuItem("New Lens", "Ctrl+N")) {
            load_lens(app, "{\"elements\": []}", false);
            app.lens_path.clear();
        }
        if (ImGui::MenuItem("Open Lens...", "Ctrl+O"))
            open_file_dialog(app, 1);
        if (ImGui::MenuItem("Insert Lens...", "Ctrl+Shift+O"))
            open_file_dialog(app, 2);
        if (ImGui::MenuItem("Save Lens", "Ctrl+S")) {
            if (app.lens_path.empty())
                open_file_dialog(app, 3);
            else
                save(app, app.lens_path);
        }
        if (ImGui::MenuItem("Save Lens As...", "Ctrl+Shift+S"))
            open_file_dialog(app, 3);
        ImGui::Separator();
        if (ImGui::MenuItem("Quit (Cancel)"))
            app.quit = true;
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Edit")) {
        if (ImGui::MenuItem("Undo", "Ctrl+Z", false, !app.undo.empty()))
            do_undo(app, false);
        if (ImGui::MenuItem("Redo", "Ctrl+Shift+Z", false, !app.redo.empty()))
            do_undo(app, true);
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Element")) {
        const bool any = !app.selected.empty();
        if (ImGui::MenuItem("Identify", "Ctrl+I", false, any))
            app.identify_until = now() + 0.6;
        if (ImGui::MenuItem("Remove", "Delete", false, any))
            remove_selected(app);
        if (ImGui::MenuItem("Duplicate", "Ctrl+D", false, any))
            duplicate_selected(app);
        if (ImGui::MenuItem("Group", "Ctrl+G", false, any))
            group_selected(app);
        ImGui::Separator();
        for (const lens::ElementKind &k : lens::element_kinds())
            if (ImGui::MenuItem((std::string("Add ") + k.label).c_str()))
                add_element(app, k.uitype, k.label);
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("View")) {
        if (ImGui::MenuItem("Solo Selection", nullptr, &app.solo))
            app.dirty = true;
        if (ImGui::MenuItem("Preview Shadow Element", nullptr, &app.preview_shadow))
            app.dirty = true;
        ImGui::Separator();
        ImGui::MenuItem("Flare Presets", nullptr, &app.show_presets);
        ImGui::EndMenu();
    }
    ImGui::EndMenuBar();
}

void toolbar(App &app) {
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
    if (ImGui::Button("New")) {
        load_lens(app, "{\"elements\": []}", false);
        app.lens_path.clear();
    }
    ImGui::SameLine();
    if (ImGui::Button("Load"))
        open_file_dialog(app, 1);
    ImGui::SameLine();
    if (ImGui::Button("Insert"))
        open_file_dialog(app, 2);
    ImGui::SameLine();
    if (ImGui::Button("Save")) {
        if (app.lens_path.empty())
            open_file_dialog(app, 3);
        else
            save(app, app.lens_path);
    }
    ImGui::SameLine();
    if (ImGui::Button("Save As"))
        open_file_dialog(app, 3);
    ImGui::SameLine(0, 20);
    ImGui::BeginDisabled(app.undo.empty());
    if (ImGui::ArrowButton("undo", ImGuiDir_Left))
        do_undo(app, false);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Undo (Ctrl+Z)");
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(app.redo.empty());
    if (ImGui::ArrowButton("redo", ImGuiDir_Right))
        do_undo(app, true);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Redo (Ctrl+Shift+Z)");
    ImGui::EndDisabled();
    ImGui::PopStyleColor();
}

// The element palette: a button per element kind, its default element's thumbnail on it.
void palette(App &app) {
    const float size = 64;
    for (const lens::ElementKind &k : lens::element_kinds()) {
        ImGui::PushID(k.uitype);
        const ImVec2 p = ImGui::GetCursorScreenPos();
        if (ImGui::InvisibleButton("add", ImVec2(size, size)))
            add_element(app, k.uitype, k.label);
        const bool hot = ImGui::IsItemHovered();
        if (hot) {
            ImGui::SetTooltip("Add %s", k.label);
            app.hint = "Click to add the element at the end of the lens.";
        }
        ImDrawList *d = ImGui::GetWindowDrawList();
        d->AddRectFilled(p, ImVec2(p.x + size, p.y + size), IM_COL32(0, 0, 0, 255), 4);
        static std::map<std::string, std::string> texts;
        std::string &text = texts[k.uitype];
        if (text.empty()) {
            Json e = lens::new_element(k.uitype, 1);
            Json doc = {{"elements", Json::array({e})}};
            text = doc.dump();
        }
        const Json e = lens::new_element(k.uitype, 1);
        const double sz = lens::value(e, "size") ? lens::value(e, "size")->get<double>() : 1.0;
        const double br = lens::value(e, "brightness") ? lens::value(e, "brightness")->get<double>() : 1.0;
        const bool multi = lens::is_generator(e);
        if (GLuint t = thumb(app, text, 96, 96, multi ? 0.3 : std::clamp(0.08 / sz, 0.02, 4.0),
                             std::clamp(0.3 / br, 0.1, 20.0), multi))
            d->AddImageRounded((ImTextureID)(intptr_t)t, p, ImVec2(p.x + size, p.y + size), ImVec2(0, 0),
                               ImVec2(1, 1), IM_COL32_WHITE, 4);
        d->AddRect(p, ImVec2(p.x + size, p.y + size), hot ? IM_COL32(80, 150, 255, 255) : IM_COL32(70, 70, 70, 255), 4);
        ImGui::PopID();
        ImGui::SameLine();
    }
    ImGui::NewLine();
}

void file_dialog(App &app) {
    if (app.file_action) {
        ImGui::OpenPopup("Lens file");
    }
    if (ImGui::BeginPopupModal("Lens file", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        static const char *const titles[] = {"", "Open a lens", "Insert a lens's elements", "Save the lens as"};
        ImGui::TextUnformatted(titles[std::clamp(app.file_action, 0, 3)]);
        ImGui::SetNextItemWidth(520);
        if (ImGui::IsWindowAppearing())
            ImGui::SetKeyboardFocusHere();
        const bool enter = ImGui::InputText("##path", app.file_path, sizeof app.file_path,
                                            ImGuiInputTextFlags_EnterReturnsTrue);
        if (ImGui::Button("OK", ImVec2(100, 0)) || enter) {
            if (app.file_action == 3) {
                save(app, app.file_path);
            } else {
                const std::string text = read_file(app.file_path);
                if (text.empty())
                    app.status = std::string("Cannot open ") + app.file_path;
                else {
                    load_lens(app, text, app.file_action == 2);
                    if (app.file_action == 1)
                        app.lens_path = app.file_path;
                }
            }
            app.file_action = 0;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(100, 0))) {
            app.file_action = 0;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

void shortcuts(App &app) {
    const ImGuiIO &io = ImGui::GetIO();
    if (io.WantTextInput)
        return;
    const bool ctrl = io.KeyCtrl, shift = io.KeyShift;
    if (ctrl && ImGui::IsKeyPressed(ImGuiKey_Z, false))
        do_undo(app, shift);
    else if (ctrl && ImGui::IsKeyPressed(ImGuiKey_Y, false))
        do_undo(app, true);
    else if (ctrl && ImGui::IsKeyPressed(ImGuiKey_D, false))
        duplicate_selected(app);
    else if (ctrl && ImGui::IsKeyPressed(ImGuiKey_G, false))
        group_selected(app);
    else if (ctrl && ImGui::IsKeyPressed(ImGuiKey_I, false))
        app.identify_until = now() + 0.6;
    else if (ctrl && ImGui::IsKeyPressed(ImGuiKey_S, false))
        shift || app.lens_path.empty() ? open_file_dialog(app, 3) : save(app, app.lens_path);
    else if (ctrl && ImGui::IsKeyPressed(ImGuiKey_O, false))
        open_file_dialog(app, shift ? 2 : 1);
    else if (!ctrl && ImGui::IsKeyPressed(ImGuiKey_Delete, false) && !ImGui::IsAnyItemActive() &&
             app.tree_focused)
        remove_selected(app);
}

void ok(App &app) {
    app.saved = !app.out_path.empty() && write_file(app.out_path, lens::write(app.lens));
    app.quit = true;
}

void frame(App &app) {
    app.hint.clear();
    const ImGuiViewport *vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::Begin("MillSaph Lens Editor", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_MenuBar | ImGuiWindowFlags_NoBringToFrontOnFocus);
    menus(app);
    toolbar(app);
    palette(app);
    ImGui::Separator();

    const float bottom = ImGui::GetFrameHeightWithSpacing() + 6;
    const float h = ImGui::GetContentRegionAvail().y - bottom;
    const float presets = app.show_presets ? 300 : 0, elements = 320, properties = 470;
    if (app.show_presets) {
        ImGui::BeginChild("presets", ImVec2(presets, h), ImGuiChildFlags_Borders);
        presets_panel(app);
        ImGui::EndChild();
        ImGui::SameLine();
    }
    ImGui::BeginChild("preview", ImVec2(ImGui::GetContentRegionAvail().x - elements - properties -
                                            ImGui::GetStyle().ItemSpacing.x * 2,
                                        h));
    preview_panel(app);
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("elements", ImVec2(elements, h), ImGuiChildFlags_Borders);
    elements_panel(app);
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("properties", ImVec2(0, h), ImGuiChildFlags_Borders);
    properties_panel(app);
    ImGui::EndChild();

    // the hint of what is under the mouse; Cancel, OK
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(ImVec4(0.6f, 0.75f, 0.95f, 1.0f), "%s", app.hint.c_str());
    ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - 210);
    if (ImGui::Button("Cancel", ImVec2(100, 0)))
        app.quit = true;
    ImGui::SameLine();
    if (ImGui::Button("OK", ImVec2(100, 0)) ||
        (ImGui::IsKeyPressed(ImGuiKey_Enter, false) && ImGui::GetIO().KeyCtrl))
        ok(app);
    file_dialog(app);
    shortcuts(app);
    ImGui::End();
    if (app.move_pending) {
        app.move_pending = false;
        move_selected(app, app.move_target, app.move_into);
    }
    if (app.selected.empty() || app.selected.back() != app.last_primary) {
        app.gradient_stop = -1;
        app.last_primary = app.selected.empty() ? Path{} : app.selected.back();
    }

    // a change recorded for undo once the mouse is up (a drag: one step)
    if (app.changed && !ImGui::IsMouseDown(ImGuiMouseButton_Left) && !ImGui::IsAnyItemActive()) {
        const std::string text = lens::write(app.lens);
        if (text != app.committed) {
            app.undo.push_back(app.committed);
            if (app.undo.size() > 200)
                app.undo.erase(app.undo.begin());
            app.redo.clear();
            app.committed = text;
        }
        app.changed = false;
    }
}

bool screenshot(const char *path, int w, int h) {
    std::vector<uint8_t> px((size_t)w * h * 3);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, w, h, GL_RGB, GL_UNSIGNED_BYTE, px.data());
    FILE *f = std::fopen(path, "wb");
    if (!f)
        return false;
    std::fprintf(f, "P6\n%d %d\n255\n", w, h);
    for (int y = h - 1; y >= 0; --y)
        std::fwrite(&px[(size_t)y * w * 3], 1, (size_t)w * 3, f);
    std::fclose(f);
    return true;
}

void style() {
    ImGui::StyleColorsDark();
    ImGuiStyle &s = ImGui::GetStyle();
    s.FrameRounding = 3;
    s.GrabRounding = 3;
    s.ChildRounding = 3;
    s.PopupRounding = 3;
    s.WindowBorderSize = 0;
    s.FramePadding = ImVec2(6, 3);
    s.ItemSpacing = ImVec2(6, 4);
    ImVec4 *c = s.Colors;
    c[ImGuiCol_WindowBg] = ImVec4(0.13f, 0.13f, 0.13f, 1);
    c[ImGuiCol_ChildBg] = ImVec4(0.15f, 0.15f, 0.15f, 1);
    c[ImGuiCol_PopupBg] = ImVec4(0.12f, 0.12f, 0.12f, 1);
    c[ImGuiCol_MenuBarBg] = ImVec4(0.11f, 0.11f, 0.11f, 1);
    c[ImGuiCol_FrameBg] = ImVec4(0.20f, 0.20f, 0.20f, 1);
    c[ImGuiCol_FrameBgHovered] = ImVec4(0.26f, 0.26f, 0.26f, 1);
    c[ImGuiCol_FrameBgActive] = ImVec4(0.30f, 0.30f, 0.30f, 1);
    c[ImGuiCol_Button] = ImVec4(0.22f, 0.22f, 0.22f, 1);
    c[ImGuiCol_ButtonHovered] = ImVec4(0.30f, 0.30f, 0.30f, 1);
    c[ImGuiCol_ButtonActive] = ImVec4(0.35f, 0.35f, 0.35f, 1);
    c[ImGuiCol_Header] = ImVec4(0.0f, 0.37f, 0.65f, 1);
    c[ImGuiCol_HeaderHovered] = ImVec4(0.25f, 0.25f, 0.25f, 1);
    c[ImGuiCol_HeaderActive] = ImVec4(0.0f, 0.42f, 0.72f, 1);
    c[ImGuiCol_SliderGrab] = ImVec4(0.75f, 0.75f, 0.75f, 1);
    c[ImGuiCol_SliderGrabActive] = ImVec4(1, 1, 1, 1);
    c[ImGuiCol_CheckMark] = ImVec4(0.2f, 0.6f, 1.0f, 1);
    c[ImGuiCol_Separator] = ImVec4(0.25f, 0.25f, 0.25f, 1);
    c[ImGuiCol_Border] = ImVec4(0.22f, 0.22f, 0.22f, 1);
}

void fonts() {
    static const char *const candidates[] = {
        "/usr/share/fonts/dejavu-sans-fonts/DejaVuSans.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/TTF/DejaVuSans.ttf",
        "C:\\Windows\\Fonts\\segoeui.ttf",
        "/System/Library/Fonts/SFNS.ttf",
    };
    for (const char *path : candidates)
        if (std::ifstream(path).good() && ImGui::GetIO().Fonts->AddFontFromFileTTF(path, 15.0f))
            return;
}

} // namespace

int main(int argc, char **argv) {
    App app;
    std::string in_path;
    const char *shot = nullptr;
    int select = -1;
    for (int i = 1; i + 1 < argc; i += 2) {
        if (!std::strcmp(argv[i], "--in"))
            in_path = argv[i + 1];
        else if (!std::strcmp(argv[i], "--out"))
            app.out_path = argv[i + 1];
        else if (!std::strcmp(argv[i], "--screenshot"))
            shot = argv[i + 1];
        else if (!std::strcmp(argv[i], "--select"))
            select = std::atoi(argv[i + 1]);
    }
    std::string err;
    const std::string text = in_path.empty() ? std::string(ms_lensflare_preset_texts[0]) : read_file(in_path);
    if (!lens::parse(text, app.lens, err)) {
        std::fprintf(stderr, "millsaph_lens_editor: %s\n", err.c_str());
        lens::parse("{\"elements\": []}", app.lens, err);
    }
    if (select >= 0)
        app.selected = {Path{select}};
    app.committed = lens::write(app.lens);
    app.screenshot = shot != nullptr;

    if (!glfwInit())
        return 2;
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 2);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE); // (macOS)
    GLFWwindow *window = glfwCreateWindow(1700, 960, "MillSaph Lens Editor", nullptr, nullptr);
    if (!window) {
        glfwTerminate();
        return 2;
    }
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr; // (no imgui.ini next to the host)
    style();
    fonts();
    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init("#version 150");

    int frames = 0;
    while (!glfwWindowShouldClose(window) && !app.quit) {
        if (shot)
            glfwPollEvents();
        else
            glfwWaitEventsTimeout(app.dirty || app.busy ? 0.0 : 0.5);
        app.busy = false;
        app.thumbs_budget = shot ? 1000 : 3;
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();
        frame(app);
        ImGui::Render();
        drop_unused_thumbs(app);
        int w, h;
        glfwGetFramebufferSize(window, &w, &h);
        glViewport(0, 0, w, h);
        glClearColor(0.1f, 0.1f, 0.1f, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        if (shot && ++frames == 5) {
            screenshot(shot, w, h);
            app.quit = true;
        }
        glfwSwapBuffers(window);
    }
    for (auto &t : app.thumbs)
        if (t.second.texture)
            glDeleteTextures(1, &t.second.texture);
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
    ms_effects_release();
    ms_engine_shutdown();
    return app.saved ? 0 : 1;
}
