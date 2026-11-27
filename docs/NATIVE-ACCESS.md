# DP‑UB820 native‑code access, display plane control, and the video‑pipeline walls

Method, findings and structures from reverse‑engineering a **rooted** Panasonic DP‑UB820 (Socionext
UniPhier‑class SoC, aarch64 kernel 4.4.x, **armhf** userspace, glibc 2.20; video decode = VeriSilicon
**Hantro G2** at `/dev/hantrodec`). This documents how to run your own native code on the box and drive
the closed vendor AV/graphics stack, what that unlocks, and the two proprietary‑format walls that remain.
No keys, firmware, or vendor binaries are included here.

---

## Status at a glance

**Working**
- Compile native **armhf** code and run it on the device.
- Call the device's **closed vendor libraries** from our own binary (dlopen + dlsym + cribbed signatures).
- Call HDMI/AV vendor functions (e.g. read the connected sink's audio/EDID capabilities).
- **Write** pixels to the 4K OSD graphics plane from our own code (bypassing the compositor).
- **Read** the OSD plane back → pixel‑exact screenshots of the live UI / menus / playback overlays.
- Live discovery of plane physical addresses (they move on every UI transition).
- Full map of the video decode→display pipeline, confirmed on live hardware (USB file **and** AACS disc).

**In progress / open**
- **Persistent free‑plane overlay** — we can write to a free OSD plane and the enable calls all succeed,
  but it doesn't composite yet (needs a VOC mixer plane‑select/layer call we haven't identified). See §5.
- A native media player (feed `/dev/hantrodec`, drive the `avout` video plane).

**Blocked (by proprietary format, not by access)**
- Rendering *someone else's* decoded video frame — the data is readable but in proprietary layouts
  (Hantro RFC "DTRC" compression + a non‑standard VOC output tiling). See the
  [open‑question write‑up](./decoded-frame-format-open-question.md).

---

## 1. Toolchain

Cross‑compile with **zig** — it matches the device exactly (hard‑float armhf, glibc 2.20):
```
zig cc -target arm-linux-gnueabihf.2.20 x.c -o x -ldl
# ELF flags 0x5000400 (hard-float ABI), interp /lib/ld-linux-armhf.so.3 — byte-exact with the device
```
Distro ARM cross‑gccs are typically **soft‑float and/or missing a sysroot**; don't fight them, use zig.

**Loader gotcha (important):** on this device's old `ld-2.20`, a binary that references the libc
**`stdout`/`stderr` data symbols** (via `printf` to a non‑tty, `setvbuf`, `fprintf(stderr,…)`) **crashes
at load**, in `_dl_runtime_resolve` *before* `main` runs. Log with the **`write(2)`** syscall only. Plain
`printf` to stdout is fine (it needs no data‑symbol copy‑relocation).

Device shell is **`sash`** (mangles nested quotes) with **no busybox applets on PATH** — drive it by
piping a script into `/bin/busybox ash -s` and prefix every applet with `/bin/busybox`.

## 2. The native‑code → closed‑stack method

Don't reimplement vendor logic — **call the device's own libraries**:
1. `dlopen` the vendor `.so`, `dlsym` the export.
2. **Recover the C signature from a real caller**: `objdump -d` an existing vendor binary, find the
   `blx … <Fn@plt>` site, read the register setup before it. Reference callers are the stock AV binaries.
3. Note the platform convention: most vendor functions carry a trailing `(const char *__FILE__, int
   __LINE__)` debug pair — it's just logging.

Vendor libs are either **thin RPC wrappers** to running daemons (via an RPC/event lib) or **in‑process
shims** that talk to kernel bridge nodes under `/dev/iosc/*`. Key access facts on a rooted unit:
`/dev/dtvrec/ucd`, `/dev/dtv/avout*`, `/dev/exmem` and `/dev/hantrodec` are **non‑exclusive and world‑
readable/writable**, and the **secure world is disabled** (`optee=0`) — there is **no hardware‑secured
video memory**. So ownership of the AV pipeline is enforced only by userspace convention, not the kernel.

## 3. Display architecture (what actually reaches the screen)

- `/dev/fb0..fb6` are **dummy** simple‑framebuffers (16×8) — ignore them.
- The real compositor is a **gserver** process: X11‑like, over a world‑connectable UNIX socket, with a
  client library (`Ti*` API). It owns the primary OSD plane.
- **Independent client windows are off‑screen by design** — a non‑owner process gets a display the
  compositor does not scan out (verified by freezing the UI: our client window never appeared). So the
  "polite" client path is a dead end for getting your own pixels on the output.
- The OSD planes are real HW planes programmed via the vendor AV lib. There are three local‑OSD planes
  (**LOSD0/1/2**); the UI uses **LOSD1**, leaving **LOSD0** (32‑bit ARGB) and **LOSD2** (8‑bit) free.
  Plane‑table indices: LOSD0=7, LOSD1=8, LOSD2=9; compact OSD indices 0/1/2. OSD is 1920×1080 ARGB8888
  (stride 7680), hardware‑scaled to the 4K HDMI output.

