import sys,struct
from elftools.elf.elffile import ELFFile
from capstone import Cs,CS_ARCH_ARM,CS_MODE_ARM
data=open(sys.argv[1],'rb').read(); elf=ELFFile(open(sys.argv[1],'rb'))
secs={s.name:(s['sh_addr'],s['sh_offset'],s['sh_size']) for s in elf.iter_sections()}
dynsym=elf.get_section_by_name('.dynsym'); relplt=elf.get_section_by_name('.rel.plt')
got_name={r['r_offset']:dynsym.get_symbol(r['r_info_sym']).name for r in relplt.iter_relocations()}
def va2off(va):
    for a,o,sz in secs.values():
        if a and a<=va<a+sz: return o+(va-a)
md=Cs(CS_ARCH_ARM,CS_MODE_ARM)
for stub in (0x1b1e0,0x1b1f8,0x19bcc,0x19bc0):
    o=va2off(stub); ins=list(md.disasm(data[o:o+12],stub))
    def imm(s):
        try:return int(s.split('#')[-1].replace(']','').replace('!',''),0)
        except:return 0
    if len(ins)==3 and ins[0].mnemonic=='add':
        got=(ins[0].address+8)+imm(ins[0].op_str)+imm(ins[1].op_str)+imm(ins[2].op_str)
        print(f"stub {stub:#x}: got={got:#x} name={got_name.get(got)}")
        for x in ins: print(f"    {x.address:#x} {x.mnemonic} {x.op_str}")
    else:
        print(f"stub {stub:#x}: not a standard stub:")
        for x in ins: print(f"    {x.address:#x} {x.mnemonic} {x.op_str}")
# also: list ALL stubs sorted with names, show the few around BytesToKey
print("=== full ordered stub table (name: stub) around crypto ===")
a,o,sz=secs['.plt']; allins=list(md.disasm(data[o:o+sz],a)); i=0; tbl=[]
while i<len(allins)-2:
    x,y,z=allins[i],allins[i+1],allins[i+2]
    if x.mnemonic=='add' and 'ip, pc' in x.op_str:
        def imm(s):
            try:return int(s.split('#')[-1].replace(']','').replace('!',''),0)
            except:return 0
        got=(x.address+8)+imm(x.op_str)+imm(y.op_str)+imm(z.op_str)
        tbl.append((x.address,got_name.get(got))); i+=3
    else: i+=1
for addr,nm in tbl:
    if nm and ('BytesToKey' in nm or 'DecryptInit' in nm): print(f"  {addr:#x} {nm}")
