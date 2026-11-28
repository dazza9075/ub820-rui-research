#!/bin/busybox sh
# media.cgi — LAN web remote for the UB820 native player (busybox httpd, port 8822).
#   /cgi-bin/media                 list files on the USB stick, current status
#   ?play=N[&a=K]                  play file N (index in the list) with audio track K (default 0, "none" = no audio)
#   ?info=N                        show the streams of file N with per-track play links
#   ?stop=1                        stop playback
# Only numeric indices are accepted; file names never come from the request.
BB=/bin/busybox; BIN=${UB820_BIN:-/tmp}; MNT=/tmp/usbro
Q="$QUERY_STRING"
num(){ printf '%s' "$Q" | $BB sed -n "s/.*$1=\([0-9][0-9]*\).*/\1/p" | $BB head -c 4; }
P=$(num play); I=$(num info); A=$(num a); printf '%s' "$Q" | $BB grep -q 'a=none' && A=none
STOP=$(printf '%s' "$Q" | $BB grep -c 'stop=1'); SW=$(num sw)
SEEK=$(printf '%s' "$Q" | $BB sed -n 's/.*seek=\(-\{0,1\}[0-9][0-9]*\).*/\1/p' | $BB head -c 6)
PAUSE=$(printf '%s' "$Q" | $BB sed -n 's/.*pause=\([01]\).*/\1/p')
LLQ=$(printf '%s' "$Q" | $BB sed -n 's/.*lossless=\([01]\).*/\1/p')
[ "$LLQ" = 1 ] && : > /tmp/ub820.lossless; [ "$LLQ" = 0 ] && $BB rm -f /tmp/ub820.lossless
[ -d $MNT ] || $BB mkdir -p $MNT
$BB grep -q " $MNT " /proc/mounts || $BB mount -o ro /dev/sda1 $MNT 2>/dev/null
file_n(){ $BB ls -1 $MNT 2>/dev/null | $BB sed -n "$(( $1 + 1 ))p"; }
esc(){ printf '%s' "$1" | $BB sed 's/&/\&amp;/g; s/</\&lt;/g; s/>/\&gt;/g'; }
stop_play(){
  [ -p /tmp/ub820play.ctl ] && { echo stop > /tmp/ub820play.ctl & $BB sleep 1; }
  for n in 1 2 3 4 5 6 7 8; do $BB pidof ub820play >/dev/null || break; $BB sleep 1; done
  for p in $($BB pgrep -f 'ub820run.sh'); do $BB kill "$p" 2>/dev/null; done
  $BB pidof ffmpeg >/dev/null && $BB killall ffmpeg 2>/dev/null
  if $BB grep -q 'route=4k' /tmp/ub820play.status 2>/dev/null && [ -x $BIN/homerestore ]; then
    $BB sleep 2; LD_LIBRARY_PATH=/usr_root/lib:/lib:/usr/lib:/usr/X11R6/lib:/usr/dtvrec/lib:/usr/local/lib $BIN/homerestore >> /tmp/homerestore.log 2>&1
    $BB sed -i 's/route=4k/route=4k-restored/' /tmp/ub820play.status; fi; }
printf 'Content-Type: text/html\r\n\r\n'
printf '<!doctype html><html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">'
printf '<title>UB820 Media</title><style>body{font:16px system-ui,sans-serif;margin:1em;max-width:60em}a{color:#06c}'
printf 'li{margin:.4em 0}.st{background:#eef;padding:.5em;border-radius:6px;font-family:monospace;font-size:13px}'
printf '.b{display:inline-block;padding:.3em .8em;background:#06c;color:#fff;border-radius:4px;text-decoration:none}</style></head><body>'
printf '<h2>UB820 Media</h2>'
if [ "$STOP" -gt 0 ]; then stop_play; printf '<p>Stopped.</p>'; fi
if [ -n "$SW" ] && [ -p /tmp/ub820play.ctl ]; then
  L=$($BB sed -n "$((SW+1))p" /tmp/ub820run.tracks 2>/dev/null)
  if [ -n "$L" ]; then set -- $L; echo "audio $2 $3" > /tmp/ub820play.ctl &
    printf '<p>Switching audio to track %s (%s)</p>' "$1" "$(esc "$4")"; fi
fi
if [ -n "$PAUSE" ] && [ -p /tmp/ub820play.ctl ]; then
  [ "$PAUSE" = 1 ] && echo pause > /tmp/ub820play.ctl & [ "$PAUSE" = 0 ] && echo resume > /tmp/ub820play.ctl &
  printf '<p>%s</p>' "$([ "$PAUSE" = 1 ] && echo Paused || echo Resumed)"
