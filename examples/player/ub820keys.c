/* ub820keys - let the physical remote (IR, and also CEC / LAN "dvdr_ctrl" keys) control ub820play.
 *
 * Why this is needed: our playback uses the 4KOTT route, which Finals the home UI (AjaxCeplay). apl_mgr (AM)
 * still believes home is up, so it routes nav keys to apl_dispsrv (which rejects them), PLAY/STOP to apl_medm
 * (which answers NG) and silently drops PAUSE/AUDIO/SKIP. No queue receives all keys, and none of them is ours.
 *
 * How it works (findings/remote-keys.md): every key, whatever its source (IR via /dev/dtvrec/rmcd, CEC,
 * LAN cCMD_RC_* via net_mgr 50301), ends up in AM's common dispatcher FUN_0001a564 -> router FUN_00034e30.
 * At apl_mgr 0x34e54 (Thumb, right after "ldr.w r1,[r0,#0xe0]") r0 = AM's control block and r1 = event type
 * (1 press, 2 release, 3 repeat, 4 long press); the key code is at [r0+0xe4]. We put a perf_event HARDWARE
 * breakpoint there on each apl_mgr thread (PERF_TYPE_BREAKPOINT, sample regs r0/r1): AM's code and memory are
 * never modified, AM keeps doing what it always does, and the breakpoint disappears with our fds (any exit,
 * even SIGKILL). The code at 0x34e50 is checked first; on any other apl_mgr build we refuse to run.
 *
 * Actions (press events only):
 *   PAUSE, PLAY, OK/SELECT       -> pause toggle ("pause"/"resume" on /tmp/ub820play.ctl)
 *   PAUSE_ON / PAUSE_OFF codes   -> pause / resume
 *   STOP, RETURN, EXIT, HOME     -> "stop" (ub820run.sh then runs homerestore = home screen back)
 *   RIGHT / LEFT                 -> seek +30 / -30 s
 *   SKIP>>| , FF(CUE), SEARCH>>  -> seek +5 min;  |<<SKIP, REW, SEARCH<< -> seek -5 min
 *   AUDIO                        -> next audio track from /tmp/ub820run.tracks ("audio <pid> <codec>")
 *   Seeks are debounced (presses within --debounce ms add up), then run detached exactly like the web remote:
 *   QUERY_STRING=seek=<delta> /tmp/mediaweb/cgi-bin/media (stops this session, restarts ub820run.sh there).
 *
 * Usage: ub820keys [--learn] [--log FILE] [--seek S] [--bigseek S] [--debounce MS] [--no-ppid-watch]
 *   --learn          log key codes only, take no action
 *   --no-ppid-watch  keep running when the parent (ub820run.sh) dies (default: exit then)
 * Single instance (flock /tmp/ub820keys.lock). Exits on SIGTERM/INT/HUP, when apl_mgr disappears or when
 * the parent dies. Logs with write(2) to stderr. Exit: 0 ok, 1 usage, 2 already running, 3 cannot attach.
 * Build: zig cc -target arm-linux-gnueabihf.2.20 -O2 -s ub820keys.c -o ub820keys
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <linux/perf_event.h>
#include <linux/hw_breakpoint.h>

#define AM_BP_ADDR   0x34e54u                 /* subs r4,r1,#3 : r0 = ctrl, r1 = event type */
#define AM_CHK_ADDR  0x34e50u
static const unsigned char AM_CHK[8] = { 0xd0, 0xf8, 0xe0, 0x10, 0xcc, 0x1e, 0x01, 0x29 }; /* ldr.w r1,[r0,#0xe0]; subs r4,r1,#3; cmp r1,#1 */
#define OFF_KEY      0xe4u
#define NPG          2                        /* ring buffer data pages per event */
#define MAXEV        32

#define CTL     "/tmp/ub820play.ctl"
#define STATUS  "/tmp/ub820play.status"
#define TRACKS  "/tmp/ub820run.tracks"
#define CUR     "/tmp/ub820run.cur"
#define MEDIA   "/tmp/mediaweb/cgi-bin/media"

