# subovl: subtitle overlay for the native player

`subovl` draws text subtitles on top of `ub820play`'s video in **HD mode**.

* It renders SRT/WebVTT cues with stb_truetype into **LOSD0**. That is our own free 1920x1080 ARGB8888 OSD buffer: phys `0xE1FF0000`, bus `0x21FF0000`.
* It keeps UI plane 7 pointed at LOSD0, using the flipclear method.
* It times the cues from `ub820play`'s status file.

Status: built and host-tested only (2026-11-28). **Not yet run on the device.**

**Files** (all in `poc/`):

| file | what |
|---|---|
| `subovl.c` | the daemon |
| `stb_truetype.h` | vendored v1.28 from nothings/stb, public domain / MIT |
| `subovl_font.h` | embedded fallback font, generated |
| `gen_subovl_font.c` | the host tool that generates it |
| `ub820run.subs.sh` | launcher with subtitles. A copy of `ub820run.sh`; the original is untouched. |
| `subovl-test/` | host harness, test SRT/VTT |

## Build

```
zig cc -target arm-linux-gnueabihf.2.20 -O2 -s subovl.c -o subovl -ldl -lm     # device, ~115 KB
cc -DHOST -O2 subovl.c -o subovl.host -lm                                          # host test build
# only if the fallback font must change:
cc -O2 gen_subovl_font.c -o gen_subovl_font -lm && ./gen_subovl_font /usr/share/fonts/google-noto/NotoSans-Bold.ttf > subovl_font.h
```

The device binary imports only `GLIBC_2.4` and `GLIBC_2.17` functions (libc, libm, libdl). It has no `stdout`/`stderr` data symbols (checked with `readelf --dyn-syms`). All logging goes through `write(2)`.

## Usage

```
subovl <file.srt|file.vtt> [--size PX] [--margin PX] [--status /tmp/ub820play.status]
       [--base-file /tmp/ub820run.base] [--offset SEC] [--font PATH|builtin] [--font-index N]
       [--width PCT] [--outline PX] [--shadow PX] [--color 0xRRGGBB] [--no-kern] [--verbose|-v]
```

| option | default | meaning |
|---|---|---|
| `--size` | 54 | font size in px per em. The line pitch is 1.22 × size. |
| `--margin` | 64 | Distance from the bottom edge. For `{\an7/8/9}` cues it is the distance from the top edge. |
| `--offset` | 0 | Seconds added to the media clock. A positive value shows subtitles **earlier**. |
| `--width` | 80 | Wrap width in % of 1920. |
| `--outline` / `--shadow` | 6 % / 4 % of size | Black outline radius / drop-shadow offset. |
| `--font` | see below | `builtin` forces the embedded font. |

Run it with the usual `LD_LIBRARY_PATH` (`…:/usr/dtvrec/lib:…`), because it dlopens `libavio.so`.

## How it works

### Plane 7 / AVIO

These rules follow the gotchas in LOG.md (2026-11-26).

* AVIO is **never held open.** At start, and then once a second, the daemon opens one short balanced window:
  1. `AVIO_UP_Open(0)`
  2. `_AVIO_UP_GFX_Get_Addr(7)`
  3. `AVIO_UP_GFX_Set_Addr(7, {1, 0x21FF0000, 0, 0}, 1)`, only if needed
  4. `AVIO_UP_Close()`

  A crash or `kill -9` can therefore leak an open only if it lands inside those few microseconds. If `Open` returns non-zero, the daemon makes no Get or Set call and skips that check.
