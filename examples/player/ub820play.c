/* ub820play - native MPEG-TS player for the Panasonic DP-UB820 (rooted CFW), through the stock vcavsrv decode server.
 *
 *   ub820play [options] <file.ts | ->
 *
 * Build:  zig cc -target arm-linux-gnueabihf.2.20 -O2 -s ub820play.c -o ub820play -ldl
 * Docs:   ub820play.md (usage, control protocol, launcher example).
 *
 * Proven recipe this implements (~/UB820-cfw/native-access/LOG.md and findings/vcav-*.md):
 *   - stream type 0: raw 188-byte TS, plain VcAvWrite, no chunk list; TDAV demuxes the video PID itself;
 *   - big writes: every VcAvWrite becomes one kernel FeedDec transfer and a vcavsrv session only ever delivers
 *     300 of them (findings/vcav-ts-40mb.md section 5), so the player writes 4 MiB blocks (~1.26 GB per session);
 *   - MOC routing by the player (vcavsrv never routes its decoder to the monitor): 4KOTT (SW_VIDEODL 0x13 + play
 *     mode 8) for UHD, DIVX (SW_DIVX 0x12 + play mode 7) for <= 1080p; home restore (play mode 9 + OSD preset);
 *   - open order video0->video1, stream0->stream1, then audio0->audio1 (the audio OPEN reopens AVD in DVR mode);
 *   - audio (findings/vcav-audio.md, static RE, NOT yet verified live): A/V demux mode (VIDEO + AUDIO property type
 *     2, start on AUDIO PLAY), AC-3 / E-AC-3 / AAC / MPEG audio on a TS PID; --apid none = proven video-only start.
 *
 * Hard rules kept here:
 *   - no stdio at all (zig static / ld-2.20 crashed on the stdout/stderr data symbols): write(2) only;
 *   - never block forever in VcAvWrite (libvcavif retries -ENOSPC every 10 ms with no limit): the player writes
 *     only while the PCR lead is below target AND the FeedDec in-flight count is low, and a forked watchdog kills
 *     a wedged player and restores the home screen;
 *   - never kill vcavsrv: after a clean teardown it exits by itself;
 *   - no window/OSD (flipwin) handling: that is the launcher's job.
 */
#define _GNU_SOURCE
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <dlfcn.h>
#include <signal.h>
#include <time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/select.h>
#include <sys/ioctl.h>
extern int rename(const char* oldpath, const char* newpath);   /* declared by hand: no <stdio.h> in this program */

/* ======================================================================================================== */
/* exit codes                                                                                               */
/* ======================================================================================================== */
enum {
    RC_OK      = 0,   /* end of stream, or "stop" on the control FIFO */
    RC_USAGE   = 1,
    RC_INPUT   = 2,   /* input missing / unreadable / not 188-byte TS */
    RC_LIB     = 3,   /* dlopen/dlsym of libpthread / libvcavif / libmoc / librpc failed */
    RC_MOC     = 4,   /* MOC routing failed (nothing was opened on vcavsrv) */
    RC_CONNECT = 5,   /* vcavsrv not reachable */
    RC_OPEN    = 6,   /* video/stream/audio handle open failed */
    RC_PROP    = 7,   /* STREAM/VIDEO SETPROPERTY failed */
    RC_NOAVD   = 8,   /* unmute health probe failed: stale AVD state, REBOOT the player before the next run */
    RC_PLAY    = 9,   /* VIDEO PLAY failed */
    RC_WRITE   = 10,  /* VcAvWrite failed (vcavsrv died?) */
    RC_HALT    = 11,  /* decoder halted (elapsed frozen with data pending) or never started presenting */
    RC_LIMIT   = 12,  /* per-session transfer budget used up: input truncated (the log gives the resume offset) */
    RC_SIGNAL  = 128  /* + signal number: stopped cleanly by SIGINT/SIGTERM/SIGHUP */
};

/* ======================================================================================================== */
/* logging and small helpers: write(2) only                                                                 */
/* ======================================================================================================== */
typedef struct { char b[1024]; int n; } sbuf;

static void sb_c(sbuf* s, char c){ if(s->n < (int)sizeof s->b - 2) s->b[s->n++] = c; }
static void sb_s(sbuf* s, const char* t){ while(*t) sb_c(s, *t++); }
static void sb_u(sbuf* s, unsigned long long u, int mindig){
    char t[24]; int i = 0;
    do { t[i++] = (char)('0' + u % 10); u /= 10; } while(u || i < mindig);
    while(i) sb_c(s, t[--i]);
}
static void sb_i(sbuf* s, long long v){
    if(v < 0){ sb_c(s, '-'); sb_u(s, (unsigned long long)(-(v + 1)) + 1, 1); } else sb_u(s, (unsigned long long)v, 1);
}
static void sb_x(sbuf* s, unsigned v){
    static const char H[] = "0123456789abcdef"; int started = 0;
    sb_s(s, "0x");
    for(int i = 28; i >= 0; i -= 4){ int d = (int)((v >> i) & 15); if(d || started || i == 0){ sb_c(s, H[d]); started = 1; } }
}
static void sb_t45(sbuf* s, long long t){            /* 45 kHz ticks -> seconds, 3 decimals */
    if(t < 0){ sb_c(s, '-'); t = -t; }
    long long ms = t / 45;
    sb_u(s, (unsigned long long)(ms / 1000), 1); sb_c(s, '.'); sb_u(s, (unsigned long long)(ms % 1000), 3);
}
static void write_all(int fd, const char* p, size_t n){
    while(n){ ssize_t r = write(fd, p, n); if(r < 0){ if(errno == EINTR) continue; return; } p += r; n -= (size_t)r; }
}

enum { LV_DBG, LV_INFO, LV_WARN, LV_ERR };
static int g_loglevel = LV_INFO;
static const char* g_logtag = "ub820play: ";
static sbuf L;                                       /* the log line being built */
static void lo(const char* s){ sb_s(&L, s); }
static void lo_i(long long v){ sb_i(&L, v); }
static void lo_x(unsigned v){ sb_x(&L, v); }
static void lo_t(long long t45){ sb_t45(&L, t45); }
static void lend(int lvl){
    if(lvl >= g_loglevel){
        sbuf o; o.n = 0;
        sb_s(&o, g_logtag);
        if(lvl == LV_WARN) sb_s(&o, "WARNING: ");
        if(lvl == LV_ERR)  sb_s(&o, "ERROR: ");
        L.b[L.n] = 0; sb_s(&o, L.b); o.b[o.n++] = '\n';
        write_all(2, o.b, (size_t)o.n);
    }
    L.n = 0;
}
#define LOG1(lvl, s) do{ lo(s); lend(lvl); }while(0)

static long long now_ms(void){
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}
static void sleep_ms(long ms){
    struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };
    while(nanosleep(&ts, &ts) < 0 && errno == EINTR) {}
}
static int env_int(const char* name, int dflt){
    const char* v = getenv(name); return (v && *v) ? (int)strtol(v, 0, 0) : dflt;
}
static void put32(uint8_t* p, int off, uint32_t v){ memcpy(p + off, &v, 4); }
static void put16(uint8_t* p, int off, uint16_t v){ memcpy(p + off, &v, 2); }
static uint32_t get32(const uint8_t* p, int off){ uint32_t v; memcpy(&v, p + off, 4); return v; }

/* ======================================================================================================== */
/* vcavsrv client API (libvcavif.so.0) and device ioctls.  [C] = confirmed in code or live, see findings      */
/* ======================================================================================================== */
typedef int  (*connect_t)(int);
typedef int  (*create_t)(void);
typedef int  (*open_t)(const char*, int);
typedef int  (*ioctl_t)(int, unsigned, void*, int, const char*, int);   /* trailing (file, line): server log only */
typedef int  (*ioctlr_t)(int, unsigned, int, const char*, int);
typedef int  (*write_t)(int, void*, int);
typedef int  (*ctrl_t)(int);
typedef int  (*close_t)(int);
typedef void (*delete_t)(void);
static connect_t VcAvConnect; static create_t VcAvCreate; static open_t VcAvOpen; static ioctl_t VcAvIoctl_func;
static ioctlr_t VcAvIoctl_funcR; static write_t VcAvWrite; static ctrl_t VcAvCtrl; static close_t VcAvClose;
static delete_t VcAvDelete;

#define VCAV_CTRL_RESUME   1          /* susp=0 (boot state) */
#define VCAV_CTRL_RELEASE  5          /* close TDAV, stop+close all AVD (omplayer close) */

#define VIDEO_IOCOPENQRY        0x8054d601u  /* 0x54 */
#define VIDEO_IOCCONTROL        0x8008d602u  /* {u32 sub, u32 param} */
#define VIDEO_IOCSETPROPERTY    0x8044d603u  /* 0x44 */
#define VIDEO_IOCGETPROPERTY    0x4044d604u  /* 0x44 */
#define VIDEO_IOCWAITSTATE      0x4004d605u  /* 4: 2 = AVAILABLE, 1 = LOST */
#define STREAM_IOCOPENQRY       0x4004d601u  /* 0x48 */
#define STREAM_IOCSETPROPERTY   0x8004d602u  /* 0x40: byte0 = stream type */
#define STREAM_IOCGETPROPERTY   0x4004d603u  /* 0x40 */
#define STREAM_IOCPLAY          0x8004d606u  /* 0x80, contents ignored by the server */
#define STREAM_IOCRESET         0x0000d608u  /* funcR(fd, cmd, 0): after open and before close */
#define STREAM_IOCELAPSED       0x4004d610u  /* 8: w0 = display PTS in 45 kHz (AVD_GetPlayINF +8); 0 until the first
                                                picture, frozen while paused [C] */
#define STREAM_IOCFEEDSTAT      0x8004d617u  /* 8: {0x46800, FeedDecs in flight 0..3} (vcavsrv 0x1aa90) [C live] */
#define AUDIO_IOCOPENQRY        0x4030d701u  /* 0x30 */
#define AUDIO_IOCCONTROL        0x8008d702u  /* {u32 sub, u32 param}: 0 PLAY (= A/V start), 1 STOP (= A/V stop), 3 no-op */
#define AUDIO_IOCSETPROPERTY    0x8020d703u  /* 0x20, plain memcpy on the server: consumed at the next A/V start */
#define AUDIO_IOCGETPROPERTY    0x4020d704u  /* 0x20 */
enum { ACTL_PLAY = 0, ACTL_STOP = 1, ACTL_UNMUTE = 3 };

/* VIDEO_IOCCONTROL sub-commands (vcavsrv table @0x182fc) [C]. Pause = do_pause_on/off -> AVD_ControlMode. */
enum { VCTL_PLAY = 0, VCTL_STOP = 1, VCTL_MUTE = 2, VCTL_UNMUTE = 3, VCTL_PAUSE_ON = 4, VCTL_PAUSE_OFF = 5 };

/* VIDEO property +0x11 codec ids (get_avd_video_codec @0x16d94) [C] */
enum { VC_MPEG2 = 0, VC_H264 = 2, VC_VC1 = 3, VC_HEVC = 4, VC_VP9 = 5 };
static const struct { const char* name; int id; } g_codecs[] = {
    { "mpeg2", VC_MPEG2 }, { "h264", VC_H264 }, { "vc1", VC_VC1 }, { "hevc", VC_HEVC }, { "vp9", VC_VP9 } };
static const char* codec_name(int id){
    for(unsigned i = 0; i < sizeof g_codecs / sizeof g_codecs[0]; i++) if(g_codecs[i].id == id) return g_codecs[i].name;
    return "?";
}

/* AUDIO property +0x02 = vcav audio codec -> AVD codec (vcav_avd_get_avd_audiocodec @0x16590) [C]; names from
 * apl_bdv's BD coding_type table and omplayer dumpts (findings/vcav-audio.md 3). DTS, DTS-HD, TrueHD and LPCM are
 * NOT reachable through vcavsrv: transcode them (ffmpeg -c:a eac3 -b:a 640k). WMA (12) needs an ASF header ioctl
 * that is not implemented here. */
