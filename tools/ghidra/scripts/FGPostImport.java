// SPDX-License-Identifier: GPL-2.0-or-later
// Post-import fixups for Fantasy General's BARENA.EXE (run by tools/ghidra/import.sh).
//   * names the entry point
//   * reports analysis coverage: functions, bytes of obj1 covered by functions,
//     and fixup targets in obj1 that no function contains (candidates for
//     missed functions: callbacks, jump tables, tables of handlers)
// Argument: path to the extracted/ directory (holds fixups.json, info.json).
// @category FantasyGeneral

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.*;
import ghidra.program.model.listing.*;
import ghidra.program.model.mem.MemoryBlock;
import ghidra.program.model.symbol.SourceType;

import java.nio.file.*;
import java.util.*;
import java.util.regex.*;

public class FGPostImport extends GhidraScript {
    @Override
    protected void run() throws Exception {
        String[] args = getScriptArgs();
        Path extracted = Paths.get(args.length > 0 ? args[0] : "extracted");

        Address entry = currentProgram.getImageBase().getNewAddress(
            readEntry(extracted.resolve("info.json")));
        createLabel(entry, "_cstart_", true, SourceType.USER_DEFINED);

        MemoryBlock code = currentProgram.getMemory().getBlock("obj1");
        Listing listing = currentProgram.getListing();
        FunctionManager fm = currentProgram.getFunctionManager();

        long covered = 0;
        int count = 0;
        for (Function f : fm.getFunctions(true)) {
            count++;
            AddressSetView body = f.getBody();
            covered += body.intersect(new AddressSet(code.getStart(), code.getEnd())).getNumAddresses();
        }

        // Fixup targets inside the code object that land outside any function.
        String fx = Files.readString(extracted.resolve("fixups.json"));
        Matcher m = Pattern.compile("\"target_object\": (\\d+), \"target_offset\": (\\d+)").matcher(fx);
        long codeBase = code.getStart().getOffset();
        TreeSet<Long> orphans = new TreeSet<>();
        int codeTargets = 0;
        while (m.find()) {
            if (Integer.parseInt(m.group(1)) != 1) continue;
            long a = codeBase + Long.parseLong(m.group(2));
            codeTargets++;
            if (fm.getFunctionContaining(toAddr(a)) == null) orphans.add(a);
        }

        println(String.format("functions: %d", count));
        println(String.format("obj1 bytes in functions: %d of %d (%.1f%%)",
            covered, code.getSize(), 100.0 * covered / code.getSize()));
        println(String.format("fixups targeting obj1: %d, distinct targets outside functions: %d",
            codeTargets, orphans.size()));
        int shown = 0;
        for (long a : orphans) {
            if (shown++ >= 40) break;
            Instruction ins = listing.getInstructionAt(toAddr(a));
            println(String.format("  orphan %08x %s", a, ins == null ? "(not code)" : ins.toString()));
        }
    }

    private long readEntry(Path info) throws Exception {
        Matcher m = Pattern.compile("\"entry\": (\\d+)").matcher(Files.readString(info));
        if (!m.find()) throw new IllegalStateException("no entry in " + info);
        return Long.parseLong(m.group(1));
    }
}
