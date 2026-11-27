// SPDX-License-Identifier: GPL-2.0-or-later
// Print decompiled C for the given functions (addresses in hex, or names).
//   tools/ghidra.sh --headless ghidra-project FantasyGeneral -process BARENA.elf \
//       -noanalysis -readOnly -scriptPath tools/ghidra/scripts \
//       -postScript FGDecompile.java 0x12758 _cstart_
// @category FantasyGeneral

import ghidra.app.decompiler.*;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.Symbol;

public class FGDecompile extends GhidraScript {
    @Override
    protected void run() throws Exception {
        DecompInterface ifc = new DecompInterface();
        ifc.openProgram(currentProgram);
        for (String arg : getScriptArgs()) {
            Function f = resolve(arg);
            if (f == null) { println("no function for " + arg); continue; }
            DecompileResults r = ifc.decompileFunction(f, 60, monitor);
            println("// ---- " + f.getName() + " @ " + f.getEntryPoint()
                + "  [" + f.getCallingConventionName() + "]\n"
                + (r.decompileCompleted() ? r.getDecompiledFunction().getC() : r.getErrorMessage()));
        }
        ifc.dispose();
    }

    private Function resolve(String s) {
        if (s.matches("(0x)?[0-9a-fA-F]+")) {
            Address a = toAddr(Long.parseLong(s.replaceFirst("^0x", ""), 16));
            Function f = getFunctionAt(a);
            return f != null ? f : getFunctionContaining(a);
        }
        for (Symbol sym : currentProgram.getSymbolTable().getSymbols(s)) {
            Function f = getFunctionAt(sym.getAddress());
            if (f != null) return f;
        }
        return null;
    }
}
