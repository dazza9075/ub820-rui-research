# RUI UI architecture & on-screen overlay findings (method/spec)

Reverse-engineering findings on how the `AjaxCeplay` RUI engine structures the on-screen UI, and what it
takes to add a custom on-screen panel. Findings and specifications only — no keys, firmware, or decrypted
source. Run tools on hardware you own.

## The engine is view-based
`AjaxCeplay` (WebKit + V8) renders the UI as a set of **`file://` views**, not one app. Each screen is its
own set of `.js` under `.../rui/ace001/<name>/` (e.g. `bdav/` disc playback, `nas/`, `portal/` apps,
`dlna/`, `filelist/`, `common/`, plus `jslib/` as the shared library and `home.js` as the bootstrap).
Views are loaded on demand (`add_stage_load_path(symbol, url)` → `prepare_stage` → `on_stage`, driven by
`AceAppChange`). On the player, only `jslib/` and `bdav/` ship as source `.js`; other views ship as
pre-compiled bytecode (`.js.cache`, magic `0xDEAD0007`).

Native apps (e.g. the streaming app) are separate ELF binaries launched by the app manager via a
"netlauncher" process with `/tmp` flag-file IPC — they link EGL + JavaScriptCore + the vendor framework.

## Key handling: the active view owns input
Remote input is delivered through a **proprietary device node** (not standard `evdev`; `/proc/bus/input/
devices` is empty, no `/sys/class/rc`). The engine's central key dispatcher calls a global `onkeydown`
(and `onkeypress`/`onkeyup`). A **view registers its handlers when it activates** (`view.onkeydown = …`),
so the *active view* owns input. This is why injecting a key hook from the bootstrap context fails — the
active view has already claimed `onkeydown`, and it swaps handlers per sub-mode.

## Rendering
Panels are built from primitives (`gbox`, `gtextbox`, group/stage) and drawn via a native
`force_sync_redraw`; some flows use a dedicated OSD screen mode. The home context's generic panel helper
is a reduced stub (no button-panel builder) — the full one lives in a lib that isn't loaded there.

## Adding a custom on-screen panel — the tractable path
Rather than fighting the engine from the bootstrap, add a **proper view** and load it like any other:
- Model it on the one shipping-source view (`bdav`): an entry file sets base URLs and `load()`s its
  modules; the main module creates a stage, builds components, and registers `view.onkeydown` etc.
- Trigger it from the editable bootstrap: engine timers (`setInterval`/`setTimeout`) **do fire**, so poll a
  flag and, when set, load the view via the view-load API. The view then owns input and renders natively.
- This cleanly separates *rendering* from *activation* — solve and test them independently.

### Known blocker
The engine **decrypts every `.js` on load** (native cipher), so a brand-new view file needs a decryption
key the engine will apply to that path; the per-file key scheme (16-byte windows over a shared key stream)
is engine-internal. Resolve by: static analysis of the engine's cipher routine; shipping the new view
inside an existing known-key module's load chain; or replacing an existing view (delete its `.js.cache`,
provide a same-key `.js`). Compiled-bytecode views cannot be edited as source.

## Capture tools
Extract the engine/app binaries from a stock image on your own device: demux the MAIN module into its
partitions, then convert the second CRAMFS to a tar and pull `/dtvrec/bin/*`. Engine `strings` expose the
native JS binding names, the view-URL map, and the native request protocol.
