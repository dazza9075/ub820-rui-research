#!/bin/busybox sh
# media.cgi — LAN web remote for the UB820 native player (busybox httpd, port 8822).
#   /cgi-bin/media              library + now playing (live status via ?st=1)
#   ?info=N                     file N: streams + play buttons per audio track
#   ?set=1                      settings page
# Actions (each answers with an instant return to the main page, so the back button is never needed):
#   ?play=N[&a=K|a=none]  ?stop=1  ?pause=1|0  ?seek=+-S  ?sw=K (audio track)  ?lossless=1|0
#   ?st=1                       status as JSON (polled by the page)
# Only numeric values are accepted; file names never come from the request.
BB=/bin/busybox; BIN=${UB820_BIN:-/tmp}; MNT=/tmp/usbro; ME=/cgi-bin/media
export LD_LIBRARY_PATH=/usr_root/lib:/lib:/usr/lib:/usr/X11R6/lib:/usr/dtvrec/lib:/usr/local/lib
Q="$QUERY_STRING"
num(){ printf '%s' "$Q" | $BB sed -n "s/.*\(^\|&\)$1=\([0-9][0-9]*\).*/\2/p" | $BB head -c 4; }
P=$(num play); I=$(num info); A=$(num a); printf '%s' "$Q" | $BB grep -q 'a=none' && A=none
STOP=$(printf '%s' "$Q" | $BB grep -c 'stop=1'); SW=$(num sw); SET=$(num set); ST=$(num st)
SEEK=$(printf '%s' "$Q" | $BB sed -n 's/.*seek=\(-\{0,1\}[0-9][0-9]*\).*/\1/p' | $BB head -c 6)
PAUSE=$(printf '%s' "$Q" | $BB sed -n 's/.*pause=\([01]\).*/\1/p')
LLQ=$(printf '%s' "$Q" | $BB sed -n 's/.*lossless=\([01]\).*/\1/p')
[ -d $MNT ] || $BB mkdir -p $MNT
$BB grep -q " $MNT " /proc/mounts || $BB mount -o ro /dev/sda1 $MNT 2>/dev/null
file_n(){ $BB ls -1 $MNT 2>/dev/null | $BB sed -n "$(( $1 + 1 ))p"; }
esc(){ printf '%s' "$1" | $BB sed 's/&/\&amp;/g; s/</\&lt;/g; s/>/\&gt;/g; s/"/\&quot;/g'; }
jesc(){ printf '%s' "$1" | $BB sed 's/\\/\\\\/g; s/"/\\"/g' | $BB tr -d '\n\r'; }
playing(){ $BB pidof ub820play >/dev/null; }
ctl(){ [ -p /tmp/ub820play.ctl ] && { echo "$1" > /tmp/ub820play.ctl & }; }
# stop_play [noh]: stop the session and let ub820run.sh finish by itself (it restores the home after a 4K session);
# noh = a seek restarts at once: tell ub820run to skip the restore. Kill only if it hangs, then restore here.
stop_play(){
  [ "$1" = noh ] && : > /tmp/ub820run.nohome
  ctl stop
  for n in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20; do $BB pgrep -f 'ub820run.sh' >/dev/null || return 0; $BB sleep 1; done
  for p in $($BB pgrep -f 'ub820run.sh'); do $BB kill "$p" 2>/dev/null; done
  $BB pidof ffmpeg >/dev/null && $BB killall ffmpeg 2>/dev/null
  if [ "$1" != noh ] && $BB grep -q 'route=4k ' /tmp/ub820play.status 2>/dev/null && [ -x $BIN/homerestore ]; then
    $BB sleep 2; $BIN/homerestore >> /tmp/homerestore.log 2>&1; fi
  $BB rm -f /tmp/ub820run.nohome; }
start_play(){ $BB setsid $BB nohup $BB sh $BIN/ub820run.sh "$1" "$2" "$3" auto > /tmp/ub820run.log 2>&1 < /dev/null & }
back(){ printf 'Content-Type: text/html\r\nCache-Control: no-store\r\n\r\n'
  printf '<!doctype html><meta http-equiv="refresh" content="0;url=%s"><script>location.replace("%s")</script>' "$ME" "$ME"; exit 0; }

