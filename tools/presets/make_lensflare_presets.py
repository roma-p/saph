# Writes MillSaph's own Lens Flare presets (presets/lensflare/*.json, tagged MillSaph): looks built from the
# element types the engine draws and MillSaph's generated textures.
import json, os, sys

# python3 tools/presets/make_lensflare_presets.py presets/lensflare
OUT = sys.argv[1]
os.makedirs(OUT, exist_ok=True)


def blob(center, size, bright, color, **k):
    return dict(type="blob", center=center, size=size, brightness=bright, colors=list(color), **k)


def spot(center, size, bright, inner, outer, **k):
    return dict(type="spot", center=center, size=size, brightness=bright, radii=[0, 0.75, 1],
                colors=list(inner) + list(inner) + list(outer), **k)


def ring(center, size, bright, color, width=0.12, **k):
    return dict(type="ring", center=center, size=size, brightness=bright,
                radii=[1 - width, 1 - width / 2, 1], colors=[0, 0, 0] + list(color) + [0, 0, 0], **k)


def poly(center, size, bright, color, sides=6, edge=None, **k):
    edge = edge or [c * 0.6 for c in color]
    return dict(type="fancy_spot", center=center, size=size, brightness=bright, n_sides=sides,
                roundness=0.15, blur_gradient=0.15, radii=[0, 0.8, 1],
                colors=list(color) + list(color) + list(edge), **k)


def rays(center, size, bright, number, width, color, **k):
    return dict(type="rays", center=center, size=size, brightness=bright, ray_number=number,
                ray_width=width, color=list(color), **k)


def streak(center, size, bright, color, thin=0.04, **k):
    # an anamorphic streak: a stretched blob flat on the screen
    return dict(type="blob", center=center, size=size, brightness=bright, colors=list(color),
                rel_width=4.0, rel_height=thin, **k)


def dirt(seed=1, bright=0.18, amount=0.35):
    return dict(type="blob", name="Lens Dirt", center=1, size=1.0, brightness=bright,
                colors=[1, 0.95, 0.9], image={"generate": "dirt", "seed": seed, "amount": amount},
                image_absolute_position_p=1, image_absolute_size_p=1)


def ghosts(start, step, count, size, bright, colors, kind="spot", **k):
    out = []
    for i in range(count):
        c = start + i * step
        col = colors[i % len(colors)]
        if kind == "poly":
            out.append(poly(c, size * (1 + 0.35 * (i % 3)), bright, col, **k))
        elif kind == "ring":
            out.append(ring(c, size * (1 + 0.5 * (i % 2)), bright, col, **k))
        else:
            out.append(spot(c, size * (1 + 0.3 * (i % 3)), bright, col, [x * 0.5 for x in col], **k))
    return out


P = {}

P["Classic Prime"] = [
    blob(1, 0.7, 0.3, (1, 0.85, 0.7), name="Glow"),
    blob(1, 0.25, 0.9, (1, 0.97, 0.92), name="Core"),
    rays(1, 0.9, 0.2, 12, 0.015, (1, 0.92, 0.85), name="Star"),
    *ghosts(0.55, -0.45, 5, 0.1, 0.07, [(0.6, 0.8, 1), (0.6, 1, 0.7), (1, 0.7, 0.5)]),
    ring(-1.5, 0.7, 0.03, (0.8, 0.9, 1), name="Halo"),
]

P["Anamorphic Blue"] = [
    blob(1, 0.6, 0.3, (0.7, 0.85, 1), name="Glow"),
    blob(1, 0.2, 1.0, (0.9, 0.95, 1), name="Core"),
    streak(1, 3.0, 0.5, (0.35, 0.6, 1), name="Streak"),
    streak(1, 1.5, 0.35, (0.6, 0.8, 1), thin=0.015, name="Thin streak"),
    *[dict(g, rel_width=1.6, rel_height=0.6) for g in ghosts(0.4, -0.35, 6, 0.09, 0.06, [(0.4, 0.6, 1), (0.5, 0.8, 1)], kind="ring")],
    blob(-1.2, 0.5, 0.05, (0.3, 0.5, 1), rel_width=2.5, rel_height=0.5, name="Wide ghost"),
]

