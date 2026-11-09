# Reads and writes uncompressed scanline OpenEXR files (what Nuke's Write node writes with
# compression "none", and tools/perf/images.c), with numpy only: no OpenEXR module needed.
#
#   import exr
#   image = exr.read("a.exr")              # float32 array [height, width, channels], top row
#                                          # first; channels in exr.channels("a.exr") order
#   exr.write("b.exr", image, "RGBA")      # float32 (or half=True), one letter per channel
#
# RGBA files come back as R, G, B, A whatever their order in the file. Pixels outside the data
# window are not read: the array covers the data window only (exr.window() gives it).
import struct
import numpy as np

TYPES = {0: np.uint32, 1: np.float16, 2: np.float32}  # EXR pixel types
SIZES = {0: 4, 1: 2, 2: 4}


def _header(f):
    if f.read(4) != b"\x76\x2f\x31\x01":
        raise ValueError("not an OpenEXR file")
    version = struct.unpack("<i", f.read(4))[0]
    if version & 0x1a00:  # tiled, deep or multipart (long names are fine)
        raise ValueError("tiled, deep or multipart EXR: write it as plain scanlines")
    attrs = {}
    while True:
        name = b""
        while True:
            c = f.read(1)
            if c == b"\0":
                break
            name += c
        if not name:
            return attrs
        kind = b""
        while True:
            c = f.read(1)
            if c == b"\0":
                break
            kind += c
        size = struct.unpack("<i", f.read(4))[0]
        attrs[name.decode()] = (kind.decode(), f.read(size))


def _parse(attrs):
    kind, data = attrs["channels"]
    chans, i = [], 0
    while data[i:i + 1] != b"\0":
        end = data.index(b"\0", i)
        name = data[i:end].decode()
        ptype = struct.unpack("<i", data[end + 1:end + 5])[0]
        chans.append((name, ptype))
        i = end + 1 + 16
    comp = attrs["compression"][1][0]
    if comp != 0:
        raise ValueError("compressed EXR (type %d): write it with compression none" % comp)
    x0, y0, x1, y1 = struct.unpack("<4i", attrs["dataWindow"][1])
    return chans, (x0, y0, x1, y1)


def channels(path):
    with open(path, "rb") as f:
        return [c for c, _ in _parse(_header(f))[0]]


def window(path):
    with open(path, "rb") as f:
        return _parse(_header(f))[1]


def read(path):
    with open(path, "rb") as f:
        chans, (x0, y0, x1, y1) = _parse(_header(f))
        w, h = x1 - x0 + 1, y1 - y0 + 1
        offsets = struct.unpack("<%dQ" % h, f.read(8 * h))
        out = np.zeros((h, w, len(chans)), np.float32)
        for offset in offsets:
            f.seek(offset)
            y, size = struct.unpack("<2i", f.read(8))
            line = f.read(size)
            at = 0
            for c, (_, ptype) in enumerate(chans):
                n = w * SIZES[ptype]
                out[y - y0, :, c] = np.frombuffer(line[at:at + n], TYPES[ptype]).astype(np.float32)
                at += n
    names = [c for c, _ in chans]
    if sorted(names) == sorted("RGBA") or sorted(names) == sorted("RGB"):
        order = [names.index(c) for c in "RGBA" if c in names]
        out = out[:, :, order]
    return out


def write(path, image, names="RGBA", half=False):
    image = np.asarray(image, np.float32)
    if image.ndim == 2:
        image = image[:, :, None]
    h, w, n = image.shape
    if len(names) != n:
        raise ValueError("%d channels, %d names" % (n, len(names)))
    order = sorted(range(n), key=lambda c: names[c])  # EXR channels are sorted by name
    ptype, dtype = (1, np.float16) if half else (2, np.float32)

    def attr(name, kind, data):
        return name.encode() + b"\0" + kind.encode() + b"\0" + struct.pack("<i", len(data)) + data

    chlist = b"".join(names[c].encode() + b"\0" + struct.pack("<i4x2i", ptype, 1, 1)
                      for c in order) + b"\0"
    box = struct.pack("<4i", 0, 0, w - 1, h - 1)
    header = (b"\x76\x2f\x31\x01" + struct.pack("<i", 2) +
              attr("channels", "chlist", chlist) +
              attr("compression", "compression", b"\0") +
              attr("dataWindow", "box2i", box) + attr("displayWindow", "box2i", box) +
              attr("lineOrder", "lineOrder", b"\0") +
              attr("pixelAspectRatio", "float", struct.pack("<f", 1)) +
              attr("screenWindowCenter", "v2f", struct.pack("<2f", 0, 0)) +
              attr("screenWindowWidth", "float", struct.pack("<f", 1)) + b"\0")
    line_size = w * n * (2 if half else 4)
    start = len(header) + 8 * h
    offsets = struct.pack("<%dQ" % h, *[start + y * (8 + line_size) for y in range(h)])
    with open(path, "wb") as f:
        f.write(header + offsets)
        for y in range(h):
            f.write(struct.pack("<2i", y, line_size))
            for c in order:
                f.write(image[y, :, c].astype(dtype).tobytes())
