import sys, struct, re
from capstone import Cs, CS_ARCH_ARM, CS_MODE_THUMB
from plt2 import build
path=sys.argv[1]
plt_name, secs, data = build(path)
name_plt={v:k for k,v in plt_name.items()}
def va2off(va):
    for a,o,sz in secs.values():
        if a and a<=va<a+sz: return o+(va-a)
def read(va,n):
    o=va2off(va); return data[o:o+n] if o is not None else b''
def cstr(va,m=160):
    o=va2off(va)
    if o is None: return None
    e=data.find(b'\0',o,o+m); return data[o:e] if e>0 else None
def tbt(va):
    o=va2off(va)
    if o is None or o+4>len(data): return None
    hw1,hw2=struct.unpack('<HH',data[o:o+4])
    if (hw1>>11)!=0b11110: return None
    typ=(hw2>>11)&0b11111
    if typ==0b11111: blx=False
    elif typ==0b11101: blx=True
    else: return None
    S=(hw1>>10)&1; imm10=hw1&0x3ff; J1=(hw2>>13)&1; J2=(hw2>>11)&1
    I1=(~(J1^S))&1; I2=(~(J2^S))&1
    if not blx: off=(S<<24)|(I1<<23)|(I2<<22)|(imm10<<12)|((hw2&0x7ff)<<1)
    else: off=(S<<24)|(I1<<23)|(I2<<22)|(imm10<<12)|(((hw2>>1)&0x3ff)<<2)
    if off&(1<<24): off-=(1<<25)
    base=va+4
    if blx: base&=~3
    return (base+off)&0xffffffff
ta,to,tsz=secs['.text']
def sites_for(name):
    stub=name_plt[name]; r=[]
    for va in range(ta,ta+tsz-4,2):
        if tbt(va)==stub: r.append(va)
    return r
def showv(va):
    b=cstr(va)
    if not b: b=read(va,48)
    if b and all(32<=c<127 for c in b): return '"'+b.decode('latin1')+'"'
    return b[:48].hex()
mdt=Cs(CS_ARCH_ARM,CS_MODE_THUMB); mdt.detail=True
def analyze(name, keyreg, extra):
    for site in sites_for(name):
        print(f"\n##### {name} @ {site:#x} #####")
        start=(site-160)&~1; o=va2off(start); reg={}; stack={}
        for ins in mdt.disasm(data[o:o+164],start):
            if ins.address>site: break
            ops=ins.op_str; mn=ins.mnemonic
            try:
                if mn=='movw':
                    r,i=re.match(r'(\w+), #(\w+)',ops).groups(); reg[r]=int(i,0)
                elif mn=='movt':
                    r,i=re.match(r'(\w+), #(\w+)',ops).groups(); reg[r]=(reg.get(r,0)&0xffff)|(int(i,0)<<16)
                elif mn in('mov','movs') and '#' in ops:
                    r,i=re.match(r'(\w+), #(\w+)',ops).groups(); reg[r]=int(i,0)
                elif mn in('mov','mov.w') and ',' in ops and '#' not in ops:
                    d,s=re.match(r'(\w+), (\w+)',ops).groups()
                    if s in reg: reg[d]=reg[s]
                elif mn.startswith('ldr') and '[pc' in ops:
                    m=re.match(r'(\w+), \[pc, #(-?\d+)\]',ops)
                    if m:
                        r=m.group(1); lit=((ins.address+4)&~3)+int(m.group(2)); reg[r]=struct.unpack('<I',read(lit,4))[0]
                elif mn.startswith('str') and '[sp' in ops:
                    m=re.match(r'(\w+), \[sp(?:, #(\d+))?\]',ops)
                    if m: stack[int(m.group(2) or 0)]=reg.get(m.group(1))
            except: pass
        print("   args:", {k:hex(v) for k,v in reg.items() if k in('r0','r1','r2','r3')})
        kv=reg.get(keyreg)
        print(f"   {keyreg} -> {hex(kv) if kv else None}  {showv(kv) if kv else ''}")
        for off in (0,4,8,12):
            if off in stack and stack[off] is not None:
                v=stack[off]; print(f"   [sp+{off}] = {hex(v)}  {showv(v) if v>0x10000 else ''}")
analyze('EVP_BytesToKey','r3',None)   # r3=passphrase
analyze('EVP_DecryptInit_ex','r3',None) # r3=key
analyze('EVP_DecryptInit','r3',None)
