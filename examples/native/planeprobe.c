/* UB820 — SAFE probe of the LOSD plane allocation state (getters only, no plane writes/enables).
 * UCDD_Get_MemInfo(type,&out) -> out[+8]=phys; mem types 0x11E/0x11F=LOSD0_0/1, 0x120/0x121=LOSD1,
 * 0x122/0x123=LOSD2. avio_up_get_osd_physical_addr(osd_idx,sub) as cross-check. write(2) logging.
 * Build: zig cc -target arm-linux-gnueabihf.2.20 planeprobe.c -o planeprobe -ldl */
#define _GNU_SOURCE
#include <string.h>
#include <unistd.h>
#include <dlfcn.h>
static void w(const char*s){ write(1,s,strlen(s)); }
static const char HX[]="0123456789abcdef";
static void whx(unsigned v){ char b[8]; for(int i=0;i<8;i++){b[7-i]=HX[v&15];v>>=4;} w("0x"); write(1,b,8); }
static void wi(int v){ char b[16]; int i=15; if(v==0){w("0");return;} int neg=v<0; unsigned u=neg?-v:v; b[i--]=0; while(u){b[i--]=HX[u%10];u/=10;} if(neg)b[i--]='-'; write(1,b+i+1,strlen(b+i+1)); }
typedef int (*meminfo_t)(int,void*);
typedef unsigned (*getosd_t)(int,int);
typedef int (*open_t)(int);
int main(void){
    void* u=dlopen("libucdd.so",RTLD_LAZY|RTLD_GLOBAL);
    if(!u) u=dlopen("/usr/dtvrec/lib/libucdd.so",RTLD_LAZY|RTLD_GLOBAL);
    void* a=dlopen("libavio.so",RTLD_LAZY|RTLD_GLOBAL);
    if(!a) a=dlopen("/usr/dtvrec/lib/libavio.so",RTLD_LAZY|RTLD_GLOBAL);
    w("[pp] libucdd="); whx((unsigned long)u); w(" libavio="); whx((unsigned long)a); w("\n");
    meminfo_t UCDD_Get_MemInfo = u?(meminfo_t)dlsym(u,"UCDD_Get_MemInfo"):0;
    getosd_t  get_osd = a?(getosd_t)dlsym(a,"avio_up_get_osd_physical_addr"):0;
    open_t    AVIO_UP_Open = a?(open_t)dlsym(a,"AVIO_UP_Open"):0;
    w("[pp] MemInfo="); whx((unsigned long)UCDD_Get_MemInfo); w(" get_osd="); whx((unsigned long)get_osd);
    w(" Open="); whx((unsigned long)AVIO_UP_Open); w("\n");
    if(AVIO_UP_Open){ int r=AVIO_UP_Open(0); w("[pp] AVIO_UP_Open(0)="); wi(r); w("\n"); }
    if(UCDD_Get_MemInfo){
        int types[6]={0x11E,0x11F,0x120,0x121,0x122,0x123};
        const char* nm[6]={"LOSD0_0","LOSD0_1","LOSD1_0","LOSD1_1","LOSD2_0","LOSD2_1"};
        for(int i=0;i<6;i++){ unsigned out[32]; memset(out,0,sizeof out);
            int r=UCDD_Get_MemInfo(types[i],out);
            w("[pp] "); w(nm[i]); w(" type="); whx(types[i]); w(" rc="); wi(r);
            w("  out[0..4]="); for(int k=0;k<5;k++){ whx(out[k]); w(" "); } w("\n"); }
    }
    if(get_osd){ for(int i=0;i<3;i++){ unsigned p=get_osd(i,0); unsigned p1=get_osd(i,1);
        w("[pp] get_osd idx="); wi(i); w(" sub0="); whx(p); w(" sub1="); whx(p1); w("\n"); } }
    { int(*Cl)(void)=a?(int(*)(void))dlsym(a,"AVIO_UP_Close"):0; if(Cl&&AVIO_UP_Open){ int r=Cl(); w("[pp] AVIO_UP_Close()="); wi(r); w("\n"); } }
    w("[pp] done (read-only, no plane touched)\n");
    return 0;
}