/* ---------------------------------------------------------------- logging (write(2) only) */
static void w(const char* s){ write(2, s, strlen(s)); }
static void wd(long v){ char b[24]; int i = 23, n = v < 0; b[i] = 0; if(n) v = -v; do b[--i] = '0' + v % 10; while(v /= 10); if(n) b[--i] = '-'; w(b + i); }
static void wx(unsigned long v){ char b[20]; int i = 19; b[i] = 0; do { unsigned d = v & 15; b[--i] = d < 10 ? '0' + d : 'a' + d - 10; } while(v >>= 4); b[--i] = 'x'; b[--i] = '0'; w(b + i); }
static void wts(void){
    struct timespec t; clock_gettime(CLOCK_REALTIME, &t); long s = t.tv_sec % 86400;
    char b[16]; b[0] = '0' + s / 36000; b[1] = '0' + s / 3600 % 10; b[2] = ':'; b[3] = '0' + s % 3600 / 600; b[4] = '0' + s / 60 % 10;
    b[5] = ':'; b[6] = '0' + s % 60 / 10; b[7] = '0' + s % 10; b[8] = ' '; b[9] = 0; w(b); w("ub820keys: ");
}
static long now_ms(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1000L + t.tv_nsec / 1000000; }

/* ---------------------------------------------------------------- key table (AM key codes, verified by LAN injection) */
enum { A_NONE, A_TOGGLE, A_PAUSE, A_RESUME, A_STOP, A_SEEK_S, A_SEEK_B, A_AUDIO };
struct key { unsigned code; const char* name; int act; int dir; };
static const struct key KEYS[] = {
    { 0x06,    "PAUSE",       A_TOGGLE, 0 }, { 0x0a, "PLAY", A_TOGGLE, 0 }, { 0x82, "OK", A_TOGGLE, 0 },
    { 0x80030, "PAUSE_ON",    A_PAUSE,  0 }, { 0x80031, "PAUSE_OFF", A_RESUME, 0 },
    { 0x00,    "STOP",        A_STOP,   0 }, { 0x81, "RETURN", A_STOP, 0 }, { 0x151, "EXIT", A_STOP, 0 },
    { 0x57,    "HOME",        A_STOP,   0 }, { 0x144, "HOME2", A_STOP, 0 },
    { 0x88,    "RIGHT",       A_SEEK_S, 1 }, { 0x87, "LEFT", A_SEEK_S, -1 },
    { 0x4a,    "SKIP_NEXT",   A_SEEK_B, 1 }, { 0x49, "SKIP_PREV", A_SEEK_B, -1 },
    { 0x8000a, "SKIP_NEXT_2", A_SEEK_B, 1 }, { 0x8000d, "SKIP_NEXT_3", A_SEEK_B, 1 },
    { 0x8000b, "SKIP_PREV_2", A_SEEK_B, -1 }, { 0x8000c, "SKIP_PREV_3", A_SEEK_B, -1 },
    { 0x05,    "FF",          A_SEEK_B, 1 }, { 0x04, "REW", A_SEEK_B, -1 },
    { 0x80026, "SEARCH_FWD1", A_SEEK_B, 1 }, { 0x80027, "SEARCH_FWD2", A_SEEK_B, 1 }, { 0x80028, "SEARCH_FWD3", A_SEEK_B, 1 },
    { 0x80029, "SEARCH_FWD4", A_SEEK_B, 1 }, { 0x8002a, "SEARCH_FWD5", A_SEEK_B, 1 },
    { 0x8001c, "SEARCH_REV1", A_SEEK_B, -1 }, { 0x8001d, "SEARCH_REV2", A_SEEK_B, -1 }, { 0x8001e, "SEARCH_REV3", A_SEEK_B, -1 },
    { 0x8001f, "SEARCH_REV4", A_SEEK_B, -1 }, { 0x80020, "SEARCH_REV5", A_SEEK_B, -1 },
    { 0x33,    "AUDIO",       A_AUDIO,  0 },
    /* logged only */
    { 0x85, "UP", A_NONE, 0 }, { 0x86, "DOWN", A_NONE, 0 }, { 0x80, "MENU", A_NONE, 0 }, { 0x9b, "TITLE", A_NONE, 0 },
    { 0x150, "POPUP_MENU", A_NONE, 0 }, { 0x125, "OSD", A_NONE, 0 }, { 0x140, "BLUE?", A_NONE, 0 }, { 0x141, "RED", A_NONE, 0 },
    { 0x142, "GREEN?", A_NONE, 0 }, { 0x143, "YELLOW?", A_NONE, 0 },
};
static const struct key* key_find(unsigned code){
    for(unsigned i = 0; i < sizeof KEYS / sizeof KEYS[0]; i++) if(KEYS[i].code == code) return &KEYS[i];
    return 0;
}

