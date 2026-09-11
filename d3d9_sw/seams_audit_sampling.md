# Sampling & Attribute Interpolation Audit

Adversarial correctness audit of texture sampling and attribute interpolation in
the `d3d9_sw` software rasterizer, against DirectX 9.0c behaviour.

**Scope.** Texel centre convention, bilinear filtering, addressing modes, point
rounding, attribute interpolation basis, the 1:1 sequential-load fast path, and
vector/scalar agreement. Triangle coverage, the fill rule, pixel-centre
conventions, subpixel snapping and winding are explicitly **out of scope** and
are covered by a separate audit.

Files audited: `swrast.c`, `swrast.h`, `test_simd.c`, and the sampler/texture
state plumbing in `d3d9_sw.c`. `savestate.*`, `trace.c`, `tramp.c` not examined.

---

## 1. Conventions as implemented

### 1.1 Texel centre and the u,v → texel mapping

The sampler receives normalised `u,v` and scales by texture size
(`swrast.c:180-181`):

```180:185:F:\rbo_fabre_proto\d3d9_sw\swrast.c
	fu = u * (float)t->width;
	fv = v * (float)t->height;
	if (!bilinear)
		return tex_fetch(t, (int)floorf(fu), (int)floorf(fv), au, av);
	fu -= 0.5f;
	fv -= 0.5f;
```

- **Point sampling**: texel index is `floor(u * width)`, with no half-texel
  adjustment (`swrast.c:183`). This is the correct D3D9 rule — a texel centre
  sits at `(i + 0.5)/size`, and the point-sampled texel for any `u` in
  `[i/size, (i+1)/size)` is `i`.
- **Bilinear**: the half-texel offset is subtracted *before* the floor
  (`swrast.c:184-185`), so `x0 = floor(u*width - 0.5)` and the interpolation
  runs between texel centres. This is the correct D3D9 rule.

The vector kernel uses the identical construction: point at `swrast.c:629-630`,
bilinear offset at `swrast.c:632-633` (gather path) and `swrast.c:595-596`
(sequential-load path).

### 1.2 Filter weights

Fractional weight is quantised to 8 bits by truncation (`swrast.c:189-190`):

```187:195:F:\rbo_fabre_proto\d3d9_sw\swrast.c
		int x0 = (int)floorf(fu);
		int y0 = (int)floorf(fv);
		uint32_t tx = (uint32_t)(int)((fu - (float)x0) * 256.0f);
		uint32_t ty = (uint32_t)(int)((fv - (float)y0) * 256.0f);
		uint32_t c00 = tex_fetch(t, x0, y0, au, av);
		uint32_t c10 = tex_fetch(t, x0 + 1, y0, au, av);
		uint32_t c01 = tex_fetch(t, x0, y0 + 1, au, av);
		uint32_t c11 = tex_fetch(t, x0 + 1, y0 + 1, au, av);
		return lerp8_fixed(lerp8_fixed(c00, c10, tx), lerp8_fixed(c01, c11, tx), ty);
```

The blend is a two-stage separable lerp in 8.8 fixed point with round-to-nearest
(`+0x00800080` before `>>8`, `swrast.c:161-168`). `t` ranges 0..255 out of a
notional 256, so the second texel never receives full weight. The vector twin is
`span_lerp8` (`swrast.c:546-551`), arithmetically identical, and the weight is
produced by `_mm256_cvttps_epi32` (truncate) at `swrast.c:640-643` and
`swrast.c:605-608`, matching the scalar C cast.

Filtering order is U-first then V in both paths — irrelevant to the result here
because the arithmetic is exact enough to be separable-symmetric within the
rounding already applied.

### 1.3 Addressing

Addressing is applied to the **integer texel index**, after the half-texel
offset and after the floor (`swrast.c:150-155`, called from
`swrast.c:191-194`). This is the correct order: clamping the normalised
coordinate before subtracting the half texel would collapse the outermost half
texel and produce a one-pixel artefact at every quad edge. The code gets this
right, and `swrast.c:174-175` documents the intent.

`addr_coord` (`swrast.c:124-148`) implements WRAP (mask fast path for
power-of-two at `swrast.c:130-131`, general modulo at `swrast.c:146`), CLAMP,
MIRROR, MIRRORONCE, and maps BORDER onto CLAMP. Unknown/zero modes fall through
to WRAP, which matches the D3D9 default of `D3DTADDRESS_WRAP`.

The vector kernel supports **WRAP only**; `span_kernel_ok` (`swrast.c:475-484`)
rejects the SIMD path for any other mode and for non-power-of-two sizes, and
wrapping is then done by masking (`swrast.c:497-500`, `swrast.c:556-557`).

### 1.4 Interpolation basis

