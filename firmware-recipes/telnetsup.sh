#!/bin/sh
# ensure unix98 ptys exist (telnetd needs /dev/pts) then supervise a telnet root shell
/bin/busybox mkdir -p /dev/pts 2>/dev/null
/bin/busybox mount -t devpts devpts /dev/pts 2>/dev/null
while :; do
  /bin/busybox ifconfig 2>/dev/null | /bin/busybox grep -q "inet addr" && /bin/busybox telnetd -F -p 23 -l /bin/sh
  /bin/busybox sleep 5
done
