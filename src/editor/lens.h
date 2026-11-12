// MillSaph's lens editor: the lens (its JSON, fields and order kept) and what the editor
// knows of its elements' fields.

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
    // its elements' array ({"value": {"elements": ...}} or {"elements": ...})
    Json &elements();
};

bool parse(const std::string &text, Lens &out, std::string &err);
std::string write(const Lens &lens);

// How a field is edited.
enum class Kind { Number, Integer, Bool, Text, Choice, Color, Pair };

struct Field {
    const char *name, *label, *group;
    Kind kind;
    double def, min, max; // Number / Integer: the slider's range
    const char *choices;  // Choice: "a|b|c"
    const char *types;    // the element types it is for ("" : all), "|"-separated
};

// The fields the engine reads, by group, in the order the editor shows them.
const std::vector<Field> &fields();
const Field *field(const std::string &name);
bool field_for(const Field &f, const std::string &type);

// A new element of `type` with the fields it needs.
Json new_element(const std::string &type);

// The element types the editor adds.
const std::vector<const char *> &element_types();

} // namespace lens