/* ---------------------------------------------------------------- options / state */
static int o_learn, o_ppid_watch = 1, o_seek = 30, o_bigseek = 300, o_debounce = 1000;
static volatile sig_atomic_t g_stop;
static void on_sig(int s){ (void)s; g_stop = 1; }

static int am_pid = -1, mem_fd = -1;
struct ev { int fd, tid; void* base; };
static struct ev E[MAXEV]; static int NE;
static long pgsz;

static int seek_pending; static long seek_delta, seek_due_ms; static int seek_fired;
static int paused_guess;

/* ---------------------------------------------------------------- small file helpers */
static int read_file(const char* p, char* b, int n){
    int fd = open(p, O_RDONLY | O_CLOEXEC); if(fd < 0) return -1;
    int r = read(fd, b, n - 1); close(fd); if(r < 0) r = 0; b[r] = 0; return r;
}
static int comm_is(int pid, const char* name){
    char p[48] = "/proc/", b[64]; char t[16]; int i = 15; t[i] = 0; int v = pid; do t[--i] = '0' + v % 10; while(v /= 10);
    strcat(p, t + i); strcat(p, "/comm");
    if(read_file(p, b, sizeof b) <= 0) return 0;
    char* nl = strchr(b, '\n'); if(nl) *nl = 0;
    return !strcmp(b, name);
}
static int find_pid(const char* name){
    DIR* d = opendir("/proc"); if(!d) return -1;
    struct dirent* e; int found = -1;
    while((e = readdir(d))){ if(e->d_name[0] < '1' || e->d_name[0] > '9') continue;
        int p = atoi(e->d_name); if(comm_is(p, name)){ found = p; break; } }
    closedir(d); return found;
}

/* ---------------------------------------------------------------- ub820play control */
static int ctl_send(const char* line){
    struct stat st;
    if(stat(CTL, &st) != 0 || !S_ISFIFO(st.st_mode)){ wts(); w("no control FIFO (player not running?): '"); w(line); w("' dropped\n"); return -1; }
    int fd = open(CTL, O_WRONLY | O_NONBLOCK | O_CLOEXEC);
    if(fd < 0){ wts(); w("open "CTL" failed (no reader?): '"); w(line); w("' dropped\n"); return -1; }
    char b[128]; int n = strlen(line); if(n > (int)sizeof b - 2) n = sizeof b - 2;
    memcpy(b, line, n); b[n++] = '\n';
    int r = write(fd, b, n); close(fd);
    wts(); w("ctl <- "); w(line); w(r == n ? "\n" : " (short write)\n");
    return r == n ? 0 : -1;
}
/* status: "state=playing[-paused] pos=.. ... audio=0x1100/ac3 rc=-" */
static int status_get(char* st, int n){ return read_file(STATUS, st, n); }
static int is_playing(const char* st){ return strstr(st, "state=playing") != 0 || strstr(st, "state=draining") != 0; }

