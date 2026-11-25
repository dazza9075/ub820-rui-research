# On-screen admin panel: how it was made to work (method)

Findings from building a custom on-screen admin panel (status + toggles) as a real RUI view on the
DP-UB820's `AjaxCeplay` engine. Method and specifications only. No keys, firmware or decrypted
source are included here. Run this only on hardware you own.

## Result
On the home menu, the remote sequence **Up Up Down Down Up Down** opens a panel. It shows live
status (IP, SSH, telnet, GPU power policy) and lets you toggle services. RETURN closes it back to
a working home menu. The panel can be opened and closed any number of times. Everything is
deployed as `/conf` data over a tmpfs overlay, so no reflash is needed to iterate.

## Getting data back from the backend
- The RUI `XMLHttpRequest` is a **JS shim over a native `http_request(obj)`**, not a browser XHR.
  `setRequestHeader` is a no-op. Requests **do reach** a localhost CGI (GET and POST bodies alike),
  but `responseText` comes back **empty** with status 422, regardless of Content-Type or
  Content-Length. `file://` reads through the shim also come back empty.
- What works: the backend writes its reply as a **`.js` module encrypted in the engine's own module
  format** into the overlay directory (deleting any stale `.js.cache`). The view then `load()`s it,
  the same path every engine module uses. The per-file key derivation is described in `CRYPTO.md`.
  A small static encryptor runs on the device (`src/aesrui.c`).
- Use **synchronous** requests from the view. Timers created while a view is being built never
  fire; timers created from key handlers do.

## Closing back to the home menu (the hard part)
- Switching to a view (`AceAppChange_OnChange` → `on_stage`) runs the outgoing view's stage-out
  hook, which tears the home menu (portal) down. Switching back does **not** redraw it, so you get
  a black screen.
- `System.stopAjaxApp` (what the disc app uses on RETURN) is **refused** for a view the native
  side did not launch.
- The fix came from **tracing what the native side does** when you leave Setup / Home Network /
  Network Services. Instrument the bootstrap's change function and lifecycle globals. The
  sequence is always:
  `onchange_before()` → `onchange(portalURL)` (→ `on_stage`) → `onstart()` → `onplay()`.
  `onstart`/`onplay` are shared globals that the portal's stage-in re-binds to its own
  start/redraw. Replaying that sequence on close restores a fully working home menu.
- A closed view's stage is cached (`keep_me`). A second `on_stage` reshows the dead stage
  without re-running its script, so call `discard_stage(viewURL)` before each reopen (from the
  portal, never from inside the view being discarded).

## Catching remote keys on the home menu
- Wrapping the global `onkeydown` from the bootstrap catches **nothing**: the portal receives
  keys on its **stage object's `key_hook`**, which the native side calls directly.
- The global `stages{}` map (used by the library's `discard_stage` wrapper) gives you the
  portal's stage object, so you can chain-wrap `stages[currentStage].key_hook`. Make it a pure
  pass-through that always calls the original and returns its result. Re-apply it after each
  stage change.
- Codes seen at the portal: Up 46, Down 47, Left 48, Right 49, OK 44, RETURN 45 (the same as the
  engine's `KEY_*` constants). **Number keys never reach the portal**; they are swallowed
  natively, so a numeric combo is impossible there. Up/Down are no-ops on the horizontal carousel,
  so a combo made only of them doesn't disturb the menu.
- The native engine also exports `set_system_key_hook`. It is unused here because its semantics
  are unknown.

## Safety design
- Backend: localhost-only CGI with a fixed command allowlist. State changes need POST plus a
  per-device token. The token reaches the view as an encrypted module that web content can't
  `load()`. The backend refuses to disable the last remote-access service.
- "Boot Without Patches" is **one-shot**. The init deletes the flag *before* skipping the
  experimental code, so a crash can't make it sticky. This is separate from the sticky
  auto-rollback marker, which is set when the UI fails its health check.
- The home menu's own item list lives in opaque engine bytecode (no readable strings), so adding a
  carousel entry would need a bytecode decoder first.
