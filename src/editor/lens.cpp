// See lens.h.

#include "lens.h"

#include <cstring>

namespace lens {

Json &Lens::elements() {
    Json &root = doc.contains("value") && doc["value"].is_object() ? doc["value"] : doc;
    if (!root.contains("elements") || !root["elements"].is_array())
        root["elements"] = Json::array();
    return root["elements"];
}

bool parse(const std::string &text, Lens &out, std::string &err) {
    const size_t brace = text.find('{');
    if (brace == std::string::npos) {
        err = "not a lens (no JSON)";
        return false;
    }
    try {
        out.prefix = text.substr(0, brace);
        out.doc = Json::parse(text.substr(brace));
    } catch (const std::exception &e) {
        err = e.what();
        return false;
    }
    out.elements();
    return true;
}

std::string write(const Lens &lens) { return lens.prefix + lens.doc.dump(1, '\t') + "\n"; }

static const char *const ALL = "";
static const char *const STOPS = "blob|spot|ring|polar_flare|fancy_spot|hexagon";
static const char *const SECTOR = "polar_flare|fancy_spot";
static const char *const POLAR = "polar_flare";
static const char *const RAYS = "rays";

const std::vector<Field> &fields() {
    static const std::vector<Field> list = {
        {"name", "Name", "Element", Kind::Text, 0, 0, 0, nullptr, ALL},
        {"center", "Center", "Element", Kind::Number, 1, -3, 3, nullptr, ALL},
        {"size", "Size", "Element", Kind::Number, 0.3, 0, 3, nullptr, ALL},
        {"brightness", "Brightness", "Element", Kind::Number, 0.5, 0, 3, nullptr, ALL},
        {"tint", "Tint", "Element", Kind::Color, 1, 0, 1, nullptr, ALL},
        {"color", "Color", "Element", Kind::Color, 1, 0, 1, nullptr, RAYS},
        {"rotate", "Rotate", "Element", Kind::Number, 0, -360, 360, nullptr, ALL},
        {"rotate_to_pivot", "Rotate To Pivot", "Element", Kind::Bool, 0, 0, 1, nullptr, ALL},
        {"ignore_atmosphere", "Ignore Atmosphere", "Element", Kind::Bool, 0, 0, 1, nullptr, ALL},
        {"stretch", "Stretch", "Shape", Kind::Number, 1, 0.05, 10, nullptr, ALL},
        {"stretch_distort", "Stretch Distort", "Shape", Kind::Number, 0, -1, 1, nullptr, ALL},
        {"rel_width", "Rel Width", "Shape", Kind::Number, 1, 0.01, 10, nullptr, ALL},
        {"rel_height", "Rel Height", "Shape", Kind::Number, 1, 0.01, 10, nullptr, ALL},
        {"fade_gradient", "Fade Gradient", "Shape", Kind::Number, 0, -1, 1, nullptr, STOPS},
        {"blur_gradient", "Blur Gradient", "Shape", Kind::Number, 0, 0, 1, nullptr, SECTOR},
        {"offset_bright", "Offset Bright", "Shape", Kind::Number, 0, -1, 3, nullptr, POLAR},
        {"angle_width", "Angle Width", "Sector", Kind::Number, 360, 0, 360, nullptr, SECTOR},
        {"start_angle", "Start Angle", "Sector", Kind::Number, 0, -360, 360, nullptr, SECTOR},
        {"arc_edge_softness", "Arc Edge Softness", "Sector", Kind::Number, 0, 0, 1, nullptr, SECTOR},
        {"repeats", "Repeats", "Sector", Kind::Integer, 1, 1, 32, nullptr, SECTOR},
        {"n_sides", "Sides", "Sector", Kind::Integer, 0, 0, 16, nullptr, SECTOR},
        {"roundness", "Roundness", "Sector", Kind::Number, 0, 0, 1, nullptr, SECTOR},
        {"ray_number", "Ray Number", "Rays", Kind::Integer, 8, 0, 128, nullptr, RAYS},
        {"ray_width", "Ray Width", "Rays", Kind::Number, 0.03, 0, 0.3, nullptr, RAYS},
        {"radius_range", "Radius Range", "Rays", Kind::Pair, 1, 0, 1, nullptr, RAYS},
        {"jitter_angles", "Jitter Angles", "Rays", Kind::Number, 0, 0, 1, nullptr, RAYS},
        {"rand_seed", "Random Seed", "Rays", Kind::Integer, 0, 0, 1000, nullptr, ALL},
        {"wave_brightness", "Wave Brightness", "Waves", Kind::Number, 0, 0, 3, nullptr, POLAR},
        {"wave_freq", "Wave Freq", "Waves", Kind::Number, 1, 0, 64, nullptr, POLAR},
        {"wave_width", "Wave Width", "Waves", Kind::Number, 1, 0, 1, nullptr, POLAR},
        {"wave_softness", "Wave Softness", "Waves", Kind::Number, 1, 0, 4, nullptr, POLAR},
        {"wave_phase", "Wave Phase", "Waves", Kind::Number, 0, -1, 1, nullptr, POLAR},
        {"r_wave_min", "Radius Wave Min", "Waves", Kind::Number, 1, 0, 2, nullptr, POLAR},
        {"r_wave_max", "Radius Wave Max", "Waves", Kind::Number, 1, 0, 2, nullptr, POLAR},
        {"r_wave_freq", "Radius Wave Freq", "Waves", Kind::Number, 1, 0, 64, nullptr, POLAR},
        {"r_spikyness", "Radius Spikyness", "Waves", Kind::Number, 1, 0.1, 8, nullptr, POLAR},
        {"noise_brightness", "Noise Brightness", "Noise", Kind::Number, 0, 0, 2, nullptr, POLAR},
        {"noise_width", "Noise Width", "Noise", Kind::Number, 0.3, 0, 1, nullptr, POLAR},
        {"noise_softness", "Noise Softness", "Noise", Kind::Number, 1, 0, 4, nullptr, POLAR},
        {"noise_freq", "Noise Freq", "Noise", Kind::Number, 120, 1, 400, nullptr, POLAR},
        {"noise_octaves", "Noise Octaves", "Noise", Kind::Integer, 1, 1, 8, nullptr, POLAR},
        {"blur_theta_map", "Blur Theta Map", "Noise", Kind::Number, 0, 0, 1, nullptr, POLAR},
        {"r_noise_min", "Radius Noise Min", "Noise", Kind::Number, 1, 0, 2, nullptr, POLAR},
        {"r_noise_max", "Radius Noise Max", "Noise", Kind::Number, 1, 0, 2, nullptr, POLAR},
        {"r_noise_freq", "Radius Noise Freq", "Noise", Kind::Number, 5, 0.1, 64, nullptr, POLAR},
        {"r_noise_octaves", "Radius Noise Octaves", "Noise", Kind::Integer, 1, 1, 8, nullptr, POLAR},
        {"rel_speed", "Rel Speed", "Distance", Kind::Number, 1, 0, 3, nullptr, ALL},
        {"scale_brightness_by_dist", "Scale Brightness By Dist", "Distance", Kind::Number, 0, 0, 3, nullptr, ALL},
        {"min_brightness_scale", "Min Brightness Scale", "Distance", Kind::Number, 0, 0, 1, nullptr, ALL},
        {"brightness_scale_inverted", "Brightness Scale Inverted", "Distance", Kind::Bool, 0, 0, 1, nullptr, ALL},
        {"scale_width_by_dist", "Scale Width By Dist", "Distance", Kind::Number, 0, 0, 3, nullptr, ALL},
        {"min_width_scale", "Min Width Scale", "Distance", Kind::Number, 0, 0, 1, nullptr, ALL},
        {"width_scale_inverted", "Width Scale Inverted", "Distance", Kind::Bool, 0, 0, 1, nullptr, ALL},
        {"offset_stretch_by_dist", "Offset Stretch By Dist", "Distance", Kind::Number, 0, 0, 4, nullptr, ALL},
        {"brightness_response", "Brightness Response", "Distance", Kind::Number, 1, -2, 4, nullptr, ALL},
        {"width_response", "Width Response", "Distance", Kind::Number, 1, -2, 4, nullptr, ALL},
        {"offset_mode", "Offset Mode", "Offsets", Kind::Choice, 0, 0, 0,
         "normal|horizontal stretch|vertical stretch|fixed|horizontal follow", ALL},
        {"unscaled_offset", "Offset (frame)", "Offsets", Kind::Pair, 0, -1, 1, nullptr, ALL},
        {"scaled_offset", "Offset (by distance)", "Offsets", Kind::Pair, 0, -1, 1, nullptr, ALL},
        {"offset_off_axis", "Offset Off Axis", "Offsets", Kind::Number, 0, -1, 1, nullptr, ALL},
        {"shift_edge", "Shift Edge", "Offsets", Kind::Number, 0, -2, 2, nullptr, SECTOR},
        {"use_shadow_p", "Use Shadow", "Shadow", Kind::Bool, 0, 0, 1, nullptr, ALL},
        {"shadow_rel_scale", "Shadow Rel Scale", "Shadow", Kind::Number, 1, 0.1, 4, nullptr, ALL},
        {"shadow_amount_at_edge", "Shadow Amount At Edge", "Shadow", Kind::Number, 1, -8, 8, nullptr, ALL},
        {"shadow_offset_along_axis", "Shadow Offset Along Axis", "Shadow", Kind::Number, 0, -2, 2, nullptr, ALL},
        {"shadow_offset_off_axis", "Shadow Offset Off Axis", "Shadow", Kind::Number, 0, -2, 2, nullptr, ALL},
        {"shadow_transparency", "Shadow Transparency", "Shadow", Kind::Number, 0, 0, 1, nullptr, ALL},
        {"invert_shadow", "Invert Shadow", "Shadow", Kind::Bool, 0, 0, 1, nullptr, ALL},
        {"image_scale_size", "Image Scale", "Image", Kind::Number, 1, 0.05, 4, nullptr, ALL},
        {"image_offset_rotate", "Image Rotate", "Image", Kind::Number, 0, -360, 360, nullptr, ALL},
        {"image_offset_x", "Image Offset X", "Image", Kind::Number, 0, -2, 2, nullptr, ALL},
        {"image_offset_y", "Image Offset Y", "Image", Kind::Number, 0, -2, 2, nullptr, ALL},
        {"image_absolute_position_p", "Image At The Pivot", "Image", Kind::Bool, 0, 0, 1, nullptr, ALL},
        {"image_absolute_size_p", "Image Frame Sized", "Image", Kind::Bool, 0, 0, 1, nullptr, ALL},
        {"image_absolute_rotate_p", "Image Unturned", "Image", Kind::Bool, 0, 0, 1, nullptr, ALL},
    };
    return list;
}

const Field *field(const std::string &name) {
    for (const Field &f : fields())
        if (name == f.name)
            return &f;
    return nullptr;
}

bool field_for(const Field &f, const std::string &type) {
    if (!*f.types)
        return true;
    std::string all = std::string("|") + f.types + "|";
    return all.find("|" + type + "|") != std::string::npos;
}

const std::vector<const char *> &element_types() {
    static const std::vector<const char *> list = {"blob", "spot", "ring", "rays",
                                                   "polar_flare", "fancy_spot", "texture"};
    return list;
}

Json new_element(const std::string &type) {
    Json e = Json::object();
    e["type"] = type;
    e["name"] = "New " + type;
    e["center"] = 0.5;
    e["size"] = 0.2;
    e["brightness"] = 0.3;
    if (type == "rays") {
        e["ray_number"] = 8;
        e["ray_width"] = 0.03;
        e["color"] = {1, 1, 1};
    } else if (type == "texture") {
        e["colors"] = {1, 1, 1};
        e["image"] = {{"generate", "glint"}, {"seed", 1}};
    } else if (type == "ring") {
        e["radii"] = {0.85, 0.93, 1};
        e["colors"] = {0, 0, 0, 1, 1, 1, 0, 0, 0};
    } else if (type == "blob") {
        e["colors"] = {1, 1, 1};
    } else {
        e["radii"] = {0, 0.8, 1};
        e["colors"] = {1, 1, 1, 1, 1, 1, 0.5, 0.5, 0.5};
    }
    if (type == "polar_flare" || type == "fancy_spot")
        e["angle_width"] = 360;
    return e;
}

} // namespace lens
