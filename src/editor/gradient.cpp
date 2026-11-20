// See gradient.h.

#include "gradient.h"

#include <algorithm>
#include <cstring>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <vector>

#include "imgui.h"

using lens::Json;

const char *const GRADIENT_HINT = "Click to add colors. Double-click to change colors. Drag down or "
                                  "press Delete to remove colors.";

namespace {

struct Stop {
    double r;
    float c[3];
};

std::vector<Stop> read_stops(const Json &e) {
    std::vector<Stop> out;
    const Json *radii = lens::value(e, "radii"), *colors = lens::value(e, "colors");
    const size_t n = radii && radii->is_array() ? radii->size() : 0;
    for (size_t i = 0; i < std::max<size_t>(n, 1); ++i) {
        Stop s{n ? (*radii)[i].get<double>() : 1.0, {1, 1, 1}};
        for (int k = 0; k < 3; ++k)
            if (colors && colors->is_array() && 3 * i + k < colors->size() && (*colors)[3 * i + k].is_number())
                s.c[k] = (*colors)[3 * i + k].get<float>();
            else if (colors && colors->is_array() && colors->size() >= 3 && i > 0)
                s.c[k] = out.back().c[k];
        out.push_back(s);
    }
    return out;
}

void write_stops(Json &e, const std::vector<Stop> &stops) {
    Json radii = Json::array(), colors = Json::array();
    for (const Stop &s : stops) {
        radii.push_back(std::round(s.r * 10000.0) / 10000.0);
        for (int k = 0; k < 3; ++k)
            colors.push_back(s.c[k]);
    }
    lens::set_field(e, "radii", radii);
    lens::set_field(e, "colors", colors);
}

// The colour at radius r (linear between the stops, flat outside them).
void color_at(const std::vector<Stop> &stops, double r, float out[3]) {
    if (r <= stops.front().r) {
        std::copy(stops.front().c, stops.front().c + 3, out);
        return;
    }
    for (size_t i = 1; i < stops.size(); ++i)
        if (r <= stops[i].r) {
            const double span = stops[i].r - stops[i - 1].r;
            const float t = span > 0 ? (float)((r - stops[i - 1].r) / span) : 1.0f;
            for (int k = 0; k < 3; ++k)
                out[k] = stops[i - 1].c[k] + t * (stops[i].c[k] - stops[i - 1].c[k]);
            return;
        }
    std::copy(stops.back().c, stops.back().c + 3, out);
}

ImU32 display(const float c[3], float alpha = 1.0f) {
    float d[3];
    for (int k = 0; k < 3; ++k)
        d[k] = std::pow(std::clamp(c[k], 0.0f, 1.0f), 1.0f / 2.2f);
    return ImGui::ColorConvertFloat4ToU32(ImVec4(d[0], d[1], d[2], alpha));
}

void draw_bar(ImDrawList *draw, const std::vector<Stop> &stops, ImVec2 a, ImVec2 b) {
    const float w = b.x - a.x;
    auto x_of = [&](double r) { return a.x + (float)std::clamp(r, 0.0, 1.0) * w; };
    float prev_x = a.x;
    float prev[3];
    color_at(stops, 0.0, prev);
    for (size_t i = 0; i <= stops.size(); ++i) {
        const double r = i < stops.size() ? stops[i].r : 1.0;
        float c[3];
        color_at(stops, r, c);
        const float x = x_of(r);
        if (x > prev_x)
            draw->AddRectFilledMultiColor(ImVec2(prev_x, a.y), ImVec2(x, b.y), display(prev),
                                          display(c), display(c), display(prev));
        prev_x = x;
        std::copy(c, c + 3, prev);
    }
    draw->AddRect(a, b, IM_COL32(110, 110, 110, 255));
}

// The saved gradients: ~/.millsaph/lens_gradients.json, an array of {radii, colors}.
std::string saved_path() {
    const char *home = std::getenv("HOME");
#ifdef _WIN32
    if (!home)
        home = std::getenv("USERPROFILE");
#endif
    return std::string(home ? home : ".") + "/.millsaph/lens_gradients.json";
}

Json &saved() {
    static Json list;
    static bool loaded = false;
    if (!loaded) {
        loaded = true;
        std::ifstream f(saved_path());
        std::stringstream s;
        s << f.rdbuf();
        try {
            list = Json::parse(s.str());
        } catch (...) {
        }
        if (!list.is_array()) // the first ones: a glow, a ring and a rainbow
            list = Json::array({
                {{"radii", {0, 1}}, {"colors", {1, 1, 1, 0, 0, 0}}},
                {{"radii", {0, 0.9, 0.95, 1}}, {"colors", {0, 0, 0, 0, 0, 0, 1, 1, 1, 0, 0, 0}}},
                {{"radii", {0.8, 0.85, 0.9, 0.95, 1}},
                 {"colors", {0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0}}},
            });
    }
    return list;
}

void save_saved() {
    const std::string path = saved_path();
    const std::string dir = path.substr(0, path.rfind('/'));
#ifdef _WIN32
    std::system(("mkdir \"" + dir + "\" 2>nul").c_str());
#else
    std::system(("mkdir -p '" + dir + "'").c_str());
#endif
    std::ofstream f(path);
    f << saved().dump(1) << "\n";
}

} // namespace

