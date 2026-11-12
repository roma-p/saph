// MillSaph's lens editor (millsaph_lens_editor): edits a Lens Flare lens with a live preview
// rendered by MS_LensFlare. MS_LensFlare's Edit Lens button runs it and waits:
//   millsaph_lens_editor --in lens.json --out edited.json
// Exit code 0: saved (the lens in --out), 1: cancelled. --screenshot file.ppm renders the
// window once into a file and exits (tests).

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <GLFW/glfw3.h>

#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"

#include "common/effect.h"
#include "engine/engine.h"

#include "lens.h"
#include "preview.h"
#include "lensflare_presets.h" // (generated from presets/lensflare)

using lens::Json;

namespace {

struct App {
    lens::Lens lens;
    std::string out_path, status;
    // the selection: the path of indices from the elements' array down (containers' children)
    std::vector<int> selected;
    PreviewSettings settings;
    Preview preview;
    std::vector<uint8_t> pixels;
    GLuint texture = 0;
    int tex_w = 0, tex_h = 0;
    bool dirty = true, saved = false, quit = false;
    char file_path[1024] = "";
};

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

// The array holding the element at `path` (all but its last index), or null.
Json *parent_of(App &app, const std::vector<int> &path) {
    Json *list = &app.lens.elements();
    for (size_t i = 0; i + 1 < path.size(); ++i) {
        if (path[i] < 0 || path[i] >= (int)list->size())
            return nullptr;
        Json &e = (*list)[path[i]];
        if (!e.contains("children") || !e["children"].is_array())
            return nullptr;
        list = &e["children"];
    }
    return list;
}

Json *element_at(App &app, const std::vector<int> &path) {
    Json *list = path.empty() ? nullptr : parent_of(app, path);
    if (!list || path.back() < 0 || path.back() >= (int)list->size())
        return nullptr;
    return &(*list)[path.back()];
}

double number(const Json &e, const char *key, double def) {
    return e.contains(key) && e[key].is_number() ? e[key].get<double>() : def;
}

// ------------------------------------------------------------------ the element tree

void tree(App &app, Json &list, std::vector<int> &path) {
    for (int i = 0; i < (int)list.size(); ++i) {
        Json &e = list[i];
        if (!e.is_object())
            continue;
        path.push_back(i);
        const std::string type = e.value("type", "?");
        std::string name = e.contains("name") && e["name"].is_string() ? e["name"].get<std::string>()
                                                                       : type;
        if (name.empty())
            name = type;
        const bool disabled = e.contains("disabled") && e["disabled"].is_boolean() &&
                              e["disabled"].get<bool>();
        ImGui::PushID(i);
        bool on = !disabled;
        if (ImGui::Checkbox("##on", &on)) {
            e["disabled"] = !on;
            app.dirty = true;
        }
        ImGui::SameLine();
        const bool is_selected = path == app.selected;
        if (type == "container") {
            ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_DefaultOpen;
            if (is_selected)
                flags |= ImGuiTreeNodeFlags_Selected;
            const bool open = ImGui::TreeNodeEx("##node", flags, "%s", name.c_str());
            if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen())
                app.selected = path;
            if (open) {
                if (!e.contains("children") || !e["children"].is_array())
                    e["children"] = Json::array();
                tree(app, e["children"], path);
                ImGui::TreePop();
            }
        } else {
            const std::string label = name == type ? name : name + "  (" + type + ")";
            if (ImGui::Selectable(label.c_str(), is_selected))
                app.selected = path;
        }
        ImGui::PopID();
        path.pop_back();
    }
}

