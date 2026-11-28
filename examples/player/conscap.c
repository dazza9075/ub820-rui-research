/* conscap: temporarily redirect /dev/console output (mocserver, gserver, apl_* printf logs) into a pty and log it.
 * Usage: conscap <seconds> <outfile> [maxbytes]
 * Safety: the redirect (TIOCCONS) lives only while this process holds the pty slave. Any exit -> the kernel drops
 * the redirect. We always drain the pty master (console writers must never block on us); if the output file is
 * full or fails we keep draining and discard. Hard time limit via alarm(). Logs with write(2) only. */
#define _GNU_SOURCE
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <poll.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <sys/stat.h>

static volatile int stop;
static void on_sig(int s){ (void)s; stop = 1; }
static void we(const char* s){ write(2, s, strlen(s)); }

int main(int argc, char** argv){
    if(argc < 3){ we("usage: conscap <seconds> <outfile> [maxbytes]\n"); return 1; }
    int secs = atoi(argv[1]); if(secs < 1 || secs > 600) secs = 120;
    long maxb = argc > 3 ? atol(argv[3]) : 4L*1024*1024;
    int out = open(argv[2], O_WRONLY|O_CREAT|O_TRUNC|O_NONBLOCK, 0644);
    if(out < 0){ we("open out fail\n"); return 2; }
    int m = open("/dev/ptmx", O_RDWR|O_NOCTTY);
    if(m < 0){ we("open /dev/ptmx fail\n"); return 3; }
    if(grantpt(m) || unlockpt(m)){ we("grant/unlock fail\n"); return 4; }
    char* sn = ptsname(m); if(!sn){ we("ptsname fail\n"); return 5; }
    int s = open(sn, O_RDWR|O_NOCTTY);
    if(s < 0){ we("open slave fail\n"); return 6; }
    struct termios t; if(tcgetattr(s, &t) == 0){ cfmakeraw(&t); tcsetattr(s, TCSANOW, &t); }   /* no echo / no \r games */
    signal(SIGALRM, on_sig); signal(SIGTERM, on_sig); signal(SIGINT, on_sig); signal(SIGHUP, on_sig); signal(SIGPIPE, SIG_IGN);
    fcntl(m, F_SETFL, O_NONBLOCK);
    if(ioctl(s, TIOCCONS, 0) < 0){ we("TIOCCONS fail\n"); return 7; }
    we("conscap: console redirected\n");
    alarm(secs);
    long total = 0; char buf[16384];
    while(!stop){
        struct pollfd p = { m, POLLIN, 0 };
        int r = poll(&p, 1, 200);
        if(r < 0) continue;                       /* EINTR from alarm -> loop checks stop */
        if(r == 0) continue;
        for(;;){
            ssize_t n = read(m, buf, sizeof buf);
            if(n <= 0) break;
            if(total < maxb){ ssize_t w = write(out, buf, (size_t)n); if(w > 0) total += w; }
        }
    }
    /* undo: closing the slave drops the redirect; drain once more first so no writer is mid-block */
    { ssize_t n; while((n = read(m, buf, sizeof buf)) > 0){ if(total < maxb){ ssize_t w = write(out, buf, (size_t)n); if(w > 0) total += w; } } }
    /* explicit undo: TIOCCONS on a /dev/console fd clears the redirect (CAP_SYS_ADMIN). Backstop: closing the
       master hangs up the slave, and __tty_hangup drops a redirect that points at it (also covers SIGKILL). */
    { int c = open("/dev/console", O_WRONLY|O_NOCTTY|O_NONBLOCK);
      if(c >= 0){ if(ioctl(c, TIOCCONS, 0) < 0) we("undo TIOCCONS fail (hangup backstop applies)\n"); close(c); } }
    close(m); close(s); close(out);
    we("conscap: released\n");
    return 0;
}
