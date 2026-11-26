# Native-access examples (DP-UB820)

Minimal, self-contained examples for running your own `armhf` code on a rooted DP-UB820 and driving the
closed vendor AV/graphics stack. See [`../../docs/NATIVE-ACCESS.md`](../../docs/NATIVE-ACCESS.md) for the
full write-up. No keys or vendor binaries here — these `dlopen` the libraries already on the device.

Build: `zig cc -target arm-linux-gnueabihf.2.20 <file>.c -o <file> -ldl` (install zig via `setup-toolchain.sh`).
Log with `write(2)` only — referencing libc `stdout`/`stderr` globals crashes at load on this `ld-2.20`.

| file | what it shows |
|---|---|
| `setup-toolchain.sh` | install the zig cross-toolchain |
| `hdmiprobe.c` | dlopen a vendor lib + resolve its exports |
| `hdmiaudio.c` | full vendor-call loop: RPC_start → open → getter → close |
| `planeprobe.c` | discover live OSD plane physical addresses (getters only) |
| `ucdprobe.c` | test that kernel AV nodes are non-exclusive for a root process |
| `exmemread.c` | read the OSD plane VRAM (screenshot the live UI) |
| `exmemdraw.c` | write ARGB pixels to the live OSD plane |
| `tiwintest.c`, `tiwinsweep.c` | gserver client-window experiments (shows why that path is off-screen) |
| `losd0draw.c`, `gfxenable2.c` | free-plane overlay attempt (open item: plane enable) |
| `capture_p010.sh`, `render_frame.py` | locate + dump + attempt-render the VOC video output buffer |
