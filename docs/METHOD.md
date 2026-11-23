# Method: from stock player to readable UI source

1. **Region-free + root shell.** Build firmware with the upstream `pana-fw` region-free preset,
   add a static ARM `busybox`, and a startrc hook that supervises `busybox telnetd -l /bin/sh`
   on port 23 (see `firmware-recipes/`). Mount devpts so telnetd gets a pty. Flash via USB
   (MBR/FAT32 256MB–2GB, `PANAEUSB.FRM`, **Quick Start OFF**, rear USB port). Bump the patch
   version so it's a clean upgrade.
2. **Shell access.** `scripts/sh820.py <ip>` — telnet client that execs `busybox sh` (the stock
   `/bin/sh` is `sash`, which has no `;`/`|`). `busybox --install -s /tmp/bb` for applets.
3. **Locate the crypto.** Pull the `AjaxCeplay` binary and decompile with Ghidra headless
   (`ghidra_scripts/FindKey*.java`): it links stock `libcrypto`, uses `EVP_aes_128_cbc/ecb`, and a
   native `CipherKey.setKey()` class feeds the key. The module decryptor is AES-128-CBC, IV=0.
4. **Force + capture.** Firmware that deletes the `*.js.cache` forces on-load decryption; an
   `LD_PRELOAD` shim (`src/libjshook.c`) hooking `EVP_DecryptInit*` logs key+iv to `/tmp/jshook.log`.
   The shim is comm-gated to `AjaxCeplay`, uses raw syscalls (no glibc-version issues), has no
   constructor, and fails safe — so it can't wedge boot. (Validated by load-test + a known-key test
   on-device before flashing.)
5. **Harvest + decrypt.** Boot, navigate every menu/app, play a disc. Pull `/tmp/jshook.log`,
   dedupe the key windows, and brute-match them against the encrypted `.js` with `decrypt-rui.sh`.

## Hard-won gotchas
- Always flash with **Quick Start OFF** (warm-resume hung a post-flash boot once → brick; recovered
  via power-drain + a higher-version stock image on a 1GB stick, rear port).
- `apl_acemgr` does **not** cleanly respawn `AjaxCeplay` if you kill it, and halting it mid-startup
  (gdb attach) wedges the UI — so the `LD_PRELOAD`/cache-delete approach beats live debugging here.
- busybox `mount` lacks bind; use `src/bindmount.c`.
- The tmpfs scratch log resets on reboot; the firmware hook re-captures on next boot.