static void do_pause(int want){   /* want: 1 pause, 0 resume, -1 toggle */
    char st[512]; int paused = paused_guess;
    if(status_get(st, sizeof st) > 0){ if(strstr(st, "state=done")){ wts(); w("player done: ignored\n"); return; }
        paused = strstr(st, "-paused") != 0; }
    int p = want < 0 ? !paused : want;
    if(ctl_send(p ? "pause" : "resume") == 0) paused_guess = p;
}
static void do_audio(void){
    char tr[1024], st[512];
    if(read_file(TRACKS, tr, sizeof tr) <= 0){ wts(); w("no audio track list\n"); return; }
    unsigned cur = 0; if(status_get(st, sizeof st) > 0){ char* a = strstr(st, "audio="); if(a) cur = strtoul(a + 6, 0, 0); }
    /* lines: idx pid codec name lang */
    char* lines[8]; int nl = 0; char* s = tr;
    while(*s && nl < 8){ lines[nl++] = s; char* e = strchr(s, '\n'); if(!e) break; *e = 0; s = e + 1; }
    if(nl < 2){ wts(); w(nl ? "only one audio track\n" : "no audio tracks\n"); return; }
    int ci = -1;
    for(int i = 0; i < nl; i++){ char* p = strchr(lines[i], ' '); if(p && strtoul(p + 1, 0, 0) == cur){ ci = i; break; } }
    int ni = (ci + 1) % nl;
    char idx[16], pid[16], codec[16];
    { char* p = lines[ni]; int k = 0; char* f[3] = { idx, pid, codec };
      for(int j = 0; j < 3; j++){ while(*p == ' ') p++; k = 0; while(*p && *p != ' ' && k < 15) f[j][k++] = *p++; f[j][k] = 0; } }
    if(!pid[0] || !codec[0]){ wts(); w("bad track line\n"); return; }
    char cmd[64] = "audio "; strcat(cmd, pid); strcat(cmd, " "); strcat(cmd, codec);
    wts(); w("audio track "); wd(ci); w(" -> "); w(idx); w(" ("); w(lines[ni]); w(")\n");
    if(ctl_send(cmd) == 0){
        /* keep the new track across seeks: ub820run.cur line 2 = audio track index (read by media.cgi ?seek=) */
        char cu[1024]; int n = read_file(CUR, cu, sizeof cu);
        char* l1 = n > 0 ? strchr(cu, '\n') : 0; char* l2 = l1 ? strchr(l1 + 1, '\n') : 0;
        if(l1 && l2){ char out[1100]; int k = l1 + 1 - cu; memcpy(out, cu, k); out[k] = 0; strcat(out, idx); strcat(out, l2);
            char tmp[] = CUR ".k"; int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
            if(fd >= 0){ write(fd, out, strlen(out)); close(fd); rename(tmp, CUR); } }
    }
}
/* run the web remote's seek, detached: it stops this session (and so us) and restarts ub820run.sh */
static void fire_seek(void){
    char st[512]; if(status_get(st, sizeof st) <= 0 || !is_playing(st)){ wts(); w("seek dropped: not playing\n"); seek_pending = 0; return; }
    char q[32] = "QUERY_STRING=seek="; char t[16]; int i = 15; long v = seek_delta; t[i] = 0; int neg = v < 0; if(neg) v = -v;
    do t[--i] = '0' + v % 10; while(v /= 10); if(neg) t[--i] = '-'; strcat(q, t + i);
    wts(); w("seek "); w(t + i); w(" s\n");
    seek_pending = 0; seek_fired = 1;
    pid_t p = fork();
    if(p == 0){
        signal(SIGCHLD, SIG_DFL); signal(SIGPIPE, SIG_DFL);           /* ignored dispositions survive exec */
        setsid();
        int dn = open("/dev/null", O_RDWR); if(dn >= 0){ dup2(dn, 0); dup2(dn, 1); }
        char* envp[] = { q, "PATH=/sbin:/bin:/usr/sbin:/usr/bin",
            "LD_LIBRARY_PATH=/usr_root/lib:/lib:/usr/lib:/usr/X11R6/lib:/usr/dtvrec/lib:/usr/local/lib", 0 };
        if(access(MEDIA, R_OK) == 0){ char* av[] = { "/bin/busybox", "sh", MEDIA, 0 }; execve(av[0], av, envp); }
        else {  /* fallback, same steps as media.cgi ?seek= */
            char* av[] = { "/bin/busybox", "sh", "-c",
                "B=/bin/busybox; D=${QUERY_STRING#seek=}; CF=$($B sed -n 1p /tmp/ub820run.cur); CA=$($B sed -n 2p /tmp/ub820run.cur);"
                " BS=$($B cat /tmp/ub820run.base 2>/dev/null); POS=$($B sed -n 's/.* pos=\\([0-9.]*\\).*/\\1/p' /tmp/ub820play.status);"
                " NP=$($B awk -v b=\"${BS:-0}\" -v p=\"${POS:-0}\" -v d=\"$D\" 'BEGIN{x=b+p+d; if(x<0)x=0; printf \"%.1f\", x}');"
                " [ -p /tmp/ub820play.ctl ] && echo stop > /tmp/ub820play.ctl;"
                " for n in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20; do $B pgrep -f ub820run.sh >/dev/null || break; $B sleep 1; done;"
                " $B setsid $B nohup $B sh /tmp/ub820run.sh \"$CF\" \"${CA:-0}\" \"$NP\" auto > /tmp/ub820run.log 2>&1 < /dev/null &", 0 };
            execve(av[0], av, envp);
        }
        _exit(127);
    }
    if(p < 0){ wts(); w("fork failed\n"); seek_fired = 0; }
}

