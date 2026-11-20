// See lens.h.

#include "lens.h"

#include <cmath>
#include <cstring>

namespace lens {

Json &Lens::root() { return doc.contains("value") && doc["value"].is_object() ? doc["value"] : doc; }

Json &Lens::elements() {
    Json &r = root();
    if (!r.contains("elements") || !r["elements"].is_array())
        r["elements"] = Json::array();
    return r["elements"];
}

Json &Lens::shadow() {
    Json &r = root();
    if (!r.contains("shadow") || !r["shadow"].is_array())
        r["shadow"] = Json::array();
    return r["shadow"];
}

// A lens file in segments (a "#" line before each JSON: its metadata, then its elements):
// one {"value": {...}} object.
static bool parse_segments(const std::string &text, Lens &out, std::string &err) {
    Json value = Json::object();
    size_t at = 0;
    while ((at = text.find('{', at)) != std::string::npos) {
        // a segment: from its "{" to the next line starting with "#"
        size_t end = text.find("\n#", at);
        if (end == std::string::npos)
            end = text.size();
        try {
            const Json seg = Json::parse(text.substr(at, end - at));
            if (seg.is_object())
                for (auto it = seg.begin(); it != seg.end(); ++it)
                    value[it.key()] = it.value();
        } catch (const std::exception &e) {
            err = e.what();
            return false;
        }
        at = end;
    }
    out.prefix.clear();
    out.doc = {{"value", value}};
    return true;
}

bool parse(const std::string &text, Lens &out, std::string &err) {
    if (text.rfind("#", 0) == 0 && text.find("\n#", 1) != std::string::npos) {
        if (!parse_segments(text, out, err))
            return false;
        out.elements();
        return true;
    }
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
    if (!out.doc.is_object()) {
        err = "not a lens (no object)";
        return false;
    }
    out.elements();
    return true;
}

std::string write(const Lens &lens) { return lens.prefix + lens.doc.dump(1, '\t') + "\n"; }

// ------------------------------------------------------------------ fields

static const char *const ALL = "";
static const char *const STOPS = "blob|spot|ring|polar_flare|fancy_spot|hexagon|texture";
static const char *const SECTOR = "polar_flare|fancy_spot";
static const char *const SPOT = "fancy_spot";
static const char *const POLAR = "polar_flare";
static const char *const RAYS = "rays";
static const char *const NOT_RAYS = "blob|spot|ring|polar_flare|fancy_spot|hexagon|texture|container";
static const char *const GEN = "generator";

const std::vector<const char *> &groups() {
    static const std::vector<const char *> list = {
        "Position", "Size",    "Color and Brightness", "Options", "Animation",
        "Triggers", "Noise",   "Texture Overlay",      "Shadow"};
    return list;
}

const std::vector<Field> &fields() {
    static const std::vector<Field> list = {
        // Position
        {"center", "Position", "Position", Kind::Number, 1, -3, 3, nullptr, ALL, nullptr},
        {"rotate", "Rotate", "Position", Kind::Number, 0, -180, 180, nullptr, ALL, nullptr},
        {"unscaled_offset", "Offset", "Position", Kind::Pair, 0, -1, 1, nullptr, ALL, nullptr},
        {"scaled_offset", "Offset By Distance", "Position", Kind::Pair, 0, -1, 1, nullptr, ALL, nullptr},
        {"offset_mode", "Offset Mode", "Position", Kind::Choice, 0, 0, 0,
         "normal|horizontal stretch|vertical stretch|fixed|horizontal follow", ALL, nullptr},
        {"offset_off_axis", "Offset Off Axis", "Position", Kind::Number, 0, -1, 1, nullptr, ALL, nullptr},
        {"rel_speed", "Rel Speed", "Position", Kind::Number, 1, 0, 4, nullptr, ALL, nullptr},
        // Size
        {"size", "Size", "Size", Kind::Number, 1, 0, 3, nullptr, ALL, nullptr},
        {"rel_width", "Rel Width", "Size", Kind::Number, 1, 0.01, 4, nullptr, ALL, nullptr},
        {"rel_height", "Rel Height", "Size", Kind::Number, 1, 0.01, 4, nullptr, ALL, nullptr},
        {"stretch", "Stretch", "Size", Kind::Number, 1, 0.05, 10, nullptr, ALL, nullptr},
        {"stretch_distort", "Distortion", "Size", Kind::Number, 0, -1, 1, nullptr, ALL, "blob|rays"},
        // Color and Brightness (the gradient after the colour, see the editor)
        {"brightness", "Brightness", "Color and Brightness", Kind::Number, 1, 0, 3, nullptr, ALL, nullptr},
        {"tint", "Color", "Color and Brightness", Kind::Color, 1, 0, 1, nullptr, NOT_RAYS, nullptr},
        {"color", "Color", "Color and Brightness", Kind::Color, 1, 0, 1, nullptr, RAYS, nullptr},
        {"fade_gradient", "Fade Gradient", "Color and Brightness", Kind::Number, 0, -1, 1, nullptr, SECTOR, nullptr},
        {"blur_gradient", "Blur Gradient", "Color and Brightness", Kind::Number, 0, 0, 2, nullptr, SECTOR, nullptr},
        // Options: Multi Spot / Multi Streak (their children made again, see generate)
        {"number", "Number", "Options", Kind::Integer, 4, 1, 24, nullptr, GEN, nullptr},
        {"spread", "Spread", "Options", Kind::Number, 1, 0, 4, nullptr, GEN, nullptr},
        {"vary_spacing", "Vary Spacing", "Options", Kind::Number, 0.2, 0, 1, nullptr, GEN, nullptr},
        {"vary_size", "Vary Size", "Options", Kind::Number, 0.5, 0, 1, nullptr, GEN, nullptr},
        {"vary_brightness", "Vary Brightness", "Options", Kind::Number, 0.5, 0, 1, nullptr, GEN, nullptr},
        {"vary_hue", "Vary Hue", "Options", Kind::Number, 0.1, 0, 1, nullptr, GEN, nullptr},
        {"vary_rotate", "Vary Rotate", "Options", Kind::Number, 0, 0, 1, nullptr, GEN, nullptr},
        // Options: spots, polygons, rings
        {"shape", "Shape", "Options", Kind::Shape, 0, 0, 0, "Circle|Polygon", SPOT, nullptr},
        {"n_sides", "Sides", "Options", Kind::Integer, 0, 3, 16, nullptr, SECTOR, nullptr},
        {"roundness", "Roundness", "Options", Kind::Number, 0, 0, 1, nullptr, SECTOR, nullptr},
        {"random_side_lengths", "Randomize Sides", "Options", Kind::Number, 0, 0, 1, nullptr, SECTOR, nullptr},
        {"repeats", "Repeats", "Options", Kind::Integer, 1, 1, 32, nullptr, SECTOR, nullptr},
        {"angle_width", "Angle Width", "Options", Kind::Number, 360, 0, 360, nullptr, SECTOR, nullptr},
        {"arc_edge_softness", "Angle Softness", "Options", Kind::Number, 0, 0, 1, nullptr, SECTOR, nullptr},
        {"start_angle", "Start Angle", "Options", Kind::Number, 0, -360, 360, nullptr, SECTOR, nullptr},
        {"shift_edge", "Shift Edge", "Options", Kind::Number, 0, -2, 2, nullptr, SECTOR, nullptr},
        // Options: advanced elements (and the rays made of them)
        {"wave_brightness", "Brightness Wave", "Options", Kind::Number, 0, 0, 3, nullptr, POLAR, nullptr},
        {"wave_freq", "Wave Frequency", "Options", Kind::Number, 1, 0, 64, nullptr, POLAR, nullptr},
        {"wave_phase", "Wave Phase", "Options", Kind::Number, 0, -1, 1, nullptr, POLAR, nullptr},
        {"wave_width", "Wave Width", "Options", Kind::Number, 1, 0, 1, nullptr, POLAR, nullptr},
        {"wave_softness", "Wave Softness", "Options", Kind::Number, 1, 0, 4, nullptr, POLAR, nullptr},
        {"noise_brightness", "Brightness Random", "Options", Kind::Number, 0, 0, 2, nullptr, POLAR, nullptr},
        {"noise_freq", "Random Frequency", "Options", Kind::Number, 120, 1, 400, nullptr, POLAR, nullptr},
        {"noise_width", "Random Width", "Options", Kind::Number, 0.3, 0, 1, nullptr, POLAR, nullptr},
        {"noise_softness", "Random Softness", "Options", Kind::Number, 1, 0, 4, nullptr, POLAR, nullptr},
        {"noise_octaves", "Octaves", "Options", Kind::Integer, 1, 1, 8, nullptr, POLAR, nullptr},
        {"offset_bright", "Add Brightness", "Options", Kind::Number, 0, -1, 3, nullptr, POLAR, nullptr},
        {"mult_brightness_p", "Brightness Combine", "Options", Kind::Flag, 0, 0, 1, "Add|Multiply", POLAR, nullptr},
        {"blur_theta_map", "Blur Brightness", "Options", Kind::Number, 0, 0, 1, nullptr, POLAR, nullptr},
        {"r_wave_min", "Radius Wave Min", "Options", Kind::Number, 1, 0, 2, nullptr, POLAR, nullptr},
        {"r_wave_max", "Radius Wave Max", "Options", Kind::Number, 1, 0, 2, nullptr, POLAR, nullptr},
        {"r_wave_freq", "Radius Wave Freq", "Options", Kind::Number, 1, 0, 64, nullptr, POLAR, nullptr},
        {"r_noise_min", "Radius Random Min", "Options", Kind::Number, 1, 0, 2, nullptr, POLAR, nullptr},
        {"r_noise_max", "Radius Random Max", "Options", Kind::Number, 1, 0, 2, nullptr, POLAR, nullptr},
        {"r_noise_freq", "Radius Rand Freq", "Options", Kind::Number, 5, 0.1, 64, nullptr, POLAR, nullptr},
        {"r_noise_octaves", "Radius Octaves", "Options", Kind::Integer, 1, 1, 8, nullptr, POLAR, nullptr},
        {"r_spikyness", "Spikyness", "Options", Kind::Number, 1, 0.1, 8, nullptr, POLAR, nullptr},
        // Options: rays
        {"ray_number", "Ray Number", "Options", Kind::Integer, 4, 0, 128, nullptr, RAYS, nullptr},
        {"jitter_angles", "Jitter Angles", "Options", Kind::Number, 0, 0, 1, nullptr, RAYS, nullptr},
        {"radius_range", "Vary Length", "Options", Kind::Pair, 1, 0, 1, nullptr, RAYS, nullptr},
        {"ray_width", "Ray Thickness", "Options", Kind::Number, 0.05, 0, 0.3, nullptr, RAYS, nullptr},
        {"streaks_amp", "Streakiness", "Options", Kind::Number, 0, 0, 1, nullptr, RAYS, nullptr},
        {"streaks_freq", "Streaks Frequency", "Options", Kind::Number, 1, 0, 8, nullptr, RAYS, nullptr},
        {"streaks_octaves_scale", "Streaks Detail", "Options", Kind::Number, 0.3, 0, 1, nullptr, RAYS, nullptr},
        // Animation
        {"scale_width_by_dist", "Autoscale Size", "Animation", Kind::Number, 0, 0, 3, nullptr, ALL, nullptr},
        {"min_width_scale", "Minimum Size", "Animation", Kind::Number, 0, 0, 1, nullptr, ALL, nullptr},
        {"width_scale_inverted", "Size Direction", "Animation", Kind::Flag, 0, 0, 1, "Grow|Shrink", ALL, nullptr},
        {"scale_brightness_by_dist", "Autoscale Brightness", "Animation", Kind::Number, 0, 0, 3, nullptr, ALL, nullptr},
        {"min_brightness_scale", "Minimum Brightness", "Animation", Kind::Number, 0, 0, 1, nullptr, ALL, nullptr},
        {"brightness_scale_inverted", "Brightness Direction", "Animation", Kind::Flag, 0, 0, 1, "Brighten|Darken", ALL, nullptr},
        {"offset_stretch_by_dist", "Auto Stretch", "Animation", Kind::Number, 0, 0, 4, nullptr, ALL, nullptr},
        {"rotate_to_pivot", "Auto Rotate", "Animation", Kind::Bool, 0, 0, 1, nullptr, ALL, nullptr},
        // Triggers
        {"center_trigger", "Center Trigger", "Triggers", Kind::Bool, 1, 0, 1, nullptr, ALL, nullptr},
        {"edge_trigger", "Edge Trigger", "Triggers", Kind::Bool, 1, 0, 1, nullptr, ALL, nullptr},
        {"brightness_response", "Brightness Response", "Triggers", Kind::Number, 1, -2, 4, nullptr, ALL, nullptr},
        {"width_response", "Width Response", "Triggers", Kind::Number, 1, -2, 4, nullptr, ALL, nullptr},
        // Noise
        {"ignore_atmosphere", "Ignore Atmosphere", "Noise", Kind::Bool, 0, 0, 1, nullptr, ALL, nullptr},
        {"noise_amp", "Noise Amount", "Noise", Kind::Number, 0, 0, 1, nullptr, ALL, nullptr},
        {"noise_vary_by_position", "Vary By Position", "Noise", Kind::Number, 0, 0, 1, nullptr, ALL, nullptr},
        {"simplex_noise_freq", "Noise Frequency", "Noise", Kind::Number, 4, 0, 10, nullptr, ALL, nullptr},
        {"freq_rel_x", "Frequency Rel X", "Noise", Kind::Number, 1, 0, 2, nullptr, ALL, nullptr},
        {"freq_rel_y", "Frequency Rel Y", "Noise", Kind::Number, 1, 0, 2, nullptr, ALL, nullptr},
        {"octaves_scale", "Noise Detail", "Noise", Kind::Number, 0.2, 0, 1, nullptr, ALL, nullptr},
        {"rand_seed", "Random Seed", "Noise", Kind::Integer, 0, 0, 1000, nullptr, ALL, nullptr},
        // Texture Overlay (the image itself first, see the editor)
        {"image_absolute_size_p", "Fixed Size", "Texture Overlay", Kind::Bool, 0, 0, 1, nullptr, ALL, nullptr},
        {"image_absolute_position_p", "Fixed Position", "Texture Overlay", Kind::Bool, 0, 0, 1, nullptr, ALL, nullptr},
        {"image_absolute_rotate_p", "Fixed Rotation", "Texture Overlay", Kind::Bool, 0, 0, 1, nullptr, ALL, nullptr},
        {"image_scale_size", "Scale Factor", "Texture Overlay", Kind::Number, 1, 0.05, 4, nullptr, ALL, nullptr},
        {"image_offset_x", "Offset X", "Texture Overlay", Kind::Number, 0, -2, 2, nullptr, ALL, nullptr},
        {"image_offset_y", "Offset Y", "Texture Overlay", Kind::Number, 0, -2, 2, nullptr, ALL, nullptr},
        {"image_offset_rotate", "Rotate", "Texture Overlay", Kind::Number, 0, -180, 180, nullptr, ALL, nullptr},
        // Shadow
        {"use_shadow_p", "Use Shadow", "Shadow", Kind::Bool, 0, 0, 1, nullptr, ALL, nullptr},
        {"shadow_transparency", "Transparency", "Shadow", Kind::Number, 0, 0, 1, nullptr, ALL, nullptr},
        {"shadow_dist_calc", "Distance Calculation", "Shadow", Kind::Choice, 0, 0, 0, "use hotspot|use element", ALL, nullptr},
        {"shadow_rel_scale", "Rel Scale Shadow", "Shadow", Kind::Number, 1, 0.1, 4, nullptr, ALL, nullptr},
        {"shadow_offset_along_axis", "Offset Position", "Shadow", Kind::Number, 0, -2, 2, nullptr, ALL, nullptr},
        {"shadow_offset_off_axis", "Off Axis Position", "Shadow", Kind::Number, 0, -2, 2, nullptr, ALL, nullptr},
        {"shadow_amount_at_edge", "Shading", "Shadow", Kind::Number, 1, -8, 8, nullptr, ALL, nullptr},
        {"shadow_animation_delay", "Animation Delay", "Shadow", Kind::Number, 0.1, 0, 1, nullptr, ALL, nullptr},
        {"invert_shadow", "Invert Shadow", "Shadow", Kind::Bool, 0, 0, 1, nullptr, ALL, nullptr},
    };
    return list;
}

const Field *field(const std::string &name) {
    for (const Field &f : fields())
        if (name == f.name)
            return &f;
    return nullptr;
}

static bool in_list(const char *list, const std::string &type) {
    const std::string all = std::string("|") + list + "|";
    return all.find("|" + type + "|") != std::string::npos;
}

bool field_for(const Field &f, const std::string &type) { return !*f.types || in_list(f.types, type); }

bool field_grey(const Field &f, const std::string &type) { return f.grey && in_list(f.grey, type); }

// A container's change reaches its children: scaled for sizes, brightnesses and their
// factors, offset for positions and angles, overwritten for the rest (the lens's own
// param_modes first).
Mode mode(const Json &container, const std::string &name) {
    if (container.contains("param_modes") && container["param_modes"].is_object() &&
        container["param_modes"].contains(name) && container["param_modes"][name].is_string()) {
        const std::string m = container["param_modes"][name].get<std::string>();
        if (m == "ScaleChildren")
            return Mode::Scale;
        if (m == "OffsetChildren")
            return Mode::Offset;
        return Mode::Overwrite;
    }
    static const char *const scaled[] = {
        "size", "brightness", "rel_width", "rel_height", "stretch", "tint", "rel_speed",
        "brightness_response", "width_response", "scale_width_by_dist", "scale_brightness_by_dist",
        "noise_amp", "image_scale_size", "shadow_rel_scale"};
    static const char *const offset[] = {"center", "rotate", "image_offset_rotate", "offset_off_axis"};
    for (const char *s : scaled)
        if (name == s)
            return Mode::Scale;
    for (const char *s : offset)
        if (name == s)
            return Mode::Offset;
    return Mode::Overwrite;
}

// ------------------------------------------------------------------ element kinds

const std::vector<ElementKind> &element_kinds() {
    static const std::vector<ElementKind> list = {
        {"Blob", "Blob"},
        {"Spot", "Spot"},
        {"Polygon", "Polygon"},
        {"Ring", "Ring"},
        {"MultiSpot", "Multi Spot"},
        {"Rays", "Rays"},
        {"ShimmerRays", "Shimmer Rays"},
        {"FanRays", "Fan Rays"},
        {"SpikyRays", "Spiky Rays"},
        {"ChromaRing", "Chroma Ring"},
        {"MultiStreak", "Multi Streak"},
        {"Advanced", "Advanced Element"},
    };
    return list;
}

static Json stops(std::initializer_list<double> radii, std::initializer_list<double> colors) {
    return {{"radii", Json(radii)}, {"colors", Json(colors)}};
}

Json new_element(const std::string &uitype, int number) {
    std::string label = uitype;
    for (const ElementKind &k : element_kinds())
        if (uitype == k.uitype)
            label = k.label;
    Json e = Json::object();
    if (uitype == "Group") {
        e = {{"type", "container"}, {"uitype", "Group"}, {"children", Json::array()}};
        e["name"] = "Group " + std::to_string(number);
        return e;
    }
    e["name"] = label + " " + std::to_string(number);
    e["uitype"] = uitype;
    e["center"] = 1.0;
    e["size"] = 0.5;
    e["brightness"] = 1.0;
    if (uitype == "Blob") {
        e["type"] = "blob";
        e["size"] = 1.0;
        e["colors"] = {1, 1, 1};
    } else if (uitype == "Spot" || uitype == "Polygon" || uitype == "MultiSpot") {
        e["type"] = "fancy_spot";
        e["center"] = uitype == "MultiSpot" ? 0.0 : 0.5;
        e["size"] = uitype == "MultiSpot" ? 0.05 : 0.15;
        e["brightness"] = 0.4;
        e.update(stops({0, 0.85, 1}, {1, 1, 1, 1, 1, 1, 0, 0, 0}));
        if (uitype == "Polygon")
            e["n_sides"] = 6, e["shape"] = "polygon";
    } else if (uitype == "Ring") {
        e["type"] = "fancy_spot";
        e["size"] = 0.15;
        e["brightness"] = 0.4;
        e.update(stops({0, 0.9, 0.93, 0.97, 1}, {0, 0, 0, 0, 0, 0, 1, 1, 1, 0, 0, 0, 0, 0, 0}));
    } else if (uitype == "Rays") {
        e["type"] = "rays";
        e["size"] = 0.7;
        e["ray_number"] = 3;
        e["jitter_angles"] = 1.0;
        e["ray_width"] = 0.03;
        e["color"] = {1, 1, 1};
    } else if (uitype == "ShimmerRays" || uitype == "SpikyRays") {
        e["type"] = "polar_flare";
        e["size"] = uitype == "ShimmerRays" ? 1.0 : 0.5;
        e.update(stops({0, 0.6, 1}, {1, 1, 1, 0.3, 0.3, 0.3, 0, 0, 0}));
        e["noise_brightness"] = 1.0;
        e["noise_freq"] = uitype == "ShimmerRays" ? 120.0 : 60.0;
        e["noise_width"] = 0.3;
        if (uitype == "SpikyRays") {
            e["r_noise_min"] = 0.15;
            e["r_noise_max"] = 1.0;
            e["r_noise_freq"] = 10.0;
            e["r_noise_octaves"] = 4;
            e["r_spikyness"] = 3.0;
        }
    } else if (uitype == "FanRays") {
        e["type"] = "polar_flare";
        e.update(stops({0, 1}, {1, 1, 1, 0, 0, 0}));
        e["wave_brightness"] = 1.0;
        e["wave_freq"] = 6.0;
        e["wave_width"] = 0.5;
        e["wave_softness"] = 0.8;
        e["mult_brightness_p"] = 1;
    } else if (uitype == "ChromaRing") {
        e["type"] = "fancy_spot";
        e["brightness"] = 3.0;
        e["angle_width"] = 120.0;
        e["arc_edge_softness"] = 0.2;
        e["blur_gradient"] = 0.05;
        e.update(stops({0.86, 0.875, 0.89, 0.905, 0.92, 0.935, 0.95, 0.965, 0.98, 1},
                       {0, 0, 0, 0.3, 0, 0, 1, 0, 0, 1, 0.4, 0, 0.8, 0.8, 0, 0, 1, 0, 0, 0.8, 0.8,
                        0, 0.3, 1, 0, 0, 0.6, 0, 0, 0}));
    } else if (uitype == "MultiStreak") {
        e["type"] = "blob";
        e["size"] = 1.0;
        e["rel_width"] = 6.0;
        e["rel_height"] = 0.02;
        e["rotate"] = 52.0;
        e["colors"] = {1, 1, 1};
    } else { // Advanced
        e["type"] = "polar_flare";
        e.update(stops({0, 1}, {1, 1, 1, 0, 0, 0}));
    }
    if (uitype == "MultiSpot" || uitype == "MultiStreak") {
        // a container of copies of the model, made again from Number, Spread and the Vary
        Json model = e;
        model.erase("name");
        model.erase("uitype");
        Json c = e;
        c["type"] = "container";
        c["number"] = uitype == "MultiSpot" ? 4 : 3;
        c["spread"] = uitype == "MultiSpot" ? 1.0 : 0.1;
        c["vary_spacing"] = uitype == "MultiSpot" ? 0.2 : 0.0;
        c["vary_size"] = 0.5;
        c["vary_brightness"] = 0.5;
        c["vary_hue"] = 0.1;
        c["children"] = Json::array({model});
        generate(c);
        return c;
    }
    return e;
}

// ------------------------------------------------------------------ elements

static const Json *first_primitive(const Json &e, int depth = 0) {
    if (!e.is_object() || depth > 32)
        return nullptr;
    if (e.value("type", "") != "container")
        return &e;
    if (e.contains("children") && e["children"].is_array())
        for (const Json &c : e["children"])
            if (const Json *p = first_primitive(c, depth + 1))
                return p;
    return nullptr;
}

static std::string spaced(const std::string &camel) {
    std::string out;
    for (size_t i = 0; i < camel.size(); ++i) {
        if (i && std::isupper((unsigned char)camel[i]) && !std::isupper((unsigned char)camel[i - 1]))
            out += ' ';
        out += camel[i];
    }
    return out;
}

std::string kind_label(const Json &e) {
    if (e.contains("uitype") && e["uitype"].is_string()) {
        const std::string u = e["uitype"].get<std::string>();
        for (const ElementKind &k : element_kinds())
            if (u == k.uitype)
                return k.label;
        return spaced(u);
    }
    const std::string t = e.value("type", "");
    if (t == "container")
        return "Group";
    if (t == "blob")
        return "Blob";
    if (t == "rays")
        return "Rays";
    if (t == "texture")
        return "Texture";
    if (t == "polar_flare")
        return "Advanced Element";
    if (t == "ring")
        return "Ring";
    if (t == "hexagon")
        return "Polygon";
    if (t == "fancy_spot" && e.contains("n_sides") && e["n_sides"].is_number() &&
        e["n_sides"].get<double>() > 0)
        return "Polygon";
    return "Spot";
}

std::string engine_type(const Json &e) {
    const Json *p = is_group(e) ? &e : first_primitive(e);
    return p ? p->value("type", "") : e.value("type", "");
}

bool is_group(const Json &e) {
    if (e.value("type", "") != "container")
        return false;
    // a container of one kind (the lens files' uitype Spot, Ring, ...) shows as one element
    return !(e.contains("uitype") && e["uitype"].is_string() && e["uitype"].get<std::string>() != "Group");
}

bool has_gradient(const Json &e) {
    if (is_group(e))
        return false;
    const std::string t = engine_type(e);
    return t != "rays" && t != "blob" && t != "texture";
}

const Json *value(const Json &e, const std::string &name) {
    if (e.contains(name))
        return &e[name];
    const Json *p = first_primitive(e);
    if (p && p->contains(name))
        return &(*p)[name];
    return nullptr;
}

static void apply(Json &child, const std::string &name, const Json &old, const Json &now, Mode m,
                  int depth) {
    if (!child.is_object() || depth > 32)
        return;
    const Field *f = field(name);
    const double def = f ? f->def : 0.0;
    if (m != Mode::Overwrite && now.is_number() && old.is_number()) {
        const double o = old.get<double>(), n = now.get<double>();
        const double c = child.contains(name) && child[name].is_number() ? child[name].get<double>() : def;
        if (m == Mode::Offset)
            child[name] = c + (n - o);
        else
            child[name] = o != 0.0 ? c * (n / o) : n;
    } else if (m == Mode::Scale && now.is_array() && old.is_array() && now.size() == old.size()) {
        Json c = child.contains(name) && child[name].is_array() ? child[name] : old;
        for (size_t k = 0; k < now.size() && k < c.size(); ++k) {
            const double o = old[k].is_number() ? old[k].get<double>() : 0.0;
            const double n = now[k].is_number() ? now[k].get<double>() : 0.0;
            c[k] = o != 0.0 && c[k].is_number() ? c[k].get<double>() * (n / o) : n;
        }
        child[name] = c;
    } else {
        child[name] = now;
    }
    if (child.value("type", "") == "container" && child.contains("children") &&
        child["children"].is_array())
        for (Json &g : child["children"])
            apply(g, name, old, now, m, depth + 1);
}

void set_field(Json &e, const std::string &name, const Json &v) {
    if (e.value("type", "") == "container" && name != "name" && name != "disabled" &&
        e.contains("children") && e["children"].is_array() && !generator_field(name)) {
        const Json *was = value(e, name);
        const Field *f = field(name);
        Json old = was ? *was : f ? Json(f->def) : Json();
        if (f && f->kind == Kind::Color && !was)
            old = {1.0, 1.0, 1.0};
        const Mode m = mode(e, name);
        for (Json &c : e["children"])
            apply(c, name, old, v, m, 1);
    }
    e[name] = v;
}

// ------------------------------------------------------------------ Multi Spot / Streak

bool is_generator(const Json &e) {
    const std::string u = e.contains("uitype") && e["uitype"].is_string() ? e["uitype"].get<std::string>() : "";
    return e.value("type", "") == "container" && (u == "MultiSpot" || u == "MultiStreak");
}

bool generator_field(const std::string &name) {
    const Field *f = field(name);
    return f && !std::strcmp(f->types, GEN);
}

// A repeatable random number in [0, 1) for (seed, i, k).
static double hash01(int seed, int i, int k) {
    uint32_t h = (uint32_t)seed * 0x9E3779B1u ^ (uint32_t)(i + 1) * 0x85EBCA77u ^ (uint32_t)(k + 7) * 0xC2B2AE3Du;
    h ^= h >> 15;
    h *= 0x2C1B3C6Du;
    h ^= h >> 12;
    h *= 0x297A2D39u;
    h ^= h >> 15;
    return (h & 0xFFFFFF) / 16777216.0;
}

// Hue turned by `turn` (a fraction of a turn), the luminance kept roughly.
static void turn_hue(double rgb[3], double turn) {
    const double a = turn * 2.0 * M_PI, c = std::cos(a), s = std::sin(a);
    const double k = 1.0 / 3.0, q = std::sqrt(k);
    double m[3][3];
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            m[i][j] = (i == j ? c : 0.0) + k * (1.0 - c);
    m[0][1] -= q * s, m[0][2] += q * s;
    m[1][0] += q * s, m[1][2] -= q * s;
    m[2][0] -= q * s, m[2][1] += q * s;
    double out[3];
    for (int i = 0; i < 3; ++i)
        out[i] = std::fmax(0.0, m[i][0] * rgb[0] + m[i][1] * rgb[1] + m[i][2] * rgb[2]);
    for (int i = 0; i < 3; ++i)
        rgb[i] = out[i];
}

void generate(Json &e) {
    if (!e.contains("children") || !e["children"].is_array() || e["children"].empty())
        return;
    const Json model = e["children"][0];
    auto num = [&](const char *k, double d) {
        return e.contains(k) && e[k].is_number() ? e[k].get<double>() : d;
    };
    const int n = (int)std::fmax(1.0, std::fmin(64.0, num("number", 4)));
    const double spread = num("spread", 1), vspace = num("vary_spacing", 0), vsize = num("vary_size", 0);
    const double vbright = num("vary_brightness", 0), vhue = num("vary_hue", 0), vrot = num("vary_rotate", 0);
    const int seed = (int)num("rand_seed", 0);
    const bool streak = e.value("uitype", "") == "MultiStreak";
    // the container's own position, size, brightness, angle and colour (a group's change
    // reaches them), the rest from the first child
    const double center = num("center", 1.0), size = num("size", 1.0);
    const double bright = num("brightness", 1.0), rotate = num("rotate", 0.0);
    Json children = Json::array();
    for (int i = 0; i < n; ++i) {
        Json c = model;
        const double t = n > 1 ? (double)i / (n - 1) - 0.5 : 0.0;
        const double jitter = (hash01(seed, i, 0) - 0.5) * vspace / std::fmax(1, n - 1);
        if (streak) // streaks: side by side across the axis
            c["offset_off_axis"] = spread * (t + jitter) * 0.2;
        else
            c["center"] = center + spread * (t + jitter);
        c["size"] = size * std::fmax(0.05, 1.0 + vsize * (hash01(seed, i, 1) * 2 - 1));
        c["brightness"] = bright * std::fmax(0.0, 1.0 + vbright * (hash01(seed, i, 2) * 2 - 1));
        c["rotate"] = rotate + vrot * 180.0 * (hash01(seed, i, 3) * 2 - 1);
        double rgb[3] = {1, 1, 1};
        const char *key = c.contains("color") ? "color" : "tint";
        const Json &own = e.contains(key) && e[key].is_array() ? e[key] : c[key];
        if (own.is_array())
            for (int k = 0; k < 3 && k < (int)own.size(); ++k)
                rgb[k] = own[k].get<double>();
        turn_hue(rgb, vhue * (hash01(seed, i, 4) * 2 - 1) * 0.5);
        c[key] = {rgb[0], rgb[1], rgb[2]};
        children.push_back(c);
    }
    e["children"] = children;
}

} // namespace lens