Interpolants are plane equations built from the barycentric gradients
(`swrast.c:1014-1028`). Perspective correction is enabled only when all three
`rhw` are positive and not all equal (`swrast.c:904-905`); when enabled, `u,v`
are premultiplied by `rhw` (`swrast.c:1000-1005`) and divided by the
interpolated `1/w` per pixel (`swrast.c:1112-1115` scalar, `swrast.c:789-798`
vector). This is the correct D3D9 treatment of `D3DFVF_XYZRHW`.

**Along a span**, `u,v` are *recomputed* from the span base rather than
accumulated (`swrast.c:1103-1104`: `uu = dudx * kf + u_xs`), and the vector
kernel uses the identical formula against the identical base
(`swrast.c:770`, `swrast.c:787-788`). **Down the frame**, however, the row base
*is* accumulated (`swrast.c:1172-1175`). See finding F6.

Texture memory is tightly packed — `SwTex` carries no pitch (`swrast.h:21-25`)
and the allocation is `calloc(width*height, 4)` (`d3d9_sw.c:3884`), so the
implied `pitch == width` is sound.

---

## 2. Findings

Ordered by likelihood of producing a visible seam.

### F1 — Default texture filter is LINEAR; D3D9's default is POINT — **Critical**

`d3d9_sw.c:5789-5790`

```5789:5792:F:\rbo_fabre_proto\d3d9_sw\d3d9_sw.c
	dev->samp_min = D3DTEXF_LINEAR;
	dev->samp_mag = D3DTEXF_LINEAR;
	dev->samp_addru = D3DTADDRESS_WRAP;
	dev->samp_addrv = D3DTADDRESS_WRAP;
```

**D3D9 rule.** The documented default for `D3DSAMP_MAGFILTER` and
`D3DSAMP_MINFILTER` is `D3DTEXF_POINT`. A title that never calls
`SetSamplerState` for these types gets unfiltered point sampling on real D3D9.
(The `D3DTADDRESS_WRAP` defaults on the two following lines are correct.)

**What the code does.** Initialises both filters to `D3DTEXF_LINEAR`, so any
draw issued before — or without — an explicit filter set is bilinear filtered.

**Artefact.** Bilinear on an atlas fetches `x0` and `x0+1` with `x0` derived
from `u*width - 0.5`. At the first and last texel column/row of an atlas *tile*
this reaches into the adjacent tile's texels, because the addressing unit knows
nothing about tile sub-rects — it only wraps at the full 1024-wide atlas
boundary. The result is a one-pixel fringe of the neighbouring sprite's colour
along every tile edge, and softened, doubled edges on 1:1 sprite blits. This is
precisely the "half-texel bleed and seams at atlas tile boundaries" symptom.
Note that this is *correct* hardware behaviour once bilinear is selected — the
bug is selecting bilinear when D3D9 would have selected point.

**Fix in prose.** Initialise `samp_min` and `samp_mag` to `D3DTEXF_POINT` to
match the D3D9 default, and let the game's own `SetSamplerState` calls raise it
to linear where it actually asks for linear. Keep `D3D9SW_FILTER` as the
diagnostic override it already is.

---

### F2 — `norm_pixel_uv_tri` rewrites UVs per-triangle with no half-texel term — **Critical**

`d3d9_sw.c:2154-2182`, invoked unconditionally for every textured draw at
`d3d9_sw.c:2428-2431` (the `D3D9SW_PIXELUV` env flag defaults to **1**,
`d3d9_sw.c:2008-2014`).

```2172:2181:F:\rbo_fabre_proto\d3d9_sw\d3d9_sw.c
	if (maxu <= 2.0f && minu >= -2.0f && maxv <= 2.0f && minv >= -2.0f)
		return;
	if (minu < -1.0f || minv < -1.0f)
		return;
	if (maxu > (float)tw + 1.0f || maxv > (float)th + 1.0f)
		return;
	for (k = 0; k < 3; k++) {
		v[k]->u /= (float)tw;
		v[k]->v /= (float)th;
	}
```

Two independent defects, both seam-producing.

**(a) The half-texel term is missing.** The transform maps a texel-unit
coordinate `i` to `u = i / tw`. Under the D3D9 texel-centre convention the
centre of texel `i` is at `(i + 0.5) / tw`. So every rescaled draw samples half
a texel up and to the left of where it should. Under point sampling this is
harmless (`floor(i)` is still `i`) but leaves the sample sitting *exactly* on a
texel boundary, where any ULP-level perturbation flips the chosen texel — see
F6, which this finding directly enables. Under bilinear (which F1 makes the
default) it is not harmless at all: `fu - 0.5 = i - 0.5`, giving `x0 = i-1` and
a weight of 128/256, so **every output pixel is a 50/50 average of two adjacent
texels**. Sprites become uniformly soft and every sprite edge is doubled into
the neighbouring atlas tile.

