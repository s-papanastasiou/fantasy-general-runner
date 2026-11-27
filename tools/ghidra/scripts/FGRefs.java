// SPDX-License-Identifier: GPL-2.0-or-later
// List every reference to the given addresses (hex): from-address, type, the
// function it is in, and the instruction.
//   tools/ghidra.sh --headless ghidra-project FantasyGeneral -process BARENA.elf \
//       -noanalysis -readOnly -scriptPath tools/ghidra/scripts \
//       -postScript FGRefs.java 0xcb243
// @category FantasyGeneral

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.symbol.Reference;

public class FGRefs extends GhidraScript {
    @Override
    protected void run() throws Exception {
        for (String arg : getScriptArgs()) {
            Address target = toAddr(Long.parseLong(arg.replaceFirst("^0x", ""), 16));
            println("// references to " + target);
            for (Reference r : getReferencesTo(target)) {
                Address from = r.getFromAddress();
                Function f = getFunctionContaining(from);
                Instruction ins = getInstructionAt(from);
                println(String.format("%s %-6s %-28s %s", from, r.getReferenceType(),
                    f == null ? "?" : f.getName() + "@" + f.getEntryPoint(), ins == null ? "" : ins.toString()));
            }
        }
    }
}
