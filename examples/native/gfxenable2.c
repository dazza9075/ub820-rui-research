#define _GNU_SOURCE
#define _LARGEFILE64_SOURCE
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdint.h>
#include <sys/mman.h>
#include <dlfcn.h>
static void w(const char*s){ write(1,s,strlen(s)); }
static const char HX[]="0123456789abcdef";
static void wi(int v){ char b[16]; int i=15; if(v==0){w("0");return;} int neg=v<0; unsigned u=neg?-v:v; b[i--]=0; while(u){b[i--]=HX[u%10];u/=10;} if(neg)b[i--]='-'; write(1,b+i+1,strlen(b+i+1)); }
#define W 1920
#define H 1080
#define LEN 0x7E9000UL
static void draw(volatile uint32_t* p){
    if(!p) return;
    for(int i=0;i<W*H;i++) p[i]=0x00000000;
    int x0=560,y0=340,bw=800,bh=400;
    for(int y=y0;y<y0+bh;y++) for(int x=x0;x<x0+bw;x++){ int bdr=(x<x0+8||x>=x0+bw-8||y<y0+8||y>=y0+bh-8);
        p[(long)y*W+x]= bdr?0xFFFFFFFFu:0xE000C0FFu; }
    for(int y=40;y<120;y++) for(int x=40;x<W-40;x++) p[(long)y*W+x]=0xFFFF20FFu;
}
typedef int (*open_t)(int);
typedef int (*setaddr_t)(unsigned,void*,int);
typedef int (*disp_t)(int,int,int);
typedef int (*alpha_t)(unsigned,void*,int);
int main(void){
    int fd=open("/dev/exmem",O_RDWR|O_SYNC);
    if(fd<0){ w("[g2] exmem fail\n"); return 1; }
    unsigned long long offs[2]={0xe1ff0000ULL,0xe27dc000ULL};
    for(int b=0;b<2;b++){ void* m=mmap64(NULL,LEN,PROT_READ|PROT_WRITE,MAP_SHARED,fd,(off64_t)offs[b]);
        if(m!=MAP_FAILED){ draw((volatile uint32_t*)m); munmap(m,LEN); } }
    close(fd); w("[g2] content written\n");
    void* a=dlopen("/usr/dtvrec/lib/libavio.so",RTLD_LAZY|RTLD_GLOBAL);
    if(!a){ w("[g2] dlopen fail\n"); return 2; }
    open_t    Open=(open_t)dlsym(a,"AVIO_UP_Open");
    setaddr_t SetAddr=(setaddr_t)dlsym(a,"AVIO_UP_GFX_Set_Addr");
    disp_t    Disp=(disp_t)dlsym(a,"AVIO_UP_GFX_Disp");
    alpha_t   Alpha=(alpha_t)dlsym(a,"AVIO_UP_GFX_Set_OSDAlphaPlane");
    if(Open){ int r=Open(0); w("[g2] Open="); wi(r); w("\n"); }
    /* desc = {mode, front_phys(128-aligned), mode, back_phys} for plane_tbl_idx 7 (LOSD0) */
    unsigned desc[4]={0, 0xe1ff0000u, 0, 0xe27dc000u};
    if(SetAddr){ int r=SetAddr(7, desc, 0); w("[g2] Set_Addr(7,...)="); wi(r); w("\n"); }
    else w("[g2] no Set_Addr\n");
    /* replicate LOSD1 alpha words {0,4,0} */
    unsigned acfg[3]={0,4,0};
    if(Alpha){ int r=Alpha(7, acfg, 0); w("[g2] Set_OSDAlphaPlane(7,...)="); wi(r); w("\n"); }
    if(Disp){ int r=Disp(1,0,0); w("[g2] Disp(1,0,0)="); wi(r); w("\n"); }
    w("[g2] done — check TV\n");
    return 0;
}
