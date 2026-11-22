// What MillSaph's two windows (the lens editor, the preset browser) share: a GLFW window
// with Dear ImGui on OpenGL 3.2, its frame loop, the screenshots of their tests.

#pragma once

#include <functional>
#include <string>

namespace window {

struct Loop {
    std::function<void()> frame;        // the ImGui calls of a frame (between NewFrame and Render)
    std::function<void()> after_render; // optional: after ImGui::Render, before drawing
    std::function<double()> wait;       // how long to wait for events (< 0: none, draw again)
    std::function<bool()> quit;         // whether to close the window
    std::function<void()> shut_down;    // optional: before ImGui and the window go (GL there)
    std::function<void()> look;         // optional: the ImGui style and fonts (else dark)
};

// Runs `loop` in a window `width` x `height` until it quits or the window is closed. With
// `shot`: no waiting, the window saved there (PPM) after `shot_frames` frames, then closed.
// Returns 0, or 2 when the window cannot be made.
int run(const char *title, int width, int height, const char *shot, int shot_frames,
        const Loop &loop);

// The window's pixels (w x h) as a PPM file. Returns false if it cannot be written.
bool screenshot(const char *path, int w, int h);

// A whole file ("" if it cannot be read).
std::string read_file(const std::string &path);

} // namespace window
