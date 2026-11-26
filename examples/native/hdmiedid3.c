/* UB820 POC — EDID reader using raw write(2) only (no stdio globals, no buffering).
 * Empirically, binaries on this device that reference libc stdout/stderr data symbols crash at
 * load; printf-only works. write(2) avoids both the global-symbol crash and output buffering, so
 * every line is visible even if a later vendor call faults. Reads raw sink EDID to parse CEA-861
 * Short Audio Descriptors (definitive DTS / DTS-HD support of the connected sink).
 * Build: zig cc -target arm-linux-gnueabihf.2.20 hdmiedid3.c -o hdmiedid3 -ldl */
#define _GNU_SOURCE
#include <string.h>
#include <unistd.h>
#include <dlfcn.h>

static void w(const char *s){ write(1, s, strlen(s)); }
static void wn(const char *s, int n){ write(1, s, n); }
static const char HX[]="0123456789abcdef";
static void whex8(unsigned v){ char b[2]={HX[(v>>4)&15],HX[v&15]}; wn(b,2); }
static void wint(int v){ char b[16]; int i=15; if(v==0){w("0");return;} int neg=v<0; unsigned u=neg?-v:v;
    b[i--]=0; while(u){b[i--]=HX[u%10]; u/=10;} if(neg)b[i--]='-'; wn(b+i+1,(int)strlen(b+i+1)); }
static void whexdump(const unsigned char*p,int n){
    for(int i=0;i<n;i+=16){ w("  "); whex8((i>>8)&0xff); whex8(i&0xff); w(": ");
        for(int j=0;j<16;j++){ if(i+j<n){whex8(p[i+j]); w(" ");} else w("   "); } w("\n"); } }

static const char *HDMI="/usr/dtvrec/lib/libhdmi.so";
static const char *RPC ="/usr/dtvrec/lib/librpc.so";
typedef int (*rpc_start_t)(void*,void*,void**); typedef int (*rpc_end_t)(void*);
typedef int (*open_t)(int*,const char*,int); typedef int (*get_t)(int,void*,const char*,int);
typedef int (*get5_t)(int,int,void*,const char*,int); /* EDID: (handle, arg1, buf, file, line) */
typedef int (*close_t)(int,const char*,int);

int main(void){
    w("[e3] up\n");
    dlopen(RPC,RTLD_NOW|RTLD_GLOBAL);
    void*hh=dlopen(HDMI,RTLD_NOW|RTLD_GLOBAL);
    if(!hh){ w("[e3] dlopen libhdmi FAIL\n"); return 1; }
    rpc_start_t RPC_start=(rpc_start_t)dlsym(RTLD_DEFAULT,"RPC_start");
    rpc_end_t   RPC_end  =(rpc_end_t)dlsym(RTLD_DEFAULT,"RPC_end");
    open_t  TxOpen =(open_t)dlsym(hh,"__HDMI_TxOpen");
    get5_t  TxEdid =(get5_t)dlsym(hh,"__HDMI_TxGetSinkEdidInfo");
    get_t   TxAud  =(get_t)dlsym(hh,"__HDMI_TxGetSinkAudioInfo");
    close_t TxClose=(close_t)dlsym(hh,"__HDMI_TxClose");
    if(!RPC_start||!TxOpen||!TxEdid||!TxClose){ w("[e3] missing sym\n"); return 2; }

    void*ctx=0; int rs=RPC_start(0,0,&ctx); w("[e3] RPC_start rc="); wint(rs); w("\n");
    int h=0,ro=-1; for(int t=0;t<8;t++){ ro=TxOpen(&h,__FILE__,__LINE__); if(!ro)break; usleep(200000);}
    w("[e3] TxOpen rc="); wint(ro); w(" handle="); wint(h); w("\n");
    if(ro){ if(RPC_end)RPC_end(ctx); return 3; }

    static unsigned char buf[2048];
    memset(buf,0,sizeof buf);
    int a1s[6]={1,2,128,256,512,255};
    for(int k=0;k<6;k++){ memset(buf,0,sizeof buf);
        int re=TxEdid(h,a1s[k],buf,__FILE__,__LINE__);
        w("[e3] GetSinkEdidInfo arg1="); wint(a1s[k]); w(" rc="); wint(re);
        w(" first8="); for(int j=0;j<8;j++) whex8(buf[j]); w("\n");
        if(re==0 && buf[0]==0x00 && buf[1]==0xff){ w("[e3] RAW EDID found:\n"); whexdump(buf,256); break; } }

    if(TxAud){ memset(buf,0,256); int ra=TxAud(h,buf,__FILE__,__LINE__);
        w("[e3] GetSinkAudioInfo rc="); wint(ra); w("\n");
        if(ra==0){ w("[e3] audio struct 256:\n"); whexdump(buf,256); } }

    int rc=TxClose(h,__FILE__,__LINE__); w("[e3] TxClose rc="); wint(rc); w("\n");
    if(RPC_end)RPC_end(ctx); w("[e3] done\n"); return 0;
}
