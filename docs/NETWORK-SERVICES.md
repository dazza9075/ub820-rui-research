# Replacing the "Network Services" screen with your own page

How to make the **Network Services** tile on a Panasonic UB-series player open a page **you** control
— a fast local launcher instead of the remote VieraConnect portal — and how to launch the built-in
streaming apps from it. Method and your-own-code only. No keys, no firmware, no decrypted vendor source.

> Scope: this is about customising **your own** player's UI. Nothing here touches disc protection,
> region, or DRM. It is all data files and a local override you can turn off at any time.

## 1. What Network Services actually is

The "Network Services" tile launches the **VieraConnect** web portal, rendered by the player's
**AjaxCeplay** engine (`VCAjaxCeplay`, a WebKit/V8-based app framework — *not* a plain HTML browser).
The portal's entry page is a JavaScript "home-screen" app fetched from a URL named in a config file:

```
/usr2/dtvrec/data/vc/default_url_<region>.vieraconnect    # e.g. default_url_eu.vieraconnect
  initurl   https://bd-<region>.vieraconnect.<tld>/data/home-screen.js   # the entry page
  localurl  file:///usr/dtvrec/data/vcdemo/demo/home-screen.js           # local fallback (often absent)
  errorurl  file:///usr/dtvrec/data/vcdemo/demo/home-screen.js
  resurl    .../resource-<region>.js                                      # resource bundle
  ...
```

On many units the remote host still resolves (an AWS endpoint) and the portal loads; on others it stalls
and you get the "cannot connect" panel. Either way, **you can repoint `initurl` at your own local file.**

## 2. The override (toggleable, reversible, reboot-persistent)

`default_url_*.vieraconnect` lives on a read-only cramfs, but `/conf` is writable and survives firmware
flashes. Build an edited copy and **bind-mount** it over the original:

```sh
# build an override that points the entry page at our local file
cp /usr2/dtvrec/data/vc/default_url_eu.vieraconnect /conf/vc/default_url_eu.override
sed -i -e 's#^initurl .*#initurl file:///conf/vc/home-screen.js#' \
       -e 's#^localurl .*#localurl file:///conf/vc/home-screen.js#' \
       -e 's#^errorurl .*#errorurl file:///conf/vc/home-screen.js#' /conf/vc/default_url_eu.override
mount -o bind /conf/vc/default_url_eu.override /usr2/dtvrec/data/vc/default_url_eu.vieraconnect
```

`net_mgr` reads the config when Network Services opens, so the change takes effect on the next open with
**no reboot**. `umount` the path to revert instantly. See [`ns-redirect.sh`](../tools/ns-redirect.sh) for a
clean `on|off|apply|status` wrapper, and wire `… apply` into your boot script (it re-creates the mount if
a flag file is set) to make the redirect **survive reboot**.

## 3. Writing the home-screen page (the engine contract)

The entry page is an AjaxCeplay app, not HTML. A hand-written minimal page throws
`proc_require_initial_stage_error` and the engine finalises immediately (black screen). The reliable way
is to **model on a shipped panel** that already renders on this engine (the comm-error / please-wait
panels). The required shape:

```js
var sym = {};
onopen = function () {};
sym.bg    = new gbox({ translate:[1920/2,-1080/2,0], visible_p:true, width:1920, height:1080, color:[16,24,40,255] });
sym.title = new gtextbox({ /* align:LEFT, valign:MIDDLE, font_name:"F015T-regular", word_break:BREAK_ALL ... */ });
sym.panel = new container({ translate:[0,0,0], components:[sym.bg, sym.title /*, rows...*/] });

var stage_ = new stage({
  symbol: "home_screen", keep_me: true,
  in:  [{ from:["default"], hook:function(o){ o.visible_p=true; o.translate=[-960,540,0]; complete_on_stage(o); } }],
  out: [{ to:["default"],  hook:function(o){ o.visible_p=false; complete_off_stage(o); } }],
  initial_cursor: [],
  key_hook: function (type, keyVal) { /* see below */ return true; },
  components: [sym.panel]
});
```

Gotchas learned the hard way:

- **Coordinates** use `translate = [w/2 + x, -h/2 - y, 0]` for a box `w×h` at screen `(x,y)` from top-left.
  Logical space is 1920×1080 (scaled to the real 1280×720 framebuffer).
- **Only `LEFT`/`MIDDLE` alignment** values appear in shipped panels; an undefined constant during stage
  construction can throw. Use the shipped panels' exact property set.
- **Repaint rule:** an object redraws when a *draw-significant* property (e.g. `color`) changes. Changing
  only `.text` may not repaint — toggle `color` (or call `force_redraw()`) when you update text.
- **Keys** arrive in the stage `key_hook(type, keyVal)`. On this hardware: `UP=4098 DOWN=4101 OK/SELECT=4121`,
  and each physical press fires three events — type `1` (down), `2` (repeat), `0` (up). **Act on type 1
  only**, or navigation double-steps. `RETURN`/`EXIT` → `bd_extension.__tv_start()` leaves to live TV.

A full working example page is in [`examples/home-screen.example.js`](../examples/home-screen.example.js).

## 4. Launching the built-in apps from your page

The VC engine **blocks `XMLHttpRequest`** (locked to the authenticated portal origin), but the **native
`http_request({ url, method, sync:true, onload })`** call works and can reach a **localhost helper**. So:

1. Run a tiny local HTTP helper on the player (we reuse an existing on-device admin CGI on `127.0.0.1`).
2. Give it a `launch` action that issues the proven launch primitives:
   - **Netflix:** `POST cCMD_RC_NETFLIX` to `http://<player-ip>/WAN/dvdr/dvdr_ctrl.cgi` with header
     `User-Agent: MEI-LAN-REMOTE-CALL` (this is the standard LAN remote-control API; enable *Voice Control*
     so the player runs its on-LAN control server).
   - **YouTube / others:** DIAL — `POST` to `http://127.0.0.1:60030/dial_srv/apps/<AppName>`.
3. From the page, on OK: `http_request({ url:"http://127.0.0.1:<port>/.../launch?app=netflix", method:"GET", sync:true, onload:... })`.

`sync:true` matters — async callbacks do not reliably fire from engine hooks. See
[`examples/launch-helper.sh`](../examples/launch-helper.sh) for the helper logic (the control endpoint and
DIAL calls), minus any device-specific auth token.

## 5. Reference: on-device layout

Validated live `/dev/fmaN` map (useful when unpacking / knowing what survives a flash):

| part | mount | fs | notes |
|------|-------|----|-------|
| fma6 | /usr  | romfs | repack as **romfs**, never cramfs (brick risk) |
| fma7 | /usr2 | cramfs | all vendor software + the RUI/VC data |
| fma8 | /work | jffs2 | logs + Network Services runtime (keep mostly free) |
| fma10| /nv   | jffs2 | app caches, resume data |
| fma11| /conf | jffs2 | config DB; **survives flash** — put your overrides here |

SoC: Socionext UniPhier (PX3). Unpack firmware containers with `unblob` (cleaner than binwalk).

## 6. Turning it off

`umount /usr2/dtvrec/data/vc/default_url_eu.vieraconnect` (or `ns-redirect.sh off`) restores the stock
portal immediately. Removing the `/conf` flag file stops it re-applying at boot. Nothing is permanent and
no partition is rewritten.
