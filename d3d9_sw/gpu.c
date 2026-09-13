/* Hardware backend: the same 2D pipeline the software rasteriser runs, handed
 * to a real GPU.
 *
 * Why at all, when the rasteriser reaches 60 fps in most rooms: address space.
 * This is a 32-bit process with 2 GB, the arena reserves 768 MB of it up front
 * to hold texture payload, and a live driver was measured at 229 MB. Textures
 * that live on the adapter are not in this process at all, so the trade is
 * several hundred megabytes in our favour - and address space, not frame time,
 * is what this game eventually dies of.
 *
 * Stage 1, which is what is here: stand a real device and swapchain on the
 * game's window and present the software framebuffer through it. That buys no
 * frame time. It buys proof that a driver can own this window without upsetting
 * the game, and it builds every piece stage 2 needs - device, swapchain,
 * shaders, input layout, blend states, sampler, dynamic buffers - against a
 * frame whose correct output is already known, so anything wrong is visible
 * immediately rather than tangled up with a new draw path.
 *
 * The device is created through the real d3d11.dll loaded by absolute path.
 * Windows' d3d11 imports exactly one function from dxgi.dll, CreateDXGIFactory2,
 * and the loader binds that to our own trampoline by name; gpuprobe raises a
 * flag for the length of the creation call so the real factory is handed over
 * for that one moment and the game keeps getting ours at every other.
 */
#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#include <windows.h>
#include <stdint.h>
#include <stdio.h>

#include "swrast.h"

#include "gpu_quad_vs.h"
#include "gpu_quad_ps.h"

unsigned savestate_getenv(const char *name, char *buf, unsigned cap);
int gpuprobe_hold_real_factory(int on);

/* Hand-declared rather than taken from d3d11.h, for the same reason gpuprobe
 * does it: this file has to describe the real API, and the project's own
 * headers describe our imitation of it. Only the slots called are named, and
 * every slot before one that is called has to be named too, because COM
 * dispatch is by position and a missing entry is a call to its neighbour.
 */
typedef struct GpuUnkVtbl {
	HRESULT(WINAPI *QueryInterface)(void *, const GUID *, void **);
	ULONG(WINAPI *AddRef)(void *);
	ULONG(WINAPI *Release)(void *);
} GpuUnkVtbl;

typedef struct GpuUnk {
	GpuUnkVtbl *v;
} GpuUnk;

#define GPU_REL(p)                                                                       \
	do {                                                                             \
		if (p) {                                                                 \
			((GpuUnk *)(p))->v->Release(p);                                  \
			(p) = NULL;                                                      \
		}                                                                        \
	} while (0)

/* --- the handful of D3D11/DXGI structures actually passed --- */

typedef struct {
	UINT Width, Height;
	struct {
		UINT Numerator, Denominator;
	} RefreshRate;
	UINT Format;
	UINT ScanlineOrdering, Scaling;
} GpuModeDesc;

typedef struct {
	GpuModeDesc BufferDesc;
	struct {
		UINT Count, Quality;
	} SampleDesc;
	UINT BufferUsage;
	UINT BufferCount;
	HWND OutputWindow;
	BOOL Windowed;
	UINT SwapEffect;
	UINT Flags;
} GpuSwapDesc;

typedef struct {
	UINT Width, Height, MipLevels, ArraySize, Format;
	struct {
		UINT Count, Quality;
	} SampleDesc;
	UINT Usage, BindFlags, CPUAccessFlags, MiscFlags;
} GpuTex2DDesc;

typedef struct {
	const void *pSysMem;
	UINT SysMemPitch, SysMemSlicePitch;
} GpuSubresData;

typedef struct {
	UINT ByteWidth, Usage, BindFlags, CPUAccessFlags, MiscFlags, StructureByteStride;
} GpuBufferDesc;

typedef struct {
	void *pData;
	UINT RowPitch, DepthPitch;
} GpuMapped;

typedef struct {
	const char *SemanticName;
	UINT SemanticIndex, Format, InputSlot, AlignedByteOffset, InputSlotClass,
		InstanceDataStepRate;
} GpuInputElem;

typedef struct {
	FLOAT TopLeftX, TopLeftY, Width, Height, MinDepth, MaxDepth;
} GpuViewport;

typedef struct {
	BOOL BlendEnable;
	UINT SrcBlend, DestBlend, BlendOp, SrcBlendAlpha, DestBlendAlpha, BlendOpAlpha;
	UINT8 RenderTargetWriteMask;
} GpuRTBlendDesc;

typedef struct {
	BOOL AlphaToCoverageEnable, IndependentBlendEnable;
	GpuRTBlendDesc RenderTarget[8];
} GpuBlendDesc;

typedef struct {
	UINT FillMode, CullMode;
	BOOL FrontCounterClockwise;
	INT DepthBias;
	FLOAT DepthBiasClamp, SlopeScaledDepthBias;
	BOOL DepthClipEnable, ScissorEnable, MultisampleEnable, AntialiasedLineEnable;
} GpuRastDesc;

typedef struct {
	UINT Filter, AddressU, AddressV, AddressW;
	FLOAT MipLODBias;
	UINT MaxAnisotropy, ComparisonFunc;
	FLOAT BorderColor[4];
	FLOAT MinLOD, MaxLOD;
} GpuSamplerDesc;

/* D3D11 enum values used below, named so the calls read as themselves. */
enum {
	GPU_FMT_B8G8R8A8 = 87,
	GPU_FMT_R32G32_FLOAT = 16,
	GPU_FMT_R8G8B8A8_UNORM = 28,
	GPU_USAGE_DEFAULT = 0,
	GPU_USAGE_DYNAMIC = 2,
	GPU_BIND_VB = 1,
	GPU_BIND_CB = 4,
	GPU_BIND_SRV = 8,
	GPU_BIND_RTV = 32,
	GPU_CPU_WRITE = 0x10000,
	GPU_MAP_WRITE_DISCARD = 4,
	GPU_TOPO_TRILIST = 4,
	GPU_INPUT_PER_VERTEX = 0,
	GPU_BLEND_ZERO = 1,
	GPU_BLEND_ONE = 2,
	GPU_BLEND_SRC_COLOR = 3,
	GPU_BLEND_SRC_ALPHA = 5,
	GPU_BLEND_INV_SRC_ALPHA = 6,
	GPU_BLENDOP_ADD = 1,
	GPU_BLENDOP_REVSUB = 3,
	GPU_WRITE_ALL = 15,
	GPU_FILTER_POINT = 0,
	GPU_FILTER_LINEAR = 0x15,
	GPU_ADDR_CLAMP = 3,
	GPU_SWAP_DISCARD = 0,
	GPU_SWAP_FLIP_DISCARD = 4,
	GPU_DRIVER_HARDWARE = 1
};

