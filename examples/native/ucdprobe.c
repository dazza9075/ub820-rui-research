/* UB820 — ownership probe (SAFE: open+close only, no ioctls, no hardware writes).
 * Tests whether the kernel driver nodes the compositor (gserver) holds are EXCLUSIVE.
 * If a 2nd root process can open /dev/dtvrec/ucd etc., ownership is userspace convention,
 * not kernel-enforced -> root can get in. write(2) only (no stdio globals; those crash at load).
 * Build: zig cc -target arm-linux-gnueabihf.2.20 ucdprobe.c -o ucdprobe */
#define _GNU_SOURCE
#include <string.h>
#include <fcntl.h>
#include <errno.h>
#include <unistd.h>
#include <sys/mman.h>

static void w(const char*s){ write(1,s,strlen(s)); }
static const char HX[]="0123456789";
static void wint(int v){ char b[16]; int i=15; if(v==0){w("0");return;} int neg=v<0; unsigned u=neg?-v:v;
    b[i--]=0; while(u){b[i--]=HX[u%10];u/=10;} if(neg)b[i--]='-'; write(1,b+i+1,strlen(b+i+1)); }

static void tryopen(const char*path){
    w("open "); w(path); w(" ... ");
    int fd=open(path,O_RDWR);
    if(fd<0){ w("FAIL errno="); wint(errno);
        if(errno==EBUSY) w(" (EBUSY=exclusive!)");
        if(errno==EACCES) w(" (EACCES)");
        if(errno==ENOENT) w(" (no node)");
        w("\n"); return; }
    w("OK fd="); wint(fd); w(" (NOT exclusive)\n");
    close(fd);
}

int main(void){
    w("[ucd] second-process open test (gserver already holds these):\n");
    tryopen("/dev/dtvrec/ucd");
    tryopen("/dev/dtv/avout0");
    tryopen("/dev/dtv/avout1");
    tryopen("/dev/dtv/audio0");
    tryopen("/dev/iosc/lavio_bridge");
    /* confirm fb0 is a dummy: open + mmap + read its size via lseek end */
    w("[ucd] fb0 dummy check: ");
    int fb=open("/dev/fb0",O_RDWR);
    if(fb<0){ w("open fail errno="); wint(errno); w("\n"); }
    else {
        off_t end=lseek(fb,0,SEEK_END);
        w("opened, size(lseek END)="); wint((int)end); w(" bytes");
        void*m=mmap(0,4096,PROT_READ,MAP_SHARED,fb,0);
        if(m==MAP_FAILED){ w(" mmap FAIL errno="); wint(errno); }
        else { w(" mmap OK"); munmap(m,4096); }
        w("\n"); close(fb);
    }
    w("[ucd] done (nothing written to any device)\n");
    return 0;
}
