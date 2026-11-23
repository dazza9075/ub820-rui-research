# --- Lines we ADD to the stock /target/startrc (sash). Not the whole file ---
# (The stock startrc is Panasonic's; only these additions are ours.)

# 1) Load the AES key-capture shim into every process (comm-gated to AjaxCeplay inside the .so):
setenv LD_PRELOAD /lib/libjshook.so

# 2) Just before the final `exec /usr/dtvrec/bin/apl_mgr -d`, start a supervised telnet root shell
#    (detached, network-gated, self-respawning). See telnetsup.sh.
/bin/busybox setsid /bin/busybox sh /usr/target/telnetsup.sh >/dev/null 2>&1 &
