# VcAv client API, VcAvCtrl enum, codec enums, VcAvWrite format: static RE (2026-11-27)

Static analysis only. Nothing was run, signalled or written on the device. Binaries were copied read-only
with `busybox cat`, and their md5s match the device:

| binary | device path | md5 |
|---|---|---|
| libvcavif.so.0.0.0 | /usr_root/lib/ | 9f1c6a9d59962602fecb1f3eaeeec7f7 |
| vcavsrv | /usr2/bin/ | 035d00f576e84191dc07d4b9d4b48da2 |
| omplayer | /usr2/bin/ | cd6d9868b2ea078fedba49cda585ec3c |
| mocserver | /usr2/dtvrec/bin/ | 8bed2caec0e454fc632f41cc52af2510 |
| libavd.so | /usr2/dtvrec/lib/ | 94834b18033be9d60edf5aac7053fb1a |

Tools: capstone (Thumb) plus PLT, literal and string annotation. The scripts are in the session scratchpad
`tools/` folder (`tdis.py`, `plt.py`, `calls.py`, `findcalls.py`). All five binaries are Thumb-2. vcavsrv and
omplayer are non-PIE (absolute addresses). libvcavif and libavd are PIC (addresses are from their link base).

Legend: **[C]** = confirmed (seen in code at the cited address). **[I]** = inferred (strong evidence, not proven).

---

## 0. The big corrections to the plan / skeleton

1. **`VcAvCtrl` takes ONE argument: `int VcAvCtrl(int cmd)`.** [C] libvcavif `VcAvCtrl`@0x2a0f1834 only
   reads r0. It sends a type‑3 message on the *global control socket*, not on a stream handle. The command
   is a **server‑global "ChangeMode"** (suspend/resume, jukebox/TDAV open). It is **not** per‑stream
   start/stop. Per‑stream start/stop/pause/mute go through **`VcAvIoctl_func`** on the device handles
   (`VIDEO_IOCCONTROL` etc., §3).
2. **`VcAvOpen` ignores its 2nd argument.** [C] It only uses r0 (name). omplayer passes `0x80002`
   (`O_RDWR|O_CLOEXEC`), which is an open(2)-style shim. The handle it returns is a fresh UNIX socket fd to
   vcavsrv.
3. **`VcAvConnect(int retries)` must be called first, once per process.** [C] It opens the *control*
   socket `/tmp/vcavsrv`, and every ack is read from that socket. omplayer calls `VcAvConnect(10)`@0x24852.
4. **You cannot `VcAvWrite` to a `/dev/dtv/videoN` handle.** [C] Video-class write gives EINVAL
   (vcavsrv main@0x126c0→0x1266a). ES data goes to **`/dev/dtv/streamN`**, and the video handle is
   control-only.
5. **`VcAvWrite` paces itself.** [C] If the server replies `-28` (‑ENOSPC, "no space:%d" @0x1b89c), the
   client does `nanosleep(10 ms)` and re-sends the whole write (@0x2a0f17f2). No free-space poll is needed.
   But one write must fit in the server buffer, or it retries forever (§5).
6. **Route B: vcavsrv never calls `AVD_SetVideoCodecInf` or `AVD_SetVideoOutMode`.** [C] Its import list
   has neither. The codec is passed inside **`AVD_Start`'s video config** (§6). `AVD_SetVideoCodecInf` has
   exactly one stock caller (apl_bdv), and only for an optional HEVC extension. `AVD_SetVideoOutMode` has
   **no** stock caller in any binary we have.

---

## 1. libvcavif wire protocol (client side) [C]

Exports: `VcAvCreate VcAvDelete VcAvConnect VcAvOpen VcAvIoctl_func VcAvIoctl_funcR VcAvWrite VcAvClose
VcAvCtrl VcavNameToType VcavCalcChunkListContentInfo`.

Every request is a fixed **0x98-byte message**:

| off | meaning |
|---|---|
| +0x00 | open: device index = `name[strlen-1]-'0'` |
| +0x04 | kind: **0 OPEN, 1 WRITE, 2 IOCTL, 3 CTRL** (4 = default/unset) |
| +0x08 | open: type (VcavNameToType); write: len; ioctl: cmd; ctrl: cmd |
| +0x10.. | ioctl arg bytes (≤0x80), or ioctlR's int arg |
| +0x90 | getpid() |
| +0x94 | pthread_self() |

The request goes on the per-stream fd. The **ack (0x98 bytes, result at +0x08) is read on the global
control fd** (`WaitAck`@0x2a0f0ed8). Writes send the header followed by the raw payload on the stream fd.

| function | real signature | evidence |
|---|---|---|
| `VcAvCreate()` | fork+exec `/usr/local/bin/vcavsrv` | @0x2a0f1140 |
| `VcAvConnect(int retries)` → ctl fd | connect `/tmp/vcavsrv`, `sleep(1)` between tries; stores the global fd | @0x2a0f1244 → helper @0x2a0f0da4 |
| `VcAvDelete()` | closes the global ctl fd | @0x2a0f11e8 |
| `VcAvOpen(const char *name, int ignored)` → fd | new socket (helper(10)), sends OPEN, waits for ack. Returns -1 on unknown name ("unknown device %s") | @0x2a0f129c |
| `VcAvWrite(int fd, void *buf, int len)` → len or -1 | header (kind 1, len) + payload. On ack ‑28 it sleeps 10 ms and retries | @0x2a0f1688 |
| `VcAvIoctl_func(int fd, unsigned cmd, void *arg, int argsize)` → ack result | argsize ≤ 0x80. The reply data is copied back into `arg`. omplayer appends a `(func, line)` pair on the stack, which is ignored | @0x2a0f144c |
| `VcAvIoctl_funcR(int fd, unsigned cmd, int arg)` | arg by value at msg+0x10 | @0x2a0f15dc |
| `VcAvCtrl(int cmd)` → ack result | kind 3 on the ctl fd | @0x2a0f1834 |
| `VcAvClose(int fd)` | plain close(fd) | @0x2a0f1820 |
| `VcavNameToType(const char*)` | index in the name table (strcmp) | @0x2a0f18dc |

**Locally handled ioctls** (no request sent; the client *reads* `argsize` bytes **directly from the stream
fd**, i.e. events that the server pushes): `0x4004d604`, `0x4004d605`, `0x4008d705` [C @0x2a0f1468‑0x2a0f14be].
**`0x8004d60b` (SETCHUNKLIST)** also sends the serialised chunk arrays (`@0x2a0f1080`, sized by
`VcavCalcChunkListContentInfo`) [C].

### Name → type table (`VcavNameToType`, .data.rel.ro 0x2a101c50) [C]
| type | name | vcavsrv class (table @0x1d090) |
|---|---|---|
| 0‑5 | `/dev/dtv/audio0` … `audio5` | 0 = audio (vc_audio.c) |
| 6‑8 | `/dev/dtv/video0` … `video2` | 1 = video (vc_av.c), **no write** |
| 9‑11 | `/dev/dtv/stream0` … `stream2` | 2 = stream (vc_stream.c), **data path** |
| 12 | (not found) | 3 → VcAvOpen returns -1 |

## 2. VcAvCtrl(cmd): vcavsrv `doVcAvCtrl` / "ChangeMode" [C]

Dispatcher: vcavsrv main, msg kind 3 → @0x1276c, `tbb` on `cmd-1` @0x1279a (12 entries). `cmd` 0 or >12
gives "invalid mode:%d" and is still acked with 0. Every command acks 0. Globals: `susp`=0x2f5a0,
`spec`=0x2f5a4, `vmode`=0x302c0, `rep`=0x30380.

**While `susp`≠0, every OPEN/WRITE/IOCTL on a stream handle fails with ‑1 ("vcav now suspended")**
[C @0x12598 / @0x125ce→0x1270e]. Boot value of `susp` = 0 [C, .data].

