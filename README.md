# DP-UB820 RUI research & homebrew firmware toolkit

Reverse-engineering notes and tooling for the **Panasonic DP-UB820** 4K Blu-ray player
(firmware model **A8640EU**, shared across UB420/UB820/UB9000). Documents how to build custom
firmware (region-free + a root shell) and how the encrypted "RUI" WebKit/JavaScript UI is
protected — and how to recover **your own** UI source on **your own** device.

> ⚠️ **What this repo is and isn't.** This is *method and tooling only*. It does **not** contain
> Panasonic firmware, Panasonic binaries, decrypted Panasonic UI source, or extracted keys.
> Those are Panasonic's copyrighted material and/or content-protection keys — run the tools on
> hardware **you own** to produce them locally. Do not redistribute them. For authorised
> research / interoperability / repair on your own device.

## What's been figured out
- The `.FRM` firmware is whole-file **AES-encrypted but not signed**, so repacked images flash and
  boot. (Firmware crypto + a region-free preset come from the upstream `pana-fw` project.)
- A minimal custom firmware adds a **busybox `telnetd` root shell** (port 23) and keeps region-free.
- The on-screen UI ("RUI") is a **WebKit + V8** app (engine binary `AjaxCeplay`). Its `.js` modules
  are a **second, separate layer of encryption**: **AES-128-CBC, IV=0, no padding**, via a native
  JS class `CipherKey.setKey()`. Keys are 16-byte windows over a key stream (shared across modules).
- The engine normally runs **pre-built `.js.cache` bytecode** (`0xDEAD0007`), so it never decrypts
  `.js` at runtime. **Trick:** ship firmware with the `.js.cache` deleted → the engine is forced to
  decrypt each `.js` on load → an `LD_PRELOAD` shim hooking OpenSSL `EVP_DecryptInit` logs the keys.
  Navigate the menus and you capture the key stream; decrypt your UI source offline.

## Repo layout
- `src/libjshook.c` — LD_PRELOAD interposer: logs AES key/iv/passphrase when `AjaxCeplay` decrypts
  (comm-gated, raw-syscall I/O, fail-safe). `*_memcpy_variant.c` also hooks the `setKey` memcpy.
- `src/bindmount.c` — tiny `mount(MS_BIND)` helper (busybox on-device lacks bind).
- `scripts/` — `sh820.py` (telnet shell client), `decrypt-rui.sh` (openssl AES-128-CBC), ELF/Thumb
  analysis helpers (`plt2.py`, etc.).
- `ghidra_scripts/` — headless decompile scripts used to locate the crypto (`CipherKey`, `setKey`).
- `firmware-recipes/` — the shell additions we inject (telnet supervisor + startrc hook). The
  region-free patches, busybox binary, and repack tool come from upstream — see below.
- `docs/` — METHOD.md (full walkthrough) and CRYPTO.md (the RUI scheme).

## Reproduce on your own device
1. Get the repack toolchain: **WeebDataHoarder/pana-fw** (has the A8640EU firmware AES key +
   region-free preset). Build a static ARM `busybox` (busybox.net).
2. Build firmware = region-free preset + busybox + `firmware-recipes/` (telnet). Flash it
   (USB, MBR/FAT32 256MB–2GB, rename `PANAEUSB.FRM`, **Quick Start OFF**, rear port).
3. Cross-compile `src/libjshook.c` (armv7 hard-float) and add it as `/lib/libjshook.so` with
   `setenv LD_PRELOAD /lib/libjshook.so` in startrc; also delete the `*.js.cache` (that have a
   `.js` sibling) to force decryption.
4. Flash, boot, navigate every menu/app/disc. Pull `/tmp/jshook.log`, then
   `decrypt-rui.sh <enc.js> <keyhex>` (IV = 32 zero hex) to recover each module.

## Credits / upstream
- `WeebDataHoarder/pana-fw` — firmware decrypt/repack + region-free preset (the foundation).
- This repo adds: the telnet shell recipe, the RUI-encryption analysis, and the key-capture method.

## License
Original code here is MIT (see LICENSE). Panasonic trademarks/firmware are not included and remain
Panasonic's property.
