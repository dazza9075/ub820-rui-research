/* UB820 native-code POC — Stage 2: make ONE real, read-only vendor call.
 *
 * Replicates Panasonic's OWN non-disruptive capability probe (debug tag "check_app_support.c"
 * in /usr/local/bin/omplayer), which does:  RPC_start -> __HDMI_TxOpen -> __HDMI_Tx*InfoForMenu
 * -> __HDMI_TxClose -> RPC_end  during normal operation. We call the AUDIO variant to read what
 * the connected sink (soundbar/TV) advertises it can accept — this is pure EDID read-back, no
 * pipeline reconfig. Signatures recovered from omplayer's call site (objdump) + libhdmi disasm:
 *     int RPC_start(void*, void*, void **out_ctx);
 *     int RPC_end(void *ctx);
 *     int __HDMI_TxOpen(int *out_handle, const char *file, int line);              // 0 == OK
 *     int __HDMI_TxGetSinkAudioInfoForMenu(int handle, void *outbuf, const char*, int); // 0 == OK
 *     int __HDMI_TxGetSinkAudioInfo(int handle, void *outbuf, const char*, int);        // 0 == OK
 *     int __HDMI_TxClose(int handle, const char *file, int line);
 *
 * Build:  zig cc -target arm-linux-gnueabihf.2.20 hdmiaudio.c -o hdmiaudio -ldl
 * Run:    LD_LIBRARY_PATH=/usr/dtvrec/lib:/lib:/usr/lib ./hdmiaudio
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <dlfcn.h>

static const char *HDMI = "/usr/dtvrec/lib/libhdmi.so";
static const char *RPC  = "/usr/dtvrec/lib/librpc.so";

typedef int (*rpc_start_t)(void *, void *, void **);
typedef int (*rpc_end_t)(void *);
typedef int (*open_t)(int *, const char *, int);
typedef int (*get_t)(int, void *, const char *, int);
typedef int (*close_t)(int, const char *, int);

static void hexdump(const unsigned char *b, int n) {
    for (int i = 0; i < n; i += 16) {
        printf("    %03x: ", i);
        for (int j = 0; j < 16; j++) {
            if (i + j < n) printf("%02x ", b[i + j]); else printf("   ");
        }
        printf(" |");
        for (int j = 0; j < 16 && i + j < n; j++) {
            unsigned char c = b[i + j];
            printf("%c", (c >= 32 && c < 127) ? c : '.');
        }
        printf("|\n");
    }
}

int main(void)
{
    printf("[s2] native probe up; opening vendor libs...\n");
    void *hr = dlopen(RPC,  RTLD_NOW | RTLD_GLOBAL);
    void *hh = dlopen(HDMI, RTLD_NOW | RTLD_GLOBAL);
    if (!hh) { printf("[s2] dlopen libhdmi FAILED: %s\n", dlerror()); return 1; }
    if (!hr) { printf("[s2] dlopen librpc note: %s (may be pulled via NEEDED)\n", dlerror()); }

    rpc_start_t RPC_start = (rpc_start_t)dlsym(RTLD_DEFAULT, "RPC_start");
    rpc_end_t   RPC_end   = (rpc_end_t)  dlsym(RTLD_DEFAULT, "RPC_end");
    open_t   TxOpen  = (open_t)  dlsym(hh, "__HDMI_TxOpen");
    get_t    TxGetA  = (get_t)   dlsym(hh, "__HDMI_TxGetSinkAudioInfoForMenu");
    get_t    TxGetA2 = (get_t)   dlsym(hh, "__HDMI_TxGetSinkAudioInfo");
    close_t  TxClose = (close_t) dlsym(hh, "__HDMI_TxClose");
    printf("[s2] syms: RPC_start=%p RPC_end=%p TxOpen=%p GetAudioForMenu=%p GetAudio=%p TxClose=%p\n",
           (void*)RPC_start,(void*)RPC_end,(void*)TxOpen,(void*)TxGetA,(void*)TxGetA2,(void*)TxClose);
    if (!RPC_start || !TxOpen || !TxGetA || !TxClose) { printf("[s2] missing a required sym; abort.\n"); return 2; }

    /* 1) establish RPC channel (match omplayer: only the ctx out-ptr matters) */
    void *ctx = 0;
    int rs = RPC_start((void*)0, (void*)0, &ctx);
    printf("[s2] RPC_start rc=%d ctx=%p\n", rs, ctx);

    /* 2) open an HDMI TX client handle (retry on transient failure, like omplayer) */
    int handle = 0, ro = -1;
    for (int t = 0; t < 8; t++) {
        ro = TxOpen(&handle, __FILE__, __LINE__);
        printf("[s2] __HDMI_TxOpen try%d rc=%d handle=0x%x\n", t, ro, handle);
        if (ro == 0) break;
        usleep(200000);
    }
    if (ro != 0) { printf("[s2] TxOpen never succeeded; stopping (no harm done).\n"); if (RPC_end) RPC_end(ctx); return 3; }

    /* 3) READ sink audio caps (ForMenu variant = the proven-safe call site) */
    unsigned char buf[512];
    memset(buf, 0, sizeof buf);
    int rg = TxGetA(handle, buf, __FILE__, __LINE__);
    printf("[s2] __HDMI_TxGetSinkAudioInfoForMenu rc=%d\n", rg);
    if (rg == 0) { printf("[s2] sink audio (ForMenu) struct bytes:\n"); hexdump(buf, 256); }

    /* 3b) also the raw (non-menu) variant if present */
    if (TxGetA2) {
        unsigned char buf2[512];
        memset(buf2, 0, sizeof buf2);
        int rg2 = TxGetA2(handle, buf2, __FILE__, __LINE__);
        printf("[s2] __HDMI_TxGetSinkAudioInfo rc=%d\n", rg2);
        if (rg2 == 0) { printf("[s2] sink audio (raw) struct bytes:\n"); hexdump(buf2, 256); }
    }

    /* 4) clean teardown */
    int rc = TxClose(handle, __FILE__, __LINE__);
    printf("[s2] __HDMI_TxClose rc=%d\n", rc);
    if (RPC_end) RPC_end(ctx);
    printf("[s2] done.\n");
    return 0;
}
