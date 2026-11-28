/* keyinj <amcode> [press|release|both] : inject one remote key into apl_mgr (AM, queue 0x90c35100) exactly like
 * net_mgr's LAN path does (EV msg ty 2, k 2, c 59, f 1=press/2=release, payload = AM key code; sender 50301).
 * Used to emulate the IR-only HOME key (AM code 0x57) for tests. Logs via write(2). */
#include <dlfcn.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
static void w(const char* s){ write(2, s, strlen(s)); }
static void wi(long v){ char b[24]; int n = 0; unsigned long u = v < 0 ? -v : v; do b[n++] = '0' + u % 10; while(u /= 10); if(v < 0) w("-"); while(n) write(2, &b[--n], 1); }
typedef int (*snd_t)(uint32_t, uint32_t, const void*, uint32_t);
int main(int c, char** v){
    if(c < 2){ w("usage: keyinj <amcode> [press|release|both]\n"); return 1; }
    uint32_t key = strtoul(v[1], 0, 0); const char* m = c > 2 ? v[2] : "both";
    void* h = dlopen("libev.so", RTLD_NOW); if(!h) h = dlopen("/usr/dtvrec/lib/libev.so", RTLD_NOW);
    if(!h){ w("dlopen libev.so failed\n"); return 2; }
    snd_t snd = (snd_t)dlsym(h, "EV_send_message"); if(!snd){ w("no EV_send_message\n"); return 2; }
    int rc = 0;
    for(int f = 1; f <= 2; f++){
        if(f == 1 && !strcmp(m, "release")) continue;
        if(f == 2 && !strcmp(m, "press")) continue;
        uint32_t d[9] = { 50001, 2, 0, 2, 59, (uint32_t)f, 0, 0, key };
        int r = snd(0x90c35100u, sizeof d, d, 50301);
        w(f == 1 ? "press " : "release "); wi(key); w(" -> "); wi(r); w("\n"); if(r) rc = 3;
        if(f == 1){ struct timespec t = { 0, 150000000 }; nanosleep(&t, 0); }
    }
    return rc;
}