typedef struct GpuDeviceVtbl {
	GpuUnkVtbl unk;
	HRESULT(WINAPI *CreateBuffer)(void *, const GpuBufferDesc *, const GpuSubresData *,
				      void **);
	HRESULT(WINAPI *CreateTexture1D)(void *, const void *, const void *, void **);
	HRESULT(WINAPI *CreateTexture2D)(void *, const GpuTex2DDesc *, const GpuSubresData *,
					 void **);
	HRESULT(WINAPI *CreateTexture3D)(void *, const void *, const void *, void **);
	HRESULT(WINAPI *CreateShaderResourceView)(void *, void *, const void *, void **);
	HRESULT(WINAPI *CreateUnorderedAccessView)(void *, void *, const void *, void **);
	HRESULT(WINAPI *CreateRenderTargetView)(void *, void *, const void *, void **);
	HRESULT(WINAPI *CreateDepthStencilView)(void *, void *, const void *, void **);
	HRESULT(WINAPI *CreateInputLayout)(void *, const GpuInputElem *, UINT, const void *,
					   SIZE_T, void **);
	HRESULT(WINAPI *CreateVertexShader)(void *, const void *, SIZE_T, void *, void **);
	HRESULT(WINAPI *CreateGeometryShader)(void *, const void *, SIZE_T, void *, void **);
	HRESULT(WINAPI *CreateGeometryShaderWithStreamOutput)(void *, const void *, SIZE_T,
							      const void *, UINT, const UINT *,
							      UINT, UINT, void *, void **);
	HRESULT(WINAPI *CreatePixelShader)(void *, const void *, SIZE_T, void *, void **);
	HRESULT(WINAPI *CreateHullShader)(void *, const void *, SIZE_T, void *, void **);
	HRESULT(WINAPI *CreateDomainShader)(void *, const void *, SIZE_T, void *, void **);
	HRESULT(WINAPI *CreateComputeShader)(void *, const void *, SIZE_T, void *, void **);
	HRESULT(WINAPI *CreateClassLinkage)(void *, void **);
	HRESULT(WINAPI *CreateBlendState)(void *, const GpuBlendDesc *, void **);
	HRESULT(WINAPI *CreateDepthStencilState)(void *, const void *, void **);
	HRESULT(WINAPI *CreateRasterizerState)(void *, const GpuRastDesc *, void **);
	HRESULT(WINAPI *CreateSamplerState)(void *, const GpuSamplerDesc *, void **);
	HRESULT(WINAPI *CreateQuery)(void *, const void *, void **);
} GpuDeviceVtbl;

typedef struct GpuCtxVtbl {
	GpuUnkVtbl unk;
	void(WINAPI *GetDevice)(void *, void **);
	HRESULT(WINAPI *GetPrivateData)(void *, const GUID *, UINT *, void *);
	HRESULT(WINAPI *SetPrivateData)(void *, const GUID *, UINT, const void *);
	HRESULT(WINAPI *SetPrivateDataInterface)(void *, const GUID *, const void *);
	void(WINAPI *VSSetConstantBuffers)(void *, UINT, UINT, void *const *);
	void(WINAPI *PSSetShaderResources)(void *, UINT, UINT, void *const *);
	void(WINAPI *PSSetShader)(void *, void *, void *const *, UINT);
	void(WINAPI *PSSetSamplers)(void *, UINT, UINT, void *const *);
	void(WINAPI *VSSetShader)(void *, void *, void *const *, UINT);
	void(WINAPI *DrawIndexed)(void *, UINT, UINT, INT);
	void(WINAPI *Draw)(void *, UINT, UINT);
	HRESULT(WINAPI *Map)(void *, void *, UINT, UINT, UINT, GpuMapped *);
	void(WINAPI *Unmap)(void *, void *, UINT);
	void(WINAPI *PSSetConstantBuffers)(void *, UINT, UINT, void *const *);
	void(WINAPI *IASetInputLayout)(void *, void *);
	void(WINAPI *IASetVertexBuffers)(void *, UINT, UINT, void *const *, const UINT *,
					 const UINT *);
	void(WINAPI *IASetIndexBuffer)(void *, void *, UINT, UINT);
	void(WINAPI *DrawIndexedInstanced)(void *, UINT, UINT, UINT, INT, UINT);
	void(WINAPI *DrawInstanced)(void *, UINT, UINT, UINT, UINT);
	void(WINAPI *GSSetConstantBuffers)(void *, UINT, UINT, void *const *);
	void(WINAPI *GSSetShader)(void *, void *, void *const *, UINT);
	void(WINAPI *IASetPrimitiveTopology)(void *, UINT);
	void(WINAPI *VSSetShaderResources)(void *, UINT, UINT, void *const *);
	void(WINAPI *VSSetSamplers)(void *, UINT, UINT, void *const *);
	void(WINAPI *Begin)(void *, void *);
	void(WINAPI *End)(void *, void *);
	HRESULT(WINAPI *GetData)(void *, void *, void *, UINT, UINT);
	void(WINAPI *SetPredication)(void *, void *, BOOL);
	void(WINAPI *GSSetShaderResources)(void *, UINT, UINT, void *const *);
	void(WINAPI *GSSetSamplers)(void *, UINT, UINT, void *const *);
	void(WINAPI *OMSetRenderTargets)(void *, UINT, void *const *, void *);
	void(WINAPI *OMSetRenderTargetsAndUnorderedAccessViews)(void *, UINT, void *const *,
								void *, UINT, UINT,
								void *const *, const UINT *);
	void(WINAPI *OMSetBlendState)(void *, void *, const FLOAT *, UINT);
	void(WINAPI *OMSetDepthStencilState)(void *, void *, UINT);
	void(WINAPI *SOSetTargets)(void *, UINT, void *const *, const UINT *);
	void(WINAPI *DrawAuto)(void *);
	void(WINAPI *DrawIndexedInstancedIndirect)(void *, void *, UINT);
	void(WINAPI *DrawInstancedIndirect)(void *, void *, UINT);
	void(WINAPI *Dispatch)(void *, UINT, UINT, UINT);
	void(WINAPI *DispatchIndirect)(void *, void *, UINT);
	void(WINAPI *RSSetState)(void *, void *);
	void(WINAPI *RSSetViewports)(void *, UINT, const GpuViewport *);
	void(WINAPI *RSSetScissorRects)(void *, UINT, const RECT *);
	void(WINAPI *CopySubresourceRegion)(void *, void *, UINT, UINT, UINT, UINT, void *,
					    UINT, const void *);
	void(WINAPI *CopyResource)(void *, void *, void *);
	void(WINAPI *UpdateSubresource)(void *, void *, UINT, const void *, const void *, UINT,
					UINT);
	void(WINAPI *CopyStructureCount)(void *, void *, UINT, void *);
	void(WINAPI *ClearRenderTargetView)(void *, void *, const FLOAT *);
} GpuCtxVtbl;

typedef struct GpuSwapVtbl {
	GpuUnkVtbl unk;
	/* IDXGIObject */
	HRESULT(WINAPI *SetPrivateData)(void *, const GUID *, UINT, const void *);
	HRESULT(WINAPI *SetPrivateDataInterface)(void *, const GUID *, const void *);
	HRESULT(WINAPI *GetPrivateData)(void *, const GUID *, UINT *, void *);
	HRESULT(WINAPI *GetParent)(void *, const GUID *, void **);
	/* IDXGIDeviceSubObject */
	HRESULT(WINAPI *GetDevice)(void *, const GUID *, void **);
	/* IDXGISwapChain */
	HRESULT(WINAPI *Present)(void *, UINT, UINT);
	HRESULT(WINAPI *GetBuffer)(void *, UINT, const GUID *, void **);
} GpuSwapVtbl;

