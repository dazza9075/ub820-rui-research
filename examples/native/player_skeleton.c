/* UB820: native player SKELETON (Route A: VcAv client -> vcavsrv decode server).
 * STATIC SCAFFOLD. NOT yet fired on the device (live playback needs the user to watch the output).
 * Build: zig cc -target arm-linux-gnueabihf.2.20 player_skeleton.c -o player_skeleton -ldl
 * Usage (future, live): ./player_skeleton <raw_annexb_es_file> <h264|hevc> [width height fps]
 *
 * Everything below comes from static RE of libvcavif / vcavsrv / omplayer.
 * See ~/UB820-cfw/native-access/findings/vcav-ctrl-codec.md (addresses + [C]onfirmed / [I]nferred).
 *
 * Confirmed client API (libvcavif.so.0):
 *   int VcAvConnect(int retries);                 // opens the global control socket /tmp/vcavsrv (call first)
 *   int VcAvOpen(const char *name, int ignored);  // 2nd arg unused (omplayer passes 0x80002); returns a socket fd
 *   int VcAvIoctl_func(int fd, unsigned cmd, void *arg, int argsize);   // argsize <= 0x80, reply copied back
 *   int VcAvIoctl_funcR(int fd, unsigned cmd, int arg);
 *   int VcAvWrite(int fd, void *buf, int len);    // stream fds only; retries itself every 10 ms on -ENOSPC
 *   int VcAvCtrl(int cmd);                         // ONE arg: server-global mode switch (NOT per-stream start)
 *   int VcAvClose(int fd); void VcAvDelete(void);  // Delete closes the control socket
 * Video handles are control-only (write = EINVAL). ES goes to /dev/dtv/streamN in "chunk mode":
 * STREAM_IOCSETCHUNKLIST (chunk sizes summing to len) before every VcAvWrite.
 *
 * Logging: write(2) only (stdout/stderr globals crash at load on ld-2.20).
 */
#define _GNU_SOURCE
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdlib.h>
#include <stdint.h>
#include <errno.h>
#include <dlfcn.h>
#include <sys/select.h>

static void w(const char*s){ write(2,s,strlen(s)); }
static void wi(long v){ char b[24]; int i=23; unsigned long u; int n=v<0; b[i]=0;
    u = n ? (unsigned long)(-v) : (unsigned long)v; if(!u) b[--i]='0';
    while(u){ b[--i]=(char)('0'+u%10); u/=10; } if(n) b[--i]='-'; w(b+i); }
static void wx(unsigned v){ static const char H[]="0123456789abcdef"; char b[11]; b[0]='0'; b[1]='x';
    for(int i=0;i<8;i++) b[2+i]=H[(v>>(28-4*i))&15]; b[10]=0; w(b); }
#define LOG(msg,val) do{ w(msg); wi(val); w("\n"); }while(0)

/* ---------------- VcAv control commands: VcAvCtrl(cmd), vcavsrv doVcAvCtrl/ChangeMode [C] ------------ */
enum {
    VCAV_CTRL_RESUME          = 1,  /* susp=0 only */
    VCAV_CTRL_SUSPEND_A       = 2,  /* susp=1, stop+close all AVD */
    VCAV_CTRL_SUSPEND_B       = 3,  /* same as 2 */
    VCAV_CTRL_RESUME_JUKEBOX  = 4,  /* susp=0, open jukebox/ExtPCM AVD (type 0x20) */
    VCAV_CTRL_RELEASE         = 5,  /* susp=1, close TDAV, stop+close all AVD   (omplayer close) */
    VCAV_CTRL_ACQUIRE         = 6,  /* susp=0, open jukebox AVD + TDAV a/v      (omplayer open)  */
    VCAV_CTRL_SPEC0           = 7,  /* spec=0 */
    VCAV_CTRL_SPEC1           = 8,  /* spec=1 */
    VCAV_CTRL_REPORT_OFF      = 9,  /* rep=0 + MOC report */
    VCAV_CTRL_REPORT_ON       = 10, /* rep=1 */
    VCAV_CTRL_VMODE2          = 11, /* vmode=2 (same var as VIDEO_IOCOPENQRY arg[3]) */
    VCAV_CTRL_REINIT_FS       = 12, /* AVD_InitFS(jukebox,8) */
};

