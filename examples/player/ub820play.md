# ub820play: native MPEG-TS player for the DP-UB820

`ub820play` plays a 188-byte MPEG transport stream from a file or a pipe on the UB820's own decoder. It talks to the stock `vcavsrv` decode server through `libvcavif` and routes the decoder to the screen through `libmoc`. It implements the recipe proven live on 2026-11-27 (Apollo 13 4K HDR played through):

* raw TS (stream type 0);
* 4 MiB writes;
* MOC routing;
* clean teardown with a home restore.

Window and OSD handling (flipwin, the alpha hole) is **not** done by the player. That is the launcher's job.

Source: `ub820play.c`. Background: `~/UB820-cfw/native-access/LOG.md` and `findings/vcav-*.md`.

## Build

```
zig cc -target arm-linux-gnueabihf.2.20 -O2 -s ub820play.c -o ub820play -ldl
```

* It builds without warnings, including with `-Wall -Wextra`.
* The binary imports only GLIBC ≤ 2.17 symbols and uses no stdio: all logging is `write(2)`.
* Run it with the usual library path:
  `LD_LIBRARY_PATH=/usr_root/lib:/lib:/usr/lib:/usr/dtvrec/lib:/usr/local/lib`.

## Usage

```
ub820play [options] <file.ts | ->          (- = stdin)
```