static const GUID GPU_IID_ID3D11Texture2D = {
	0x6f15aaf2, 0xd208, 0x4e89, { 0x9a, 0xb4, 0x48, 0x95, 0x35, 0xd3, 0x4f, 0x9c }
};

typedef HRESULT(WINAPI *PFN_CreateDeviceAndSwapChain)(void *adapter, int driver, HMODULE sw,
						      UINT flags, const UINT *levels,
						      UINT nlevels, UINT sdk,
						      const GpuSwapDesc *scd, void **swap,
						      void **dev, UINT *got, void **ctx);

/* --- state --- */

struct GpuVert {
	float x, y, u, v;
	float r, g, b, a;
};

static struct {
	int tried, up;
	HWND hwnd;
	void *dev, *ctx, *swap, *rtv;
	void *vs, *ps, *layout, *cb, *ps_cb, *vb, *smp_point, *smp_linear;
	void *blend_over, *blend_add, *blend_mod, *blend_rsub, *blend_off;
	void *fb_tex, *fb_srv; /* stage 1: the software framebuffer as a texture */
	int fb_w, fb_h;
	/* stage 2: the game's frame, built at its own size and rescaled once on
	 * the way to the window */
	void *rt_tex, *rt_rtv, *rt_srv, *rt_stage;
	int rt_w, rt_h;
	void *rs_plain, *rs_scissor;
	void *query; /* an event fence, so a park can wait for the adapter */
	int bb_w, bb_h;
	unsigned frames, uploads, draws, verts;
	void (*log)(const char *);
} g;

#define GPU_VB_VERTS 16384

static void gpu_say(const char *fmt, ...)
{
	char line[512];
	va_list ap;

	if (!g.log)
		return;
	va_start(ap, fmt);
	_vsnprintf(line, sizeof(line), fmt, ap);
	va_end(ap);
	line[sizeof(line) - 1] = 0;
	g.log(line);
}

static int gpu_on(void)
{
	char b[8];

	return savestate_getenv("D3D11SW_GPU", b, sizeof(b)) && b[0] > '0' && b[0] <= '9';
}

static HMODULE gpu_system_dll(const char *leaf)
{
	char path[MAX_PATH];
	UINT n = GetSystemDirectoryA(path, MAX_PATH);

	if (!n || n >= MAX_PATH - 32)
		return NULL;
	_snprintf(path + n, MAX_PATH - n, "\\%s", leaf);
	return LoadLibraryA(path);
}

static void *gpu_blend_op(int src, int dst, int op)
{
	GpuBlendDesc bd;
	void *st = NULL;
	GpuDeviceVtbl *dv = (GpuDeviceVtbl *)((GpuUnk *)g.dev)->v;

	memset(&bd, 0, sizeof(bd));
	bd.RenderTarget[0].BlendEnable = src != 0;
	bd.RenderTarget[0].SrcBlend = src ? (UINT)src : GPU_BLEND_ONE;
	bd.RenderTarget[0].DestBlend = dst ? (UINT)dst : GPU_BLEND_ZERO;
	bd.RenderTarget[0].BlendOp = (UINT)op;
	/* Alpha is carried along rather than composited: nothing downstream reads
	 * the backbuffer's alpha, and giving it the colour factors would make a
	 * multiply blend darken it toward zero for no reader's benefit. */
	bd.RenderTarget[0].SrcBlendAlpha = GPU_BLEND_ONE;
	bd.RenderTarget[0].DestBlendAlpha = GPU_BLEND_ZERO;
	bd.RenderTarget[0].BlendOpAlpha = GPU_BLENDOP_ADD;
	bd.RenderTarget[0].RenderTargetWriteMask = GPU_WRITE_ALL;
	if (FAILED(dv->CreateBlendState(g.dev, &bd, &st)))
		return NULL;
	return st;
}

static void *gpu_blend(int src, int dst)
{
	return gpu_blend_op(src, dst, GPU_BLENDOP_ADD);
}

static void *gpu_sampler(int filter)
{
	GpuSamplerDesc sd;
	void *st = NULL;
	GpuDeviceVtbl *dv = (GpuDeviceVtbl *)((GpuUnk *)g.dev)->v;

	memset(&sd, 0, sizeof(sd));
	sd.Filter = (UINT)filter;
	/* Clamp on every axis. The software path reached the same conclusion from
	 * the other direction: draws whose coordinates stay inside the texture do
	 * not need wrapping, and this game's do. */
	sd.AddressU = sd.AddressV = sd.AddressW = GPU_ADDR_CLAMP;
	sd.MaxLOD = 3.402823466e+38f;
	if (FAILED(dv->CreateSamplerState(g.dev, &sd, &st)))
		return NULL;
	return st;
}

static int gpu_blit(void *srv);
static int gpu_rt_ensure(int w, int h);

static void gpu_teardown(void)
{
	GPU_REL(g.fb_srv);
	GPU_REL(g.fb_tex);
	GPU_REL(g.blend_off);
	GPU_REL(g.blend_rsub);
	GPU_REL(g.blend_mod);
	GPU_REL(g.blend_add);
	GPU_REL(g.blend_over);
	GPU_REL(g.smp_linear);
	GPU_REL(g.smp_point);
	GPU_REL(g.vb);
	GPU_REL(g.query);
	GPU_REL(g.rs_scissor);
	GPU_REL(g.rs_plain);
	GPU_REL(g.rt_stage);
	GPU_REL(g.rt_srv);
	GPU_REL(g.rt_rtv);
	GPU_REL(g.rt_tex);
	GPU_REL(g.ps_cb);
	GPU_REL(g.cb);
	GPU_REL(g.layout);
	GPU_REL(g.ps);
	GPU_REL(g.vs);
	GPU_REL(g.rtv);
	GPU_REL(g.swap);
	GPU_REL(g.ctx);
	GPU_REL(g.dev);
	g.up = 0;
}

void gpu_set_log(void (*log)(const char *))
{
	g.log = log;
}

/* Returns 1 once the device is usable. Safe to call every frame; it does its
 * work on the first and answers from a flag afterwards. */
