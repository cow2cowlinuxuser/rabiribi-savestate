/* Can this process talk to a real GPU while we are standing in for D3D11?
 *
 * Two questions, and the second is the one that decides the project.
 *
 * The first is ordinary: load Windows' own d3d11.dll by absolute path, ask it
 * for a hardware device, and see what comes back. A 32-bit process on a machine
 * with a working driver should manage that.
 *
 * The second is the obstacle. The real d3d11.dll imports from dxgi.dll, and the
 * loader resolves a new module's imports by base name against everything already
 * loaded. Our own dxgi.dll is loaded under that name and exports three factory
 * functions, which is nowhere near what d3d11.dll asks of it. If the loader
 * binds the real d3d11 to our trampoline, device creation fails or the process
 * dies, and a GPU backend needs our dxgi.dll to become a full proxy for the
 * system one before anything else can happen.
 *
 * Reasoning about which way the loader jumps is less reliable than asking it, so
 * this asks. Nothing here touches the render path; it creates a device, reports
 * what it found, and releases it.
 */
#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#include <windows.h>
#include <stdio.h>

unsigned savestate_getenv(const char *name, char *buf, unsigned cap);

/* Declared locally rather than pulled from d3d11.h: this file must describe the
 * real API, and the project's own headers describe our imitation of it. */
typedef enum { GP_HARDWARE = 1, GP_WARP = 5 } GpDriver;

typedef HRESULT(WINAPI *PFN_D3D11CreateDevice)(void *adapter, int driver, HMODULE sw, UINT flags,
					       const UINT *levels, UINT nlevels, UINT sdk,
					       void **dev, UINT *got, void **ctx);

/* Only the slots this file calls, in their real vtable order. Reaching past the
 * last one named here would be reading a layout nobody checked. */
struct GpUnkVtbl {
	HRESULT(WINAPI *QueryInterface)(void *, const GUID *, void **);
	ULONG(WINAPI *AddRef)(void *);
	ULONG(WINAPI *Release)(void *);
};
struct GpUnk {
	struct GpUnkVtbl *v;
};

/* IDXGIDevice. Every slot up to the one being called has to be named, because
 * the call is made by position: IDXGIObject contributes four methods between
 * IUnknown and anything of DXGI's own, and skipping them lands on
 * SetPrivateData with another function's arguments. */
struct GpDxgiDeviceVtbl {
	struct GpUnkVtbl unk;
	/* IDXGIObject */
	HRESULT(WINAPI *SetPrivateData)(void *, const GUID *, UINT, const void *);
	HRESULT(WINAPI *SetPrivateDataInterface)(void *, const GUID *, const void *);
	HRESULT(WINAPI *GetPrivateData)(void *, const GUID *, UINT *, void *);
	HRESULT(WINAPI *GetParent)(void *, const GUID *, void **);
	/* IDXGIDevice. GetAdapter says what is wanted here more directly than
	 * GetParent with an interface id, and is one slot further on. */
	HRESULT(WINAPI *GetAdapter)(void *, void **);
};

/* IDXGIAdapter::GetDesc sits after IDXGIObject's four and IDXGIAdapter's
 * EnumOutputs. DXGI_ADAPTER_DESC is 1 wide string plus five scalars plus a LUID.
 */
struct GpAdapterDesc {
	WCHAR Description[128];
	UINT VendorId, DeviceId, SubSysId, Revision;
	SIZE_T DedicatedVideoMemory, DedicatedSystemMemory, SharedSystemMemory;
	LUID AdapterLuid;
};

struct GpAdapterVtbl {
	struct GpUnkVtbl unk;
	/* IDXGIObject */
	HRESULT(WINAPI *SetPrivateData)(void *, const GUID *, UINT, const void *);
	HRESULT(WINAPI *SetPrivateDataInterface)(void *, const GUID *, const void *);
	HRESULT(WINAPI *GetPrivateData)(void *, const GUID *, UINT *, void *);
	HRESULT(WINAPI *GetParent)(void *, const GUID *, void **);
	/* IDXGIAdapter */
	HRESULT(WINAPI *EnumOutputs)(void *, UINT, void **);
	HRESULT(WINAPI *GetDesc)(void *, struct GpAdapterDesc *);
};

static const GUID GP_IID_IDXGIDevice = { 0x54ec77fa,
					 0x1377,
					 0x44e6,
					 { 0x8c, 0x32, 0x88, 0xfd, 0x5f, 0x44, 0xc8, 0x4c } };

/* Raised only around a call into the real D3D11, so the one function it imports
 * from dxgi.dll can be answered with the real one. Nothing else is affected:
 * outside this window the game keeps getting our software factory. */
