/* clockovl: on-screen clock overlay. Same proven method as ovl.c: hold ONE AVIO_UP_Open, poll
   _AVIO_UP_GFX_Get_Addr(7) every 10ms, draw into gserver's CURRENT front UI buffer via /dev/exmem
   (bus+0xC0000000) when it flips, when our pixels were overwritten, or when the time text changes.
   Never calls Set_Addr. Close on SIGTERM/SIGINT/SIGHUP/timeout.
   Usage: clockovl [seconds(0=forever, default 600)] [utc_offset_minutes | "uk" (GMT/BST auto)]
   Exits (with Close) if gserver disappears (standby) so we never hold AVIO across a UI teardown. */
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
#include <dirent.h>
#include <sys/types.h>
static void w(const char*s){ write(1,s,strlen(s)); }
static void wx(unsigned v){ char b[11]="0x00000000"; const char*H="0123456789abcdef"; for(int i=0;i<8;i++) b[9-i]=H[(v>>(4*i))&15]; w(b); }
static void wd(long v){ char b[24]; int i=23; b[i]=0; unsigned long u=v<0?-v:v; do{b[--i]='0'+u%10;u/=10;}while(u); if(v<0)b[--i]='-'; w(b+i); }
#define W 1920
#define H 1080
#define LEN 0x7E9000UL
#define BUSOFF 0xC0000000ULL
/* 5x7 glyphs, rows top->bottom, bit4 = leftmost */
static const unsigned char G[11][7]={
 {0x0E,0x11,0x13,0x15,0x19,0x11,0x0E},{0x04,0x0C,0x04,0x04,0x04,0x04,0x0E},{0x0E,0x11,0x01,0x02,0x04,0x08,0x1F},
 {0x1F,0x02,0x04,0x02,0x01,0x11,0x0E},{0x02,0x06,0x0A,0x12,0x1F,0x02,0x02},{0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E},
 {0x06,0x08,0x10,0x1E,0x11,0x11,0x0E},{0x1F,0x01,0x02,0x04,0x08,0x08,0x08},{0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E},
 {0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C},{0x00,0x0C,0x0C,0x00,0x0C,0x0C,0x00}};
#define S 6                 /* pixel scale: glyph 30x42 */
#define GW (5*S)
#define GH (7*S)
#define ADV (GW+S*2)        /* glyph advance */
#define PAD 18
#define NCH 8               /* HH:MM:SS */
#define BOXW (PAD*2+NCH*ADV-S*2)
#define BOXH (PAD*2+GH)
#define BX0 (W-60-BOXW)
#define BY0 40
#define BG  0xFF101820u
#define EDGE 0xFF3A86FFu
#define FG  0xFFFFFFFFu
static volatile int stop=0;
static void onsig(int s){ (void)s; stop=1; }
struct map { unsigned bus; uint32_t* p; } maps[4]; static int nmaps=0, fd;
static uint32_t* getmap(unsigned bus){
  for(int i=0;i<nmaps;i++) if(maps[i].bus==bus) return maps[i].p;
  if(nmaps==4){ munmap(maps[0].p,LEN); memmove(maps,maps+1,3*sizeof maps[0]); nmaps=3; }
  uint32_t* p=mmap64(NULL,LEN,PROT_READ|PROT_WRITE,MAP_SHARED,fd,(off64_t)(bus+BUSOFF));
  if(p==MAP_FAILED){ w("mmap fail "); wx(bus); w("\n"); return 0; }
  maps[nmaps].bus=bus; maps[nmaps].p=p; nmaps++; w("mapped "); wx(bus); w("\n"); return p;
}
/* UK: BST from last Sunday of March 01:00 UTC to last Sunday of October 01:00 UTC */
static int dow(int y,int m,int d){ static const int t[]={0,3,2,5,0,3,5,1,4,6,2,4}; if(m<3) y--; return (y+y/4-y/100+y/400+t[m-1]+d)%7; }
static long days_from_civil(long y,unsigned m,unsigned d){ y-=m<=2; long era=(y>=0?y:y-399)/400; unsigned yoe=(unsigned)(y-era*400);
  unsigned doy=(153*(m+(m>2?-3:9))+2)/5+d-1; unsigned doe=yoe*365+yoe/4-yoe/100+doy; return era*146097+(long)doe-719468; }
static long uk_offset(long t){ long days=t/86400; long y=1970+days/366; while(days_from_civil(y+1,1,1)<=days) y++;
  int ls_mar=31-dow(y,3,31), ls_oct=31-dow(y,10,31);
  long a=days_from_civil(y,3,ls_mar)*86400+3600, b=days_from_civil(y,10,ls_oct)*86400+3600;
  return (t>=a && t<b)?60:0; }