static int gpu_init(HWND hwnd)
{
	HMODULE real_dxgi, real_d3d11;
	PFN_CreateDeviceAndSwapChain create;
	GpuSwapDesc scd;
	GpuDeviceVtbl *dv;
	RECT rc;
	UINT level = 0;
	HRESULT hr;

	if (g.up)
		return 1;
	if (g.tried)
		return 0;
	g.tried = 1;
	if (!gpu_on())
		return 0;
	if (!hwnd || !GetClientRect(hwnd, &rc) || rc.right <= 0 || rc.bottom <= 0) {
		gpu_say("gpu: no usable client area on %p, staying in software",
			(void *)hwnd);
		return 0;
	}
	g.hwnd = hwnd;
	g.bb_w = rc.right;
	g.bb_h = rc.bottom;

	real_dxgi = gpu_system_dll("dxgi.dll");
	real_d3d11 = gpu_system_dll("d3d11.dll");
	if (!real_dxgi || !real_d3d11) {
		gpu_say("gpu: system dxgi=%p d3d11=%p - one of them would not load",
			(void *)real_dxgi, (void *)real_d3d11);
		return 0;
	}
	create = (PFN_CreateDeviceAndSwapChain)GetProcAddress(real_d3d11,
							      "D3D11CreateDeviceAndSwapChain");
	if (!create) {
		gpu_say("gpu: real d3d11 has no D3D11CreateDeviceAndSwapChain");
		return 0;
	}

	memset(&scd, 0, sizeof(scd));
	scd.BufferDesc.Width = (UINT)g.bb_w;
	scd.BufferDesc.Height = (UINT)g.bb_h;
	scd.BufferDesc.Format = GPU_FMT_B8G8R8A8;
	scd.SampleDesc.Count = 1;
	scd.BufferUsage = 32; /* RENDER_TARGET_OUTPUT */
	scd.BufferCount = 2;
	scd.OutputWindow = hwnd;
	scd.Windowed = TRUE;
	/* The old blit model, not flip. Flip forbids anything else drawing to the
	 * window, and this stage has to be able to fall back to the software
	 * present without tearing the window's ownership in half. */
	scd.SwapEffect = GPU_SWAP_DISCARD;

	/* The real d3d11 imports CreateDXGIFactory2 and the loader binds it to
	 * our trampoline by name, so the real one is lent out for exactly this
	 * call and taken back the moment it returns. */
	gpuprobe_hold_real_factory(1);
	hr = create(NULL, GPU_DRIVER_HARDWARE, NULL, 0, NULL, 0, 7, &scd, &g.swap, &g.dev,
		    &level, &g.ctx);
	gpuprobe_hold_real_factory(0);
	if (FAILED(hr) || !g.dev || !g.swap) {
		gpu_say("gpu: device and swapchain REFUSED hr=%08lX, staying in software",
			(unsigned long)hr);
		gpu_teardown();
		return 0;
	}
	dv = (GpuDeviceVtbl *)((GpuUnk *)g.dev)->v;

	{
		GpuSwapVtbl *sv = (GpuSwapVtbl *)((GpuUnk *)g.swap)->v;
		void *back = NULL;

		if (FAILED(sv->GetBuffer(g.swap, 0, &GPU_IID_ID3D11Texture2D, &back)) ||
		    !back) {
			gpu_say("gpu: swapchain has no back buffer to take");
			gpu_teardown();
			return 0;
		}
		hr = dv->CreateRenderTargetView(g.dev, back, NULL, &g.rtv);
		GPU_REL(back);
		if (FAILED(hr)) {
			gpu_say("gpu: no render target view on the back buffer, hr=%08lX",
				(unsigned long)hr);
			gpu_teardown();
			return 0;
		}
	}

	if (FAILED(dv->CreateVertexShader(g.dev, kGpuQuadVS, sizeof(kGpuQuadVS), NULL,
					  &g.vs)) ||
	    FAILED(dv->CreatePixelShader(g.dev, kGpuQuadPS, sizeof(kGpuQuadPS), NULL, &g.ps))) {
		gpu_say("gpu: the driver rejected our shaders");
		gpu_teardown();
		return 0;
	}
	{
		static const GpuInputElem elems[3] = {
			{ "POSITION", 0, GPU_FMT_R32G32_FLOAT, 0, 0, GPU_INPUT_PER_VERTEX, 0 },
			{ "TEXCOORD", 0, GPU_FMT_R32G32_FLOAT, 0, 8, GPU_INPUT_PER_VERTEX, 0 },
			{ "COLOR", 0, 2 /* R32G32B32A32_FLOAT */, 0, 16, GPU_INPUT_PER_VERTEX,
			  0 }
		};

		if (FAILED(dv->CreateInputLayout(g.dev, elems, 3, kGpuQuadVS,
						 sizeof(kGpuQuadVS), &g.layout))) {
			gpu_say("gpu: input layout rejected");
			gpu_teardown();
			return 0;
		}
	}
	{
		GpuBufferDesc bd;

		memset(&bd, 0, sizeof(bd));
		bd.ByteWidth = 16;
		bd.Usage = GPU_USAGE_DYNAMIC;
		bd.BindFlags = GPU_BIND_CB;
		bd.CPUAccessFlags = GPU_CPU_WRITE;
		if (FAILED(dv->CreateBuffer(g.dev, &bd, NULL, &g.cb))) {
			gpu_say("gpu: constant buffer rejected");
			gpu_teardown();
			return 0;
		}
		memset(&bd, 0, sizeof(bd));
		bd.ByteWidth = 16;
		bd.Usage = GPU_USAGE_DYNAMIC;
		bd.BindFlags = GPU_BIND_CB;
		bd.CPUAccessFlags = GPU_CPU_WRITE;
		if (FAILED(dv->CreateBuffer(g.dev, &bd, NULL, &g.ps_cb))) {
			gpu_say("gpu: pixel constant buffer rejected");
			gpu_teardown();
			return 0;
		}
		memset(&bd, 0, sizeof(bd));
		bd.ByteWidth = GPU_VB_VERTS * sizeof(struct GpuVert);
		bd.Usage = GPU_USAGE_DYNAMIC;
		bd.BindFlags = GPU_BIND_VB;
		bd.CPUAccessFlags = GPU_CPU_WRITE;
		if (FAILED(dv->CreateBuffer(g.dev, &bd, NULL, &g.vb))) {
			gpu_say("gpu: vertex buffer rejected");
			gpu_teardown();
			return 0;
		}
	}
	{
		/* Culling off, because the game's quads arrive in whichever winding
		 * the sprite's flip gave them and the software path never cared.
		 * Scissor is the only thing that differs between the two states. */
		GpuRastDesc rd;

		memset(&rd, 0, sizeof(rd));
		rd.FillMode = 3; /* SOLID */
		rd.CullMode = 1; /* NONE */
		rd.DepthClipEnable = TRUE;
		if (FAILED(dv->CreateRasterizerState(g.dev, &rd, &g.rs_plain))) {
			gpu_say("gpu: rasteriser state rejected");
			gpu_teardown();
			return 0;
		}
		rd.ScissorEnable = TRUE;
		if (FAILED(dv->CreateRasterizerState(g.dev, &rd, &g.rs_scissor))) {
			gpu_say("gpu: scissored rasteriser state rejected");
			gpu_teardown();
			return 0;
		}
	}
	g.smp_point = gpu_sampler(GPU_FILTER_POINT);
	g.smp_linear = gpu_sampler(GPU_FILTER_LINEAR);
	g.blend_over = gpu_blend(GPU_BLEND_SRC_ALPHA, GPU_BLEND_INV_SRC_ALPHA);
	g.blend_add = gpu_blend(GPU_BLEND_SRC_ALPHA, GPU_BLEND_ONE);
	g.blend_mod = gpu_blend(GPU_BLEND_ZERO, GPU_BLEND_SRC_COLOR);
	g.blend_off = gpu_blend(0, 0);
	/* Reverse subtract, dst - src. The town's census puts it at about a
	 * megapixel a frame, which is small but not optional: once the adapter
	 * owns the frame, a blend it will not take is a sprite that is simply not
	 * there. */
	g.blend_rsub = gpu_blend_op(GPU_BLEND_SRC_ALPHA, GPU_BLEND_ONE, GPU_BLENDOP_REVSUB);
	if (!g.smp_point || !g.smp_linear || !g.blend_over || !g.blend_add || !g.blend_mod ||
	    !g.blend_rsub || !g.blend_off) {
		gpu_say("gpu: a sampler or blend state was refused");
		gpu_teardown();
		return 0;
	}

	g.up = 1;
	gpu_say("gpu: up at feature level %u.%u on a %dx%d window. The three blend modes "
		"the frame mix reports - over, add and multiply - are all here as real "
		"states, which is the same set the vector kernel handles",
		(unsigned)(level >> 12), (unsigned)((level >> 8) & 0xf), g.bb_w, g.bb_h);
	return 1;
}

