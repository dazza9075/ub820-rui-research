/* UB820 — FIRST LIGHT via direct OSD-plane VRAM write (bypass gserver entirely).
 * /dev/exmem (world-writable, = /dev/mem driver) exposes physical OSD VRAM. 0xE1010000 and
 * 0xE17FC000 are LOSD1's front/back buffers, 1920x1080 ARGB8888, stride 7680, 0x7E9000 bytes each
 * (the plane gserver scans out for the home screen). We mmap both and fill them, cycling
 * red->green->blue every ~1.5s for 15s so it's unmistakably our code. gserver may repaint between
 * our writes, so we re-fill at 10Hz. write(2) logging (no stdio globals). NO vendor calls, no registers.
 * Build: zig cc -target arm-linux-gnueabihf.2.20 exmemdraw.c -o exmemdraw */
#define _GNU_SOURCE
#define _LARGEFILE64_SOURCE
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <stdint.h>
#include <sys/mman.h>

static void w(const char*s){ write(1,s,strlen(s)); }
static const char HXD[]="0123456789abcdef";
static void whexp(unsigned long v){ char b[8]; for(int i=0;i<8;i++){ b[7-i]=HXD[v&15]; v>>=4;} w("0x"); write(1,b,8); }
static void wint(long v){ char b[24]; int i=23; if(v==0){w("0");return;} int neg=v<0; unsigned long u=neg?-v:v;
    b[i--]=0; while(u){b[i--]=HXD[u%10];u/=10;} if(neg)b[i--]='-'; write(1,b+i+1,strlen(b+i+1)); }

#define W 1920
#define H 1080
#define PX (W*H)
#define LEN 0x7E9000UL

int main(void){
    w("[ex] up; open /dev/exmem...\n");
    int fd = open("/dev/exmem", O_RDWR|O_SYNC);
    if(fd<0){ w("[ex] open /dev/exmem failed errno="); wint(errno);
        fd=open("/dev/mem", O_RDWR|O_SYNC);
        if(fd<0){ w(" ; /dev/mem also failed errno="); wint(errno); w("\n"); return 1; }
        w(" ; using /dev/mem\n"); }
    else w("[ex] /dev/exmem open\n");

    unsigned long long offs[2] = {0xE1010000ULL, 0xE17FC000ULL};
    volatile uint32_t* p[2] = {0,0};
    for(int b=0;b<2;b++){
        void* m = mmap64(NULL, LEN, PROT_READ|PROT_WRITE, MAP_SHARED, fd, (off64_t)offs[b]);
        if(m==MAP_FAILED){ w("[ex] mmap buf"); wint(b); w(" FAILED errno="); wint(errno); w("\n"); p[b]=0; }
        else { p[b]=(volatile uint32_t*)m; w("[ex] mapped buf"); wint(b); w(" @"); whexp((unsigned long)m); w("\n"); }
    }
    if(!p[0] && !p[1]){ w("[ex] no buffers mapped; abort.\n"); return 2; }

    uint32_t cols[3] = {0xFFFF0000u, 0xFF00FF00u, 0xFF0000FFu}; /* ARGB opaque R,G,B */
    const char* nm[3] = {"RED","GREEN","BLUE"};
    w("[ex] writing cycling colors for 15s — WATCH TV\n");
    int last=-1;
    for(int t=0; t<150; t++){                 /* 150 * 100ms = 15s */
        int ci = (t/15) % 3;                  /* change color every 1.5s */
        if(ci!=last){ w("[ex] color -> "); w(nm[ci]); w("\n"); last=ci; }
        uint32_t c = cols[ci];
        for(int b=0;b<2;b++){ volatile uint32_t* q=p[b]; if(!q) continue;
            for(int i=0;i<PX;i++) q[i]=c; }
        usleep(100000);
    }
    w("[ex] done (gserver will repaint the home screen).\n");
    for(int b=0;b<2;b++) if(p[b]) munmap((void*)p[b], LEN);
    close(fd);
    return 0;
}
