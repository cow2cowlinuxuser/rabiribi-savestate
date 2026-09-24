/* Recover LoadLibrary / GetProcAddress arguments from a decrypted image dump.
 *
 * rabiribi.exe is ciphertext on disk. savestate_dump_image writes the image
 * after the Steam stub has decrypted it, with ImageBase set to the base the
 * process had and each section's PointerToRawData set to its VirtualAddress,
 * so file offset equals RVA.
 *
 * A debugger is the wrong instrument for this. The game imports
 * IsDebuggerPresent, the instructions do not exist until the stub has run,
 * and a breakpoint planted on the file has nothing real to bind to. The call
 * sites are ordinary FF 15 [IAT] once the image is plaintext, and the DLL and
 * export names are immediates in those calls. This reads them.
 *
 * What it cannot see is a name computed into a register. Those sites are
 * printed as unresolved so a later session log has a finite list to chase,
 * rather than an open question.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { kMaxPushes = 6, kMaxInsns = 8, kMaxName = 260 };

enum api_id { API_GPA, API_LLA, API_LLW, API_LLEX, API_N };

static const char *api_name[] = {
	"GetProcAddress", "LoadLibraryA", "LoadLibraryW", "LoadLibraryExW"
};

/* Closest push to the call is argument 1 (stdcall, right to left). */
static int api_argc[] = { 2, 1, 1, 3 };
static int api_name_arg[] = { 2, 1, 1, 1 }; /* 1-based */

struct push {
	int kind; /* 0 imm, 1 imm8, 2 mem, 3 reg */
	uint32_t val;
	int reg;
};

struct image {
	uint8_t *b;
	size_t n;
	uint32_t base;
	uint32_t size;
	uint32_t text_rva;
	uint32_t text_end; /* RVA past the section */
};

static int rva_ok(const struct image *im, uint32_t rva, uint32_t need)
{
	return rva + need >= rva && (size_t)rva + need <= im->n && rva < im->size;
}

static const uint8_t *at_rva(const struct image *im, uint32_t rva)
{
	if (!rva_ok(im, rva, 1))
		return NULL;
	return im->b + rva;
}

static int ptr_in(const struct image *im, uint32_t va)
{
	return va >= im->base && va - im->base < im->size;
}

/* Print a literal the immediate points at. Returns 1 if it was a string. */
static int read_literal(const struct image *im, uint32_t va, int wide, char *out, size_t outn)
{
	uint32_t rva;
	size_t i;

	if (!ptr_in(im, va))
		return 0;
	rva = va - im->base;
	if (wide) {
		if (!rva_ok(im, rva, 2))
			return 0;
		for (i = 0; i + 1 < outn && rva_ok(im, rva + (uint32_t)(i * 2), 2); i++) {
			uint16_t c = (uint16_t)im->b[rva + i * 2] |
				     ((uint16_t)im->b[rva + i * 2 + 1] << 8);
			if (c == 0) {
				out[i] = 0;
				return i > 0 && i < 200;
			}
			if (c < 32 || c > 126)
				return 0;
			out[i] = (char)c;
		}
		return 0;
	}
	for (i = 0; i + 1 < outn && rva_ok(im, rva + (uint32_t)i, 1); i++) {
		unsigned char c = im->b[rva + i];
		if (c == 0) {
			out[i] = 0;
			return i > 0 && i < 200;
		}
		if (c < 32 || c > 126)
			return 0;
		out[i] = (char)c;
	}
	return 0;
}

/* Walk backwards from the call, one encoded push at a time. Pushes are
 * stored closest-first, which is argument order. */
