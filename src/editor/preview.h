// MillSaph's lens editor: the lens rendered by MS_LensFlare itself (the engine, on the GPU
// when there is one) over a background, for the preview.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct PreviewSettings {
    double hotspot[2] = {0.72, 0.62}; // fractions of the frame, y up
    double pivot[2] = {0.5, 0.5};
    double scale_widths = 0.21; // MS_LensFlare's, a fraction of the width
    double brightness = 1.0;
    int background = 0; // 0: black, 1: grey, 2: a dusk gradient
};

class Preview {
  public:
    // The lens `text` at width x height into rgba (8 bits, sRGB-ish, top row first); false
    // with the reason in err.
    bool render(const std::string &text, const PreviewSettings &s, int width, int height,
                std::vector<uint8_t> &rgba, std::string &err);
    const char *api() const { return api_; }

  private:
    std::vector<float> source_, output_;
    const char *api_ = "";
};
