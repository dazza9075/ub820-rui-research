import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.*;
import ghidra.program.model.listing.*;
import ghidra.program.model.address.*;
public class FindReg extends GhidraScript {
    public void run() throws Exception {
        DecompInterface d=new DecompInterface(); d.openProgram(currentProgram);
        FunctionManager fm=currentProgram.getFunctionManager();
        AddressSpace sp=currentProgram.getAddressFactory().getDefaultAddressSpace();
        // function containing the registration code at 0x39a90
        for (long a : new long[]{0x39a90L, 0x482dcL}) {
            Function f=fm.getFunctionContaining(sp.getAddress(a));
            if(f==null){ println("no func @ "+Long.toHexString(a)); continue; }
            println("\n============ "+f.getName()+" @ "+f.getEntryPoint()+" ============");
            DecompileResults r=d.decompileFunction(f,120,monitor);
            if(r!=null&&r.decompileCompleted()) println(r.getDecompiledFunction().getC());
        }
    }
}
