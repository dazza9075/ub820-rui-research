#!/bin/sh
# Network Services redirection toggle (persistent). Lives at /conf/vc/ns-redirect.sh.
# Redirects the VieraConnect initurl/localurl/errorurl to our local /conf/vc/home-screen.js by
# bind-mounting an override over the (read-only cramfs) default_url_eu.vieraconnect. A flag file in
# /conf makes it survive reboot: v16 init calls `ns-redirect.sh apply` at boot and re-creates the mount.
#   on     -> redirect to custom page + set flag (persists)
#   off    -> revert to stock + clear flag
#   apply  -> (boot) if flag set, re-create the mount
#   status -> print current state
BB=/sbin/busybox
VC=/usr2/dtvrec/data/vc/default_url_eu.vieraconnect
OV=/conf/vc/default_url_eu.override
JS=/conf/vc/home-screen.js
FLAG=/conf/vc/ns_redirect.flag

unmount_all() { while $BB mount | $BB grep -q " $VC "; do $BB umount "$VC" 2>/dev/null || break; done; }

mk_override() {   # VC must be unmounted (stock) before calling
  $BB cat "$VC" > "$OV"
  $BB sed -i \
    -e 's#^initurl .*#initurl file://'"$JS"'#' \
    -e 's#^localurl .*#localurl file://'"$JS"'#' \
    -e 's#^errorurl .*#errorurl file://'"$JS"'#' "$OV"
}

mount_override() { unmount_all; mk_override; $BB mount -o bind "$OV" "$VC"; }

case "$1" in
  on)     [ -f "$JS" ] || { echo "ERROR: $JS missing"; exit 1; }; mount_override; $BB touch "$FLAG"; echo "on (custom)" ;;
  off)    unmount_all; $BB rm -f "$FLAG"; echo "off (stock)" ;;
  apply)  if [ -f "$FLAG" ] && [ -f "$JS" ]; then mount_override; echo "applied (custom)"; else echo "flag off -> stock"; fi ;;
  status) if $BB mount | $BB grep -q " $VC "; then m=custom; else m=stock; fi
          [ -f "$FLAG" ] && f=on || f=off
          echo "mount=$m flag=$f"; $BB grep '^initurl' "$VC" ;;
  *) echo "usage: ns-redirect.sh {on|off|apply|status}"; exit 1 ;;
esac