# ---------------- status JSON ----------------
if [ -n "$ST" ]; then
  printf 'Content-Type: application/json\r\nCache-Control: no-store\r\n\r\n'
  S=$($BB cat /tmp/ub820play.status 2>/dev/null)
  g(){ printf '%s' "$S" | $BB sed -n "s/.*\(^\| \)$1=\([^ ]*\).*/\2/p"; }
  state=$(g state); PZ=false; case "$state" in *-paused) PZ=true; state=${state%-paused};; esac; pos=$(g pos); route=$(g route); au=$(g audio); playing || { [ "$state" = done ] || state=idle; }
  B=$($BB cat /tmp/ub820run.base 2>/dev/null); D=$($BB cat /tmp/ub820run.dur 2>/dev/null)
  T=$($BB awk -v b="${B:-0}" -v p="${pos:-0}" 'BEGIN{printf "%.1f", b+p}')
  F=$($BB sed -n 1p /tmp/ub820run.cur 2>/dev/null); F=${F##*/}; CA=$($BB sed -n 2p /tmp/ub820run.cur 2>/dev/null)
  E=$($BB grep -E 'ub820run: (player exit|subtitles|LOSSLESS)|ERROR' /tmp/ub820run.log 2>/dev/null | $BB tail -1)
  printf '{"state":"%s","t":%s,"dur":%s,"file":"%s","track":"%s","audio":"%s","route":"%s","paused":%s,"last":"%s","tracks":[' \
    "$(jesc "$state")" "${T:-0}" "${D:-0}" "$(jesc "$F")" "$(jesc "$CA")" "$(jesc "$au")" "$(jesc "$route")" \
    "$PZ" "$(jesc "$E")"
  c=""; [ -s /tmp/ub820run.tracks ] && while read -r n pid k cod lang; do
    printf '%s{"n":%s,"codec":"%s","lang":"%s"}' "$c" "$n" "$(jesc "$cod")" "$(jesc "$lang")"; c=","; done < /tmp/ub820run.tracks
  printf ']}'; exit 0
fi

# ---------------- actions ----------------
[ "$LLQ" = 1 ] && { : > /tmp/ub820.lossless; back; }
[ "$LLQ" = 0 ] && { $BB rm -f /tmp/ub820.lossless; back; }
if [ "$STOP" -gt 0 ]; then stop_play; back; fi
if [ -n "$SW" ]; then
  L=$($BB sed -n "$((SW+1))p" /tmp/ub820run.tracks 2>/dev/null)
  if [ -n "$L" ] && playing; then set -- $L; ctl "audio $2 $3"
    $BB sed -i "2s/.*/$SW/" /tmp/ub820run.cur 2>/dev/null; fi   # a later seek keeps this track
  back
fi
if [ -n "$PAUSE" ]; then [ "$PAUSE" = 1 ] && ctl pause; [ "$PAUSE" = 0 ] && ctl resume; back; fi
if [ -n "$SEEK" ] && [ -f /tmp/ub820run.cur ]; then
  CF=$($BB sed -n 1p /tmp/ub820run.cur); CA=$($BB sed -n 2p /tmp/ub820run.cur)
  B=$($BB cat /tmp/ub820run.base 2>/dev/null); POS=$($BB sed -n 's/.* pos=\([0-9.]*\).*/\1/p' /tmp/ub820play.status)
  D=$($BB cat /tmp/ub820run.dur 2>/dev/null)
  NP=$($BB awk -v b="${B:-0}" -v p="${POS:-0}" -v d="$SEEK" -v e="${D:-0}" 'BEGIN{x=b+p+d; if(e>0 && x>e-10) x=e-10; if(x<0)x=0; printf "%.1f", x}')
  stop_play noh
  start_play "$CF" "${CA:-0}" "$NP"
  back
fi
if [ -n "$P" ]; then
  F=$(file_n "$P")
  if [ -n "$F" ] && [ -f "$MNT/$F" ]; then playing && stop_play noh; start_play "$MNT/$F" "${A:-0}" ""; fi
  back
fi

# ---------------- pages ----------------
printf 'Content-Type: text/html\r\nCache-Control: no-store\r\n\r\n'
cat <<'EOF'
<!doctype html><html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>UB820 Media</title><style>
:root{--bg:#111418;--card:#1b2027;--fg:#e8ecf1;--mut:#9aa6b2;--acc:#3d8bfd;--bd:#2a313b}
@media (prefers-color-scheme: light){:root{--bg:#f3f5f8;--card:#fff;--fg:#14181d;--mut:#5c6773;--acc:#0b63ce;--bd:#d9dee5}}
*{box-sizing:border-box}body{font:16px/1.4 system-ui,sans-serif;margin:0;background:var(--bg);color:var(--fg)}
nav{position:sticky;top:0;display:flex;gap:.3em;padding:.6em;background:var(--card);border-bottom:1px solid var(--bd);z-index:2}
nav a{padding:.45em .9em;border-radius:8px;color:var(--fg);text-decoration:none;font-weight:600}
nav a.on{background:var(--acc);color:#fff}
main{max-width:56em;margin:0 auto;padding:.8em}
.card{background:var(--card);border:1px solid var(--bd);border-radius:12px;padding:1em;margin:0 0 1em}
h2{font-size:1.05em;margin:0 0 .6em;color:var(--mut);text-transform:uppercase;letter-spacing:.05em}
.b{display:inline-block;min-width:3.2em;text-align:center;padding:.55em .8em;margin:.15em;background:var(--acc);color:#fff;border-radius:8px;text-decoration:none;font-weight:600}
.b.g{background:transparent;color:var(--fg);border:1px solid var(--bd)}.b.on{outline:3px solid #fff3}
ol{margin:0;padding:0;list-style:none}li{display:flex;align-items:center;gap:.5em;padding:.5em 0;border-top:1px solid var(--bd)}
li:first-child{border-top:0}li a.t{flex:1;color:var(--fg);text-decoration:none;word-break:break-word}
.mut{color:var(--mut);font-size:.9em}.bar{height:8px;background:var(--bd);border-radius:4px;overflow:hidden;margin:.6em 0}
.bar i{display:block;height:100%;background:var(--acc);width:0}#np .ttl{font-weight:600;word-break:break-word}
pre{white-space:pre-wrap;font-size:12px;color:var(--mut)}
</style></head><body>
EOF
TAB=lib; [ -n "$SET" ] && TAB=set; [ -n "$I" ] && TAB=lib
on(){ [ "$TAB" = "$1" ] && printf ' class="on"'; }
printf '<nav><a href="%s"%s>Library</a><a href="%s#np"%s>Now playing</a><a href="%s?set=1"%s>Settings</a></nav><main>' \
  "$ME" "$(on lib)" "$ME" "" "$ME" "$(on set)"

if [ "$TAB" = set ]; then
  printf '<div class="card"><h2>Audio</h2>'
  if [ -f /tmp/ub820.lossless ]; then
    printf '<p>Lossless DTS-HD / TrueHD passthrough: <b>ON</b> (experimental)</p><a class="b" href="?lossless=0">Turn off</a>'
  else printf '<p>Lossless DTS-HD / TrueHD passthrough: <b>off</b>. DTS and TrueHD are converted to Dolby Digital 5.1.</p><a class="b g" href="?lossless=1">Turn on (experimental)</a>'; fi
  printf '<p class="mut">Experimental mode currently reaches the AV receiver as <b>stereo PCM</b> (tested 2026-11-28), so Dolby Digital 5.1 (off) is the better choice for now.</p></div>'
  printf '<div class="card"><h2>TV remote during playback</h2><p class="mut">OK / PLAY / PAUSE = pause &middot; STOP / RETURN / HOME = stop &middot; LEFT / RIGHT = -/+30 s &middot; SKIP / FF / REW = -/+5 min &middot; AUDIO = next audio track</p></div>'
  printf '</main></body></html>'; exit 0
fi

# now playing (filled by the script below)
cat <<'EOF'
<div class="card" id="np"><h2>Now playing</h2><div class="ttl" id="npf">-</div>
<div class="bar"><i id="npb"></i></div><div class="mut" id="npt">idle</div>
<div id="npc" style="margin-top:.5em">
<a class="b g" href="?seek=-300">&laquo; 5m</a><a class="b g" href="?seek=-30">&lsaquo; 30s</a>
<a class="b" id="pp" href="?pause=1">Pause</a>
<a class="b g" href="?seek=30">30s &rsaquo;</a><a class="b g" href="?seek=300">5m &raquo;</a>
<a class="b" style="background:#c0392b" href="?stop=1">Stop</a></div>
<div id="npa" style="margin-top:.4em"></div><div class="mut" id="npl"></div></div>
<script>
function f(s){s=Math.max(0,Math.floor(s));var h=Math.floor(s/3600),m=Math.floor(s%3600/60),x=s%60;return (h?h+":"+(m<10?"0":""):"")+m+":"+(x<10?"0":"")+x}
function up(){var r=new XMLHttpRequest();r.open("GET","?st=1&_="+Date.now());r.onload=function(){try{var j=JSON.parse(r.responseText)}catch(e){return}
var on=j.state=="playing"||j.state=="prebuffer"||j.state=="draining";document.getElementById("npc").style.display=on?"":"none";
document.getElementById("npf").textContent=j.file||"-";
document.getElementById("npt").textContent=on?(j.paused?"Paused ":"")+f(j.t)+(j.dur>0?" / "+f(j.dur):""):(j.state=="idle"||!j.state?"Nothing playing":"Stopped ("+j.state+")");
document.getElementById("npb").style.width=(on&&j.dur>0?Math.min(100,100*j.t/j.dur):0)+"%";
var pp=document.getElementById("pp");pp.textContent=j.paused?"Resume":"Pause";pp.href=j.paused?"?pause=0":"?pause=1";
var a="";if(on&&j.tracks.length>1)for(var i=0;i<j.tracks.length;i++){var t=j.tracks[i];a+='<a class="b g'+(String(t.n)==j.track?' on':'')+'" href="?sw='+t.n+'">Audio '+(t.n+1)+": "+t.codec+(t.lang?" ("+t.lang+")":"")+"</a>"}
document.getElementById("npa").innerHTML=a;document.getElementById("npl").textContent=on?"":(j.last||"")};r.send()}
up();setInterval(up,2000);
</script>
EOF

if [ -n "$I" ]; then
  F=$(file_n "$I")
  printf '<div class="card"><h2>File</h2>'
  if [ -n "$F" ]; then
    printf '<div class="ttl"><b>%s</b></div><pre>' "$(esc "$F")"
    $BIN/ffmpeg -hide_banner -i "$MNT/$F" 2>&1 | $BB grep -E 'Duration|Stream #' | $BB sed 's/&/\&amp;/g; s/</\&lt;/g'
    printf '</pre>'
    n=0; for t in $($BIN/ffmpeg -hide_banner -i "$MNT/$F" 2>&1 | $BB grep 'Stream #.*Audio:' | $BB sed 's/ /_/g'); do
      printf '<a class="b" href="?play=%s&a=%s">Play, audio %s</a> ' "$I" "$n" "$((n+1))"; n=$((n+1)); done
    printf '<a class="b g" href="?play=%s&a=none">Play without audio</a> <a class="b g" href="%s">Back to library</a>' "$I" "$ME"
  else printf '<p>No such file.</p><a class="b g" href="%s">Back to library</a>' "$ME"; fi
  printf '</div></main></body></html>'; exit 0
fi

printf '<div class="card"><h2>USB library</h2><ol>'
i=0; $BB ls -1 $MNT 2>/dev/null | while IFS= read -r f; do
  printf '<li><a class="t" href="?play=%s">%s</a><a class="b g" href="?info=%s">Tracks</a><a class="b" href="?play=%s">Play</a></li>' \
    "$i" "$(esc "$f")" "$i" "$i"; i=$((i+1)); done
printf '</ol></div></main></body></html>'