enum { AC_AAC = 0, AC_AC3 = 1, AC_MPEG = 2, AC_HEAAC = 7, AC_EAC3 = 8 };
static const struct { const char* name; int id; } g_acodecs[] = {
    { "aac", AC_AAC }, { "ac3", AC_AC3 }, { "eac3", AC_EAC3 }, { "ddp", AC_EAC3 }, { "mpeg", AC_MPEG }, { "mp2", AC_MPEG },
    { "mp3", AC_MPEG }, { "heaac", AC_HEAAC } };
static const char* acodec_name(int id){
    for(unsigned i = 0; i < sizeof g_acodecs / sizeof g_acodecs[0]; i++) if(g_acodecs[i].id == id) return g_acodecs[i].name;
    return "raw";
}
static int parse_acodec(const char* s){             /* name or number 0..255; -1 = invalid */
    for(unsigned i = 0; i < sizeof g_acodecs / sizeof g_acodecs[0]; i++) if(!strcmp(s, g_acodecs[i].name)) return g_acodecs[i].id;
    char* e; long v = strtol(s, &e, 0);
    return (e != s && !*e && v >= 0 && v < 256) ? (int)v : -1;
}

#define STREAM_TYPE_TS188  0          /* raw 188-byte TS ring mode [C live] */
#define PID_DEFAULT        0x1011     /* video and PCR PID of `ffmpeg -streamid 0:4113` */

/* Limits [C: findings/vcav-ts-40mb.md, vcav-hevc-tiles.md, LOG 2026-11-27 ~22:40 and ~23:25] */
#define TS_PKT        188
#define BLOCK_MAX     (4L * 1024 * 1024 / TS_PKT * TS_PKT)    /* 4194280; one write must stay <= ~4.37 MB */
#define BLOCK_MIN     (TS_PKT * 512L)
#define PREBUF_MAX    (2L * 1024 * 1024 / TS_PKT * TS_PKT)    /* before PLAY the type-0 ring takes only a few MB */
#define RING_EST      (9L * 1024 * 1024)                       /* type-0 rtsc ring, >= ~9.3 MB live [I] */
#define SESSION_XFERS 300                                      /* FeedDec transfers per vcavsrv session [C live] */
#define SESSION_WARN  290
#define PARTIAL_MIN   (1L * 1024 * 1024)                       /* smallest early (starving-input) write */

static int ioc(int fd, unsigned cmd, void* arg, int sz){
    int r = VcAvIoctl_func(fd, cmd, arg, sz, "ub820play", __LINE__);
    if(g_loglevel <= LV_DBG){ lo("  ioctl "); lo_x(cmd); lo(" -> "); lo_i(r); lend(LV_DBG); }
    return r;
}
static int vctl(int vfd, unsigned sub){
    uint32_t a[2] = { sub, 0 };
    int r = ioc(vfd, VIDEO_IOCCONTROL, a, 8);
    if(r < 0){ lo("VIDEO_IOCCONTROL sub "); lo_i(sub); lo(" failed: "); lo_i(r); lend(LV_WARN); }
    return r;
}

/* ======================================================================================================== */
/* options                                                                                                  */
/* ======================================================================================================== */
static struct {
    const char* input;
    int codec;            /* -1 = from the PMT */
    int route4k;          /* 1 = 4KOTT routing, 0 = DIVX routing */
    int vpid, pcrpid;     /* -1 = from the PMT, else PID_DEFAULT */
    int apid, acodec;     /* apid: -1 = first playable audio track in the PMT, -2 = none (video-only);
                             acodec: vcav audio codec, -1 = from the PMT */
    int afs;              /* audio sampling rate in Hz (AUDIO property +0x1c; 0 also means 48 kHz) */
    double lead;          /* seconds of PCR lead over the display clock */
    const char* ctl;      /* control FIFO, "" = none */
    const char* status;   /* status file, "" = none */
    long block;           /* bytes per VcAvWrite */
    int max_writes;       /* stop feeding after this many writes (session transfer budget) */
    int max_inflight;     /* write only while FeedDecs in flight <= this */
    int inflight_set;     /* --max-inflight given: no automatic clamp */
    int watchdog;         /* seconds without main-loop progress before the watchdog fires; 0 = off */
    int width, height, fps1000;
} O;

static const char USAGE[] =
"usage: ub820play [options] <file.ts | ->\n"
"  --codec mpeg2|h264|vc1|hevc|vp9  video codec (default: from the PMT)\n"
"  --4k | --hd                      4KOTT routing (UHD; the home UI is torn down) | DIVX routing (<=1080p, default)\n"
"  --vpid PID  --pcrpid PID         video / PCR PID (default: from the PMT, else 0x1011)\n"
"  --apid PID|none                  audio PID (default: first playable audio track in the PMT; none = video only)\n"
"  --acodec aac|ac3|eac3|mpeg|heaac|N  vcav audio codec (default: from the PMT)\n"
"  --afs HZ                         audio sampling rate (default 48000)\n"
"  --lead SECONDS                   PCR lead kept ahead of the display clock (default 3)\n"
"  --ctl PATH                       control FIFO (default /tmp/ub820play.ctl; \"\" = off)\n"
"  --status PATH                    status file (default /tmp/ub820play.status; \"\" = off)\n"
"  --block BYTES                    bytes per write, 96256..4194280 (default 4194280)\n"
"  --max-writes N                   writes per vcavsrv session (default 300 = the FeedDec limit)\n"
"  --max-inflight N                 write only while FeedDecs in flight <= N, 0..2 (default 1)\n"
"  --watchdog SECONDS               kill + home restore if the player is wedged (default 20; 0 = off)\n"
"  --size WxH  --fps F              VIDEO property size / rate (default 1920x1080, 24)\n"
"  --quiet | -v                     warnings and errors only | debug log\n";

static int parse_num(const char* s, long* out){
    char* e; errno = 0; long v = strtol(s, &e, 0);
    if(errno || e == s || *e) return -1;
    *out = v; return 0;
}
static int parse_args(int argc, char** argv){
    O.codec = -1; O.vpid = -1; O.pcrpid = -1; O.apid = -1; O.acodec = -1; O.afs = 48000; O.lead = 3.0;
    O.ctl = "/tmp/ub820play.ctl"; O.status = "/tmp/ub820play.status";
    O.block = BLOCK_MAX; O.max_writes = 2147483647;   /* unlimited: draining the server's per-transfer events
                                                              * (drain_events) removes the old 300-transfer stall;
                                                              * UB820_DRAIN=0 restores the 300 cap */
    if(getenv("UB820_DRAIN") && !atoi(getenv("UB820_DRAIN"))) O.max_writes = SESSION_XFERS; O.max_inflight = 1; O.watchdog = 20;
    O.width = 1920; O.height = 1080; O.fps1000 = 24000;
    for(int i = 1; i < argc; i++){
        const char* a = argv[i];
        if(a[0] != '-' || !strcmp(a, "-")){
            if(O.input){ LOG1(LV_ERR, "more than one input given"); return -1; }
            O.input = a; continue;
        }
        if(!strcmp(a, "--")){
            if(i + 1 >= argc || O.input || i + 2 < argc){ LOG1(LV_ERR, "exactly one input after --"); return -1; }
            O.input = argv[++i]; continue; }
        if(!strcmp(a, "-h") || !strcmp(a, "--help")){ write_all(1, USAGE, sizeof USAGE - 1); exit(0); }
        if(!strcmp(a, "--4k")){ O.route4k = 1; continue; }
        if(!strcmp(a, "--hd")){ O.route4k = 0; continue; }
        if(!strcmp(a, "--quiet") || !strcmp(a, "-q")){ g_loglevel = LV_WARN; continue; }
        if(!strcmp(a, "-v") || !strcmp(a, "--verbose")){ g_loglevel = LV_DBG; continue; }
        /* options with a value: "--opt value" or "--opt=value" */
        const char* val = 0; char name[32]; const char* eq = strchr(a, '=');
        if(eq){ size_t n = (size_t)(eq - a); if(n >= sizeof name) n = sizeof name - 1; memcpy(name, a, n); name[n] = 0; val = eq + 1; }
        else { size_t n = strlen(a); if(n >= sizeof name) n = sizeof name - 1; memcpy(name, a, n); name[n] = 0;
               if(i + 1 < argc) val = argv[++i]; }
        if(!val){ lo("option "); lo(name); lo(" needs a value"); lend(LV_ERR); return -1; }
        long n = 0;
        if(!strcmp(name, "--codec")){
            O.codec = -2;
            for(unsigned k = 0; k < sizeof g_codecs / sizeof g_codecs[0]; k++) if(!strcmp(val, g_codecs[k].name)) O.codec = g_codecs[k].id;
            if(O.codec == -2){ lo("unknown codec '"); lo(val); lo("'"); lend(LV_ERR); return -1; }
        }
        else if(!strcmp(name, "--vpid")   && !parse_num(val, &n) && n > 0 && n < 0x1fff) O.vpid = (int)n;
        else if(!strcmp(name, "--pcrpid") && !parse_num(val, &n) && n > 0 && n < 0x1fff) O.pcrpid = (int)n;
        else if(!strcmp(name, "--apid") && !strcmp(val, "none")) O.apid = -2;
        else if(!strcmp(name, "--apid")   && !parse_num(val, &n) && n > 0 && n < 0x1fff) O.apid = (int)n;
        else if(!strcmp(name, "--acodec") && (O.acodec = parse_acodec(val)) >= 0) {}
        else if(!strcmp(name, "--afs") && !parse_num(val, &n) && n >= 0 && n <= 192000) O.afs = (int)n;
        else if(!strcmp(name, "--lead")){ char* e; double d = strtod(val, &e);
            if(e == val || *e || d < 0.5 || d > 30){ LOG1(LV_ERR, "--lead must be 0.5..30 seconds"); return -1; } O.lead = d; }
        else if(!strcmp(name, "--ctl")) O.ctl = val;
        else if(!strcmp(name, "--status")) O.status = val;
        else if(!strcmp(name, "--block") && !parse_num(val, &n)){
            if(n < BLOCK_MIN) n = BLOCK_MIN;
            if(n > BLOCK_MAX) n = BLOCK_MAX;
            O.block = n / TS_PKT * TS_PKT; }
        else if(!strcmp(name, "--max-writes") && !parse_num(val, &n) && n >= 1 && n <= 100000) O.max_writes = (int)n;
        else if(!strcmp(name, "--max-inflight") && !parse_num(val, &n) && n >= 0 && n <= 2){ O.max_inflight = (int)n; O.inflight_set = 1; }
        else if(!strcmp(name, "--watchdog") && !parse_num(val, &n) && n >= 0 && n <= 3600) O.watchdog = (int)n;
        else if(!strcmp(name, "--size")){ char* e; long w = strtol(val, &e, 10), h = (*e == 'x' || *e == 'X') ? strtol(e + 1, &e, 10) : 0;
            if(w < 16 || h < 16 || w > 8192 || h > 8192 || *e){ LOG1(LV_ERR, "--size wants WxH"); return -1; }
            O.width = (int)w; O.height = (int)h; }
        else if(!strcmp(name, "--fps")){ char* e; double d = strtod(val, &e);
            if(e == val || *e || d < 1 || d > 240){ LOG1(LV_ERR, "--fps must be 1..240"); return -1; } O.fps1000 = (int)(d * 1000 + 0.5); }
        else { lo("bad option or value: "); lo(name); lo(" "); lo(val); lend(LV_ERR); return -1; }
    }
    if(!O.input){ write_all(2, USAGE, sizeof USAGE - 1); return -1; }
    /* default: in-flight blocks + the new one must fit the ~9 MiB type-0 ring (gate: written - consumed + len <= ring).
     * An explicit --max-inflight is taken as given (2 = the server's own gate). */
    if(!O.inflight_set){
        if(O.max_inflight >= 1 && 2 * O.block > RING_EST - 512 * 1024) O.max_inflight = 0;
        if(O.max_inflight == 2 && 3 * O.block > RING_EST - 512 * 1024) O.max_inflight = 1;
    }
    return 0;
}

