# Safe custom-firmware platform & RUI modding (method)

How to build a **recoverable** development platform on the DP-UB820 (A8640EU) and iterate on the
RUI **without reflashing** — method and architecture only, no keys/firmware/decrypted source.

## 1. Structural recoverability

A repacked `.FRM` is AES-encrypted but **not signed**. The MAIN module holds the kernel + the three
filesystems (`fma4` kernel, `fma5`/`fma7` cramfs, `fma6` romfs); **PROG** (bootloader) and **MINI**
(miniboot) are separate modules. **Only patch MAIN.** PROG/MINI pass through untouched, so the
bootloader-driven USB recovery path stays intact.

**Verify it** (don't assume): decrypt both your stock and modified FRM and compare module hashes —
PROG/MINI/DRV1/BUCD must be byte-identical; only MAIN differs. A ~40-line Go tool over the pana-fw
`firmware` package (`archive.UnpackFirmwareArchive` → `ModuleEntries.FromBuffer` → `mod.Data()`) does
this. This is *recoverable*, **not** "unbrickable" — power-loss mid-flash, NAND/EEPROM faults etc. still
exist.

**Recovery that works for a boots-but-crashes image:** put a **higher-version** known-good FRM on a
small (≤2 GB physical) FAT32/MBR stick, reach the player's "Play" state, then **hot-plug** the stick →
the bootloader force-updates in a clean update environment immune to the broken userland.

## 2. Boot init: recovery before experiments

Order matters. A hanging `/conf` hook must never block recovery:

1. GPU power policy (non-blocking, always).
2. **Recovery/safety decisions first** — USB_SAFE failsafe + auto-rollback marker.
3. Controlled RUI overlay — early (must precede UI init) but **`timeout`-bounded**.
4. SSH/supervisor management plane — **before** any arbitrary `/conf` code.
5. Arbitrary user hook — **last, time-bounded**.
6. **UI health check → auto-rollback**: arm a marker before experimental code; clear it only after the
   UI engine is confirmed up *and stable*; if it never comes up, the marker persists and the **next boot
   skips experimental code**. Health = manager **and** renderer process up and stable over a window
   (process existence alone is not health).

Keep an **independent telnet/SSH recovery service** launched separately (never gated by experiments),
a **USB_SAFE** marker file that skips all `/conf` hooks at boot (validate it physically), a process cap
(`ulimit -u`) early, and a **strict log ceiling** (log to tmpfs, persist only a bounded tail to the tiny
`~1.5 MB` config partition, with a free-space reserve).

> Gotcha: the on-device `/bin/sh` is **sash** (no `[ ]`, `||`, `|`, `;`). Write init/run scripts as
> `#!/bin/busybox sh` (ash) or call busybox per command — a `[ ]||` line under `#!/bin/sh` fork-bombs.

## 3. Live-edit loop (no reflash)

The engine runs pre-built `.js.cache` bytecode; with the cache **absent** it re-decrypts the `.js` on
load. So: keep patched modules in the config partition, at boot build a tmpfs copy of the `jslib` dir
with your patched modules in place and **their `.js.cache` removed** (absent — a 0-byte cache crashes the
engine), then `mount --bind` it over the read-only original. Auto-rollback (above) protects it.

## 4. Admin backend security

If you add a local management HTTP service: bind **localhost only**; make state changes **POST + a random
token** (stored on-device, never in the page or repo); **no wildcard CORS**; strict **command allowlist**
(no arbitrary shell, inputs constrained e.g. `[a-z]*`); no debug endpoints in production; bounded logging.
A hidden remote-key combination is an *activation* mechanism, not authentication. LAN exposure needs its
own deliberate auth design.

## 5. RUI input/render architecture (for an on-screen admin overlay)

Findings for anyone attempting a native-looking overlay:
- **Input** is delivered via a proprietary device node, **not** standard `evdev` (`/proc/bus/input/devices`
  is empty, no `/sys/class/rc`). The core key dispatcher lives in the main engine lib; home-screen keys do
  not reach the bootstrap stage hook, global `onkeydown`, or a re-defined property (the engine is too old
  for `Object.defineProperty`), and that lib resists re-encryption.
- **Panels** render through a dedicated OSD screen mode plus a built stage; the home context's panel
  helper is a reduced stub. Primitives (`gbox`/`gtextbox`/`stage`) and the modal `waitStage` key path are
  available; `setTimeout`/`setInterval` fire.
- A promising route is a **native app launched from a home menu item** (the player already launches native
  apps), or a passive native listener on the input node signalling a localhost service that patched JS
  polls — both sidestep fighting the JS engine.
