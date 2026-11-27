// SPDX-License-Identifier: GPL-2.0-or-later
// Report the largest ranges of the code object (obj1) that are still not
// disassembled, with a hex/ASCII preview, so they can be classified as data,
// padding or missed code.
// @category FantasyGeneral

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.*;
import ghidra.program.model.listing.*;
import ghidra.program.model.mem.MemoryBlock;

import java.util.*;

public class FGUndefinedReport extends GhidraScript {
    @Override
    protected void run() throws Exception {
        MemoryBlock code = currentProgram.getMemory().getBlock("obj1");
        AddressSet rest = new AddressSet(code.getStart(), code.getEnd());
        Listing listing = currentProgram.getListing();
        for (Instruction i : listing.getInstructions(rest, true))
            rest.delete(i.getMinAddress(), i.getMaxAddress());

        List<AddressRange> ranges = new ArrayList<>();
        rest.getAddressRanges().forEach(ranges::add);
        long total = 0, zero = 0;
        for (AddressRange r : ranges) {
            total += r.getLength();
            boolean allZero = true;
            for (Address a = r.getMinAddress(); a.compareTo(r.getMaxAddress()) <= 0; a = a.add(1))
                if (getByte(a) != 0) { allZero = false; break; }
            if (allZero) zero += r.getLength();
        }
        ranges.sort((a, b) -> Long.compare(b.getLength(), a.getLength()));
        println(String.format("not disassembled: %d bytes in %d ranges (%d bytes all-zero)",
            total, ranges.size(), zero));
        int n = 0;
        for (AddressRange r : ranges) {
            if (n++ >= 25) break;
            byte[] b = getBytes(r.getMinAddress(), (int) Math.min(48, r.getLength()));
            StringBuilder hex = new StringBuilder(), asc = new StringBuilder();
            for (byte x : b) {
                hex.append(String.format("%02x", x & 0xff));
                asc.append(x >= 32 && x < 127 ? (char) x : '.');
            }
            Function before = getFunctionBefore(r.getMinAddress());
            println(String.format("  %s len=%6d after %s\n      %s\n      %s",
                r.getMinAddress(), r.getLength(), before == null ? "-" : before.getName(), hex, asc));
        }
    }
}