/* ======================================================================================================== */
/* signals: handled only inside pselect() (blocked everywhere else, so no VcAv call is ever interrupted)    */
/* ======================================================================================================== */
static volatile sig_atomic_t g_sig;
static sigset_t g_waitmask, g_origmask;                     /* mask used while waiting in pselect: termination signals open */
static void on_signal(int s){ if(!g_sig) g_sig = s; }

static void signals_setup(void){
    struct sigaction sa; memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal; sigemptyset(&sa.sa_mask); sa.sa_flags = 0;       /* no SA_RESTART */
    sigaction(SIGINT, &sa, 0); sigaction(SIGTERM, &sa, 0); sigaction(SIGHUP, &sa, 0);
    signal(SIGPIPE, SIG_IGN);       /* a dead vcavsrv / closed pipe must give -1, not kill us mid-teardown */
    sigset_t blk; sigemptyset(&blk); sigaddset(&blk, SIGINT); sigaddset(&blk, SIGTERM); sigaddset(&blk, SIGHUP);
    sigprocmask(SIG_BLOCK, &blk, &g_waitmask);
    g_origmask = g_waitmask;
    sigdelset(&g_waitmask, SIGINT); sigdelset(&g_waitmask, SIGTERM); sigdelset(&g_waitmask, SIGHUP);
}

/* ======================================================================================================== */
/* MPEG-TS input: 188-byte packets, resync on a lost 0x47, PCR / PTS / PSI tracking                         */
/* ======================================================================================================== */
#define RBUF_CAP (256 * 1024)
static struct {
    int fd, eof, is_stdin;
    uint8_t* r; long rlen;                /* raw read buffer */
    int synced, m2ts_checked;
    long long in_pos;                     /* input bytes moved out of the read buffer (packets + skipped) */
    long long skipped; long resyncs;
} IN;

static struct {
    uint8_t* b; long len, cap;            /* the next VcAvWrite block (whole packets) */
    int pcr_valid; uint32_t pcr_last;     /* last PCR (45 kHz) inside the block */
} BLK;

static struct {
    int vpid, pcrpid;                     /* PIDs used for tracking (final after the prebuffer) */
    int track;                            /* 0 = PSI only (prebuffer scan), 1 = PCR/PTS */
    int pcr_first_valid; uint32_t pcr_first;
    int pcr_any_valid;   uint32_t pcr_any;            /* last PCR appended to any block */
    int pts_first_valid; uint32_t pts_first;
    int pts_max_valid;   uint32_t pts_max;            /* max video PTS (45 kHz) appended */
    /* PSI auto-detection */
    int pmt_pid, pmt_seen, pmt_vpid, pmt_vtype, pmt_pcrpid;
    int n_audio; struct { int pid, stype, desc, acodec; } audio[8];   /* acodec -1 = not playable via vcavsrv */
    int other_pcr_pid;                                 /* a PID that carries PCR but is not the PCR PID */
} TS;

static int stream_type_codec(int st){
    switch(st){ case 0x01: case 0x02: return VC_MPEG2; case 0x1b: return VC_H264; case 0x24: return VC_HEVC;
                case 0xea: return VC_VC1; default: return -1; }
}
/* PMT audio stream -> vcav audio codec: 1 = audio but not playable, -2 = not audio. `desc` = AC-3 (0x6a), E-AC-3
 * (0x7a) or DTS (0x7b) descriptor tag for DVB-style private streams (stream_type 0x06), else 0. */
static int stream_type_acodec(int st, int desc){
    switch(st){
    case 0x0f: return AC_AAC;                      /* ADTS AAC: what vcavsrv itself feeds AVD */
    case 0x81: return AC_AC3;
    case 0x87: return AC_EAC3;                     /* ffmpeg mpegts writes E-AC-3 as 0x87 */
    case 0x03: case 0x04: return AC_MPEG;
    case 0x06: return desc == 0x6a ? AC_AC3 : desc == 0x7a ? AC_EAC3 : desc == 0x7b ? -1 : -2;
    case 0x11: case 0x80: case 0x82: case 0x83: case 0x84: case 0x85: case 0x86: case 0x8a: case 0xa1: case 0xa2:
        return -1;                                 /* LATM, LPCM, DTS*, TrueHD, BD DD+ secondary: not reachable */
    default: return -2;
    }
}
static const char* stream_type_name(int st, int desc){
    switch(st){
    case 0x03: case 0x04: return "mpeg-audio"; case 0x0f: return "aac"; case 0x11: return "aac-latm";
    case 0x80: return "lpcm"; case 0x81: return "ac3"; case 0x87: case 0x84: case 0xa1: return "eac3";
    case 0x82: case 0x8a: return "dts"; case 0x83: return "truehd"; case 0x85: case 0x86: case 0xa2: return "dts-hd";
    case 0x06: return desc == 0x6a ? "ac3" : desc == 0x7a ? "eac3" : desc == 0x7b ? "dts" : "private";
    default: return "?";
    }
}
/* PSI section start inside one packet; sections spanning packets are ignored (ffmpeg never does that for PAT/PMT) */
static const uint8_t* psi_section(const uint8_t* p, int* len){
    if(!(p[1] & 0x40)) return 0;                                  /* PUSI */
    int off = 4;
    if(p[3] & 0x20) off += 1 + p[4];                              /* adaptation field */
    if(!(p[3] & 0x10) || off >= TS_PKT) return 0;
    off += 1 + p[off];                                            /* pointer field */
    if(off + 3 > TS_PKT) return 0;
    int sl = ((p[off + 1] & 0x0f) << 8) | p[off + 2];
    if(off + 3 + sl > TS_PKT || sl < 9) return 0;
    *len = 3 + sl; return p + off;
}
static void ts_psi(const uint8_t* p, int pid){
    int sl; const uint8_t* s = psi_section(p, &sl); if(!s) return;
    if(pid == 0 && s[0] == 0x00 && !TS.pmt_pid){
        for(int i = 8; i + 4 <= sl - 4; i += 4){
            int prog = (s[i] << 8) | s[i + 1], ppid = ((s[i + 2] & 0x1f) << 8) | s[i + 3];
            if(prog != 0){ TS.pmt_pid = ppid; break; }
        }
    } else if(pid == TS.pmt_pid && s[0] == 0x02 && !TS.pmt_seen){
        TS.pmt_seen = 1;
        TS.pmt_pcrpid = ((s[8] & 0x1f) << 8) | s[9];
        int pil = ((s[10] & 0x0f) << 8) | s[11];
        for(int i = 12 + pil; i + 5 <= sl - 4; ){
            int st = s[i], epid = ((s[i + 1] & 0x1f) << 8) | s[i + 2], eil = ((s[i + 3] & 0x0f) << 8) | s[i + 4];
            if(!TS.pmt_vpid && stream_type_codec(st) >= 0){ TS.pmt_vpid = epid; TS.pmt_vtype = st; }
            int desc = 0;
            for(int d = i + 5; d + 2 <= i + 5 + eil && d + 2 <= sl - 4; d += 2 + s[d + 1])
                if(s[d] == 0x6a || s[d] == 0x7a || s[d] == 0x7b){ desc = s[d]; break; }
            if(TS.n_audio < 8 && stream_type_acodec(st, desc) != -2){
                TS.audio[TS.n_audio].pid = epid; TS.audio[TS.n_audio].stype = st; TS.audio[TS.n_audio].desc = desc;
                TS.audio[TS.n_audio].acodec = stream_type_acodec(st, desc); TS.n_audio++;
            }
            i += 5 + eil;
        }
    }
}
static void ts_packet(const uint8_t* p){
    int pid = ((p[1] & 0x1f) << 8) | p[2];
    int afc = (p[3] >> 4) & 3;
    if(!TS.track){ if(pid == 0 || (TS.pmt_pid && pid == TS.pmt_pid)) ts_psi(p, pid); return; }
    if((afc & 2) && p[4] >= 7 && (p[5] & 0x10)){                 /* PCR: 33-bit base / 2 = 45 kHz, fits u32 */
        uint32_t base_hi = ((uint32_t)p[6] << 24) | ((uint32_t)p[7] << 16) | ((uint32_t)p[8] << 8) | p[9];
        uint32_t pcr45 = base_hi;                                 /* = (33-bit base) >> 1 */
        if(pid == TS.pcrpid){
            BLK.pcr_valid = 1; BLK.pcr_last = pcr45;
            TS.pcr_any_valid = 1; TS.pcr_any = pcr45;
            if(!TS.pcr_first_valid){ TS.pcr_first_valid = 1; TS.pcr_first = pcr45; }
        } else if(!TS.other_pcr_pid) TS.other_pcr_pid = pid;
    }
    if(pid == TS.vpid && (p[1] & 0x40) && (afc & 1)){           /* PES header: take the PTS */
        int off = 4 + ((afc & 2) ? 1 + p[4] : 0);
        if(off + 14 <= TS_PKT && p[off] == 0 && p[off + 1] == 0 && p[off + 2] == 1 && (p[off + 7] & 0x80)){
            const uint8_t* h = p + off + 9;
            uint32_t pts45 = ((uint32_t)(h[0] & 0x0e) << 28) | ((uint32_t)h[1] << 21) | ((uint32_t)(h[2] >> 1) << 14)
                           | ((uint32_t)h[3] << 6) | (uint32_t)(h[4] >> 2);   /* (33-bit PTS) >> 1 */
            if(!TS.pts_first_valid){ TS.pts_first_valid = 1; TS.pts_first = pts45; }
            if(!TS.pts_max_valid || (int32_t)(pts45 - TS.pts_max) > 0){ TS.pts_max_valid = 1; TS.pts_max = pts45; }
        }
    }
}

/* Move whole packets from the read buffer into the block, up to `cap` bytes. Returns -1 on unsupported input. */
static int packetize(long cap){
    long i = 0;
    if(!IN.m2ts_checked && IN.rlen >= 2 * 192 + 8){
        IN.m2ts_checked = 1;
        const uint8_t* r = IN.r;
        if(r[0] != 0x47 && r[4] == 0x47 && r[196] == 0x47 && r[388] == 0x47){
            LOG1(LV_ERR, "input looks like 192-byte M2TS/TTS packets; remux to plain 188-byte MPEG-TS (-f mpegts)");
            return -1;
        }
    }
    while(IN.rlen - i >= TS_PKT && BLK.len + TS_PKT <= cap){
        const uint8_t* q = IN.r + i;
        if(q[0] == 0x47){
            if(!IN.synced){                                       /* lock only on two sync bytes in a row */
                if(IN.rlen - i < 2 * TS_PKT && !IN.eof) break;    /* need more data to confirm */
                if(IN.rlen - i >= 2 * TS_PKT && q[TS_PKT] != 0x47){ i++; IN.skipped++; continue; }
                IN.synced = 1;
                if(IN.skipped){ lo("TS sync (re)acquired at input byte "); lo_i(IN.in_pos + i); lo(" after skipping ");
                                lo_i(IN.skipped); lo(" bytes in total"); lend(IN.resyncs <= 10 ? LV_WARN : LV_DBG); }
            }
            memcpy(BLK.b + BLK.len, q, TS_PKT);
            ts_packet(BLK.b + BLK.len);
            BLK.len += TS_PKT; i += TS_PKT;
        } else {
            if(IN.synced){ IN.synced = 0; IN.resyncs++;
                if(IN.resyncs <= 10){ lo("TS sync lost at input byte "); lo_i(IN.in_pos + i); lo(", resyncing"); lend(LV_WARN); } }
            i++; IN.skipped++;
        }
    }
    if(i){ memmove(IN.r, IN.r + i, (size_t)(IN.rlen - i)); IN.rlen -= i; IN.in_pos += i; }
    if(IN.eof && IN.rlen > 0 && IN.rlen < TS_PKT){             /* trailing partial packet */
        lo("dropping "); lo_i(IN.rlen); lo(" trailing bytes (partial TS packet)"); lend(LV_WARN);
        IN.in_pos += IN.rlen; IN.rlen = 0;
    }
    return 0;
}
/* One read() into the read buffer (the caller made sure the fd is readable or regular). */
static int input_read(void){
    if(IN.eof || IN.rlen >= RBUF_CAP) return 0;
    ssize_t r = read(IN.fd, IN.r + IN.rlen, (size_t)(RBUF_CAP - IN.rlen));
    if(r < 0){
        if(errno == EINTR || errno == EAGAIN) return 0;
        lo("input read error: "); lo(strerror(errno)); lo(" (treated as end of input)"); lend(LV_ERR);
        IN.eof = 1; return -1;
    }
    if(r == 0){ IN.eof = 1; return 0; }
    IN.rlen += r;
    return 0;
}
static int input_done(void){ return IN.eof && IN.rlen < TS_PKT; }

