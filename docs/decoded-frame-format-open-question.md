# DP‑UB820 (Socionext UniPhier + VeriSilicon Hantro G2): decoded‑video‑frame memory format — findings & open question

**Platform:** Panasonic DP‑UB820 4K UHD Blu‑ray player. SoC is a Socionext/Panasonic UniPhier‑class part
(model string `A8641`/`A8640`), aarch64 kernel 4.4.x, armhf userspace, glibc 2.20. Video decode HW =
**VeriSilicon/Google Hantro G2** VPU exposed at `/dev/hantrodec` (major 235, world rw). Device is rooted;
the secure world is **disabled at boot** (`optee=0`), so there is **no hardware‑secured video memory** —
every frame/plane buffer is mappable from normal DRAM via `/dev/exmem` / `/dev/mem`.

We can already read the **OSD/graphics plane** perfectly (linear ARGB8888 — we reconstruct the UI,
menus, and playback overlays pixel‑exact from RAM). The thing we **cannot** render is a **decoded video
frame**, and the blocker is purely the pixel **format**, not access. This note documents what we found
and poses the open question.

---

## The pipeline (confirmed by disasm of `libavio`/`libavd` + live `/proc/<pid>/maps`)

```
HEVC ES  ──►  Hantro G2 (/dev/hantrodec)  ──►  DTRC‑compressed reference frames (proprietary)
                                                     │
                                          VPE post‑processor (decompress + scale/convert)
                                                     │
                                                     ▼
                                   VOC "display" output buffer  ──►  VOC compositor ──► HDMI
```

Control/metadata lives in an AVIO shared block (`pavio_shm`, UCDD mem_id 109) mapped into every AV
process (`mocserver`, `apl_bdv`/`fmpsrv`, `gserver`) at a per‑boot physical address (seen at
`0x81ed0000`). Frame buffer physical addresses are allocated per session and are read live from a table
inside that block.

Test clip: **3840×2160/24p, HDR10 (PQ/SMPTE‑2084), BT.2020, YCbCr 4:2:0, 10‑bit, HEVC**.

---

## Wall #1 — the Hantro reference frames are "DTRC" compressed (proprietary)

`libavio` is full of **DTRC** symbols: `avio_set_dtrctable`, `avio_print_dtrctable`, `DTRCCONFIG`,
`DTRCVERSION`, `DTRCINTEN`, `DTRC_OBJECT_END`, `VPE_IF_INPUT_SEL_DTRC`,
`VPE_IF_INPUT_DTRC_TABLE_ADDR_Y/_C`, `vpe_dtrc_object_addr/size`. A per‑frame `dtrc_sw` flag (0/1) toggles
it.

**DTRC = "Decoder Tiled Reference Compression"** — the Panasonic/Socionext name for VeriSilicon Hantro
G2 **Reference‑Frame Compression (RFC)**. This is confirmed by the mainline Linux Hantro driver, which
treats the compressed format as **unknown/undocumented** and only uses compression when a *post‑processor*
output format (NV12/NV15) is selected — i.e. the kernel **never decompresses RFC in software**; it hands
the addresses to the VPU/post‑processor and consumes the linear/tiled PP output instead. `hantro_hw.h`
gives only the metadata granularity (16‑byte compression‑table units; luma 8×8 coded blocks; chroma two
8×4 blocks, Cb then Cr) — **not the meaning of the bits**.

On device the DTRC references are `0x3f4800`‑byte (4 MB) buffers (far too small for an uncompressed 4K
plane, confirming they're compressed), with separate Y/C **tile‑status tables** (Y table `0x24000`, C
table `0x12000` for 4K). Byte‑entropy of the resident buffers matches the public model (low‑entropy
tile‑status + high‑entropy compressed payload).

**→ A software DTRC decompressor is not feasible from public information** (would require clean‑room RE of
the Hantro RFC codec).

## Wall #2 — the VOC post‑processor output is *tiled* (format unknown)

The VPE emits a larger buffer that the VOC compositor scans out. Its size is **`0x1842000` =
25,436,160 bytes**, which is an **exact P010 4:2:0 fit for 3840×2208** (Y stride 7680 B × 2208 + CbCr at
half height). pitch_pixel = 3840 (128‑aligned); height padded 2160→2208. We locate it live and dump it
read‑only during playback (verified non‑zero, 10‑bit, values 0–1022).

But it is **not linear**: read as linear P010/NV12 it shows **correct letterbox black bars + brightness**
but the picture content is **tile‑scrambled** (a regular ~vertical tick texture; the user confirms the
scene is "mostly bright" while our gray regions are the scrambled bright picture). Notably the **10‑bit
value sits in the low bits (0–1023 directly), not shifted `<<6` as in standard P010.** De‑tiling with
every common Hantro layout (NV12_4L4 / P010_4L4 4×4 tiles, and 8×8/16×16/8×4/16×4/32×4/64×4) does **not**
resolve it — a bit‑exact 4×4 de‑tiler that round‑trips in self‑test produces a *sheared* image on real
data (stride/tile‑order mismatch), and no tile geometry cleanly wins on an image‑likeness score.

So even the "linear" PP output on this part is in a **non‑standard tiled layout** we haven't matched.

---

## THE OPEN QUESTION (for anyone who knows Socionext UniPhier / Hantro G2 internals)

**On this Socionext/Panasonic UniPhier + Hantro G2 platform, what is the exact byte layout of the VOC/VPE
post‑processor output buffer, and/or the Hantro G2 "DTRC"/RFC reference‑frame compression format?**

Specifics that would unlock a software reader:

1. **VOC output tiling.** A `0x1842000`‑byte, 3840×2208, 10‑bit‑in‑low‑bits, 4:2:0 semi‑planar buffer that
   is clearly tiled. What is the tile geometry and tile‑ordering (e.g. a specific Hantro `*_4L4` variant,
   a super‑tile, a VOC‑specific swizzle, or a raster‑in‑tile / tile‑in‑raster order)? Is Y tiled the same
   as CbCr? Is there row padding beyond stride 7680?
2. **DTRC/RFC format.** The Hantro G2 RFC compressed reference format + the 16‑byte tile‑status table bit
   meanings (`DTRCCONFIG`/`DTRCVERSION` semantics). Is there any public or licensable spec?
3. Any pointer to Socionext/VeriSilicon docs, a vendor BSP, or ffmpeg/gstreamer/V4L2 code that handles
   this specific PP output layout.

If you can identify the tiling of the VOC output buffer alone, that's enough — we can already locate and
dump it live; we just need to un‑swizzle it to RGB.

---

## Context / why this is only a curiosity, not a blocker

Reaching the data is **not** the problem — `optee=0` means nothing is secured in memory; we dump decoded
frames (including from AACS discs) from normal DRAM. The only locks are **proprietary formats** (DTRC
compression, VOC tiling) and the **output link** (HDCP). For building our *own* native player on this box
we write frames to the plane in a standard format we choose, so none of this is on the critical path —
this write‑up exists to close the "can we read others' decoded frames?" thread and to invite an answer
on the format.

*Findings produced by on‑device reverse‑engineering (read‑only memory dumps + static disassembly). No
proprietary code or keys are reproduced here.*