| option | default | meaning |
|---|---|---|
| `--codec mpeg2\|h264\|vc1\|hevc\|vp9` | from the PMT | VIDEO property +0x11 (0 / 2 / 3 / 4 / 5). VP9 has no TS stream_type, so it must be given explicitly. |
| `--4k` | | 4KOTT routing: SW_VIDEODL (0x13) + play mode 8. Required for UHD. **The home UI is torn down** (see Known limits). |
| `--hd` | yes | DIVX routing: SW_DIVX (0x12) + play mode 7. For ≤ 1080p. The home survives, and the video shows only through an alpha hole in the OSD. |
| `--vpid PID` | PMT, else 0x1011 | video PID that TDAV filters (VIDEO property +0x02) |
| `--pcrpid PID` | PMT, else 0x1011 | PCR PID. Used for pacing, and written to VIDEO +0x06 / AUDIO +0x0e (the server ignores both). |
| `--apid PID\|none` | first playable PMT audio track | Audio PID. `none` = video-only: the proven start (VIDEO property 5). |
| `--acodec aac\|ac3\|eac3\|mpeg\|heaac\|N` | from the PMT | vcav audio codec (AUDIO property +0x02): aac 0, ac3 1, mpeg 2, heaac 7, eac3 8 |
| `--afs HZ` | 48000 | AUDIO property +0x1c |
| `--lead SECONDS` | 3 | PCR lead kept ahead of the display clock |
| `--ctl PATH` | `/tmp/ub820play.ctl` | control FIFO (`""` = off) |
| `--status PATH` | `/tmp/ub820play.status` | status file (`""` = off) |
| `--block BYTES` | 4194280 | bytes per write, 96256 … 4194280 (rounded to whole packets) |
| `--max-writes N` | 300 | writes per vcavsrv session (the FeedDec transfer limit) |
| `--max-inflight N` | 1 | write only while the FeedDecs in flight are ≤ N (0 … 2). The default is clamped so that the in-flight blocks fit the ~9 MiB ring. An explicit value is used as given (2 = the server's own gate). The value in effect is logged. |
| `--watchdog SECONDS` | 20 | kill the player and restore the home routing if the main loop is wedged (0 = off) |
| `--size WxH`, `--fps F` | 1920x1080, 24 | VIDEO property +0x18/+0x1c/+0x28. Only the size class (SD / ≤720 / above) and rate > 50 fps matter to `do_avd_start`. These are the skeleton's proven values; their effect in TS mode is unverified. |
| `--quiet`, `-v` | | warnings and errors only; or debug output (every ioctl, every write) |

### Environment (debug overrides, the same names as player_skeleton)

| env | meaning |
|---|---|
| `VMOCAVDEV=0x12..0x17` | override the widget device |
| `VMOCPLAYMODE=N` | override the play mode set before routing |
| `VMOC=0` | skip MOC routing: the decoder runs, but no picture is visible |
| `VMOCHOMEOSD=0` | skip the home OSD preset in the restore |
| `VPOOLCHK=0` | do not query the ACE memory pool (`/dev/fb6`) |

## Launcher example

```
ffmpeg -i file.mkv -map 0:v:0 -map 0:a:0 -c copy -f mpegts -streamid 0:4113 -streamid 1:4352 -pcr_period 20 - \
  | ub820play --codec hevc --4k -
```

* This gives video on 0x1011 (with PCR) and audio on 0x1100.
* **Audio is only played for AC-3, E-AC-3, AAC and MPEG audio.** DTS, DTS-HD, TrueHD and LPCM cannot be reached through vcavsrv. The player then warns and plays video only. To keep the sound, transcode it: `-c:a eac3 -b:a 640k` (or `-c:a ac3 -b:a 640k`).
* **The A/V mode is not verified live yet.** If a run with audio shows no picture or stalls, add `--apid none` to fall back to the proven video-only path. Better still, drop `-map 0:a:0`.
* Extra tracks for switching: `-map 0:a:1 -streamid 2:4353` (0x1101), and so on.
* Add `-mpegts_flags +resend_headers` if the stream may be joined mid-way.
* A ≤ 1080p file uses `--hd` (the default) together with the launcher's alpha-hole window.
* Wrap the launcher around this: flipwin before and after (gwin re-points plane 7 about 3 s after the use-case change), then wait for the player to exit.
* Before starting, if no vcavsrv is running, remove a stale `/tmp/vcavsrv.alive` (as `vtest.sh` does). A crashed vcavsrv leaves it behind, and the next one then waits for ever.

## Behaviour

1. **Input.** The player reads the file or stdin with `pselect()` + `read()`, so short reads, slow pipes and EOF are handled. It packs whole 188-byte packets into the write block.
   * A lost `0x47` sync byte triggers a resync: the player skips bytes until two sync bytes 188 apart.
   * The skipped byte count is logged.
   * 192-byte M2TS input is rejected with a hint. A trailing partial packet is dropped.
2. **Prebuffer.** Up to 2 MiB (or the whole input, if it is smaller) is read **before the device is touched**.
   * PAT/PMT in that window supply the codec, the PIDs and the audio track list.
   * Bad input therefore never routes the monitor.
   * The prebuffer goes out in one write (≤ 2 MB, because the type-0 ring takes only a few MB before PLAY). Then PLAY is sent. Tiny inputs are played after EOF.
3. **Session open.** The steps are:
   1. watchdog fork;
   2. `dlopen` libpthread (preload), libvcavif, libmoc/librpc;
   3. MOC route;
   4. `VcAvConnect` (or `VcAvCreate` + connect);
   5. open video0→video1 (wait AVAILABLE, STOP), stream0→stream1 (+ reset), audio0→audio1;
   6. STREAM property type 0;
   7. VIDEO property (type 5 for video-only, 2 for A/V; the PIDs; the codec);
   8. AUDIO property (A/V only);
   9. the **unmute health probe**. If it fails, vcavsrv has no AVD channel (stale state after an earlier vcavsrv crash) and the player exits with rc 8. **Reboot** before the next run.
4. **Start.** Video unmute → VIDEO PLAY → [AUDIO unmute → AUDIO PLAY] → STREAM_IOCPLAY → video unmute.
5. **PCR pacing.**
   * The player tracks the last PCR written, in 45 kHz units (base/2 = 27 MHz/600).
   * It polls `elapsed` (`0x4004d610`, the display PTS) every 50 ms.
   * It writes the next block only while `pcr_written − elapsed < lead`. Before the first picture the reference is the first PCR.
   * Blocks are 4 MiB. When pacing says wait, the player waits; it does not write small pieces. The only exception is an input so slow that the lead drops below about 1 s: then a partial block of ≥ 1 MiB (or anything, below 0.25 s) goes out early.
   * Every write also needs `FeedDecs in flight` (`0x8004d617`) ≤ `--max-inflight`. Together with the lead this keeps vcavsrv's ring gate from ever refusing a write, so VcAvWrite (which retries −ENOSPC for ever) never blocks.
   * Because PTS ≈ PCR + mux delay (0.7 s with ffmpeg), the real buffer is about lead + 0.7 s. Expect the lead to overshoot by up to one block (about 1.3 s at 25 Mbps).
6. **End of stream.** After EOF and the last write, the player drains until elapsed reaches the highest video PTS written (or the last PCR if there was none), or until elapsed has not moved for 1.5 s. Then it tears down. rc 0.
7. **Halt detection.**
   * Elapsed frozen for 4 s while data is pending → `DECODER HALT`, clean teardown, rc 11. Pending means either ≥ 0.5 s of written data is ahead of the clock, or a full/final block is ready but the in-flight gate keeps refusing it.
   * A frozen clock with nothing pending (the block is still filling from a slow input) is logged as an UNDERRUN and the player keeps waiting.
   * No picture within 10 s + lead after PLAY → rc 11, with a hint (wrong codec or PID; in A/V mode, a missing audio PID).
   * None of this runs while paused.
8. **Session limit.** vcavsrv delivers only 300 FeedDec transfers per session, and one write is one transfer. So:
   * 290 writes logs a WARNING.
   * At `--max-writes` (300) the player stops feeding, plays out what was delivered and exits with **rc 12**. The log gives `resume at input byte N (PCR t s)`.
   * At 4 MiB per write that is ~1.26 GB: about 6.7 min at 25 Mbps, about 2.8 min at 60 Mbps.
   * **TODO: seamless renewal.** Untested candidates (findings/vcav-ts-40mb.md §5d, vcav-audio.md §6):
     * A/V mode may itself retire the transfer entries. The first A/V run should check this with `--block 96256`: if it plays past ~31 MB, the limit is gone. Then raise `--max-writes`.
     * Otherwise, a STREAM `0x8004d61a` TermFeed re-arm + STREAM_IOCPLAY at in-flight 0 (with a visible glitch).
9. **Signals.** SIGTERM/SIGINT/SIGHUP are only delivered inside `pselect()`, never in the middle of a VcAv call. The original mask is restored around `VcAvCreate()`, so a vcavsrv the player spawns does not inherit the blocked set. They give a clean teardown and **rc 128+signal**. SIGPIPE is ignored.
10. **Teardown.** The order is:
    1. [AUDIO STOP];
    2. VIDEO STOP;
    3. STREAM reset;
    4. close the handles;
    5. `VcAvCtrl(5)`, `VcAvCtrl(1)`, `VcAvDelete`;
    6. MOC restore: Unmap, SetMonitorSelect(NONE), play mode 9 (a real change), home OSD preset `{0,0x222},{7,0},{0x1f,0}` + `{0x27,1}`.

    vcavsrv is never killed; it exits by itself.
11. **Watchdog.** This is a forked child that never touches vcavsrv. It covers two cases:
    * The main loop makes no progress for `--watchdog` s, for example while wedged inside VcAvWrite or an ioctl. The child SIGKILLs the player. vcavsrv then cleans up on the closed sockets, as after the TIMEOUT kills seen live. The child waits 2 s, restores the home routing, removes the FIFO and writes `state=done rc=137 reason=watchdog`.
    * The player dies while still routed (kill -9, crash). The child does the same restore and writes `rc=-1 reason=crashed`.

## Control FIFO

The player creates the FIFO with `mkfifo` (default `/tmp/ub820play.ctl`) and removes it at exit if it created it. It is read non-blocking on every loop iteration. Commands are one per line:

| command | effect |
|---|---|
| `stop` (or `quit`) | clean teardown, rc 0 |
| `pause` | VIDEO_IOCCONTROL sub 4 (`do_pause_on` → `AVD_ControlMode`). Feeding and halt detection pause too. **Untested live on this path.** |
| `resume` (or `play`) | VIDEO_IOCCONTROL sub 5 (`do_pause_off`) |
| `status` | writes the status file immediately |
| `audio <pid> [codec]` | Switches the audio track (A/V mode only; the codec comes from the PMT if omitted). The sequence is AUDIO STOP (TDAV filters off + AVD_Stop of **audio and video**), new AUDIO property, video unmute, AUDIO PLAY, video unmute. Feeding continues. Expect a picture freeze of up to one GOP plus an audio gap. Pacing then follows the wall clock until a new picture shows. Ignored in video-only mode, because A/V needs a restart. **Untested live.** |

Example: `echo pause > /tmp/ub820play.ctl`, `echo "audio 0x1101 eac3" > /tmp/ub820play.ctl`.

## Status file

`/tmp/ub820play.status` is rewritten about once a second, and on `status`, via a temp file + rename. It is one line:

```
state=playing pos=12.345 elapsed=13.745 bytes=46137080 in=48234220 writes=11/300 lead=3.120 inflight=0 codec=hevc route=4k audio=0x1100/eac3 rc=-
```

| field | meaning |
|---|---|
| `state` | `init` `prebuffer` `playing` `playing-paused` `draining` `stopping` `done` |
| `pos` | seconds since the first video PTS |
| `elapsed` | raw display PTS in seconds (includes the stream's start offset, about 1.4 s with ffmpeg) |
| `bytes` | bytes written to vcavsrv |
| `in` | input bytes consumed |
| `writes` | used / budget |
| `lead` | PCR lead in seconds |
| `audio` | `pid/codec` or `none` |
| `rc` | exit code once `done`, else `-` |
| `reason` | optional: `stop-command`, `signal`, `halt`, `no-picture`, `write-failed`, `audio-switch-failed`, `watchdog`, `crashed` |

## Exit codes

| rc | meaning |
|---|---|
| 0 | end of stream, or `stop` |
| 1 | usage / unknown codec |
| 2 | input error (no TS packets, M2TS) |
| 3 | library load failed |
| 4 | MOC routing failed |
| 5 | vcavsrv not reachable |
| 6 | handle open failed |
| 7 | property set failed |
| 8 | unmute health probe failed: **reboot the player** |
| 9 | PLAY (or the A/V start / audio switch) failed |
| 10 | VcAvWrite failed |
| 11 | decoder halt / no picture |
| 12 | session transfer budget used: input truncated (resume offset in the log) |
| 128+N | stopped by signal N |
| 137 | killed by the watchdog |

## Known limits

* **4KOTT kills the home UI.** The restore brings back the pool and `ACEMGR Run`, but not the home's ADS resources; this is an open issue. A ≤ 1080p `--hd` run leaves the home intact. **Do not use the YouTube DIAL start/stop for recovery:** it leaves a SysV semaphore held and AVD in a bad state (LOG 2026-11-27 ~18:30).
* **A vcavsrv crash while AVD is open needs a reboot.** It leaves stale SHMAVD/avccdd state behind. rc 8 at the health probe is the symptom.
* **Seeking is not supported.** Restart the pipeline from an offset (`ffmpeg -ss`).
* **Validation so far:** the player was only exercised on the host, against stub vcavsrv/MOC libraries. Tested: file and stdin input, a tiny file, garbage and mid-stream resync, SIGTERM, every FIFO command, the 290/300 limit, halt, the watchdog, kill -9 recovery and A/V track switching. The device run is pending.
