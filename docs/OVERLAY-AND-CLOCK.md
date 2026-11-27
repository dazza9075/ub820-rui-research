# DP-UB820 — on-screen overlay and clock (native code, no compositor changes)

Status (2026-11-27): **working, verified on a real TV.** A small native daemon draws a live clock
over the stock UI and over 4K playback; it survives navigation, Setup, standby and reboot, and is
toggled from the admin panel.

## Ground truth (read-only probes first)
- `examples/native/iosdprobe.c` calls `_AVIO_UP_GFX_Get_Addr(plane, u32[4])` for planes 0–21
  (balanced `AVIO_UP_Open(0)` / `AVIO_UP_Close()`). At the home screen only **plane 7** is set:
  `{sel=1, addr=0x21010000, 0, 0}`.
- Pressing a nav key and probing again shows plane 7 flipping between `0x21010000` and
  `0x217fc000`: this is gserver's **double-buffered page flip of the UI plane**.
- The address is a **bus address = physical − 0xC0000000** (0x21010000 ↔ phys 0xE1010000, the UI
  buffer that `/dev/exmem` writes were already proven to show on screen). Passing a raw physical
  address to `AVIO_UP_GFX_Set_Addr` is why earlier "free plane" attempts never appeared.

## Three proofs
1. `flip7.c` — copy the current UI buffer into the spare LOSD0 buffer, draw on it, then
   `AVIO_UP_GFX_Set_Addr(7, {1, bus, 0, 0}, 1)`. Our frame is shown; any remote key makes gserver
   flip back (clean restore).
2. `ovl.c` — never calls Set_Addr. Polls `Get_Addr(7)` every 10 ms and redraws a banner into
   gserver's *current* front buffer whenever it flips or our pixels were overwritten. No visible
   flicker during navigation.
3. `flipwin.c` — punch an alpha-0 rectangle into a UI copy (half RGB black, half RGB white, both
   alpha 0). Result is uniformly the colour of the video layer → **per-pixel alpha is honoured**;
   a native player can show video through a hole in the UI plane.

## The clock (`examples/native/clockovl.c`, `examples/hbclock/hbclock.sh`)
- Same method as `ovl.c`; embedded 5×7 font ×6, rendered to an off-screen tile and copied row by
  row. Redraws on flip / overwrite / text change.
- `hbclock 0 uk` = run forever, UK GMT/BST rule (last Sunday Mar/Oct, 01:00 UTC) computed in-binary
  (the device clock is correct UTC; only its display TZ is CJT).
- Holds **one** `AVIO_UP_Open` for its lifetime and closes it on exit; exits if gserver disappears.
  Keep opens balanced — `AVIO_UP_Open` is refcounted system-wide with a hard max of 7, and leaking
  opens corrupts the mixer (black screen).
- `hbclock.sh supervise` is started detached from the user tweak script at boot; it starts the
  clock only while an enable flag exists and the UI is up. Normal standby keeps gserver alive, so
  the clock simply continues.

## Gotchas
- Device shell (sash) rejects `VAR=x cmd`; export `LD_LIBRARY_PATH` first.
- Binaries must log with `write(2)` only (stdout/stderr data symbols crash at load on ld-2.20).
- `/sbin/reboot` ends in STANDBY; send a POWERON afterwards.

## Video player status
See `docs/VCAV-API.md` and `examples/native/player_skeleton.c`. The vcavsrv client protocol works
end to end live (server spawn, mode, handles, codec, chunked ES write, teardown), but at the home
screen the decoder channel cannot be opened (`AVCC_Open VDEC DDOPEN -2`). The stock USB player
does not use vcavsrv: its `fmpsrv` drives libavd in-process through the `lavd` iosc bridge with
explicit lock/unlock. Acquiring the decoder is the current open item.