P["Golden Sunset"] = [
    blob(1, 1.2, 0.3, (1, 0.6, 0.25), name="Warm haze"),
    blob(1, 0.35, 0.9, (1, 0.85, 0.6), name="Sun"),
    rays(1, 1.2, 0.12, 24, 0.01, (1, 0.75, 0.4), name="Rays"),
    *ghosts(0.6, -0.3, 6, 0.08, 0.06, [(1, 0.6, 0.3), (1, 0.8, 0.4), (0.9, 0.4, 0.2)], kind="poly", sides=6),
    ring(-0.9, 0.6, 0.04, (1, 0.6, 0.3), name="Amber ring"),
    dirt(seed=3, bright=0.12),
]

P["Zoom Lens Ghosts"] = [
    blob(1, 0.6, 0.28, (1, 0.95, 0.9), name="Glow"),
    blob(1, 0.18, 0.9, (1, 1, 1), name="Core"),
    rays(1, 0.7, 0.15, 8, 0.02, (1, 0.95, 0.9), name="Star"),
    *ghosts(0.7, -0.22, 10, 0.07, 0.05, [(0.5, 1, 0.6), (1, 0.5, 0.8), (0.5, 0.7, 1), (1, 0.9, 0.4)], kind="poly", sides=7),
    *ghosts(-1.4, -0.3, 3, 0.25, 0.025, [(0.6, 0.9, 1), (1, 0.8, 0.6)], kind="ring"),
]

P["Dirty Lens"] = [
    blob(1, 0.6, 0.3, (1, 0.92, 0.85), name="Glow"),
    blob(1, 0.2, 0.9, (1, 1, 1), name="Core"),
    dirt(seed=7, bright=0.4, amount=0.6),
    *ghosts(0.5, -0.5, 4, 0.12, 0.05, [(0.8, 0.9, 1), (1, 0.85, 0.7)]),
]

P["Rainbow Prism"] = [
    blob(1, 0.6, 0.28, (1, 1, 1), name="Glow"),
    blob(1, 0.2, 0.9, (1, 1, 1), name="Core"),
    rays(1, 1.3, 0.35, 16, 0.025, (1, 1, 1), name="Rainbow rays", image={"generate": "ramp"}),
    dict(type="texture", name="Prism glint", center=1.25, size=0.35, colors=[0.6, 0.6, 0.6],
         rotate_to_pivot=1, image={"generate": "glint", "seed": 2}),
    dict(type="texture", name="Prism glint 2", center=0.3, size=0.25, colors=[0.4, 0.4, 0.4],
         rotate_to_pivot=1, image={"generate": "glint", "seed": 5}),
    ring(-0.8, 0.5, 0.06, (1, 1, 1), name="Rainbow ring", width=0.2, image={"generate": "ramp", "amount": 1}),
]

P["Iris Bokeh"] = [
    blob(1, 0.55, 0.28, (1, 0.95, 0.9), name="Glow"),
    blob(1, 0.18, 0.9, (1, 1, 1), name="Core"),
    *[dict(type="fancy_spot", center=c, size=s, brightness=b, radii=[0, 1], colors=[1, 1, 1, 0.8, 0.85, 1],
           image={"generate": "iris", "sides": 7, "seed": i, "detail": 0.15, "amount": 0.3})
      for i, (c, s, b) in enumerate([(0.55, 0.16, 0.06), (0.2, 0.1, 0.05), (-0.4, 0.24, 0.04), (-0.9, 0.18, 0.045), (-1.4, 0.32, 0.025)])],
]