| cmd | handler | effect | omplayer use |
|---|---|---|---|
| 1 | @0x1285a | `susp=0` (resume only) | – |
| 2 | @0x127fa | `susp=1`; stop+close all AVD (0x15648: AVD_Stop/StopExtPcmAudio/StopJukebox/AVD_Close) | – |
| 3 | @0x127fa | same as 2 | – |
| 4 | @0x12818 | `susp=0`; open jukebox/ExtPCM AVD channel (0x16724: `AVD_Open(&h, ch, 0x20)`, audio delay, `AVD_SetAudioSearchMode`) | – |
| **5** | @0x1280c→0x183f8 | **`susp=1`; close TDAV fds (0x1c6e0); stop+close all AVD (0x15648)**. Teardown. | close path @0x1197ae (if excl. flag) |
| **6** | @0x1284e→0x1840c | **`susp=0`; open jukebox AVD (as 4); open `/dev/dtv/tdavaudio0`+`tdavvideo0` and configure them (0x1c628 "vcav_tdav_open_av")**. Bring-up. | open path @0x11995e |
| 7 | @0x12846 | `spec=0` (only read by Finalize @0x1236a) | open path @0x119952 (if x84==1) |
| 8 | @0x1283e | `spec=1` | open path @0x119952 (else) |
| 9 | @0x12836→0x18424(0) | `rep=0`; MOC `_MOC_ReportStreamAttributes` (0x168e4(0)) | close path @0x1197bc |
| 10 | @0x1282e→0x18424(1) | `rep=1` (VIDEO_IOCSETPROPERTY then re-reports to MOC) | open path @0x11997c |
| 11 | @0x12826→0x16ab8(2) | `vmode=2` (MOC report path: vmode==2 → 1, else 0x19). Same variable as VIDEO_IOCOPENQRY arg[3] | open path @0x119958 |
| 12 | @0x12806→0x16c00 | if jukebox AVD is open: `AVD_InitFS(h,8)` + MOC re-report | – |

omplayer's sequences [C @0x119938‑0x11997c, @0x1197a4‑0x1197bc, also @0x11b916/0x11ba92]:
- **open** (only when its "exclusive" flag `[+0x8e]==1`): `Ctrl(7|8)`, `Ctrl(11)`, `Ctrl(6)`. If `[+0x8f]==1`: `Ctrl(10)`.
- **close**: `Ctrl(5)`, and if `[+0x8f]`: `Ctrl(9)`.

**TDAV note [C]:** the TDAV fds + jukebox AVD are otherwise opened only by an *audio*-class OPEN
(`vcav_av_open`@0x17e38, called from main@0x12988). **A video-only client should send `Ctrl(6)`**, or open
an audio handle too. [I] that the stream path needs TDAV (stream code has `set_tdav_path`,
`control_av_pid`, `disable_tdav_tsccheck`).

## 3. Device ioctls (magic 0xd6 = video/stream, 0xd7 = audio)

Names come from omplayer's own error strings. omplayer's labels are partly inconsistent (see 0x8004d606).

### VIDEO (class 1; server `video_control`/`video_setproperty` @0x181fc) [C]
| cmd | size | name | server behaviour |
|---|---|---|---|
| 0x8054d601 | 0x54 | VIDEO_IOCOPENQRY | `arg[0]=arg[2]` (returns the requested unit no.); `vmode=arg[3]` @0x1831c |
| 0x8008d602 | 8 | VIDEO_IOCCONTROL `{u32 cmd, u32 param}` | see sub-commands below |
| 0x8044d603 | 0x44 | VIDEO_IOCSETPROPERTY | memcpy to 0x30330, push aspect/size to MOC (0x16110); `rep==1` → re-report |
| 0x4044d604 | 0x44 | VIDEO_IOCGETPROPERTY | memcpy from 0x30330 |
| 0x4004d605 | 4 | wait-state (client-local read) | server pushes `2`=AVAILABLE on open of video1/2 (@0x128b2). `1`=LOST |
| 0x4004d608 | 4 | ? | returns 0x80 or 0x40 |