static HMODULE gp_system_dll(const char *leaf);

static volatile LONG g_want_real;
static void *g_real_factory;

int gpuprobe_wants_real_factory(void)
{
	return g_want_real != 0;
}

void *gpuprobe_real_factory_fn(void)
{
	return g_real_factory;
}

/* Raise before a call into the real D3D11 and lower the moment it returns.
 *
 * Process-wide rather than per-thread, and that is a real limit: it is correct
 * while only one thread creates devices and the game is not calling DXGI at the
 * same moment, which holds for the probe and for a backend that initialises on
 * the present thread. Anything that creates devices concurrently needs this to
 * become thread-local first.
 */
int gpuprobe_hold_real_factory(int on)
{
	if (on && !g_real_factory) {
		HMODULE dxgi = gp_system_dll("dxgi.dll");

		if (dxgi)
			g_real_factory = (void *)GetProcAddress(dxgi, "CreateDXGIFactory2");
		if (!g_real_factory)
			return 0;
	}
	InterlockedExchange(&g_want_real, on ? 1 : 0);
	return 1;
}

static void gp_log(void (*sink)(const char *), const char *fmt, ...)
{
	char line[512];
	va_list ap;

	va_start(ap, fmt);
	_vsnprintf(line, sizeof(line), fmt, ap);
	va_end(ap);
	line[sizeof(line) - 1] = 0;
	sink(line);
}

/* Address space in use, in MB. The only figure that matters here is the
 * difference between two of these, so the absolute value is incidental. */
static unsigned gp_va_used(void)
{
	MEMORYSTATUSEX ms;

	ms.dwLength = sizeof(ms);
	if (!GlobalMemoryStatusEx(&ms))
		return 0;
	return (unsigned)((ms.ullTotalVirtual - ms.ullAvailVirtual) >> 20);
}

static HMODULE gp_system_dll(const char *leaf)
{
	char path[MAX_PATH];
	UINT n = GetSystemDirectoryA(path, MAX_PATH);

	/* GetSystemDirectory in a 32-bit process under WOW64 already answers
	 * SysWOW64, which is the copy this process can load. Naming SysWOW64
	 * outright would be wrong on a 32-bit Windows. */
	if (!n || n >= MAX_PATH - 32)
		return NULL;
	_snprintf(path + n, MAX_PATH - n, "\\%s", leaf);
	return LoadLibraryA(path);
}