static void on_key(unsigned code, unsigned type){
    const struct key* k = key_find(code);
    if(type != 1) return;                                  /* press only (2 release, 3 repeat, 4 long press) */
    wts(); w("key "); wx(code); w(" "); w(k ? k->name : "?"); w(o_learn ? " (learn)\n" : "\n");
    if(o_learn || !k || k->act == A_NONE) return;
    if(seek_fired){ wts(); w("seek in progress: ignored\n"); return; }
    switch(k->act){
    case A_TOGGLE: do_pause(-1); break;
    case A_PAUSE:  do_pause(1); break;
    case A_RESUME: do_pause(0); break;
    case A_STOP:   seek_pending = 0; ctl_send("stop"); break;
    case A_AUDIO:  do_audio(); break;
    case A_SEEK_S: case A_SEEK_B:
        if(!seek_pending){ seek_pending = 1; seek_delta = 0; }
        seek_delta += k->dir * (k->act == A_SEEK_S ? o_seek : o_bigseek);
        seek_due_ms = now_ms() + o_debounce;
        wts(); w("seek pending "); wd(seek_delta); w(" s\n");
        break;
    }
}

/* ---------------------------------------------------------------- perf hardware breakpoints on apl_mgr */
static int ev_has(int tid){ for(int i = 0; i < NE; i++) if(E[i].tid == tid) return 1; return 0; }
static void ev_del(int i){ close(E[i].fd); munmap(E[i].base, (1 + NPG) * pgsz); E[i] = E[--NE]; }
static int ev_add(int tid){
    if(NE >= MAXEV) return -1;
    struct perf_event_attr a; memset(&a, 0, sizeof a);
    a.type = PERF_TYPE_BREAKPOINT; a.size = sizeof a;
    a.bp_type = HW_BREAKPOINT_X; a.bp_addr = AM_BP_ADDR; a.bp_len = 2;       /* Thumb instruction */
    a.sample_period = 1; a.wakeup_events = 1;
    a.sample_type = PERF_SAMPLE_TID | PERF_SAMPLE_REGS_USER;
    a.sample_regs_user = 0x3;                                                /* r0, r1 */
    a.exclude_kernel = 1; a.exclude_hv = 1;
    int fd = syscall(__NR_perf_event_open, &a, tid, -1, -1, 8 /* PERF_FLAG_FD_CLOEXEC */);
    if(fd < 0){ wts(); w("perf_event_open tid "); wd(tid); w(" failed: "); w(strerror(errno)); w("\n"); return -1; }
    void* b = mmap(0, (1 + NPG) * pgsz, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if(b == MAP_FAILED){ close(fd); wts(); w("perf mmap failed\n"); return -1; }
    E[NE].fd = fd; E[NE].tid = tid; E[NE].base = b; NE++;
    return 0;
}
static int ev_scan(void){          /* attach to every apl_mgr thread not yet covered; returns #events */
    char p[48] = "/proc/"; char t[16]; int i = 15; t[i] = 0; int v = am_pid; do t[--i] = '0' + v % 10; while(v /= 10);
    strcat(p, t + i); strcat(p, "/task");
    DIR* d = opendir(p); if(!d) return -1;
    struct dirent* e;
    while((e = readdir(d))){ if(e->d_name[0] < '1' || e->d_name[0] > '9') continue;
        int tid = atoi(e->d_name); if(!ev_has(tid)) ev_add(tid); }
    closedir(d); return NE;
}
static void ev_drain(int i){
    struct perf_event_mmap_page* mp = E[i].base; char* data = (char*)E[i].base + pgsz; uint64_t sz = NPG * pgsz;
    uint64_t head = mp->data_head; __sync_synchronize(); uint64_t tail = mp->data_tail;
    while(tail < head){
        struct perf_event_header h; unsigned char rec[256];
        for(unsigned k = 0; k < sizeof h; k++) ((char*)&h)[k] = data[(tail + k) % sz];
        if(h.size < sizeof h || h.size > sizeof rec){ tail = head; break; }
        for(unsigned k = 0; k < h.size; k++) rec[k] = data[(tail + k) % sz];
        if(h.type == PERF_RECORD_SAMPLE && h.size >= 8 + 8 + 8 + 16){
            uint64_t regs[3]; memcpy(regs, rec + 16, sizeof regs);           /* abi, r0, r1 */
            uint32_t ctrl = (uint32_t)regs[1], type = (uint32_t)regs[2], key = 0xffffffffu;
            if(regs[0] != 0 && pread(mem_fd, &key, 4, (off_t)ctrl + OFF_KEY) == 4) on_key(key, type);
            else { wts(); w("sample without key (abi "); wd((long)regs[0]); w(")\n"); }
        }
        tail += h.size;
    }
    __sync_synchronize(); mp->data_tail = tail;
}

static void usage(void){ w("usage: ub820keys [--learn] [--log FILE] [--seek S] [--bigseek S] [--debounce MS] [--no-ppid-watch]\n"); }

int main(int argc, char** argv){
    for(int i = 1; i < argc; i++){
        if(!strcmp(argv[i], "--learn")) o_learn = 1;
        else if(!strcmp(argv[i], "--no-ppid-watch")) o_ppid_watch = 0;
        else if(!strcmp(argv[i], "--seek") && i + 1 < argc) o_seek = atoi(argv[++i]);
        else if(!strcmp(argv[i], "--bigseek") && i + 1 < argc) o_bigseek = atoi(argv[++i]);
        else if(!strcmp(argv[i], "--debounce") && i + 1 < argc) o_debounce = atoi(argv[++i]);
        else if(!strcmp(argv[i], "--log") && i + 1 < argc){
            int fd = open(argv[++i], O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
            if(fd >= 0){ struct stat st; if(fstat(fd, &st) == 0 && st.st_size > 256 * 1024) ftruncate(fd, 0); dup2(fd, 2); close(fd); } }
        else { usage(); return 1; }
    }
    pgsz = sysconf(_SC_PAGESIZE);
    int lk = open("/tmp/ub820keys.lock", O_RDWR | O_CREAT | O_CLOEXEC, 0644);
    if(lk < 0 || flock(lk, LOCK_EX | LOCK_NB) != 0){ wts(); w("already running\n"); return 2; }
    struct sigaction sa; memset(&sa, 0, sizeof sa); sa.sa_handler = on_sig;
    sigaction(SIGTERM, &sa, 0); sigaction(SIGINT, &sa, 0); sigaction(SIGHUP, &sa, 0);
    signal(SIGPIPE, SIG_IGN); signal(SIGCHLD, SIG_IGN);                   /* seek children: no zombies */
    pid_t ppid = getppid();

    am_pid = find_pid("apl_mgr");
    if(am_pid < 0){ wts(); w("apl_mgr not running\n"); return 3; }
    { char p[48] = "/proc/"; char t[16]; int i = 15; t[i] = 0; int v = am_pid; do t[--i] = '0' + v % 10; while(v /= 10);
      strcat(p, t + i); strcat(p, "/mem"); mem_fd = open(p, O_RDONLY | O_CLOEXEC); }
    unsigned char chk[8];
    if(mem_fd < 0 || pread(mem_fd, chk, 8, AM_CHK_ADDR) != 8 || memcmp(chk, AM_CHK, 8)){
        wts(); w("apl_mgr code at 0x34e50 not as expected (other firmware?): refusing\n"); return 3; }
    if(ev_scan() <= 0){ wts(); w("could not attach any breakpoint\n"); return 3; }
    wts(); w(o_learn ? "LEARN mode, " : ""); w("watching apl_mgr pid "); wd(am_pid); w(" ("); wd(NE); w(" threads), bp "); wx(AM_BP_ADDR);
    w(", seek "); wd(o_seek); w("/"); wd(o_bigseek); w(" s\n");

    long next_scan = now_ms() + 2000;
    while(!g_stop){
        struct pollfd P[MAXEV]; for(int i = 0; i < NE; i++){ P[i].fd = E[i].fd; P[i].events = POLLIN; P[i].revents = 0; }
        int r = poll(P, NE, 200);
        if(r < 0 && errno != EINTR) break;
        for(int i = NE - 1; i >= 0 && r > 0; i--){
            if(P[i].revents & POLLIN) ev_drain(i);
            if(P[i].revents & (POLLHUP | POLLERR | POLLNVAL)) ev_del(i);   /* thread exited */
        }
        long t = now_ms();
        if(seek_pending && t >= seek_due_ms) fire_seek();
        if(t >= next_scan){
            next_scan = t + 2000;
            if(!comm_is(am_pid, "apl_mgr")){ wts(); w("apl_mgr gone: exit\n"); break; }
            if(o_ppid_watch && getppid() != ppid){ wts(); w("parent gone: exit\n"); break; }
            ev_scan();
        }
    }
    if(seek_pending && !g_stop) fire_seek();
    while(NE) ev_del(NE - 1);
    if(mem_fd >= 0) close(mem_fd);
    wts(); w("exit\n");
    return 0;
}
