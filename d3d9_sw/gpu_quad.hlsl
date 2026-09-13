// The whole of this game's pixel pipeline, in two shaders.
//
// The frame mix counters say tex=100, flat=100, ztest=0, atest=0: every draw is
// a textured quad multiplied by one flat vertex colour, with no depth, no alpha
// test and nothing resembling a light. There is no second case to handle, so
// there is no second shader.
//
// Positions arrive in pixels rather than clip space, because that is what the
// game's vertices already are by the time they reach us and converting here
// costs one multiply-add against converting every vertex on the CPU. gXform
// carries (2/width, -2/height, -1, +1): the y term is negative because D3D's
// clip space counts upward and a framebuffer counts downward.

cbuffer Xform : register(b0)
{
	float4 gXform;
};

struct VIn {
	float2 pos : POSITION;
	float2 uv  : TEXCOORD0;
	float4 col : COLOR0;
};

struct VOut {
	float4 pos : SV_POSITION;
	float2 uv  : TEXCOORD0;
	float4 col : COLOR0;
};

VOut vs_main(VIn i)
{
	VOut o;
	o.pos = float4(i.pos.x * gXform.x + gXform.z,
		       i.pos.y * gXform.y + gXform.w, 0.0f, 1.0f);
	o.uv = i.uv;
	o.col = i.col;
	return o;
}

Texture2D    gTex : register(t0);
SamplerState gSmp : register(s0);

// The per-pixel work the software path does outside of sampling and blending.
// All of it is off in the common case, but a draw that needs one of these
// cannot be handed back to the rasteriser: the two paths write to different
// surfaces, so a frame split between them would come apart. Cheaper to let the
// shader carry all three and switch them off with a constant.
//
//   x  alpha test reference, 0 for off
//   y  alpha sharpen slope about the halfway point, 0 or 1 for off
//   z  multiply-identity fade, nonzero for on
//   w  unused
cbuffer Ctl : register(b0)
{
	float4 gCtl;
};

float4 ps_main(VOut i) : SV_TARGET
{
	float4 c = gTex.Sample(gSmp, i.uv);

	// A distance field stores the glyph edge at 0.5 and ramps over several
	// texels, so a magnified one has no crisp edge to sample. Steepening the
	// ramp about that midpoint recovers an antialiased edge.
	if (gCtl.y > 1.0f)
		c.a = saturate((c.a - 0.5f) * gCtl.y + 0.5f);

	c *= i.col;

	// Under a multiply blend a transparent source would otherwise drag the
	// destination to black. White is the multiplicative identity, so fading
	// toward it by alpha makes "transparent" mean "leaves this alone".
	if (gCtl.z != 0.0f)
		c.rgb = lerp(float3(1.0f, 1.0f, 1.0f), c.rgb, c.a);

	if (gCtl.x > 0.0f)
		clip(c.a - gCtl.x);

	return c;
}
