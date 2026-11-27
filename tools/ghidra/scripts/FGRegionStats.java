// SPDX-License-Identifier: GPL-2.0-or-later
// Function count and code size per region of the code map (docs/NOTES.md).
// @category FantasyGeneral

import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.Function;

public class FGRegionStats extends GhidraScript {
    private static final Object[][] REGIONS = {
        {"game C (1)", 0x10000L, 0x3e3aeL},
        {"game asm graphics", 0x3e3aeL, 0x43de6L},
        {"game C (2)", 0x43de6L, 0x48d53L},
        {"unidentified", 0x48d53L, 0x4cc60L},
        {"Miles AIL 3", 0x4cc60L, 0x58324L},
        {"game C (3)", 0x58324L, 0x89fa0L},
        {"UVBELib", 0x89fa0L, 0x8c041L},
        {"CRT bits", 0x8c041L, 0x8c100L},
        {"Smacker", 0x8c100L, 0x8fe00L},
        {"Watcom CRT", 0x8fe00L, 0x98d40L},
    };

    @Override
    protected void run() throws Exception {
        long[] count = new long[REGIONS.length], bytes = new long[REGIONS.length];
        long[] small = new long[REGIONS.length], large = new long[REGIONS.length];
        for (Function f : currentProgram.getFunctionManager().getFunctions(true)) {
            long a = f.getEntryPoint().getOffset();
            for (int i = 0; i < REGIONS.length; i++) {
                if (a >= (Long) REGIONS[i][1] && a < (Long) REGIONS[i][2]) {
                    long n = f.getBody().getNumAddresses();
                    count[i]++;
                    bytes[i] += n;
                    if (n < 64) small[i]++;
                    if (n >= 1024) large[i]++;
                }
            }
        }
        println(String.format("%-20s %6s %9s %7s %7s", "region", "funcs", "bytes", "<64B", ">=1KB"));
        for (int i = 0; i < REGIONS.length; i++)
            println(String.format("%-20s %6d %9d %7d %7d", REGIONS[i][0], count[i], bytes[i], small[i], large[i]));
    }
}
