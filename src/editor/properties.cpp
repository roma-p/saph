// The selected element's properties, in groups: its fields, images and gradients (see app.h).

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "imgui.h"

#include "app.h"
#include "gradient.h"

namespace editor {

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
    const bool wide = f.max - f.min >= 100; // (angles, frequencies)
    const char *fmt = mode == lens::Mode::Scale ? "x%.3f"
                      : mode == lens::Mode::Offset ? (wide ? "%+.2f" : "%+.3f")
                      : wide ? "%.2f" : "%.3f";
    switch (f.kind) {
    case lens::Kind::Number:
    case lens::Kind::Integer: {
        const bool integer = f.kind == lens::Kind::Integer;
        float v = (float)number(e, key, lens::neutral(e, key));
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
        static const char *const kinds[] = {"dirt", "glint", "ramp", "iris", "field"};
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
        if (kind == "field") {
            // a plain colour ("color"), faded to the edges ("edges" [x, y]), "aspect" wide
            float col[3] = {1, 1, 1};
            if (im.contains("color") && im["color"].is_array())
                for (int c = 0; c < 3 && c < (int)im["color"].size(); ++c)
                    col[c] = im["color"][c].get<float>();
            row_label("Color", false);
            ImGui::TableSetColumnIndex(1);
            if (ImGui::ColorEdit3("##fieldcolor", col, ImGuiColorEditFlags_NoInputs)) {
                im["color"] = {col[0], col[1], col[2]};
                changed = true;
            }
            float edges[2] = {0, 0};
            if (im.contains("edges") && im["edges"].is_array())
                for (int c = 0; c < 2 && c < (int)im["edges"].size(); ++c)
                    edges[c] = im["edges"][c].get<float>();
            row_label("Edge Fade", false);
            ImGui::TableSetColumnIndex(1);
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (ImGui::SliderFloat2("##edges", edges, 0.0f, 1.0f, "%.2f")) {
                im["edges"] = {edges[0], edges[1]};
                changed = true;
            }
            float aspect = (float)number(im, "aspect", 1.0);
            row_label("Aspect", false);
            ImGui::TableSetColumnIndex(1);
            ImGui::SetNextItemWidth(VALUE_W);
            if (ImGui::DragFloat("##aspect", &aspect, 0.01f, 0.1f, 10.0f, "%.3f")) {
                im["aspect"] = aspect;
                changed = true;
            }
        }
        for (const Knob &k : knobs) {
            if (kind == "field")
                break;
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
    ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoClip, VALUE_W);
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
        const bool show = gen_field ? gen
                          : grp      ? group != "Options" && std::strcmp(f.name, "color")
                                     : lens::field_for(f, type);
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
                changed |= edit_field(*e, *f, grey, grp);
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

} // namespace editor
