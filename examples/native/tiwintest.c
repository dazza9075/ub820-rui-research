/* UB820 — FIRST PIXELS ON SCREEN from our own native code.
 * Connects to the gserver compositor via libtiwin (the sanctioned client path, UI-safe:
 * gserver multiplexes windows so this coexists with the live UI). Signatures cribbed from
 * /usr/dtvrec/bin/fmpsrv call sites:
 *   void* TiOpenDisplay(int id);          void* TiRootWindow(void* dpy);
 *   void* TiCreateWindow(dpy, parent, int x,int y,int w,int h);
 *   void* TiCreateGC(dpy, int);           int TiSetForeground(dpy, gc, unsigned argb);
 *   int TiFillRectangle(dpy, win, gc, x,y,w,h);   int TiMapWindow(dpy, win);
 *   int TiSetWindowBackground(dpy,win,argb); int TiUnmapWindow(dpy,win); int TiCloseDisplay(dpy);
 * write(2) logging only (stdio-global refs crash at load on this device's loader).
 * Build: zig cc -target arm-linux-gnueabihf.2.20 tiwintest.c -o tiwintest -ldl
 * Run:   LD_LIBRARY_PATH=/usr_root/lib:/usr/dtvrec/lib:/lib:/usr/lib ./tiwintest */
#define _GNU_SOURCE
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdlib.h>
#include <dlfcn.h>

static void w(const char*s){ write(1,s,strlen(s)); }
static const char HXD[]="0123456789abcdef";
static void whexp(unsigned long v){ char b[16]; for(int i=0;i<8;i++){ b[7-i]=HXD[v&15]; v>>=4;} w("0x"); write(1,b,8); }
static void wint(long v){ char b[24]; int i=23; if(v==0){w("0");return;} int neg=v<0; unsigned long u=neg?-v:v;
    b[i--]=0; while(u){b[i--]=HXD[u%10];u/=10;} if(neg)b[i--]='-'; write(1,b+i+1,strlen(b+i+1)); }

typedef void* (*open_t)(int);
typedef void* (*root_t)(void*);
typedef void* (*cwin_t)(void*,void*,int,int,int,int);
typedef void* (*cgc_t)(void*,int);
typedef int   (*fg_t)(void*,void*,unsigned);
typedef int   (*fill_t)(void*,void*,void*,int,int,int,int);
typedef int   (*map_t)(void*,void*);
typedef int   (*bg_t)(void*,void*,unsigned);
typedef int   (*upd_t)(void*);
typedef int   (*close_t)(void*);
typedef int   (*depth_t)(int);
typedef int   (*clear_t)(void*);
typedef int   (*border_t)(void*,void*,int);