bool gradient_edit(Json &e, int &selected, std::string &hint) {
    std::vector<Stop> stops = read_stops(e);
    bool changed = false;
    ImGui::PushID("gradient");
    ImDrawList *draw = ImGui::GetWindowDrawList();
    const float w = ImGui::GetContentRegionAvail().x, bar_h = 24.0f, mark_h = 14.0f;
    const ImVec2 at = ImGui::GetCursorScreenPos();
    const ImVec2 a(at.x + 6, at.y + 4), b(at.x + w - 6, at.y + 4 + bar_h);
    ImGui::InvisibleButton("bar", ImVec2(w, bar_h + mark_h + 10));
    const bool hovered = ImGui::IsItemHovered(), active = ImGui::IsItemActive();
    if (hovered || active)
        hint = GRADIENT_HINT;
    auto r_of = [&](float x) { return std::clamp((double)((x - a.x) / (b.x - a.x)), 0.0, 1.0); };
    auto x_of = [&](double r) { return a.x + (float)r * (b.x - a.x); };
    static bool dragging = false, removing = false;
    const ImVec2 m = ImGui::GetMousePos();
    if (selected >= (int)stops.size())
        selected = (int)stops.size() - 1;
    if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
        int near = -1;
        float best = 7.0f;
        for (int i = 0; i < (int)stops.size(); ++i) {
            const float d = std::fabs(x_of(stops[i].r) - m.x);
            if (d <= best)
                best = d, near = i;
        }
        if (near < 0 && m.y <= b.y + 2) { // a new stop, the colour there
            Stop s{r_of(m.x), {1, 1, 1}};
            color_at(stops, s.r, s.c);
            stops.push_back(s);
            near = (int)stops.size() - 1;
            changed = true;
        }
        selected = near;
        dragging = near >= 0;
        removing = false;
        if (near >= 0 && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            dragging = false;
            ImGui::OpenPopup("stop colour");
        }
    }
    if (active && dragging && selected >= 0 && selected < (int)stops.size()) {
        const ImVec2 drag = ImGui::GetMouseDragDelta(ImGuiMouseButton_Left, 1.0f);
        removing = stops.size() > 1 && m.y > b.y + mark_h + 30;
        if (drag.x != 0 || drag.y != 0) {
            const double r = r_of(m.x);
            if (r != stops[selected].r) {
                stops[selected].r = r;
                changed = true;
            }
        }
    }
    if (dragging && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        if (removing && selected >= 0 && selected < (int)stops.size() && stops.size() > 1) {
            stops.erase(stops.begin() + selected);
            selected = -1;
            changed = true;
        }
        dragging = removing = false;
    }
    if (hovered && selected >= 0 && selected < (int)stops.size() && stops.size() > 1 &&
        ImGui::IsKeyPressed(ImGuiKey_Delete)) {
        stops.erase(stops.begin() + selected);
        selected = -1;
        changed = true;
    }
    // kept in order of radius, the selection following its stop
    if (changed && selected >= 0) {
        const Stop keep = stops[selected];
        std::stable_sort(stops.begin(), stops.end(), [](const Stop &p, const Stop &q) { return p.r < q.r; });
        for (int i = 0; i < (int)stops.size(); ++i)
            if (stops[i].r == keep.r && !std::memcmp(stops[i].c, keep.c, sizeof keep.c))
                selected = i;
    }
    if (ImGui::BeginPopup("stop colour")) {
        if (selected >= 0 && selected < (int)stops.size() &&
            ImGui::ColorPicker3("##picker", stops[selected].c, ImGuiColorEditFlags_Float | ImGuiColorEditFlags_HDR))
            changed = true;
        ImGui::EndPopup();
    }
    // the bar, its stops under it (the selected one blue)
    draw_bar(draw, stops, a, b);
    for (int i = 0; i < (int)stops.size(); ++i) {
        const float x = x_of(stops[i].r), y = b.y + 3;
        const bool sel = i == selected;
        const float alpha = sel && removing ? 0.3f : 1.0f;
        const ImVec2 p[5] = {ImVec2(x, y), ImVec2(x + 5, y + 5), ImVec2(x + 5, y + mark_h),
                             ImVec2(x - 5, y + mark_h), ImVec2(x - 5, y + 5)};
        draw->AddConvexPolyFilled(p, 5, sel ? IM_COL32(40, 140, 255, (int)(255 * alpha)) : IM_COL32(230, 230, 230, (int)(255 * alpha)));
        draw->AddRectFilled(ImVec2(x - 3, y + 6), ImVec2(x + 3, y + mark_h - 2), display(stops[i].c, alpha));
    }
    if (changed)
        write_stops(e, stops);

    // Saved Gradients, Stop Location
    if (ImGui::Button("Saved Gradients"))
        ImGui::OpenPopup("saved");
    if (ImGui::BeginPopup("saved")) {
        if (ImGui::Selectable("Save Current", false, ImGuiSelectableFlags_DontClosePopups)) {
            Json g = {{"radii", Json::array()}, {"colors", Json::array()}};
            for (const Stop &s : stops) {
                g["radii"].push_back(s.r);
                for (int k = 0; k < 3; ++k)
                    g["colors"].push_back(s.c[k]);
            }
            saved().push_back(g);
            save_saved();
        }
        int remove = -1;
        for (int i = 0; i < (int)saved().size(); ++i) {
            ImGui::PushID(i);
            Json tmp = Json::object();
            tmp["radii"] = saved()[i].value("radii", Json::array());
            tmp["colors"] = saved()[i].value("colors", Json::array());
            const std::vector<Stop> g = read_stops(tmp);
            const ImVec2 p = ImGui::GetCursorScreenPos();
            if (ImGui::InvisibleButton("g", ImVec2(180, 20))) {
                stops = g;
                write_stops(e, stops);
                selected = -1;
                changed = true;
                ImGui::CloseCurrentPopup();
            }
            draw_bar(ImGui::GetWindowDrawList(), g, ImVec2(p.x, p.y + 2), ImVec2(p.x + 180, p.y + 18));
            ImGui::SameLine();
            if (ImGui::SmallButton("x"))
                remove = i;
            ImGui::PopID();
        }
        if (remove >= 0) {
            saved().erase(saved().begin() + remove);
            save_saved();
        }
        ImGui::EndPopup();
    }
    ImGui::SameLine();
    const float field_w = 70.0f;
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, ImGui::GetContentRegionAvail().x - field_w -
                                                                      ImGui::CalcTextSize("Stop Location").x - 8));
    ImGui::AlignTextToFramePadding();
    ImGui::BeginDisabled(selected < 0 || selected >= (int)stops.size());
    ImGui::TextUnformatted("Stop Location");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(field_w);
    float loc = selected >= 0 && selected < (int)stops.size() ? (float)stops[selected].r : 1.0f;
    if (ImGui::DragFloat("##loc", &loc, 0.002f, 0.0f, 1.0f, "%.3f") && selected >= 0) {
        stops[selected].r = std::clamp((double)loc, 0.0, 1.0);
        const Stop keep = stops[selected];
        std::stable_sort(stops.begin(), stops.end(), [](const Stop &p, const Stop &q) { return p.r < q.r; });
        for (int i = 0; i < (int)stops.size(); ++i)
            if (stops[i].r == keep.r)
                selected = i;
        write_stops(e, stops);
        changed = true;
    }
    ImGui::EndDisabled();
    ImGui::PopID();
    return changed;
}
