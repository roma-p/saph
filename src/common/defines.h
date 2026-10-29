#pragma once

// only work with compile time know size arrays
#define ARRAY_SIZE(array) ((int)(sizeof(array) / sizeof((array)[0])))
