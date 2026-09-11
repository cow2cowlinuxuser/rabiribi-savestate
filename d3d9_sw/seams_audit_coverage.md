# Seams audit: triangle coverage

Adversarial audit of one narrow question: **which pixels does a triangle claim.**
Pixel-centre convention, top-left fill rule, subpixel snapping, edge-function
precision, winding/culling, and viewport/scissor only insofar as they move
coverage. Texture sampling, filtering, addressing and blending are deliberately
out of scope and are covered by a separate audit.

Files audited: `swrast.c`, `swrast.h`, and the vertex/viewport/state path in
`d3d9_sw.c`. `savestate.*`, `trace.c`, `tramp.c` ignored.

---

## Conventions as implemented

These are statements of fact about the current code, with citations. This section
is durable reference material; it should not need re-deriving.

### Coverage pipeline, end to end

There is exactly **one** path from a draw call to coverage. `d3d9_sw.c:2578`
(`swrast_triangles`) is the sole producer, and `swrast.c:1365` (`triangle_rect`
from `run_tile`) is the sole consumer. There is no second/fast blit rasteriser to
diverge from. Stages, in order:

1. Vertex load. `load_xyzrhw` (`d3d9_sw.c:1551`) copies `x,y,z,rhw` verbatim for
   `D3DFVF_XYZRHW`; `load_decl` for declarations.
2. Optional transform. Only if a vertex shader ran (`d3d9_sw.c:2368`), or if the
   FVF/decl is *not* pre-transformed **and** a heuristic says the coordinates do
   not already look like pixels (`d3d9_sw.c:2392`).
3. Viewport map (`d3d9_sw.c:1732`), applied only on those transformed paths.
4. Subpixel snap (`swrast.c:1708-1710`), applied unconditionally to every
   triangle, as the very last stage before binning.
5. Bin to 64x64 tiles from the snapped bounding box (`swrast.c:1711-1770`).
6. Per-tile setup and scan (`swrast.c:886-1176`).

### Pixel centre convention

**Pixel centres are at `(x + 0.5, y + 0.5)` and vertex positions are consumed
raw, with no half-pixel adjustment anywhere.** Edge functions are seeded at
`(minx + 0.5f, miny + 0.5f)` (`swrast.c:927-929`) and stepped by whole pixels
(`swrast.c:1100-1102`, `swrast.c:1169-1171`).

The only `0.5` constants in the coverage path are those pixel centres
(`swrast.c:927-929`), the snap rounding term (`swrast.c:77-78`), and the viewport
map's NDC-to-screen scale (`d3d9_sw.c:1738-1739`). The `-= 0.5f` at
`swrast.c:184-185` and `swrast.c:595-596,632-633` are texel-centre corrections
inside the sampler, not position adjustments. A full grep of `d3d9_sw.c` for any
`x`/`y` add-assign or subtract-assign of a half finds nothing.

**This is correct, and the premise that a `-0.5` is owed here is a misreading of
the D3D9 rule.** D3D9 rasterises in a continuous screen space in which integer
coordinate `n` is the *boundary* between pixel `n-1` and pixel `n`, and samples
coverage at `(i+0.5, j+0.5)`. The famous `-0.5` is what *application* code
applies to its own vertex positions to line texel centres up with pixel centres;
it is not applied by the runtime or the driver. A reimplementation that inserted
a `-0.5` would shift every quad half a pixel and would itself manufacture seams.
See Verified Correct #1.

### Top-left fill rule

The rule exists, its edge classification is correct for all four orientations,
and it is implemented by an epsilon.

Winding is first normalised so signed area is positive (`swrast.c:895-900`), so
edge orientation is determined purely by the edge-function gradients. An edge is
classified **inclusive** when `dwdx > 0`, or `dwdx == 0 && dwdy > 0`; every other
edge is **exclusive** (`swrast.c:358-363`). The gradients are
`dwdx = -(Qy - Py)`, `dwdy = (Qx - Px)` for edge `P -> Q` (`swrast.c:920-925`).