fi
if [ -n "$SEEK" ] && [ -f /tmp/ub820run.cur ]; then
  CF=$($BB sed -n 1p /tmp/ub820run.cur); CA=$($BB sed -n 2p /tmp/ub820run.cur)
  B=$($BB cat /tmp/ub820run.base 2>/dev/null); POS=$($BB sed -n 's/.* pos=\([0-9.]*\).*/\1/p' /tmp/ub820play.status)
  NP=$($BB awk -v b="${B:-0}" -v p="${POS:-0}" -v d="$SEEK" 'BEGIN{x=b+p+d; if(x<0)x=0; printf "%.1f", x}')
  stop_play
  $BB setsid $BB nohup $BB sh $BIN/ub820run.sh "$CF" "${CA:-0}" "$NP" auto > /tmp/ub820run.log 2>&1 < /dev/null &
  printf '<p>Seeking to %ss</p>' "$NP"
fi
if [ -n "$P" ]; then
  F=$(file_n "$P")
  if [ -n "$F" ] && [ -f "$MNT/$F" ]; then
    stop_play
    $BB setsid $BB nohup $BB sh $BIN/ub820run.sh "$MNT/$F" "${A:-0}" "" auto > /tmp/ub820run.log 2>&1 < /dev/null &
    printf '<p>Playing <b>%s</b> (audio track %s)</p>' "$(esc "$F")" "${A:-0}"
  else printf '<p>No such file.</p>'; fi
fi
if [ -n "$I" ]; then
  F=$(file_n "$I")
  if [ -n "$F" ]; then
    printf '<h3>%s</h3><pre>' "$(esc "$F")"
    $BIN/ffmpeg -hide_banner -i "$MNT/$F" 2>&1 | $BB grep -E 'Duration|Stream #' | $BB sed 's/&/\&amp;/g; s/</\&lt;/g'
    printf '</pre><p>'
    n=0; for t in $($BIN/ffmpeg -hide_banner -i "$MNT/$F" 2>&1 | $BB grep 'Stream #.*Audio:' | $BB sed 's/ /_/g'); do
      printf '<a class="b" href="?play=%s&a=%s">Play with audio %s</a> ' "$I" "$n" "$n"; n=$((n+1)); done
    printf '<a class="b" href="?play=%s&a=none">Play without audio</a></p>' "$I"
  fi
fi
printf '<p><a class="b" href="?stop=1">Stop</a> <a class="b" href="?pause=1">Pause</a> <a class="b" href="?pause=0">Resume</a> '
printf '<a class="b" href="?seek=-300">-5m</a> <a class="b" href="?seek=-30">-30s</a> <a class="b" href="?seek=30">+30s</a> <a class="b" href="?seek=300">+5m</a> <a class="b" href="?">Refresh</a></p>'
if $BB pidof ub820play >/dev/null && [ -s /tmp/ub820run.tracks ]; then
  printf '<p>Audio: '; while read -r n pid k c lang; do
    printf '<a class="b" href="?sw=%s">%s %s%s</a> ' "$n" "$n" "$(esc "$c")" "${lang:+ ($lang)}"; done < /tmp/ub820run.tracks; printf '</p>'
fi
printf '<div class="st">'; if [ -f /tmp/ub820play.status ]; then esc "$($BB cat /tmp/ub820play.status)"; else printf 'idle'; fi
printf '<br>'; [ -f /tmp/ub820run.log ] && esc "$($BB grep 'ub820run:' /tmp/ub820run.log | $BB tail -2)"; printf '</div>'
if [ -f /tmp/ub820.lossless ]; then printf '<p>Lossless DTS/TrueHD passthrough: <b>ON</b> (experimental) <a class="b" href="?lossless=0">turn off</a></p>'
else printf '<p>Lossless DTS/TrueHD passthrough: off (DTS/TrueHD are converted to AC-3) <a class="b" href="?lossless=1">turn on (experimental)</a></p>'; fi
printf '<h3>USB</h3><ol start="0">'
i=0; $BB ls -1 $MNT 2>/dev/null | while IFS= read -r f; do
  printf '<li><a href="?play=%s">%s</a> &middot; <a href="?info=%s">tracks</a></li>' "$i" "$(esc "$f")" "$i"; i=$((i+1)); done
printf '</ol></body></html>'
