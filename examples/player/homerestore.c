/* homerestore - bring the ACE home screen back after a 4KOTT (play mode 8 / SW_VIDEODL) session.
 *
 * What it does: sends apl_acemgr the same "go home" event that apl_mgr (AM) sends it after a
 * netlauncher/4KOTT app has ended:  EV message  c:59 (0x3b, event)  f:42004 (0xa414)  payload[0]=11.
 * apl_acemgr's 42004 handler (0x18c14) maps payload 11 -> ACE app type 7 (portal home_stage) and runs
 * Start(7): restart check ("restart for mode diff") -> Stop -> Run -> apl_pre_setting_home (playmode 9,
 * OSD preset) -> AMP_StartApp(7) -> "Start url ...home_stage/stage.js" -> ADS ONPLAY -> ACEMGR Play ->
 * c:62 f:67100 to ADS ("creat resoure for 50060"). That is the step missing after our teardown.
 * Full derivation: ~/UB820-cfw/native-access/findings/home-restore.md
 *
 * v2 (2026-11-28, after live test: 42004 alone -> StartApp:7 + PORTAL ON_START, but no "[ACEMGR] Play",
 * no LOSD ON, no PORTAL ON_PLAY -> gserver buffers stay alpha 0):
 *   In the stock flow the Play step is driven by ADS, not AM: AM also sends ADS "c:54 f:21010" (home screen
 *   request); ADS's state machine then calls ACEMGR_PlayerExecFunc(APL_ACEMGR_PFUNC_ONPLAY=5, NULL)
 *   (apl_dispsrv 0x6e0bc) -> acemgr 1501 handler 0x183ec -> player cmd 5 Play (state 3 -> 4) -> LOSD ON,
 *   ON_PLAY, portal draws. We never sent 21010, so ADS never called ONPLAY.
 *   Step 2 (default): call that same public client API ourselves: libacemgr.so ACEMGR_PlayerExecFunc(5, NULL).
 *   It makes its own auto-id reply queue, sends c:51 f:1501 {replyq, 5, 1} to 0x90c38c00 (sender 50101),
 *   select()s up to 30 s for the c:52 reply, destroys the queue, returns 0 on success.
 *   Optional --ads21010: also send ADS the AM request c:54 f:21010 (exact AM 0x2d1e8 layout). ADS then does
 *   its own ONPLAY + ACEMGR_NotifyEvent(42010 start-menu info) + state update, but replies c:55 to AM's
 *   queue, which AM did not ask for (see doc, risk 8).
 *
 * Wire format (libev.so EV_send_message(qid, len, data, sender) -> ioctl(/dev/dtvrec/evk, 0x8909)):
 *   qid    = 0x90000000 | (50060 << 8) = 0x90c38c00   (apl_acemgr's queue, EV_create_queue @acemgr 0x140e2)
 *   sender = 50001 (0xc351, AM)                        (arrives as msg+4; 42004 handler does not check it)
 *   len    = 0x24, data = u32[9] { tg=50001, ty=1, fs=0, k=1, c=59, f=42004, r=0, s=0, payload0=11 }
 *   (identical to apl_mgr 0x2e450 builder + 0x31da4 sender; see the six sites 0x47ca4/0x48a34/... )
 *
 * Safety gates (refuses unless --force):
 *   - ACE memory pool must be attached: ioctl(/dev/fb6, 0x800446a5) == 1. If it is detached, our video
 *     path is still up (4KOTT); starting home would run apl_pre_setting_home -> playmode 9 + decoder
 *     select NONE under our playback, and AjaxCeplay would block waiting for graphics memory.
 *   - apl_acemgr must be running; no cobalt/netflix process may be running (AM is not in its home state then).
 *   - Idempotent when home is already up: Start(7) with current app 7 -> "ace ignore", no state change;
 *     ONPLAY in state 4 (playing) -> table entry (1,-1) "ace ignore".
 *
 * Build: zig cc -target arm-linux-gnueabihf.2.20 homerestore.c -o homerestore -ldl
 * Run:   export LD_LIBRARY_PATH=/usr/dtvrec/lib:/usr/lib:/lib ; /tmp/homerestore [opts]
 *   --delay MS     wait before sending (default 1500; lets AM's 20039 -> "[ACEMGR] Run" finish)
 *   --wait S       max seconds to wait for the pool to attach (default 3)
 *   --payload N    42004 payload (default 11 = normal home; 60/66/67 = home variants, see doc)
 *   --raw          bypass libev, do the 0x8909 ioctl directly (same 0x810-byte struct libev builds)
 *   --dry-run      print the message, send nothing
 *   --force        skip the safety gates
 *   --play-delay MS  wait between 42004 and ONPLAY (default 1500; acemgr serialises anyway)
 *   --no-play      v1 behaviour (42004 only)
 *   --play-only    skip 42004, only ONPLAY (if StartApp:7 already happened)
 *   --ads21010     additionally send ADS c:54 f:21010 as AM, before ONPLAY
 * Exit: 0 sent, 1 usage, 2 gate refused, 3 libev/ioctl/ONPLAY error.
 */