/* ---------------- device ioctls (sent via VcAvIoctl_func) [C] -------------------------------------- */
#define VIDEO_IOCOPENQRY        0x8054d601u /* 0x54: w0=no(out) w1=prio w2=req_no(1) w3=vmode(1|2) b0x10=2 */
#define VIDEO_IOCCONTROL        0x8008d602u /* 8:  {u32 sub, u32 param} */
#define VIDEO_IOCSETPROPERTY    0x8044d603u /* 0x44 */
#define VIDEO_IOCGETPROPERTY    0x4044d604u /* 0x44 */
#define VIDEO_IOCWAITSTATE      0x4004d605u /* 4: client-local read of a server push; 2=AVAILABLE 1=LOST */
#define STREAM_IOCOPENQRY       0x4004d601u /* 0x48: w0=no(out) w1=ch b0xa=2 b0xb=flag */
#define STREAM_IOCSETPROPERTY   0x8004d602u /* 0x40: b0 = stream type */
#define STREAM_IOCGETPROPERTY   0x4004d603u /* 0x40 */
#define STREAM_IOCPLAY          0x8004d606u /* 0x80: b0 = 2 (omplayer mmf_stream_play) */
#define STREAM_IOCRESET         0x0000d608u /* via VcAvIoctl_funcR(fd, cmd, 0); after open and at close */
#define STREAM_IOCSETCHUNKLIST  0x8004d60bu /* 0x28 */
#define STREAM_IOCSTATCHUNK     0x8004d60du /* 8: returns {0x46800, 1} */
#define STREAM_IOCGETWRITESIZE  0x8004d61bu /* 8: u64 bytes accepted */

/* VIDEO_IOCCONTROL sub-commands [C] */
enum { VCTL_PLAY=0, VCTL_STOP=1, VCTL_MUTE=2, VCTL_UNMUTE=3, VCTL_PAUSE_ON=4, VCTL_PAUSE_OFF=5,
       VCTL_SYNC_OFF=6, VCTL_SYNC_ON=7 };
#define VCTL_STOP_KEEP_LAST 0x100   /* OR into VCTL_STOP: don't clear the output (omplayer uses 0x301) */

/* vcav video codec ids (VIDEO property +0x11) -> AVD enum [C get_avd_video_codec] */
enum { VCAV_VCODEC_MPEG2=0, VCAV_VCODEC_H264=2, VCAV_VCODEC_VC1=3, VCAV_VCODEC_HEVC=4,
       VCAV_VCODEC_VP9=5 };
/* AVD enum (libavd): MPEG2=1 H264=2 VC1=4 HEVC=9 VP9=10 */

#define VPID_DEFAULT 0x1011   /* omplayer default video PID for non-TS media [C] */
#define PCRPID_DEFAULT 0x1001 /* omplayer default PCR PID [C] */
#define STREAM_TYPE_ES 5      /* [I] omplayer uses 3/4/5 for chunked ES; 5 -> feed_chunk_raw directly */
#define CHUNK_TYPE_SIMPLE 1   /* [C] (flags&0xd)==1: sizes array only */

typedef int  (*connect_t)(int);
typedef int  (*open_t)(const char*, int);
typedef int  (*ioctl_t)(int, unsigned, void*, int, const char*, int);  /* trailing pair ignored by lib */
typedef int  (*ioctlr_t)(int, unsigned, int, const char*, int);
typedef int  (*write_t)(int, void*, int);
typedef int  (*ctrl_t)(int);
typedef int  (*close_t)(int);
typedef void (*delete_t)(void);

