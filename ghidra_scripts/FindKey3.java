import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.*;
import ghidra.program.model.listing.*;
import ghidra.program.model.address.*;
import ghidra.program.model.symbol.*;

public class FindKey3 extends GhidraScript {
    public void run() throws Exception {
        DecompInterface d = new DecompInterface(); d.openProgram(currentProgram);
        FunctionManager fm = currentProgram.getFunctionManager();
        ReferenceManager rm = currentProgram.getReferenceManager();
        AddressSpace sp = currentProgram.getAddressFactory().getDefaultAddressSpace();
        // decompile the accessor + key-setter + decrypt
        long[] funcs = {0x45904L, 0x458f8L, 0x398ecL, 0x393beL};
        for (long a : funcs) {
            Function f = fm.getFunctionContaining(sp.getAddress(a));
            if (f == null) { println("no func @ "+Long.toHexString(a)); continue; }
            println("\n============ "+f.getName()+" @ "+f.getEntryPoint()+" ============");
            DecompileResults r = d.decompileFunction(f, 90, monitor);
            if (r!=null && r.decompileCompleted()) println(r.getDecompiledFunction().getC());
        }
        // data refs TO the native funcs (registration tables) -> look around for name strings
        for (long a : new long[]{0x398ecL, 0x393beL}) {
            println("\n#### references to "+Long.toHexString(a)+" ####");
            for (Reference ref : rm.getReferencesTo(sp.getAddress(a))) {
                println("  from "+ref.getFromAddress()+" type="+ref.getReferenceType());
            }
            // also thumb addr
            for (Reference ref : rm.getReferencesTo(sp.getAddress(a|1))) {
                println("  (thumb) from "+ref.getFromAddress()+" type="+ref.getReferenceType());
            }
        }
    }
}
