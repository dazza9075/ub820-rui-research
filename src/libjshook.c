/* libjshook.c  -- LD_PRELOAD AES-key interposer for Panasonic ACE (DP-UB820).
 *
 * Purpose: when AjaxCeplay (the RUI renderer) decrypts an encrypted .js via
 * OpenSSL, log the AES key / IV / passphrase so we can decrypt the RUI offline.
 *
 * Safety design:
 *  - Comm-gated: only arms inside a process whose /proc/self/comm contains
 *    "AjaxCeplay". In every other process (init, busybox, apl_mgr, netflix,
 *    bdv, ...) the hooks are a pass-through no-op, so preloading globally via
 *    startrc cannot change their behaviour.
 *  - No constructor; lazy init on first interposed call.
 *  - Raw syscalls for file I/O (no glibc symbol-version dependency).
 *  - Logs <=16 key bytes / 16 iv bytes (AES-128 confirmed) to avoid over-read.
 *  - Always forwards to the real OpenSSL function via dlsym(RTLD_NEXT).
 *  - Log goes to /tmp/jshook.log (tmpfs); retrieved over telnet before reboot.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <sys/syscall.h>
#include <fcntl.h>
#include <unistd.h>

typedef unsigned char u8;
typedef unsigned long ul;

/* ---- raw syscall I/O ---- */
static long xopen(const char *p, int fl, int mode){ return syscall(SYS_open, p, fl, mode); }
static long xwrite(int fd, const void *b, unsigned n){ return syscall(SYS_write, fd, b, n); }
static long xread(int fd, void *b, unsigned n){ return syscall(SYS_read, fd, b, n); }
static long xclose(int fd){ return syscall(SYS_close, fd); }

static int armed = -1;   /* -1 unknown, 0 no, 1 yes */
static int logfd = -1;

static int contains_ajax(const char *s, int n){
    for (int i = 0; i + 9 <= n; i++)
        if (s[i]=='A'&&s[i+1]=='j'&&s[i+2]=='a'&&s[i+3]=='x'&&
            s[i+4]=='C'&&s[i+5]=='e'&&s[i+6]=='p'&&s[i+7]=='l'&&s[i+8]=='a')
            return 1;
    return 0;
}
static void init_once(void){
    if (armed != -1) return;
    char c[96]; int n = 0;
    int f = xopen("/proc/self/comm", O_RDONLY, 0);
    if (f >= 0){ n = (int)xread(f, c, sizeof c - 1); xclose(f); }
    if (n < 0) n = 0;
    if (n > 0 && c[n-1]=='\n') n--;
    if (contains_ajax(c, n)){
        armed = 1;
        logfd = xopen("/tmp/jshook.log", O_WRONLY|O_CREAT|O_APPEND, 0666);
    } else {
        armed = 0;
    }
}

static char *sapp(char *o, const char *s){ while (*s) *o++ = *s++; return o; }
static char *shex(char *o, const u8 *p, int n){
    static const char h[] = "0123456789abcdef";
    for (int i = 0; i < n; i++){ *o++ = h[p[i]>>4]; *o++ = h[p[i]&15]; }
    return o;
}
static char *sptr(char *o, ul v){
    static const char h[] = "0123456789abcdef";
    for (int i = 28; i >= 0; i -= 4) *o++ = h[(v>>i)&0xf];
    return o;
}
static void flush(const char *buf, char *end){ if (logfd >= 0) xwrite(logfd, buf, (unsigned)(end - buf)); }