* **flipclear's guard** decides whether to re-point:
  * plane 7 = `sel=1` and `0x21010000` or `0x217fc000` (gserver's LOSD1 buffers): re-point to LOSD0;
  * already `0x21FF0000`: nothing;
  * anything else: logged once and left alone.
* If gserver is not running (standby, UI stack torn down), subovl makes **no AVIO call at all** and exits.
* On exit (SIGTERM/INT/HUP, or the player is `state=done` for more than 2 s), it clears LOSD0 and **leaves plane 7 alone**. gserver re-flips plane 7 on its next draw.
* The re-point also undoes a gserver flip caused by a remote keypress during playback, within about 1 s.

### Drawing

* LOSD0 is mapped once from `/dev/exmem` (`O_SYNC`, `mmap64` at `0xE1FF0000`). It is fully cleared at start and at exit only.
* Each cue is composed off-screen into a tile:
  * fill coverage from stb_truetype, with subpixel x and kerning;
  * a soft disc dilation for the outline;
  * a shifted outline at 55 % for the shadow.
* The tile is then copied row by row (`memcpy`, as clockovl does) into its bounding box. A new cue rewrites the **union of the old and new boxes in one pass**: pixels outside the new box become 0. There is no blank frame and no full-buffer repaint.
* There are two slots: bottom, and top (`{\an7}`, `{\an8}`, `{\an9}`). Cues active at the same time are joined in start order.
* Text is centred, greedy word-wrapped to 80 %, and limited to 3 lines; extra lines are dropped and logged with `-v`. CJK text may break between any two ideographs.
* Pixel format is `0xAARRGGBB`, which is little-endian `bgra` in ffmpeg. clockovl's `EDGE 0xFF3A86FF` (blue) confirms the order.
* **Premultiplied alpha doesn't matter here.** The outline (radius ≥ 2) encloses the white fill's anti-aliased fringe, so every partial-alpha pixel is pure black: the host check counts 0 non-black partial pixels. Inside the outline alpha is 255, and the white-to-black blend is plain RGB.
* **Self-heal:** each 100 ms poll reads up to 6 opaque "sentinel" pixels of the current cue. If someone cleared LOSD0 (flipclear, for example), the cue is redrawn.
* Compose time on the host is 1–13 ms per cue, most of it the outline dilation. Expect roughly 10× that on the ARM (unmeasured). It is paid only when the cue changes.

### Fonts (first that works)

1. `--font PATH` (face `--font-index`, default 0).
2. `/tmp/NotoSansCJK-Regular.ttc` (face 0 = JP; CFF outlines; host-tested with the official static OTC).
3. `/tmp/subovl.ttf`, `/tmp/subovl.otf`.
4. A shallow scan (depth 2) of these directories for `.ttf`, `.otf` and `.ttc`, accepting only files with a valid magic:
   * `/tmp/fonts`
   * `/usr/local/share/fonts`
   * `/usr/share/fonts`
   * `/usr/local/font(s)`
   * `/usr/X11R6/lib/X11/fonts`
   * `/usr/lib/fonts`
   * `/usr/dtvrec/font(s)`

   None of these is known to hold a usable font on the UB820; nothing in the research notes names a device font path.
5. The embedded fallback: Noto Sans Bold (OFL), 40 px, 4 bpp, ASCII + Latin-1 + quotes, dashes, ellipsis, € and ♪. It is bilinear-scaled to `--size`. Other characters show as `?`.

stb_truetype does **not** read CFF2 or variable fonts. Fedora's `NotoSansCJK-VF.ttc` will not work, so use the static OTC. The font is `mmap`ed, so the 19 MB `.ttc` in tmpfs costs no extra RAM.

### Timing

* The status file is polled every 100 ms, re-opened each time because the player `rename()`s it. The base file is re-read on every poll.
* Media time = `base + pos + extrapolation + --offset`:
  * The anchor is reset whenever a *new* status write changes `pos` or the state. The anchor time is backdated by the file's age (mtime vs `CLOCK_REALTIME`, clamped to 0.5 s).
  * The extrapolation uses `CLOCK_MONOTONIC`, is capped at 1.5 s, and only applies in `playing` and `draining`. It is frozen in `*-paused`.
* Cues are shown only in `playing`, `draining` and their `-paused` forms, and only once `pos` has been seen. In `init`, `prebuffer`, `stopping` and `done` nothing is shown.
* There is no extrapolation while `pos` is still 0. The player reports 0 until the first picture, so this stops cues flashing early after a start or seek.
* **Stale files:** status writes older than subovl's own start are ignored. That covers the previous run's `state=done` and an rc=12 renewal's `done`. The done timer is armed only after a live state has been seen in this run. The watchdog's minimal `state=done rc=.. reason=..` line, which has no `pos`, is handled.
* Host timing test: cue on/off edges land 50–90 ms after the ideal time, which is the poll granularity.

### SRT / WebVTT parsing

* Any line containing `-->` starts a cue. Timestamps may be `H:MM:SS,mmm`, `MM:SS.mmm` or similar, and VTT cue settings are ignored. The text runs to the next blank line.
* Handled: BOM, CRLF, missing index lines, `WEBVTT`/`NOTE` blocks.
* Stripped: `<…>` tags, `{…}` ASS override blocks, `\N`, `\n` and `\h`.
* Decoded: `&amp; &lt; &gt; &quot; &apos; &nbsp;` and numeric `&#…;`.
* The file is re-parsed whenever its size or mtime changes, checked once a second, so a growing extract is picked up.

## Launcher: `ub820run.subs.sh`

```
ub820run.subs.sh <file> [audio|none] [start_s] [sub: N|none|auto|/path/file.srt]
```

**Arg 4 defaults to `none`**, which behaves exactly like `ub820run.sh`.

`media.cgi` is not wired to this script yet:

* It launches `ub820run.sh` and passes only 3 args.
* Its `stop_play` finds the launcher with `pgrep -f 'ub820run.sh'`. That pattern does **not** match `ub820run.subs.sh`.

So, launched by hand, stop and seek from the web page work only because the launcher exits (and cleans up) once the player stops. To use subtitles from the web page, either:

* teach `media.cgi` to call `ub820run.subs.sh` with a 4th arg and widen its pgrep pattern (for example `ub820run`), or
* deploy this script under the old name once it is proven.

| arg 4 | what happens |
|---|---|
| `N` | Subtitle stream N, as ffmpeg's `0:s:N`, counting **all** subtitle streams. The list is in `/tmp/ub820run.subtracks` as `idx codec lang text\|bitmap`. |
| `N`, text codec (subrip/srt/ass/ssa/mov_text/webvtt/text) | Extracted in the **background**: `nice -n 10 $FF -nostdin -y -i file -map 0:s:N -flush_packets 1 -f srt /tmp/ub820.srt`. subovl picks cues up as the file grows. On a clean finish, `/tmp/ub820.srt.key` (file, track and size) is written so later runs and seeks reuse the file. An unfinished extract is killed on exit and leaves no key. |
| `N`, bitmap codec (hdmv_pgs_subtitle, dvd_subtitle, dvb_subtitle, …) | Logs `bitmap subtitles not supported yet` and plays without subtitles. |
| `auto` | Uses `<file>.srt` or `<file>.vtt` next to the media if present, else the first text stream. |
| `PATH` | Uses that `.srt` or `.vtt` directly. |

Other behaviour:

* **One `subovl` per player session**, inside the renewal loop:
  1. `$BIN/subovl <srt> $UB820_SUBOPT >> /tmp/subovl.log &` just before the player;
  2. `kill` (TERM) and `wait` right after it.

  Each rc=12 renewal starts a fresh daemon after the loop's `flipclear`. Leftover daemons from an earlier run get `killall` (TERM) first.
* **HD mode only.** In 4K mode the home UI is torn down and plane 7's state is unknown, so subtitles are skipped with a log line. `UB820_SUBS_4K=1` overrides this; it is untested, but the plane guard still applies.
* **Cleanup.** The lossless `trap … EXIT` is merged into `cleanup()`, which:
  * stops subovl,
  * stops or finalises the extract,
  * unmounts the patched vcavsrv if it was bound.

  `trap 'exit 143' TERM INT HUP` makes `kill` from `media.cgi` run the cleanup too.
* `/tmp/ub820run.cur` gains a 3rd line (the subtitle arg). `media.cgi` reads only lines 1–2.
* `UB820_DRY=1` also prints the extract and subovl commands.
* Host dry and fake runs worked: index, `auto`, missing index, unreadable file, cache reuse, and TERM cleanup.

## Host test harness

```
poc/subovl-test/test_host.sh [static NotoSansCJK-Regular.ttc]     (OUT=dir to choose the output dir)
```

* Renders single cues into a 1920x1080 raw dump and converts them with `ffmpeg -f rawvideo -pix_fmt bgra -s 1920x1080 -i x.raw x.png`. It also writes `*_bg.png` (over a gradient, so the black outline is visible) and `*_bottom.png` (a 1:1 crop).
* Fonts covered: TTF (glyf), the CJK OTC (CFF), and the builtin font.
* Content covered: Latin-1, tags, top cues, wrapping, overlapping cues, VTT, and the 3-line cap.
* Checks that nothing outside the boxes is touched and that partial-alpha pixels are black.
* Runs a growing-SRT scenario: the file is missing at start and cues are appended, as background extraction produces them. It also clears the fake LOSD0 mid-cue to check that the sentinel redraw fires.
* Runs the daemon loop against a scripted status file. The fake LOSD0 is a file (`SUBOVL_FAKEFB`), and a fake libavio flips plane 7 back every N checks (`SUBOVL_FAKE_FLIP`). It covers:
  * stale done ignored,
  * `init`, `prebuffer`, `playing`,
  * base offset,
  * pause freeze, resume,
  * done → exit within about 2 s,
  * buffer cleared,
  * AVIO balanced.

## Known limits / to verify on the device

1. **Watch the once-a-second Open/Close pattern first.** It is a deliberate change from clockovl's proven pattern, which holds one open, and it is unproven on hardware. The log prints `plane check #N: AVIO_UP_Open=.. Get_Addr(7)=.. sel=.. addr=..` on the first check and on every change. Expect:
   * `Open=0` every time;
   * `addr=0x21ff0000 (LOSD0)` after the first re-point;
   * `exit (...): plane checks N, re-points M` at the end.

   Afterwards, confirm that gserver and the next playback still behave, since Opens are refcounted with a maximum of 7.
2. **First live run:** start `subovl -v` by hand over a playing HD video, after `flipclear`, and check that:
   * the text is visible;
   * the outline looks right (this also confirms the per-pixel alpha conclusion on real hardware);
   * the cue changes line up with speech.
3. **The static ffmpeg must have the `srt` muxer and the `subrip` encoder.** Check with `$FF -hide_banner -muxers | grep srt` and `-encoders | grep subrip`. Without them the extract fails, and `/tmp/ub820run.log` shows the ffmpeg error.
4. **Compose cost on ARM** is unmeasured. The `-v` log prints milliseconds per cue.
5. **Seeks and renewals restart at a keyframe.** With `-ss` and stream copy, `pos=0` is the keyframe *before* the seek point, so subtitles can be off by up to one GOP after a seek or renewal. `--offset` (via `UB820_SUBOPT`) is the manual workaround. A real fix would read the first PTS against the requested start.
6. **Extraction reads the whole file.** On a big MKV over USB the first run's cues trail the extractor. A start at minute 60 waits until the extractor reaches minute 60, and it competes with playback for I/O at nice 10. Later runs of the same file and track use the cache. Possible improvements:
   * a second `-ss <start> -copyts` extract for the first minutes;
   * extracting the sidecar on the host.
7. Plane 7 is re-pointed within about 1 s of a gserver flip, so a remote keypress flashes the home UI for up to a second. That is by design: the alternative is to leave the UI covering the video.
8. ASS styling, positioning (other than `\an7/8/9`), karaoke, and colours are dropped. RTL text and complex shaping (Arabic, Indic) are not shaped; stb_truetype does no shaping.
9. Bitmap subtitles (PGS, DVD, DVB) are out of scope. A later route is to decode them with ffmpeg to images and blit them into the same LOSD0 box.