void tree_panel(App &app) {
    ImGui::TextDisabled("Elements");
    if (ImGui::Button("Add"))
        ImGui::OpenPopup("add");
    if (ImGui::BeginPopup("add")) {
        for (const char *type : lens::element_types())
            if (ImGui::Selectable(type)) {
                Json *list = &app.lens.elements();
                Json *sel = element_at(app, app.selected);
                std::vector<int> where;
                if (sel && sel->value("type", "") == "container") { // into the container
                    list = &(*sel)["children"];
                    where = app.selected;
                } else if (sel) { // after the selection
                    list = parent_of(app, app.selected);
                    where.assign(app.selected.begin(), app.selected.end() - 1);
                }
                list->push_back(lens::new_element(type));
                where.push_back((int)list->size() - 1);
                app.selected = where;
                app.dirty = true;
            }
        if (ImGui::Selectable("container")) {
            Json c = {{"type", "container"}, {"name", "Group"}, {"children", Json::array()}};
            app.lens.elements().push_back(c);
            app.selected = {(int)app.lens.elements().size() - 1};
            app.dirty = true;
        }
        ImGui::EndPopup();
    }
    Json *list = app.selected.empty() ? nullptr : parent_of(app, app.selected);
    const int at = app.selected.empty() ? -1 : app.selected.back();
    const bool any = list && at >= 0 && at < (int)list->size();
    ImGui::BeginDisabled(!any);
    ImGui::SameLine();
    if (ImGui::Button("Duplicate") && any) {
        list->insert(list->begin() + at + 1, (*list)[at]);
        app.selected.back() = at + 1;
        app.dirty = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("Delete") && any) {
        list->erase(list->begin() + at);
        app.selected.clear();
        app.dirty = true;
    }
    ImGui::SameLine();
    if (ImGui::ArrowButton("up", ImGuiDir_Up) && any && at > 0) {
        std::swap((*list)[at], (*list)[at - 1]);
        app.selected.back() = at - 1;
        app.dirty = true;
    }
    ImGui::SameLine();
    if (ImGui::ArrowButton("down", ImGuiDir_Down) && any && at + 1 < (int)list->size()) {
        std::swap((*list)[at], (*list)[at + 1]);
        app.selected.back() = at + 1;
        app.dirty = true;
    }
    ImGui::EndDisabled();
    ImGui::Separator();
    ImGui::BeginChild("tree");
    std::vector<int> path;
    tree(app, app.lens.elements(), path);
    ImGui::EndChild();
}

// ------------------------------------------------------------------ an element's fields

bool edit_field(Json &e, const lens::Field &f) {
    const char *key = f.name;
    bool changed = false;
    switch (f.kind) {
    case lens::Kind::Text: {
        char buf[256];
        std::snprintf(buf, sizeof buf, "%s", e.value(key, std::string()).c_str());
        if (ImGui::InputText(f.label, buf, sizeof buf)) {
            e[key] = std::string(buf);
            changed = true;
        }
        break;
    }
    case lens::Kind::Number: {
        float v = (float)number(e, key, f.def);
        if (ImGui::SliderFloat(f.label, &v, (float)f.min, (float)f.max, "%.3f",
                               ImGuiSliderFlags_None)) {
            e[key] = (double)v;
            changed = true;
        }
        break;
    }
    case lens::Kind::Integer: {
        int v = (int)number(e, key, f.def);
        if (ImGui::SliderInt(f.label, &v, (int)f.min, (int)f.max)) {
            e[key] = v;
            changed = true;
        }
        break;
    }
    case lens::Kind::Bool: {
        bool v = e.contains(key) && ((e[key].is_boolean() && e[key].get<bool>()) ||
                                     (e[key].is_number() && e[key].get<double>() != 0));
        if (ImGui::Checkbox(f.label, &v)) {
            e[key] = v ? 1 : 0;
            changed = true;
        }
        break;
    }
    case lens::Kind::Choice: {
        std::vector<std::string> items;
        for (const char *p = f.choices; *p;) {
            const char *bar = std::strchr(p, '|');
            items.emplace_back(p, bar ? (size_t)(bar - p) : std::strlen(p));
            p = bar ? bar + 1 : p + std::strlen(p);
        }
        const std::string now = e.value(key, items[0]);
        if (ImGui::BeginCombo(f.label, now.c_str())) {
            for (const std::string &item : items)
                if (ImGui::Selectable(item.c_str(), item == now)) {
                    e[key] = item;
                    changed = true;
                }
            ImGui::EndCombo();
        }
        break;
    }
    case lens::Kind::Color: {
        float c[3] = {1, 1, 1};
        if (e.contains(key) && e[key].is_array())
            for (int k = 0; k < 3 && k < (int)e[key].size(); ++k)
                c[k] = e[key][k].get<float>();
        if (ImGui::ColorEdit3(f.label, c, ImGuiColorEditFlags_Float | ImGuiColorEditFlags_HDR)) {
            e[key] = {c[0], c[1], c[2]};
            changed = true;
        }
        break;
    }
    case lens::Kind::Pair: {
        float v[2] = {(float)f.def, (float)f.def};
        if (e.contains(key) && e[key].is_array())
            for (int k = 0; k < 2 && k < (int)e[key].size(); ++k)
                v[k] = e[key][k].get<float>();
        if (ImGui::SliderFloat2(f.label, v, (float)f.min, (float)f.max, "%.3f")) {
            e[key] = {v[0], v[1]};
            changed = true;
        }
        break;
    }
    }
    if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Right) && e.contains(key)) {
        e.erase(key); // right click: back to the default (the field left out)
        changed = true;
    }
    return changed;
}

