#!/bin/sh
# Minimal on-device launch helper for a custom Network Services page.
# Run behind a localhost-only HTTP server (e.g. busybox httpd on 127.0.0.1) as a CGI, or adapt inline.
# The custom page calls it with native http_request(sync) -> this issues the proven launch primitives.
#   ?app=netflix | youtube | amazon
# NOTE: enable the player's "Voice Control" so its on-LAN control server (thttpd :80) runs.
BB=/sbin/busybox
app="$1"; [ -n "$app" ] || app=$($BB printf '%s' "$QUERY_STRING" | $BB sed -n 's/.*app=\([a-z]*\).*/\1/p')
# the player's own LAN IP (the control endpoint is served per-interface, not reliably on 127.0.0.1)
ip=$($BB ifconfig 2>/dev/null | $BB grep -o 'inet addr:[0-9.]*' | $BB grep -v 127.0.0.1 | $BB head -1 | $BB cut -d: -f2)
case "$app" in
  netflix) $BB wget -q -O /dev/null -U "MEI-LAN-REMOTE-CALL" \
             --post-data "cCMD_RC_NETFLIX.x=100&cCMD_RC_NETFLIX.y=100" \
             "http://$ip/WAN/dvdr/dvdr_ctrl.cgi" ;;
  youtube) $BB wget -q -O /dev/null --post-data "" --header "Content-Type: text/plain" \
             "http://127.0.0.1:60030/dial_srv/apps/YouTube" ;;
  amazon)  $BB wget -q -O /dev/null --post-data "" --header "Content-Type: text/plain" \
             "http://127.0.0.1:60030/dial_srv/apps/AmazonInstantVideo" ;;
  *) echo "unknown app"; exit 1 ;;
esac
printf 'Content-Type: application/json\r\n\r\n{"launch":"%s"}' "$app"