static int decode_pushes(const struct image *im, uint32_t call_rva, struct push *p)
{
	uint32_t rva = call_rva;
	int n = 0, steps = 0;

	while (n < kMaxPushes && steps < kMaxInsns && rva > 0) {
		const uint8_t *q;
		steps++;
		if (rva >= 5 && (q = at_rva(im, rva - 5)) && q[0] == 0x68) {
			p[n].kind = 0;
			p[n].val = (uint32_t)q[1] | ((uint32_t)q[2] << 8) |
				   ((uint32_t)q[3] << 16) | ((uint32_t)q[4] << 24);
			n++;
			rva -= 5;
			continue;
		}
		if (rva >= 2 && (q = at_rva(im, rva - 2)) && q[0] == 0x6A) {
			p[n].kind = 1;
			p[n].val = q[1];
			n++;
			rva -= 2;
			continue;
		}
		if (rva >= 6 && (q = at_rva(im, rva - 6)) && q[0] == 0xFF && q[1] == 0x35) {
			p[n].kind = 2;
			p[n].val = (uint32_t)q[2] | ((uint32_t)q[3] << 8) |
				   ((uint32_t)q[4] << 16) | ((uint32_t)q[5] << 24);
			n++;
			rva -= 6;
			continue;
		}
		if (rva >= 1 && (q = at_rva(im, rva - 1)) && q[0] >= 0x50 && q[0] <= 0x57) {
			p[n].kind = 3;
			p[n].reg = q[0] - 0x50;
			n++;
			rva -= 1;
			continue;
		}
		/* mov reg, imm32 sitting under a push reg we already took. */
		if (n > 0 && p[n - 1].kind == 3 && rva >= 5 &&
		    (q = at_rva(im, rva - 5)) && q[0] == (uint8_t)(0xB8 + p[n - 1].reg)) {
			p[n - 1].kind = 0;
			p[n - 1].val = (uint32_t)q[1] | ((uint32_t)q[2] << 8) |
				       ((uint32_t)q[3] << 16) | ((uint32_t)q[4] << 24);
			rva -= 5;
			continue;
		}
		break;
	}
	return n;
}

static const char *regn[] = { "eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi" };

static void describe_push(const struct image *im, const struct push *p, int wide, FILE *out)
{
	char lit[kMaxName];

	if (p->kind == 3) {
		fprintf(out, "reg %s", regn[p->reg]);
		return;
	}
	if (p->kind == 1) {
		fprintf(out, "imm8 %u", p->val);
		return;
	}
	if (p->kind == 2) {
		fprintf(out, "mem %08X", p->val);
		return;
	}
	if (p->val < 0x10000) {
		fprintf(out, "ordinal #%u", p->val);
		return;
	}
	if (read_literal(im, p->val, wide, lit, sizeof(lit))) {
		fprintf(out, "\"%s\"", lit);
		return;
	}
	if (!wide && read_literal(im, p->val, 1, lit, sizeof(lit))) {
		fprintf(out, "L\"%s\"", lit);
		return;
	}
	if (wide && read_literal(im, p->val, 0, lit, sizeof(lit))) {
		fprintf(out, "\"%s\"", lit);
		return;
	}
	fprintf(out, "imm %08X", p->val);
}

static uint32_t find_slot(const struct image *im, const char *dll_sub, const char *func)
{
	const IMAGE_DOS_HEADER *dos = (const IMAGE_DOS_HEADER *)im->b;
	const IMAGE_NT_HEADERS32 *nt;
	const IMAGE_IMPORT_DESCRIPTOR *imp;
	uint32_t rva, end;
	int plus;

	if (im->n < sizeof(*dos) || dos->e_magic != IMAGE_DOS_SIGNATURE)
		return 0;
	if ((uint32_t)dos->e_lfanew + sizeof(*nt) > im->n)
		return 0;
	nt = (const IMAGE_NT_HEADERS32 *)(im->b + dos->e_lfanew);
	if (nt->Signature != IMAGE_NT_SIGNATURE)
		return 0;
	plus = nt->OptionalHeader.Magic == 0x20B;
	if (plus)
		return 0;
	rva = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress;
	if (!rva || !rva_ok(im, rva, sizeof(*imp)))
		return 0;
	for (imp = (const IMAGE_IMPORT_DESCRIPTOR *)(im->b + rva); imp->Name; imp++) {
		const char *dll;
		const uint8_t *thunk;
		uint32_t oft, ft, i;

		if (!rva_ok(im, imp->Name, 1))
			break;
		dll = (const char *)(im->b + imp->Name);
		if (!strstr(dll, dll_sub))
			continue;
		oft = imp->OriginalFirstThunk ? imp->OriginalFirstThunk : imp->FirstThunk;
		ft = imp->FirstThunk;
		if (!rva_ok(im, oft, 4) || !rva_ok(im, ft, 4))
			continue;
		thunk = im->b + oft;
		end = im->size;
		for (i = 0; oft + i * 4 + 4 <= end && i < 4000; i++) {
			uint32_t v = (uint32_t)thunk[i * 4] | ((uint32_t)thunk[i * 4 + 1] << 8) |
				     ((uint32_t)thunk[i * 4 + 2] << 16) |
				     ((uint32_t)thunk[i * 4 + 3] << 24);
			const char *nm;
			if (v == 0)
				break;
			if (v & 0x80000000)
				continue;
			if (!rva_ok(im, v + 2, 1))
				continue;
			nm = (const char *)(im->b + v + 2);
			if (strcmp(nm, func) == 0)
				return ft + i * 4;
		}
	}
	return 0;
}