/* ---- interposed OpenSSL functions ---- */
typedef int (*dinex_t)(void*, const void*, void*, const u8*, const u8*);
int EVP_DecryptInit_ex(void *ctx, const void *type, void *impl, const u8 *key, const u8 *iv){
    init_once();
    static dinex_t real = 0; if (!real) real = (dinex_t)dlsym(RTLD_NEXT, "EVP_DecryptInit_ex");
    if (armed == 1 && logfd >= 0){
        char b[160], *o = b;
        o = sapp(o, "DecryptInit_ex ra="); o = sptr(o, (ul)__builtin_return_address(0));
        o = sapp(o, " key="); o = key ? shex(o, key, 16) : sapp(o, "NULL");
        o = sapp(o, " iv=");  o = iv  ? shex(o, iv, 16)  : sapp(o, "NULL");
        *o++ = '\n'; flush(b, o);
    }
    return real ? real(ctx, type, impl, key, iv) : -1;
}

typedef int (*din_t)(void*, const void*, const u8*, const u8*);
int EVP_DecryptInit(void *ctx, const void *type, const u8 *key, const u8 *iv){
    init_once();
    static din_t real = 0; if (!real) real = (din_t)dlsym(RTLD_NEXT, "EVP_DecryptInit");
    if (armed == 1 && logfd >= 0){
        char b[160], *o = b;
        o = sapp(o, "DecryptInit ra="); o = sptr(o, (ul)__builtin_return_address(0));
        o = sapp(o, " key="); o = key ? shex(o, key, 16) : sapp(o, "NULL");
        o = sapp(o, " iv=");  o = iv  ? shex(o, iv, 16)  : sapp(o, "NULL");
        *o++ = '\n'; flush(b, o);
    }
    return real ? real(ctx, type, key, iv) : -1;
}

typedef int (*cinex_t)(void*, const void*, void*, const u8*, const u8*, int);
int EVP_CipherInit_ex(void *ctx, const void *type, void *impl, const u8 *key, const u8 *iv, int enc){
    init_once();
    static cinex_t real = 0; if (!real) real = (cinex_t)dlsym(RTLD_NEXT, "EVP_CipherInit_ex");
    if (armed == 1 && logfd >= 0){
        char b[176], *o = b;
        o = sapp(o, "CipherInit_ex enc="); *o++ = enc ? '1' : '0';
        o = sapp(o, " ra="); o = sptr(o, (ul)__builtin_return_address(0));
        o = sapp(o, " key="); o = key ? shex(o, key, 16) : sapp(o, "NULL");
        o = sapp(o, " iv=");  o = iv  ? shex(o, iv, 16)  : sapp(o, "NULL");
        *o++ = '\n'; flush(b, o);
    }
    return real ? real(ctx, type, impl, key, iv, enc) : -1;
}

typedef int (*b2k_t)(const void*, const void*, const u8*, const u8*, int, int, u8*, u8*);
int EVP_BytesToKey(const void *type, const void *md, const u8 *salt, const u8 *data,
                   int datalen, int count, u8 *key, u8 *iv){
    init_once();
    static b2k_t real = 0; if (!real) real = (b2k_t)dlsym(RTLD_NEXT, "EVP_BytesToKey");
    int r = real ? real(type, md, salt, data, datalen, count, key, iv) : -1;
    if (armed == 1 && logfd >= 0){
        char b[400], *o = b;
        int dl = datalen; if (dl < 0) dl = 0; if (dl > 64) dl = 64;
        o = sapp(o, "BytesToKey ra="); o = sptr(o, (ul)__builtin_return_address(0));
        o = sapp(o, " count="); { int c=count; char t[12]; int k=0; if(c==0)t[k++]='0'; while(c){t[k++]='0'+c%10;c/=10;} while(k)*o++=t[--k]; }
        o = sapp(o, " pass="); o = data ? shex(o, data, dl) : sapp(o, "NULL");
        o = sapp(o, " salt="); o = salt ? shex(o, salt, 8)  : sapp(o, "NULL");
        o = sapp(o, " derivedkey="); o = key ? shex(o, key, 16) : sapp(o, "NULL");
        o = sapp(o, " derivediv=");  o = iv  ? shex(o, iv, 16)  : sapp(o, "NULL");
        *o++ = '\n'; flush(b, o);
    }
    return r;
}