#include <dlfcn.h>
#include <dirent.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#define FBIO_CHECK_MEM_POOL_ATTACH 0x800446a5u
#define EVK_SEND_MESSAGE           0x8909u
#define MID_AM      50001u   /* apl_mgr   0xc351 */
#define MID_ACEMGR  50060u   /* apl_acemgr 0xc38c */
#define EV_QID(mid) (0x90000000u | ((uint32_t)(mid) << 8))
#define APL_C_EVENT 59u      /* c:59 = 0x3b notify event (no reply) */
#define APL_F_ACE_CHG 42004u /* 0xa414 */

static void w(const char* s){ write(2, s, strlen(s)); }
static void wu(unsigned long v, int hex){
    char b[24]; int i = 23; b[i] = 0;
    if(!v) b[--i] = '0';
    while(v){ unsigned d = v % (hex ? 16 : 10); b[--i] = d < 10 ? '0' + d : 'a' + d - 10; v /= hex ? 16 : 10; }
    if(hex){ b[--i] = 'x'; b[--i] = '0'; }
    w(b + i);
}
static void wi(long v){ if(v < 0){ w("-"); v = -v; } wu((unsigned long)v, 0); }
static void msleep(int ms){ struct timespec t = { ms / 1000, (ms % 1000) * 1000000L }; nanosleep(&t, 0); }

static int pool_attached(void){
    int fd = open("/dev/fb6", O_RDWR); if(fd < 0) return -1;
    uint32_t v = 0xffffffffu; int r = ioctl(fd, FBIO_CHECK_MEM_POOL_ATTACH, &v); close(fd);
    return r < 0 ? -1 : (int)v;
}

/* 1 if any /proc/<pid>/comm contains needle */
static int proc_running(const char* needle){
    DIR* d = opendir("/proc"); if(!d) return -1;
    struct dirent* e; int found = 0;
    while(!found && (e = readdir(d))){
        if(e->d_name[0] < '0' || e->d_name[0] > '9') continue;
        char p[64] = "/proc/"; strncat(p, e->d_name, 20); strcat(p, "/comm");
        int fd = open(p, O_RDONLY); if(fd < 0) continue;
        char c[32] = {0}; read(fd, c, sizeof c - 1); close(fd);
        if(strstr(c, needle)) found = 1;
    }
    closedir(d); return found;
}

#define MID_ADS     50101u   /* apl_dispsrv 0xc3b5 */
#define APL_C_REQ_ADS 54u    /* c:54 = 0x36 screen request to ADS (reply c:55) */
#define APL_F_HOME_SCR 21010u /* 0x5212 */
#define ACEMGR_PFUNC_ONPLAY 5

typedef int (*acemgr_exec_t)(int func, int* arg);

