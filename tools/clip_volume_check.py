#!/usr/bin/env python3
# Where do draws 286/287/288 put their geometry, in the GUEST'S OWN numbers?
#
# Layout under test (it is a hypothesis, and the run prints what would falsify
# it): the vertex shader's c0..c2 are the world matrix' rotation rows, c3 its
# translation, and c7..c10 are the view-projection's four rows -- shared by all
# three draws, which is why only the world matrix can separate them.
#
# THE NEGATIVE THIS MUST BE ABLE TO PRINT: if the layout is wrong, draw 288 --
# the one that demonstrably rasterises 87 primitives and 90716 fragments -- will
# NOT come out inside the clip volume. That is the check, and it is printed
# every run whether it passes or fails.

verts = [  # the first four vertices of 0xe585000, stride 11, shared by all three draws
    (1169.7983, -1672.5906, 2168.2236),
    (1477.5428, -1969.5702, 1895.9917),
    (1278.5767, -2561.8410, 1212.4653),
    (1084.0103, -2590.8418, 1263.0806),
]

# view-projection rows, identical in all six window draws
c7  = (-1.1917515, 0.0, -0.0019157454, -0.001917663)
c8  = (-0.0022853818, 0.0, 0.99899817, 0.99999815)
c9  = (0.0, 2.118673, 0.0, 0.0)
c10 = (123.781944, -557.5288, -535.76154, -526.29785)

draws = {
    286: dict(c0=(-2.0773509, -2.5312598, 0.0), c1=(2.5312598, -2.0773509, 0.0),
              c2=(0.0, 0.0, 3.274548), c3=(15416.857, -2809.7197, -32.219723)),
    287: dict(c0=(0.32096183, -3.2587802, 0.0), c1=(3.2587802, 0.32096183, 0.0),
              c2=(0.0, 0.0, 3.274548), c3=(26035.129, 10463.118, -3559.9998)),
    288: dict(c0=(0.32037368, 2.9828444, 0.0), c1=(-2.9828444, 0.32037368, 0.0),
              c2=(0.0, 0.0, 3.0), c3=(-16564.238, 19323.756, -128.0)),
}
# What the renderer measured for each, so the arithmetic is checked against the
# GPU rather than against itself.
observed = {286: "0 primitives after clip",
            287: "0 primitives after clip",
            288: "87 primitives after clip, 90716 fragments"}

def world(v, d):
    return tuple(v[0]*d['c0'][i] + v[1]*d['c1'][i] + v[2]*d['c2'][i] + d['c3'][i]
                 for i in range(3))

def clip(w):
    return tuple(w[0]*c7[i] + w[1]*c8[i] + w[2]*c9[i] + c10[i] for i in range(4))

for n, d in draws.items():
    print(f"draw {n}: renderer says {observed[n]}")
    inside = 0
    for v in verts:
        w = world(v, d)
        c = clip(w)
        ok = c[3] > 0 and all(abs(c[i]) <= c[3] for i in range(3))
        inside += ok
        ndc = tuple(c[i]/c[3] for i in range(3)) if c[3] != 0 else None
        where = ("BEHIND THE CAMERA (w<0)" if c[3] <= 0 else
                 f"ndc=({ndc[0]:+.3f}, {ndc[1]:+.3f}, {ndc[2]:+.3f})")
        print(f"  world=({w[0]:10.1f},{w[1]:10.1f},{w[2]:9.1f})  w={c[3]:10.1f}  {where}"
              f"  {'inside' if ok else 'OUTSIDE'}")
    print(f"  -> {inside} of {len(verts)} dumped vertices inside the clip volume\n")

# The layout is only worth anything if it agrees with the GPU on the draw that
# DID rasterise. Say so explicitly rather than leaving the reader to notice.
c288 = [clip(world(v, draws[288])) for v in verts]
if any(c[3] > 0 and all(abs(c[i]) <= c[3] for i in range(3)) for c in c288):
    print("LAYOUT CHECK PASSES: draw 288, which the GPU rasterised, comes out inside\n"
          "the clip volume under this layout -- so the same arithmetic putting 286 and\n"
          "287 outside is evidence, not an artefact of guessing the constant order.")
else:
    print("LAYOUT CHECK FAILS: draw 288 rasterised 87 primitives on the GPU but this\n"
          "layout puts every one of its vertices outside the clip volume. The constant\n"
          "order above is WRONG and NOTHING on this page may be used as evidence.")