// The colour stops: radius and colour each (radii, colors).
bool edit_stops(Json &e) {
    bool changed = false;
    if (!e.contains("radii") || !e["radii"].is_array())
        e["radii"] = {1.0};
    if (!e.contains("colors") || !e["colors"].is_array())
        e["colors"] = {1.0, 1.0, 1.0};
    Json &radii = e["radii"], &colors = e["colors"];
    while (colors.size() < radii.size() * 3)
        colors.push_back(colors.size() >= 3 ? colors[colors.size() - 3] : Json(1.0));
    int remove = -1;
    for (int k = 0; k < (int)radii.size(); ++k) {
        ImGui::PushID(k);
        float r = radii[k].get<float>();
        float c[3] = {colors[3 * k].get<float>(), colors[3 * k + 1].get<float>(),
                      colors[3 * k + 2].get<float>()};
        ImGui::SetNextItemWidth(90);
        if (ImGui::SliderFloat("##r", &r, 0.0f, 1.0f, "r %.3f")) {
            radii[k] = r;
            changed = true;
        }
        ImGui::SameLine();
        if (ImGui::ColorEdit3("##c", c, ImGuiColorEditFlags_Float | ImGuiColorEditFlags_NoInputs)) {
            for (int j = 0; j < 3; ++j)
                colors[3 * k + j] = c[j];
            changed = true;
        }
        ImGui::SameLine();
        if (radii.size() > 1 && ImGui::SmallButton("x"))
            remove = k;
        ImGui::PopID();
    }
    if (remove >= 0) {
        radii.erase(radii.begin() + remove);
        colors.erase(colors.begin() + 3 * remove, colors.begin() + 3 * remove + 3);
        changed = true;
    }
    if (ImGui::SmallButton("Add stop")) {
        radii.push_back(1.0);
        for (int j = 0; j < 3; ++j)
            colors.push_back(colors[colors.size() - 3]);
        changed = true;
    }
    return changed;
}