**(b) The rescale decision is made per-triangle, from that triangle's own UV
range.** Whether the divide happens depends on thresholds evaluated against
`minu/maxu/minv/maxv` of one triangle. Adjacent quads tiling one surface have
*different* UV sub-ranges, so they can be classified differently:

- A quad whose texel-space extent happens to fall entirely inside `[-2, 2]`
  (a sprite two texels wide, or any quad near the atlas origin) hits the early
  return at line 2172 and is **not** rescaled, while its neighbour spanning
  texels 0..64 **is**. The un-rescaled quad then interprets texel units as
  normalised coordinates and samples the entire atlas across two pixels.
- A quad that overhangs slightly — `maxu > tw + 1.0` at line 2176, easy for a
  scrolling background — is not rescaled while its neighbours are.

The two triangles *within* a single quad do share a UV range (both triangles of
a corner-fan quad span the full u and v extent), so this does not split a quad
down its diagonal; it splits at **quad-to-quad joins**, which is exactly where
the reported seams are.

**Artefact.** (a) uniform half-texel softening and doubled sprite edges;
(b) hard discontinuities where two adjacent quads of one background were
classified differently — one renders correctly, its neighbour renders garbage or
a wildly wrong region of the atlas.

**Fix in prose.** The classification must be made once per draw call over the
whole vertex batch, not per triangle, so that every quad of one surface is
treated identically; and the mapping must be `(i + 0.5) / size`, not `i / size`,
to land on texel centres. Better still, decide from the bound pixel shader or
the declared texture dimensions rather than from a magnitude heuristic on the
coordinates. Given the risk this heuristic carries, flipping the `D3D9SW_PIXELUV`
default to off and confirming against a draw log whether any draw genuinely
supplies texel-unit UVs would be the safer posture.

---

### F3 — Filter selection ORs MIN and MAG instead of choosing by scale — **Major**

`d3d9_sw.c:2193-2195`

```2193:2195:F:\rbo_fabre_proto\d3d9_sw\d3d9_sw.c
	int bilinear = filter_override() >= 0
			       ? filter_override()
			       : (filter_is_smooth(d->samp_mag) || filter_is_smooth(d->samp_min));
```

**D3D9 rule.** `D3DSAMP_MAGFILTER` governs magnification (texel:pixel ratio
below 1) and `D3DSAMP_MINFILTER` governs minification; they are selected
independently per pixel and never combined.

**What the code does.** Takes the logical OR, so `MAGFILTER = POINT` with
`MINFILTER = LINEAR` — a completely ordinary setting for a 2D title that wants
crisp 1:1 sprites but smooth downscaling — yields bilinear on **everything**,
including the magnified 1:1 sprites where point was requested.

**Artefact.** Same bleed and edge doubling as F1, and it survives the F1 fix,
because it re-derives bilinear from a `MINFILTER` the game legitimately set to
linear.

**Fix in prose.** Compare the interpolated texel-space gradient against the
pixel step to decide magnification versus minification — for the axis-aligned
sprite quads this game submits, `|dudx| * width` versus 1 is a sufficient test,
and it is already computed at `swrast.c:1017`. Select `samp_mag` when
magnifying and `samp_min` when minifying. A cheaper interim would be to honour
`samp_mag` alone, since essentially every draw in this title magnifies or is 1:1.

---

### F4 — Sequential-load fast path derives the base texel from lane 0 only — **Major**

`swrast.c:589-604`

```589:604:F:\rbo_fabre_proto\d3d9_sw\swrast.c
	if (s->seq_u && !s->persp) {
		if (!s->bilinear) {
			int x0 = _mm256_extract_epi32(_mm256_cvtps_epi32(_mm256_floor_ps(fu)), 0);
			int y0 = _mm256_extract_epi32(_mm256_cvtps_epi32(_mm256_floor_ps(fv)), 0);
			return span_loadu_wrap(s, x0, y0);
		}
		fu = _mm256_sub_ps(fu, _mm256_set1_ps(0.5f));
		fv = _mm256_sub_ps(fv, _mm256_set1_ps(0.5f));
		{
			__m256 ffu = _mm256_floor_ps(fu), ffv = _mm256_floor_ps(fv);
			int x0 = _mm256_extract_epi32(_mm256_cvtps_epi32(ffu), 0);
			int y0 = _mm256_extract_epi32(_mm256_cvtps_epi32(ffv), 0);
			__m256i c00 = span_loadu_wrap(s, x0, y0);
```

**D3D9 rule.** Every pixel samples at its own interpolated coordinate. There is
no notion of a group of eight pixels sharing a base texel.

