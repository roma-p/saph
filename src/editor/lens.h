// MillSaph's lens editor: the lens (its JSON, fields and order kept) and what the editor
// knows of its elements: their kinds (the palette's element types), their fields in property
// groups, and how a group's change reaches the elements under it.

#pragma once

#include <string>
#include <vector>

#include "nlohmann/json.hpp"

namespace lens {

using Json = nlohmann::ordered_json;

// A lens's text: optional characters before its "{" (kept), then the JSON.
struct Lens {
    std::string prefix;
    Json doc;
    // the lens's root object (the one under "value" when there is one)
    Json &root();
    // its elements' array, and its shadow's (the lens shadow element, one at most)
    Json &elements();
    Json &shadow();
};

bool parse(const std::string &text, Lens &out, std::string &err);
std::string write(const Lens &lens);

// How a field is edited. Flag: a 0/1 number shown as a two-item menu (choices "off|on").
// Shape: fancy_spot's n_sides shown as Circle / Polygon.
enum class Kind { Number, Integer, Bool, Text, Choice, Color, Pair, Flag, Shape };

// How a container's change reaches its children (the lens files' param_modes).
enum class Mode { Scale, Offset, Overwrite };

struct Field {
    const char *name, *label, *group;
    Kind kind;
    double def, min, max; // Number / Integer: the slider's range
    const char *choices;  // Choice / Flag: "a|b|c"
    const char *types;    // the engine types it is for ("": all), "|"-separated
    const char *grey;     // the engine types it is shown greyed for (no effect), or null
};

// The property groups, in the editor's order ("Options" is shown "<kind> Options").
const std::vector<const char *> &groups();

// The fields the engine reads, by group, in the order the editor shows them.
const std::vector<Field> &fields();
const Field *field(const std::string &name);
bool field_for(const Field &f, const std::string &type);
bool field_grey(const Field &f, const std::string &type);
Mode mode(const Json &container, const std::string &name);

// An element kind: what the palette adds, its "uitype" in the lens and its label.
struct ElementKind {
    const char *uitype, *label;
};
const std::vector<ElementKind> &element_kinds();

// A new element of a kind (its "uitype"); "Group": an empty group.
Json new_element(const std::string &uitype, int number);

// The element's kind label ("Blob", "Spot", "Group", ...), its engine type (for a container
// holding one kind: its first primitive's), and whether it shows as one row (a primitive, or
// a container of one kind) rather than a group.
std::string kind_label(const Json &e);
std::string engine_type(const Json &e);
bool is_group(const Json &e);
bool has_gradient(const Json &e);

// The element's value of a field: its own, else its first primitive's, else null.
const Json *value(const Json &e, const std::string &name);

// A field's value when e leaves it out: its default; 0 for a group's offsets.
double neutral(const Json &e, const std::string &name);

// Sets a field on e and, for a container, on the elements under it by the field's mode:
// scaled by new / old, offset by new - old, or overwritten.
void set_field(Json &e, const std::string &name, const Json &v);

// Multi Spot / Multi Streak: their children made again from the container's Number,
// Spread and Vary fields (the first child the model).
bool is_generator(const Json &e);
bool generator_field(const std::string &name);
void generate(Json &e);

} // namespace lens