static connect_t VcAvConnect; static open_t VcAvOpen; static ioctl_t VcAvIoctl_func;
static ioctlr_t VcAvIoctl_funcR; static write_t VcAvWrite; static ctrl_t VcAvCtrl;
static close_t VcAvClose; static delete_t VcAvDelete;

static void put32(uint8_t*p,int off,uint32_t v){ memcpy(p+off,&v,4); }
static void put16(uint8_t*p,int off,uint16_t v){ memcpy(p+off,&v,2); }
static uint32_t get32(const uint8_t*p,int off){ uint32_t v; memcpy(&v,p+off,4); return v; }

static int ioc(int fd, unsigned cmd, void* arg, int sz, int line){
    int r = VcAvIoctl_func(fd, cmd, arg, sz, "poc", line);
    w("  ioctl "); wx(cmd); w(" -> "); wi(r); w("\n");
    return r;
}
static int vctl(int vfd, unsigned sub, unsigned param, int line){
    uint32_t a[2] = { sub, param };
    w("  VIDEO_IOCCONTROL sub="); wx(sub); w("\n");
    return ioc(vfd, VIDEO_IOCCONTROL, a, 8, line);
}

/* ---------------- open helpers: exact omplayer decoder_mmf sequences [C] --------------------------- */
static int open_video(int vmode){
    int v0 = VcAvOpen("/dev/dtv/video0", 0x80002);
    LOG("VcAvOpen(video0) -> ", v0);
    if(v0 < 0) return -1;
    uint8_t q[0x54]; memset(q,0,sizeof q);
    put32(q,0x04,0);          /* prio (omplayer: [ctx+8]) TODO(live): any non-default needed? */
    put32(q,0x08,1);          /* requested unit -> server returns it in w0 */
    put32(q,0x0c,(uint32_t)vmode); /* 2 = omplayer "exclusive" mode, 1 = normal */
    q[0x10] = 2;
    int r;
    do { r = ioc(v0, VIDEO_IOCOPENQRY, q, 0x54, __LINE__); } while(r < 0 && errno == EINTR);
    int no = (int)get32(q,0);
    if(r < 0 || no < 0){ VcAvClose(v0); return -1; }
    char name[] = "/dev/dtv/video0"; name[sizeof name - 2] = (char)('0'+no);
    int v = VcAvOpen(name, 0x80002);
    VcAvClose(v0);
    w("VcAvOpen("); w(name); w(") -> "); wi(v); w("\n");
    if(v < 0) return -1;
    /* wait for AVAILABLE (server pushes 4 bytes on the socket; 2=AVAILABLE, 1=LOST) */
    for(int tries=0; tries<50; tries++){
        fd_set rf; FD_ZERO(&rf); FD_SET(v,&rf);
        struct timeval tv = { 0, 200000 };
        int s = select(v+1, &rf, 0, 0, &tv);
        if(s < 0 && errno == EINTR) continue;
        if(s <= 0) continue;
        uint32_t st = 0;
        if(ioc(v, VIDEO_IOCWAITSTATE, &st, 4, __LINE__) < 0) break;
        LOG("  video state=", (long)st);
        if(st == 2){ vctl(v, VCTL_STOP, 0, __LINE__); return v; }  /* omplayer sends STOP{1,0} here */
    }
    w("video never became AVAILABLE\n");
    VcAvClose(v); return -1;
}

static int open_stream(void){
    int s0 = VcAvOpen("/dev/dtv/stream0", 0x80002);
    LOG("VcAvOpen(stream0) -> ", s0);
    if(s0 < 0) return -1;
    uint8_t q[0x48]; memset(q,0,sizeof q);
    put32(q,0x04,0);   /* ch (omplayer [ctx+8]) */
    q[0x0a] = 2;
    q[0x0b] = 0;       /* omplayer: bit3 of its flags */
    int r;
    do { r = ioc(s0, STREAM_IOCOPENQRY, q, 0x48, __LINE__); } while(r < 0 && errno == EINTR);
    int no = (int)get32(q,0);
    if(r < 0 || no < 0){ VcAvClose(s0); return -1; }
    char name[] = "/dev/dtv/stream0"; name[sizeof name - 2] = (char)('0'+no);
    int s = VcAvOpen(name, 0x80002);
    VcAvClose(s0);
    w("VcAvOpen("); w(name); w(") -> "); wi(s); w("\n");
    if(s < 0) return -1;
    LOG("  STREAM reset(0xd608) -> ", VcAvIoctl_funcR(s, STREAM_IOCRESET, 0, "poc", __LINE__));
    return s;
}

