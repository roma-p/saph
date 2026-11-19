// MillSaph's preset window (millsaph_preset_browser): the presets of one effect as
// thumbnails, to pick one. Every effect's Load Preset button runs it and waits:
//   millsaph_preset_browser --dir <the effect's presets folder> --out picked.txt [--title T]
// Exit code 0: a preset picked (the path of its file in --out), 1: cancelled.
// --screenshot file.ppm draws the window once into a file and exits (tests); --select name
// selects that preset first.
// A preset: <name>.json ({"name", "tags", "params"}, common/preset.h) and its thumbnail
// <name>.png beside it.

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include <GLFW/glfw3.h>
#include <nlohmann/json.hpp>

#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#include "stb_image.h"

namespace fs = std::filesystem;

namespace {

struct Preset {
    std::string name, path, thumbnail, lower; // lower: name and tags in lower case (search)
    std::vector<std::string> tags;
    GLuint texture = 0;
    int w = 0, h = 0;
    bool tried = false; // thumbnail read (or missing)
};

struct App {
    std::vector<Preset> presets;
    std::vector<std::string> tags; // every tag, sorted
    int tag = 0;                    // 0: all, else tags[tag - 1]
    char search[128] = "";
    float tile = 150.0f;            // thumbnail width
    int selected = -1;
    std::string out_path, title;
    bool picked = false, quit = false;
};

std::string lower(std::string s) {
    for (char &c : s)
        c = (char)std::tolower((unsigned char)c);
    return s;
}

std::string read_file(const std::string &path) {
    std::ifstream f(path, std::ios::binary);
    std::stringstream s;
    s << f.rdbuf();
    return s.str();
}

// The presets of `dir`: "Default" first, then by name (case aside).
void load(App &app, const std::string &dir) {
    std::error_code ec;
    std::set<std::string> tags;
    for (const auto &entry : fs::directory_iterator(dir, ec)) {
        if (entry.path().extension() != ".json")
            continue;
        nlohmann::json j = nlohmann::json::parse(read_file(entry.path().string()), nullptr, false);
        if (!j.is_object() || !j.contains("params"))
            continue;
        Preset p;
        p.path = entry.path().string();
        p.thumbnail = fs::path(entry.path()).replace_extension(".png").string();
        p.name = j.value("name", entry.path().stem().string());
        p.lower = lower(p.name);
        if (j.contains("tags") && j["tags"].is_array())
            for (const auto &t : j["tags"])
                if (t.is_string()) {
                    p.tags.push_back(t.get<std::string>());
                    tags.insert(t.get<std::string>());
                    p.lower += " " + lower(t.get<std::string>());
                }
        app.presets.push_back(std::move(p));
    }
    std::sort(app.presets.begin(), app.presets.end(), [](const Preset &a, const Preset &b) {
        const bool da = a.name == "Default", db = b.name == "Default";
        return da != db ? da : lower(a.name) < lower(b.name);
    });
    app.tags.assign(tags.begin(), tags.end());
}

// Reads a preset's thumbnail into a texture (once).
void thumbnail(Preset &p) {
    p.tried = true;
    int n = 0;
    unsigned char *px = stbi_load(p.thumbnail.c_str(), &p.w, &p.h, &n, 4);
    if (!px)
        return;
    glGenTextures(1, &p.texture);
    glBindTexture(GL_TEXTURE_2D, p.texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, p.w, p.h, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
    stbi_image_free(px);
}

bool visible(const App &app, const Preset &p) {
    if (app.tag > 0 && std::find(p.tags.begin(), p.tags.end(), app.tags[app.tag - 1]) == p.tags.end())
        return false;
    return !app.search[0] || p.lower.find(lower(app.search)) != std::string::npos;
}

void pick(App &app) {
    if (app.selected < 0)
        return;
    std::ofstream f(app.out_path, std::ios::binary);
    f << app.presets[app.selected].path << "\n";
    app.picked = (bool)f;
    app.quit = true;
}

// A thumbnail (or a dark box until it is read) of w pixels, 16:9.
void image(Preset &p, float w) {
    const float h = w * 9.0f / 16.0f;
    if (p.texture) {
        ImGui::Image((ImTextureID)(intptr_t)p.texture, ImVec2(w, h));
    } else {
        const ImVec2 at = ImGui::GetCursorScreenPos();
        ImGui::GetWindowDrawList()->AddRectFilled(at, ImVec2(at.x + w, at.y + h),
                                                  IM_COL32(20, 20, 20, 255), 4.0f);
        ImGui::Dummy(ImVec2(w, h));
    }
}

void frame(App &app) {
    const ImGuiViewport *vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::Begin("presets", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus);
    const float right = 380.0f;

    // the list: search, tag, size, then the thumbnails
    ImGui::BeginChild("list", ImVec2(ImGui::GetContentRegionAvail().x - right, 0));
    ImGui::SetNextItemWidth(260);
    if (ImGui::IsWindowAppearing())
        ImGui::SetKeyboardFocusHere();
    ImGui::InputTextWithHint("##search", "Search presets", app.search, sizeof app.search);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(180);
    if (ImGui::BeginCombo("##tag", app.tag ? app.tags[app.tag - 1].c_str() : "All tags")) {
        if (ImGui::Selectable("All tags", app.tag == 0))
            app.tag = 0;
        for (int i = 0; i < (int)app.tags.size(); ++i)
            if (ImGui::Selectable(app.tags[i].c_str(), app.tag == i + 1))
                app.tag = i + 1;
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(140);
    ImGui::SliderFloat("##size", &app.tile, 90.0f, 320.0f, "Size %.0f");
    int shown = 0;
    for (const Preset &p : app.presets)
        shown += visible(app, p);
    ImGui::SameLine();
    ImGui::TextDisabled("%d presets", shown);

    ImGui::BeginChild("grid", ImVec2(0, 0), ImGuiChildFlags_Borders);
    const float pad = 10.0f, cell = app.tile + pad;
    const int columns = std::max(1, (int)((ImGui::GetContentRegionAvail().x + pad) / cell));
    int col = 0, reads = 0;
    for (int i = 0; i < (int)app.presets.size(); ++i) {
        Preset &p = app.presets[i];
        if (!visible(app, p))
            continue;
        if (col > 0)
            ImGui::SameLine(0.0f, pad);
        ImGui::BeginGroup();
        ImGui::PushID(i);
        const ImVec2 at = ImGui::GetCursorScreenPos();
        // a few thumbnails read per frame, those on screen first (the window opens at once)
        if (!p.tried && reads < 6 && ImGui::IsRectVisible(ImVec2(app.tile, app.tile)))
            ++reads, thumbnail(p);
        image(p, app.tile);
        // the name, cut to the tile
        std::string label = p.name;
        while (label.size() > 3 && ImGui::CalcTextSize(label.c_str()).x > app.tile)
            label = label.substr(0, label.size() - 4) + "...";
        ImGui::TextUnformatted(label.c_str());
        const ImVec2 end(at.x + app.tile, ImGui::GetCursorScreenPos().y);
        ImGui::SetCursorScreenPos(at);
        if (ImGui::InvisibleButton("tile", ImVec2(app.tile, end.y - at.y)))
            app.selected = i;
        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            app.selected = i;
            pick(app);
        }
        if (app.selected == i || ImGui::IsItemHovered())
            ImGui::GetWindowDrawList()->AddRect(
                ImVec2(at.x - 3, at.y - 3), ImVec2(end.x + 3, end.y + 1),
                app.selected == i ? IM_COL32(40, 120, 220, 255) : IM_COL32(120, 120, 120, 255),
                5.0f, 0, app.selected == i ? 3.0f : 1.0f);
        ImGui::PopID();
        ImGui::EndGroup();
        if (++col == columns)
            col = 0;
        else
            continue;
        ImGui::Dummy(ImVec2(0, pad * 0.5f));
    }
    for (Preset &p : app.presets) // then the others, a few per frame
        if (!p.tried && reads < 2)
            ++reads, thumbnail(p);
    ImGui::EndChild();
    ImGui::EndChild();

    // the selected preset
    ImGui::SameLine();
    ImGui::BeginChild("info", ImVec2(0, 0), ImGuiChildFlags_Borders);
    ImGui::TextDisabled("%s", app.title.c_str());
    ImGui::Separator();
    if (app.selected >= 0) {
        Preset &p = app.presets[app.selected];
        if (!p.tried)
            thumbnail(p);
        image(p, ImGui::GetContentRegionAvail().x);
        ImGui::Spacing();
        ImGui::TextWrapped("%s", p.name.c_str());
        if (!p.tags.empty()) {
            ImGui::Spacing();
            ImGui::TextDisabled("Tags");
            for (size_t t = 0; t < p.tags.size(); ++t) {
                if (t)
                    ImGui::SameLine();
                ImGui::SmallButton(p.tags[t].c_str());
            }
        }
    } else {
        ImGui::TextDisabled("Pick a preset.");
    }
    const float buttons = ImGui::GetFrameHeightWithSpacing();
    ImGui::SetCursorPosY(ImGui::GetWindowHeight() - buttons - ImGui::GetStyle().WindowPadding.y);
    ImGui::BeginDisabled(app.selected < 0);
    if (ImGui::Button("Load", ImVec2(120, 0)))
        pick(app);
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(120, 0)))
        app.quit = true;
    ImGui::EndChild();
    ImGui::End();