/* Brings the device up outside the present path.
 *
 * Stage 1 could init lazily from its own present hook, because that hook was
 * the only caller. At mode 2 the hook is deliberately not installed - the
 * adapter presents its own frame instead - so without this there is nothing
 * left to create the device, and every draw quietly declines against a backend
 * that was never asked to exist. */
int gpu_ensure(HWND hwnd)
{
	return gpu_init(hwnd);
}

/* Stage 1: the software framebuffer, presented through the GPU.
 *
 * The rasteriser is untouched and still produces the frame; this replaces only
 * the blit that puts it on screen. If it looks wrong, the fault is in this file
 * and not in anything that computed a pixel, which is the point of doing it
 * before the draw path rather than after.
 */
int gpu_present_framebuffer(HWND hwnd, const uint32_t *pixels, int w, int h)
{
	GpuDeviceVtbl *dv;
	GpuCtxVtbl *cv;
	GpuSwapVtbl *sv;

	/* Lazily, because this is the only place that reliably knows the window:
	 * the swapchain owns it and the one-shot installer runs before any of
	 * that is reachable. */
	if (!g.up && !gpu_init(hwnd))
		return 0;
	if (!pixels || w <= 0 || h <= 0)
		return 0;
	dv = (GpuDeviceVtbl *)((GpuUnk *)g.dev)->v;
	cv = (GpuCtxVtbl *)((GpuUnk *)g.ctx)->v;
	sv = (GpuSwapVtbl *)((GpuUnk *)g.swap)->v;

	if (g.fb_tex && (g.fb_w != w || g.fb_h != h)) {
		GPU_REL(g.fb_srv);
		GPU_REL(g.fb_tex);
	}
	if (!g.fb_tex) {
		GpuTex2DDesc td;

		memset(&td, 0, sizeof(td));
		td.Width = (UINT)w;
		td.Height = (UINT)h;
		td.MipLevels = 1;
		td.ArraySize = 1;
		td.Format = GPU_FMT_B8G8R8A8;
		td.SampleDesc.Count = 1;
		td.Usage = GPU_USAGE_DYNAMIC;
		td.BindFlags = GPU_BIND_SRV;
		td.CPUAccessFlags = GPU_CPU_WRITE;
		if (FAILED(dv->CreateTexture2D(g.dev, &td, NULL, &g.fb_tex)))
			return 0;
		if (FAILED(dv->CreateShaderResourceView(g.dev, g.fb_tex, NULL, &g.fb_srv))) {
			GPU_REL(g.fb_tex);
			return 0;
		}
		g.fb_w = w;
		g.fb_h = h;
	}
	{
		GpuMapped m;

		if (FAILED(cv->Map(g.ctx, g.fb_tex, 0, GPU_MAP_WRITE_DISCARD, 0, &m)))
			return 0;
		/* Row by row: the driver picks its own pitch and it is rarely the
		 * width, so one memcpy of the whole image would shear it. */
		{
			int y;

			for (y = 0; y < h; y++)
				memcpy((char *)m.pData + (size_t)y * m.RowPitch,
				       pixels + (size_t)y * w, (size_t)w * 4);
		}
		cv->Unmap(g.ctx, g.fb_tex, 0);
	}
	if (!gpu_blit(g.fb_srv))
		return 0;
	sv->Present(g.swap, 1, 0);
	if (++g.frames == 1)
		gpu_say("gpu: first frame presented through the adapter");
	return 1;
}

/* One image, rescaled onto the whole window and shown. Shared by the stage 1
 * present of a software frame and the stage 2 present of a GPU-rendered one,
 * because from here they are the same operation on different sources. */
static int gpu_blit(void *srv)
{
	GpuCtxVtbl *cv = (GpuCtxVtbl *)((GpuUnk *)g.ctx)->v;

	{
		struct GpuVert *vp;
		GpuMapped m;
		/* Two triangles in pixel coordinates, which is what the shader's
		 * transform expects. White, because the source is already the
		 * finished image and the modulate must not alter it. */
		const float x1 = (float)g.bb_w, y1 = (float)g.bb_h;
		static const float uv[6][2] = { { 0, 0 }, { 1, 0 }, { 0, 1 },
						{ 1, 0 }, { 1, 1 }, { 0, 1 } };
		const float px[6][2] = { { 0, 0 },   { x1, 0 },  { 0, y1 },
					 { x1, 0 }, { x1, y1 }, { 0, y1 } };
		int i;

		if (FAILED(cv->Map(g.ctx, g.vb, 0, GPU_MAP_WRITE_DISCARD, 0, &m)))
			return 0;
		vp = (struct GpuVert *)m.pData;
		for (i = 0; i < 6; i++) {
			vp[i].x = px[i][0];
			vp[i].y = px[i][1];
			vp[i].u = uv[i][0];
			vp[i].v = uv[i][1];
			vp[i].r = vp[i].g = vp[i].b = vp[i].a = 1.0f;
		}
		cv->Unmap(g.ctx, g.vb, 0);
	}
	{
		GpuMapped m;
		float *x;

		if (FAILED(cv->Map(g.ctx, g.cb, 0, GPU_MAP_WRITE_DISCARD, 0, &m)))
			return 0;
		x = (float *)m.pData;
		x[0] = 2.0f / (float)g.bb_w;
		x[1] = -2.0f / (float)g.bb_h;
		x[2] = -1.0f;
		x[3] = 1.0f;
		cv->Unmap(g.ctx, g.cb, 0);
	}
	{
		/* Every control off: this is a straight copy of a finished image
		 * and none of the per-pixel work belongs to it. */
		GpuMapped m;
		float *x;

		if (FAILED(cv->Map(g.ctx, g.ps_cb, 0, GPU_MAP_WRITE_DISCARD, 0, &m)))
			return 0;
		x = (float *)m.pData;
		x[0] = x[1] = x[2] = x[3] = 0.0f;
		cv->Unmap(g.ctx, g.ps_cb, 0);
	}
	{
		UINT stride = sizeof(struct GpuVert), offset = 0;
		GpuViewport vp;
		void *rtvs[1];

		rtvs[0] = g.rtv;
		vp.TopLeftX = vp.TopLeftY = 0.0f;
		vp.Width = (float)g.bb_w;
		vp.Height = (float)g.bb_h;
		vp.MinDepth = 0.0f;
		vp.MaxDepth = 1.0f;
		cv->OMSetRenderTargets(g.ctx, 1, rtvs, NULL);
		cv->RSSetViewports(g.ctx, 1, &vp);
		cv->IASetInputLayout(g.ctx, g.layout);
		cv->IASetPrimitiveTopology(g.ctx, GPU_TOPO_TRILIST);
		cv->IASetVertexBuffers(g.ctx, 0, 1, &g.vb, &stride, &offset);
		cv->VSSetShader(g.ctx, g.vs, NULL, 0);
		cv->VSSetConstantBuffers(g.ctx, 0, 1, &g.cb);
		cv->PSSetShader(g.ctx, g.ps, NULL, 0);
		cv->PSSetConstantBuffers(g.ctx, 0, 1, &g.ps_cb);
		cv->PSSetShaderResources(g.ctx, 0, 1, &srv);
		/* Linear, because the window is rarely the source's size and this
		 * is a rescale. Point would alias the downscale into shimmer. */
		cv->PSSetSamplers(g.ctx, 0, 1, &g.smp_linear);
		cv->OMSetBlendState(g.ctx, g.blend_off, NULL, 0xffffffffu);
		cv->RSSetState(g.ctx, g.rs_plain);
		cv->Draw(g.ctx, 6, 0);
	}
	return 1;
}