static int load_image(struct image *im, const char *path)
{
	HANDLE h, map;
	const uint8_t *view;
	LARGE_INTEGER sz;
	const IMAGE_DOS_HEADER *dos;
	const IMAGE_NT_HEADERS32 *nt;
	const IMAGE_SECTION_HEADER *sec;
	int i;

	h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
	if (h == INVALID_HANDLE_VALUE) {
		fprintf(stderr, "cannot open %s (%lu)\n", path, GetLastError());
		return 0;
	}
	if (!GetFileSizeEx(h, &sz) || sz.QuadPart < 0x200 || sz.QuadPart > 64 * 1024 * 1024) {
		fprintf(stderr, "unexpected size\n");
		CloseHandle(h);
		return 0;
	}
	map = CreateFileMappingA(h, NULL, PAGE_READONLY, 0, 0, NULL);
	if (!map) {
		CloseHandle(h);
		return 0;
	}
	view = MapViewOfFile(map, FILE_MAP_READ, 0, 0, 0);
	CloseHandle(map);
	CloseHandle(h);
	if (!view)
		return 0;
	dos = (const IMAGE_DOS_HEADER *)view;
	if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
		fprintf(stderr, "not a PE\n");
		UnmapViewOfFile(view);
		return 0;
	}
	nt = (const IMAGE_NT_HEADERS32 *)(view + dos->e_lfanew);
	if (nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.Magic != 0x10B) {
		fprintf(stderr, "need a PE32 dump\n");
		UnmapViewOfFile(view);
		return 0;
	}
	im->n = (size_t)sz.QuadPart;
	im->b = (uint8_t *)malloc(im->n);
	if (!im->b) {
		UnmapViewOfFile(view);
		return 0;
	}
	im->base = nt->OptionalHeader.ImageBase;
	im->size = nt->OptionalHeader.SizeOfImage;
	im->text_rva = 0;
	im->text_end = im->size;
	sec = IMAGE_FIRST_SECTION(nt);
	for (i = 0; i < nt->FileHeader.NumberOfSections; i++) {
		char name[9];
		memcpy(name, sec[i].Name, 8);
		name[8] = 0;
		if (strcmp(name, ".text") == 0) {
			im->text_rva = sec[i].VirtualAddress;
			im->text_end = sec[i].VirtualAddress + sec[i].Misc.VirtualSize;
			if (im->text_end > im->size)
				im->text_end = im->size;
		}
	}
	memcpy(im->b, view, im->n);
	UnmapViewOfFile(view);
	return 1;
}