Working the four cases for a clockwise-on-screen (area > 0, y-down) triangle:

| Edge | Traversal | `dwdx` | `dwdy` | Classified |
|---|---|---|---|---|
| top | left to right | `0` | `> 0` | inclusive |
| bottom | right to left | `0` | `< 0` | exclusive |
| left | bottom to top | `> 0` | — | inclusive |
| right | top to bottom | `< 0` | — | exclusive |

Top and left inclusive, bottom and right exclusive. That is the D3D9 rule, and
it is correct for the quad-internal diagonal too: the diagonal is traversed in
opposite directions by the quad's two triangles, so exactly one of them sees it
as inclusive.

The implementation is a one-sided **erosion**: exclusive edges have
`1.0e-4f * (|dwdx| + |dwdy|) + 1.0e-30f` subtracted from the row seed
(`swrast.c:362`, applied at `swrast.c:938-940`), and the per-pixel test is
`w0 >= 0 && w1 >= 0 && w2 >= 0` (`swrast.c:1107`, vector twin at
`swrast.c:776-779`). Inclusive edges are tested against exact zero, unbiased.
The bias magnitude works out to roughly `1e-4` of a pixel of perpendicular
distance, since `w` is (distance x edge length) and the bias scales with edge
length.

**Consequence, and it is the crux of this audit: the rule is only watertight when
the edge function returns *exactly* `0.0f` on a pixel centre that lies on the
shared edge.** If it returns a small non-zero of either sign, the inclusive side
may reject (negative) while the exclusive side also rejects (positive but below
the bias) — nobody claims the pixel. See Finding 1.

### Subpixel snapping

`D3D9SW_SUBPIXEL` (`swrast.c:30-43`) defaults to **4 fractional bits**, clamped
to `0..8`; `0` disables snapping. `snap_vert` (`swrast.c:73-79`) does
`floorf(v->x * grid + 0.5f) / grid` on x and y only — round-half-up to the
nearest 1/16. `grid` is a power of two so the divide is exact and the result is
an exact multiple of 1/16, exactly representable in float32 for any realistic
screen coordinate.

Snapping is applied at the correct place: **after** every transform, in
`swrast_triangles` before the bounding box and before the copy into the bin list
(`swrast.c:1705-1710`), so the binner and every tile that touches the triangle
see one identical coordinate set. It is a pure function of the input coordinate,
so two adjacent quads that are handed byte-identical shared-edge coordinates
snap to byte-identical results. See Verified Correct #3.

Snapping guarantees an edge is either *exactly* on a pixel centre or at least
1/16 px away from it. That is a real robustness win, and it means the residual
failure mode is narrow — but it is also, perversely, what makes the exact-zero
case *common* rather than rare, and the exact-zero case is precisely the one the
epsilon fill rule cannot handle safely. See Finding 1.

### Winding and culling

Screen space is y-down, so a visually clockwise front-facing triangle has
positive signed area under `orient2d` (`swrast.c:81-84`). `D3DCULL_CCW` discards
`area < 0`, `D3DCULL_CW` discards `area > 0`, `D3DCULL_NONE` discards nothing
(`swrast.c:891-894`). Zero-area triangles are dropped (`swrast.c:887`).

This matches D3D9, whose default `D3DCULL_CCW` culls counter-clockwise
screen-space triangles. Default state is set to `D3DCULL_CCW` (`d3d9_sw.c:5803`)
and is honoured from `D3DRS_CULLMODE` unless `D3D9SW_NOCULL` is set
(`d3d9_sw.c:1968-1974`, `d3d9_sw.c:2031`).

Winding *inconsistency between a quad's two triangles cannot affect the fill
rule*, because area is normalised to positive before any gradient is computed
(`swrast.c:895-900`). Strip parity is also handled correctly: `tri_ix`
(`d3d9_sw.c:1762-1773`) swaps the first two indices on odd triangles, matching
D3D9's strip winding alternation. See Verified Correct #4.

### Viewport and scissor