/* ------------------------------------------------------------------
 * Stage 2: the draws themselves.
 *
 * The address-space argument, which is the reason any of this exists: in the
 * heavy scene the arena holds 641 MB of texture payload inside a 2 GB process,
 * and a save needs roughly 275 MB in one contiguous piece. Mirroring those
 * textures onto the adapter and letting go of the CPU copies is worth about
 * three times what the driver costs to load.
 *
 * Everything renders into an offscreen target of the game's own size rather
 * than straight into the swapchain. The window is not reliably the same size as
 * the game's render target, the readback path needs a surface to copy out of,
 * and keeping the two separate means the rescale happens in exactly one place -
 * the same fullscreen quad stage 1 already uses.
 * ------------------------------------------------------------------ */

typedef struct GpuTex {
	void *tex, *srv;
	int w, h;
	unsigned gen; /* content generation last uploaded */
} GpuTex;

/* The caller owns the slot and hangs it on its own resource object, which keeps
 * this file from needing to know what a resource is. */
int gpu_tex_sync(void **slot, const uint32_t *pixels, int w, int h, unsigned gen)
{
	GpuDeviceVtbl *dv;
	GpuCtxVtbl *cv;
	GpuTex *t;

	if (!g.up || !slot || !pixels || w <= 0 || h <= 0)
		return 0;
	dv = (GpuDeviceVtbl *)((GpuUnk *)g.dev)->v;
	cv = (GpuCtxVtbl *)((GpuUnk *)g.ctx)->v;
	t = (GpuTex *)*slot;
	if (t && (t->w != w || t->h != h)) {
		GPU_REL(t->srv);
		GPU_REL(t->tex);
		HeapFree(GetProcessHeap(), 0, t);
		t = NULL;
		*slot = NULL;
	}
	if (!t) {
		GpuTex2DDesc td;

		t = (GpuTex *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*t));
		if (!t)
			return 0;
		memset(&td, 0, sizeof(td));
		td.Width = (UINT)w;
		td.Height = (UINT)h;
		td.MipLevels = 1;
		td.ArraySize = 1;
		td.Format = GPU_FMT_B8G8R8A8;
		td.SampleDesc.Count = 1;
		/* Default rather than dynamic: most of these are uploaded once and
		 * sampled for the rest of the session, and a default texture is the
		 * one the driver is free to keep entirely in video memory. A dynamic
		 * one has to stay mappable, which is the CPU-side copy this whole
		 * exercise is trying to be rid of. */
		td.Usage = GPU_USAGE_DEFAULT;
		td.BindFlags = GPU_BIND_SRV;
		if (FAILED(dv->CreateTexture2D(g.dev, &td, NULL, &t->tex)) ||
		    FAILED(dv->CreateShaderResourceView(g.dev, t->tex, NULL, &t->srv))) {
			GPU_REL(t->srv);
			GPU_REL(t->tex);
			HeapFree(GetProcessHeap(), 0, t);
			return 0;
		}
		t->w = w;
		t->h = h;
		t->gen = gen - 1; /* force the first upload */
		*slot = t;
	}
	if (t->gen != gen) {
		cv->UpdateSubresource(g.ctx, t->tex, 0, NULL, pixels, (UINT)w * 4, 0);
		t->gen = gen;
		g.uploads++;
	}
	return 1;
}

void gpu_tex_drop(void **slot)
{
	GpuTex *t = slot ? (GpuTex *)*slot : NULL;

	if (!t)
		return;
	GPU_REL(t->srv);
	GPU_REL(t->tex);
	HeapFree(GetProcessHeap(), 0, t);
	*slot = NULL;
}

/* The offscreen target the game's frame is built in. */
static int gpu_rt_ensure(int w, int h)
{
	GpuDeviceVtbl *dv = (GpuDeviceVtbl *)((GpuUnk *)g.dev)->v;
	GpuTex2DDesc td;

	if (g.rt_tex && g.rt_w == w && g.rt_h == h)
		return 1;
	GPU_REL(g.rt_srv);
	GPU_REL(g.rt_rtv);
	GPU_REL(g.rt_tex);
	GPU_REL(g.rt_stage);
	memset(&td, 0, sizeof(td));
	td.Width = (UINT)w;
	td.Height = (UINT)h;
	td.MipLevels = 1;
	td.ArraySize = 1;
	td.Format = GPU_FMT_B8G8R8A8;
	td.SampleDesc.Count = 1;
	td.Usage = GPU_USAGE_DEFAULT;
	td.BindFlags = GPU_BIND_RTV | GPU_BIND_SRV;
	if (FAILED(dv->CreateTexture2D(g.dev, &td, NULL, &g.rt_tex)) ||
	    FAILED(dv->CreateRenderTargetView(g.dev, g.rt_tex, NULL, &g.rt_rtv)) ||
	    FAILED(dv->CreateShaderResourceView(g.dev, g.rt_tex, NULL, &g.rt_srv))) {
		GPU_REL(g.rt_srv);
		GPU_REL(g.rt_rtv);
		GPU_REL(g.rt_tex);
		return 0;
	}
	g.rt_w = w;
	g.rt_h = h;
	return 1;
}

