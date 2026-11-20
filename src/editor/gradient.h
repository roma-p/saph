// MillSaph's lens editor: an element's gradient (its "radii" and "colors": a colour per radius,
// 0 at its center, 1 at its edge) edited on a bar, with the saved gradients.

#pragma once

#include <string>

#include "lens.h"

// The gradient bar of e: click to add a stop, double-click to change its colour, drag to move
// it, drag it down (or Delete) to remove it; then the saved gradients and the selected stop's
// location. `selected`: the selected stop (kept between frames). True when e changed. `hint`:
// set to the bar's help while the mouse is over it.
bool gradient_edit(lens::Json &e, int &selected, std::string &hint);

// The hint shown while the mouse is over the bar.
extern const char *const GRADIENT_HINT;
