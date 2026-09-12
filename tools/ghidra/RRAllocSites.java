// Finds how a packed executable reaches its C runtime allocator.
//
// Run this against the program open in the Code Browser. It writes a report to
// F:\rbo_fabre_proto\ghidra_report.txt and prints the same thing to the console.
//
// The question it exists to answer: our redirect found none of ucrtbase's
// allocator addresses in rabiribi.exe's import table, which is what a packed
// executable looks like from the outside. Somewhere there is still a slot the
// game calls malloc through. This reports every candidate and, for the ones
// that are real, the offset from the image base - which is all our code needs
// to patch the live process directly instead of searching for it.
//
//@category Analysis
//@menupath Tools.RR Allocator Sites

import java.io.File;
import java.io.PrintWriter;
import java.util.ArrayList;
import java.util.List;

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.data.Pointer;
import ghidra.program.model.listing.Data;
import ghidra.program.model.listing.DataIterator;
import ghidra.program.model.mem.MemoryBlock;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.Symbol;
import ghidra.program.model.symbol.SymbolIterator;
import ghidra.program.model.symbol.SymbolTable;

public class RRAllocSites extends GhidraScript {

	private static final String OUT = "F:\\rbo_fabre_proto\\ghidra_report.txt";

	/* The seven the redirect needs. A hole in this set is a block freed to the
	 * wrong allocator, so we care which ones are reachable, not just whether
	 * any are. */
	private static final String[] WANTED = { "malloc", "calloc", "realloc", "free",
		"_msize", "_recalloc", "_expand", "??2@", "??3@", "operator_new" };

	private PrintWriter out;

	private void say(String s) {
		println(s);
		out.println(s);
	}

	private static boolean wanted(String name) {
		String n = name.toLowerCase();
		for (String w : WANTED) {
			String lw = w.toLowerCase();
			if (n.equals(lw) || n.endsWith("!" + lw) || n.endsWith("::" + lw)
					|| n.contains(lw)) {
				return true;
			}
		}
		return false;
	}

	@Override
	public void run() throws Exception {
		File f = new File(OUT);
		f.getParentFile().mkdirs();
		out = new PrintWriter(f);
		try {
			report();
		}
		finally {
			out.flush();
			out.close();
		}
		println("wrote " + OUT);
	}

	private void report() throws Exception {
		long base = currentProgram.getImageBase().getOffset();

		say("=== program ===");
		say("  name            " + currentProgram.getName());
		say("  path            " + currentProgram.getExecutablePath());
		say("  format          " + currentProgram.getExecutableFormat());
		say("  language        " + currentProgram.getLanguageID());
		say("  image base      " + currentProgram.getImageBase());
		say("  md5             " + currentProgram.getExecutableMD5());

		say("");
		say("=== memory blocks ===");
		say("  the packer's section is the one that is writable and has no name");
		say("  you recognise; a section with no bytes behind it was never dumped");
		for (MemoryBlock b : currentProgram.getMemory().getBlocks()) {
			say(String.format("  %-10s %s..%s  %9d  %s%s%s %s", b.getName(),
				b.getStart(), b.getEnd(), b.getSize(),
				b.isRead() ? "r" : "-", b.isWrite() ? "w" : "-",
				b.isExecute() ? "x" : "-",
				b.isInitialized() ? "has bytes" : "UNINITIALIZED"));
		}

		say("");
		say("=== external libraries the loader recorded ===");
		String[] libs = currentProgram.getExternalManager().getExternalLibraryNames();
		if (libs.length == 0) {
			say("  none - consistent with a packed image whose imports were");
			say("  resolved by hand rather than by the loader");
		}
		for (String l : libs) {
			say("  " + l + "   -> " + currentProgram.getExternalManager()
					.getExternalLibraryPath(l));
		}

		say("");
		say("=== allocator symbols anywhere in the program ===");
		say("  RVA is what our code needs: we add it to the live image base and");
		say("  patch there, with no searching at all");
		SymbolTable st = currentProgram.getSymbolTable();
		SymbolIterator it = st.getAllSymbols(true);
		int hits = 0;
		List<Address> slots = new ArrayList<>();
		while (it.hasNext() && !monitor.isCancelled()) {
			Symbol s = it.next();
			if (!wanted(s.getName())) {
				continue;
			}
			hits++;
			Address a = s.getAddress();
			int refs = currentProgram.getReferenceManager().getReferenceCountTo(a);
			String ns = s.getParentNamespace() == null ? "-"
					: s.getParentNamespace().getName();
			say(String.format("  %-28s %s  RVA %08X  %-12s ns=%-16s src=%-8s refs=%d",
				s.getName(), a, a.getOffset() - base, s.getSymbolType(), ns,
				s.getSource(), refs));
			slots.add(a);
			if (refs > 0 && refs <= 12) {
				for (Reference r : currentProgram.getReferenceManager()
						.getReferencesTo(a)) {
					say("        <- " + r.getFromAddress() + "  "
							+ r.getReferenceType());
				}
			}
		}
		if (hits == 0) {
			say("  none. The allocator is not named anywhere in this image, so the");
			say("  game either resolves it by string at runtime (see below) or the");
			say("  dump was taken before the packer wrote its table");
		}

		say("");
		say("=== allocator names as strings, and who reads them ===");
		say("  a hit here means the packer calls GetProcAddress by name, and the");
		say("  code that reads the string is the code that stores the answer");
		int sh = 0;
		DataIterator di = currentProgram.getListing().getDefinedData(true);
		while (di.hasNext() && !monitor.isCancelled()) {
			Data d = di.next();
			Object v = d.getValue();
			if (!(v instanceof String)) {
				continue;
			}
			String sv = ((String) v).trim();
			boolean match = false;
			for (String w : WANTED) {
				if (sv.equalsIgnoreCase(w)) {
					match = true;
					break;
				}
			}
			if (!match) {
				continue;
			}
			sh++;
			say(String.format("  \"%s\" at %s  RVA %08X", sv, d.getAddress(),
				d.getAddress().getOffset() - base));
			for (Reference r : currentProgram.getReferenceManager()
					.getReferencesTo(d.getAddress())) {
				say("        read by " + r.getFromAddress() + "  "
						+ r.getReferenceType());
			}
		}
		if (sh == 0) {
			say("  none");
		}

		say("");
		say("=== pointer-typed data in writable, non-executable memory ===");
		say("  this is the shape of a hand-built import table; a long run of");
		say("  pointers in one block is almost certainly it");
		MemoryBlock prev = null;
		int run = 0;
		Address runStart = null;
		int shown = 0;
		di = currentProgram.getListing().getDefinedData(true);
		while (di.hasNext() && !monitor.isCancelled() && shown < 40) {
			Data d = di.next();
			if (!(d.getDataType() instanceof Pointer)) {
				continue;
			}
			MemoryBlock b = currentProgram.getMemory().getBlock(d.getAddress());
			if (b == null || b.isExecute() || !b.isWrite()) {
				continue;
			}
			if (b != prev) {
				if (run > 3) {
					say(String.format("  %-10s run of %d pointers from %s (RVA %08X)",
						prev.getName(), run, runStart,
						runStart.getOffset() - base));
					shown++;
				}
				prev = b;
				run = 0;
				runStart = d.getAddress();
			}
			if (run == 0) {
				runStart = d.getAddress();
			}
			run++;
		}
		if (prev != null && run > 3) {
			say(String.format("  %-10s run of %d pointers from %s (RVA %08X)",
				prev.getName(), run, runStart, runStart.getOffset() - base));
		}

		say("");
		say("=== done ===");
	}
}
