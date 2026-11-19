// MS_TVDamage's settings as the kernels get them: one row of TV_FIELDS floats, filled by
// effects/ms_tvdamage.c and read by src/halide/tv_damage_generator.cpp (more settings than a
// kernel takes arguments). Lengths in pixels of the render, times in frames. Shared by C and
// C++.

#pragma once

enum {
    TV_TIME,  // the frame
    TV_SEED,  // the Seed (a whole number)
    TV_MONO,  // 1: TV Mono
    TV_WIDTH, // the frame's width in pixels (what the sizes scale with)
    // static
    TV_STATIC_AMP,   // master x Static Amplitude
    TV_STATIC_SHARE, // the share of pixels the static touches
    TV_STATIC_BOOST, // the touched pixels' gain
    // interference dots
    TV_DOTS_AMP,   // master x Interference Amp
    TV_DOTS_SHARE, // the share of cells with a dot
    TV_DOTS_W,     // a dot's cell, in pixels
    TV_DOTS_H,
    TV_DOTS_X, // the dots' move at this frame, in pixels
    TV_DOTS_Y,
    TV_DOTS_JITTER, // each row's random move, at most, in pixels
    // ghosts
    TV_GHOSTS,         // how many (0: none)
    TV_GHOST_WEIGHT,   // one ghost's weight
    TV_GHOST_STEP,     // the distance between ghosts, in pixels
    TV_GHOST_VARY,     // Vary Position
    TV_GHOST_NEGATIVE, // the share of negative ghosts
    TV_GHOST_BLUR1,    // Blur: each ghost's weight 1 and 2 pixels to the sides (the rest in the
    TV_GHOST_BLUR2,    // middle)
    // horizontal hold
    TV_HHOLD_AMP,     // the rows' largest move, in pixels (0: none)
    TV_HHOLD_FREQ,    // noise cycles over the height
    TV_HHOLD_Z,       // the noise's time
    TV_HHOLD_OCTAVES, // whole number
    TV_HHOLD_BORDER,  // the black border at the wrap, in pixels
    // vertical hold
    TV_VHOLD_ON,     // 1: rolling
    TV_VHOLD_OFFSET, // the roll at this frame, in rows
    TV_VHOLD_BAR,    // the black bar's height, in rows
    TV_VHOLD_DATA,   // Border Data
    // fast forward
    TV_FF_AMP,    // Fast Forward Amount (0: none)
    TV_FF_BANDS,  // bands over the height
    TV_FF_SHIFT,  // FF Band Shift
    TV_FF_HEIGHT, // FF Band Height
    TV_FF_PHASE,  // the bands' move at this frame (in bands)
    // dropouts
    TV_DROP_AMP,    // the streaks' brightness (0: none)
    TV_DROP_CHANCE, // the share of frames with streaks
    TV_DROP_LEN,  // a streak's and a gap's length, in pixels
    TV_DROP_GAP,
    TV_DROP_FREQ, // rows: noise cycles over the height
    TV_DROP_THRESHOLD,
    // orthicon
    TV_ORTH_AMP, // Orthicon x its gain (0: none)
    TV_ORTH_THRESHOLD,
    // colour stripes
    TV_STRIPES_AMP, // master x amplitude
    TV_STRIPES_FX,  // cycles per pixel along x and y
    TV_STRIPES_FY,
    TV_STRIPES_PHASE, // at the centre, this frame (cycles)
    // colour correction
    TV_HUE,
    TV_SATURATION,
    TV_LIGHTS_R, // Scale Lights x Tint Lights
    TV_LIGHTS_G,
    TV_LIGHTS_B,
    TV_DARKS_R, // Offset Darks + Tint Darks
    TV_DARKS_G,
    TV_DARKS_B,
    // bars
    TV_BARS_AMP,    // master x Bars Brightness
    TV_BARS_PERIOD, // rows
    TV_BARS_FREQ,   // Bar Frequency
    TV_BARS_PHASE1, // the bars' roll this frame (periods)
    TV_BARS_PHASE2,
    TV_BARS_WIDTH, // Bar1 Width
    TV_BARS_SHARP1,
    TV_BARS_REL2, // Bar2 Rel Frequency
    TV_BARS_SHARP2,
    // scanlines, vignette
    TV_SCAN_AMP,
    TV_SCAN_FREQ, // cycles per pixel
    TV_VIG_AMP,
    TV_VIG_RADIUS,
    TV_VIG_SOFT,
    TV_VIG_REL_H,
    // the picture's end
    TV_CELLS,      // 1: TV Pixels (the cells' image)
    TV_FISH,       // Fish Eye
    TV_OFF,        // Turn Off (0: none)
    TV_OFF_FADE,   // where the fading line starts (1 - Fade Out Time)
    TV_OFF_WIDTH,  // the flare's width, in pixels
    TV_OFF_BRIGHT, // Flare Brightness
    TV_FIELDS
};
