/* The block headers of the Windows heaps gameheap pins in the D3D9SW_GHHEAPS
 * slots, for the merge's pointer shift to leave alone. Placed after all other
 * code, so adding it moved none of the code a save's frames return into. */
#include <windows.h>
#include <stdint.h>

int gameheap_slot_heap(uintptr_t h);
void savestate_log_line(const char *s);

static const char said[] __attribute__((section(".zzrdata"))) =
	"  merge: %d Windows heap header(s) in the pinned heap slots kept out of the pointer "
	"shift\n";

/* Each pinned heap's _HEAP header and every block header, appended to out from
 * out[n] as lo/hi pairs; returns the pair count of all of out. Windows XORs the
 * headers with a key of the heap's own, so they read as random words, and one
 * that falls where a module sat on the saving machine was shifted like a
 * pointer. x86 _HEAP: FirstEntry +0x24, LastValidEntry +0x28, EncodeFlagMask
 * +0x4C, Encoding +0x50, 0x258 bytes in all. */
__attribute__((section(".zztext"))) int gameheap_nt_headers(uintptr_t *out, int n, int max)
{
	HANDLE hs[64];
	DWORD nh = GetProcessHeaps(64, hs), i;
	int first = n;

	if (nh > 64)
		nh = 64;
	for (i = 0; i < nh && n + 2 <= max; i++) {
		uintptr_t h = (uintptr_t)hs[i], e, last, end = 0;
		MEMORY_BASIC_INFORMATION mb;
		DWORD key;

		if (!gameheap_slot_heap(h) || !VirtualQuery((void *)h, &mb, sizeof(mb)) ||
		    mb.State != MEM_COMMIT || mb.RegionSize < 0x258 ||
		    *(const DWORD *)(h + 8) != 0xFFEEFFEEu)
			continue;
		out[n++] = h;
		out[n++] = h + 0x258;
		e = *(const uintptr_t *)(h + 0x24);
		last = *(const uintptr_t *)(h + 0x28);
		key = *(const DWORD *)(h + 0x4C) ? *(const DWORD *)(h + 0x50) : 0;
		while (e >= h + 0x258 && e + 8 <= last && n + 2 <= max) {
			DWORD w;

			if (e + 8 > end) {
				if (!VirtualQuery((void *)e, &mb, sizeof(mb)))
					break;
				end = (uintptr_t)mb.BaseAddress + mb.RegionSize;
				if (mb.State != MEM_COMMIT) {
					e = end;
					end = 0;
					continue;
				}
				if (e + 8 > end)
					break;
			}
			w = *(const DWORD *)e ^ key;
			if (!(w & 0xFFFF) || ((w ^ (w >> 8) ^ (w >> 16) ^ (w >> 24)) & 0xFF))
				break;
			out[n++] = e;
			out[n++] = e + 8;
			e += (uintptr_t)(w & 0xFFFF) * 8;
		}
	}
	if (n > first) {
		char b[160];

		wsprintfA(b, said, (n - first) / 2);
		savestate_log_line(b);
	}
	return n / 2;
}
