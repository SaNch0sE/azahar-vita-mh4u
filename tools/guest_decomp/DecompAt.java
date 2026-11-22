// SPDX-FileCopyrightText: 2026 Willi Ye
// SPDX-License-Identifier: GPL-2.0-or-later

// Headless: decompile the guest functions at or around given addresses of a raw ARM dump (a
// title's code read out of a running diagnostics build with the `vdump` command).
// Arguments: OUT.c DEPTH ADDR... Each ADDR is hexadecimal: "=1a24ec" names a function's first
// instruction; a bare address is somewhere inside one, whose start is the nearest
// `push {..., lr}` before it. Callees are decompiled too, DEPTH calls down.
import ghidra.app.cmd.disassemble.DisassembleCommand;
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.address.AddressSet;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.SourceType;
import java.io.FileWriter;
import java.io.PrintWriter;
import java.util.ArrayDeque;
import java.util.LinkedHashMap;
import java.util.LinkedHashSet;
import java.util.Map;
import java.util.Set;

public class DecompAt extends GhidraScript {
    private Address startOf(Address inside) throws Exception {
        for (long back = 0; back < 0x2000; back += 4) {
            Address a = inside.subtract(back);
            int word = getInt(a);
            if ((word & 0xffff4000) == 0xe92d4000) { // push {..., lr}
                return a;
            }
        }
        return null;
    }

    private Function functionAt(Address entry) throws Exception {
        Function f = getFunctionAt(entry);
        if (f != null) {
            return f;
        }
        new DisassembleCommand(entry, null, true).applyTo(currentProgram, monitor);
        return createFunction(entry, null);
    }

    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        int depth = Integer.parseInt(args[1]);
        Map<Address, Integer> queue = new LinkedHashMap<>();
        for (int i = 2; i < args.length; i++) {
            boolean exact = args[i].startsWith("=");
            Address a = toAddr(Long.parseLong(exact ? args[i].substring(1) : args[i], 16));
            Address entry = exact ? a : startOf(a);
            if (entry == null) {
                println("no function start before " + a);
                continue;
            }
            queue.put(entry, 0);
        }
        DecompInterface di = new DecompInterface();
        di.openProgram(currentProgram);
        Set<Address> done = new LinkedHashSet<>();
        ArrayDeque<Address> order = new ArrayDeque<>(queue.keySet());
        try (PrintWriter w = new PrintWriter(new FileWriter(args[0]))) {
            while (!order.isEmpty() && done.size() < 400) {
                Address entry = order.poll();
                if (!done.add(entry)) {
                    continue;
                }
                int level = queue.get(entry);
                Function f = functionAt(entry);
                if (f == null) {
                    w.println("// ==== " + entry + ": no function ====");
                    continue;
                }
                if (level < depth) {
                    for (Instruction in :
                         currentProgram.getListing().getInstructions(f.getBody(), true)) {
                        for (Reference r : in.getReferencesFrom()) {
                            if (r.getReferenceType().isCall() && !queue.containsKey(r.getToAddress())) {
                                queue.put(r.getToAddress(), level + 1);
                                order.add(r.getToAddress());
                            }
                        }
                    }
                }
                DecompileResults r = di.decompileFunction(f, 120, monitor);
                w.println("// ==== " + entry + " " + f.getName() + " (level " + level + ") ====");
                if (r != null && r.decompileCompleted() && r.getDecompiledFunction() != null) {
                    w.println(r.getDecompiledFunction().getC());
                } else {
                    w.println("// decompilation failed: " + (r == null ? "null" : r.getErrorMessage()));
                }
            }
        }
        di.dispose();
        println("wrote " + args[0] + ": " + done.size() + " functions");
    }
}
