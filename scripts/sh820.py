#!/usr/bin/env python3
# UB820 shell: connect via telnet (sash), immediately exec busybox ash (full syntax),
# run the command, print output. Usage: sh820.py <ip> '<shell command>'
import socket, sys, time
ip  = sys.argv[1] if len(sys.argv) > 1 else "10.82.3.224"
cmd = sys.argv[2] if len(sys.argv) > 2 else "id"
IAC,DONT,DO,WONT,WILL = 255,254,253,252,251
def neg(data):
    out=bytearray(); i=0
    while i < len(data):
        if data[i]==IAC and i+2 < len(data):
            v,o=data[i+1],data[i+2]
            if   v==DO:   out+=bytes([IAC,WONT,o])
            elif v==WILL: out+=bytes([IAC,DONT,o])
            i+=3; continue
        i+=1
    return bytes(out)
TYPED='EN"D"MARK_9ZQ7'; CLEAN='ENDMARK_9ZQ7'
s=socket.socket(); s.settimeout(8); s.connect((ip,23)); s.settimeout(3)
buf=b""
try:
    for _ in range(15):
        d=s.recv(4096)
        if not d: break
        r=neg(d)
        if r: s.sendall(r)
        buf+=d
        if b">" in d: break
        time.sleep(0.1)
except socket.timeout: pass
# drop sash -> busybox ash (full shell: pipes, ; , redirection)
s.sendall(b"/bin/busybox sh\n"); time.sleep(0.5)
# make a convenient PATH incl busybox applets via symlink dir? just run cmd
s.sendall((cmd+"\n").encode()); time.sleep(0.2)
s.sendall(("/bin/busybox echo "+TYPED+"\n").encode())
out=b""; s.settimeout(float(sys.argv[3]) if len(sys.argv)>3 else 10)
try:
    while True:
        d=s.recv(65536)
        if not d: break
        out+=d
        if CLEAN.encode() in out: break
except socket.timeout: pass
s.close()
sys.stdout.write((buf+out).decode(errors="replace").replace(TYPED,""))
