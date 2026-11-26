/* UB820 native-code POC — Stage 1: prove our own ARM binary runs on the device,
 * dlopen()s Panasonic's closed vendor library, and resolves its functions.
 * No vendor function is CALLED in stage 1 (zero crash risk). Later stages (behind argv)
 * will invoke a real read-only getter once signatures are recovered.
 *
 * Build (soft-float host gcc, override interp to the device's hard-float loader; POC uses no floats):
 *   arm-linux-gnu-gcc -O2 hdmiprobe.c -o hdmiprobe -ldl \
 *       -Wl,--dynamic-linker=/lib/ld-linux-armhf.so.3
 * Run on device:
 *   LD_LIBRARY_PATH=/usr/dtvrec/lib:/lib:/usr/lib ./hdmiprobe
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <dlfcn.h>

static const char *LIB = "/usr/dtvrec/lib/libhdmi.so";
static const char *SYMS[] = {
    "__HDMI_TxOpen", "__HDMI_TxClose", "__HDMI_TxGetPowerMode",
    "__HDMI_TxGetSinkAudioInfo", "__HDMI_TxGetSinkSpeakerAlloc",
    "__HDMI_TxGetSinkEdidInfo", "__HDMI_TxGetAudioFormat",
    "__HDMI_TxGetSinkStatus", 0
};

int main(void)
{
    printf("[poc] native binary is running on the device.\n");
    printf("[poc] dlopen %s ...\n", LIB);
    void *h = dlopen(LIB, RTLD_NOW | RTLD_GLOBAL);
    if (!h) {
        printf("[poc] dlopen FAILED: %s\n", dlerror());
        return 1;
    }
    printf("[poc] dlopen OK  handle=%p\n", h);
    int ok = 0;
    for (int i = 0; SYMS[i]; i++) {
        void *f = dlsym(h, SYMS[i]);
        printf("[poc]   %-32s = %p\n", SYMS[i], f);
        if (f) ok++;
    }
    printf("[poc] stage1 complete: %d symbols resolved from the closed vendor lib by OUR binary.\n", ok);
    dlclose(h);
    return 0;
}