**What the code does.** Computes `floor(fu)` and `floor(fv)` for all eight
lanes, then discards seven of them and keeps lane 0. `span_loadu_wrap` then
loads eight *consecutive* texels from row `y0`, i.e. it asserts
`floor(fu_k) == x0 + k` and `floor(fv_k) == y0` for `k = 0..7`. Those assertions
are only exactly true when `dudx*width` is exactly 1.0 **and** `dvdx` is exactly
0 **and** the float evaluation of `(u + du*k) * tw` reproduces `fu_0 + k`
exactly. The qualifying predicate guarantees none of the three (see F5), and the
third never holds in general: `fu_k` is computed as a fresh
multiply-add per lane at `swrast.c:787`, not as an increment of `fu_0`, so its
rounding is independent.

The bilinear branch makes this worse rather than better: the interpolation
weights `tx`, `ty` at `swrast.c:605-608` **are** computed per lane, from the
true per-lane `fu`, while the four texels they weight come from the lane-0
base. So a lane whose true floor differs from `x0 + k` gets a correct weight
applied to the wrong pair of texels — a full-texel error, not a rounding error.

**Artefact.** A one-texel-wide column of wrong texels, snapped to the 8-pixel
vector grid, appearing only on spans that took the fast path — i.e. only on 1:1
axis-aligned background blits, and only near where a group boundary coincides
with a texel boundary. Because the general gather path (`swrast.c:628-665`)
computes per-lane indices correctly, the two paths disagree, and the disagreement
shows up as a discontinuity where a fast-path quad abuts a general-path quad.

**Fix in prose.** Either tighten the predicate to a bit-exact test so the
consecutive-lane assertion is provable (see F5), or verify it at run time:
compare the vector of per-lane `floor(fu)` against `x0 + lane` and the per-lane
`floor(fv)` against `y0`, and fall back to the gather path for that group when
they disagree. The comparison is two vector compares and a `testz`, negligible
against the load it protects.

---

### F5 — The fast-path predicate is a tolerance, not an equality — **Major**

`swrast.c:1041-1043`

```1041:1043:F:\rbo_fabre_proto\d3d9_sw\swrast.c
		simd_span.seq_u = !persp &&
				  fabsf(dvdx * simd_span.th_f) < 1.0e-4f &&
				  fabsf(dudx * simd_span.tw_f - 1.0f) < 1.0e-4f;
```

**D3D9 rule.** N/A directly — this is an internal invariant. But the fast path
it gates (F4) is only *correct* under exact equality, so the predicate must
establish exact equality.

**What the code does.** Admits any span where the texel step is within 1e-4 of
one texel per pixel and the V step is within 1e-4 of zero. Over the eight lanes
of one vector group that permits up to 8e-4 texel of drift in U, and 8e-4 texel
of drift in V, relative to the lane-0-derived base that F4 assumes. A texel
boundary falling inside a group then produces a wrong fetch. Across a
640-pixel span, the *accumulated* discrepancy between the true coordinate and
the "1:1 from lane 0" model reaches 0.064 texel — bounded per group, so it does
not run away, but easily enough to straddle a boundary in some groups and not
others.

Worse: a genuinely non-1:1 blit, for instance a 1024-wide texture stretched
across 1024.1 pixels, satisfies the predicate and is then rendered as if it were
exactly 1:1. That is a systematic sampling error across the whole quad, not a
rounding artefact.

**Artefact.** Sporadic 8-pixel-quantised stair-stepping in the texel column
selected along a nearly-1:1 background span, and wholesale loss of sub-texel
scale on near-1:1 quads. Adjacent quads that fall on opposite sides of the
tolerance render with different sampling rules and meet at a visible seam.

**Fix in prose.** Require exact equality on the reconstructed values —
`dudx * tw_f == 1.0f` and `dvdx * th_f == 0.0f` as float comparisons — so that
the consecutive-lane assumption is a theorem rather than an approximation. Spans
that miss by a ULP simply take the gather path, which costs throughput on a
minority of spans and removes an entire artefact class. Combine with the run-time
verification in F4 for defence in depth.

---

### F6 — Row-base interpolants are accumulated down the frame — **Major**

`swrast.c:1172-1175`, against the span-base recomputation at `swrast.c:1103-1104`

```1168:1175:F:\rbo_fabre_proto\d3d9_sw\swrast.c
	next_row:
		w0_row += dw0dy;
		w1_row += dw1dy;
		w2_row += dw2dy;
		u_row += dudy;
		v_row += dvdy;
		z_row += dzdy;
		iw_row += diwdy;
```

**D3D9 rule.** Each pixel's attributes are the plane equation evaluated at that
pixel. Two triangles that share an edge and carry consistent attributes must
produce consistent attribute values on both sides of the edge.

**What the code does.** Within a span, the implementation is careful:
`swrast.c:1092-1095` explicitly documents recomputation-from-base to avoid
drift, and `swrast.c:1103` honours it. Down the frame it does the opposite — the
row base is accumulated, so `u_row` at scanline `y` carries `y - miny` rounding
steps. `miny` is the triangle's own bounding-box top (`swrast.c:914`), so **two
adjacent quads whose bounding boxes start at different scanlines arrive at the
same scanline with different accumulation histories** and therefore slightly
different `u_row`, even when their plane equations are mathematically identical.
The same reasoning applies horizontally via `off = xs - minx` at
`swrast.c:1071-1076`, where `xs` comes from per-triangle span clipping.

