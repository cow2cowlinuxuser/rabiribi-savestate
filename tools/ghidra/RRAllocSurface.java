// Enumerates the complete allocator surface of a statically linked CRT.
//
// Rabi-Ribi links the runtime statically: there is no CRT DLL in its imports,
// and malloc, free and realloc sit in its own .text. To redirect the game's
// memory we detour those functions, and for that to be safe we have to hook a
// closed set - a block allocated through our hook and freed through a routine
// we missed goes to HeapFree on a heap it does not belong to, which is silent
// corruption of exactly the kind we are already hunting.
//
// Every allocator in a static CRT bottoms out in one of the Heap* calls, and
// there are only a handful of those sites. This finds them, names the function
// each one lives in, and prints the head of that function with the bytes - both
// so the set can be checked for completeness and so the detour has a prologue
// it can verify before it writes anything.
//
// Writes F:\rbo_fabre_proto\ghidra_surface.txt
//
//@category Analysis
//@menupath Tools.RR Allocator Surface

import java.io.File;
import java.io.PrintWriter;
import java.util.ArrayList;
import java.util.Collections;
import java.util.HashSet;
import java.util.List;
import java.util.Set;

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.listing.InstructionIterator;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.Symbol;
import ghidra.program.model.symbol.SymbolIterator;

public class RRAllocSurface extends GhidraScript {

	private static final String OUT = "F:\\rbo_fabre_proto\\ghidra_surface.txt";

	/* The floor of every allocator. Whatever calls these is part of the set we
	 * have to hook, whether or not it carries a recognisable name. */
	private static final String[] FLOOR = { "HeapAlloc", "HeapFree", "HeapReAlloc",
		"HeapSize", "HeapCreate", "GetProcessHeap", "HeapDestroy" };

	/* Named entry points worth dumping whether or not they touch Heap* directly,
	 * because the ones that forward are the dangerous ones. */
	private static final String[] NAMED = { "_malloc", "_free", "__free_base",
		"_realloc", "_calloc", "__calloc_impl", "_msize", "_recalloc", "_expand",
		"_calloc_base", "_malloc_base", "_realloc_base", "_msize_base" };

	private PrintWriter out;

	private void say(String s) {
		println(s);
		out.println(s);
	}

	@Override
	public void run() throws Exception {
		File f = new File(OUT);
		f.getParentFile().mkdirs();
		out = new PrintWriter(f);
		try {
			body();
		}
		finally {
			out.flush();
			out.close();
		}
		println("wrote " + OUT);
	}

	private void body() throws Exception {
		long base = currentProgram.getImageBase().getOffset();
		Set<Function> surface = new HashSet<>();

		say("image base " + currentProgram.getImageBase() + "   ("
				+ currentProgram.getName() + ")");
		say("");
		say("=== who calls the Heap* floor ===");
		for (String want : FLOOR) {
			List<Address> sites = new ArrayList<>();
			SymbolIterator si = currentProgram.getSymbolTable().getAllSymbols(true);
			while (si.hasNext() && !monitor.isCancelled()) {
				Symbol s = si.next();
				if (!s.getName().equals(want)
						&& !s.getName().startsWith("PTR_" + want + "_")) {
					continue;
				}
				for (Reference r : currentProgram.getReferenceManager()
						.getReferencesTo(s.getAddress())) {
					sites.add(r.getFromAddress());
				}
			}
			Collections.sort(sites);
			say("");
			say("  " + want + ":");
			Set<Address> seen = new HashSet<>();
			for (Address a : sites) {
				if (!seen.add(a)) {
					continue;
				}
				Function fn = getFunctionContaining(a);
				if (fn == null) {
					say(String.format("    %s  (no function - data slot)", a));
					continue;
				}
				surface.add(fn);
				say(String.format("    %s  in %-34s entry %s  RVA %08X", a,
					fn.getName(), fn.getEntryPoint(),
					fn.getEntryPoint().getOffset() - base));
			}
			if (sites.isEmpty()) {
				say("    none");
			}
		}

		for (String n : NAMED) {
			SymbolIterator si = currentProgram.getSymbolTable().getSymbols(n);
			while (si.hasNext()) {
				Function fn = getFunctionContaining(si.next().getAddress());
				if (fn != null) {
					surface.add(fn);
				}
			}
		}

		say("");
		say("=== the set, with prologue bytes ===");
		say("  a detour overwrites the first 5 bytes with a jump, so it needs to");
		say("  know what it is overwriting and to refuse if the bytes moved");
		List<Function> ordered = new ArrayList<>(surface);
		Collections.sort(ordered, (a, b) -> a.getEntryPoint().compareTo(b.getEntryPoint()));
		for (Function fn : ordered) {
			if (monitor.isCancelled()) {
				break;
			}
			Address e = fn.getEntryPoint();
			say("");
			say(String.format("  %s   entry %s   RVA %08X   callers %d", fn.getName(),
				e, e.getOffset() - base, fn.getCallingFunctions(monitor).size()));
			byte[] b = new byte[16];
			try {
				currentProgram.getMemory().getBytes(e, b);
				StringBuilder sb = new StringBuilder("    bytes ");
				for (byte x : b) {
					sb.append(String.format("%02X ", x));
				}
				say(sb.toString());
			}
			catch (Exception ex) {
				say("    bytes unavailable: " + ex.getMessage());
			}
			InstructionIterator ii = currentProgram.getListing()
					.getInstructions(fn.getBody(), true);
			int n = 0;
			while (ii.hasNext() && n < 14) {
				Instruction in = ii.next();
				say(String.format("    %s  %s", in.getAddress(), in.toString()));
				n++;
			}
		}

		say("");
		say("=== done ===");
	}
}
