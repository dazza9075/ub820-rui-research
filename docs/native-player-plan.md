# DP‑UB820 — native video player: build plan (routes, API, skeleton)

Goal: a homebrew **native** media player on the UB820 that decodes a file and shows video on the HDMI
output, using the player's own decode hardware. Everything below is from static analysis of the stock
binaries (`omplayer`, `vcavsrv`, `mocserver`) and the vendor libs. **Live playback testing is PARKED** —
it needs the user present to confirm video on the output plane (I can't read the HW‑mixed output).

## Pipeline (confirmed)
```
container ──(demux)──► HEVC/H264 ES ──► decoder input ──► Hantro G2 (/dev/hantrodec) ──► DTRC refs
        ──► VPE (decompress+scale) ──► VOC output buffer ──► VOC compositor ──► HDMI
audio ──(demux)──► AC3/DTS/TrueHD/PCM ──► decode/passthrough ──► HDMI/eARC
```
HW video codecs: **H264 / HEVC / VP9 / MPEG‑2** (no AV1). Audio: AC3/EAC3/AAC native; DTS/TrueHD are
disc‑only in the stock path (would need the libavio CT_DTS producer — separate effort). optee=0 ⇒ clear
content is unprotected in memory.

## Three routes (easiest → most control)

### Route A — VcAv (feed the stock decode server) — RECOMMENDED for a first player
`omplayer` uses this. The library `libvcavif.so.0` is a thin client to the **`vcavsrv`** daemon (UNIX
socket `/tmp/vcavsrv`; vcavsrv forks on demand), which does all the AVD/AVIO/MOC decode+plane work.
API (cribbed from omplayer):
- `int type = VcavNameToType(const char *name)` — map a stream name (e.g. a `/dev/dtv/videoN`‑family
  type string) to a type id.
- `handle = VcAvOpen(const char *name, int mode)` — open a stream (returns ≥0). omplayer: name = a device/
  type string, mode = small int.
- `VcAvConnect(handle, …)` — connect/attach.
- loop: `VcAvWrite(handle, void *es, int len)` — feed elementary‑stream bytes (this is literally `write()`
  to the decoder input behind the socket). Pace with the decoder's free space.
- `VcAvCtrl(handle, int cmd, …)` — control: start / stop / flush / geometry. Observed cmd codes **5** and
  **9** in omplayer's teardown; full enum TODO (disasm `vcavsrv` `doVcAvCtrl` dispatcher).
- `VcAvClose(handle)` / `VcAvDelete(handle)` — teardown.
Pros: minimal code; vcavsrv handles decode, plane, clock, HDMI. Cons: inherits vcavsrv policy (codec
gating, DRM hooks); need to match its expected stream type.

### Route B — direct AVD_* (in‑process decode, full control)
Replicate what vcavsrv/mocserver do, in our own process (all exports present in `libavd.so`):
```
AVD_Init / AVD_InitFS                      (once)
AVD_Open(ctx, chID, typeMask)              (typeMask bit: video=1, audio=2, PG=4, …)
AVD_Resource_Alloc(...)
AVIO_UP_Open(0)                            (plane/VPE instance; BALANCE with AVIO_UP_Close — max 7!)
AVD_SetVideoCodecInf(ch, codec, &inf)      (codec enum: H264/HEVD(HEVC)/MPEG2/VC1/VP9)
AVD_SetVideoOutMode(...)                   (plane/geometry)
_MOC_ConnectOutputPath / _MOC_SetMonitorAttributes   (on‑screen size/position via mocserver RPC)
AVD_BindSync(&cfg)                         (bind A/V to master STC)
AVD_Start(...) ; AVD_Play(...)
loop: write() ES to /dev/dtv/videoN , paced by AVD_GetRestBuff(ch,&free) ; AVD_SendPCR(ch, pcr)
teardown: AVD_Stop ; AVD_Resource_Free ; AVD_Close ; AVIO_UP_Close
```
Pros: no policy middleware; we choose codec/geometry. Cons: must get the config structs right
(AVD_SetVideoCodecInf, AVD_SetVideoOutMode) — more RE; higher crash risk during bring‑up.

### Route C — direct /dev/hantrodec (public Hantro G2 API)
`libavd` opens `/dev/hantrodec` and issues standard Hantro ioctls. The Hantro G2 register/ioctl interface
is public (mainline `hantro` V4L2 driver, ffmpeg/VSI). Most work, but no vendor middleware at all. Frame
memory still via ucdd/exmem; getting frames to the plane still needs the VOC path (Route B's AVIO/MOC).
Flagged as a longer‑term spike.

## Recommended architecture for a practical homebrew player
1. **Demux + software audio with ffmpeg/libav**, cross‑compiled with zig (`zig cc -target
   arm-linux-gnueabihf.2.20`). libavformat/libavcodec are portable C and build fine; this gives us
   container parsing (mp4/mkv/ts) and software audio decode (incl. **DTS→PCM**, cheap on the CPU).
2. **Hand video ES to the HW decoder** via Route A (`VcAvWrite`) first (least code), Route B later for
   control. Pace by the decoder's free‑space query.
3. **Audio out**: PCM from ffmpeg → `AVD_StartExtPCM`/the audio path → HDMI. (Or AC3/DTS passthrough once
   the libavio producer is cracked.)
4. **A/V sync**: feed PCR/STC (`AVD_SendPCR`, `AVD_BindSync`) from the demuxer timestamps.

## Build notes / gotchas (confirmed)
- Toolchain: zig, `-target arm-linux-gnueabihf.2.20`. Log with `write(2)` only (stdout/stderr globals crash
  at load on ld‑2.20). `mmap64` for >2 GB phys.
- `AVIO_UP_Open` is refcounted, **max 7** — always balance with `AVIO_UP_Close` or you corrupt the mixer
  (black screen). Vendor fns take a trailing `(__FILE__,__LINE__)` debug pair.
- Device drive: `ssh … /bin/busybox ash -s < script`; push via base64; recover with POWEROFF/POWERON.

## Milestones (live steps marked PARKED — need user to watch the output)
1. [done, static] Full decode API mapped (VcAv + AVD_*).
2. [buildable now] Skeleton VcAv player that opens a stream, feeds a raw HEVC/H264 ES file, ctrl‑starts.
   See `poc/player_skeleton.c`.
3. [PARKED‑live] Fire the skeleton on a short raw ES → confirm video appears on HDMI.
4. [PARKED‑live] Add ffmpeg demux so it plays real containers.
5. [PARKED‑live] Audio path + A/V sync.
6. Recover the `VcAvCtrl` command enum (disasm `vcavsrv doVcAvCtrl`) + the AVD config structs (Route B).