/* ---------------- Annex-B access-unit splitter (H.264 / HEVC) ------------------------------------- */
/* Returns the offset of the next AU start after `from`, or len. An AU starts at an AUD, at parameter
 * sets / SEI that follow a VCL NAL, or at a VCL NAL whose first-slice flag is set. */
static int is_hevc;
static int nal_at(const uint8_t*b,int len,int i,int*hdr){   /* start code at i? returns sc length */
    if(i+3<=len && b[i]==0 && b[i+1]==0 && b[i+2]==1){ *hdr=i+3; return 3; }
    if(i+4<=len && b[i]==0 && b[i+1]==0 && b[i+2]==0 && b[i+3]==1){ *hdr=i+4; return 4; }
    return 0;
}
static int next_au(const uint8_t*b,int len,int from){
    int seen_vcl = 0;
    for(int i=from; i<len; i++){
        int h; int sc = nal_at(b,len,i,&h);
        if(!sc || h+2 >= len) continue;
        int t, vcl, first, prefix;
        if(is_hevc){ t=(b[h]>>1)&0x3f; vcl = t<32; first = (b[h+2]&0x80)!=0; prefix = (t>=32&&t<=40); }
        else       { t=b[h]&0x1f;      vcl = (t==1||t==5); first = (b[h+1]&0x80)!=0; prefix = (t>=6&&t<=9); }
        if(i > from){
            if(is_hevc ? t==35 : t==9) return i;            /* AUD */
            if(seen_vcl && prefix) return i;                 /* SPS/PPS/SEI after picture data */
            if(seen_vcl && vcl && first) return i;           /* first slice of next picture */
        }
        if(vcl) seen_vcl = 1;
        i = h;
    }
    return len;
}

/* STREAM_IOCSETCHUNKLIST + VcAvWrite for `n` chunks laid out back-to-back in buf [C] */
static int write_chunks(int s, uint8_t* buf, uint32_t* sizes, int n, uint32_t pts, uint32_t step){
    uint8_t cl[0x28]; memset(cl,0,sizeof cl);
    uint32_t total = 0; for(int i=0;i<n;i++) total += sizes[i];
    put32(cl,0x00,CHUNK_TYPE_SIMPLE);       /* +0 flags (bit8 = header-chunk count at +0x19 valid) */
    put32(cl,0x04,(uint32_t)(uintptr_t)sizes); /* +4 u32* sizes */
    put32(cl,0x08,(uint32_t)n);             /* +8 count (<=1000) */
    put32(cl,0x0c,pts);                     /* +0xc first PTS  [I] 90 kHz */
    put32(cl,0x10,step);                    /* +0x10 PTS step  [I] */
    if(VcAvIoctl_func(s, STREAM_IOCSETCHUNKLIST, cl, 0x28, "poc", __LINE__) < 0){ w("SETCHUNKLIST failed\n"); return -1; }
    int r = VcAvWrite(s, buf, (int)total);  /* blocks/retries on -ENOSPC inside libvcavif */
    if(r != (int)total){ LOG("VcAvWrite short/failed: ", r); return -1; }
    return r;
}

#define MAX_WRITE   (128*1024)   /* keep < server buffer (STATCHUNK says 0x46800); a bigger write retries forever */
#define MAX_CHUNKS  64

