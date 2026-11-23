import sys, struct
from elftools.elf.elffile import ELFFile
from capstone import Cs, CS_ARCH_ARM, CS_MODE_ARM
def build(path):
    data=open(path,'rb').read(); elf=ELFFile(open(path,'rb'))
    secs={s.name:(s['sh_addr'],s['sh_offset'],s['sh_size']) for s in elf.iter_sections()}
    dynsym=elf.get_section_by_name('.dynsym'); relplt=elf.get_section_by_name('.rel.plt')
    got_name={r['r_offset']:dynsym.get_symbol(r['r_info_sym']).name for r in relplt.iter_relocations()}
    a,o,sz=secs['.plt']; md=Cs(CS_ARCH_ARM,CS_MODE_ARM)
    ins=list(md.disasm(data[o:o+sz],a)); plt_name={}
    i=0
    while i < len(ins)-2:
        x,y,z=ins[i],ins[i+1],ins[i+2]
        if x.mnemonic=='add' and 'ip, pc' in x.op_str and z.mnemonic=='ldr' and z.op_str.startswith('pc'):
            def imm(s):
                try: return int(s.split('#')[-1].replace(']','').replace('!',''),0)
                except: return 0
            got=(x.address+8)+imm(x.op_str)+imm(y.op_str)+imm(z.op_str)
            nm=got_name.get(got)
            if nm: plt_name[x.address]=nm
            i+=3
        else:
            i+=1
    return plt_name, secs, data
if __name__=='__main__':
    pn,secs,data=build(sys.argv[1])
    for a,n in sorted(pn.items()):
        if any(x in n for x in('EVP_BytesToKey','EVP_DecryptInit','EVP_aes','EVP_DecryptUpdate','CipherInit')):
            print(f"{a:#x} {n}")