Viewport map is the textbook D3D9 transform with no offsets
(`d3d9_sw.c:1738-1739`). Scissor is copied from `D3DRS_SCISSORTESTENABLE` and the
device rect (`d3d9_sw.c:2035-2041`) and applied as a half-open `[x0, x1)` range,
both in the binner (`swrast.c:1721-1730`) and again in `triangle_rect`
(`swrast.c:868-877`). Tile bounds arrive as `clip_x0..clip_x1` from `run_tile`
(`swrast.c:1353-1366`) and only ever narrow the walk; the per-pixel edge test is
unchanged by them, so tiling cannot double-cover or drop a pixel by itself.

---

## Findings

Ordered by likelihood of producing a visible seam.

### Finding 1 — The fill rule erodes the exclusive edge instead of using exact arithmetic, so a pixel centre exactly on a shared edge can be claimed by neither triangle

**Severity: Critical**

**Location:** `swrast.c:358-363` (`edge_bias`), applied `swrast.c:938-940`, tested
`swrast.c:1107` and `swrast.c:776-779`.

**The D3D9 rule.** A pixel whose centre lies exactly on an edge shared by two
triangles must be covered by exactly one of them — never zero, never two. Real
hardware achieves this by snapping vertices to a fixed-point grid and then
evaluating the edge functions in **exact integer arithmetic**, where "on the
edge" is a bit-exact `== 0` and the top-left tie-break is a clean, total
decision. Exactness is not an optimisation detail; it is the entire mechanism.

**What the code does.** It snaps (good, `swrast.c:1708-1710`) and then throws the
benefit away by evaluating the edge functions in `float`. The tie-break becomes:
inclusive side accepts `w >= 0`; exclusive side accepts `w >= bias` where
`bias ~ 1e-4` px of distance. The two acceptance regions do not tile the plane.
Let the true perpendicular distance be exactly zero and let each side's float
evaluation carry independent rounding error `e`:

- Inclusive side computes `w = -|e|` → **rejects**.
- Exclusive side computes `w = +|e|`; if `|e| < bias` → **rejects**.
- Neither triangle writes the pixel. **Gap.**

The mirror case is just as available: if the exclusive side's error exceeds the
bias in the positive direction while the inclusive side also lands non-negative,
**both** write the pixel — under `D3DBLEND_SRCALPHA/D3DBLEND_ONE` additive, which
this game uses heavily (`swrast.c:967-968`), a double-blended pixel is a bright
line and is far more visible than a gap.

**Why this fires constantly rather than occasionally.** The quad diagonal is the
worst case, and 2D sprite geometry hands it to us on a plate. From the project's
own draw log, `d3d9_sw.log:78` records a 16x16 quad at
`(914,313)(930,313)(914,329)` — a square quad on integer coordinates. Its
diagonal has slope exactly 1, so it passes through the pixel centre of *every*
pixel along it: `(914.5, 313.5)`, `(915.5, 314.5)`, and so on. Every one of those
pixels is an exact-zero tie. The same holds for any square quad and, at every
other pixel, for any 2:1 quad. Square power-of-two sprite quads are the dominant
primitive in this game. So the diagonal of a typical sprite is a *contiguous run*
of tie pixels, and the fill rule's behaviour on them is decided entirely by
whether the float edge function happens to return exact zero.

