# Native media player for the DP-UB820 (hardware decode, our own code)

Status as of 2026-11-28. Method and tooling only: no Panasonic binaries, keys or firmware in this repo.

## Architecture
```
USB file → ffmpeg (static, armv7, on the player) → MPEG-TS (PCR, video PID 0x1011, audio 0x1100+i)
        → ub820play (stdin) → libvcavif → vcavsrv (Panasonic VieraCast A/V server) → libavd → HW decoder
```
- **vcavsrv stream type 0 (raw 188-byte TS).** Type 0 is the only path that runs long. vcavsrv's own chunk/PES path (type 2) halts after 300 pictures.
- **MOC routing** (libmoc over RPC to mocserver) makes the decoder visible:
  - ≤1080p: play mode 7 + stream widget SW_DIVX (0x12). The home UI survives; the OSD is cleared to alpha-0.
  - 4K: play mode 8 + SW_VIDEODL (0x13) = the "4KOTT" use case. apl_mgr tears down the home UI.
- **Home restore after 4K:** an AM "go home" event (c:59 f:42004, payload 11) over libev to apl_acemgr's queue, then `ACEMGR_PlayerExecFunc(5 /*ONPLAY*/)` from libacemgr.so (preload libapl_envlib.so for `apl_dbg_output_check`). See `examples/player/homerestore.c`.
- **Audio:** VIDEO property +0 = 2 (A/V). AUDIO property 0x20 bytes (GET 0x4020d704 / SET 0x8020d703): +0 = 2, +2 = vcav codec, +4 = PID, +0x1c = fs.
  - vcav codecs: 0 AAC, 1 AC-3, 2 MPEG audio, 8 E-AC-3.
  - Start order: video unmute → video play → audio unmute → audio PLAY → stream PLAY.
  - Switching tracks: audio stop → new property → audio play.
- **The "300 transfers" wall (solved).** vcavsrv pushes an unsolicited 8-byte event (`06 00 00 00 00 00 00 00`) onto the client's STREAM socket for every transfer. If the client never reads them, the socket buffer fills after ~300, vcavsrv's send blocks and feeding stops; this looks like a 300-picture / ~40 MB limit. The fix: before each write, read whatever is pending (FIONREAD) on the stream, video and audio fds. With that, a 94 Mbps 4K film played 3:37 in one session.
- (Historical) Before the fix, the workaround was 300 transfers per vcavsrv session
  - Use 4 MiB writes (one write must be ≤ ~4.37 MB): ~1.26 GB per session.
  - Past that, ub820play stops cleanly (rc 12) and the launcher resumes with `ffmpeg -ss`.
- **Pacing:** keep the PCR written no more than a few seconds ahead of the server's elapsed clock (ioctl 0x4004d610). VcAvWrite retries -ENOSPC forever, so never let it block.

## Files (`examples/player/`)

| file | what |
|---|---|
| `ub820play.c` | the player: TS from file or stdin, PMT autodetect, PCR pacing, A/V start, ctl FIFO (stop/pause/resume/audio), status file, watchdog, clean teardown |
| `ub820run.sh` | launcher: probe with ffmpeg; copy or transcode video (MPEG-2 for codecs the HW can't do) and audio (AC-3 for DTS/TrueHD/...); all audio tracks on separate PIDs; subtitles; session renewal; home restore |
| `media.cgi` | LAN web remote (busybox httpd): list/play/stop/pause/seek/audio switch |
| `subovl.c` | text-subtitle overlay: SRT/WebVTT drawn with stb_truetype into a free OSD buffer (LOSD0), timed from the player status |
| `homerestore.c` | brings the home screen back after a 4K (4KOTT) session |
| `conscap.c` | captures /dev/console (TIOCCONS into a pty): mocserver/apl_* logs, live |
| `semfix.c` | releases a SysV semaphore whose holder died (recovery) |

Build: `zig cc -target arm-linux-gnueabihf.2.20 -O2 -s X.c -o X -ldl` (subovl also needs `-lm`).
- Log with write(2): glibc stdout/stderr data symbols crashed at load in zig builds.
- ffmpeg: static musl build (`zig cc -target arm-linux-musleabihf -mcpu=cortex_a7`), ~4.9 MB, with demuxers + parsers + bsfs, software decoders for legacy codecs, the mpeg2video/ac3 encoders and subtitle codecs.

## Format compatibility (tested)
- **Video, hardware decode:** MPEG-1/2, H.264 (to 4K), HEVC Main10 4K HDR10/HDR10+ (tiles, B-pyramid).
- **Video, software → MPEG-2 on the fly:** MPEG-4 ASP/DivX/Xvid, MS-MPEG4v3, FLV1, MJPEG, Theora, VP9 (SD fine, 720p ≈ real time).
  - VP9 via the hardware path is rejected in TS mode.
- **Containers:** MKV/WebM, MP4/MOV, AVI, ASF, FLV, Ogg, MPEG-PS/TS/M2TS, WTV, raw ES.
- **Audio copied:** AAC, AC-3, E-AC-3, MP2/MP3.
- **Audio transcoded live to AC-3 640k:** DTS, DTS-HD MA, DTS:X, TrueHD, Atmos, FLAC, Opus, Vorbis.
- **Lossless passthrough (experimental):** a 4-byte vcavsrv patch makes its "unknown codec" path return `vcav − 0x20` as the AVD codec (0x25 DTS, 0x26 TrueHD, 0x30 DTS-HD MA). AVD accepts and decodes these; whether HDMI bitstreams them is unverified.
- **Subtitles:** SRT/ASS/WebVTT/mov_text (embedded or sidecar). Bitmap (PGS/DVD) not yet.
- **Full-length films played end to end:** 1080p H.264 + DTS-HD MA; several 4K HDR HEVC + DTS:X/TrueHD films up to 94 Mbps; 720p MPEG-4 AVI.
