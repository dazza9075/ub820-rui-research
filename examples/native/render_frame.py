#!/usr/bin/env python3
# render_frame.py -- decode a dumped UB820 VPE/VOC output buffer to PNG.
# Handles: linear P010, 4x4-tiled P010 (P010_4L4), linear NV12(8b), 4x4-tiled NV12 (NV12_4L4).
# Also an "image-likeness" score to auto-pick the right interpretation.
# Run with: python3 -I render_frame.py <raw> <mode> <W> <Hpad> [out.png]
import sys, struct, numpy as np
from PIL import Image

def load_u16le(buf, n):
    return np.frombuffer(buf[:n*2], dtype='<u2').astype(np.uint16)

def detile_4x4(flat, W, H, sample_bytes):
    # flat: 1-D array of samples in tile raster order (tiles 4x4, tiles laid row-major)
    # returns HxW array. Expects len>=(W*H). stride(pixels)=W (assumed tile-aligned)
    tpr = W // 4
    tph = H // 4
    need = tpr*tph*16
    if flat.size < need:
        flat = np.pad(flat, (0, need-flat.size))
    f = flat[:need].reshape(tph, tpr, 4, 4)       # [tileY, tileX, row, col]
    img = f.transpose(0,2,1,3).reshape(H, W)      # place each tile's 4x4 block
    return img

def luma_linear_p010(buf, W, Hpad):
    y = load_u16le(buf, W*Hpad).reshape(Hpad, W)
    return (y >> 6) & 0x3ff   # 10-bit in high bits

def luma_tiled_p010(buf, W, Hpad):
    y = load_u16le(buf, W*Hpad)
    y = (y >> 6) & 0x3ff
    return detile_4x4(y, W, Hpad, 2)

def luma_linear_nv12(buf, W, Hpad):
    y = np.frombuffer(buf[:W*Hpad], dtype=np.uint8).astype(np.uint16)
    return y.reshape(Hpad, W)

def luma_tiled_nv12(buf, W, Hpad):
    y = np.frombuffer(buf[:W*Hpad], dtype=np.uint8).astype(np.uint16)
    return detile_4x4(y, W, Hpad, 1)

def image_likeness(img10):
    # Real photographic luma: smooth locally -> small mean abs horizontal/vertical gradient
    # relative to overall dynamic range. Lower = more image-like. Noise/compressed -> ~0.5*range.
    a = img10.astype(np.float32)
    rng = a.max()-a.min()
    if rng < 1: return 1e9
    gh = np.abs(np.diff(a, axis=1)).mean()
    gv = np.abs(np.diff(a, axis=0)).mean()
    return (gh+gv)/2.0 / rng

def to_png(img10, path, bits=10):
    a = img10.astype(np.float32)
    mx = (1<<bits)-1
    g = np.clip(a/mx*255.0, 0, 255).astype(np.uint8)
    Image.fromarray(g, 'L').save(path)

MODES = {
 'lin-p010': luma_linear_p010,
 'tile-p010': luma_tiled_p010,
 'lin-nv12': luma_linear_nv12,
 'tile-nv12': luma_tiled_nv12,
}

def selftest():
    # Build a synthetic 10-bit gradient, tile it, de-tile, confirm round-trip.
    W,H=64,32
    base=np.zeros((H,W),np.uint16)
    for r in range(H):
        for c in range(W):
            base[r,c]=((r*8+c*4)&0x3ff)
    # tile it (inverse of detile): produce tile-raster flat
    tpr,tph=W//4,H//4
    f=base.reshape(tph,4,tpr,4).transpose(0,2,1,3).reshape(-1)
    # pack as P010 (<<6)
    packed=(f.astype(np.uint16)<<6)
    buf=packed.tobytes()
    out=luma_tiled_p010(buf,W,H)
    ok=np.array_equal(out,base)
    print("SELFTEST detile P010_4L4 round-trip:", "PASS" if ok else "FAIL")
    # linear sanity
    lin=(base.reshape(-1).astype(np.uint16)<<6).tobytes()
    out2=luma_linear_p010(lin,W,H)
    print("SELFTEST linear P010:", "PASS" if np.array_equal(out2,base) else "FAIL")
    # likeness: smooth gradient should score low, random high
    rnd=np.random.randint(0,1024,(H,W)).astype(np.uint16)
    print(f"SELFTEST likeness gradient={image_likeness(base):.3f} random={image_likeness(rnd):.3f} (gradient should be << random)")

if __name__=='__main__':
    if len(sys.argv)==2 and sys.argv[1]=='selftest':
        selftest(); sys.exit(0)
    raw,mode,W,Hpad=sys.argv[1],sys.argv[2],int(sys.argv[3]),int(sys.argv[4])
    out=sys.argv[5] if len(sys.argv)>5 else raw+'.'+mode+'.png'
    buf=open(raw,'rb').read()
    img=MODES[mode](buf,W,Hpad)
    print(f"{mode} {W}x{Hpad}: likeness={image_likeness(img):.4f} min={int(img.min())} max={int(img.max())} mean={float(img.mean()):.1f}")
    to_png(img,out, bits=10 if 'p010' in mode else 8)
    print("wrote",out)