The magnitude is a few ULPs, which normally rounds away. It does **not** round
away when `u * width` lands exactly on an integer, because `floor` is
discontinuous there and a downward ULP moves the selected texel by a whole
column. Landing exactly on an integer is not a corner case here: it is precisely
what F2(a) causes, and what any texel-unit UV set produces. So F2 and F6
compound — F2 puts the sample on the knife edge, F6 pushes different quads off
different sides of it.

**Artefact.** A one-texel column or row duplicated or dropped along the join
between two adjacent background quads, position dependent on where each quad's
bounding box began. Under point sampling this is a hard, clearly visible seam;
under bilinear it degrades to a weight of 255/256 versus 0/256 and is largely
invisible.

**Fix in prose.** Evaluate the row base the same way the span base is evaluated
— as `plane_at_origin + dudy * (y - miny)`, a single multiply-add from an
invariant base — so that the value at a given scanline depends only on the plane
equation and the scanline index, not on the iteration history. That makes two
triangles with identical plane coefficients produce bit-identical attributes at
every pixel regardless of their bounding boxes. Fixing F2(a) so samples land at
texel centres rather than boundaries removes the sensitivity that makes this
visible.

---

### F7 — BORDER addressing is silently CLAMP, and BORDERCOLOR is never plumbed — **Minor**

`swrast.c:133-135`

```132:135:F:\rbo_fabre_proto\d3d9_sw\swrast.c
	switch (mode) {
	case D3DTADDRESS_CLAMP:
	case D3DTADDRESS_BORDER:
		return iclamp(x, 0, n - 1);
```

**D3D9 rule.** `D3DTADDRESS_BORDER` returns the `D3DSAMP_BORDERCOLOR` constant
for any coordinate outside `[0,1]`, not the nearest edge texel.

**What the code does.** Falls through to CLAMP. `D3DSAMP_BORDERCOLOR` is not
read at all — `Dev_SetSamplerState` (`d3d9_sw.c:4027-4048`) handles only
MINFILTER, MAGFILTER, ADDRESSU and ADDRESSV, and `SwState` (`swrast.h:47-48`)
has no border colour field.

**Artefact.** If any draw uses BORDER, the outermost texel row/column is smeared
outward instead of the border colour appearing — a bright or dark edge line of
the wrong colour at the quad boundary. Whether this fires at all depends on
whether the title ever sets BORDER; the draw log at `d3d9_sw.c:4034-4036`
records sampler state and would answer it.

**Fix in prose.** Capture `D3DSAMP_BORDERCOLOR` into `SwState`, and in
`addr_coord` signal out-of-range separately from the clamped index so
`tex_fetch` can substitute the border colour. `span_kernel_ok` already rejects
non-WRAP for the vector path, so only the scalar sampler needs it.

---

### F8 — Bilinear weight cannot reach full strength — **Minor**

`swrast.c:189-190` with `swrast.c:161-163`

The fractional weight is `trunc(frac * 256)`, which spans 0..255 while
`lerp8_fixed` treats the scale as 256. The second texel therefore receives at
most 255/256 and the first retains at least 1/256, so the filter is very
slightly biased toward `c00`/`c01`, i.e. toward the upper-left texel.

**D3D9 rule.** D3D9 requires at least 8 bits of subtexel precision and real
hardware quantises comparably; a maximum weight of 255/256 is within tolerance.

**Artefact.** At most one LSB of channel error, uniform across the surface. Not
a seam cause. Listed so it is not mistaken for one, and because it is a
systematic directional bias rather than a symmetric rounding error. The vector
path reproduces it exactly (`swrast.c:640-643`, `swrast.c:546-551`), so it
cannot cause a scalar/vector divergence either.

**Fix in prose.** None required. If exactness is ever wanted, rounding the
weight to nearest rather than truncating would centre the bias, at the cost of
breaking bit-exact agreement with the current vector kernel unless both change
together.

---

### F9 — Perspective correction is disabled by an absolute, scale-dependent threshold — **Minor**

`swrast.c:904-905`

```904:905:F:\rbo_fabre_proto\d3d9_sw\swrast.c
	persp = a.rhw > 0.0f && b.rhw > 0.0f && c.rhw > 0.0f &&
		(fabsf(a.rhw - b.rhw) > 1.0e-6f || fabsf(a.rhw - c.rhw) > 1.0e-6f);
```

**D3D9 rule.** With `D3DFVF_XYZRHW`, texture coordinates are interpolated with
perspective correction using RHW. When all three RHW are equal the correction is
mathematically a no-op, so treating that case as affine is legitimate and is a
sound optimisation.