int main(void){
    w("[tiw] up\n");
    /* Apply the UI's environment (display vars libtiwin reads at init) from a dumped file,
     * BEFORE dlopen so libtiwin's constructors/getenv see them. Robust to any value content. */
    { int f=open("/tmp/ui.env",O_RDONLY); int applied=0;
      if(f>=0){ static char eb[131072]; int n=read(f,eb,sizeof eb-1); close(f);
        if(n>0){ eb[n]=0; char* p=eb;
          while(*p){ char* nl=strchr(p,'\n'); if(nl)*nl=0;
            char* eq=strchr(p,'='); if(eq && eq>p){ *eq=0; setenv(p,eq+1,1); applied++; }
            if(!nl)break; p=nl+1; } } }
      w("[tiw] applied env vars: "); wint(applied); w("\n"); }
    w("[tiw] dlopen libtiwin...\n");
    void* h = dlopen("libtiwin.so.0", RTLD_LAZY|RTLD_GLOBAL);
    if(!h) h = dlopen("/usr_root/lib/libtiwin.so.0.0.0", RTLD_LAZY|RTLD_GLOBAL);
    if(!h){ w("[tiw] dlopen FAIL: "); w(dlerror()); w("\n"); return 1; }
    w("[tiw] libtiwin open\n");

    open_t TiOpenDisplay=(open_t)dlsym(h,"TiOpenDisplay");
    root_t TiRootWindow =(root_t)dlsym(h,"TiRootWindow");
    cwin_t TiCreateWindow=(cwin_t)dlsym(h,"TiCreateWindow");
    cgc_t  TiCreateGC   =(cgc_t)dlsym(h,"TiCreateGC");
    fg_t   TiSetForeground=(fg_t)dlsym(h,"TiSetForeground");
    fill_t TiFillRectangle=(fill_t)dlsym(h,"TiFillRectangle");
    map_t  TiMapWindow  =(map_t)dlsym(h,"TiMapWindow");
    bg_t   TiSetWindowBackground=(bg_t)dlsym(h,"TiSetWindowBackground");
    map_t  TiUnmapWindow=(map_t)dlsym(h,"TiUnmapWindow");
    close_t TiCloseDisplay=(close_t)dlsym(h,"TiCloseDisplay");
    upd_t  TiUpdateDisplay=(upd_t)dlsym(h,"TiUpdateDisplay"); /* optional */
    depth_t TiChangeDepthOSDA=(depth_t)dlsym(h,"TiChangeDepthOSDA");
    clear_t TiClearDisplay=(clear_t)dlsym(h,"TiClearDisplay");
    border_t TiSetWindowBorderness=(border_t)dlsym(h,"TiSetWindowBorderness");
    w("[tiw] syms Open="); whexp((unsigned long)TiOpenDisplay);
    w(" CreateWin="); whexp((unsigned long)TiCreateWindow);
    w(" Fill="); whexp((unsigned long)TiFillRectangle);
    w(" Map="); whexp((unsigned long)TiMapWindow); w("\n");
    if(!TiOpenDisplay||!TiRootWindow||!TiCreateWindow||!TiCreateGC||!TiSetForeground||!TiFillRectangle||!TiMapWindow){
        w("[tiw] missing a required Ti sym; abort.\n"); return 2; }

    /* fmpsrv calls TiChangeDepthOSDA(1) at init — sets OSD plane depth/mode (likely 32bpp ARGB). */
    if(TiChangeDepthOSDA){ int d=TiChangeDepthOSDA(1); w("[tiw] TiChangeDepthOSDA(1)="); wint(d); w("\n"); }

    w("[tiw] TiOpenDisplay(0)...\n");
    void* dpy = TiOpenDisplay(0);
    w("[tiw] dpy="); whexp((unsigned long)dpy); w("\n");
    if(!dpy){ w("[tiw] no display (gserver refused/incompatible); abort cleanly.\n"); return 3; }
    if(TiClearDisplay){ TiClearDisplay(dpy); w("[tiw] TiClearDisplay done\n"); }

    void* root = TiRootWindow(dpy);
    w("[tiw] root="); whexp((unsigned long)root); w("\n");

    map_t  TiMapRaised=(map_t)dlsym(h,"TiMapRaised");
    w("[tiw] present syms: MapRaised="); whexp((unsigned long)TiMapRaised);
    w(" UpdateDisplay="); whexp((unsigned long)TiUpdateDisplay); w("\n");

    int X=0, Y=0, W=1920, H=1080;
    void* win = TiCreateWindow(dpy, root, X, Y, W, H);
    w("[tiw] win="); whexp((unsigned long)win); w("\n");
    if(!win){ w("[tiw] CreateWindow failed; close+abort.\n"); if(TiCloseDisplay)TiCloseDisplay(dpy); return 4; }
    if(TiSetWindowBackground) TiSetWindowBackground(dpy, win, 0xFF101010);
    if(TiSetWindowBorderness) TiSetWindowBorderness(dpy, win, 0);

    void* gc = TiCreateGC(dpy, 0);
    w("[tiw] gc="); whexp((unsigned long)gc); w("\n");

    /* draw a bright, unmistakable test pattern: red/green/blue bands */
    TiSetForeground(dpy, gc, 0xFFFF2020); TiFillRectangle(dpy, win, gc, 0,       0, W, H/3);
    TiSetForeground(dpy, gc, 0xFF20FF20); TiFillRectangle(dpy, win, gc, 0,   H/3,   W, H/3);
    TiSetForeground(dpy, gc, 0xFF2060FF); TiFillRectangle(dpy, win, gc, 0, 2*H/3,   W, H/3);
    w("[tiw] filled 3 bands; mapping RAISED (on top of UI)...\n");
    if(TiMapRaised) TiMapRaised(dpy, win); else TiMapWindow(dpy, win);
    if(TiUpdateDisplay) TiUpdateDisplay(dpy);
    w("[tiw] MAPPED+RAISED. Holding 45s, re-presenting each second...\n");

    /* re-present each second in case the swap is per-frame */
    for(int i=0;i<45;i++){ if(TiUpdateDisplay) TiUpdateDisplay(dpy); usleep(1000000); }

    w("[tiw] teardown (unmap+close)\n");
    if(TiUnmapWindow) TiUnmapWindow(dpy, win);
    if(TiUpdateDisplay) TiUpdateDisplay(dpy);
    if(TiCloseDisplay) TiCloseDisplay(dpy);
    w("[tiw] done.\n");
    return 0;
}
