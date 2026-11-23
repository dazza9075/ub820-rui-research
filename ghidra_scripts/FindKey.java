// Decompile every function that references the OpenSSL key/cipher-init funcs,
// plus their callers (2 levels up), so the AES key origin is visible as C.
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.*;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.*;
import java.util.*;

public class FindKey extends GhidraScript {
    public void run() throws Exception {
        DecompInterface d = new DecompInterface();
        d.openProgram(currentProgram);
        String[] targets = {"EVP_DecryptInit","EVP_DecryptInit_ex","EVP_CipherInit_ex",
                            "EVP_BytesToKey","EVP_DecryptUpdate","AES_set_decrypt_key"};
        SymbolTable st = currentProgram.getSymbolTable();
        FunctionManager fm = currentProgram.getFunctionManager();
        ReferenceManager rm = currentProgram.getReferenceManager();

        Set<Function> level0 = new LinkedHashSet<>();
        for (String t : targets) {
            for (Symbol s : st.getGlobalSymbols(t)) {
                for (Reference r : rm.getReferencesTo(s.getAddress())) {
                    Function f = fm.getFunctionContaining(r.getFromAddress());
                    if (f != null) level0.add(f);
                }
            }
        }
        println("### functions directly calling EVP key/cipher init: " + level0.size());

        // add callers up to 2 levels
        Set<Function> all = new LinkedHashSet<>(level0);
        Set<Function> frontier = new LinkedHashSet<>(level0);
        for (int lvl = 0; lvl < 2; lvl++) {
            Set<Function> next = new LinkedHashSet<>();
            for (Function f : frontier)
                for (Function c : f.getCallingFunctions(monitor))
                    if (all.add(c)) next.add(c);
            frontier = next;
        }
        println("### total functions to decompile (with 2 caller levels): " + all.size());

        for (Function f : all) {
            println("\n==================== " + f.getName() + " @ " + f.getEntryPoint() + " ====================");
            DecompileResults res = d.decompileFunction(f, 90, monitor);
            if (res != null && res.decompileCompleted())
                println(res.getDecompiledFunction().getC());
            else
                println("  (decompile failed)");
        }
    }
}