VIDEO_IOCCONTROL sub-commands (low byte of `cmd`) [C table @0x182fc + main @0x12b56/0x12b84, omplayer
@0x1183b4/0x118402/0x118432/0x119262/0x11a37a]:

| sub | meaning | evidence |
|---|---|---|
| **0** | **PLAY**. Server GETs the video property; if `type==5` (video-only) it calls **start** (0x12dbc→0x17fec→`do_avd_start`/`AVD_Start`, then `AVD_Play`). For A/V (type 2/3) the start is triggered by the AUDIO control sub 0 instead (@0x12b24). | "failed video play" |
| **1** | **STOP**. Bit 8 = keep last frame (omplayer uses 0x301 or 1). Without bit 8 the output is cleared (`vcav_avd_clear_video_output`→`AVD_VideoMute`) | mmf_video_stop |
| 2 | mute (`AVD_VideoMute(h,0,1)`) | 0x16ac4(0) |
| **3** | **unmute** | "failed video unmute" |
| 4 | pause on (`do_pause_on`, `AVD_ControlMode`) | 0x1706c |
| 5 | pause off (`do_pause_off`) | 0x17124 |
| 6 / 7 | sync off / on (server: no-op; 7 logs) | "failed video syncon/off" |
| 8 | no-op | |

### STREAM (class 2; `vcav_stream_ioctl` @0x1a4d4) [C unless noted]
| cmd | size | omplayer name | server behaviour |
|---|---|---|---|
| 0x4004d601 | 0x48 | STREAM_IOCOPENQRY | returns `arg[0]=1` (→ open `stream1`) @0x1aae4 |
| 0x8004d602 | 0x40 | STREAM_IOCSETPROPERTY | copy to 0x303a8. **byte0 = stream type** (selects the write path, §5). Only valid on stream1/2 (type 10/11 → set up queues) @0x1a80c |
| 0x4004d603 | 0x40 | STREAM_IOCGETPROPERTY | @0x1a724 |
| 0x8004d606 | 0x80 | **STREAM_IOCPLAY** (mmf path; byte0=2). The stream_driver path mislabels it SETPROPERTY. | main @0x12b98 pairs the client's video/audio fds, then posts msg 1 to the stream thread @0x1aa52 |
| 0xd608 | (funcR, 0) | (reset/flush, called right after open and at close) | main special-case @0x12a6c |
| 0x8004d60b | 0x28 | STREAM_IOCSETCHUNKLIST | §5 |
| 0x8004d60d | 8 | STREAM_IOCSTATCHUNK | returns `{0x46800, 1}` @0x1aaea. [I] 0x46800 = buffer size |
| 0x8004d61b | 8 | get write size (u64 bytes accepted) | @0x1aac6 |
| 0x4004d610 | 8 | elapsed time | stream_driver_get_elapsed_time |

## 4. Exact Open sequence (names / modes) for H.264 / HEVC [C, omplayer decoder_mmf]

The names are the same for every codec. **The codec is NOT in the open name.** It goes in the VIDEO property
byte +0x11.