static int uk_mode=0;
static int find_pid(const char* name){ DIR* d=opendir("/proc"); if(!d) return -1; struct dirent* e; int found=-1; char path[64], buf[64];
  while((e=readdir(d))){ if(e->d_name[0]<'0'||e->d_name[0]>'9') continue; strcpy(path,"/proc/"); strcat(path,e->d_name); strcat(path,"/comm");
    int f=open(path,O_RDONLY); if(f<0) continue; int n=read(f,buf,sizeof buf-1); close(f); if(n<=0) continue; buf[n]=0; if(buf[n-1]=='\n') buf[n-1]=0;
    if(!strcmp(buf,name)){ found=atoi(e->d_name); break; } }
  closedir(d); return found; }
static void fmt(char* s, long off_min){
  long t=(long)time(0); if(uk_mode) off_min=uk_offset(t); t+=off_min*60; long d=((t%86400)+86400)%86400;
  int h=d/3600, m=(d/60)%60, sec=d%60;
  s[0]='0'+h/10; s[1]='0'+h%10; s[2]=':'; s[3]='0'+m/10; s[4]='0'+m%10; s[5]=':'; s[6]='0'+sec/10; s[7]='0'+sec%10; s[8]=0;
}
static void draw(uint32_t* p, const char* s){
  /* render into a local tile first, then copy rows -> one fast write per row, no partial glyphs visible */
  static uint32_t tile[BOXH][BOXW];
  for(int y=0;y<BOXH;y++) for(int x=0;x<BOXW;x++){ int e=(y<3||y>=BOXH-3||x<3||x>=BOXW-3); tile[y][x]=e?EDGE:BG; }
  for(int c=0;c<NCH;c++){ int gi = s[c]==':' ? 10 : s[c]-'0'; int ox=PAD+c*ADV, oy=PAD;
    for(int r=0;r<7;r++) for(int b=0;b<5;b++) if(G[gi][r]&(0x10>>b))
      for(int yy=0;yy<S;yy++) for(int xx=0;xx<S;xx++) tile[oy+r*S+yy][ox+b*S+xx]=FG; }
  for(int y=0;y<BOXH;y++) memcpy(p+(BY0+y)*W+BX0, tile[y], BOXW*4);
}
static int intact(uint32_t* p){
  return p[(BY0+1)*W+BX0+1]==EDGE && p[(BY0+BOXH-2)*W+BX0+BOXW-2]==EDGE && p[(BY0+BOXH/2)*W+BX0+8]==BG && p[(BY0+1)*W+BX0+BOXW/2]==EDGE;
}
int main(int argc,char**argv){
  long maxsec = argc>1 ? atol(argv[1]) : 600; long off = 0;
  if(argc>2){ if(!strcmp(argv[2],"uk")) uk_mode=1; else off=atol(argv[2]); }
  if(maxsec<0) maxsec=600; if(off<-840||off>840) off=0;
  signal(SIGTERM,onsig); signal(SIGINT,onsig); signal(SIGHUP,onsig);
  void*a=dlopen("/usr/dtvrec/lib/libavio.so",RTLD_LAZY|RTLD_GLOBAL); if(!a){w("dlopen fail\n");return 1;}
  int(*Open)(int)=dlsym(a,"AVIO_UP_Open"); int(*Close)(void)=dlsym(a,"AVIO_UP_Close");
  int(*Get)(unsigned,unsigned*)=dlsym(a,"_AVIO_UP_GFX_Get_Addr");
  if(!Open||!Close||!Get){w("dlsym fail\n");return 2;}
  fd=open("/dev/exmem",O_RDWR|O_SYNC); if(fd<0){ w("exmem fail\n"); return 3; }
  int gpid=find_pid("gserver"); if(gpid<0){ w("gserver not running -> exit\n"); close(fd); return 5; }
  w("gserver pid="); wd(gpid); w(uk_mode?" tz=uk\n":"\n");
  int r=Open(0); w("Open="); wd(r); w(" offset_min="); wd(off); w("\n"); if(r!=0){ close(fd); return 4; }
  time_t t0=time(0); unsigned last=0; long redraws=0, flips=0; char prev[9]="", cur[9];
  time_t lastchk=0; int gone=0;
  while(!stop && (maxsec==0 || time(0)-t0<maxsec)){
    time_t now=time(0); if(now!=lastchk){ lastchk=now; if(kill(gpid,0)!=0){ gone=1; break; } }
    unsigned d[4];
    if(Get(7,d)==0 && d[0]<=1 && d[1] && d[1]!=0xffffffffu && !(d[1]&0x7f)){
      uint32_t* p=getmap(d[1]);
      if(p){ fmt(cur,off); int changed=(d[1]!=last); if(changed) flips++;
        if(changed || strcmp(cur,prev) || !intact(p)){ draw(p,cur); memcpy(prev,cur,9); redraws++; }
        last=d[1]; }
    }
    usleep(10000);
  }
  for(int i=0;i<nmaps;i++) munmap(maps[i].p,LEN); close(fd);
  r=Close(); w("Close="); wd(r); w(" flips="); wd(flips); w(" redraws="); wd(redraws); w(gone?" (gserver gone)\n":stop?" (signal)\n":" (timeout)\n");
  return 0;
}