P["Sci-Fi Streak"] = [
    blob(1, 0.5, 0.35, (0.5, 0.8, 1), name="Glow"),
    blob(1, 0.15, 1.2, (0.9, 0.97, 1), name="Core"),
    streak(1, 5.0, 0.6, (0.3, 0.7, 1), thin=0.02, name="Long streak"),
    streak(1, 2.0, 0.4, (0.8, 0.9, 1), thin=0.008, name="Hot line"),
    rays(1, 0.6, 0.25, 4, 0.01, (0.7, 0.9, 1), name="Cross"),
    *[dict(g, rel_width=2.0, rel_height=0.5) for g in ghosts(0.3, -0.5, 4, 0.08, 0.05, [(0.3, 0.6, 1)], kind="ring")],
]

P["Subtle Natural"] = [
    blob(1, 0.5, 0.2, (1, 0.95, 0.88), name="Glow"),
    blob(1, 0.15, 0.6, (1, 0.98, 0.95), name="Core"),
    *ghosts(0.5, -0.4, 4, 0.08, 0.025, [(0.8, 0.9, 1), (0.9, 1, 0.85)]),
    ring(-1.2, 0.5, 0.015, (0.9, 0.95, 1), name="Faint halo"),
]

P["Starburst Spotlight"] = [
    blob(1, 0.7, 0.3, (1, 0.97, 0.9), name="Glow"),
    blob(1, 0.22, 1.0, (1, 1, 1), name="Core"),
    rays(1, 1.6, 0.3, 6, 0.02, (1, 0.97, 0.92), name="Main star"),
    rays(1, 1.0, 0.15, 36, 0.006, (1, 0.95, 0.9), name="Fine rays", radius_range=[0.4, 1], jitter_angles=0.5, rand_seed=3),
    ring(1, 0.6, 0.05, (1, 0.9, 0.8), name="Hot ring", width=0.06),
    *ghosts(0.4, -0.6, 3, 0.1, 0.04, [(0.9, 0.9, 1)]),
]

P["Vintage Warm"] = [
    blob(1, 1.0, 0.28, (1, 0.75, 0.5), name="Haze"),
    blob(1, 0.3, 0.8, (1, 0.9, 0.7), name="Core"),
    *ghosts(0.5, -0.35, 6, 0.11, 0.06, [(1, 0.6, 0.35), (0.9, 0.75, 0.4), (0.7, 0.5, 0.3)]),
    poly(-1.0, 0.3, 0.03, (1, 0.7, 0.4), sides=5, name="Pentagon ghost"),
    dirt(seed=11, bright=0.2, amount=0.45),
]

P["Polar Shimmer"] = [
    blob(1, 0.6, 0.28, (0.85, 0.9, 1), name="Glow"),
    blob(1, 0.2, 0.9, (1, 1, 1), name="Core"),
    dict(type="polar_flare", name="Shimmer", center=1, size=0.8, brightness=0.35, radii=[0, 0.5, 0.9],
         colors=[0.8, 0.9, 1, 0.4, 0.5, 0.8, 0, 0, 0], noise_brightness=1, noise_width=0.35,
         noise_softness=1, noise_freq=90, blur_theta_map=0.05, rand_seed=4),
    dict(type="polar_flare", name="Waves", center=1, size=0.5, brightness=0.3, radii=[0, 0.6, 1],
         colors=[1, 1, 1, 0.6, 0.7, 1, 0, 0, 0], wave_brightness=1, wave_freq=10, wave_width=0.4, wave_softness=0.6),
    *ghosts(0.45, -0.45, 4, 0.1, 0.05, [(0.6, 0.8, 1), (0.8, 0.6, 1)], kind="poly", sides=8),
]

# each a preset file (src/common/preset.h): its lens as the Lens text, tagged MillSaph (the
# Lens Flare's Preset menu lists these, CMakeLists.txt)
for name, elements in P.items():
    lens = {"value": {"metadata": {"preset-name": name, "author": "MillSaph"}, "elements": elements}}
    preset = {"name": name, "tags": ["MillSaph"], "params": {"lens_": json.dumps(lens, indent=1)}}
    fn = name.lower().replace(" ", "_").replace("-", "_") + ".json"
    with open(os.path.join(OUT, fn), "w") as f:
        json.dump(preset, f, indent=1)
        f.write("\n")
print(len(P), "presets")
