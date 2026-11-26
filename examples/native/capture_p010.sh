#!/usr/bin/env bash
# capture_p010.sh -- during ACTIVE 4K playback, locate + dump the linear P010 VPE/VOC
# output buffer from UB820 RAM and render it to PNG.  READ-ONLY on device.
#
# Pipeline recap (confirmed 2026-11-26):
#   HANTRO G2 -> DTRC-compressed refs (0x3f4800-byte bufs, ~0xd4/0xd5 region, PROPRIETARY)
#               -> VPE decompresses+scales -> LINEAR P010 4:2:0 output (0x1842000-byte bufs)
#               -> VOC composites -> HDMI.
#   The 0x1842000 (25,436,160) buffer == P010 3840x2208: Y stride 7680B (3840px*2),
#   then CbCr interleaved at half height. Addresses are allocated per-session -> read live.
set -e
HOST=${HOST:-root@DEVICE_IP}
KEY=${KEY:-$HOME/.ssh/ub820_key}
SSH="ssh -i $KEY -o ConnectTimeout=10 -o StrictHostKeyChecking=no $HOST"
OUT=${1:-frame}

echo "[*] reading pavio_shm addr table..."
{ echo 'BB=/bin/busybox'; echo '/tmp/exmemread 0x81ed0000 0x20000 2>/dev/null | $BB base64'; } \
  | $SSH '/bin/busybox ash -s' 2>/dev/null | base64 -d > /tmp/shm.$$.raw

ADDR=$(python3 -I - "/tmp/shm.$$.raw" <<'PY'
import struct,sys
d=open(sys.argv[1],'rb').read()
w=lambda o: struct.unpack_from('<I',d,o)[0]
# addr table lives ~0x1e1d0..0x1e320; entries are (addr, 0, size). Find the P010 buffer:
# size == 0x1842000 (P010 4K). Take the first such addr.
found=[]
for o in range(0x1e1d0,0x1e340,4):
    v=w(o)
    if v==0x1842000:
        # back up to the nearest preceding page-aligned addr
        for b in range(o-4,o-0x14,-4):
            a=w(b)
            if a>=0x80000000 and a<0xfc000000 and (a&0xfff)==0:
                found.append(a); break
print('%#x'%found[0] if found else '')
PY
)
rm -f /tmp/shm.$$.raw
[ -z "$ADDR" ] && { echo "[!] no 0x1842000 P010 buffer in addr table -- is 4K playback ACTIVE?"; exit 1; }
echo "[*] P010 output buffer at $ADDR ; dumping 0x1842000 bytes (read-only)..."
{ echo 'BB=/bin/busybox'; echo "/tmp/exmemread $ADDR 0x1842000 2>/dev/null | \$BB base64"; } \
  | $SSH '/bin/busybox ash -s' 2>/dev/null | base64 -d > "$OUT.p010.raw"
echo "[*] dumped $(wc -c < "$OUT.p010.raw") bytes -> $OUT.p010.raw"
echo "[*] rendering (linear P010 first)..."
D=$(dirname "$0")
python3 -I "$D/render_frame.py" "$OUT.p010.raw" lin-p010 3840 2208 "$OUT.lin.png"
python3 -I "$D/render_frame.py" "$OUT.p010.raw" tile-p010 3840 2208 "$OUT.tile.png"
echo "[*] done. Compare $OUT.lin.png vs $OUT.tile.png -- the lower-likeness / coherent one is the frame."