**What the code does.** Uses an **absolute** tolerance of 1e-6 on the RHW
*difference*, independent of RHW magnitude. For a draw with RHW around 1.0 that
is a relative tolerance of 1e-6 and entirely safe. For a draw with small RHW —
say 5e-4 across a far plane — two genuinely different RHW values can differ by
less than 1e-6 and be treated as affine, and the same threshold is
proportionally 500x looser. Only `b` and `c` are compared against `a`; if
`a.rhw == b.rhw` and `c` differs, the second clause catches it, so the test
itself is complete.

For this title, which draws pre-transformed sprite quads that almost certainly
carry RHW = 1 uniformly, this is very unlikely to fire. Flagged for completeness.

**Artefact.** If it fires, affine texture interpolation across a quad that
needed perspective correction — a swim in the texture along the quad diagonal,
and a mismatch with any neighbouring quad that did take the perspective path.

**Fix in prose.** Make the comparison relative to the magnitude of the RHW
values rather than absolute, so the decision scales with the data.

---

### F10 — Possible FMA contraction divergence between scalar and vector — **Speculative**

`swrast.c:1103-1104` versus `swrast.c:787-788`

The scalar path evaluates `dudx * kf + u_xs` as C source. The vector path
evaluates the same expression with explicit `_mm256_mul_ps` followed by
`_mm256_add_ps`, which is unconditionally two roundings. If the compiler is
permitted to contract the scalar multiply-add into a single FMA — which it may
do by default under GCC's `-ffp-contract=fast` when the target supports FMA —
the scalar path rounds once and the vector path twice, and the two produce
different `u` in the last bit.

That is normally invisible, but combined with F2(a) placing samples exactly on
texel boundaries it is another mechanism for a whole-texel disagreement between
the scalar reference and the vector kernel, at exactly the coordinates where
seams appear.

I could not settle this by reading: the build flags are in `build.ps1`, which I
was instructed not to run, and the answer depends on both the flag and the
target ISA the 32-bit DLL is compiled for. The existing harness reports
`delta=0` on the tested scenes (see §5), which is evidence against contraction
being active *or* against the tested scenes hitting boundary-exact
coordinates — it does not distinguish the two, because the harness never
constructs boundary-exact UVs. See Open Question OQ2.

**Fix in prose.** If contraction is active, compile `swrast.c` with
`-ffp-contract=off`, or restructure the two interpolation sites so both paths
perform the same number of roundings.

---

## 3. Verified correct

Checked against the D3D9 rules and found right; these should not be
re-investigated.

- **Point-sample texel selection.** `floor(u * width)` with no half-texel
  offset, `swrast.c:183`. Correct D3D9 convention.
- **`floor` rather than truncation.** Both paths use true floor —
  `floorf` at `swrast.c:183`, `swrast.c:187-188`, and `_mm256_floor_ps` at
  `swrast.c:591-592`, `swrast.c:598`, `swrast.c:629-630`, `swrast.c:635`. So
  negative `u,v` (which the WRAP path routinely produces for scrolling
  backgrounds) select the correct texel, and there is no truncation-toward-zero
  discontinuity straddling `u = 0`. The `_mm256_cvtps_epi32` applied to an
  already-floored value at `swrast.c:636` is exact regardless of rounding mode.
- **Bilinear half-texel offset.** Subtracted before the floor at
  `swrast.c:184-185`, `swrast.c:595-596`, `swrast.c:632-633`. Correct.
- **Order of addressing versus half-texel offset.** Addressing is applied to the
  final integer index inside `tex_fetch` (`swrast.c:152-153`), after the
  half-texel subtraction and the floor, and independently to `x0` and `x0+1`
  (`swrast.c:191-194`). This is the correct order and is the single most common
  place to get a one-pixel quad-edge artefact. It is right here.
- **WRAP power-of-two masking.** `x & (n-1)` at `swrast.c:130-131` is exactly
  equivalent to the general modulo at `swrast.c:113-121` for power-of-two `n`,
  including for negative `x` (two's-complement masking yields the correct
  non-negative residue). The vector masking at `swrast.c:497-500` and
  `swrast.c:556-557` matches.
- **MIRROR.** `swrast.c:136-142` computes the period-`2n` reflection correctly,
  with the negative-modulo correction, and matches D3D9's mirror.
- **Vector wrap-aware row load.** `span_loadu_wrap` (`swrast.c:553-569`) takes
  the fast unaligned load only when all eight texels are within the row
  (`x0 <= tw_mask - 7`) and otherwise assembles them with per-texel masking.
  The bounds test is correct — no out-of-row read, no missed wrap.
- **Sequential path ignoring the coverage mask is safe.** `swrast.c:589-625`
  loads texels for dead lanes, but every index is masked into the texture, so
  there is no out-of-bounds read, and the dead lanes are discarded by the masked
  store at `swrast.c:830-833`.
