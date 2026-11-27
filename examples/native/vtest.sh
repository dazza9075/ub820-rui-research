#!/bin/busybox sh
# Bounded live video test. Usage: vtest.sh <clip> <h264|hevc> <w> <h> <fps>
BB=/sbin/busybox
export LD_LIBRARY_PATH=/usr_root/lib:/lib:/usr/lib:/usr/X11R6/lib:/usr/dtvrec/lib:/usr/local/lib
LOG=/tmp/vtest.log; : > $LOG
had_srv=0; $BB pidof vcavsrv >/dev/null && had_srv=1
echo "had_vcavsrv=$had_srv" >> $LOG
/tmp/flipwin >> $LOG 2>&1
/tmp/player_skeleton "$@" >> $LOG 2>&1 &
P=$!
i=0; while $BB kill -0 $P 2>/dev/null && [ $i -lt 40 ]; do $BB sleep 1; i=$((i+1)); done
if $BB kill -0 $P 2>/dev/null; then echo "TIMEOUT: killing player" >> $LOG; $BB kill $P; $BB sleep 1; $BB kill -9 $P 2>/dev/null; fi
wait $P 2>/dev/null; echo "player exit=$?" >> $LOG
if [ $had_srv = 0 ]; then $BB killall vcavsrv 2>/dev/null && echo "killed spawned vcavsrv" >> $LOG; fi
echo "vtest done (plane 7 still on window; press a key or send LEFT/RIGHT to restore)" >> $LOG