/* libacemgr.so ACEMGR_PlayerExecFunc (0x2d160c44): the API apl_dispsrv uses. Blocks <= 30 s for the reply. */
static int ace_exec(int func){
    { const char* pre[] = {"libdbg_output.so","libev.so","librpc.so","libapl_envlib.so"};       /* libacemgr deps not in its DT_NEEDED */
      for(unsigned k=0;k<sizeof pre/sizeof pre[0];k++) dlopen(pre[k], RTLD_NOW|RTLD_GLOBAL); }
    void* h = dlopen("libacemgr.so", RTLD_NOW);
    if(!h) h = dlopen("/usr/dtvrec/lib/libacemgr.so", RTLD_NOW);
    if(!h){ w("dlopen libacemgr.so FAIL: "); w(dlerror()); w("\n"); return -110; }
    acemgr_exec_t f = (acemgr_exec_t)dlsym(h, "ACEMGR_PlayerExecFunc");
    if(!f){ w("dlsym ACEMGR_PlayerExecFunc FAIL\n"); return -111; }
    return f(func, 0);   /* arg NULL -> acemgr uses 1 (0x183f8) */
}

typedef int (*ev_send_t)(uint32_t qid, uint32_t len, const void* data, uint32_t sender);

static int send_libev(uint32_t qid, const uint32_t* data, uint32_t len, uint32_t sender){
    void* h = dlopen("libev.so", RTLD_NOW);
    if(!h) h = dlopen("/usr/dtvrec/lib/libev.so", RTLD_NOW);
    if(!h){ w("dlopen libev.so FAIL: "); w(dlerror()); w("\n"); return -100; }
    ev_send_t f = (ev_send_t)dlsym(h, "EV_send_message");
    if(!f){ w("dlsym EV_send_message FAIL\n"); return -101; }
    return f(qid, len, data, sender);   /* 0 ok, 2 bad/no such queue, 7 ioctl error */
}

static int send_raw(uint32_t qid, const uint32_t* data, uint32_t len, uint32_t sender){
    /* libev EV_send_message @0x281e2248: {qid, sender, len, (unused), data[0x800]} -> ioctl 0x8909 */
    static uint32_t buf[0x810 / 4];
    memset(buf, 0, sizeof buf);
    buf[0] = qid; buf[1] = sender; buf[2] = len;
    memcpy(&buf[4], data, len);
    int fd = open("/dev/dtvrec/evk", O_RDWR); if(fd < 0){ w("open /dev/dtvrec/evk FAIL\n"); return -102; }
    int r = ioctl(fd, EVK_SEND_MESSAGE, buf); close(fd);
    return r;
}