/* Opens the frame. The size is the game's render target, not the window. */
int gpu_frame_begin(int w, int h, int clear, uint32_t argb)
{
	GpuCtxVtbl *cv;
	GpuViewport vp;
	void *rtvs[1];

	if (!g.up || !gpu_rt_ensure(w, h))
		return 0;
	cv = (GpuCtxVtbl *)((GpuUnk *)g.ctx)->v;
	rtvs[0] = g.rt_rtv;
	cv->OMSetRenderTargets(g.ctx, 1, rtvs, NULL);
	vp.TopLeftX = vp.TopLeftY = 0.0f;
	vp.Width = (float)w;
	vp.Height = (float)h;
	vp.MinDepth = 0.0f;
	vp.MaxDepth = 1.0f;
	cv->RSSetViewports(g.ctx, 1, &vp);
	cv->IASetInputLayout(g.ctx, g.layout);
	cv->IASetPrimitiveTopology(g.ctx, GPU_TOPO_TRILIST);
	cv->VSSetShader(g.ctx, g.vs, NULL, 0);
	cv->VSSetConstantBuffers(g.ctx, 0, 1, &g.cb);
	cv->PSSetShader(g.ctx, g.ps, NULL, 0);
	cv->PSSetConstantBuffers(g.ctx, 0, 1, &g.ps_cb);
	{
		GpuMapped m;
		float *x;

		if (FAILED(cv->Map(g.ctx, g.cb, 0, GPU_MAP_WRITE_DISCARD, 0, &m)))
			return 0;
		x = (float *)m.pData;
		x[0] = 2.0f / (float)w;
		x[1] = -2.0f / (float)h;
		x[2] = -1.0f;
		x[3] = 1.0f;
		cv->Unmap(g.ctx, g.cb, 0);
	}
	if (clear) {
		FLOAT c[4];

		/* The game's colour is 0xAARRGGBB and the target is linear-free
		 * UNORM, so this is a plain rescale with no gamma in it. */
		c[0] = (float)((argb >> 16) & 255) / 255.0f;
		c[1] = (float)((argb >> 8) & 255) / 255.0f;
		c[2] = (float)(argb & 255) / 255.0f;
		c[3] = (float)((argb >> 24) & 255) / 255.0f;
		cv->ClearRenderTargetView(g.ctx, g.rt_rtv, c);
	}
	return 1;
}

/* D3DBLEND -> D3D11_BLEND. Only the factors this game has ever asked for; an
 * unknown one returns zero so the caller can decline the draw rather than
 * silently substitute a blend that looks nearly right. */
static UINT gpu_factor(int d3d9)
{
	switch (d3d9) {
	case 1: return GPU_BLEND_ZERO;
	case 2: return GPU_BLEND_ONE;
	case 3: return GPU_BLEND_SRC_COLOR;
	case 4: return 4; /* INV_SRC_COLOR */
	case 5: return GPU_BLEND_SRC_ALPHA;
	case 6: return GPU_BLEND_INV_SRC_ALPHA;
	case 7: return 7; /* DEST_ALPHA */
	case 8: return 8; /* INV_DEST_ALPHA */
	case 9: return 9; /* DEST_COLOR */
	case 10: return 10; /* INV_DEST_COLOR */
	default: return 0;
	}
}

/* One draw. Returns 0 if it cannot be honoured exactly, and the caller must
 * then decline the whole frame rather than send this one elsewhere: a frame
 * split between two backends is two half-drawn images, not one picture. */
int gpu_draw(const SwTri *tris, int n, void *texslot, const SwState *st)
{
	GpuCtxVtbl *cv;
	GpuTex *t = (GpuTex *)texslot;
	int i;

	if (!g.up || !tris || n <= 0 || n * 3 > GPU_VB_VERTS)
		return 0;
	if (!t || !t->srv)
		return 0; /* untextured draws are not in this game's mix */
	cv = (GpuCtxVtbl *)((GpuUnk *)g.ctx)->v;

	{
		GpuMapped m;
		struct GpuVert *vp;

		if (FAILED(cv->Map(g.ctx, g.vb, 0, GPU_MAP_WRITE_DISCARD, 0, &m)))
			return 0;
		vp = (struct GpuVert *)m.pData;
		for (i = 0; i < n; i++) {
			const SwVert *v[3];
			int k;

			v[0] = &tris[i].a;
			v[1] = &tris[i].b;
			v[2] = &tris[i].c;
			for (k = 0; k < 3; k++) {
				struct GpuVert *o = &vp[i * 3 + k];
				uint32_t c = v[k]->color;

				/* Half a pixel left and up. The rasteriser samples
				 * pixel centres from integer corners; D3D's raster
				 * rule puts the centre at +0.5, so without this every
				 * sprite lands half a pixel down and right of where
				 * the software path puts it. */
				o->x = v[k]->x - 0.5f;
				o->y = v[k]->y - 0.5f;
				o->u = v[k]->u;
				o->v = v[k]->v;
				/* D3DCOLOR is 0xAARRGGBB. */
				o->r = (float)((c >> 16) & 255) / 255.0f;
				o->g = (float)((c >> 8) & 255) / 255.0f;
				o->b = (float)(c & 255) / 255.0f;
				o->a = (float)((c >> 24) & 255) / 255.0f;
			}
		}
		cv->Unmap(g.ctx, g.vb, 0);
	}
	{
		GpuMapped m;
		float *x;
		float sharpen = (st->alpha_sharpen > 0) ? (float)st->alpha_sharpen / 256.0f
						       : 0.0f;

		if (FAILED(cv->Map(g.ctx, g.ps_cb, 0, GPU_MAP_WRITE_DISCARD, 0, &m)))
			return 0;
		x = (float *)m.pData;
		/* Only GREATER and GREATEREQUAL appear, and the shader's clip is
		 * the same test for both to within one quantisation step of an
		 * eight-bit alpha. Anything else declines below. */
		x[0] = st->alpha_test ? (float)st->alpha_ref / 255.0f : 0.0f;
		x[1] = sharpen;
		x[2] = st->mul_identity ? 1.0f : 0.0f;
		x[3] = 0.0f;
		cv->Unmap(g.ctx, g.ps_cb, 0);
	}
	if (st->alpha_test && st->alpha_func != 0 && st->alpha_func != 7 &&
	    st->alpha_func != 6)
		return 0; /* ALWAYS, GREATER, GREATEREQUAL only */
	{
		void *bs;

		if (!st->blend_enable) {
			bs = g.blend_off;
		} else if (st->blend_op == 1 /* ADD */) {
			UINT s = gpu_factor(st->src_blend), d = gpu_factor(st->dst_blend);

			if (!s || !d)
				return 0;
			if (s == GPU_BLEND_SRC_ALPHA && d == GPU_BLEND_INV_SRC_ALPHA)
				bs = g.blend_over;
			else if (s == GPU_BLEND_SRC_ALPHA && d == GPU_BLEND_ONE)
				bs = g.blend_add;
			else if (s == GPU_BLEND_ZERO && d == GPU_BLEND_SRC_COLOR)
				bs = g.blend_mod;
			else
				return 0;
		} else if (st->blend_op == 3 /* REVSUBTRACT */) {
			UINT s = gpu_factor(st->src_blend), d = gpu_factor(st->dst_blend);

			if (s != GPU_BLEND_SRC_ALPHA || d != GPU_BLEND_ONE)
				return 0;
			bs = g.blend_rsub;
		} else {
			return 0;
		}
		cv->OMSetBlendState(g.ctx, bs, NULL, 0xffffffffu);
	}
	{
		RECT sc;
		void *rs = st->scissor_enable ? g.rs_scissor : g.rs_plain;

		if (st->scissor_enable) {
			sc.left = st->scissor_x0;
			sc.top = st->scissor_y0;
			sc.right = st->scissor_x1;
			sc.bottom = st->scissor_y1;
			cv->RSSetScissorRects(g.ctx, 1, &sc);
		}
		cv->RSSetState(g.ctx, rs);
	}
	{
		UINT stride = sizeof(struct GpuVert), offset = 0;
		void *smp = st->bilinear ? g.smp_linear : g.smp_point;

		cv->IASetVertexBuffers(g.ctx, 0, 1, &g.vb, &stride, &offset);
		cv->PSSetShaderResources(g.ctx, 0, 1, &t->srv);
		cv->PSSetSamplers(g.ctx, 0, 1, &smp);
		cv->Draw(g.ctx, (UINT)(n * 3), 0);
	}
	g.draws++;
	g.verts += (unsigned)(n * 3);
	return 1;
}

