/* UB820 — sweep gserver display indices to find the one composited ON TOP of the home screen.
 * For id 0..4: TiOpenDisplay(id) -> fullscreen solid color -> MapRaised -> UpdateDisplay, hold 8s.
 * Distinct colors so the user can report WHICH id broke through the carousel:
 *   id0=RED id1=GREEN id2=BLUE id3=YELLOW id4=WHITE.
 * Applies the UI's env (from /tmp/ui.env) before dlopen. write(2) logging only.
 * Build: zig cc -target arm-linux-gnueabihf.2.20 tiwinsweep.c -o tiwinsweep -ldl */
#define _GNU_SOURCE
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdlib.h>
#include <dlfcn.h>
static void w(const char*s){ write(1,s,strlen(s)); }
static const char HXD[]="0123456789abcdef";
static void whexp(unsigned long v){ char b[8]; for(int i=0;i<8;i++){ b[7-i]=HXD[v&15]; v>>=4;} w("0x"); write(1,b,8); }
static void wint(long v){ char b[24]; int i=23; if(v==0){w("0");return;} int neg=v<0; unsigned long u=neg?-v:v;
    b[i--]=0; while(u){b[i--]=HXD[u%10];u/=10;} if(neg)b[i--]='-'; write(1,b+i+1,strlen(b+i+1)); }

typedef void* (*open_t)(int); typedef void* (*root_t)(void*);
typedef void* (*cwin_t)(void*,void*,int,int,int,int); typedef void* (*cgc_t)(void*,int);
typedef int (*fg_t)(void*,void*,unsigned); typedef int (*fill_t)(void*,void*,void*,int,int,int,int);
typedef int (*map_t)(void*,void*); typedef int (*upd_t)(void*); typedef int (*close_t)(void*);
typedef int (*depth_t)(int); typedef int (*clear_t)(void*);

int main(void){
    w("[sw] up\n");
    { int f=open("/tmp/ui.env",O_RDONLY); int a=0;
      if(f>=0){ static char eb[131072]; int n=read(f,eb,sizeof eb-1); close(f);
        if(n>0){ eb[n]=0; char* p=eb; while(*p){ char* nl=strchr(p,'\n'); if(nl)*nl=0;
          char* eq=strchr(p,'='); if(eq&&eq>p){*eq=0; setenv(p,eq+1,1); a++;} if(!nl)break; p=nl+1; } } }
      w("[sw] env applied: "); wint(a); w("\n"); }
    void* h=dlopen("libtiwin.so.0",RTLD_LAZY|RTLD_GLOBAL);
    if(!h) h=dlopen("/usr_root/lib/libtiwin.so.0.0.0",RTLD_LAZY|RTLD_GLOBAL);
    if(!h){ w("[sw] dlopen FAIL\n"); return 1; }
    open_t TiOpenDisplay=(open_t)dlsym(h,"TiOpenDisplay");
    close_t TiCloseDisplay=(close_t)dlsym(h,"TiCloseDisplay");
    root_t TiRootWindow=(root_t)dlsym(h,"TiRootWindow");
    cwin_t TiCreateWindow=(cwin_t)dlsym(h,"TiCreateWindow");
    cgc_t TiCreateGC=(cgc_t)dlsym(h,"TiCreateGC");
    fg_t TiSetForeground=(fg_t)dlsym(h,"TiSetForeground");
    fill_t TiFillRectangle=(fill_t)dlsym(h,"TiFillRectangle");
    map_t TiMapRaised=(map_t)dlsym(h,"TiMapRaised");
    map_t TiMapWindow=(map_t)dlsym(h,"TiMapWindow");
    map_t TiUnmapWindow=(map_t)dlsym(h,"TiUnmapWindow");
    upd_t TiUpdateDisplay=(upd_t)dlsym(h,"TiUpdateDisplay");
    depth_t TiChangeDepthOSDA=(depth_t)dlsym(h,"TiChangeDepthOSDA");
    clear_t TiClearDisplay=(clear_t)dlsym(h,"TiClearDisplay");
    if(!TiOpenDisplay||!TiCreateWindow||!TiFillRectangle){ w("[sw] missing sym\n"); return 2; }
    if(TiChangeDepthOSDA) TiChangeDepthOSDA(1);

    unsigned colors[5]={0xFFFF0000u,0xFF00FF00u,0xFF0000FFu,0xFFFFFF00u,0xFFFFFFFFu};
    const char* names[5]={"RED","GREEN","BLUE","YELLOW","WHITE"};
    for(int id=0; id<5; id++){
        w("\n[sw] === display id "); wint(id); w(" => "); w(names[id]); w(" (8s) ===\n");
        void* dpy=TiOpenDisplay(id);
        w("[sw] dpy="); whexp((unsigned long)dpy); w("\n");
        if(!dpy){ w("[sw] open failed, skip\n"); continue; }
        if(TiClearDisplay) TiClearDisplay(dpy);
        void* root=TiRootWindow(dpy);
        void* win=TiCreateWindow(dpy,root,0,0,1920,1080);
        w("[sw] win="); whexp((unsigned long)win); w("\n");
        if(win){
            void* gc=TiCreateGC(dpy,0);
            TiSetForeground(dpy,gc,colors[id]);
            TiFillRectangle(dpy,win,gc,0,0,1920,1080);
            if(TiMapRaised) TiMapRaised(dpy,win); else if(TiMapWindow) TiMapWindow(dpy,win);
            if(TiUpdateDisplay) TiUpdateDisplay(dpy);
            for(int s=0;s<8;s++){ if(TiUpdateDisplay) TiUpdateDisplay(dpy); usleep(1000000); }
            if(TiUnmapWindow) TiUnmapWindow(dpy,win);
            if(TiUpdateDisplay) TiUpdateDisplay(dpy);
        }
        if(TiCloseDisplay) TiCloseDisplay(dpy);
    }
    w("\n[sw] sweep done.\n");
    return 0;
}