```
VcAvConnect(10);                                   // @0x24852
// video (mmf_video_open @0x118d6c)
v0 = VcAvOpen("/dev/dtv/video0", 0x80002);         // @0x118da2
q[0x54]={0}; q.w[1]=prio; q.w[2]=1; q.w[3]= excl?2:1; q.b[0x10]=2;
VcAvIoctl_func(v0, 0x8054d601, q, 0x54);           // VIDEO_IOCOPENQRY → q.w[0]=1
v  = VcAvOpen("/dev/dtv/video1", 0x80002); VcAvClose(v0);
do { select(v readable); VcAvIoctl_func(v,0x4004d605,&st,4); } while(st!=2);   // 2=AVAILABLE
VcAvIoctl_func(v, 0x8008d602, {1,0}, 8);           // STOP/reset
// stream (mmf_stream_open @0x117cb8)
s0 = VcAvOpen("/dev/dtv/stream0", 0x80002);        // @0x117cfa
q[0x48]={0}; q.w[1]=ch; q.b[0xa]=2; q.b[0xb]=flag;
VcAvIoctl_func(s0, 0x4004d601, q, 0x48);           // STREAM_IOCOPENQRY → q.w[0]=1
s  = VcAvOpen("/dev/dtv/stream1", 0x80002); VcAvClose(s0);
VcAvIoctl_funcR(s, 0xd608, 0);
// audio, if any: "/dev/dtv/audio0" → AUDIO_IOCOPENQRY 0x4030d701 → "/dev/dtv/audioN"
```

## 5. What vcavsrv expects for VcAvWrite input

**STREAM property (0x40 bytes, SETPROPERTY 0x8004d602)**: omplayer GETs it, then sets byte0 = stream
type (`media==3`→4, `media==5`→5, else 3), byte1 = 0x10 or 0, +0x14 = ch [C @0x1187a0‑0x118862].

Write dispatch on stream type (`tbb` @0x1b6a2) [C]:
- **type 0/1**: raw byte stream, `memcpy` into the ring buffer (@0x1b750). No chunk list. [I] TS/PS passthrough.
- **type 2**: rejected.
- **type 3**: chunk mode (@0x1b6ac). Chunk type 4 → 0x19434. Chunk type 2 → mp4 path (`feed_chunk_mp4`: converts
  length-prefixed NALs, "nallen=…"). Otherwise the generic chunk path.
- **type 4/5**: chunk mode → `feed_chunk_raw` 0x19434.

**Chunk mode = every `VcAvWrite` must be preceded by `STREAM_IOCSETCHUNKLIST` (0x8004d60b, 0x28-byte
struct), and the chunk sizes must sum to exactly `len`** (`len == [0x30388+0x124]`, else error @0x1c060)
[C, omplayer `driver_write_es` @0x11c07c→0x11c08e does exactly SETCHUNKLIST then VcAvWrite].

ChunkList (client view; server copy/parse @0x1a596‑0x1a6a8, client serialiser @0x2a0f1080/`VcavCalcChunkListContentInfo`):
| off | field | evidence |
|---|---|---|
| +0x00 | u32 flags: low nibble = chunk type (1 simple, 3 simple‑alt, 4 with‑header, 2 mp4/two arrays); **bit 8** = header‑chunk count at +0x19 is valid (omplayer ORs 0x100) | [C] |
| +0x04 | `u32 *sizes` (one entry per chunk) | [C] |
| +0x08 | u32 count (≤1000) | [C] cmp 0x3e8 |
| +0x0c | u32 → [0x128] first PTS | [C] stored; [I] 90 kHz PTS |
| +0x10 | u32 → [0x134] PTS step per chunk | [C] used as `pts = first + (i)*step` (mla @0x19490); [I] units |
| +0x14 | u32 → [0x16c] (types ≠1) | [C] |
| +0x18 | u8 → [0x16a] (types ≠1); for types 2/4 the client sends a ptr here (extra data) | [C] |
| +0x19 | u8 header-chunk count (if bit 8) | [C] |
| +0x1c | u16 extra-data length (types 2/4; 0xffff→0) | [C] client Calc |
| +0x24 | 2nd array ptr (type 2) | [C] client |

Payload format: **ES bytes, one chunk per access unit**. The server packetises each chunk in ≤0xff00
pieces with its PTS (0x187fc/0x18eb4), which looks like PES building [I]. It then feeds the rtsc/TDAV path
(`/dev/dtvrec/rtsc/*`, `/dev/dtv/tdav*`) with the PIDs from the VIDEO property [I]. Because there is a
dedicated *mp4→start-code* converter for chunk type 2, the other chunk types expect **Annex‑B (start
codes)** [I]. **So the input is raw Annex‑B ES split into AUs, not PES or TS** (types 3/4/5). TS passthrough
is likely only for stream types 0/1 [I].

