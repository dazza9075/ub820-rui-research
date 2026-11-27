/* iosdprobe: READ-ONLY. AVIO_UP_Open(0) -> _AVIO_UP_GFX_Get_Addr(plane,u32[4]) for planes 0..21 -> AVIO_UP_Close().
   Balanced open/close. No writes to any plane. */
#include <string.h>
#include <unistd.h>
#include <dlfcn.h>
static void w(const char*s){ write(1,s,strlen(s)); }
static void wx(unsigned v){ char b[11]="0x00000000"; const char*H="0123456789abcdef"; for(int i=0;i<8;i++) b[9-i]=H[(v>>(4*i))&15]; w(b); }
static void wd(int v){ char b[12]; int i=11; b[i]=0; unsigned u=v<0?-v:v; do{b[--i]='0'+u%10;u/=10;}while(u); if(v<0)b[--i]='-'; w(b+i); }
int main(void){
  void*a=dlopen("/usr/dtvrec/lib/libavio.so",RTLD_LAZY|RTLD_GLOBAL); if(!a){w("dlopen fail\n");return 1;}
  int(*Open)(int)=dlsym(a,"AVIO_UP_Open"); int(*Close)(void)=dlsym(a,"AVIO_UP_Close");
  int(*Get)(unsigned,unsigned*)=dlsym(a,"_AVIO_UP_GFX_Get_Addr");
  if(!Open||!Close||!Get){w("dlsym fail\n");return 2;}
  int r=Open(0); w("Open="); wd(r); w("\n"); if(r!=0){ w("open failed, not closing\n"); return 3; }
  for(unsigned p=0;p<=21;p++){ unsigned d[4]={0xdeadbeef,0xdeadbeef,0xdeadbeef,0xdeadbeef};
    int g=Get(p,d); w("plane "); wd(p); w(" rc="); wd(g); w(" ");
    for(int i=0;i<4;i++){ wx(d[i]); w(" "); } w("\n"); }
  r=Close(); w("Close="); wd(r); w("\n"); return 0;
}
