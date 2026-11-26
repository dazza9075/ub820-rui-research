/* UB820 — free-plane (LOSD0) overlay test. LOSD0 = free ARGB 1920x1080 plane at 0xe1ff0000/0xe27dc000
 * (gserver uses LOSD1, not this). Write a transparent background with an opaque floating box, to BOTH
 * double-buffers. If LOSD0 is composited in the mixer, we get a persistent overlay OVER the live UI with
 * the home screen visible underneath. No vendor calls, no plane-enable (test if already enabled first).
 * Build: zig cc -target arm-linux-gnueabihf.2.20 losd0draw.c -o losd0draw */
#define _GNU_SOURCE
#define _LARGEFILE64_SOURCE
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdint.h>
#include <sys/mman.h>
static void w(const char*s){ write(1,s,strlen(s)); }
#define W 1920
#define H 1080
#define LEN 0x7E9000UL
static void draw(volatile uint32_t* p){
    if(!p) return;
    for(int i=0;i<W*H;i++) p[i]=0x00000000;            /* fully transparent background */
    int x0=560,y0=340,bw=800,bh=400;                   /* centered box */
    for(int y=y0;y<y0+bh;y++) for(int x=x0;x<x0+bw;x++){
        uint32_t c;
        int bdr = (x<x0+8||x>=x0+bw-8||y<y0+8||y>=y0+bh-8);
        c = bdr ? 0xFFFFFFFFu : 0xE000C0FFu;           /* white border, translucent cyan fill */
        p[(long)y*W+x]=c;
    }
    /* a solid magenta bar across the top so it's unmistakably ours */
    for(int y=40;y<120;y++) for(int x=40;x<W-40;x++) p[(long)y*W+x]=0xFFFF20FFu;
}
int main(void){
    w("[l0] open /dev/exmem...\n");
    int fd=open("/dev/exmem",O_RDWR|O_SYNC);
    if(fd<0){ w("[l0] open fail\n"); return 1; }
    unsigned long long offs[2]={0xe1ff0000ULL,0xe27dc000ULL};
    for(int b=0;b<2;b++){
        void* m=mmap64(NULL,LEN,PROT_READ|PROT_WRITE,MAP_SHARED,fd,(off64_t)offs[b]);
        if(m==MAP_FAILED){ w("[l0] mmap fail buf\n"); continue; }
        draw((volatile uint32_t*)m);
        munmap(m,LEN);
        w("[l0] drew LOSD0 buffer\n");
    }
    close(fd);
    w("[l0] done — content written to LOSD0 (persists; no enable call made)\n");
    return 0;
}