**Where it actually breaks.** For small integer-coordinate quads like the log
entry above the arithmetic happens to be exact (all operands and products are
small integers or halves, well inside float32's 24-bit integer range), both sides
return exactly `0.0f`, and the rule *does* award the pixel to exactly one
triangle. That is why this is not visible on every sprite. It stops being exact
as soon as either of the following holds, and then the diagonal tie run flips to
gap-or-double:

- Coordinates are snapped-but-not-integer — any rotated, scaled, or sub-pixel
  scrolled quad. Then operands are multiples of 1/16 and the edge-function
  products need up to ~30 mantissa bits; float32 has 24. Rounding of order
  `0.5` in `w` units appears, against a bias of order `1e-4 * edge_length`
  (0.05 for a 512-px edge). The error is an order of magnitude larger than the
  bias, so the rule is simply not in control.
- Large quads (backgrounds). Even on integer coordinates, once coordinate
  products approach 2^24 the differences stop being exact.

**Visual artefact.** A one-pixel line along the diagonal of sprite and background
quads, running corner to corner. Dark (a gap showing whatever was underneath)
under alpha-over, bright (double-blended) under additive. Because the sign of the
rounding is a property of the coordinates, a given quad's diagonal tends to be
uniformly wrong along its whole length rather than dotted — a clean diagonal
scratch across the sprite. The same mechanism produces vertical/horizontal lines
between neighbouring quads whenever a shared axis-aligned edge lands on a pixel
centre, which happens exactly when the game places geometry on half-integer
coordinates.

**Fix, in prose.** Stop rasterising in float. The snap already puts every vertex
on an exact 1/16 grid, so convert the snapped x/y to integers in subpixel units
and evaluate the three edge functions in 64-bit integer arithmetic. With
coordinates bounded by a few thousand pixels the products fit comfortably and the
result is exact, so "on the edge" is a bit-exact `== 0`. Then implement the
top-left rule as a true tie-break rather than an epsilon: accept `w > 0`
unconditionally, and accept `w == 0` if and only if the edge is classified
top-or-left by the existing (correct) gradient-sign test. Delete `edge_bias`
entirely. The interpolation of u/v/z/colour can stay in float — only the coverage
decision needs to be exact. This is a structural change to setup and to the
per-pixel/per-lane test, and it is the change that actually closes the class of
bug rather than retuning the epsilon.

---

### Finding 2 — Edge functions are evaluated from per-triangle origins with reversed operand order, so the same geometric edge yields different values from each side

**Severity: Critical** (it is the mechanism that supplies the error `e` in
Finding 1; listed separately because it is separately fixable and separately
wrong)

**Location:** `swrast.c:81-84` (`orient2d`), seeded `swrast.c:927-929`, x-rebased
`swrast.c:1071-1074`, per-pixel `swrast.c:1100-1102`, per-row accumulated
`swrast.c:1169-1171`.

**The D3D9 rule.** An edge shared by two triangles must evaluate to the same
value (up to sign) from both sides. Hardware guarantees this by evaluating in
exact arithmetic, which makes evaluation order irrelevant. Any float
implementation that wants to be watertight must at minimum be *bit-exactly
antisymmetric* under swapping the edge's two endpoints, and *independent of the
triangle's bounding box*.

**What the code does.** Three separate order dependencies, all of which break
that:

1. **Reversed operand order is not exactly antisymmetric.**
   `orient2d(a,b,c) = (bx-ax)*(cy-ay) - (by-ay)*(cx-ax)` (`swrast.c:83`). Swap
   `a` and `b` and the products become `(ax-bx)*(cy-by)` and `(ay-by)*(cx-bx)` —
   *different operands*, hence different rounding. Mathematically the negation;
   numerically not. This is exactly what happens on a quad diagonal: for the
   triangle `(TL,TR,BR)` the diagonal is edge `BR -> TL` and is evaluated as
   `orient2d(BR, TL, p)` (`swrast.c:928`), while for `(TL,BR,BL)` the same
   diagonal is edge `TL -> BR`, evaluated as `orient2d(TL, BR, p)`
   (`swrast.c:929`). Two different expressions for one geometric edge.

2. **The row seed depends on the triangle's own clipped bounding box.**
   `w*_row` is seeded at `(minx + 0.5f, miny + 0.5f)` (`swrast.c:927-929`) where
   `minx`/`miny` come from that triangle's bbox clamped to the tile
   (`swrast.c:907-916`). Two quads sharing a vertical edge have different
   `minx`, so the shared edge's function is evaluated from different origins and
   rounds differently. The same triangle even gets different origins in
   different tiles, so a single triangle's own edge can shift by a pixel at a
   64-px tile boundary.

3. **Values are accumulated, not recomputed.** Down the triangle,
   `w*_row += dw*dy` once per row (`swrast.c:1169-1171`) — up to 64 roundings per
   tile, drifting. Across the span, `w = dwdx * (x - xs) + w_xs` where
   `w_xs = w_row + dwdx * (xs - minx)` (`swrast.c:1071-1074`, `swrast.c:1100-1102`),
   so the value at a given pixel depends on `xs`, the row's clipped span start —
   which differs between the two triangles of a quad, since they occupy
   complementary halves of it. The comment at `swrast.c:1092-1095` correctly
   identifies that accumulating along the span would be worse and rebases from
   the span start instead, but rebasing from a *per-triangle, per-row* origin
   just trades drift for asymmetry, which for shared-edge coverage is the more
   damaging of the two.

**Visual artefact.** Supplies the error that turns Finding 1's exact-zero ties
into gaps or double-hits: diagonal scratches inside quads (mechanism 1), lines
along quad-to-quad joins (mechanism 2), and one-pixel notches at 64-px tile
boundaries along an otherwise clean edge (mechanism 2, same-triangle case).

**Fix, in prose.** Subsumed by Finding 1's fix — exact integer edge functions are
order-independent, so all three mechanisms vanish. If exact arithmetic is
rejected, the partial mitigation is to canonicalise each edge before evaluation
(sort its two endpoints by a total order on the coordinate pair, evaluate once in
that canonical order, and negate for the triangle that needs the other sign) and
to seed edge functions from a fixed global origin such as `(0.5, 0.5)` rather
than from the triangle's bbox corner, recomputing `w` from that origin per row
rather than accumulating. That makes evaluation a pure function of the edge and
the pixel, which restores the antisymmetry the fill rule depends on — but it
still leaves the epsilon of Finding 1 in place and is therefore a partial fix
only.

---

### Finding 3 — Whether the world-view-projection transform is applied at all is decided per draw call by a heuristic on the first triangle

**Severity: Major**, conditional on the game issuing non-pre-transformed draws
without a vertex shader

**Location:** `d3d9_sw.c:2392`, heuristic at `d3d9_sw.c:1750-1760`, helper at
`d3d9_sw.c:1744-1748`.

**The D3D9 rule.** Whether the fixed-function transform pipeline runs is
determined solely by the vertex format: `D3DFVF_XYZRHW` / `D3DDECLUSAGE_POSITIONT`
bypasses it, `D3DFVF_XYZ` does not. It is never inferred from coordinate values.

**What the code does.** For a draw that is neither shader-transformed nor
declared pre-transformed, it applies the WVP matrix **only if**
`!tri_looks_pixel(&batch[0], ...)` — that is, only if the first triangle's
coordinates do not happen to look like screen pixels: max absolute coordinate
`> 2.5` and all three vertices within `[-8, dim+8]` (`d3d9_sw.c:1750-1760`). Two
failure shapes follow:

- The decision is taken from `batch[0]` alone and applied to the entire batch. A
  batch containing a mix, or two batches that together form one continuous
  background surface but whose first triangles classify differently, get
  different treatment — one transformed, one not — and their shared edge is no
  longer shared at all.
- The guard band is only 8 pixels (`d3d9_sw.c:1746-1747`). A pre-transformed-
  looking quad that legitimately extends more than 8 px off-screen — routine for
  a scrolling background — fails `vert_in_px`, so the whole draw gets a WVP
  transform applied to what are already screen coordinates.

**Visual artefact.** Not a subtle one-pixel seam: a whole-quad displacement,
appearing as a hard discontinuity at draw-call boundaries in an otherwise
continuous surface, or geometry vanishing. Where it fires on only some quads of a
tiled background, the result reads as a gross crack.

**Fix, in prose.** Key the decision entirely off the declared vertex format, as
D3D9 does: `D3DFVF_XYZRHW` and `POSITIONT` bypass the transform, everything else
goes through it, with no inspection of coordinate values. If a value-based
fallback must be retained for a specific misbehaving title, it should be gated
behind an explicit opt-in, evaluated over every vertex in the batch rather than
one triangle, and applied consistently for the lifetime of the frame rather than
re-decided per draw.

---

### Finding 4 — The scalar and vector coverage tests are not guaranteed to agree, and adjacent quads can take different paths

**Severity: Minor** (Speculative on the current build; see Open Question 3)

**Location:** scalar `swrast.c:1100-1102`, vector `swrast.c:772-774`, path
selection `swrast.c:972-976`.

**The D3D9 rule.** Coverage must be a function of geometry alone. It must not
depend on which internal code path happened to be selected for a triangle.

**What the code does.** The scalar path computes `dw0dx * kf + w0_xs` as C float
expressions; the vector path computes the same thing as an explicit
`_mm256_mul_ps` followed by `_mm256_add_ps` (`swrast.c:772`). Under
`-ffp-contract=on`, which is clang's default and the build uses plain `-O2` with
no float flags (`build.ps1:5`), the scalar expression is *permitted* to contract
to a fused multiply-add, which rounds once instead of twice. The vector path
cannot. The comment at `swrast.c:1092-1095` asserting that "the two paths agree
exactly" is an assertion, not a guarantee.

This matters because path selection is per triangle and depends on state that can
differ between neighbouring quads: `use_simd` requires textured, flat vertex
colour, no depth test, no draw-id buffer, full write mask, over/add/no blend, and
a **power-of-two** texture with WRAP addressing (`swrast.c:972-976`,
`swrast.c:475-484`). A quad sampling a non-power-of-two atlas takes the scalar
path while its neighbour takes the vector path, and their shared edge is then
evaluated by two different roundings — feeding Finding 1 again.

Severity is Minor rather than Major only because the build targets baseline
x86-64 and 32-bit x86 (`build.ps1:10`, `build.ps1:16`) with no `-mfma`, so FMA is
probably not actually emitted, in which case the two paths do agree today. That
is a property of the current compiler invocation, not of the code.

**Visual artefact.** If contraction occurs: intermittent one-pixel seams along
edges shared between quads that differ in texture dimensions or blend mode, with
no obvious geometric pattern — the hardest variant to diagnose.

**Fix, in prose.** Make the guarantee explicit rather than assumed. Compile the
rasteriser with contraction disabled, or compute the edge functions through a
single shared helper whose rounding behaviour is pinned. Exact integer edge
functions (Finding 1) make the whole question moot, since integer arithmetic has
no contraction hazard and the scalar and vector forms are then bit-identical by
construction.

---

### Finding 5 — The fill-rule bias contaminates attribute interpolation

**Severity: Minor** (correctness nit; magnitude is below visibility)

**Location:** `swrast.c:938-940` mutate `w0_row`/`w1_row`/`w2_row` in place, and
`PLANE_AT` at `swrast.c:1016` then uses those same biased values to seed u, v, z
and 1/w (`swrast.c:1025-1028`).

The bias is a coverage-only tie-break construct and has no business entering
attribute interpolation. It shifts every interpolant by roughly `1e-4` of a pixel
of barycentric offset. **This is not visible** and I am recording it only so it is
not mistaken for a cause later: it will not produce a seam. It disappears
naturally when `edge_bias` is deleted per Finding 1. Keeping separate biased and
unbiased row values would be the fix if the epsilon were retained.

---

## Verified correct

Checked and found right. This ground does not need re-investigating.

1. **No spurious half-pixel offset.** Pixel centres are sampled at `+0.5`
   (`swrast.c:927-929`) and vertex positions are used raw. There is no `-0.5`
   applied to positions anywhere in the coverage path, and there should not be —
   the `-0.5` is an application-side texel-alignment convention, not a runtime
   transform. Confirmed by exhaustive grep of both files for half-pixel
   constants and for position add/subtract-assigns. The viewport transform
   (`d3d9_sw.c:1738-1739`) is the plain D3D9 NDC-to-screen map with no offset,
   which is also correct.

2. **Top-left edge classification is correct for every orientation.** The
   gradient-sign test at `swrast.c:360` yields top-inclusive, left-inclusive,
   bottom-exclusive, right-exclusive for the area-normalised winding, verified by
   working all four cases (table above). It is correct for the quad-internal
   diagonal as well as for axis-aligned quad-to-quad joins. The *classification*
   is right; only its epsilon *implementation* is wrong (Finding 1).

3. **Snapping is applied at the right stage and is order-independent.**
   `snap_vert` is called on all three vertices after every transform and before
   the bounding box and the bin-list copy (`swrast.c:1705-1710`), so binner and
   all tiles see one coordinate set. It is a pure function of the input
   coordinate, so identical shared-edge inputs snap identically — an edge shared
   by two quads does land on identical coordinates from both sides, provided the
   game supplies identical coordinates. `grid` is a power of two
   (`swrast.c:40`), so `/grid` is exact and the result is an exactly
   representable multiple of 1/16. There is no per-triangle transform after
   snapping. **The specific concern that snapping happens after a per-triangle
   transform and can crack a shared edge does not apply to this code.**

4. **Winding and culling match D3D9, and quad-internal winding inconsistency
   cannot affect the fill rule.** `D3DCULL_CCW` discards `area < 0` in y-down
   screen space, which is D3D9's convention (`swrast.c:889-894`); default state
   is `D3DCULL_CCW` (`d3d9_sw.c:5803`). Area is normalised positive before any
   gradient is computed (`swrast.c:895-900`), so the fill rule is immune to which
   way round the source data winds each triangle. Strip parity is handled
   correctly at `d3d9_sw.c:1762-1773`. Zero-area triangles are dropped
   (`swrast.c:887`), matching hardware.

5. **Scissor is half-open and applied consistently.** `[x0, x1) x [y0, y1)` in
   both the binner (`swrast.c:1721-1730`) and `triangle_rect`
   (`swrast.c:868-877`), matching `D3DRS_SCISSORTESTENABLE` semantics and the
   device rect as copied at `d3d9_sw.c:2035-2041`. Scissor narrowing can only
   shrink the walk; it never alters the edge test, so it cannot itself drop or
   duplicate an interior pixel.

6. **Tiling does not affect coverage.** `run_tile` (`swrast.c:1348-1367`) passes
   tile bounds as clip rectangles only. The per-pixel edge test
   (`swrast.c:1107`) is unchanged by them, so no pixel is covered twice or
   missed because of tile decomposition. Submission order is preserved within a
   tile and each tile is owned by one thread, so blending order is
   deterministic. (The tile origin *does* perturb edge-function rounding — that
   is Finding 2 mechanism 2, a precision issue, not a tiling-logic issue.)

7. **The bounding box is conservative and cannot clip a covered pixel.**
   `floorf` on the minima and `ceilf` on the maxima (`swrast.c:907-916`,
   `swrast.c:1717-1720`), and the tile range is derived as
   `x0 >> 6 .. (x1-1) >> 6` (`swrast.c:1766-1768`), which is correct for the
   half-open `[x0, x1)` box. Worked the boundary cases (`fx1` exactly on a tile
   multiple, `fx1` just past one, `fx0` mid-pixel): no covered pixel is excluded
   and no tile is missed.

8. **`span_clip`'s row narrowing is safe.** It solves each edge for the row's x
   range (`swrast.c:369-397`) and can only tighten `xs` upward and `xe`
   downward; its empty marker (`xs = 1, xe = 0`) survives subsequent calls
   because those only tighten further. The caller then widens by one pixel each
   way (`swrast.c:1063-1068`) to absorb rounding in the divides, and the exact
   per-pixel edge test remains the authority (`swrast.c:1107`). This
   optimisation does not change coverage. It also correctly consumes the
   *biased* row values, so it is consistent with the fill rule.

9. **The vector kernel's tail handling is correct.** `span_avx2`
   (`swrast.c:769-834`) masks lanes beyond `xe` with `tail` (`swrast.c:771`) and
   combines it into `live` before any load or store, and all stores are masked
   (`swrast.c:830-833`). No coverage leaks past the span or the tile.

10. **Single funnel.** `swrast_triangles` is called from exactly one place
    (`d3d9_sw.c:2578`) and `triangle_rect` from exactly one place
    (`swrast.c:1365`). There is no alternate sprite/blit rasteriser applying a
    different convention, so the conventions documented above are global.

---

## Open questions

Each with the specific experiment that settles it.

1. **Does the game actually place quads on integer, half-integer, or arbitrary
   coordinates?** This decides whether Finding 1 fires on every sprite diagonal
   or only on transformed geometry. The draw log at `d3d9_sw.log:78` shows
   `(914.0, 313.0)`-type values, but it is printed with `%.1f`
   (`d3d9_sw.c:2450`) so integer-looking values may be `914.03`, and the entry
   may be from the synthetic test rather than the game.
   **Experiment:** enable the existing per-triangle draw log
   (`d3d9_sw.c:2432-2472`, `g_draw_log`) on a real gameplay frame, with the
   format widened to `%.6f`, and histogram the fractional parts of x and y over
   a frame. Integer or half-integer clustering means the exact-zero tie case is
   the common case.

2. **On a real frame, are the diagonal ties resolving to gaps, to double-hits, or
   correctly?** The sign of the float error is not determinable by reading.
   **Experiment:** the draw-id owner map already exists for exactly this
   (`swrast.c:45-71`, `swrast.c:1881` dump, armed via `D3D9SW_DRAWID`). Capture a
   frame, and additionally instrument a debug counter that, per triangle, counts
   pixels where any `|w|` is within one bias of zero and records whether the
   pixel was written. Zero writes on a tie run is a gap; two writes from the same
   draw is a double-hit. Alternatively, render a single square quad with additive
   blend and constant white at `0x40` alpha and inspect the diagonal: a brighter
   diagonal is a double-hit, a darker one a gap.

3. **Does the compiler actually contract the scalar edge computation to FMA?**
   Settles whether Finding 4 is live or dormant.
   **Experiment:** disassemble `triangle_rect` in `d3d9_sw.pdb` / the shipped
   `d3d9_sw.dll` and look for `vfmadd*ss` in the span loop around the `w0/w1/w2`
   computation. No FMA instruction means the two paths agree on this build.
   Separately confirm the 32-bit build uses SSE2 scalar float rather than x87 —
   x87's 80-bit intermediates would make the scalar path's rounding differ from
   the vector path's unconditionally, and would also silently change the
   exactness arguments in Finding 1.

4. **Is `D3D9SW_SUBPIXEL` actually left at its default of 4 in the deployed
   configuration?** If it is ever set to 0, snapping is disabled entirely
   (`swrast.c:40`), the guarantee that edges are either exactly on or at least
   1/16 px from a pixel centre is lost, and Finding 1 degrades from "narrow but
   systematic" to "everywhere".
   **Experiment:** dump the environment as seen by the game process
   (`envdump.ps1` already enumerates this variable at line 26) at DLL load, and
   log the resolved grid value once from `subpixel_grid`.

5. **Does the game ever issue non-pre-transformed draws without a vertex
   shader?** This decides whether Finding 3 is reachable at all.
   **Experiment:** the draw log already prints `vs=%d` (`d3d9_sw.c:2460`); add
   the resolved `transformed` flag (`d3d9_sw.c:2390-2391`) and the
   `tri_looks_pixel` result to the same line, then check whether any gameplay
   draw reaches the `d3d9_sw.c:2392` branch, and whether the heuristic's answer
   is ever inconsistent between draws within a frame.

6. **Do adjacent background quads share byte-identical vertex coordinates in the
   vertex buffer?** Verified Correct #3 (snapping is order-independent) only
   guarantees a watertight shared edge if the game supplies the same float from
   both sides. If the background is built by accumulating a per-quad stride in
   float, the two sides can differ by more than half a subpixel step and snap to
   *different* grid values, which is a genuine geometric crack that no fill rule
   or exact edge function can close.
   **Experiment:** for a background draw, dump the raw pre-snap x/y of every
   vertex at full precision and check that each interior edge coordinate appears
   with a bit-identical value from both neighbouring quads.
