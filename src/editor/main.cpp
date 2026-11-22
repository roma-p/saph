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
#include <filesystem>
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
#include "common/preset.h"
#include "engine/engine.h"

#include "gradient.h"
#include "lens.h"
#include "preview.h"
#include "app.h"
#include "window.h"
#include "lensflare_presets.h" // (generated from presets/lensflare)

namespace editor {

double now() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

bool write_file(const std::string &path, const std::string &text) {
    std::ofstream f(path, std::ios::binary);
    f << text;
    return (bool)f;
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
        if (selected && app.scroll_to_selection) {
            ImGui::SetScrollHereY(0.5f);
            app.scroll_to_selection = false;
        }
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

// The dock's presets: the compiled ones, then the bundle's files not among them, by name.
void load_presets(App &app) {
    for (int i = 0; i < MS_LENSFLARE_PRESETS; ++i)
        app.presets.push_back({ms_lensflare_preset_menu[i + 1], ms_lensflare_preset_texts[i]});
    char dir[2048];
    if (ms_presets_dir("lensflare", dir, sizeof dir)) {
        std::vector<LensPreset> found;
        std::error_code ec;
        for (const auto &f : std::filesystem::directory_iterator(dir, ec)) {
            if (f.path().extension() != ".json")
                continue;
            Json j = Json::parse(window::read_file(f.path().string()), nullptr, false);
            if (j.is_discarded() || !j.is_object() || !j.contains("params") ||
                !j["params"].is_object() || !j["params"].contains("lens_") ||
                !j["params"]["lens_"].is_string())
                continue;
            const std::string name = j.value("name", f.path().stem().string());
            if (std::none_of(app.presets.begin(), app.presets.end(),
                             [&](const LensPreset &p) { return p.name == name; }))
                found.push_back({name, j["params"]["lens_"].get<std::string>()});
        }
        std::sort(found.begin(), found.end(), [](const LensPreset &a, const LensPreset &b) {
            return a.name < b.name;
        });
        app.presets.insert(app.presets.end(), found.begin(), found.end());
    }
}

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
    for (int i = 0; i < (int)app.presets.size(); ++i) {
        const std::string &name = app.presets[i].name, &text = app.presets[i].text;
        std::string low = name;
        std::transform(low.begin(), low.end(), low.begin(), ::tolower);
        if (!needle.empty() && low.find(needle) == std::string::npos)
            continue;
        if (shown++ % app.columns)
            ImGui::SameLine();
        ImGui::BeginGroup();
        ImGui::PushID(i);
        const ImVec2 p = ImGui::GetCursorScreenPos();
        if (ImGui::InvisibleButton("preset", ImVec2(tw, th))) {
            load_lens(app, text, false);
        }
        const bool hot = ImGui::IsItemHovered();
        if (hot)
            app.hint = "Click to load the preset, replacing all the current elements.";
        ImDrawList *d = ImGui::GetWindowDrawList();
        d->AddRectFilled(p, ImVec2(p.x + tw, p.y + th), IM_COL32(0, 0, 0, 255), 4);
        PreviewSettings s;
        if (GLuint t = thumb(app, text, 256, 144, s.scale_widths, 1.0))
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
                const std::string text = window::read_file(app.file_path);
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

} // namespace editor

using namespace editor;

int main(int argc, char **argv) {
    App app;
    load_presets(app);
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
    const std::string text = in_path.empty() ? std::string(ms_lensflare_preset_texts[0]) : window::read_file(in_path);
    if (!lens::parse(text, app.lens, err)) {
        std::fprintf(stderr, "millsaph_lens_editor: %s\n", err.c_str());
        lens::parse("{\"elements\": []}", app.lens, err);
    }
    if (select >= 0)
        app.selected = {Path{select}};
    app.committed = lens::write(app.lens);
    app.screenshot = shot != nullptr;

    window::Loop loop;
    loop.look = [] {
        style();
        fonts();
    };
    loop.frame = [&] {
        app.busy = false;
        app.thumbs_budget = shot ? 1000 : 3;
        frame(app);
    };
    loop.after_render = [&] { drop_unused_thumbs(app); };
    loop.wait = [&] { return app.dirty || app.busy ? 0.0 : 0.5; };
    loop.quit = [&] { return app.quit; };
    loop.shut_down = [&] {
        for (auto &t : app.thumbs)
            if (t.second.texture)
                glDeleteTextures(1, &t.second.texture);
    };
    if (window::run("MillSaph Lens Editor", 1700, 960, shot, 5, loop) != 0)
        return 2;
    ms_effects_release();
    ms_engine_shutdown();
    return app.saved ? 0 : 1;
}