int main(int argc, char** argv){
    if(argc<3){ w("usage: player_skeleton <annexb_es> <h264|hevc> [width height fps]\n"); return 1; }
    is_hevc = (argv[2][0]=='h' && argv[2][1]=='e');
    int width  = argc>3 ? atoi(argv[3]) : 1920;
    int height = argc>4 ? atoi(argv[4]) : 1080;
    int fps    = argc>5 ? atoi(argv[5]) : 24;
    uint32_t step = (uint32_t)(90000 / (fps>0?fps:24));   /* [I] PTS units */

    /* libvcavif needs pthread_sigmask etc. but doesn't DT_NEED libpthread on this glibc: preload it globally */
    if(!dlopen("libpthread.so.0", RTLD_NOW|RTLD_GLOBAL)){ w("dlopen libpthread FAIL: "); w(dlerror()); w("\n"); return 2; }
    void* h = dlopen("libvcavif.so.0", RTLD_NOW|RTLD_GLOBAL);
    if(!h) h = dlopen("/usr_root/lib/libvcavif.so.0", RTLD_LAZY|RTLD_GLOBAL);
    if(!h){ w("dlopen libvcavif FAIL: "); w(dlerror()); w("\n"); return 2; }
    VcAvConnect    =(connect_t)dlsym(h,"VcAvConnect");
    VcAvOpen       =(open_t)   dlsym(h,"VcAvOpen");
    VcAvIoctl_func =(ioctl_t)  dlsym(h,"VcAvIoctl_func");
    VcAvIoctl_funcR=(ioctlr_t) dlsym(h,"VcAvIoctl_funcR");
    VcAvWrite      =(write_t)  dlsym(h,"VcAvWrite");
    VcAvCtrl       =(ctrl_t)   dlsym(h,"VcAvCtrl");
    VcAvClose      =(close_t)  dlsym(h,"VcAvClose");
    VcAvDelete     =(delete_t) dlsym(h,"VcAvDelete");
    if(!VcAvConnect||!VcAvOpen||!VcAvIoctl_func||!VcAvIoctl_funcR||!VcAvWrite||!VcAvCtrl||!VcAvClose||!VcAvDelete){
        w("missing VcAv syms\n"); return 3; }

    /* load the ES file */
    int fd = open(argv[1], O_RDONLY);
    if(fd<0){ w("open es file failed\n"); return 4; }
    long cap = 1<<20, len = 0; uint8_t* es = malloc((size_t)cap); int n;
    while(es && (n=(int)read(fd, es+len, (size_t)(cap-len)))>0){ len+=n; if(len==cap){ cap*=2; es=realloc(es,(size_t)cap);} }
    close(fd);
    if(!es || len<=0){ w("empty es\n"); return 4; }
    LOG("ES bytes=", len);

    /* 1. control socket + server mode (omplayer "exclusive" open sequence) */
    int ctl = VcAvConnect(1);
    if(ctl < 0){ typedef int (*create_t)(void); create_t VcAvCreate=(create_t)dlsym(h,"VcAvCreate");
        if(VcAvCreate){ LOG("VcAvCreate (spawn vcavsrv) -> ", VcAvCreate()); ctl = VcAvConnect(10); } }
    LOG("VcAvConnect -> ", ctl);
    if(ctl < 0){ w("vcavsrv not reachable (is it running? VcAvCreate() would spawn it)\n"); return 5; }
    LOG("VcAvCtrl(11 vmode2) -> ", VcAvCtrl(VCAV_CTRL_VMODE2));
    LOG("VcAvCtrl(6 acquire) -> ", VcAvCtrl(VCAV_CTRL_ACQUIRE));   /* resume + open TDAV/jukebox */

    int v = -1, s = -1, rc = 0;
    /* 2. handles */
    v = open_video(2);
    if(v < 0){ rc = 6; goto out; }
    s = open_stream();
    if(s < 0){ rc = 7; goto out; }

    /* 3. stream property: GET, set type, SET */
    { uint8_t sp[0x40]; memset(sp,0,sizeof sp);
      ioc(s, STREAM_IOCGETPROPERTY, sp, 0x40, __LINE__);
      sp[0] = STREAM_TYPE_ES;  /* chunked ES */
      sp[1] = 0;               /* mode (omplayer 0x10 or 0) */
      if(ioc(s, STREAM_IOCSETPROPERTY, sp, 0x40, __LINE__) < 0){ rc = 8; goto out; } }

    /* 4. video property: GET, set type/pid/codec/size, SET */
    { uint8_t vp[0x44]; memset(vp,0,sizeof vp);
      ioc(v, VIDEO_IOCGETPROPERTY, vp, 0x44, __LINE__);
      vp[0x00] = 5;                                   /* video-only: start is driven by VIDEO PLAY */
      put16(vp,0x02,VPID_DEFAULT);
      put16(vp,0x06,PCRPID_DEFAULT);
      vp[0x10] = 5;                                   /* omplayer: 5 (or 6 if its flag bit3) */
      vp[0x11] = is_hevc ? VCAV_VCODEC_HEVC : VCAV_VCODEC_H264;
      put32(vp,0x18,(uint32_t)height);
      put32(vp,0x1c,(uint32_t)width);
      put32(vp,0x24,1000);                            /* pixel aspect (x1000) */
      put32(vp,0x28,(uint32_t)(fps*1000));            /* [I] rate x1000 (omplayer default 0x6d38) */
      if(ioc(v, VIDEO_IOCSETPROPERTY, vp, 0x44, __LINE__) < 0){ rc = 9; goto out; } }

    /* 5. feed + start. Prebuffer the first write, then unmute, PLAY (-> AVD_Start/AVD_Play), sync, stream play. */
    static uint32_t sizes[MAX_CHUNKS];
    long off = 0; uint32_t pts = 0; int started = 0; long fed = 0;
    while(off < len){
        int nch = 0; long base = off, cur = off;
        while(cur < len && nch < MAX_CHUNKS){
            long nx = next_au(es, (int)len, (int)cur);
            if(nch>0 && (nx-base) > MAX_WRITE) break;
            if(nx-cur > MAX_WRITE){ w("WARNING: single AU > MAX_WRITE (may retry forever)\n"); }
            sizes[nch++] = (uint32_t)(nx-cur); cur = nx;
        }
        if(write_chunks(s, es+base, sizes, nch, pts, step) < 0){ rc = 10; break; }
        pts += step*(uint32_t)nch; fed += cur-base; off = cur;
        if(!started){
            vctl(v, VCTL_UNMUTE, 0, __LINE__);
            vctl(v, VCTL_PLAY, 0, __LINE__);          /* server: type==5 -> do_avd_start -> AVD_Start, AVD_Play */
            vctl(v, VCTL_SYNC_ON, 0, __LINE__);
            { uint8_t pa[0x80]; memset(pa,0,sizeof pa); pa[0]=2; ioc(s, STREAM_IOCPLAY, pa, 0x80, __LINE__); }
            { uint32_t st[2]={0,0}; ioc(s, STREAM_IOCSTATCHUNK, st, 8, __LINE__);
              LOG("  STATCHUNK w0=", (long)st[0]); LOG("  STATCHUNK w1=", (long)st[1]); }
            started = 1;
        }
    }
    LOG("fed ES bytes=", fed);
    for(int i=0;i<30;i++) usleep(100000);   /* let it drain (TODO: EOS / GETWRITESIZE polling) */

out:
    /* 6. teardown: balanced, mirrors omplayer close path */
    if(v >= 0) vctl(v, VCTL_STOP, 0, __LINE__);
    if(s >= 0){ VcAvIoctl_funcR(s, STREAM_IOCRESET, 0, "poc", __LINE__); VcAvClose(s); }
    if(v >= 0) VcAvClose(v);
    LOG("VcAvCtrl(5 release) -> ", VcAvCtrl(VCAV_CTRL_RELEASE));
    LOG("VcAvCtrl(1 resume)  -> ", VcAvCtrl(VCAV_CTRL_RESUME));   /* [I] restore boot state susp=0 */
    VcAvDelete();
    free(es);
    LOG("done rc=", rc);
    return rc;
}