/* Runs once. Returns 1 if a hardware device was created. */
int gpuprobe_run(void (*sink)(const char *))
{
	char buf[8];
	HMODULE ours_dxgi, real_dxgi, ours_d3d11, real_d3d11;
	PFN_D3D11CreateDevice create;
	UINT level = 0;
	void *dev = NULL, *ctx = NULL;
	HRESULT hr;
	unsigned va_entry, va_loaded, va_live, va_after;

	if (!savestate_getenv("D3D11SW_GPUPROBE", buf, sizeof(buf)) || buf[0] != '1')
		return 0;

	va_entry = gp_va_used();
	ours_dxgi = GetModuleHandleA("dxgi.dll");
	ours_d3d11 = GetModuleHandleA("d3d11.dll");

	/* Load the system dxgi first and by full path. If the loader hands back
	 * the module already loaded under that base name, it has told us the
	 * answer to the second question before we even reach d3d11. */
	real_dxgi = gp_system_dll("dxgi.dll");
	gp_log(sink,
	       "gpuprobe: ours dxgi=%p d3d11=%p | system dxgi=%p -> %s",
	       (void *)ours_dxgi, (void *)ours_d3d11, (void *)real_dxgi,
	       !real_dxgi		    ? "WOULD NOT LOAD"
	       : real_dxgi == ours_dxgi	    ? "THE SAME MODULE - the loader matched our "
					      "trampoline by name, so the real d3d11 would "
					      "bind to it and a proxy is required first"
					    : "a distinct module, so both can be resident at once");

	/* The single import the real d3d11 makes of dxgi. Resolved from the
	 * system module we just loaded by path, not from whatever the loader
	 * would pick by name - picking by name is the whole problem. */
	if (real_dxgi)
		g_real_factory = (void *)GetProcAddress(real_dxgi, "CreateDXGIFactory2");
	if (!g_real_factory) {
		gp_log(sink, "gpuprobe: system dxgi has no CreateDXGIFactory2 to borrow");
		return 0;
	}

	real_d3d11 = gp_system_dll("d3d11.dll");
	if (!real_d3d11 || real_d3d11 == ours_d3d11) {
		gp_log(sink,
		       "gpuprobe: system d3d11=%p %s - no hardware path from here",
		       (void *)real_d3d11,
		       real_d3d11 ? "is our own module" : "would not load");
		return 0;
	}
	create = (PFN_D3D11CreateDevice)GetProcAddress(real_d3d11, "D3D11CreateDevice");
	if (!create) {
		gp_log(sink, "gpuprobe: system d3d11 has no D3D11CreateDevice");
		return 0;
	}

	/* No swapchain and no debug layer: this asks whether a driver will talk
	 * to us, not whether we can present yet. */
	/* Both system modules are resident by here, so anything beyond this point
	 * is the device itself rather than the cost of loading code. */
	va_loaded = gp_va_used();

	InterlockedExchange(&g_want_real, 1);
	hr = create(NULL, GP_HARDWARE, NULL, 0, NULL, 0, 7, &dev, &level, &ctx);
	if (FAILED(hr) || !dev) {
		gp_log(sink,
		       "gpuprobe: hardware device REFUSED hr=%08lX. Retrying WARP to "
		       "separate a driver problem from a loader one",
		       (unsigned long)hr);
		hr = create(NULL, GP_WARP, NULL, 0, NULL, 0, 7, &dev, &level, &ctx);
		if (FAILED(hr) || !dev) {
			InterlockedExchange(&g_want_real, 0);
			gp_log(sink, "gpuprobe: WARP refused too, hr=%08lX", (unsigned long)hr);
			return 0;
		}
		gp_log(sink,
		       "gpuprobe: WARP came up at feature level %u.%u but hardware did "
		       "not, so the loader is fine and the driver is the problem",
		       (unsigned)(level >> 12), (unsigned)((level >> 8) & 0xf));
	} else {
		gp_log(sink, "gpuprobe: hardware device CREATED at feature level %u.%u",
		       (unsigned)(level >> 12), (unsigned)((level >> 8) & 0xf));
	}
	/* Lowered the moment creation returns. Anything the game asks for after
	 * this point must still get the software factory. */
	InterlockedExchange(&g_want_real, 0);

	{
		struct GpUnk *d = (struct GpUnk *)dev;
		struct GpUnk *dxdev = NULL;

		if (SUCCEEDED(d->v->QueryInterface(d, &GP_IID_IDXGIDevice, (void **)&dxdev)) &&
		    dxdev) {
			struct GpDxgiDeviceVtbl *dv = (struct GpDxgiDeviceVtbl *)dxdev->v;
			struct GpUnk *ad = NULL;

			if (SUCCEEDED(dv->GetAdapter(dxdev, (void **)&ad)) && ad) {
				struct GpAdapterVtbl *av = (struct GpAdapterVtbl *)ad->v;
				struct GpAdapterDesc desc;

				memset(&desc, 0, sizeof(desc));
				if (SUCCEEDED(av->GetDesc(ad, &desc))) {
					char name[160];

					WideCharToMultiByte(CP_ACP, 0, desc.Description, -1, name,
							    sizeof(name), NULL, NULL);
					name[sizeof(name) - 1] = 0;
					gp_log(sink,
					       "gpuprobe: adapter \"%s\" vendor=%04X device=%04X, "
					       "%u MB dedicated video, %u MB shared. That video "
					       "memory is the point: textures living there are "
					       "not in this process's 2 GB",
					       name, desc.VendorId, desc.DeviceId,
					       (unsigned)(desc.DedicatedVideoMemory >> 20),
					       (unsigned)(desc.SharedSystemMemory >> 20));
				}
				ad->v->Release(ad);
			}
			dxdev->v->Release(dxdev);
		}
	}

	va_live = gp_va_used();

	if (ctx)
		((struct GpUnk *)ctx)->v->Release(ctx);
	((struct GpUnk *)dev)->v->Release(dev);
	va_after = gp_va_used();

	/* The whole question in one line. A single reading with the device up
	 * says nothing, because it cannot be told apart from the address space
	 * the process was using anyway; only the differences are evidence.
	 *
	 * The cost is what a live driver takes. The residue is what it does not
	 * give back on release, which matters because a backend would hold the
	 * device for the life of the process and never get to find out. Against
	 * both of those sits the arena, and the trade is only worth making if
	 * what we stop holding is larger than what the driver takes. */
	gp_log(sink,
	       "gpuprobe: address space %u MB at entry, %u MB with the modules loaded, "
	       "%u MB with a live device, %u MB after release. Driver cost %u MB, "
	       "residue %u MB",
	       va_entry, va_loaded, va_live, va_after, va_live - va_loaded,
	       va_after - va_entry);
	gp_log(sink, "gpuprobe: device released, nothing kept");
	return 1;
}
