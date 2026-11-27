// SPDX-License-Identifier: GPL-2.0-or-later
// Recover code Ghidra's auto-analysis misses in BARENA.EXE, using the LE fixups
// (every absolute address in the image has a fixup, so they are a complete list
// of pointer locations).
//
//  1. Pointers stored in the data object (obj3) that target the code object are
//     function pointers (callbacks, handler tables): disassemble + create function.
//  2. Pointers stored in the code object at locations that are not instructions
//     are Watcom switch jump tables (placed inline before the dispatch code):
//     define them as pointers and disassemble their targets as case labels.
//
// Repeats until nothing new is found, re-running auto-analysis in between.
// Argument: path to the extracted/ directory.
// @category FantasyGeneral

import ghidra.app.cmd.disassemble.DisassembleCommand;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.*;
import ghidra.program.model.data.PointerDataType;
import ghidra.program.model.listing.*;
import ghidra.program.model.mem.*;

import java.nio.file.*;
import java.util.*;
import java.util.regex.*;

public class FGRecoverCode extends GhidraScript {
    record Fixup(long src, long target) {}

    @Override
    protected void run() throws Exception {
        String[] args = getScriptArgs();
        Path extracted = Paths.get(args.length > 0 ? args[0] : "extracted");
        List<Fixup> fixups = loadFixups(extracted);
        Memory mem = currentProgram.getMemory();
        MemoryBlock code = mem.getBlock("obj1"), data = mem.getBlock("obj3");
        Listing listing = currentProgram.getListing();

        recoverFromFixups(fixups, code, data, listing);
        report(code);

        // The code object holds almost no data, so the aggressive instruction
        // finder is safe here: it picks up library routines (Miles, Smacker,
        // int-N stub tables) that are only reached through computed calls.
        println("running Aggressive Instruction Finder");
        setAnalysisOption(currentProgram, "Aggressive Instruction Finder", "true");
        analyzeAll(currentProgram);
        recoverFromFixups(fixups, code, data, listing);
        report(code);
    }

    private void recoverFromFixups(List<Fixup> fixups, MemoryBlock code, MemoryBlock data,
                                   Listing listing) throws Exception {
        for (int round = 1; round <= 10; round++) {
            int newFuncs = 0, tables = 0, labels = 0;
            for (Fixup f : fixups) {
                Address src = toAddr(f.src), tgt = toAddr(f.target);
                if (!code.contains(tgt)) continue;
                boolean tgtIsCode = listing.getInstructionAt(tgt) != null;

                if (data.contains(src)) {
                    if (getFunctionAt(tgt) != null || !looksLikeCode(tgt)) continue;
                    if (listing.getInstructionContaining(tgt) != null && !tgtIsCode) continue;
                    if (!tgtIsCode) disasmAt(tgt);
                    if (listing.getInstructionAt(tgt) != null && createFunction(tgt, null) != null) newFuncs++;
                } else if (code.contains(src) && listing.getInstructionContaining(src) == null) {
                    // A pointer sitting in the code object outside any instruction: jump table entry.
                    if (listing.getDefinedDataAt(src) == null) {
                        try {
                            clearListing(src, src.add(3));
                            createData(src, PointerDataType.dataType);
                            tables++;
                        } catch (Exception e) { /* overlaps something; leave it */ }
                    }
                    if (!tgtIsCode && looksLikeCode(tgt)) {
                        disasmAt(tgt);
                        if (listing.getInstructionAt(tgt) != null) labels++;
                    }
                }
            }
            println(String.format("round %d: %d new functions, %d table entries, %d case labels",
                round, newFuncs, tables, labels));
            if (newFuncs == 0 && tables == 0 && labels == 0) break;
            analyzeChanges(currentProgram);
        }
    }

    private boolean looksLikeCode(Address a) {
        try {
            byte b0 = getByte(a), b1 = getByte(a.add(1));
            return !(b0 == 0 && b1 == 0);  // zero runs are padding / data
        } catch (Exception e) {
            return false;
        }
    }

    private void report(MemoryBlock code) {
        long covered = 0;
        int count = 0;
        AddressSet codeSet = new AddressSet(code.getStart(), code.getEnd());
        for (Function f : currentProgram.getFunctionManager().getFunctions(true)) {
            count++;
            covered += f.getBody().intersect(codeSet).getNumAddresses();
        }
        long insnBytes = 0;
        for (Instruction i : currentProgram.getListing().getInstructions(codeSet, true))
            insnBytes += i.getLength();
        println(String.format("functions: %d; obj1 bytes in functions: %.1f%%; obj1 bytes disassembled: %.1f%%",
            count, 100.0 * covered / code.getSize(), 100.0 * insnBytes / code.getSize()));
    }

    private List<Fixup> loadFixups(Path extracted) throws Exception {
        // The extractor writes fixups with page-relative sources; resolve them
        // with the object table from info.json.
        String info = Files.readString(extracted.resolve("info.json"));
        Matcher om = Pattern.compile(
            "\"base\": (\\d+),[^}]*?\"page_map_index\": (\\d+),\\s*\"page_count\": (\\d+)").matcher(info);
        List<long[]> objs = new ArrayList<>();  // base, first page, page count
        while (om.find())
            objs.add(new long[] {Long.parseLong(om.group(1)), Long.parseLong(om.group(2)), Long.parseLong(om.group(3))});

        String fx = Files.readString(extracted.resolve("fixups.json"));
        Matcher m = Pattern.compile(
            "\"page\": (\\d+)[^}]*?\"target_object\": (\\d+), \"target_offset\": (\\d+)[^}]*?\"src_off\": (-?\\d+)").matcher(fx);
        List<Fixup> out = new ArrayList<>();
        while (m.find()) {
            long page = Long.parseLong(m.group(1));
            int tobj = Integer.parseInt(m.group(2));
            long toff = Long.parseLong(m.group(3)), soff = Long.parseLong(m.group(4));
            long[] so = null;
            for (long[] o : objs) if (o[1] <= page && page < o[1] + o[2]) so = o;
            if (so == null || soff < 0) continue;  // negative = continuation of a straddling fixup
            out.add(new Fixup(so[0] + (page - so[1]) * 4096 + soff, objs.get(tobj - 1)[0] + toff));
        }
        println("loaded " + out.size() + " fixups");
        return out;
    }

    private void disasmAt(Address a) {
        new DisassembleCommand(a, null, true).applyTo(currentProgram, monitor);
    }
}
