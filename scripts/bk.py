import sys,struct,re
from plt2 import build
from capstone import Cs,CS_ARCH_ARM,CS_MODE_THUMB
plt_name,secs,data=build(sys.argv[1]); name_plt={v:k for k,v in plt_name.items()}
def va2off(va):
    for a,o,sz in secs.values():
        if a and a<=va<a+sz: return o+(va-a)
def rd(va,n):
    o=va2off(va); return data[o:o+n] if o is not None else b''
def cstr(va,m=200):
    o=va2off(va)
    if o is None: return None
    e=data.find(b'\0',o,o+m); return data[o:e] if e>0 else None
def tbt(va):
    o=va2off(va)
    if o is None or o+4>len(data): return None
    hw1,hw2=struct.unpack('<HH',data[o:o+4])
    if (hw1>>11)!=0b11110: return None
    typ=(hw2>>11)&0b11111
    blx = typ==0b11101
    if typ not in(0b11111,0b11101): return None
    S=(hw1>>10)&1;imm10=hw1&0x3ff;J1=(hw2>>13)&1;J2=(hw2>>11)&1;I1=(~(J1^S))&1;I2=(~(J2^S))&1
    off=(S<<24)|(I1<<23)|(I2<<22)|(imm10<<12)|((((hw2>>1)&0x3ff)<<2) if blx else ((hw2&0x7ff)<<1))
    if off&(1<<24): off-=(1<<25)
    base=va+4
    if blx: base&=~3
    return (base+off)&0xffffffff
ta,to,tsz=secs['.text']; BK=name_plt['EVP_BytesToKey']
sites=[va for va in range(ta,ta+tsz-4,2) if tbt(va)==BK]
md=Cs(CS_ARCH_ARM,CS_MODE_THUMB); md.detail=True
def val(va):
    b=cstr(va)
    if b and 1<len(b)<64 and all(32<=c<127 for c in b): return '"'+b.decode()+'"'
    return rd(va,16).hex()
for site in sites:
    print(f"\n===== EVP_BytesToKey @ {site:#x} =====")
    start=(site-90)&~1; o=va2off(start)
    for ins in md.disasm(data[o:o+94],start):
        if ins.address>site: break
        line=f"  {ins.address:#x}: {ins.mnemonic:7} {ins.op_str}"
        t=tbt(ins.address)
        if ins.mnemonic in('bl','blx') and t in plt_name: line+=f"  ; -> {plt_name[t]}"
        print(line)