int main(int argc, char** argv){
    int delay = 1500, waits = 3, raw = 0, dry = 0, force = 0; uint32_t payload = 11;
    int play_delay = 1500, do_chg = 1, do_play = 1, ads21010 = 0;
    for(int i = 1; i < argc; i++){
        if(!strcmp(argv[i], "--delay") && i + 1 < argc) delay = atoi(argv[++i]);
        else if(!strcmp(argv[i], "--wait") && i + 1 < argc) waits = atoi(argv[++i]);
        else if(!strcmp(argv[i], "--payload") && i + 1 < argc) payload = (uint32_t)strtoul(argv[++i], 0, 0);
        else if(!strcmp(argv[i], "--raw")) raw = 1;
        else if(!strcmp(argv[i], "--dry-run")) dry = 1;
        else if(!strcmp(argv[i], "--force")) force = 1;
        else if(!strcmp(argv[i], "--play-delay") && i + 1 < argc) play_delay = atoi(argv[++i]);
        else if(!strcmp(argv[i], "--no-play")) do_play = 0;
        else if(!strcmp(argv[i], "--play-only")) do_chg = 0;
        else if(!strcmp(argv[i], "--ads21010")) ads21010 = 1;
        else { w("usage: homerestore [--delay ms] [--wait s] [--payload n] [--raw] [--dry-run] [--force]\n"
                 "                   [--play-delay ms] [--no-play] [--play-only] [--ads21010]\n"); return 1; }
    }

    /* gates */
    int pool = pool_attached();
    for(int t = 0; pool != 1 && t < waits * 10; t++){ msleep(100); pool = pool_attached(); }
    w("ACE mem pool: "); wi(pool); w(pool == 1 ? " attached\n" : " NOT attached\n");
    int acemgr = proc_running("apl_acemgr");
    int ott = proc_running("cobalt") == 1;   /* apl_netflix is resident (hidden) on this box: not an OTT owner */
    w("apl_acemgr running: "); wi(acemgr); w("  cobalt/netflix running: "); wi(ott); w("\n");
    if(!force){
        if(pool != 1){ w("REFUSED: pool detached -> our 4KOTT path is still up; tear it down (playmode 9) first\n"); return 2; }
        if(acemgr != 1){ w("REFUSED: apl_acemgr not running\n"); return 2; }
        if(ott){ w("REFUSED: an OTT app owns the screen; AM will restore home itself when it exits\n"); return 2; }
    }

    uint32_t data[9] = { MID_AM, 1, 0, 1, APL_C_EVENT, APL_F_ACE_CHG, 0, 0, payload };
    uint32_t qid = EV_QID(MID_ACEMGR), len = sizeof data;
    w("msg -> qid "); wu(qid, 1); w(" sender "); wi(MID_AM); w(" len "); wi(len); w(" data:");
    for(int i = 0; i < 9; i++){ w(" "); wi(data[i]); } w("\n");
    /* AM's ADS request (apl_mgr 0x2d1e8 + 0x31da4): tg=0 ty=1 fs=0 k=1 c=54 f=21010 r=0 s=0 payload0=0 */
    uint32_t ads[9] = { 0, 1, 0, 1, APL_C_REQ_ADS, APL_F_HOME_SCR, 0, 0, 0 };
    if(ads21010){ w("ads  -> qid "); wu(EV_QID(MID_ADS), 1); w(" sender 50001 c:54 f:21010 len 36\n"); }
    w("steps: "); w(do_chg ? "42004 " : ""); w(ads21010 ? "ads21010 " : ""); w(do_play ? "ONPLAY" : ""); w("\n");
    if(dry){ w("dry-run: not sent\n"); return 0; }

    int r;
    if(do_chg){
        if(delay > 0) msleep(delay);
        r = raw ? send_raw(qid, data, len, MID_AM) : send_libev(qid, data, len, MID_AM);
        w(raw ? "42004: ioctl 0x8909 = " : "42004: EV_send_message = "); wi(r); w("\n");
        if(r != 0) return 3;
        w("  expect: [ACEMGR] 50001> c:59 f:42004 ... [AMP] StartApp:7 ... Start url ...home_stage\n");
    }
    if(ads21010){
        r = raw ? send_raw(EV_QID(MID_ADS), ads, sizeof ads, MID_AM) : send_libev(EV_QID(MID_ADS), ads, sizeof ads, MID_AM);
        w("21010: send = "); wi(r); w("\n");
        if(r != 0) return 3;
        w("  expect: [ADS] <50001 c:54f:21010 ... [ADS] call ACEMGR_PlayerExecFunc(...ONPLAY)\n");
    }
    if(do_play){
        if(play_delay > 0) msleep(play_delay);
        for(int t = 0; t < 8; t++){             /* the home app may still be starting after 42004: retry */
            r = ace_exec(ACEMGR_PFUNC_ONPLAY);
            w("ONPLAY: ACEMGR_PlayerExecFunc(5) = "); wi(r); w(r == 0 ? " ok\n" : " (retry)\n");
            if(r == 0) break;
            msleep(1000);
        }
        if(r != 0) return 3;
        w("  expect: [ACEMGR] 50101> c:51 f:1501 / [ACEMGR] Play / LOSD ON / [PORTAL] ON_PLAY\n");
    }
    return 0;
}
