/* flip7: point the LIVE UI plane (Set_Addr plane 7) at the free LOSD0 buffer containing a copy of the
   current UI + our banner. No auto-restore: any remote keypress makes gserver flip plane 7 back.
   Encoding confirmed live: desc {sel=1, BUS addr = phys-0xC0000000, 0, 0}. Balanced Open/Close. */
#define _GNU_SOURCE
#define _LARGEFILE64_SOURCE
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdint.h>
#include <sys/mman.h>
#include <dlfcn.h>
static void w(const char*s){ write(1,s,strlen(s)); }
static void wx(unsigned v){ char b[11]="0x00000000"; const char*H="0123456789abcdef"; for(int i=0;i<8;i++) b[9-i]=H[(v>>(4*i))&15]; w(b); }
static void wd(int v){ char b[12]; int i=11; b[i]=0; unsigned u=v<0?-v:v; do{b[--i]='0'+u%10;u/=10;}while(u); if(v<0)b[--i]='-'; w(b+i); }
#define W 1920
#define H 1080
#define LEN 0x7E9000UL
#define LOSD0_PHYS 0xE1FF0000ULL
#define BUSOFF 0xC0000000u
int main(void){
  void*a=dlopen("/usr/dtvrec/lib/libavio.so",RTLD_LAZY|RTLD_GLOBAL); if(!a){w("dlopen fail\n");return 1;}
  int(*Open)(int)=dlsym(a,"AVIO_UP_Open"); int(*Close)(void)=dlsym(a,"AVIO_UP_Close");
  int(*Get)(unsigned,unsigned*)=dlsym(a,"_AVIO_UP_GFX_Get_Addr"); int(*Set)(unsigned,unsigned*,int)=dlsym(a,"AVIO_UP_GFX_Set_Addr");
  if(!Open||!Close||!Get||!Set){w("dlsym fail\n");return 2;}
  int r=Open(0); w("Open="); wd(r); w("\n"); if(r!=0) return 3;
  unsigned cur[4]; Get(7,cur); w("plane7 now: sel="); wd(cur[0]); w(" addr="); wx(cur[1]); w("\n");
  int ok = (cur[0]==1 && (cur[1]==0x21010000u || cur[1]==0x217fc000u));
  if(!ok){ w("unexpected plane7 state -> abort (no write)\n"); Close(); return 4; }
  int fd=open("/dev/exmem",O_RDWR|O_SYNC); if(fd<0){ w("exmem fail\n"); Close(); return 5; }
  uint32_t* src=mmap64(NULL,LEN,PROT_READ,MAP_SHARED,fd,(off64_t)(cur[1]+(unsigned long long)BUSOFF));
  uint32_t* dst=mmap64(NULL,LEN,PROT_READ|PROT_WRITE,MAP_SHARED,fd,(off64_t)LOSD0_PHYS);
  if(src==MAP_FAILED||dst==MAP_FAILED){ w("mmap fail\n"); close(fd); Close(); return 6; }
  memcpy(dst,src,LEN);
  /* window: x 360..1560, y 200..880. left half alpha0+black, right half alpha0+white; 6px opaque yellow frame */
  for(int y=194;y<886;y++) for(int x=354;x<1566;x++){ uint32_t c;
    if(y<200||y>=880||x<360||x>=1560) c=0xFFFFFF00u; else c=(x<960)?0x00000000u:0x00FFFFFFu; dst[y*W+x]=c; }
  munmap(src,LEN); munmap(dst,LEN); close(fd); w("LOSD0 = UI copy + alpha0 window (L black / R white)\n");
  unsigned d[4]={1,(unsigned)(LOSD0_PHYS-BUSOFF),0,0};
  r=Set(7,d,1); w("Set_Addr(7,{1,"); wx(d[1]); w(",0,0},1)="); wd(r); w("\n");
  Get(7,cur); w("plane7 after: sel="); wd(cur[0]); w(" addr="); wx(cur[1]); w("\n");
  r=Close(); w("Close="); wd(r); w("\nRestore: press any nav key (gserver re-flips plane 7)\n");
  return 0;
}