- **NaN / out-of-range coordinate guard agrees between paths.** Scalar
  `swrast.c:176-179` zeroes `u` when it is not strictly inside `(-1e6, 1e6)`,
  including NaN; vector `swrast.c:579-582` zeroes when `|u| < 1e6` is false
  under an ordered compare, which is false for NaN. The two are equivalent
  including at exactly `±1e6`.
- **Perspective divide agrees between paths.** Both guard `iw == 0` and
  substitute `q = 1` — scalar `swrast.c:1113`, vector `swrast.c:794-795`.
- **Perspective premultiply.** `u * rhw` at `swrast.c:1000-1005`, divided by the
  interpolated `1/w`. Correct D3D9 treatment of RHW vertices.
- **Along-span interpolation is recomputed, not accumulated.** `swrast.c:1103`
  and vector `swrast.c:770`, `swrast.c:787-788` use the identical
  `d * k + base` form against the identical base written at `swrast.c:1085-1087`.
  The two paths index identically. (The *row* base is a separate matter — F6.)
- **Fast-path perspective guard is consistent.** `!s->persp` appears in both the
  predicate (`swrast.c:1041`) and the kernel (`swrast.c:589`), so the
  sequential path can never run on a perspective span.
- **Vector path is correctly restricted.** `span_kernel_ok` (`swrast.c:475-484`)
  rejects null/empty textures, non-power-of-two dimensions, and any addressing
  mode other than WRAP; `use_simd` (`swrast.c:972-976`) additionally requires
  flat colour, no depth, full write mask and a supported blend. So CLAMP and
  MIRROR draws deterministically take the scalar path — slower, but consistent.
- **Texture pitch assumption.** `SwTex` has no pitch field (`swrast.h:21-25`)
  and indexes `y * width + x` (`swrast.c:154`); the backing allocation is
  `calloc(width * height, 4)` (`d3d9_sw.c:3884`) and the handoff at
  `d3d9_sw.c:2263-2265` passes the same `w`/`h`. Tightly packed, so no shear.
- **Sampler address defaults.** `D3DTADDRESS_WRAP` at `d3d9_sw.c:5791-5792`
  matches the D3D9 default. (The filter defaults on the two preceding lines do
  not — F1.)
- **Sampler state round-trips through the state block.** Save/restore at
  `d3d9_sw.c:4460-4463` and `d3d9_sw.c:4497-4500` covers all four sampler fields
  the rasterizer consumes, so a state block cannot leave stale filtering or
  addressing behind.
- **Empirical scalar/vector agreement.** `test_simd.exe` reports
  `worst channel delta=0` on all ten cases, including both 1:1 sequential-load
  cases, on this machine (`cpu features = 0xf`, so AVX2 and AVX-512VL are both
  present and the EVEX gather path is the auto-selected one). The three gather
  implementations — `span_gather_avx2`, `span_gather_insert`,
  `span_gather_evex` (`swrast.c:503-541`) — are exercised in the benchmark and
  produce matching timings without triggering a mismatch.

---

## 4. Test coverage gaps

`test_simd.c` compares the scalar rasterizer against the vector one. It is a
*differential* harness only: it has no independent model of D3D9 sampling, so
**any convention error shared by both paths is invisible to it**. Every finding
in §2 except F4, F5 and F10 is of that shared kind, and the harness passes
cleanly regardless.

Specific gaps:

1. **Perspective correction is never exercised.** `build_scene` assigns one
   `rhw` per triangle and writes it to all three vertices
   (`test_simd.c:43`, `test_simd.c:51`). `persp` at `swrast.c:904-905` requires
   the RHW values to *differ*, so it is false for every triangle in every case.
   The perspective code at `swrast.c:1112-1119` and `swrast.c:789-798` has zero
   coverage. Randomising `rhw` per vertex would fix this in one line.
2. **Only WRAP addressing is ever tested.** `st.addr_u = st.addr_v = 1` in every
   case (`test_simd.c:89-90`, `test_simd.c:156`, `test_simd.c:222`). CLAMP,
   MIRROR, MIRRORONCE and BORDER in `addr_coord` (`swrast.c:124-148`) are never
   executed by any test. Since `span_kernel_ok` rejects them from the vector
   path anyway, a differential test could not cover them — they need direct
   assertions against expected texel indices.
3. **Only power-of-two textures.** `TW 256`, `TH 128` (`test_simd.c:12-13`).
   The general modulo WRAP path at `swrast.c:146` and the non-power-of-two
   rejection in `span_kernel_ok` are never reached.
