/* ovl: persistent overlay daemon (option 1: redraw into gserver's own UI buffers).
   Holds ONE AVIO_UP_Open for its lifetime (Close on exit/SIGTERM/SIGINT/timeout). Polls Get_Addr(7)
   every 10ms; whenever the front buffer changes OR our banner pixels were overwritten, redraws banner
   into the current front buffer via /dev/exmem. Never calls Set_Addr. Auto-exits after MAXSEC.
   Usage: ovl [seconds]   (default 300) */
#define _GNU_SOURCE
#define _LARGEFILE64_SOURCE
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdint.h>
#include <signal.h>
#include <stdlib.h>
#include <time.h>
#include <sys/mman.h>
#include <dlfcn.h>
static void w(const char*s){ write(1,s,strlen(s)); }
static void wx(unsigned v){ char b[11]="0x00000000"; const char*H="0123456789abcdef"; for(int i=0;i<8;i++) b[9-i]=H[(v>>(4*i))&15]; w(b); }
static void wd(long v){ char b[24]; int i=23; b[i]=0; unsigned long u=v<0?-v:v; do{b[--i]='0'+u%10;u/=10;}while(u); if(v<0)b[--i]='-'; w(b+i); }
#define W 1920
#define H 1080
#define LEN 0x7E9000UL
#define BUSOFF 0xC0000000ULL
#define BY0 40
#define BY1 110
#define BX0 60
#define BX1 (W-60)
#define MAG 0xFFFF00FFu
#define WHT 0xFFFFFFFFu
static volatile int stop=0;
static void onsig(int s){ (void)s; stop=1; }
struct map { unsigned bus; uint32_t* p; } maps[4]; int nmaps=0; int fd;
static uint32_t* getmap(unsigned bus){
  for(int i=0;i<nmaps;i++) if(maps[i].bus==bus) return maps[i].p;
  if(nmaps==4){ munmap(maps[0].p,LEN); memmove(maps,maps+1,3*sizeof maps[0]); nmaps=3; }
  uint32_t* p=mmap64(NULL,LEN,PROT_READ|PROT_WRITE,MAP_SHARED,fd,(off64_t)(bus+BUSOFF));
  if(p==MAP_FAILED){ w("mmap fail "); wx(bus); w("\n"); return 0; }
  maps[nmaps].bus=bus; maps[nmaps].p=p; nmaps++; w("mapped "); wx(bus); w("\n"); return p;
}
static void draw(uint32_t* p, int phase){
  for(int y=BY0;y<BY1;y++){ uint32_t* row=p+y*W; for(int x=BX0;x<BX1;x++){
    int b=(y<BY0+5||y>=BY1-5||x<BX0+5||x>=BX1-5); row[x]=b?WHT:MAG; } }
  /* small green "alive" block that shifts each redraw so the user can see redraws happen */
  int gx=BX0+20+(phase%20)*80; for(int y=BY0+15;y<BY1-15;y++) for(int x=gx;x<gx+60;x++) p[y*W+x]=0xFF00FF00u;
}
static int intact(uint32_t* p){
  return p[(BY0+2)*W+BX0+2]==WHT && p[(BY0+30)*W+BX1-10]==MAG && p[(BY1-2)*W+W/2]==WHT && p[(BY0+30)*W+BX0+10]==MAG;
}
int main(int argc,char**argv){
  long maxsec = argc>1 ? atol(argv[1]) : 300; if(maxsec<1||maxsec>3600) maxsec=300;
  signal(SIGTERM,onsig); signal(SIGINT,onsig); signal(SIGHUP,onsig);
  void*a=dlopen("/usr/dtvrec/lib/libavio.so",RTLD_LAZY|RTLD_GLOBAL); if(!a){w("dlopen fail\n");return 1;}
  int(*Open)(int)=dlsym(a,"AVIO_UP_Open"); int(*Close)(void)=dlsym(a,"AVIO_UP_Close");
  int(*Get)(unsigned,unsigned*)=dlsym(a,"_AVIO_UP_GFX_Get_Addr");
  if(!Open||!Close||!Get){w("dlsym fail\n");return 2;}
  fd=open("/dev/exmem",O_RDWR|O_SYNC); if(fd<0){ w("exmem fail\n"); return 3; }
  int r=Open(0); w("Open="); wd(r); w("\n"); if(r!=0) return 4;
  time_t t0=time(0); unsigned last=0; long redraws=0, flips=0; int phase=0;
  while(!stop && time(0)-t0<maxsec){
    unsigned d[4]; if(Get(7,d)!=0){ usleep(10000); continue; }
    unsigned bus=d[1];
    if(d[0]<=1 && bus && bus!=0xffffffffu && !(bus&0x7f)){
      uint32_t* p=getmap(bus);
      if(p){ int changed=(bus!=last); if(changed) flips++;
        if(changed || !intact(p)){ draw(p,phase++); redraws++; } last=bus; }
    }
    usleep(10000);
  }
  for(int i=0;i<nmaps;i++) munmap(maps[i].p,LEN); close(fd);
  r=Close(); w("Close="); wd(r); w(" flips="); wd(flips); w(" redraws="); wd(redraws); w(stop?" (signal)\n":" (timeout)\n");
  return 0;
}