### Writing pixels directly (works)
The OSD plane VRAM lives in physical DRAM reachable via `/dev/exmem` (`mmap` only — these mem‑driver nodes
do **not** support `read()`/`dd`). `mmap` the plane's current physical buffer and write ARGB8888; it shows
immediately because it's the scanned‑out buffer, and persists until the compositor next repaints (it's
damage‑based). **Plane buffers are re‑allocated on every UI transition — always re‑discover the address**
(via the vendor mem‑info getter) instead of hard‑coding it. Offsets above 2 GB need `mmap64` (armhf off_t
is 32‑bit).

### Reading pixels (works)
`mmap` the plane read‑only, dump the bytes, and interpret ARGB32 little‑endian (byte order B,G,R,A) → RGB.
This yields exact screenshots of whatever OSD is on screen. The UI plane is double‑buffered — capture both
halves and use the one holding the current frame.

## 4. HDMI / AV vendor calls (works)

The HDMI library is a set of RPC wrappers to a running HDMI server. A safe, confirmed sequence to read the
connected sink's capabilities: open an RPC channel → open an HDMI handle → call a sink‑info getter → close.
All return success and return real EDID‑derived data. The exact call shapes are cribbed from a stock
binary whose own debug tag shows it is a non‑disruptive capability probe, so replicating it is safe.
(Mis‑signatured calls can crash the HDMI server, but the platform watchdog restarts it and the unit
self‑heals.)

## 5. Free‑plane overlay — SOLVED, see OVERLAY-AND-CLOCK.md

> **Update 2026-11-27:** solved. Plane 7 is the live UI plane and `Set_Addr` takes bus addresses
> (phys − 0xC0000000). Working overlay + persistent clock: [OVERLAY-AND-CLOCK.md](OVERLAY-AND-CLOCK.md).


Goal: a **persistent** overlay that composites over the live UI without fighting the UI's repaints — by
using a **free** OSD plane (LOSD0) instead of the UI's plane.

What works: the free plane is allocated (32‑bit ARGB, 1920×1080), we can write our content into its VRAM
and read it straight back. The enable sequence —
`AVIO_UP_Open` → `AVIO_UP_GFX_Set_Addr(plane, {mode, front_phys, mode, back_phys})` →
`AVIO_UP_GFX_Set_OSDAlphaPlane(plane, {…})` → `AVIO_UP_GFX_Disp(on, osd_idx, sub)` — **all return success,
but the plane does not composite to the output.** So a *cold* OSD plane needs more than
Set_Addr+alpha+Disp; the missing step is most likely a **VOC mixer plane‑select / layer** call (candidates
in the vendor lib: `ASYNCMIX_PLANESEL*`, the `*Mixer*` family). The `Set_Addr` descriptor is confirmed to
be four words `{mode(≤1), phys(128‑aligned), mode, phys}` (the two double‑buffer addresses).

If you know the UniPhier/VOC mixer layer‑enable path, that's the piece that unlocks a persistent overlay.

## 6. Video pipeline & the two format walls

```
HEVC ES → /dev/dtv/videoN → Hantro G2 (/dev/hantrodec) → DTRC-compressed refs
        → VPE (decompress + scale) → VOC output buffer → VOC compositor → HDMI
```
Decode control is the vendor `AVD_*` / `AVIO_UP_Video_*` / `_MOC_*` families; frame addresses are published
in an AVIO shared‑memory block and read live. Players: a file player for USB media, a separate Blu‑ray
engine + trusted‑task cluster for AACS discs (content protection is on the **keys** and the **HDMI output
(HDCP)** — **not** on the frame buffers in RAM).

Two walls, both **format**, not access (decoded frames — including from AACS discs — sit in normal readable
DRAM because `optee=0`):
1. **DTRC** = Panasonic/Socionext name for Hantro G2 **Reference‑Frame Compression** — proprietary and
   undocumented; the mainline kernel never software‑decompresses it.
2. The **VOC post‑processor output** is a `P010`‑sized (3840×2208, 10‑bit, 4:2:0 semi‑planar) buffer but
   in a **non‑standard tiled** layout — linear and all common Hantro tile geometries fail to de‑scramble
   it (correct black bars/brightness, scrambled picture).

Full details and an **open question for anyone who knows the Socionext/Hantro internals** are in
[`decoded-frame-format-open-question.md`](./decoded-frame-format-open-question.md).

## 7. Example tooling

Minimal, self‑contained C/shell/Python examples of each method (vendor‑call loop, OSD read, OSD write,
plane‑address discovery, VOC frame capture) are under [`../examples/native/`](../examples/native/). They
contain **no keys and no vendor binaries** — they `dlopen` the libraries that are already on the device.
