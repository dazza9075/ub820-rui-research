import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.*;
import ghidra.program.model.listing.*;
import ghidra.program.model.address.*;
import ghidra.program.model.symbol.*;
public class FindKey4 extends GhidraScript {
    public void run() throws Exception {
        DecompInterface d = new DecompInterface(); d.openProgram(currentProgram);
        FunctionManager fm = currentProgram.getFunctionManager();
        ReferenceManager rm = currentProgram.getReferenceManager();
        AddressSpace sp = currentProgram.getAddressFactory().getDefaultAddressSpace();
        // decompile the module-decrypt orchestrator and its callers
        long[] funcs = {0x399a0L};
        java.util.LinkedHashSet<Function> set = new java.util.LinkedHashSet<>();
        for (long a : funcs){ Function f=fm.getFunctionContaining(sp.getAddress(a)); if(f!=null) set.add(f); }
        // add callers of 0x399a0
        Function base = fm.getFunctionContaining(sp.getAddress(0x399a0L));
        if (base!=null) for (Function c: base.getCallingFunctions(monitor)) set.add(c);
        for (Function f : set){
            println("\n============ "+f.getName()+" @ "+f.getEntryPoint()+" ============");
            DecompileResults r=d.decompileFunction(f,120,monitor);
            if(r!=null&&r.decompileCompleted()) println(r.getDecompiledFunction().getC());
        }
    }
}
