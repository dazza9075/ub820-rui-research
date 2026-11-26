/* UB820 — native player SKELETON (Route A: VcAv via the vcavsrv decode server).
 * STATIC SCAFFOLD ONLY — NOT yet fired (live playback needs the user to watch the output plane).
 * Structure + API cribbed from omplayer. Values marked TODO must be confirmed against vcavsrv before
 * live use. Build: zig cc -target arm-linux-gnueabihf.2.20 player_skeleton.c -o player_skeleton -ldl
 * Usage (future, live): ./player_skeleton <raw_h264_or_hevc_es_file>
 *
 * Confirmed from omplayer disasm:
 *   handle = VcAvOpen(const char *name, int mode);   // name/mode from omplayer literal pool (TODO resolve)
 *   VcAvConnect(handle, ...);
 *   VcAvWrite(handle, void *es, int len);            // feed ES bytes (behind the socket this write()s to the decoder)
 *   VcAvCtrl(int cmd, ...);                           // cmd in r0. Seen: 5,7,8,9,11 (state/stop). START cmd = TODO
 *   VcAvClose(handle); VcAvDelete(handle);
 *   int VcavNameToType(const char *name);
 * libvcavif talks to /tmp/vcavsrv (vcavsrv forks on demand) which runs the AVD_/AVIO_/MOC decode+plane path.
 */
#define _GNU_SOURCE
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdlib.h>
#include <dlfcn.h>
static void w(const char*s){ write(2,s,strlen(s)); }
static const char HX[]="0123456789abcdef";
static void wi(int v){ char b[16]; int i=15; if(v==0){w("0");return;} int n=v<0;unsigned u=n?-v:v;b[i--]=0;while(u){b[i--]=HX[u%10];u/=10;}if(n)b[i--]='-';write(2,b+i+1,strlen(b+i+1)); }

typedef int  (*nametotype_t)(const char*);
typedef int  (*open_t)(const char*, int);
typedef int  (*connect_t)(int, int);
typedef int  (*write_t)(int, void*, int);
typedef int  (*ctrl_t)(int, int, int);
typedef int  (*close_t)(int);

int main(int argc, char** argv){
    if(argc<2){ w("usage: player_skeleton <es_file>\n"); return 1; }
    void* h = dlopen("libvcavif.so.0", RTLD_LAZY|RTLD_GLOBAL);
    if(!h) h = dlopen("/usr/dtvrec/lib/libvcavif.so.0", RTLD_LAZY|RTLD_GLOBAL);
    if(!h){ w("dlopen libvcavif FAIL: "); w(dlerror()); w("\n"); return 2; }
    nametotype_t VcavNameToType=(nametotype_t)dlsym(h,"VcavNameToType");
    open_t    VcAvOpen   =(open_t)dlsym(h,"VcAvOpen");
    connect_t VcAvConnect=(connect_t)dlsym(h,"VcAvConnect");
    write_t   VcAvWrite  =(write_t)dlsym(h,"VcAvWrite");
    ctrl_t    VcAvCtrl   =(ctrl_t)dlsym(h,"VcAvCtrl");
    close_t   VcAvClose  =(close_t)dlsym(h,"VcAvClose");
    close_t   VcAvDelete =(close_t)dlsym(h,"VcAvDelete");
    if(!VcAvOpen||!VcAvWrite){ w("missing VcAv syms\n"); return 3; }

    /* TODO(confirm from vcavsrv): the stream name + mode. omplayer passes a device/type string + small int.
     * Likely a "/dev/dtv/video0"-family name or a VcavNameToType input. */
    const char* name = "video0";   /* TODO */
    int mode = 0;                  /* TODO */
    int handle = VcAvOpen(name, mode);
    w("VcAvOpen -> handle="); wi(handle); w("\n");
    if(handle < 0){ w("open failed\n"); return 4; }
    if(VcAvConnect) VcAvConnect(handle, 0);

    /* TODO(confirm): the START ctrl command. Stop/state cmds seen = 5,7,8,9,11; the start/play cmd is TODO.
     * if(VcAvCtrl) VcAvCtrl(START_CMD, handle, 0); */

    /* feed the elementary stream, paced. (Proper pacing: query decoder free space; here: fixed chunks.) */
    int fd = open(argv[1], O_RDONLY);
    if(fd<0){ w("open es file failed\n"); return 5; }
    static unsigned char buf[65536];
    long total=0; int n;
    while((n=read(fd, buf, sizeof buf))>0){
        int off=0;
        while(off<n){
            int wr = VcAvWrite(handle, buf+off, n-off);
            if(wr<=0){ usleep(5000); continue; }  /* backpressure: retry (TODO: use free-space query) */
            off += wr; total += wr;
        }
    }
    close(fd);
    w("fed ES bytes="); wi((int)total); w("\n");

    /* let it drain, then teardown (balanced) */
    for(int i=0;i<40;i++) usleep(100000);
    /* if(VcAvCtrl) VcAvCtrl(5, handle, 0);  // stop (cmd 5 seen in omplayer teardown) */
    if(VcAvClose) VcAvClose(handle);
    if(VcAvDelete) VcAvDelete(handle);
    w("done (skeleton). NOT a finished player — see native-player-plan.md.\n");
    return 0;
}
