import sys,struct,re
from plt2 import build
from capstone import Cs,CS_ARCH_ARM,CS_MODE_THUMB
plt_name,secs,data=build(sys.argv[1])
def va2off(va):
    for a,o,sz in secs.values():
        if a and a<=va<a+sz: return o+(va-a)
ta,to,tsz=secs['.text']; text=data[to:to+tsz]
# movw Rd,#0xb094 : hw1=0xF24B, hw2 low byte 0x94 ; search \x4b\xf2 .. then 0x94
hits=[]
for m in re.finditer(b'\x4b\xf2', text):
    off=m.start(); hw2=text[off+2:off+4]
    if len(hw2)==2 and hw2[0]==0x94:
        hits.append(ta+off)
print("movw #0xb094 at:",[hex(x) for x in hits])
# Also search any code referencing string via literal pool word 0xCB094 (already none). 
# Dump function around each hit (scan back for push{...lr})
md=Cs(CS_ARCH_ARM,CS_MODE_THUMB); md.detail=True
def func_start(va):
    o=va2off(va)
    for b in range(0,2000,2):
        chunk=data[o-b:o-b+4]
        if len(chunk)<2: break
        hw=struct.unpack('<H',chunk[:2])[0]
        # push {..,lr}: T1 0xB5xx ; push.w (stmdb sp!) 0xE92D
        if (hw&0xff00)==0xb500: return va-b
        if hw==0xe92d: return va-b
    return va-0x60
for h in hits:
    fs=func_start(h)
    print(f"\n==== func @ {fs:#x} (ref {h:#x}) ====")
    o=va2off(fs)
    for ins in md.disasm(data[o:o+ (h-fs)+0x120], fs):
        line=f"  {ins.address:#x}: {ins.mnemonic:7} {ins.op_str}"
        # annotate blx plt
        m=re.match(r'#(0x[0-9a-f]+)',ins.op_str)
        if ins.mnemonic in('bl','blx') and m:
            t=int(m.group(1),0)
            if t in plt_name: line+=f"   ; -> {plt_name[t]}"
        print(line)
        if ins.address> h+0x100: break
