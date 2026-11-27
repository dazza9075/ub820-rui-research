#!/bin/busybox sh
# On-screen clock control. Lives in /conf/hbclock/ (survives flash). Flag /conf/hbclock/on = enabled.
#   hbclock.sh on|off|status|supervise|stop
# supervise: loop (started at boot by /conf/user-tweaks.sh via setsid nohup). Starts the clock only while
# enabled AND gserver (UI) has been up >=10s; the clock exits by itself (AVIO Close) if gserver goes away
# (standby), and supervise restarts it after wake. Never runs in Boot-Without-Patches (user-tweaks skipped).
BB=/sbin/busybox
D=/conf/hbclock
FLAG=$D/on
LOG=/tmp/hbclock.log
LIBS=/usr_root/lib:/lib:/usr/lib:/usr/X11R6/lib:/usr/dtvrec/lib:/usr/local/lib
running(){ $BB pidof hbclock >/dev/null 2>&1; }
startclk(){ LD_LIBRARY_PATH=$LIBS $BB nohup $D/hbclock 0 uk </dev/null >>$LOG 2>&1 & }
stopclk(){ $BB killall hbclock 2>/dev/null; i=0; while running && [ $i -lt 20 ]; do $BB usleep 100000; i=$((i+1)); done; }
case "$1" in
  on)  $BB touch $FLAG; echo on ;;
  off) $BB rm -f $FLAG; stopclk; echo off ;;
  stop) stopclk ;;
  status) if [ -f $FLAG ]; then echo on; else echo off; fi ;;
  supervise)
    [ -f /tmp/hbclock.sup.pid ] && $BB kill -0 "$($BB cat /tmp/hbclock.sup.pid)" 2>/dev/null && exit 0
    echo $$ > /tmp/hbclock.sup.pid
    up=0
    while :; do
      if $BB pidof gserver >/dev/null 2>&1; then up=$((up+1)); else up=0; fi
      if [ -f $FLAG ]; then
        if [ $up -ge 2 ] && ! running; then
          [ -f $LOG ] && [ "$($BB wc -c < $LOG)" -gt 20000 ] && $BB tail -c 4000 $LOG > $LOG.t && $BB mv $LOG.t $LOG
          startclk
        fi
      else
        running && stopclk
      fi
      $BB sleep 5
    done ;;
  *) echo "usage: $0 on|off|status|supervise|stop" ;;
esac
