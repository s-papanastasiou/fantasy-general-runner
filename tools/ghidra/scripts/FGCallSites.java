// SPDX-License-Identifier: GPL-2.0-or-later
// For each target function given (hex address), decompile every caller and
// print the lines that call the target, with ctx lines before (default 1,
// set with an argument ctx=N) and one after.
// Used to recover the arguments of interrupt helpers (int386, DPMI 0300h
// wrappers) at their call sites.
//   tools/ghidra/run.py FGCallSites.java 92902 8b724
// @category FantasyGeneral

import ghidra.app.decompiler.*;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.Reference;

import java.util.*;

public class FGCallSites extends GhidraScript {
    @Override
    protected void run() throws Exception {
        DecompInterface ifc = new DecompInterface();
        ifc.openProgram(currentProgram);
        int ctx = 1;
        for (String arg : getScriptArgs()) {
            if (arg.startsWith("ctx=")) { ctx = Integer.parseInt(arg.substring(4)); continue; }
            Function target = getFunctionAt(toAddr(Long.parseLong(arg.replaceFirst("^0x", ""), 16)));
            if (target == null) { println("no function at " + arg); continue; }
            Set<Function> callers = new TreeSet<>(Comparator.comparing(Function::getEntryPoint));
            for (Reference r : getReferencesTo(target.getEntryPoint())) {
                Function f = getFunctionContaining(r.getFromAddress());
                if (f != null) callers.add(f);
            }
            println("=== " + target.getName() + " @ " + target.getEntryPoint()
                + " : " + callers.size() + " callers");
            for (Function f : callers) {
                DecompileResults res = ifc.decompileFunction(f, 60, monitor);
                if (!res.decompileCompleted()) { println("  " + f.getName() + ": decompile failed"); continue; }
                String[] lines = res.getDecompiledFunction().getC().split("\n");
                for (int i = 0; i < lines.length; i++) {
                    if (!lines[i].contains(target.getName() + "(")) continue;
                    StringBuilder sb = new StringBuilder();
                    for (int j = Math.max(0, i - ctx); j <= Math.min(lines.length - 1, i + 1); j++)
                        sb.append("\n      ").append(lines[j].trim());
                    println("  " + f.getName() + ":" + sb);
                }
            }
        }
        ifc.dispose();
    }
}