int main(int argc, char **argv)
{
	struct image im;
	uint32_t slot_rva[API_N], slot_va[API_N];
	uint32_t rva;
	int api, calls = 0, named = 0, unresolved = 0;
	const char *path = argc > 1 ? argv[1]
				    : "C:\\Program Files (x86)\\Steam\\steamapps\\common\\Rabi-Ribi\\rabiribi.exe.dump_00850000.exe";

	if (!load_image(&im, path))
		return 1;
	printf("image base %08X  size %08X  .text %08X-%08X  file %s\n",
	       im.base, im.size, im.text_rva, im.text_end, path);

	for (api = 0; api < API_N; api++) {
		slot_rva[api] = find_slot(&im, "KERNEL32", api_name[api]);
		slot_va[api] = slot_rva[api] ? im.base + slot_rva[api] : 0;
		printf("slot %-16s rva %08X va %08X\n", api_name[api], slot_rva[api], slot_va[api]);
		if (!slot_va[api]) {
			fprintf(stderr, "missing slot %s\n", api_name[api]);
			return 1;
		}
	}

	for (rva = im.text_rva; rva + 6 < im.text_end; rva++) {
		const uint8_t *q = im.b + rva;
		uint32_t disp;
		int which = -1;
		struct push pushes[kMaxPushes];
		int np, argi, wide;

		if (q[0] != 0xFF || q[1] != 0x15)
			continue;
		disp = (uint32_t)q[2] | ((uint32_t)q[3] << 8) |
		       ((uint32_t)q[4] << 16) | ((uint32_t)q[5] << 24);
		for (api = 0; api < API_N; api++)
			if (disp == slot_va[api])
				which = api;
		if (which < 0)
			continue;
		calls++;
		np = decode_pushes(&im, rva, pushes);
		wide = (which == API_LLW || which == API_LLEX);
		argi = api_name_arg[which] - 1;
		printf("%08X  %-16s  argc %d  pushes %d  arg: ",
		       rva, api_name[which], api_argc[which], np);
		if (argi < np)
			describe_push(&im, &pushes[argi], wide, stdout);
		else
			printf("missing");
		if (argi < np && (pushes[argi].kind == 0 || pushes[argi].kind == 1))
			named++;
		else
			unresolved++;
		if (np > 1) {
			int k;
			printf("   [");
			for (k = 0; k < np; k++) {
				if (k)
					printf(", ");
				describe_push(&im, &pushes[k], wide, stdout);
			}
			printf("]");
		}
		printf("\n");
		rva += 5;
	}
	printf("direct calls %d  literal %d  unresolved %d\n", calls, named, unresolved);

	/* DxLib does not leave the import as FF 15. It loads the slot into a
	 * register once (`mov esi, [GetProcAddress]`, `mov edi, [LoadLibraryW]`)
	 * and then calls the register. The argument is still an immediate push
	 * a few bytes earlier. */
	for (rva = im.text_rva; rva + 6 < im.text_end; rva++) {
		const uint8_t *q = im.b + rva;
		int reg, api, back;
		uint32_t disp, arg = 0;
		char lit[kMaxName];

		if (q[0] != 0x8B || (q[1] & 0xC7) != 0x05)
			continue;
		disp = (uint32_t)q[2] | ((uint32_t)q[3] << 8) |
		       ((uint32_t)q[4] << 16) | ((uint32_t)q[5] << 24);
		api = -1;
		for (reg = 0; reg < API_N; reg++)
			if (disp == slot_va[reg])
				api = reg;
		if (api < 0)
			continue;
		reg = (q[1] >> 3) & 7;
		/* Walk forward a short way. The register is live for one loader
		 * function, which in this image is well under 2 KB. */
		{
			uint32_t p;
			uint32_t end = rva + 0x2000;
			if (end > im.text_end)
				end = im.text_end;
			for (p = rva + 6; p + 2 < end; p++) {
				/* 0xCC and 0xC3 occur as immediate bytes (an IAT slot's low
				 * byte is CC). Stopping on them cuts the loader off before
				 * its first call. The window above is the bound instead. */
				if (im.b[p] != 0xFF || im.b[p + 1] != (uint8_t)(0xD0 + reg))
					continue;
				arg = 0;
				for (back = 1; back <= 48; back++) {
					const uint8_t *s;
					if (p < (uint32_t)back + 4)
						break;
					s = im.b + p - back;
					if (s[0] != 0x68)
						continue;
					arg = (uint32_t)s[1] | ((uint32_t)s[2] << 8) |
					      ((uint32_t)s[3] << 16) | ((uint32_t)s[4] << 24);
					break;
				}
				printf("%08X  %-16s  via %s  arg: ", p, api_name[api], regn[reg]);
				if (!arg)
					printf("missing\n");
				else if (read_literal(&im, arg, api == API_LLW || api == API_LLEX, lit, sizeof(lit)))
					printf("\"%s\"\n", lit);
				else if (read_literal(&im, arg, 0, lit, sizeof(lit)))
					printf("\"%s\"\n", lit);
				else
					printf("imm %08X\n", arg);
				calls++;
			}
		}
		rva += 5;
	}
	printf("calls including indirect %d\n", calls);

	/* DxLib's xinput probe writes the three DLL names into locals with
	 * `mov [ebp+disp], imm32` and hands the list to a helper, so there is
	 * no push immediately in front of LoadLibrary. */
	for (rva = im.text_rva; rva + 10 < im.text_end; rva++) {
		const uint8_t *q = im.b + rva;
		uint32_t imm;
		char lit[kMaxName];
		size_t len;

		if (q[0] != 0xC7 || q[1] != 0x85)
			continue;
		imm = (uint32_t)q[6] | ((uint32_t)q[7] << 8) |
		      ((uint32_t)q[8] << 16) | ((uint32_t)q[9] << 24);
		if (!read_literal(&im, imm, 1, lit, sizeof(lit)))
			continue;
		len = strlen(lit);
		if (len < 4 || _stricmp(lit + len - 4, ".dll") != 0)
			continue;
		printf("%08X  dll-list          \"%s\"\n", rva, lit);
	}
	free(im.b);
	return 0;
}