Pacing [C]: the server returns ‑28 when `len*factor > bufsize` (@0x1b668→0x1b868) or when
`buffered+len > limit` (@0x1b694→0x1bfea). The client sleeps 10 ms and retries. **A single write bigger than
the whole buffer retries forever.** STATCHUNK reports 0x46800 (288 KiB) [I as buffer size]. Keep each write
well below that (for example ≤128 KiB). A huge 4K I-frame is a risk to watch.

## 6. Video property (0x44 bytes) and codec enums

VIDEO property, as omplayer sets it (GET 0x4044d604 → modify → SET 0x8044d603, @0x1188c6‑0x118a14), and
as vcavsrv reads it (start @0x12dbc, do_avd_start @0x17208, setprop @0x1822a):

| off | field | value omplayer uses |
|---|---|---|
| +0x00 u8 | **type**: 5 = video-only (start on VIDEO PLAY); 2 (or 3) = A/V (start on AUDIO play) | [C] 5 if no audio fd |
| +0x01 u8 | media (printed "media=") | [C] passthrough |
| +0x02 u16 | **video PID** | [C] TS: the real PID; otherwise **0x1011** |
| +0x06 u16 | PCR PID | [C] otherwise **0x1001** |
| +0x0c u32 | (same as media) | [C] |
| +0x10 u8 | 5 or 6 (flag bit 3) | [C] meaning [I] unknown |
| +0x11 u8 | **vcav video codec** (table below) | [C] read @0x12e1a |
| +0x18 u32 | height (720) | [C] compared ≤576/≤720 in do_avd_start |
| +0x1c u32 | width (1280) | [C] compared ≤720/≤1280 |
| +0x24 u32 | pixel_aspect (1000; ≤1 forced to 1000) | [C] "pixel_aspect = %d" |
| +0x28 u32 | rate (default 0x6d38). PAL test 25000/50000 | [C] [I] fps×1000 |
| +0x30.. | 0 / misc | |

**vcav video codec → AVD codec enum** (`get_avd_video_codec` @0x16d94, tbb @0x16da0) [C]:
| vcav (+0x11) | log string | AVD enum |
|---|---|---|
| 0 | VIDEO_CODEC_MPEG2 | 1 |
| 1 | (unknown → H264 + error) | 2 |
| **2** | **VIDEO_CODEC_H264** | **2** |
| 3 | VIDEO_CODEC_VC1 | 4 |
| **4** | **VIDEO_CODEC_HEVD (HEVC)** | **9** |
| 5 | VIDEO_CODEC_VP9 | 10 (0xa) |
| 6 | VIDEO_CODEC_HEVD | 9 |

vcav audio codec → AVD (0x16590): 0→4, 1→0, 2→1, 7→9, 8→0x11, 12→8, others → 4 + "unknown_codec" [C]. The
audio codec names are unknown. The audio fs (Hz → enum, 0x164f8) is [C]: 8000→0x10, 11025→0xf, 12000→0xe,
16000→0xd, 22050→0xc, 24000→0xb, 32000→0xa, 44100→9, 48000→8, 64000→7, 88200→6, 96000→5.

### Route B: AVD_Start config (do_avd_start @0x17208) [C layout, I semantics]
`AVD_Start(h, A, AUD, VID, X0, X1, 0, mask?, 0, X2)`: 10 args @0x173b2‑0x173d0 (r0‑r3 + 6 on the stack:
`[sp]=sp+0xe0`(0x40 zero), `[sp+4]=sp+0x38`, `[sp+8]=0`, `[sp+0xc]=1/6`, `[sp+0x10]=0`, `[sp+0x14]=sp+0x24`).
- VID (0x20): +0 u16 video PID; +4 AVD codec (table above); +8 size class (0/1 NTSC‑SD, 2/3 PAL‑SD,
  4 ≤1280x720, 5/6 larger with rate ≤/>50000); +0xc = [0x30360]; +0x10 = ch (0x15780).