4. **The 1:1 test uses an exactly-representable step, so the tolerance band in
   F5 is never entered.** `run_1to1` sets `uw = W/TW = 640/256 = 2.5`
   (`test_simd.c:137`) across 640 pixels, giving `dudx * TW` of exactly 1.0 in
   binary floating point. The predicate at `swrast.c:1041-1043` admits anything
   within 1e-4 of that, and no test constructs a step in the admitted-but-not-
   exact region. A case with, say, `uw = W/TW * (1 + 3e-5)` would exercise the
   fast path under the conditions F4 and F5 describe.
5. **The 1:1 test draws a single full-screen quad, so quad-to-quad joins are
   never tested.** `run_1to1` submits two triangles forming one quad
   (`test_simd.c:158-163`). The reported symptom is a seam *between adjacent
   quads*, which requires at least two quads sharing an edge with continuous
   UVs, and requires checking the two columns either side of the join against
   the expected texel run. No such test exists.
6. **The pass threshold tolerates a 1-LSB channel difference.** `run_1to1`
   returns `bad == 0 && worst <= 1` (`test_simd.c:193`) and `bad` only counts
   deltas greater than 1 (`test_simd.c:186`). A systematic one-step weight
   discrepancy between the paths would pass silently. In practice the observed
   `worst` is 0, so nothing is being hidden today, but the harness would not
   report a regression to 1.
7. **No boundary-exact UVs.** `build_scene` draws `u,v` from a continuous
   random range (`test_simd.c:53-54`), so `u * width` lands exactly on an
   integer with probability ~0. Every finding whose mechanism is "floor is
   discontinuous exactly here" — F2(a), F4, F6, F10 — is systematically excluded
   from the test population. A case that sets UVs to exact texel multiples, and
   to exact texel multiples plus and minus one ULP, would be the highest-value
   addition.
8. **`norm_pixel_uv_tri` is untested entirely.** It lives in `d3d9_sw.c` and the
   harness links only the rasterizer, so the heuristic in F2 — the one applied
   to every textured draw the game makes — has no test at all.

---

## 5. Open questions

**OQ1 — Does the title actually supply texel-unit texture coordinates?**
F2's severity depends on whether `norm_pixel_uv_tri` ever fires in practice. It
is enabled by default but its thresholds may never be met.
*Experiment:* enable the existing draw log (`g_draw_log`, written at
`d3d9_sw.c:2449-2464`, which already prints both the normalised `uv=` range and
the derived `texel=` range per draw) for one frame of gameplay, and check
whether any draw arrives with a UV range outside `[-2, 2]`. If none does, F2 is
dead code and the finding drops to Speculative; if some do, check whether
*adjacent* draws of the same background are classified consistently.

**OQ2 — Is the scalar interpolation multiply-add contracted to an FMA?**
Determines whether F10 is real.
*Experiment:* disassemble the built `swrast.o`/DLL around the span loop at
`swrast.c:1103` and look for `vfmadd`/`vfmadd231ss` versus separate
`mulss`/`addss`. Requires only `objdump -d` on the existing artefact — no
rebuild. Alternatively, read the flags in `build.ps1` for `-ffp-contract` and
`-mfma`/`-march`.

**OQ3 — Which sampler filter does the game actually request, and when?**
F1 only bites for draws issued before or without an explicit `SetSamplerState`.
*Experiment:* the sampler-state log line at `d3d9_sw.c:4034-4036` already
records every call including non-zero samplers. Capture one frame and check
whether MAGFILTER/MINFILTER are set before the first textured draw, and whether
MAG and MIN are ever set to different values (which is what makes F3 bite).

**OQ4 — Does any draw use CLAMP or BORDER addressing?**
Determines whether F7 matters and whether the untested scalar addressing paths
in gap 2 are live.
*Experiment:* same draw log — the `addr=%d,%d` field at `d3d9_sw.c:2464`
already reports it per draw.

**OQ5 — Do adjacent background quads actually straddle the F5 tolerance band?**
*Experiment:* the per-draw log prints each triangle's screen and texel extents
when a draw has 16 or fewer triangles (`d3d9_sw.c:2467`). For a background draw,
compute `dudx * texture_width` from the logged corner coordinates and check
whether it is exactly 1.0, within 1e-4 of 1.0, or neither. That directly
classifies which quads take the fast path and whether neighbouring quads take
different paths.

**OQ6 — Are seams visible under forced point sampling?**
A single cheap discriminator between the filter-selection findings (F1, F3) and
the geometry-of-sampling findings (F4, F5, F6).
*Experiment:* run with `D3D9SW_FILTER=point` (`d3d9_sw.c:1996-2006`). If the
seams vanish, F1/F3 dominate. If they sharpen into hard one-pixel column
offsets, F2/F6 dominate. Also run with `D3D9SW_NOSIMD=1` (`swrast.c:418`) and
with `D3D9SW_PIXELUV=0` (`d3d9_sw.c:2012`) to isolate F4/F5 and F2 respectively.
These four env vars together partition the finding list, and none requires a
rebuild.