    if (ImGui::IsKeyPressed(ImGuiKey_Escape))
        app.quit = true;
    if ((ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter)) &&
        app.selected >= 0)
        pick(app);
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
    std::string dir, select;
    const char *shot = nullptr;
    for (int i = 1; i + 1 < argc; i += 2) {
        if (!std::strcmp(argv[i], "--dir"))
            dir = argv[i + 1];
        else if (!std::strcmp(argv[i], "--out"))
            app.out_path = argv[i + 1];
        else if (!std::strcmp(argv[i], "--title"))
            app.title = argv[i + 1];
        else if (!std::strcmp(argv[i], "--screenshot"))
            shot = argv[i + 1];
        else if (!std::strcmp(argv[i], "--select"))
            select = argv[i + 1];
    }
    load(app, dir);
    if (app.presets.empty()) {
        std::fprintf(stderr, "millsaph_preset_browser: no presets in %s\n", dir.c_str());
        return 2;
    }
    for (int i = 0; i < (int)app.presets.size(); ++i)
        if (app.presets[i].name == select)
            app.selected = i;
    if (app.selected < 0)
        app.selected = 0;

    if (!glfwInit())
        return 2;
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 2);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE); // (macOS)
    const std::string title = "MillSaph Presets" + (app.title.empty() ? "" : " - " + app.title);
    GLFWwindow *window = glfwCreateWindow(1280, 780, title.c_str(), nullptr, nullptr);
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
        // (thumbnails still to read: keep drawing)
        bool pending = false;
        for (const Preset &p : app.presets)
            pending |= !p.tried;
        if (shot || pending)
            glfwPollEvents();
        else
            glfwWaitEventsTimeout(0.5);
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
        if (shot && ++frames == 60) {
            screenshot(shot, w, h);
            app.quit = true;
        }
        glfwSwapBuffers(window);
    }
    for (Preset &p : app.presets)
        if (p.texture)
            glDeleteTextures(1, &p.texture);
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
    return app.picked ? 0 : 1;
}
