/* Custom Network Services — v10 (working launcher).
 * Built on the known-good commerror skeleton. Renders a navigable menu; OK launches the selected app via
 * the PROVEN path: native http_request (sync) -> our admin CGI on :8820 -> cCMD_RC_NETFLIX / DIAL YouTube.
 * Keys (empirical on this UB820): UP=4098, DOWN=4101, OK/SELECT=4121; each press fires type 1(down),
 * 2(repeat), 0(up) -> act on type 1 only. report() pings the CGI dbg log so the page stays observable.
 */

var sym = {};

var lang = 0;
if (system.language.indexOf("en-US") == 0) { lang = 0; }
else if (system.language.indexOf("en-GB") == 0) { lang = 3; }
var losd_type = "QHD";
if (bd_extension.__get_losd_resolution().indexOf("QHD") == -1) { losd_type = "FHD"; }
onopen = function() {};

var K_OK = 4121, K_UP = 4098, K_DN = 4101;   /* empirical; TXK_* also checked as fallback */

var ROWS = [
    { label: "YouTube", act: "youtube" },
    { label: "Netflix", act: "netflix" },
    { label: "Amazon",  act: "amazon"  }
];
var sel = 0;

var C_BG = [16, 24, 40, 255], C_TITLE = [255, 255, 255, 255];
var C_ROW = [200, 205, 215, 255], C_SEL = [255, 210, 90, 255];
var C_ST_A = [120, 200, 255, 255], C_ST_B = [90, 170, 230, 255];

function tr(x, y, w, h) { return [w / 2 + x, -h / 2 - y, 0.0]; }

function report(tag) {
    try { if (typeof http_request == "function") {
        http_request({ url: "http://127.0.0.1:8820/cgi-bin/admin?action=dbg&v=" + tag, method: "GET", sync: true, onload: function () {} });
    } } catch (e) {}
}

sym.bg = new gbox({ "translate": tr(0, 0, 1920, 1080), "visible_p": true, "width": 1920, "height": 1080, "color": C_BG });
sym.title = new gtextbox({ "translate": tr(480, 150, 1300, 80), "visible_p": true, "align": LEFT, "valign": MIDDLE,
    "text": "Network Services", "font_size": 52, "width": 1300, "height": 80, "color": C_TITLE,
    "font_name": "F015T-regular", "word_break": BREAK_ALL });

var ROW_X = 520, ROW_Y0 = 320, ROW_DY = 82, ROW_W = 1000, ROW_H = 64;
sym.rows = [];
var comps = [sym.bg, sym.title];
for (var i = 0; i < ROWS.length; i++) {
    sym.rows[i] = new gtextbox({ "translate": tr(ROW_X, ROW_Y0 + i * ROW_DY, ROW_W, ROW_H), "visible_p": true,
        "align": LEFT, "valign": MIDDLE, "text": ROWS[i].label, "font_size": 40, "width": ROW_W, "height": ROW_H,
        "color": C_ROW, "font_name": "F015T-regular", "word_break": BREAK_ALL });
    comps.push(sym.rows[i]);
}
sym.hint = new gtextbox({ "translate": tr(520, 800, 1000, 48), "visible_p": true, "align": LEFT, "valign": MIDDLE,
    "text": "UP/DOWN  Move      OK  Open      RETURN  Exit", "font_size": 26, "width": 1000, "height": 48,
    "color": [150, 155, 165, 255], "font_name": "F015T-regular", "word_break": BREAK_ALL });
comps.push(sym.hint);
/* status line (color toggles on change so it actually repaints) */
sym.status = new gtextbox({ "translate": tr(520, 870, 1000, 48), "visible_p": true, "align": LEFT, "valign": MIDDLE,
    "text": "", "font_size": 26, "width": 1000, "height": 48, "color": C_ST_A,
    "font_name": "F015T-regular", "word_break": BREAK_ALL });
comps.push(sym.status);

sym.panel = new container({ "translate": [0.0, 0.0, 0.0], "components": comps });

var stToggle = 0;
function setStatus(s) { stToggle ^= 1; sym.status.text = s; sym.status.color = stToggle ? C_ST_A : C_ST_B; try { force_redraw(); } catch (e) {} }

function paint() {
    for (var i = 0; i < sym.rows.length; i++) {
        sym.rows[i].color = (i == sel) ? C_SEL : C_ROW;
        sym.rows[i].text  = (i == sel ? "> " : "   ") + ROWS[i].label;
    }
    try { force_redraw(); } catch (e) {}
}

function doLaunch(act) {
    report("LAUNCH_" + act);
    setStatus("Opening " + act + " ...");
    try {
        http_request({ url: "http://127.0.0.1:8820/cgi-bin/admin?action=launch&app=" + act, method: "GET", sync: true,
            onload: function (status, header, body) { report("LAUNCHRESP_" + act + "_s" + status); } });
    } catch (e) { setStatus("launch error"); report("LAUNCHERR_" + act); }
}

var menuStage = new stage({
    "symbol": "home_screen", "keep_me": true,
    "in": [ { "from": ["default"], "hook": function (obj) {
                obj.visible_p = true; obj.translate = [-960.0, 540.0, 0.0]; complete_on_stage(obj); paint(); report("VC_STAGE_IN");
            } } ],
    "out": [ { "to": ["default"], "hook": function (obj) { obj.visible_p = false; complete_off_stage(obj); } } ],
    "initial_cursor": [],
    "key_hook": function (type, keyVal) {
        if (type != KEY_DOWN) { return true; }          /* act once per press */
        if (keyVal == K_UP   || keyVal == TXK_UP)   { sel = (sel + ROWS.length - 1) % ROWS.length; paint(); return true; }
        if (keyVal == K_DN   || keyVal == TXK_DOWN) { sel = (sel + 1) % ROWS.length; paint(); return true; }
        if (keyVal == K_OK   || keyVal == TXK_ENTER){ doLaunch(ROWS[sel].act); return true; }
        if (keyVal == TXK_RETURN || keyVal == TXK_EXIT) { bd_extension.__tv_start(); return true; }
        return true;
    },
    "components": [ sym.panel ]
});
