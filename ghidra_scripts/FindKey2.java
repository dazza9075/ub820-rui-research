import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.*;
import ghidra.program.model.listing.*;
import ghidra.program.model.address.*;
import ghidra.program.model.symbol.*;
import java.util.*;

public class FindKey2 extends GhidraScript {
    public void run() throws Exception {
        DecompInterface d = new DecompInterface();
        d.openProgram(currentProgram);
        FunctionManager fm = currentProgram.getFunctionManager();
        ReferenceManager rm = currentProgram.getReferenceManager();
        AddressSpace sp = currentProgram.getAddressFactory().getDefaultAddressSpace();

        long[] stubs = {0x19bccL, 0x1a5d4L, 0x1aab4L, 0x1b1f8L, 0x1a1d0L};
        String[] names = {"DecryptInit_ex","DecryptInit","CipherInit_ex","BytesToKey","DecryptUpdate"};
        Set<Function> seed = new LinkedHashSet<>();
        for (int i = 0; i < stubs.length; i++) {
            Address a = sp.getAddress(stubs[i]);
            int cnt = 0;
            for (Reference r : rm.getReferencesTo(a)) {
                Function f = fm.getFunctionContaining(r.getFromAddress());
                if (f != null) { seed.add(f); cnt++; }
            }
            println("refs to " + names[i] + " @ " + a + ": " + cnt);
        }
        // known crypto/js functions by address
        long[] known = {0x398ecL, 0x44ea0L, 0x471a8L, 0x433b4L, 0x1bc94L, 0x34e70L};
        for (long k : known) {
            Address a = sp.getAddress(k);
            Function f = fm.getFunctionAt(a);
            if (f == null) f = fm.getFunctionContaining(a);
            if (f != null) seed.add(f);
        }
        Set<Function> all = new LinkedHashSet<>(seed);
        for (Function f : seed)
            for (Function c : f.getCallingFunctions(monitor)) all.add(c);
        println("### decompiling " + all.size() + " functions ###");
        for (Function f : all) {
            println("\n==================== " + f.getName() + " @ " + f.getEntryPoint() + " ====================");
            DecompileResults res = d.decompileFunction(f, 90, monitor);
            if (res != null && res.decompileCompleted())
                println(res.getDecompiledFunction().getC());
            else println("  (decompile failed)");
        }
    }
}
