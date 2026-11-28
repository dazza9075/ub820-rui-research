// zig cc -target arm-linux-gnueabihf.2.20 -O2 mocaprobe.c -o mocaprobe -ldl ; run while playing
#include <stdio.h>
#include <stdint.h>
#include <dlfcn.h>
typedef int (*gm_t)(uint16_t*, int, const char*, const char*, int);
typedef int (*gd_t)(int, int, uint16_t*, int, const char*, const char*, int);
typedef int (*rs_t)(int, int, void**); typedef void (*re_t)(void*);
int main(void){
  void *m = dlopen("/usr/dtvrec/lib/libmoc.so", RTLD_NOW|RTLD_GLOBAL); if(!m){puts(dlerror());return 1;}
  gm_t GM = (gm_t)dlsym(m,"_MOC_GetMonitorAttributes"); gd_t GD = (gd_t)dlsym(m,"_MOC_GetDecoderStreamAttributes");
  rs_t RS = (rs_t)dlsym(RTLD_DEFAULT,"RPC_start"); re_t RE = (re_t)dlsym(RTLD_DEFAULT,"RPC_end");
  if(!GM||!GD||!RS||!RE){puts("sym");return 1;}
  void *c = 0; RS(0,0,&c);
  static const uint16_t mon[] = {0x9e,0x9f,0xa0,0xa1,0x109,0x107,0x83,0x3f,0x5e,0x23,0x24,0x25,0x26,0x27,0xb0,0xb4,0xb5,0xbf,0x98};
  for(unsigned i=0;i<sizeof mon/2;i++){ uint16_t p[2]={mon[i],0xffff}; int r=GM(p,1,"probe","mocaprobe",1);
    printf("mon %#x = %d (rc %#x)\n", mon[i], (int16_t)p[1], r); }
  static const int types[] = {0x19, 0x16, 0x04, 0x01};
  static const uint16_t sa[] = {0x23,0x24,0x27,0x28,0x2c,0x2d,0x32,0x53,0x52};
  for(int dec=0; dec<2; dec++) for(unsigned t=0;t<4;t++) for(unsigned i=0;i<sizeof sa/2;i++){
    uint16_t p[2]={sa[i],0xffff}; int r=GD(dec,types[t],p,1,"probe","mocaprobe",2);
    if(!r) printf("dec%d type%#x attr %#x = %d\n", dec, types[t], sa[i], (int16_t)p[1]); }
  RE(c); return 0; }