/* ======================================================================================================== */
/* control FIFO and status file                                                                             */
/* ======================================================================================================== */
static int g_ctl_fd = -1, g_ctl_created;
static char g_ctl_line[256]; static int g_ctl_len, g_ctl_overflow;

static void ctl_open(void){
    if(!O.ctl || !*O.ctl) return;
    struct stat st;
    if(stat(O.ctl, &st) == 0){
        if(!S_ISFIFO(st.st_mode)){ lo(O.ctl); lo(" exists and is not a FIFO: control disabled"); lend(LV_WARN); return; }
    } else if(mkfifo(O.ctl, 0666) == 0) g_ctl_created = 1;
    else { lo("mkfifo "); lo(O.ctl); lo(": "); lo(strerror(errno)); lo(": control disabled"); lend(LV_WARN); return; }
    /* O_RDWR: we are a writer too, so a client closing its end never leaves us at a permanent EOF */
    g_ctl_fd = open(O.ctl, O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if(g_ctl_fd < 0){ lo("open "); lo(O.ctl); lo(": "); lo(strerror(errno)); lend(LV_WARN); }
}
static void ctl_close(void){
    if(g_ctl_fd >= 0) close(g_ctl_fd);
    g_ctl_fd = -1;
    if(g_ctl_created) unlink(O.ctl);
    g_ctl_created = 0;
}

typedef enum { ST_INIT, ST_PREBUF, ST_PLAYING, ST_DRAINING, ST_STOPPING, ST_DONE } pstate;
static const char* const g_state_name[] = { "init", "prebuffer", "playing", "draining", "stopping", "done" };

static struct {
    pstate state; int paused, rc;
    int connected, routed;
    int v, s, au;                          /* vcavsrv handles */
    int started;                           /* PLAY sent */
    int presenting;                        /* elapsed has been nonzero */
    uint32_t elapsed; long long el_change_ms; long long t_play;
    int inflight;
    long long bytes;                       /* bytes written to vcavsrv */
    long long written_in_pos;              /* input offset just after the last written packet */
    int writes;
    int wpcr_valid; uint32_t wpcr;         /* last PCR written */
    int want_stop, want_status;
    const char* reason;
    int av, apid, acodec;                  /* A/V mode (VIDEO type 2) and the current audio track */
    int sw_pending; uint32_t sw_old; long long sw_t;   /* audio switch: AVD restarted, waiting for a new picture */
} P;
enum { STG_INIT, STG_CONNECT, STG_OPEN, STG_FEED, STG_TEARDOWN };   /* watchdog stages */
static void hb(int stage);

static long long lead45(void){
    if(!P.wpcr_valid) return -1;
    uint32_t ref = P.elapsed ? P.elapsed : TS.pcr_first;     /* before the first picture: lead from the first PCR */
    if(P.sw_pending)                                          /* after an A/V restart: the clock as it would have run */
        ref = P.sw_old + (uint32_t)((now_ms() - P.sw_t) * 45);
    return (long long)(int32_t)(P.wpcr - ref);
}

static void status_write(void){
    if(!O.status || !*O.status) return;
    sbuf s; s.n = 0;
    sb_s(&s, "state="); sb_s(&s, g_state_name[P.state]); if(P.paused) sb_s(&s, "-paused");
    uint32_t start = TS.pts_first_valid ? TS.pts_first : TS.pcr_first;
    sb_s(&s, " pos="); sb_t45(&s, (P.presenting && (TS.pts_first_valid || TS.pcr_first_valid)) ? (long long)(int32_t)(P.elapsed - start) : 0);
    sb_s(&s, " elapsed="); sb_t45(&s, (long long)P.elapsed);
    sb_s(&s, " bytes="); sb_i(&s, P.bytes);
    sb_s(&s, " in="); sb_i(&s, IN.in_pos);
    sb_s(&s, " writes="); sb_i(&s, P.writes); sb_c(&s, '/'); sb_i(&s, O.max_writes);
    long long ld = lead45();
    sb_s(&s, " lead="); if(!P.wpcr_valid) sb_s(&s, "?"); else sb_t45(&s, ld);
    sb_s(&s, " inflight="); sb_i(&s, P.inflight);
    sb_s(&s, " codec="); sb_s(&s, codec_name(O.codec));
    sb_s(&s, " route="); sb_s(&s, O.route4k ? "4k" : "hd");
    sb_s(&s, " audio="); if(P.av){ sb_x(&s, (unsigned)P.apid); sb_c(&s, '/'); sb_s(&s, acodec_name(P.acodec)); } else sb_s(&s, "none");
    sb_s(&s, " rc="); if(P.state == ST_DONE) sb_i(&s, P.rc); else sb_c(&s, '-');
    if(P.reason){ sb_s(&s, " reason="); sb_s(&s, P.reason); }
    sb_c(&s, '\n');
    char tmp[512]; size_t n = strlen(O.status);
    if(n + 5 > sizeof tmp) return;
    memcpy(tmp, O.status, n); memcpy(tmp + n, ".tmp", 5);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if(fd < 0) return;
    write_all(fd, s.b, (size_t)s.n); close(fd);
    rename(tmp, O.status);
}

/* ======================================================================================================== */
/* AUDIO: raw-TS A/V demux mode (findings/vcav-audio.md, static RE; NOT yet run live)                      */
/*  - VIDEO property +0x00 = 2 and AUDIO property type 2: the A/V decode starts on AUDIO PLAY (tdav_start_av */
/*    + AVD_Start TY=1 + AVD_Play); VIDEO PLAY is then an ack-only no-op [C];                               */
/*  - AUDIO property (0x20, GET -> fill -> SET like omplayer mmf_audio_set_demux_property): +0x00 type 2,    */
/*    +0x02 vcav codec, +0x04 PID (TDAV filter), +0x0e PCR PID (unused), +0x10 inport 5, +0x14 bit 2 clear,  */
/*    +0x1c fs Hz. No channel field: AVD takes the layout from the bitstream. Bitstream vs PCM on HDMI is    */
/*    decided by mocserver from the user's Setup menu, never by the client;                                 */
/*  - track switch: vcavsrv has no PID-change ioctl, so AUDIO STOP (TDAV filters off + AVD_Stop of audio AND */
/*    video) -> new property -> AUDIO PLAY. The feed keeps running (no new STREAM_IOCPLAY); expect a freeze   */
/*    of up to one GOP while video re-syncs at the next IRAP [I].                                           */
/* --apid none keeps the proven video-only start (VIDEO property 5, AUDIO property untouched).              */
/* ======================================================================================================== */
static int actl(unsigned sub){
    uint32_t a[2] = { sub, 0 };
    int r = ioc(P.au, AUDIO_IOCCONTROL, a, 8);
    if(r < 0){ lo("AUDIO_IOCCONTROL sub "); lo_i(sub); lo(" failed: "); lo_i(r); lend(LV_WARN); }
    return r;
}
static int set_audio_prop(int pid, int acodec){
    uint8_t ap[0x20]; memset(ap, 0, sizeof ap);
    ioc(P.au, AUDIO_IOCGETPROPERTY, ap, 0x20);
    ap[0x00] = 2;                                   /* demux A/V: PLAY/STOP start/stop the A/V decode */
    ap[0x02] = (uint8_t)acodec;
    put16(ap, 0x04, (uint16_t)pid);
    put16(ap, 0x06, 0);                             /* streamid */
    put16(ap, 0x0e, (uint16_t)O.pcrpid);            /* pcr: passed along, never used by the server */
    ap[0x10] = 5;                                   /* inport, as omplayer */
    put32(ap, 0x14, get32(ap, 0x14) & ~4u);         /* extention_mask: omplayer clears bit 2 for TS input */
    put32(ap, 0x1c, (uint32_t)O.afs);
    int r = ioc(P.au, AUDIO_IOCSETPROPERTY, ap, 0x20);
    if(r < 0){ LOG1(LV_ERR, "AUDIO_IOCSETPROPERTY failed"); return -1; }
    lo("audio: PID "); lo_x((unsigned)pid); lo(", codec "); lo(acodec_name(acodec)); lo(" ("); lo_i(acodec); lo("), fs ");
    lo_i(O.afs); lend(LV_INFO);
    return 0;
}
/* codec of a PMT audio track, -1 if unknown/unplayable */
static int pmt_acodec(int pid){
    for(int i = 0; i < TS.n_audio; i++) if(TS.audio[i].pid == pid) return TS.audio[i].acodec;
    return -1;
}
static void audio_switch(int pid, int acodec){
    if(!P.av){ LOG1(LV_WARN, "audio switch ignored: playing video-only (--apid none or no playable audio track); "
                              "A/V needs a restart"); return; }
    if(acodec < 0) acodec = pmt_acodec(pid);
    if(acodec < 0){ lo("audio switch to PID "); lo_x((unsigned)pid); lo(" ignored: codec unknown, give it (audio <pid> <codec>)");
                    lend(LV_WARN); return; }
    if(pid == P.apid && acodec == P.acodec){ LOG1(LV_INFO, "audio switch: already on that track"); return; }
    if(!P.started){ P.apid = pid; P.acodec = acodec; set_audio_prop(pid, acodec); return; }
    lo("audio switch to PID "); lo_x((unsigned)pid); lo(" ("); lo(acodec_name(acodec)); lo(")"); lend(LV_INFO);
    hb(STG_FEED);
    if(actl(ACTL_STOP) < 0 || set_audio_prop(pid, acodec) < 0){ P.want_stop = 1; P.rc = RC_PLAY; P.reason = "audio-switch-failed"; return; }
    vctl(P.v, VCTL_UNMUTE);
    if(actl(ACTL_PLAY) < 0){ P.want_stop = 1; P.rc = RC_PLAY; P.reason = "audio-switch-failed"; return; }
    vctl(P.v, VCTL_UNMUTE);
    P.apid = pid; P.acodec = acodec;
    /* AVD restarted: elapsed may read 0 or stay frozen at the old value until the next IRAP is shown. Until it
     * moves to something new, pace on the old value advanced by wall clock and re-arm the start-up grace. */
    P.sw_pending = 1; P.sw_old = P.elapsed; P.sw_t = now_ms();
    P.presenting = 0; P.t_play = P.sw_t; P.el_change_ms = P.sw_t;
    P.want_status = 1;
}

/* ======================================================================================================== */
/* control commands                                                                                         */
/* ======================================================================================================== */
static void do_pause(int on){
    if(!P.started || P.v < 0){ LOG1(LV_WARN, "pause/resume ignored: not playing yet"); return; }
    if(on == P.paused) return;
    /* VIDEO sub 4/5 = do_pause_on/off -> AVD_ControlMode [C code]; UNTESTED live on the raw-TS path */
    if(vctl(P.v, on ? VCTL_PAUSE_ON : VCTL_PAUSE_OFF) < 0) return;
    P.paused = on;
    P.el_change_ms = now_ms();            /* freeze timers restart on every pause transition */
    LOG1(LV_INFO, on ? "paused" : "resumed");
    P.want_status = 1;
}
static void ctl_command(char* c){
    while(*c == ' ' || *c == '\t') c++;
    char* e = c + strlen(c); while(e > c && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r')) *--e = 0;
    if(!*c) return;
    lo("control: "); lo(c); lend(LV_DBG);
    if(!strcmp(c, "stop") || !strcmp(c, "quit")){ P.want_stop = 1; P.reason = "stop-command"; }
    else if(!strcmp(c, "pause")) do_pause(1);
    else if(!strcmp(c, "resume") || !strcmp(c, "play")) do_pause(0);
    else if(!strcmp(c, "status")) P.want_status = 1;
    else if(!strncmp(c, "audio", 5) && (c[5] == ' ' || c[5] == '\t')){
        char *a = c + 6, *e1;
        long pid = strtol(a, &e1, 0);
        while(*e1 == ' ' || *e1 == '\t') e1++;
        int codec = *e1 ? parse_acodec(e1) : -1;
        if(e1 == a || pid <= 0 || pid >= 0x1fff || (*e1 && codec < 0)){ LOG1(LV_WARN, "usage: audio <pid> [codec]"); return; }
        audio_switch((int)pid, codec);
    }
    else { lo("unknown control command '"); lo(c); lo("'"); lend(LV_WARN); }
}
static void ctl_poll(void){
    if(g_ctl_fd < 0) return;
    char buf[256];
    for(;;){
        ssize_t r = read(g_ctl_fd, buf, sizeof buf);
        if(r <= 0) return;
        for(ssize_t i = 0; i < r; i++){
            if(buf[i] == '\n'){
                g_ctl_line[g_ctl_len] = 0;
                if(!g_ctl_overflow) ctl_command(g_ctl_line);
                g_ctl_len = 0; g_ctl_overflow = 0;
            } else if(g_ctl_len < (int)sizeof g_ctl_line - 1) g_ctl_line[g_ctl_len++] = buf[i];
            else g_ctl_overflow = 1;
        }
    }
}

/* ======================================================================================================== */
/* MOC (mocserver) video routing + home restore. findings/vcav-video-visible.md, vcav-ui-restore.md.         */
/* All libmoc calls take a trailing (func, file, line) used only for mocserver logging.                     */
/* ======================================================================================================== */
#define MOC_MON0                0
#define MOC_DEC0                0
#define AVDEV_NONE              0x00
#define AVDEV_STREAM_WIDGET     0x0e
#define AVDEV_SW_DIVX           0x12      /* stock USB/MKV player: use case DIVX, home survives */
#define AVDEV_SW_VIDEODL        0x13      /* VC engine / Cobalt: with play mode 8 -> use case 4KOTT (home Final'd) */
#define MOCA_ChangePlayMode_Ch0 0x12
#define MOC_PLAYMODE_NAVI       1
#define MOC_PLAYMODE_DIVX       7
#define MOC_PLAYMODE_VIERACAST  8
#define MOC_PLAYMODE_HOME       9
#define FBIO_CHECK_MEM_POOL_ATTACH 0x800446a5u   /* _IOR on /dev/fb6, as apl_mgr 0x20cf4 */
typedef int  (*moc2_t)(int, int, const char*, const char*, int);
typedef int  (*moc1_t)(int, const char*, const char*, int);
typedef int  (*mocsz_t)(int, uint16_t*, int, const char*, const char*, int);
typedef int  (*mocattr_t)(uint16_t*, int, const char*, const char*, int);
typedef int  (*rpcs_t)(int, int, void**);
typedef void (*rpce_t)(void*);
static moc2_t MOC_SetMonitorSelect, MOC_BindDecoder, MOC_MapStreamWidget;
static moc1_t MOC_UnmapStreamWidget; static mocsz_t MOC_ChangeSizeStreamWidget;
static mocattr_t MOC_SetMonitorAttributes, MOC_SetOSDAttributes;
static rpcs_t RPC_start_; static rpce_t RPC_end_;
static int moc_mapped, moc_pm_set;

static void moclog(const char* what, int rc){
    lo("  MOC "); lo(what); lo(" -> "); lo_x((unsigned)rc); lend(rc ? LV_WARN : LV_DBG);
}
static int moc_load(void){
    void* m = dlopen("libmoc.so", RTLD_NOW | RTLD_GLOBAL);
    if(!m) m = dlopen("/usr/dtvrec/lib/libmoc.so", RTLD_NOW | RTLD_GLOBAL);
    if(!m){ lo("dlopen libmoc: "); lo(dlerror()); lend(LV_ERR); return -1; }
    MOC_SetMonitorSelect       = (moc2_t)   dlsym(m, "_MOC_SetMonitorSelect");
    MOC_BindDecoder            = (moc2_t)   dlsym(m, "_MOC_BindDecoder");
    MOC_MapStreamWidget        = (moc2_t)   dlsym(m, "_MOC_MapStreamWidget");
    MOC_UnmapStreamWidget      = (moc1_t)   dlsym(m, "_MOC_UnmapStreamWidget");
    MOC_ChangeSizeStreamWidget = (mocsz_t)  dlsym(m, "_MOC_ChangeSizeStreamWidget");
    MOC_SetMonitorAttributes   = (mocattr_t)dlsym(m, "_MOC_SetMonitorAttributes");
    MOC_SetOSDAttributes       = (mocattr_t)dlsym(m, "_MOC_SetOSDAttributes");
    void* rp = dlopen("librpc.so", RTLD_NOW | RTLD_GLOBAL);      /* libmoc's DT_NEEDED: already loaded */
    RPC_start_ = (rpcs_t)dlsym(rp ? rp : RTLD_DEFAULT, "RPC_start");
    RPC_end_   = (rpce_t)dlsym(rp ? rp : RTLD_DEFAULT, "RPC_end");
    if(!MOC_SetMonitorSelect || !MOC_BindDecoder || !MOC_MapStreamWidget || !MOC_UnmapStreamWidget ||
       !MOC_ChangeSizeStreamWidget || !MOC_SetMonitorAttributes || !MOC_SetOSDAttributes || !RPC_start_ || !RPC_end_){
        LOG1(LV_ERR, "libmoc/librpc: missing symbols"); return -1; }
    return 0;
}
/* ACE memory pool as apl_mgr sees it: 1 attached (home can draw), 0 detached (home gets Final'd), -1 unknown */
static int moc_pool_attached(void){
    if(!env_int("VPOOLCHK", 1)) return -2;
    int fd = open("/dev/fb6", O_RDWR | O_CLOEXEC); uint32_t v = 0xffffffffu;
    if(fd < 0) return -1;
    int r = ioctl(fd, FBIO_CHECK_MEM_POOL_ATTACH, &v); close(fd);
    return r < 0 ? -1 : (int)v;
}
static void moc_pool_log(const char* when){
    int a = moc_pool_attached(); if(a == -2) return;
    lo("  ACE mem pool ("); lo(when); lo("): ");
    lo(a == 1 ? "attached" : a == 0 ? "DETACHED (home will be Final'd)" : "unknown"); lend(LV_DBG);
}
/* Play mode first, then monitor select / widget size / map / bind, as apl_meda_plynv (DIVX) and netlauncher
 * (Cobalt) do. Env overrides: VMOCAVDEV (0xf..0x17), VMOCPLAYMODE (>0). */
static int moc_route(int avdev, int playmode){
    int r; void* ctx = 0;
    avdev = env_int("VMOCAVDEV", avdev); if(avdev < 0x0f || avdev > 0x17) avdev = AVDEV_SW_DIVX;
    playmode = env_int("VMOCPLAYMODE", playmode);
    lo("MOC route: avdev "); lo_x((unsigned)avdev); lo(", play mode "); lo_i(playmode); lend(LV_INFO);
    RPC_start_(0, 0, &ctx);
    if(playmode > 0){
        uint16_t a[2] = { MOCA_ChangePlayMode_Ch0, (uint16_t)playmode };
        r = MOC_SetMonitorAttributes(a, 1, "moc_route", "ub820play", __LINE__); moclog("Set ChangePlayMode_Ch0", r);
        moc_pm_set = playmode;              /* even on error: the restore then forces a real change back to 9 */
    }
    r = MOC_SetMonitorSelect(MOC_MON0, AVDEV_STREAM_WIDGET, "moc_route", "ub820play", __LINE__);
    moclog("SetMonitorSelect(0, STREAM_WIDGET)", r);
    if(r){ RPC_end_(ctx); return -1; }
    { uint16_t sz[5][2] = { {0,0}, {1,0}, {2,0}, {3,0}, {8,1} };   /* full size, KeepAspect_PinP = 1 */
      r = MOC_ChangeSizeStreamWidget(MOC_MON0, &sz[0][0], 5, "moc_route", "ub820play", __LINE__);
      moclog("ChangeSizeStreamWidget(0, full)", r); }
    r = MOC_MapStreamWidget(MOC_MON0, avdev, "moc_route", "ub820play", __LINE__);
    moclog("MapStreamWidget(0, avdev)", r);
    if(r == 0) moc_mapped = 1;
    r = MOC_BindDecoder(MOC_DEC0, avdev, "moc_route", "ub820play", __LINE__);
    moclog("BindDecoder(0, avdev)", r);
    RPC_end_(ctx);
    moc_pool_log("after route");
    return r ? -1 : 0;
}
/* Unmap -> SetMonitorSelect(NONE) -> play mode 9 as a REAL change (9 -> 9 is skipped by mocserver, so go via 1
 * when we never changed it) -> home OSD preset {0,0x222},{7,0},{0x1f,0} + {0x27,1} (apl_pre_setting_home).
 * After a 4KOTT run the home is already Final'd: this restores the pool and ACEMGR Run, not the home's ADS
 * resources (open issue, see LOG 2026-11-27 ~17:40). VMOCHOMEOSD=0 skips the OSD preset. */
static void moc_restore(void){
    void* ctx = 0; int r;
    LOG1(LV_INFO, "MOC unroute + home restore");
    RPC_start_(0, 0, &ctx);
    if(moc_mapped){ r = MOC_UnmapStreamWidget(MOC_MON0, "moc_restore", "ub820play", __LINE__);
                    moclog("UnmapStreamWidget(0)", r); moc_mapped = 0; }
    r = MOC_SetMonitorSelect(MOC_MON0, AVDEV_NONE, "moc_restore", "ub820play", __LINE__);
    moclog("SetMonitorSelect(0, NONE)", r);
    if(moc_pm_set <= 0 || moc_pm_set == MOC_PLAYMODE_HOME){
        uint16_t a[2] = { MOCA_ChangePlayMode_Ch0, MOC_PLAYMODE_NAVI };
        r = MOC_SetMonitorAttributes(a, 1, "moc_restore", "ub820play", __LINE__); moclog("Set ChangePlayMode_Ch0=1", r);
        RPC_end_(ctx); sleep_ms(500); ctx = 0; RPC_start_(0, 0, &ctx);     /* AjaxCeplay waits 500 ms too */
    }
    { uint16_t a[2] = { MOCA_ChangePlayMode_Ch0, MOC_PLAYMODE_HOME };
      r = MOC_SetMonitorAttributes(a, 1, "moc_restore", "ub820play", __LINE__); moclog("Set ChangePlayMode_Ch0=9", r);
      moc_pm_set = 0; }
    if(env_int("VMOCHOMEOSD", 1)){
        uint16_t o[3][2] = { {0, 0x222}, {7, 0}, {0x1f, 0} };
        r = MOC_SetOSDAttributes(&o[0][0], 3, "moc_restore", "ub820play", __LINE__); moclog("SetOSDAttributes(home)", r);
        uint16_t e[2] = { 0x27, 1 };
        r = MOC_SetOSDAttributes(e, 1, "moc_restore", "ub820play", __LINE__); moclog("SetOSDAttributes({0x27,1})", r);
    }
    RPC_end_(ctx);
    for(int i = 0; i < 6; i++){ int a = moc_pool_attached(); if(a == 1 || a == -2 || a == -1) break; sleep_ms(500); }
    moc_pool_log("after restore");
}

/* ======================================================================================================== */
/* watchdog: a forked child that never touches vcavsrv.                                                    */
/*  - main loop wedged (e.g. inside VcAvWrite / a VcAv ioctl) for --watchdog s -> SIGKILL the player; the   */
/*    closed sockets make vcavsrv clean up exactly as after the TIMEOUT kills seen live;                    */
/*  - player gone while MOC is still routed (killed -9, crashed) -> restore the home routing;               */
/*  - either way it removes our FIFO and writes a final status line, then exits.                            */
/* ======================================================================================================== */
static struct wdshm {
    volatile long long hb_ms;
    volatile int armed, routed, pm, stage;
    volatile int done, ctl_created;
} *WD;
static const char* const g_stage_name[] = { "init", "connect", "open", "feed", "teardown" };
static void hb(int stage){ if(WD){ WD->hb_ms = now_ms(); WD->stage = stage; } }

static void watchdog_child(pid_t parent, int in_fd){
    g_logtag = "ub820play[watchdog]: ";
    signal(SIGINT, SIG_IGN); signal(SIGTERM, SIG_IGN); signal(SIGHUP, SIG_IGN);
    sigset_t none; sigemptyset(&none); sigprocmask(SIG_SETMASK, &none, 0);
    if(in_fd >= 0) close(in_fd);                     /* do not hold the input pipe open */
    { int nul = open("/dev/null", O_RDWR); if(nul >= 0){ dup2(nul, 0); dup2(nul, 1); if(nul > 2) close(nul); } }
    const char* why = 0; int rc = 0;
    for(;;){
        sleep_ms(250);
        if(getppid() != parent){ if(WD->done || !WD->routed) _exit(0); why = "crashed"; rc = -1; break; }
        if(WD->armed && O.watchdog > 0 && now_ms() - WD->hb_ms > (long long)O.watchdog * 1000){
            lo("no progress for "); lo_i(O.watchdog); lo(" s in stage '"); lo(g_stage_name[WD->stage]);
            lo("': killing the player (SIGKILL); vcavsrv cleans up on the closed sockets"); lend(LV_ERR);
            kill(parent, SIGKILL);
            for(int i = 0; i < 40 && getppid() == parent; i++) sleep_ms(100);
            why = "watchdog"; rc = 128 + SIGKILL; break;
        }
    }
    if(rc < 0) LOG1(LV_ERR, "player exited without restoring the home routing; restoring it now");
    if(WD->routed){
        sleep_ms(2000);                             /* let vcavsrv release AVD before the path is torn down */
        if(moc_load() == 0){ moc_mapped = 1; moc_pm_set = WD->pm; moc_restore(); }
    }
    if(WD->ctl_created) unlink(O.ctl);
    if(O.status && *O.status){                      /* our copy of the player state is stale: minimal line */
        sbuf s; s.n = 0;
        sb_s(&s, "state=done rc="); sb_i(&s, rc); sb_s(&s, " reason="); sb_s(&s, why); sb_c(&s, '\n');
        int fd = open(O.status, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
        if(fd >= 0){ write_all(fd, s.b, (size_t)s.n); close(fd); }
    }
    _exit(0);
}
static void watchdog_start(void){
    WD = mmap(0, sizeof *WD, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if(WD == MAP_FAILED){ WD = 0; LOG1(LV_WARN, "watchdog: mmap failed, running without it"); return; }
    memset((void*)WD, 0, sizeof *WD);
    hb(STG_INIT);
    pid_t parent = getpid(), c = fork();
    if(c < 0){ LOG1(LV_WARN, "watchdog: fork failed, running without it"); return; }
    if(c == 0) watchdog_child(parent, IN.is_stdin ? -1 : IN.fd);   /* stdin: replaced by /dev/null in the child */
}

/* ======================================================================================================== */
/* vcavsrv session                                                                                          */
/* ======================================================================================================== */
static int vcav_load(void){
    /* libvcavif uses pthread_sigmask etc. without DT_NEEDing libpthread on this glibc: preload it globally */
    if(!dlopen("libpthread.so.0", RTLD_NOW | RTLD_GLOBAL)){ lo("dlopen libpthread: "); lo(dlerror()); lend(LV_ERR); return -1; }
    void* h = dlopen("libvcavif.so.0", RTLD_NOW | RTLD_GLOBAL);
    if(!h) h = dlopen("/usr_root/lib/libvcavif.so.0", RTLD_LAZY | RTLD_GLOBAL);
    if(!h){ lo("dlopen libvcavif: "); lo(dlerror()); lend(LV_ERR); return -1; }
    VcAvConnect     = (connect_t)dlsym(h, "VcAvConnect");
    VcAvCreate      = (create_t) dlsym(h, "VcAvCreate");
    VcAvOpen        = (open_t)   dlsym(h, "VcAvOpen");
    VcAvIoctl_func  = (ioctl_t)  dlsym(h, "VcAvIoctl_func");
    VcAvIoctl_funcR = (ioctlr_t) dlsym(h, "VcAvIoctl_funcR");
    VcAvWrite       = (write_t)  dlsym(h, "VcAvWrite");
    VcAvCtrl        = (ctrl_t)   dlsym(h, "VcAvCtrl");
    VcAvClose       = (close_t)  dlsym(h, "VcAvClose");
    VcAvDelete      = (delete_t) dlsym(h, "VcAvDelete");
    if(!VcAvConnect || !VcAvOpen || !VcAvIoctl_func || !VcAvIoctl_funcR || !VcAvWrite || !VcAvCtrl || !VcAvClose || !VcAvDelete){
        LOG1(LV_ERR, "libvcavif: missing symbols"); return -1; }
    return 0;
}
/* video0 -> OPENQRY -> videoN, wait for AVAILABLE, STOP {1,0} (omplayer decoder_mmf) [C] */
static int open_video(void){
    int v0 = VcAvOpen("/dev/dtv/video0", 0x80002);
    if(v0 < 0){ LOG1(LV_ERR, "VcAvOpen(video0) failed"); return -1; }
    uint8_t q[0x54]; memset(q, 0, sizeof q);
    put32(q, 0x04, 0); put32(q, 0x08, 1); put32(q, 0x0c, 1);   /* prio 0, unit 1, vmode 1 (VC engine; no Ctrl(11)) */
    q[0x10] = 2;
    int r = ioc(v0, VIDEO_IOCOPENQRY, q, 0x54), no = (int)get32(q, 0);
    if(r < 0 || no < 0 || no > 9){ VcAvClose(v0); LOG1(LV_ERR, "VIDEO_IOCOPENQRY failed"); return -1; }
    char name[] = "/dev/dtv/video0"; name[sizeof name - 2] = (char)('0' + no);
    int v = VcAvOpen(name, 0x80002);
    VcAvClose(v0);
    if(v < 0){ lo("VcAvOpen("); lo(name); lo(") failed"); lend(LV_ERR); return -1; }
    for(int tries = 0; tries < 50; tries++){                    /* server pushes 4 bytes: 2 = AVAILABLE */
        hb(STG_OPEN);
        fd_set rf; FD_ZERO(&rf); FD_SET(v, &rf);
        struct timeval tv = { 0, 200000 };
        int sr = select(v + 1, &rf, 0, 0, &tv);
        if(sr <= 0) continue;
        uint32_t st = 0;
        if(ioc(v, VIDEO_IOCWAITSTATE, &st, 4) < 0) break;
        if(st == 2){ vctl(v, VCTL_STOP); lo("opened "); lo(name); lend(LV_DBG); return v; }
    }
    LOG1(LV_ERR, "video handle never became AVAILABLE");
    VcAvClose(v); return -1;
}
static int open_stream(void){
    int s0 = VcAvOpen("/dev/dtv/stream0", 0x80002);
    if(s0 < 0){ LOG1(LV_ERR, "VcAvOpen(stream0) failed"); return -1; }
    uint8_t q[0x48]; memset(q, 0, sizeof q);
    q[0x0a] = 2;
    int r = ioc(s0, STREAM_IOCOPENQRY, q, 0x48), no = (int)get32(q, 0);
    if(r < 0 || no < 0 || no > 9){ VcAvClose(s0); LOG1(LV_ERR, "STREAM_IOCOPENQRY failed"); return -1; }
    char name[] = "/dev/dtv/stream0"; name[sizeof name - 2] = (char)('0' + no);
    int s = VcAvOpen(name, 0x80002);
    VcAvClose(s0);
    if(s < 0){ lo("VcAvOpen("); lo(name); lo(") failed"); lend(LV_ERR); return -1; }
    VcAvIoctl_funcR(s, STREAM_IOCRESET, 0, "ub820play", __LINE__);
    lo("opened "); lo(name); lend(LV_DBG);
    return s;
}
/* audio0 -> OPENQRY -> audioN AFTER video+stream: makes vcavsrv reopen AVD in DVR mode (findings/vdec-acquire.md) */
static int open_audio(void){
    int a0 = VcAvOpen("/dev/dtv/audio0", 0x80002);
    if(a0 < 0){ LOG1(LV_ERR, "VcAvOpen(audio0) failed"); return -1; }
    uint8_t q[0x30]; memset(q, 0, sizeof q);
    put32(q, 0x04, 0); put32(q, 0x08, 1); q[0x10] = 0; q[0x11] = 1; q[0x12] = 3;
    int r = ioc(a0, AUDIO_IOCOPENQRY, q, 0x30), no = (int)get32(q, 0);
    if(r < 0 || no < 1 || no > 5){ VcAvClose(a0); LOG1(LV_ERR, "AUDIO_IOCOPENQRY failed"); return -1; }
    char name[] = "/dev/dtv/audio0"; name[sizeof name - 2] = (char)('0' + no);
    int a = VcAvOpen(name, 0x80002);
    VcAvClose(a0);
    if(a < 0){ lo("VcAvOpen("); lo(name); lo(") failed"); lend(LV_ERR); return -1; }
    lo("opened "); lo(name); lo(" (AVD DVR mode)"); lend(LV_DBG);
    return a;
}
static int set_properties(void){
    uint8_t sp[0x40]; memset(sp, 0, sizeof sp);
    ioc(P.s, STREAM_IOCGETPROPERTY, sp, 0x40);
    sp[0] = STREAM_TYPE_TS188; sp[1] = 0;                       /* must be set before the first write */
    if(ioc(P.s, STREAM_IOCSETPROPERTY, sp, 0x40) < 0){ LOG1(LV_ERR, "STREAM_IOCSETPROPERTY failed"); return -1; }

    uint8_t vp[0x44]; memset(vp, 0, sizeof vp);
    ioc(P.v, VIDEO_IOCGETPROPERTY, vp, 0x44);
    vp[0x00] = P.av ? 2 : 5;                                    /* 5 video-only: start on VIDEO PLAY; 2 A/V: on AUDIO PLAY */
    put16(vp, 0x02, (uint16_t)O.vpid);                          /* stream type <= 1: the real TS PID [C] */
    put16(vp, 0x06, (uint16_t)O.pcrpid);
    vp[0x10] = 5;
    vp[0x11] = (uint8_t)O.codec;
    put32(vp, 0x18, (uint32_t)O.height);                        /* size class only (SD / 720 / above) [C] */
    put32(vp, 0x1c, (uint32_t)O.width);
    put32(vp, 0x24, 1000);                                      /* pixel aspect x1000 */
    put32(vp, 0x28, (uint32_t)O.fps1000);                       /* rate x1000; > 50000 picks size class 6 [C] */
    put32(vp, 0x30, 0);
    put32(vp, 0x34, 0);                                         /* 0 = do_avd_start (AVD_Start + AVD_Play) */
    if(ioc(P.v, VIDEO_IOCSETPROPERTY, vp, 0x44) < 0){ LOG1(LV_ERR, "VIDEO_IOCSETPROPERTY failed"); return -1; }
    if(P.av && set_audio_prop(P.apid, P.acodec) < 0) return -1;   /* read by the A/V start (0x12dbc) */
    return 0;
}
static void poll_clock(long long now){
    uint32_t et[2] = { 0, 0 };
    if(VcAvIoctl_func(P.s, STREAM_IOCELAPSED, et, 8, "ub820play", __LINE__) < 0) return;
    if(P.sw_pending){
        if(!et[0] || et[0] == P.sw_old) return;
        P.sw_pending = 0; P.presenting = 1; P.elapsed = et[0]; P.el_change_ms = now;
        lo("picture again "); lo_i(now - P.sw_t); lo(" ms after the audio switch"); lend(LV_INFO);
        return;
    }
    if(et[0] != P.elapsed){
        if(!P.presenting && et[0]){ P.presenting = 1; lo("first picture after "); lo_i(now - P.t_play); lo(" ms"); lend(LV_INFO); }
        P.elapsed = et[0]; P.el_change_ms = now;
    }
}
static int query_inflight(void){
    uint32_t fq[2] = { 0, 0 };
    if(VcAvIoctl_func(P.s, STREAM_IOCFEEDSTAT, fq, 8, "ub820play", __LINE__) < 0) return -1;
    P.inflight = (int)fq[1];
    return P.inflight;
}
/* EXPERIMENT (UB820_DRAIN=1): vcavsrv may push unsolicited event bytes onto the client sockets (libvcavif reads some
 * "client-local" ioctls straight from the fd). If nobody reads them, the socket buffer fills and the server's next
 * send blocks -> its stream thread stalls (candidate cause of the 300-transfer limit). Between our synchronous
 * requests any readable bytes are events, so drain them (and log a sample). */
static long g_drained[3];
static void drain_events(void){
    static int on = -1; if(on < 0) on = getenv("UB820_DRAIN") ? (atoi(getenv("UB820_DRAIN")) ? 1 : 0) : 1;   /* default ON */
    if(!on) return;
    int fds[3] = { P.s, P.v, P.au };
    for(int k = 0; k < 3; k++){
        int fd = fds[k]; if(fd < 0) continue;
        for(;;){
            int avail = 0; if(ioctl(fd, FIONREAD, &avail) < 0 || avail <= 0) break;
            unsigned char buf[4096]; int want = avail < (int)sizeof buf ? avail : (int)sizeof buf;
            ssize_t r = read(fd, buf, (size_t)want); if(r <= 0) break;
            if(g_drained[k] == 0 || (g_drained[k] < 4096 && g_loglevel <= LV_INFO)){
                lo("drain fd "); lo_i(fd); lo(k==0?" (stream)":k==1?" (video)":" (audio)"); lo(": "); lo_i((long)r); lo(" bytes:");
                for(int i = 0; i < r && i < 24; i++){ static const char hx[] = "0123456789abcdef"; char t[4] = { ' ', hx[buf[i]>>4], hx[buf[i]&15], 0 }; lo(t); }
                lend(LV_INFO); }
            g_drained[k] += r;
        }
    }
}

static int write_block(void){
    drain_events();
    hb(STG_FEED);
    int n = (int)BLK.len;
    int r = VcAvWrite(P.s, BLK.b, n);
    hb(STG_FEED);
    if(r != n){ lo("VcAvWrite("); lo_i(n); lo(") -> "); lo_i(r); lo(" (vcavsrv gone?)"); lend(LV_ERR); return -1; }
    P.writes++; P.bytes += n;
    P.written_in_pos = IN.in_pos;               /* the block held everything consumed from the input so far */
    if(BLK.pcr_valid){ P.wpcr_valid = 1; P.wpcr = BLK.pcr_last; }
    if(g_loglevel <= LV_DBG){ lo("write #"); lo_i(P.writes); lo(" "); lo_i(n); lo(" bytes, pcr "); lo_t(P.wpcr);
                              lo(", lead "); lo_t(lead45()); lend(LV_DBG); }
    BLK.len = 0; BLK.pcr_valid = 0;
    if(P.writes == SESSION_WARN && O.max_writes == SESSION_XFERS){
        lo_i(P.writes); lo(" writes: approaching the "); lo_i(SESSION_XFERS);
        lo("-transfer vcavsrv session limit; feeding stops at "); lo_i(O.max_writes); lend(LV_WARN);
    }
    return 0;
}
static int start_playback(void){
    /* omplayer mmf_play order: video unmute, PLAY (type 5 -> AVD_Start + AVD_Play; A/V: ack only), [audio unmute,
     * AUDIO PLAY], STREAM_IOCPLAY, video unmute again */
    if(vctl(P.v, VCTL_UNMUTE) < 0){ LOG1(LV_ERR, "unmute failed before PLAY"); return RC_NOAVD; }
    if(vctl(P.v, VCTL_PLAY) < 0){ LOG1(LV_ERR, "VIDEO PLAY failed (if vcavsrv died: dmesg | grep vcavsrv)"); return RC_PLAY; }
    if(P.av){                                      /* A/V: the real start (tdav_start_av + AVD_Start TY=1 + AVD_Play) */
        actl(ACTL_UNMUTE);                         /* server no-op, kept for omplayer's order */
        if(actl(ACTL_PLAY) < 0){ LOG1(LV_ERR, "AUDIO PLAY (A/V start) failed"); return RC_PLAY; }
    }
    { uint8_t pa[0x80]; memset(pa, 0, sizeof pa); pa[0] = 2; ioc(P.s, STREAM_IOCPLAY, pa, 0x80); }
    vctl(P.v, VCTL_UNMUTE);
    P.started = 1; P.t_play = now_ms(); P.el_change_ms = P.t_play;
    LOG1(LV_INFO, P.av ? "PLAY (A/V)" : "PLAY (video only)");
    return 0;
}

/* ======================================================================================================== */
/* main loop helpers                                                                                        */
/* ======================================================================================================== */
/* Wait up to `ms` for input data, a control command or a signal. Signals are only deliverable in here. */
static void wait_events(int want_input, long ms){
    fd_set rf; FD_ZERO(&rf); int maxfd = -1;
    if(want_input){ FD_SET(IN.fd, &rf); maxfd = IN.fd; }
    if(g_ctl_fd >= 0){ FD_SET(g_ctl_fd, &rf); if(g_ctl_fd > maxfd) maxfd = g_ctl_fd; }
    struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };
    int r = pselect(maxfd + 1, &rf, 0, 0, &ts, &g_waitmask);
    if(r <= 0) return;
    if(want_input && FD_ISSET(IN.fd, &rf)) input_read();
    if(g_ctl_fd >= 0 && FD_ISSET(g_ctl_fd, &rf)) ctl_poll();
}

/* Should the current block go out now? Implements PCR pacing + the in-flight gate. */
static int ready_to_write(void){
    if(BLK.len == 0 || P.paused) return 0;
    int full = BLK.len + TS_PKT > BLK.cap;
    int last = input_done();
    long long ld = lead45(), target = (long long)(O.lead * 45000);
    if(P.wpcr_valid && ld >= target) return 0;                  /* far enough ahead: wait, do not write small */
    if(!full && !last){
        /* early (partial) write only when a slow input is about to starve the decoder; each costs a transfer */
        long long low = target / 3 < 45000 ? target / 3 : 45000;
        if(!P.wpcr_valid) return 0;
        if(!((ld < low && BLK.len >= PARTIAL_MIN) || ld < 45000 / 4)) return 0;
    }
    if(O.max_writes - P.writes <= 0) return 0;
    int inf = query_inflight();
    if(inf > O.max_inflight) return 0;                          /* -1 (query failed) falls through: write */
    return 1;
}

int main(int argc, char** argv){
    if(parse_args(argc, argv) < 0) return RC_USAGE;
    signals_setup();
    memset(&P, 0, sizeof P); P.v = P.s = P.au = -1;
    if(O.watchdog == 0) LOG1(LV_INFO, "watchdog disabled");

    /* ---- input + prebuffer (before touching the device, so bad input never routes the monitor) ---- */
    if(!strcmp(O.input, "-")){ IN.fd = 0; IN.is_stdin = 1; }
    else { IN.fd = open(O.input, O_RDONLY | O_CLOEXEC);
           if(IN.fd < 0){ lo("open "); lo(O.input); lo(": "); lo(strerror(errno)); lend(LV_ERR); return RC_INPUT; } }
    IN.r = malloc(RBUF_CAP); BLK.cap = O.block; BLK.b = malloc((size_t)BLOCK_MAX);
    if(!IN.r || !BLK.b){ LOG1(LV_ERR, "out of memory"); return RC_INPUT; }
    P.state = ST_PREBUF;
    TS.track = 0;
    while(!g_sig && BLK.len + TS_PKT <= PREBUF_MAX && !input_done()){
        wait_events(1, 200);
        if(packetize(PREBUF_MAX) < 0) return RC_INPUT;
    }
    if(g_sig){ lo("signal "); lo_i(g_sig); lo(" before playback: nothing to clean up"); lend(LV_INFO); return RC_SIGNAL + g_sig; }
    if(BLK.len == 0){ LOG1(LV_ERR, "no MPEG-TS packets in the input"); return RC_INPUT; }

    /* ---- PIDs and codec: CLI, else PMT, else defaults ---- */
    if(TS.pmt_seen){
        lo("PMT: video PID "); lo_x((unsigned)TS.pmt_vpid); lo(" type "); lo_x((unsigned)TS.pmt_vtype);
        lo(", PCR PID "); lo_x((unsigned)TS.pmt_pcrpid);
        for(int k = 0; k < TS.n_audio; k++){ lo(", audio PID "); lo_x((unsigned)TS.audio[k].pid); lo(" ");
            lo(stream_type_name(TS.audio[k].stype, TS.audio[k].desc)); if(TS.audio[k].acodec < 0) lo(" (not playable)"); }
        lend(LV_INFO);
    } else LOG1(LV_INFO, "no PAT/PMT in the first 2 MB");
    if(O.vpid < 0)   O.vpid   = (TS.pmt_seen && TS.pmt_vpid) ? TS.pmt_vpid : PID_DEFAULT;
    if(O.pcrpid < 0) O.pcrpid = (TS.pmt_seen && TS.pmt_pcrpid && TS.pmt_pcrpid != 0x1fff) ? TS.pmt_pcrpid : PID_DEFAULT;
    if(O.codec < 0){
        if(TS.pmt_seen && stream_type_codec(TS.pmt_vtype) >= 0) O.codec = stream_type_codec(TS.pmt_vtype);
        else { LOG1(LV_ERR, "cannot tell the video codec from the stream: pass --codec"); return RC_USAGE; }
    } else if(TS.pmt_seen && stream_type_codec(TS.pmt_vtype) >= 0 && stream_type_codec(TS.pmt_vtype) != O.codec){
        lo("--codec "); lo(codec_name(O.codec)); lo(" overrides the PMT ("); lo(codec_name(stream_type_codec(TS.pmt_vtype)));
        lo(")"); lend(LV_WARN);
    }
    /* audio: --apid none -> video-only; --apid PID -> that PID; default -> first playable PMT audio track */
    if(O.apid == -1){
        for(int k = 0; k < TS.n_audio && O.apid == -1; k++) if(TS.audio[k].acodec >= 0) O.apid = TS.audio[k].pid;
        if(O.apid == -1 && TS.n_audio){
            lo("no audio track vcavsrv can play (DTS/TrueHD/LPCM need a transcode, e.g. -c:a eac3 -b:a 640k): video only");
            lend(LV_WARN); }
    }
    if(O.apid > 0){
        if(O.acodec < 0) O.acodec = pmt_acodec(O.apid);
        if(O.acodec < 0){ lo("audio PID "); lo_x((unsigned)O.apid); lo(": codec unknown or unplayable; pass --acodec or --apid none");
                          lend(LV_ERR); return RC_USAGE; }
        P.av = 1; P.apid = O.apid; P.acodec = O.acodec;
    }
    TS.vpid = O.vpid; TS.pcrpid = O.pcrpid; TS.track = 1;
    for(long o = 0; o < BLK.len; o += TS_PKT) ts_packet(BLK.b + o);  /* rescan the prebuffer with the final PIDs */
    if(!TS.pcr_first_valid){
        lo("no PCR on PID "); lo_x((unsigned)O.pcrpid); lo(" in the prebuffer");
        if(TS.other_pcr_pid){ lo(" (PCR seen on PID "); lo_x((unsigned)TS.other_pcr_pid); lo(": pass --pcrpid?)"); }
        lo("; pacing falls back to decoder back-pressure until a PCR shows up"); lend(LV_WARN);
    }
    lo("input "); lo(IN.is_stdin ? "stdin" : O.input); lo(": codec "); lo(codec_name(O.codec)); lo(", video PID ");
    lo_x((unsigned)O.vpid); lo(", PCR PID "); lo_x((unsigned)O.pcrpid); lo(", route "); lo(O.route4k ? "4KOTT" : "DIVX");
    lo(", audio "); if(P.av){ lo_x((unsigned)P.apid); lo(" "); lo(acodec_name(P.acodec)); } else lo("none (video-only)");
    lo(", lead "); lo_t((long long)(O.lead * 45000)); lo(" s, block "); lo_i(O.block); lo(", max in flight ");
    lo_i(O.max_inflight); lo(", prebuffer "); lo_i(BLK.len);
    lend(LV_INFO);

    /* ---- watchdog, libraries, MOC route, vcavsrv session ---- */
    if(O.watchdog > 0) watchdog_start();
    ctl_open();
    if(WD) WD->ctl_created = g_ctl_created;
    status_write();
    if(vcav_load() < 0 || moc_load() < 0){ P.rc = RC_LIB; goto teardown; }

    hb(STG_CONNECT);
    if(WD) WD->armed = 1;
    if(env_int("VMOC", 1)){
        if(WD){ WD->pm = O.route4k ? MOC_PLAYMODE_VIERACAST : MOC_PLAYMODE_DIVX; WD->routed = 1; }
        P.routed = 1;
        int rr = moc_route(O.route4k ? AVDEV_SW_VIDEODL : AVDEV_SW_DIVX, O.route4k ? MOC_PLAYMODE_VIERACAST : MOC_PLAYMODE_DIVX);
        if(WD) WD->pm = moc_pm_set;
        if(rr < 0){ LOG1(LV_ERR, "MOC routing failed (nothing opened on vcavsrv)"); P.rc = RC_MOC; goto teardown; }
    } else LOG1(LV_WARN, "VMOC=0: MOC routing skipped (no picture will be visible)");

    hb(STG_CONNECT);
    {   int ctl = VcAvConnect(1);
        if(ctl < 0 && VcAvCreate){
            LOG1(LV_INFO, "spawning vcavsrv");
            sigset_t cur; sigprocmask(SIG_SETMASK, &g_origmask, &cur);   /* the server must not inherit our blocked mask */
            VcAvCreate();
            sigprocmask(SIG_SETMASK, &cur, 0);
            hb(STG_CONNECT); ctl = VcAvConnect(10); }
        hb(STG_CONNECT);
        if(ctl < 0){ LOG1(LV_ERR, "vcavsrv not reachable"); P.rc = RC_CONNECT; goto teardown; }
        P.connected = 1; }

    hb(STG_OPEN);
    /* order matters: video, stream, THEN audio (vcav_av_open pairs the audio OPEN with the video handle) */
    if((P.v = open_video()) < 0 || (P.s = open_stream()) < 0 || (P.au = open_audio()) < 0){ P.rc = RC_OPEN; goto teardown; }
    hb(STG_OPEN);
    if(set_properties() < 0){ P.rc = RC_PROP; goto teardown; }
    /* health probe before any data: unmute needs an AVD channel this vcavsrv really opened. If it fails, an earlier
     * vcavsrv crash left SHMAVD/avccdd state behind and PLAY would SEGV vcavsrv: only a reboot clears it. */
    if(vctl(P.v, VCTL_UNMUTE) < 0){
        LOG1(LV_ERR, "unmute health probe failed: vcavsrv has no open AVD channel (stale state after an earlier "
                     "vcavsrv crash?). REBOOT the player before the next run. Not feeding, not sending PLAY.");
        P.rc = RC_NOAVD; goto teardown; }

    /* ---- prebuffer write, then PLAY (tiny inputs: the whole input, i.e. PLAY after EOF) ---- */
    hb(STG_FEED);
    if(write_block() < 0){ P.rc = RC_WRITE; goto teardown; }
    if((P.rc = start_playback()) != 0) goto teardown;
    P.state = ST_PLAYING;
    BLK.cap = O.block;

    /* ---- main loop ---- */
    {   long long last_poll = 0, last_status = 0, last_log = 0, drain_t0 = 0;
        int warned_underrun = 0, limit_hit = 0;
        const long long halt_ms = 4000, start_ms = 10000 + (long long)(O.lead * 1000), eos_freeze_ms = 1500;
        for(;;){
            long long now = now_ms();
            hb(STG_FEED);
            if(g_sig){ lo("signal "); lo_i(g_sig); lo(": stopping"); lend(LV_INFO); P.rc = RC_SIGNAL + g_sig; P.reason = "signal"; break; }
            if(P.want_stop){ LOG1(LV_INFO, "stop requested"); break; }
            if(now - last_poll >= 50){ poll_clock(now); last_poll = now; }

            if(P.state == ST_PLAYING){
                if(packetize(BLK.cap) < 0){ P.rc = RC_INPUT; break; }
                if(ready_to_write() && write_block() < 0){ P.rc = RC_WRITE; P.reason = "write-failed"; break; }
                if(input_done() && BLK.len == 0){
                    lo("end of input after "); lo_i(P.writes); lo(" writes, "); lo_i(P.bytes); lo(" bytes; draining"); lend(LV_INFO);
                    P.state = ST_DRAINING; drain_t0 = now;
                } else if(P.writes >= O.max_writes){
                    /* TODO(session renewal): a seamless restart needs a way to give the 300 kernel transfer entries
                     * back. Candidates (all UNTESTED, findings/vcav-ts-40mb.md 5d): carrying audio so TDAV retires
                     * them (5d.3), or STREAM 0x8004d61a TermFeed+ClearBuffer re-arm + STREAM_IOCPLAY at in-flight 0
                     * (5d.4, visible glitch). Until then: stop feeding, play out what was delivered, exit RC_LIMIT. */
                    lo("session transfer budget used ("); lo_i(P.writes); lo(" writes): input truncated; resume at input byte ");
                    lo_i(P.written_in_pos); lo(" (PCR "); lo_t(P.wpcr); lo(" s)"); lend(LV_WARN);
                    limit_hit = 1; P.state = ST_DRAINING; drain_t0 = now;
                }
                /* halt / underrun detection (not while paused; timers restart on pause transitions) */
                if(P.state == ST_PLAYING && !P.paused){
                    long long ld = lead45();
                    if(!P.presenting && now - P.t_play > start_ms){
                        lo("decoder never presented a picture ("); lo_i(now - P.t_play); lo(" ms after PLAY, "); lo_i(P.bytes);
                        lo(" bytes written): wrong --codec/--vpid, or no decodable picture");
                        if(P.av){ lo("; A/V mode: is there audio on PID "); lo_x((unsigned)P.apid); lo("? (else --apid none)"); }
                        lend(LV_ERR);
                        P.rc = RC_HALT; P.reason = "no-picture"; break;
                    }
                    if(P.presenting && now - P.el_change_ms > halt_ms){
                        /* pending = data ahead of the clock, or a block ready to go that the gate keeps refusing */
                        int ready = BLK.len > 0 && (BLK.len + TS_PKT > BLK.cap || input_done());
                        if(!P.wpcr_valid || ld > 45000 / 2 || ready){
                            lo("DECODER HALT: elapsed frozen at "); lo_t(P.elapsed); lo(" s for "); lo_i(now - P.el_change_ms);
                            lo(" ms, lead "); lo_t(ld); lo(" s"); if(ready) lo(", a ready block refused by the in-flight gate");
                            lo(" (writes "); lo_i(P.writes); lo(", bytes ");
                            lo_i(P.bytes); lo(", inflight "); lo_i(P.inflight); lo("): stopping cleanly"); lend(LV_ERR);
                            P.rc = RC_HALT; P.reason = "halt"; break;
                        } else if(!warned_underrun){
                            warned_underrun = 1;
                            LOG1(LV_WARN, "UNDERRUN: the display clock caught up with the input (input slower than real time?)");
                        }
                    } else if(now - P.el_change_ms < 500) warned_underrun = 0;
                }
            } else if(P.state == ST_DRAINING && !P.paused){
                uint32_t target = TS.pts_max_valid ? TS.pts_max : TS.pcr_any;
                int have_target = TS.pts_max_valid || TS.pcr_any_valid;
                if(P.presenting && have_target && (int32_t)(P.elapsed - target) >= 0){ LOG1(LV_INFO, "reached the last picture"); break; }
                if(P.presenting && now - P.el_change_ms >= eos_freeze_ms){ LOG1(LV_INFO, "display clock stopped: end of stream"); break; }
                if(!P.presenting && now - P.t_play > start_ms && now - drain_t0 > 1000){
                    LOG1(LV_ERR, "end of input but the decoder never presented a picture"); P.rc = RC_HALT; P.reason = "no-picture"; break; }
            }
            if(now - last_status >= 1000 || P.want_status){ status_write(); last_status = now; P.want_status = 0; }
            if(now - last_log >= 5000){
                last_log = now;
                lo(g_state_name[P.state]); if(P.paused) lo("(paused)"); lo(": elapsed "); lo_t(P.elapsed); lo(" s, lead ");
                if(P.wpcr_valid) lo_t(lead45()); else lo("?"); lo(" s, writes "); lo_i(P.writes); lo(", bytes "); lo_i(P.bytes);
                lo(", inflight "); lo_i(P.inflight); lend(LV_INFO);
            }
            /* read more only while the read buffer has room and the block has not been fully assembled */
            int want_input = P.state == ST_PLAYING && !IN.eof && IN.rlen < RBUF_CAP;
            wait_events(want_input, 20);
            ctl_poll();                                 /* non-blocking; pselect above also wakes on commands */
        }
        if(limit_hit && P.rc == RC_OK) P.rc = RC_LIMIT;
    }

teardown:
    /* balanced, mirrors the omplayer close path; MOC restore last, after vcavsrv released AVD */
    P.state = ST_STOPPING; status_write();
    hb(STG_TEARDOWN);
    if(P.av && P.au >= 0 && P.started) actl(ACTL_STOP);   /* A/V stop; the STREAM reset below repeats it harmlessly */
    if(P.v >= 0) vctl(P.v, VCTL_STOP);
    hb(STG_TEARDOWN);
    if(P.s >= 0){ VcAvIoctl_funcR(P.s, STREAM_IOCRESET, 0, "ub820play", __LINE__); VcAvClose(P.s); }
    if(P.v >= 0) VcAvClose(P.v);
    if(P.au >= 0) VcAvClose(P.au);
    hb(STG_TEARDOWN);
    if(P.connected){
        lo("VcAvCtrl(5 release) -> "); lo_i(VcAvCtrl(VCAV_CTRL_RELEASE)); lend(LV_DBG); hb(STG_TEARDOWN);
        lo("VcAvCtrl(1 resume) -> "); lo_i(VcAvCtrl(VCAV_CTRL_RESUME)); lend(LV_DBG); hb(STG_TEARDOWN);
        VcAvDelete();
    }
    hb(STG_TEARDOWN);
    if(P.routed){ moc_restore(); P.routed = 0; }
    if(WD){ WD->routed = 0; WD->armed = 0; WD->done = 1; }
    ctl_close();
    P.state = ST_DONE; status_write();
    lo("done: rc "); lo_i(P.rc); lo(", "); lo_i(P.writes); lo(" writes, "); lo_i(P.bytes); lo(" bytes, last elapsed ");
    lo_t(P.elapsed); lo(" s"); lend(LV_INFO);
    return P.rc;
}
