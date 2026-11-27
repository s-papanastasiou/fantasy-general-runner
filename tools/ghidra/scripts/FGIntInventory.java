// SPDX-License-Identifier: GPL-2.0-or-later
// Inventory of every software interrupt in the code object: which DOS / DPMI /
// BIOS / mouse services the program requests, and from which functions.
//
// For each INT instruction it scans back (same function, up to 24
// instructions, stopping at a call or another INT) for immediate loads into
// AX/AH/AL and BX/BL, which hold the function number and, for DPMI
// get/set-vector calls (int 31h AX=0200h-0205h) and DOS 25h/35h, the vector.
// Also lists who references the `int N; ret` stub tables used for interrupts
// chosen at run time.
// @category FantasyGeneral

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.*;
import ghidra.program.model.lang.Register;
import ghidra.program.model.listing.*;
import ghidra.program.model.mem.MemoryBlock;
import ghidra.program.model.scalar.Scalar;
import ghidra.program.model.symbol.Reference;

import java.util.*;

public class FGIntInventory extends GhidraScript {
    @Override
    protected void run() throws Exception {
        MemoryBlock code = currentProgram.getMemory().getBlock("obj1");
        Listing listing = currentProgram.getListing();
        TreeMap<String, TreeSet<String>> byService = new TreeMap<>();

        for (Instruction ins : listing.getInstructions(new AddressSet(code.getStart(), code.getEnd()), true)) {
            if (!ins.getMnemonicString().equalsIgnoreCase("INT")) continue;
            Scalar s = ins.getScalar(0);
            if (s == null) continue;
            int vec = (int) s.getUnsignedValue();
            Function fn = getFunctionContaining(ins.getAddress());

            Long ah = null, al = null, ax = null, bl = null;
            Instruction p = ins.getPrevious();
            for (int i = 0; i < 24 && p != null; i++, p = p.getPrevious()) {
                if (fn != null && !fn.getBody().contains(p.getAddress())) break;
                String m = p.getMnemonicString().toUpperCase();
                if (m.equals("CALL") || m.equals("INT") || m.startsWith("RET")) break;
                if (!m.equals("MOV") || p.getNumOperands() != 2) continue;
                Object[] dst = p.getOpObjects(0);
                Scalar imm = p.getScalar(1);
                if (dst.length != 1 || !(dst[0] instanceof Register) || imm == null) continue;
                String r = ((Register) dst[0]).getName().toUpperCase();
                long v = imm.getUnsignedValue();
                switch (r) {
                    case "EAX": case "AX": if (ax == null && ah == null && al == null) ax = v & 0xffff; break;
                    case "AH": if (ax == null && ah == null) ah = v & 0xff; break;
                    case "AL": if (ax == null && al == null) al = v & 0xff; break;
                    case "EBX": case "BX": case "BL": if (bl == null) bl = v & 0xff; break;
                }
            }
            if (ax == null && ah != null) ax = (ah << 8) | (al == null ? 0x100 * 0 : al) | (al == null ? 0x10000L : 0);
            String fnName = fn == null ? "<no function>" : fn.getName();
            String svc = describe(vec, ax, bl, al);
            byService.computeIfAbsent(svc, k -> new TreeSet<>())
                .add(fnName + "@" + ins.getAddress());
        }

        println("=== software interrupts by service (" + byService.size() + " distinct) ===");
        for (Map.Entry<String, TreeSet<String>> e : byService.entrySet())
            println(String.format("%-34s x%-3d %s", e.getKey(), e.getValue().size(),
                String.join(" ", e.getValue())));

        println("\n=== references to int-N stub tables ===");
        for (long t : new long[] {0x8b327L, 0x929abL, 0x8b324L, 0x929a8L}) {
            Address a = toAddr(t);
            for (Reference r : getReferencesTo(a)) {
                Function f = getFunctionContaining(r.getFromAddress());
                println(String.format("  %s <- %s (%s)", a, r.getFromAddress(), f == null ? "-" : f.getName()));
            }
        }
    }

    private String describe(int vec, Long ax, Long bl, Long al) {
        String n = String.format("int %02xh", vec);
        if (ax == null) return n + " ?";
        boolean ahOnly = (ax & 0x10000L) != 0;
        long v = ax & 0xffff;
        String f = ahOnly ? String.format("AH=%02x", v >> 8) : String.format("AX=%04x", v);
        String extra = "";
        if (vec == 0x31 && !ahOnly && v >= 0x200 && v <= 0x205 && bl != null) extra = String.format(" vec=%02x", bl);
        if (vec == 0x21 && ((v >> 8) == 0x25 || (v >> 8) == 0x35) && !ahOnly) extra = String.format(" vec=%02x", v & 0xff);
        return n + " " + f + extra;
    }
}