/* Closes the frame: rescale the game's target onto the window and show it. */
int gpu_frame_end(void)
{
	GpuSwapVtbl *sv;

	if (!g.up || !g.rt_srv)
		return 0;
	if (!gpu_blit(g.rt_srv))
		return 0;
	sv = (GpuSwapVtbl *)((GpuUnk *)g.swap)->v;
	sv->Present(g.swap, 1, 0);
	if (++g.frames == 1)
		gpu_say("gpu: first frame drawn and presented entirely on the adapter");
	return 1;
}

/* The rendered frame, back in system memory, for the game's own readback. */
int gpu_readback(uint32_t *dst, unsigned dst_pitch, int w, int h)
{
	GpuDeviceVtbl *dv;
	GpuCtxVtbl *cv;
	GpuMapped m;
	int y;

	if (!g.up || !g.rt_tex || !dst || w <= 0 || h <= 0)
		return 0;
	dv = (GpuDeviceVtbl *)((GpuUnk *)g.dev)->v;
	cv = (GpuCtxVtbl *)((GpuUnk *)g.ctx)->v;
	if (!g.rt_stage) {
		GpuTex2DDesc td;

		memset(&td, 0, sizeof(td));
		td.Width = (UINT)g.rt_w;
		td.Height = (UINT)g.rt_h;
		td.MipLevels = 1;
		td.ArraySize = 1;
		td.Format = GPU_FMT_B8G8R8A8;
		td.SampleDesc.Count = 1;
		td.Usage = 3; /* STAGING */
		td.CPUAccessFlags = 0x20000; /* READ */
		if (FAILED(dv->CreateTexture2D(g.dev, &td, NULL, &g.rt_stage)))
			return 0;
	}
	/* A full stall on the GPU, which is why this is not on the frame path:
	 * the game asks for it at scene transitions, a few times a session. */
	cv->CopyResource(g.ctx, g.rt_stage, g.rt_tex);
	if (FAILED(cv->Map(g.ctx, g.rt_stage, 0, 1 /* READ */, 0, &m)))
		return 0;
	if (w > g.rt_w)
		w = g.rt_w;
	if (h > g.rt_h)
		h = g.rt_h;
	for (y = 0; y < h; y++)
		memcpy((char *)dst + (size_t)y * dst_pitch,
		       (const char *)m.pData + (size_t)y * m.RowPitch, (size_t)w * 4);
	cv->Unmap(g.ctx, g.rt_stage, 0);
	return 1;
}

/* Stands the adapter down for the length of a save, and blacks the window.
 *
 * The audio stack is already treated this way - dsh_quiet and xa2_sw_park run
 * before suspend_all rather than during it, on the reasoning that nothing
 * should be mid-operation for the whole window rather than merely for the copy.
 * The adapter has the stronger claim: suspending threads stops ours, and the
 * GPU is not one of ours. It carries on with whatever is queued, and the video
 * memory manager stays free to move the mappings underneath a copy that is
 * reading them.
 *
 * Three steps, in this order. Unbind everything, so no draw still references a
 * texture. Clear to black and present, which both gives the save a window that
 * is not half a frame and guarantees the queued work ahead of it has been
 * submitted. Then block until the adapter reports that work finished, because
 * submitting is not the same as being done and only the second one makes the
 * mappings quiet.
 */
void gpu_park(int on)
{
	GpuDeviceVtbl *dv;
	GpuCtxVtbl *cv;
	GpuSwapVtbl *sv;
	static const FLOAT black[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
	void *none[1];
	int spins;
	LARGE_INTEGER t0, t1, pf;
	double ms;

	if (!g.up || !on)
		return;
	dv = (GpuDeviceVtbl *)((GpuUnk *)g.dev)->v;
	cv = (GpuCtxVtbl *)((GpuUnk *)g.ctx)->v;
	sv = (GpuSwapVtbl *)((GpuUnk *)g.swap)->v;

	none[0] = NULL;
	cv->PSSetShaderResources(g.ctx, 0, 1, none);
	if (g.rt_rtv)
		cv->ClearRenderTargetView(g.ctx, g.rt_rtv, black);
	if (g.rtv) {
		void *rtvs[1];

		rtvs[0] = g.rtv;
		cv->OMSetRenderTargets(g.ctx, 1, rtvs, NULL);
		cv->ClearRenderTargetView(g.ctx, g.rtv, black);
	}
	/* No vsync wait. The save is already holding the process still and a
	 * sixteen millisecond sleep here would be added to every save for the
	 * sake of a frame nobody is going to look at. */
	sv->Present(g.swap, 0, 0);

	if (!g.query) {
		struct {
			UINT Query, MiscFlags;
		} qd;

		qd.Query = 0; /* D3D11_QUERY_EVENT */
		qd.MiscFlags = 0;
		if (FAILED(dv->CreateQuery(g.dev, &qd, &g.query)))
			g.query = NULL;
	}
	if (!g.query) {
		/* Without a fence there is no way to know the adapter is finished,
		 * so say so rather than let a save believe it was made safe. */
		gpu_say("gpu: parked without a fence - no event query, so the adapter "
			"may still be working while the snapshot is copied");
		return;
	}
	cv->End(g.ctx, g.query);
	/* Bounded, and timed. This runs before the save samples its first clock,
	 * so whatever it costs lands in the total without appearing in any phase -
	 * which is exactly the kind of cost that gets blamed on something else. */
	QueryPerformanceCounter(&t0);
	QueryPerformanceFrequency(&pf);
	for (spins = 0; spins < 2000000; spins++) {
		BOOL done = FALSE;

		if (cv->GetData(g.ctx, g.query, &done, sizeof(done), 0) == S_OK) {
			QueryPerformanceCounter(&t1);
			ms = (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)pf.QuadPart;
			/* Only when it is worth knowing about. An idle adapter answers
			 * immediately and does not need a line every save. */
			if (ms > 5.0)
				gpu_say("gpu: park waited %.1f ms over %d poll(s) for the "
					"adapter to go idle",
					ms, spins + 1);
			return;
		}
	}
	QueryPerformanceCounter(&t1);
	gpu_say("gpu: parked, but the adapter did not report idle within 2000000 polls "
		"(%.1f ms) - the snapshot goes ahead anyway",
		(double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)pf.QuadPart);
}

void gpu_prof_take(unsigned *uploads, unsigned *draws, unsigned *verts)
{
	*uploads = g.uploads;
	*draws = g.draws;
	*verts = g.verts;
	g.uploads = g.draws = g.verts = 0;
}

int gpu_is_up(void)
{
	return g.up;
}

void gpu_shutdown(void)
{
	if (g.up || g.dev)
		gpu_teardown();
}