// Its image: none, one of MillSaph's generated textures, or one written in the lens.
bool edit_image(Json &e) {
    bool changed = false;
    const bool has = e.contains("image") && e["image"].is_object();
    const bool generated = has && e["image"].contains("generate");
    const char *now = !has ? "None" : generated ? "Generated" : "Image in the lens";
    if (ImGui::BeginCombo("Texture", now)) {
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
    if (has && generated) {
        Json &im = e["image"];
        static const char *const kinds[] = {"dirt", "glint", "ramp", "iris"};
        const std::string kind = im.value("generate", "dirt");
        if (ImGui::BeginCombo("Kind", kind.c_str())) {
            for (const char *k : kinds)
                if (ImGui::Selectable(k, kind == k)) {
                    im["generate"] = k;
                    changed = true;
                }
            ImGui::EndCombo();
        }
        int seed = (int)number(im, "seed", 0);
        if (ImGui::SliderInt("Seed", &seed, 0, 100)) {
            im["seed"] = seed;
            changed = true;
        }
        float amount = (float)number(im, "amount", kind == "dirt" ? 0.35 : kind == "ramp" ? 1 : 0.5);
        if (ImGui::SliderFloat("Amount", &amount, 0, 1)) {
            im["amount"] = amount;
            changed = true;
        }
        float scale = (float)number(im, "scale", 1);
        if (ImGui::SliderFloat("Scale", &scale, 0.1f, 4)) {
            im["scale"] = scale;
            changed = true;
        }
        float detail = (float)number(im, "detail", 0.5);
        if (ImGui::SliderFloat("Detail", &detail, 0, 1)) {
            im["detail"] = detail;
            changed = true;
        }
        if (kind == "iris" || kind == "ramp") {
            int sides = (int)number(im, "sides", kind == "iris" ? 6 : 0);
            if (ImGui::SliderInt(kind == "iris" ? "Blades" : "Linear", &sides, 0, kind == "iris" ? 12 : 1)) {
                im["sides"] = sides;
                changed = true;
            }
        }
    } else if (has) {
        ImGui::TextDisabled("%d x %d texels", (int)number(e["image"], "xdim", 0),
                            (int)number(e["image"], "ydim", 0));
    }
    return changed;
}

void properties_panel(App &app) {
    Json *e = element_at(app, app.selected);
    if (!e) {
        ImGui::TextDisabled("Select an element.");
        return;
    }
    const std::string type = e->value("type", "");
    ImGui::Text("%s", type.c_str());
    ImGui::Separator();
    ImGui::BeginChild("fields");
    bool changed = false;
    std::string group;
    bool open = false;
    for (const lens::Field &f : lens::fields()) {
        if (!lens::field_for(f, type) || (type == "container" && std::strcmp(f.name, "name")))
            continue;
        if (group != f.group) {
            group = f.group;
            open = ImGui::CollapsingHeader(f.group, group == "Element" ? ImGuiTreeNodeFlags_DefaultOpen : 0);
            if (open && group == "Element" && type != "rays" && type != "container" &&
                ImGui::TreeNodeEx("Colour stops", ImGuiTreeNodeFlags_DefaultOpen)) {
                changed |= edit_stops(*e);
                ImGui::TreePop();
            }
            if (open && group == "Element" && type != "container")
                changed |= edit_image(*e);
        }
        if (open)
            changed |= edit_field(*e, f);
    }
    ImGui::EndChild();
    if (changed)
        app.dirty = true;
}

// ------------------------------------------------------------------ the preview

void upload(App &app, int w, int h) {
    if (!app.texture)
        glGenTextures(1, &app.texture);
    glBindTexture(GL_TEXTURE_2D, app.texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, app.pixels.data());
    app.tex_w = w, app.tex_h = h;
}

void preview_panel(App &app) {
    ImVec2 room = ImGui::GetContentRegionAvail();
    room.y -= ImGui::GetFrameHeightWithSpacing() * 3;
    int w = (int)room.x, h = (int)(room.x * 9 / 16);
    if (h > room.y)
        h = (int)room.y, w = h * 16 / 9;
    w = w < 64 ? 64 : w, h = h < 36 ? 36 : h;
    if (app.dirty || w != app.tex_w || h != app.tex_h) {
        std::string err;
        if (app.preview.render(lens::write(app.lens), app.settings, w, h, app.pixels, err)) {
            upload(app, w, h);
            app.status = std::string("Preview on ") + app.preview.api();
        } else {
            app.status = "Preview failed: " + err;
        }
        app.dirty = false;
    }
    const ImVec2 at = ImGui::GetCursorScreenPos();
    ImGui::Image((ImTextureID)(intptr_t)app.texture, ImVec2((float)w, (float)h));
    // drag on it: left the hotspot, right the pivot
    if (ImGui::IsItemHovered() || ImGui::IsItemActive()) {
        const ImVec2 m = ImGui::GetMousePos();
        const double fx = (m.x - at.x) / w, fy = 1.0 - (m.y - at.y) / h;
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left) && ImGui::IsItemHovered()) {
            app.settings.hotspot[0] = fx, app.settings.hotspot[1] = fy;
            app.dirty = true;
        } else if (ImGui::IsMouseDown(ImGuiMouseButton_Right) && ImGui::IsItemHovered()) {
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
    float sw = (float)app.settings.scale_widths, br = (float)app.settings.brightness;
    ImGui::SetNextItemWidth(200);
    if (ImGui::SliderFloat("Scale Widths", &sw, 0.02f, 1.0f))
        app.settings.scale_widths = sw, app.dirty = true;
    ImGui::SameLine();
    ImGui::SetNextItemWidth(150);
    if (ImGui::SliderFloat("Brightness", &br, 0.0f, 3.0f))
        app.settings.brightness = br, app.dirty = true;
    ImGui::SameLine();
    ImGui::SetNextItemWidth(110);
    if (ImGui::Combo("Background", &app.settings.background, "Black\0Grey\0Dusk\0"))
        app.dirty = true;
    ImGui::TextDisabled("Left drag: hotspot, right drag: pivot (preview only).  %s", app.status.c_str());
}

// ------------------------------------------------------------------ the window

void menu(App &app) {
    if (!ImGui::BeginMenuBar())
        return;
    if (ImGui::BeginMenu("Lens")) {
        if (ImGui::BeginMenu("Load preset")) {
            for (int i = 0; i < MS_LENSFLARE_PRESETS; ++i)
                if (ImGui::MenuItem(ms_lensflare_preset_menu[i + 1])) {
                    std::string err;
                    lens::Lens next;
                    if (lens::parse(ms_lensflare_preset_texts[i], next, err)) {
                        app.lens = next;
                        app.selected.clear();
                        app.dirty = true;
                    }
                }
            ImGui::EndMenu();
        }
        ImGui::Separator();
        ImGui::InputText("##file", app.file_path, sizeof app.file_path);
        if (ImGui::MenuItem("Open file")) {
            std::string err;
            lens::Lens next;
            if (lens::parse(read_file(app.file_path), next, err)) {
                app.lens = next;
                app.selected.clear();
                app.dirty = true;
            } else {
                app.status = "Cannot open: " + err;
            }
        }
        if (ImGui::MenuItem("Save to file"))
            app.status = write_file(app.file_path, lens::write(app.lens)) ? "Saved to the file"
                                                                          : "Cannot save the file";
        ImGui::EndMenu();
    }
    const float right = ImGui::GetWindowWidth() - 190;
    ImGui::SetCursorPosX(right);
    if (ImGui::Button("Apply") ||
        (ImGui::IsKeyPressed(ImGuiKey_Enter, false) && ImGui::GetIO().KeyCtrl)) {
        app.saved = !app.out_path.empty() && write_file(app.out_path, lens::write(app.lens));
        app.quit = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel"))
        app.quit = true;
    ImGui::EndMenuBar();
}

void frame(App &app) {
    const ImGuiViewport *vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::Begin("MillSaph Lens Editor", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_MenuBar |
                     ImGuiWindowFlags_NoBringToFrontOnFocus);
    menu(app);
    const float left = 300, right = 360;
    ImGui::BeginChild("left", ImVec2(left, 0), ImGuiChildFlags_Borders);
    tree_panel(app);
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("middle", ImVec2(ImGui::GetContentRegionAvail().x - right, 0));
    preview_panel(app);
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("right", ImVec2(0, 0), ImGuiChildFlags_Borders);
    properties_panel(app);
    ImGui::EndChild();
    ImGui::End();
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
        app.selected = {select};
    std::snprintf(app.file_path, sizeof app.file_path, "%s", "lens.json");

    if (!glfwInit())
        return 2;
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 2);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE); // (macOS)
    GLFWwindow *window = glfwCreateWindow(1500, 860, "MillSaph Lens Editor", nullptr, nullptr);
    if (!window) {
        glfwTerminate();
        return 2;
    }
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr; // (no imgui.ini next to the host)
    ImGui::StyleColorsDark();
    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init("#version 150");

    int frames = 0;
    while (!glfwWindowShouldClose(window) && !app.quit) {
        if (shot)
            glfwPollEvents();
        else
            glfwWaitEventsTimeout(app.dirty ? 0.0 : 0.5);
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();
        frame(app);
        ImGui::Render();
        int w, h;
        glfwGetFramebufferSize(window, &w, &h);
        glViewport(0, 0, w, h);
        glClearColor(0.1f, 0.1f, 0.1f, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        if (shot && ++frames == 4) {
            screenshot(shot, w, h);
            app.quit = true;
        }
        glfwSwapBuffers(window);
    }
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
    ms_effects_release();
    ms_engine_shutdown();
    return app.saved ? 0 : 1;
}