- AUD (0x40): +0 u16 audio PID; +4 AVD audio codec; +8 fs enum; +0xc ch. Zeroed when video-only.
- A (0x2c): +8=2, +0xc u16 mask (1=A/V, 4=video-only, 8=audio-only via 0x171e8), +0x10/+0x1c=[0x30360], +0x14/+0x18=-1.
- `[sp+0x38]`: +0 = 0xf if no video else 0, +4 u16 = 100.
- Preceded by `AVD_SetSectionPtsValid(h, {1,1})` (@0x17358; success = 0xa0010000, compared @0x17364).
  AVD OK = **0xa0010000**, 0xa0010004 = not open, 0xa001000b = bad codec, 0xa001000c = bad handle [C libavd].

`AVD_SetVideoCodecInf(h, codec, void **pinf)` [C libavd @0x2917b0d8]: accepts **only codec 2 (H264,
copies 8 bytes) or 9 (HEVC, copies 16 bytes)** from `*pinf` into per-channel shm (+0x408 / +0x410).
Otherwise it returns 0xa001000b. Its only stock caller is apl_bdv @0x10c99a: `(h, 9, &p)`, with p → {u32 0;
u8 flag=1; u8 0; u32 3; u8 0,0}, and only when a stream flag ([+0x24]==1) is set. **Optional; not needed for
basic decode** [I].
`AVD_SetVideoOutMode(h, a, b, c, d)`: 5 args (r1‑r3 stored together + 1 stack word) @0x29176cec. **No
stock caller** (apl_bdv uses `AVD_SetVideoOutModeSelect`). Layout unknown and not needed.

## 7. Recommended Route‑A start sequence (video-only raw ES)

```
VcAvConnect(10)
VcAvCtrl(11); VcAvCtrl(6)                       // full-screen mode + resume + open TDAV/jukebox
open video (§4, OPENQRY mode=2) → wait AVAILABLE → VIDEO_IOCCONTROL{1}
open stream (§4) → funcR(0xd608)
STREAM GETPROP → byte0=5 → SETPROP
VIDEO GETPROP → type=5, pid=0x1011, pcr=0x1001, codec=2|4, w/h, aspect=1000 → SETPROP
prebuffer: SETCHUNKLIST+VcAvWrite a few AUs
VIDEO_IOCCONTROL{3} (unmute), {0} (PLAY → AVD_Start/Play), {7} (sync on)
STREAM_IOCPLAY (0x8004d606, 0x80 bytes, byte0=2); STREAM_IOCSTATCHUNK (8)
loop SETCHUNKLIST + VcAvWrite
teardown: VIDEO_IOCCONTROL{1}; funcR(s,0xd608); VcAvClose(s); VcAvClose(v); VcAvCtrl(5); VcAvCtrl(1); VcAvDelete()
```
Ordering source: omplayer `mmf_play` @0x11838c does video play → audio play → stream play in one function [C].
The prebuffer-before-play order, and the final `Ctrl(1)` to restore boot state (`susp=0`), are my choices [I].

## 8. Still unknown
- Chunk PTS units and timebase (90 kHz assumed), and the exact meaning of chunk types 1 vs 3, and of stream types 3 vs 4 vs 5.
- Whether vcavsrv's rtsc path needs SPS/PPS(/VPS) as separate header chunks (bit 8 / +0x19), or accepts them inline in the first AU.
- What `0x8004d606`'s 0x80-byte arg holds beyond byte0=2.
- Video property +0x10 (5/6), +0x01 media codes, and the meaning of omplayer's `media` values (0/1 = TS?, 3, 4, 5).
- The real server buffer size (`[0x30388+0xc8]`) and the float factor at @0x1b63c used for types 3‑5.
- Whether AVD_Start succeeds before any data has been written (omplayer may prebuffer first).
- Audio path enums (names of the vcav audio codec values).
