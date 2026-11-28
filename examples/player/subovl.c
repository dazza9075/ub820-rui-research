/* subovl: subtitle overlay daemon for the DP-UB820 native player (ub820play + ub820run.subs.sh).
 *
 * Renders SRT (or WebVTT) cues with stb_truetype into LOSD0, our own free 1920x1080 ARGB8888 OSD buffer
 * (phys 0xE1FF0000, bus 0x21FF0000), and keeps UI plane 7 pointed at it (the flipclear method).
 * Timing comes from ub820play's status file (pos, once a second) + the launcher's base file, interpolated
 * with CLOCK_MONOTONIC while playing, frozen while paused.
 *
 * Usage: subovl <file.srt|file.vtt> [--size PX] [--margin PX] [--status PATH] [--base-file PATH]
 *               [--offset SEC] [--font PATH] [--font-index N] [--width PCT] [--outline PX] [--shadow PX]
 *               [--color 0xRRGGBB] [--no-kern] [--verbose]
 *   HOST build only: --render-at SEC --out FILE.raw   (render the cue(s) active at SEC into a fake LOSD0, dump it)
 *                    --text "line1\nline2"            (render this text instead of an SRT cue)
 *
 * Safety rules (LOG.md 2026-11-26/27):
 *  - AVIO_UP_Open is refcounted, max 7, and leaks survive the process. So we never hold it: each plane check is
 *    one short Open -> Get -> (Set) -> Close window. Open != 0 -> no Get/Set at all. Signal handlers only set a flag.
 *  - Set_Addr(7) only when plane 7 shows sel=1 + one of gserver's two LOSD1 buffers (flipclear's guard). Already
 *    LOSD0 -> nothing. Anything else -> logged, left alone.
 *  - No AVIO call at all when gserver is not running (standby: the UI stack is torn down) -> we exit.
 *  - Pixels are written only inside the current cue's bounding box (full clear only at start and exit).
 *  - On exit plane 7 is left alone; gserver re-flips it on its next draw.
 * Logging: write(2) only (zig glibc builds crash on stdio's stdout/stderr data symbols); snprintf is fine.
 *
 * Build (device): zig cc -target arm-linux-gnueabihf.2.20 -O2 -s subovl.c -o subovl -ldl -lm
 * Build (host test): cc -DHOST -O2 subovl.c -o subovl.host -lm
 */
#define _GNU_SOURCE
#define _LARGEFILE64_SOURCE
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>      /* snprintf/vsnprintf only: never stdout/stderr */
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <time.h>
#include <math.h>
#include <errno.h>
#include <dirent.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#ifndef HOST
#include <dlfcn.h>
#endif

#define STBTT_assert(x) ((void)0)
#define STBTT_STATIC
#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"
#include "subovl_font.h"          /* embedded fallback: Noto Sans Bold ASCII + Latin-1, 4bpp (gen_subovl_font.c) */

/* ---------------------------------------------------------------------------------------------------- */
#define FBW 1920
#define FBH 1080
#define LEN 0x7E9000UL
#define LOSD0_PHYS 0xE1FF0000ULL
#define BUSOFF 0xC0000000u
#define LOSD0_BUS ((unsigned)(LOSD0_PHYS - BUSOFF))      /* 0x21FF0000 */
#define GS_BUF_A 0x21010000u                            /* gserver's LOSD1 front/back */
#define GS_BUF_B 0x217fc000u
#define MAXLINES 3

static volatile sig_atomic_t g_stop;
static void onsig(int s){ (void)s; g_stop = 1; }

static int g_verbose;
static void lw(const char* s){ size_t n = strlen(s); while(n){ ssize_t r = write(2, s, n); if(r <= 0) break; s += r; n -= (size_t)r; } }
static void lg(const char* fmt, ...){
    char b[1024]; int n = snprintf(b, sizeof b, "[subovl] ");
    va_list ap; va_start(ap, fmt); vsnprintf(b + n, sizeof b - (size_t)n - 1, fmt, ap); va_end(ap);
    size_t l = strlen(b); if(l && b[l-1] != '\n'){ b[l] = '\n'; b[l+1] = 0; } lw(b);
}
#define LV(...) do{ if(g_verbose) lg(__VA_ARGS__); }while(0)

static double mono(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec * 1e-9; }
static double realnow(void){ struct timespec t; clock_gettime(CLOCK_REALTIME, &t); return t.tv_sec + t.tv_nsec * 1e-9; }
static void sleep_s(double s){ if(s <= 0) return; struct timespec t; t.tv_sec = (time_t)s; t.tv_nsec = (long)((s - (double)t.tv_sec) * 1e9); nanosleep(&t, 0); }

/* ---------------------------------------------------------------------------------------------------- */
/* options                                                                                              */
static struct {
    const char* srt; const char* status; const char* base; const char* font; int font_index;
    int size, margin, width_pct, outline, shadow, kern; double offset; uint32_t color;
    double render_at; const char* out; const char* text;    /* HOST test */
} O;

/* ---------------------------------------------------------------------------------------------------- */
/* file helpers                                                                                         */
static char* slurp(const char* path, size_t* len, struct stat* st_out){
    int fd = open(path, O_RDONLY | O_CLOEXEC); if(fd < 0) return 0;
    struct stat st; if(fstat(fd, &st) < 0){ close(fd); return 0; }
    size_t cap = st.st_size > 0 ? (size_t)st.st_size + 1 : 4096, n = 0; char* b = malloc(cap + 1);
    if(!b){ close(fd); return 0; }
    for(;;){ if(n == cap){ char* nb = realloc(b, cap * 2 + 1); if(!nb) break; b = nb; cap *= 2; }
        ssize_t r = read(fd, b + n, cap - n); if(r <= 0) break; n += (size_t)r; }
    close(fd); b[n] = 0; if(len) *len = n; if(st_out) *st_out = st; return b;
}

/* ---------------------------------------------------------------------------------------------------- */
/* SRT / WebVTT                                                                                         */
typedef struct { double t0, t1; char* text; int top; } cue;
static cue* g_cues; static int g_ncues;
static off_t g_srt_size = -1; static struct timespec g_srt_mtime;

static int parse_ts(const char* s, const char** end, double* out){
    /* [H+:]MM:SS[,.]mmm   (also tolerates H:MM:SS without fraction) */
    long f[3]; int nf = 0; const char* p = s;
    while(*p == ' ' || *p == '\t') p++;
    for(;;){ if(*p < '0' || *p > '9') return -1; long v = 0; while(*p >= '0' && *p <= '9'){ v = v * 10 + (*p - '0'); p++; }
        if(nf == 3) return -1; f[nf++] = v; if(*p == ':'){ p++; continue; } break; }
    if(nf < 2) return -1;
    double frac = 0;
    if(*p == ',' || *p == '.'){ p++; double sc = 0.1; while(*p >= '0' && *p <= '9'){ frac += (*p - '0') * sc; sc *= 0.1; p++; } }
    double t = nf == 3 ? f[0] * 3600.0 + f[1] * 60.0 + f[2] : f[0] * 60.0 + f[1];
    *out = t + frac; if(end) *end = p; return 0;
}
static int utf8_put(char* o, uint32_t c){
    if(c < 0x80){ o[0] = (char)c; return 1; }
    if(c < 0x800){ o[0] = (char)(0xC0 | (c >> 6)); o[1] = (char)(0x80 | (c & 63)); return 2; }
    if(c < 0x10000){ o[0] = (char)(0xE0 | (c >> 12)); o[1] = (char)(0x80 | ((c >> 6) & 63)); o[2] = (char)(0x80 | (c & 63)); return 3; }
    o[0] = (char)(0xF0 | (c >> 18)); o[1] = (char)(0x80 | ((c >> 12) & 63)); o[2] = (char)(0x80 | ((c >> 6) & 63)); o[3] = (char)(0x80 | (c & 63)); return 4;
}
/* strip <tags>, {\ass} blocks (noting \an7/8/9 = top), entities, \N \n \h; trim lines; drop empty lines */
static char* clean_text(const char* in, int* top){
    size_t n = strlen(in); char* o = malloc(n * 2 + 8); size_t k = 0; *top = 0;
    for(const char* p = in; *p; ){
        if(*p == '\r'){ p++; continue; }
        if(*p == '<'){ const char* q = strchr(p, '>'); if(q){ p = q + 1; continue; } }
        if(*p == '{'){ const char* q = strchr(p, '}');
            if(q){ for(const char* a = p; a + 3 < q; a++) if(a[0] == '\\' && a[1] == 'a' && a[2] == 'n' && a[3] >= '7' && a[3] <= '9') *top = 1;
                p = q + 1; continue; } }
        if(*p == '\\' && (p[1] == 'N' || p[1] == 'n')){ o[k++] = '\n'; p += 2; continue; }
        if(*p == '\\' && p[1] == 'h'){ o[k++] = ' '; p += 2; continue; }
        if(*p == '&'){
            static const struct { const char* e; uint32_t c; } E[] = { {"&amp;",'&'},{"&lt;",'<'},{"&gt;",'>'},{"&quot;",'"'},
                {"&apos;",'\''},{"&nbsp;",0xA0},{"&lrm;",0},{"&rlm;",0} };
            int hit = 0;
            for(unsigned i = 0; i < sizeof E / sizeof E[0]; i++){ size_t l = strlen(E[i].e);
                if(!strncmp(p, E[i].e, l)){ if(E[i].c) k += (size_t)utf8_put(o + k, E[i].c); p += l; hit = 1; break; } }
            if(!hit && p[1] == '#'){ const char* q = p + 2; uint32_t c = 0; int hex = (*q == 'x' || *q == 'X'); if(hex) q++;
                const char* d0 = q; while(*q && *q != ';' && q - p < 12){ int v = (*q >= '0' && *q <= '9') ? *q - '0' : hex && ((*q|32) >= 'a' && (*q|32) <= 'f') ? (*q|32) - 'a' + 10 : -1;
                    if(v < 0) break; c = c * (hex ? 16 : 10) + (uint32_t)v; q++; }
                if(*q == ';' && q > d0 && c && c < 0x110000){ k += (size_t)utf8_put(o + k, c); p = q + 1; hit = 1; } }
            if(hit) continue;
        }
        o[k++] = *p++;
    }
    o[k] = 0;
    /* trim each line, drop empty ones */
    char* r = malloc(k + 1); size_t m = 0;
    for(char* line = o; line && *line; ){
        char* nl = strchr(line, '\n'); if(nl) *nl = 0;
        char* a = line; while(*a == ' ' || *a == '\t') a++;
        size_t l = strlen(a); while(l && (a[l-1] == ' ' || a[l-1] == '\t')) l--;
        if(l){ if(m) r[m++] = '\n'; memcpy(r + m, a, l); m += l; }
        line = nl ? nl + 1 : 0;
    }
    r[m] = 0; free(o); return r;
}
static int cue_cmp(const void* a, const void* b){ double d = ((const cue*)a)->t0 - ((const cue*)b)->t0; return d < 0 ? -1 : d > 0; }
static void free_cues(cue* c, int n){ for(int i = 0; i < n; i++) free(c[i].text); free(c); }

/* parse the whole file. Blocks: [index]\n<ts> --> <ts> [settings]\ntext...\n\n. An unterminated last block (file still being
   written by the extractor) is accepted; the next reload replaces it. */
static int parse_subs(const char* buf, cue** out){
    int cap = 256, n = 0; cue* c = malloc(sizeof(cue) * (size_t)cap);
    const char* p = buf;
    if((unsigned char)p[0] == 0xEF && (unsigned char)p[1] == 0xBB && (unsigned char)p[2] == 0xBF) p += 3;
    while(*p){
        const char* le = strchr(p, '\n'); size_t ll = le ? (size_t)(le - p) : strlen(p);
        const char* arrow = 0; for(const char* a = p; a + 2 < p + ll; a++) if(a[0] == '-' && a[1] == '-' && a[2] == '>'){ arrow = a; break; }
        if(!arrow){ p = le ? le + 1 : p + ll; continue; }
        double t0, t1; const char* e;
        if(parse_ts(p, &e, &t0) < 0 || parse_ts(arrow + 3, &e, &t1) < 0){ p = le ? le + 1 : p + ll; continue; }
        p = le ? le + 1 : p + ll;
        /* text until a blank line (or a line that is itself a timing line: malformed file without blank separators) */
        const char* ts = p; const char* te = p;
        while(*p){
            const char* l2 = strchr(p, '\n'); size_t len2 = l2 ? (size_t)(l2 - p) : strlen(p);
            size_t vis = len2; while(vis && (p[vis-1] == '\r' || p[vis-1] == ' ' || p[vis-1] == '\t')) vis--;
            if(!vis) { p = l2 ? l2 + 1 : p + len2; break; }
            int timing = 0; for(const char* a = p; a + 2 < p + len2; a++) if(a[0] == '-' && a[1] == '-' && a[2] == '>'){ timing = 1; break; }
            if(timing) break;
            te = p + len2; p = l2 ? l2 + 1 : p + len2;
        }
        size_t tl = (size_t)(te - ts); char* raw = malloc(tl + 1); memcpy(raw, ts, tl); raw[tl] = 0;
        /* a trailing bare number line is the next block's index when blank lines are missing: drop it */
        int top; char* txt = clean_text(raw, &top); free(raw);
        if(!*txt || t1 <= t0){ free(txt); continue; }
        if(n == cap){ cap *= 2; cue* nc = realloc(c, sizeof(cue) * (size_t)cap); if(!nc){ free(txt); break; } c = nc; }
        c[n].t0 = t0; c[n].t1 = t1; c[n].text = txt; c[n].top = top; n++;
    }
    qsort(c, (size_t)n, sizeof(cue), cue_cmp);
    *out = c; return n;
}
/* (re)load when size or mtime changed. Returns 1 if reloaded. Missing/empty file is not an error (extractor still starting). */
static int load_subs(int force){
    struct stat st;
    if(stat(O.srt, &st) < 0){ if(g_srt_size != -2){ lg("subtitle file %s not there (yet)", O.srt); g_srt_size = -2; } return 0; }
    if(!force && st.st_size == g_srt_size && st.st_mtim.tv_sec == g_srt_mtime.tv_sec && st.st_mtim.tv_nsec == g_srt_mtime.tv_nsec) return 0;
    size_t len; char* b = slurp(O.srt, &len, &st); if(!b) return 0;
    cue* c; int n = parse_subs(b, &c); free(b);
    free_cues(g_cues, g_ncues); g_cues = c; g_ncues = n;
    g_srt_size = st.st_size; g_srt_mtime = st.st_mtim;
    static int loads; loads++;
    if(loads == 1 || g_verbose) lg("loaded %d cues from %s (%ld bytes)%s", n, O.srt, (long)st.st_size, n ? "" : " (none yet)");
    if(n) LV("first cue %.3f..%.3f, last cue %.3f..%.3f", c[0].t0, c[0].t1, c[n-1].t0, c[n-1].t1);
    return 1;
}

/* ---------------------------------------------------------------------------------------------------- */
/* fonts: stb_truetype (TTF/OTF-CFF/TTC first face) or the embedded fallback bitmap font                 */
static stbtt_fontinfo g_font; static int g_have_ttf; static float g_scale;     /* px per font unit */
static float g_asc, g_desc;          /* px, desc positive */
static float g_fscale;               /* fallback: target px / SUBOVL_FONT_EM */
static const char* g_font_path;

static int font_magic_ok(const unsigned char* d, size_t n){
    if(n < 12) return 0;
    return (d[0]==0 && d[1]==1 && d[2]==0 && d[3]==0) || !memcmp(d, "OTTO", 4) || !memcmp(d, "true", 4) || !memcmp(d, "ttcf", 4);
}
static int try_font(const char* path, int index){
    int fd = open(path, O_RDONLY | O_CLOEXEC); if(fd < 0) return 0;
    struct stat st; if(fstat(fd, &st) < 0 || st.st_size < 1024){ close(fd); return 0; }
    unsigned char* d = mmap(0, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0); close(fd);
    if(d == MAP_FAILED) return 0;
    if(!font_magic_ok(d, (size_t)st.st_size)){ munmap(d, (size_t)st.st_size); LV("font %s: not a TrueType/OpenType file", path); return 0; }
    if(!memcmp(d, "ttcf", 4)){ int nf = stbtt_GetNumberOfFonts(d); if(index >= nf) index = 0; }
    int off = stbtt_GetFontOffsetForIndex(d, index);
    if(off < 0 || (off_t)off >= st.st_size || !stbtt_InitFont(&g_font, d, off) || !stbtt_FindGlyphIndex(&g_font, 'A')){
        munmap(d, (size_t)st.st_size); lg("font %s: stb_truetype could not use face %d (CFF2/variable fonts are not supported)", path, index); return 0; }
    g_have_ttf = 1; g_font_path = path; return 1;
}
static int has_font_ext(const char* n){ size_t l = strlen(n); if(l < 5) return 0; const char* e = n + l - 4;
    return !strcasecmp(e, ".ttf") || !strcasecmp(e, ".otf") || !strcasecmp(e, ".ttc"); }
static int scan_dir(const char* dir, int depth){
    DIR* d = opendir(dir); if(!d) return 0; struct dirent* e; int ok = 0; char path[512];
    while(!ok && (e = readdir(d))){
        if(e->d_name[0] == '.') continue;
        snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
        struct stat st; if(stat(path, &st) < 0) continue;
        if(S_ISDIR(st.st_mode)){ if(depth > 0) ok = scan_dir(path, depth - 1); }
        else if(S_ISREG(st.st_mode) && has_font_ext(e->d_name)){ char* p = strdup(path); if(try_font(p, 0)) ok = 1; else free(p); }
    }
    closedir(d); return ok;
}
static void font_init(void){
    static const char* fixed[] = { "/tmp/NotoSansCJK-Regular.ttc", "/tmp/subovl.ttf", "/tmp/subovl.otf", 0 };
    static const char* dirs[] = { "/tmp/fonts", "/usr/local/share/fonts", "/usr/share/fonts", "/usr/local/font", "/usr/local/fonts",
        "/usr/X11R6/lib/X11/fonts", "/usr/lib/fonts", "/usr/dtvrec/font", "/usr/dtvrec/fonts", 0 };
    int ok = 0;
    if(O.font && !strcmp(O.font, "builtin")) fixed[0] = 0, dirs[0] = 0;      /* force the embedded fallback */
    else if(O.font) ok = try_font(O.font, O.font_index);
    for(int i = 0; !ok && fixed[i]; i++) ok = try_font(fixed[i], O.font_index);
    for(int i = 0; !ok && dirs[i]; i++) ok = scan_dir(dirs[i], 2);
    if(ok){
        g_scale = stbtt_ScaleForMappingEmToPixels(&g_font, (float)O.size);
        int a, d, gap; stbtt_GetFontVMetrics(&g_font, &a, &d, &gap);
        g_asc = a * g_scale; g_desc = -d * g_scale;
        lg("font %s face %d, %d px (ascent %.1f, descent %.1f)", g_font_path, O.font_index, O.size, g_asc, g_desc);
    } else {
        g_fscale = (float)O.size / SUBOVL_FONT_EM;
        g_asc = SUBOVL_FONT_ASC * g_fscale; g_desc = SUBOVL_FONT_DESC * g_fscale;
        lg("no usable TrueType font: using the embedded fallback font (ASCII + Latin-1 only), %d px", O.size);
    }
}
/* glyph handle: TTF glyph index, or fallback table index (+1; 0 = missing) */
static int fb_find(uint32_t cp){
    int lo = 0, hi = SUBOVL_FONT_N - 1;
    while(lo <= hi){ int m = (lo + hi) / 2; if(subovl_font_g[m].cp == cp) return m + 1; if(subovl_font_g[m].cp < cp) lo = m + 1; else hi = m - 1; }
    return 0;
}
static int glyph_of(uint32_t cp){
    if(cp == 0xA0) cp = ' ';
    if(g_have_ttf){ int g = stbtt_FindGlyphIndex(&g_font, (int)cp); if(!g && cp > 0x7F) g = stbtt_FindGlyphIndex(&g_font, 0xFFFD); return g; }
    int g = fb_find(cp); if(!g) g = fb_find('?'); return g;
}
static float glyph_adv(int g){
    if(g_have_ttf){ int adv, lsb; stbtt_GetGlyphHMetrics(&g_font, g, &adv, &lsb); return adv * g_scale; }
    return g ? subovl_font_g[g-1].adv4 * 0.25f * g_fscale : 0.f;
}
static float glyph_kern(int g1, int g2){
    if(!O.kern || !g_have_ttf || !g1 || !g2) return 0.f;
    return stbtt_GetGlyphKernAdvance(&g_font, g1, g2) * g_scale;
}
/* max-merge one glyph's coverage into cov (bw x bh) with the pen at (px, baseline by) */
static unsigned char* g_tmp; static size_t g_tmpcap;
static void glyph_draw(unsigned char* cov, int bw, int bh, int g, float px, int by){
    if(!g) return;
    if(g_have_ttf){
        int ix = (int)floorf(px); float fx = px - (float)ix; int x0, y0, x1, y1;
        stbtt_GetGlyphBitmapBoxSubpixel(&g_font, g, g_scale, g_scale, fx, 0, &x0, &y0, &x1, &y1);
        int w = x1 - x0, h = y1 - y0; if(w <= 0 || h <= 0 || w > 4096 || h > 4096) return;
        size_t need = (size_t)w * (size_t)h; if(need > g_tmpcap){ free(g_tmp); g_tmp = malloc(need); g_tmpcap = g_tmp ? need : 0; if(!g_tmp) return; }
        stbtt_MakeGlyphBitmapSubpixel(&g_font, g_tmp, w, h, w, g_scale, g_scale, fx, 0, g);
        for(int y = 0; y < h; y++){ int ty = by + y0 + y; if(ty < 0 || ty >= bh) continue;
            for(int x = 0; x < w; x++){ int tx = ix + x0 + x; if(tx < 0 || tx >= bw) continue;
                unsigned char v = g_tmp[y * w + x]; if(v > cov[ty * bw + tx]) cov[ty * bw + tx] = v; } }
        return;
    }
    /* fallback: bilinear-scaled 4bpp atlas glyph */
    const struct subovl_glyph* G = &subovl_font_g[g-1]; if(!G->w || !G->h) return;
    float s = g_fscale;
    float gx0 = px + G->x0 * s, gy0 = (float)by + G->y0 * s;
    int tx0 = (int)floorf(gx0), ty0 = (int)floorf(gy0), tx1 = (int)ceilf(gx0 + G->w * s), ty1 = (int)ceilf(gy0 + G->h * s);
    for(int ty = ty0; ty < ty1; ty++){ if(ty < 0 || ty >= bh) continue;
        float sy = ((float)ty + 0.5f - gy0) / s - 0.5f; int iy = (int)floorf(sy); float fy = sy - (float)iy;
        for(int tx = tx0; tx < tx1; tx++){ if(tx < 0 || tx >= bw) continue;
            float sx = ((float)tx + 0.5f - gx0) / s - 0.5f; int ixx = (int)floorf(sx); float fxx = sx - (float)ixx;
            float acc = 0;
            for(int dy = 0; dy < 2; dy++) for(int dx = 0; dx < 2; dx++){
                int X = ixx + dx, Y = iy + dy; if(X < 0 || Y < 0 || X >= G->w || Y >= G->h) continue;
                unsigned idx = G->off + (unsigned)(Y * G->w + X);
                unsigned v = (subovl_font_bits[idx >> 1] >> ((idx & 1) ? 0 : 4)) & 15;
                acc += v * 17.f * (dx ? fxx : 1 - fxx) * (dy ? fy : 1 - fy); }
            unsigned char v = (unsigned char)(acc > 255 ? 255 : acc + 0.5f);
            if(v > cov[ty * bw + tx]) cov[ty * bw + tx] = v; } }
}

/* ---------------------------------------------------------------------------------------------------- */
/* layout + composition                                                                                 */
static uint32_t utf8_next(const unsigned char** pp){
    const unsigned char* p = *pp; uint32_t c = *p++;
    if(c >= 0xC0 && c < 0xE0 && (p[0] & 0xC0) == 0x80){ c = ((c & 31) << 6) | (p[0] & 63); p += 1; }
    else if(c >= 0xE0 && c < 0xF0 && (p[0] & 0xC0) == 0x80 && (p[1] & 0xC0) == 0x80){ c = ((c & 15) << 12) | ((uint32_t)(p[0] & 63) << 6) | (p[1] & 63); p += 2; }
    else if(c >= 0xF0 && c < 0xF8 && (p[0] & 0xC0) == 0x80 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80){
        c = ((c & 7) << 18) | ((uint32_t)(p[0] & 63) << 12) | ((uint32_t)(p[1] & 63) << 6) | (p[2] & 63); p += 3; }
    else if(c >= 0x80) c = 0xFFFD;
    *pp = p; return c;
}
static int is_cjk(uint32_t c){ return (c >= 0x2E80 && c <= 0x9FFF) || (c >= 0xAC00 && c <= 0xD7AF) || (c >= 0xF900 && c <= 0xFAFF) || (c >= 0xFF00 && c <= 0xFFEF) || (c >= 0x20000 && c <= 0x2FFFF); }

typedef struct { int g; uint32_t cp; float adv; } gl;
typedef struct { int a, b; float w; } line;       /* glyph range [a,b) in the glyph array */

typedef struct { int x, y, w, h; uint32_t* px; int nsent; int sidx[6]; uint32_t sval[6]; } tile;

static float range_width(const gl* G, int a, int b){
    float w = 0; for(int i = a; i < b; i++){ w += G[i].adv; if(i > a) w += glyph_kern(G[i-1].g, G[i].g); } return w; }

/* greedy word wrap of a paragraph G[a,b) into lines (appended to L, up to MAXLINES total) */
static int wrap(const gl* G, int a, int b, float maxw, line* L, int nl){
    while(a < b && G[a].cp == ' ') a++;
    while(a < b && nl < MAXLINES){
        int i = a, brk = -1; float w = 0;
        for(; i < b; i++){
            if(i > a && (G[i].cp == ' ' || is_cjk(G[i].cp) || is_cjk(G[i-1].cp))) brk = i;
            float add = G[i].adv + (i > a ? glyph_kern(G[i-1].g, G[i].g) : 0);
            if(w + add > maxw && i > a) break;
            w += add;
        }
        int e = i;
        if(i < b){ if(brk > a) e = brk; }          /* else: one word wider than the line -> hard break at i */
        int te = e; while(te > a && G[te-1].cp == ' ') te--;
        L[nl].a = a; L[nl].b = te; L[nl].w = range_width(G, a, te); nl++;
        a = e; while(a < b && G[a].cp == ' ') a++;
    }
    return nl;
}

/* render text into a new tile (pixels 0xAARRGGBB, white fill + black outline/shadow). Returns 0 if nothing to draw. */
static int compose(const char* text, int top, tile* T){
    memset(T, 0, sizeof *T);
    size_t n = strlen(text); if(!n) return 0;
    gl* G = malloc(sizeof(gl) * (n + 1)); int ng = 0;
    int para[64][2]; int np = 0, ps = 0;
    for(const unsigned char* p = (const unsigned char*)text; *p; ){
        uint32_t c = utf8_next(&p);
        if(c == '\n'){ if(np < 64){ para[np][0] = ps; para[np][1] = ng; np++; } ps = ng; continue; }
        if(c == '\t') c = ' ';
        if(c < 0x20 || c == 0x200B || c == 0x200E || c == 0x200F || c == 0xFEFF) continue;
        G[ng].cp = c; G[ng].g = glyph_of(c); G[ng].adv = glyph_adv(G[ng].g); ng++;
    }
    if(np < 64){ para[np][0] = ps; para[np][1] = ng; np++; }
    float maxw = FBW * (float)O.width_pct / 100.f;
    line L[MAXLINES]; int nl = 0;
    for(int k = 0; k < np && nl < MAXLINES; k++) nl = wrap(G, para[k][0], para[k][1], maxw, L, nl);
    int dropped = 0; for(int k = 0; k < np; k++) if(para[k][1] > para[k][0]) dropped++;
    float widest = 0; for(int k = 0; k < nl; k++) if(L[k].w > widest) widest = L[k].w;
    if(nl == 0 || widest <= 0){ free(G); return 0; }
    if(dropped > nl) LV("more than %d lines: extra lines dropped", MAXLINES);

    int R = O.outline, S = O.shadow, pad = R + S + 3;
    int lineh = (int)lroundf(O.size * 1.22f);
    int asc = (int)ceilf(g_asc), desc = (int)ceilf(g_desc);
    int bw = (int)ceilf(widest) + 2 * pad, bh = pad + asc + (nl - 1) * lineh + desc + pad;
    if(bw > FBW) bw = FBW;
    if(bh > FBH / 2) bh = FBH / 2;
    unsigned char* cov = calloc((size_t)bw * (size_t)bh, 1);
    unsigned char* blk = calloc((size_t)bw * (size_t)bh, 1);
    for(int k = 0; k < nl; k++){
        float x = ((float)bw - L[k].w) * 0.5f; int by = pad + asc + k * lineh;
        for(int i = L[k].a; i < L[k].b; i++){
            if(i > L[k].a) x += glyph_kern(G[i-1].g, G[i].g);
            if(G[i].cp != ' ') glyph_draw(cov, bw, bh, G[i].g, x, by);
            x += G[i].adv;
        }
    }
    free(G);
    /* outline: soft disc dilation of the fill coverage, radius R (weight 1 inside R, linear falloff to R+1) */
    for(int dy = -R - 1; dy <= R + 1; dy++) for(int dx = -R - 1; dx <= R + 1; dx++){
        float dist = sqrtf((float)(dx * dx + dy * dy)); float wgt = (float)R + 1.f - dist; if(wgt <= 0) continue; if(wgt > 1) wgt = 1;
        unsigned W8 = (unsigned)(wgt * 256.f);
        int ya = dy < 0 ? -dy : 0, yb = dy > 0 ? bh - dy : bh, xa = dx < 0 ? -dx : 0, xb = dx > 0 ? bw - dx : bw;
        for(int y = ya; y < yb; y++){ const unsigned char* src = cov + (y + dy) * bw + dx; unsigned char* dst = blk + y * bw;
            for(int x = xa; x < xb; x++){ unsigned v = (src[x] * W8) >> 8; if(v > dst[x]) dst[x] = (unsigned char)v; } }
    }
    T->w = bw; T->h = bh; T->px = malloc(sizeof(uint32_t) * (size_t)bw * (size_t)bh);
    uint32_t fr = (O.color >> 16) & 255, fg = (O.color >> 8) & 255, fb = O.color & 255;
    for(int y = 0; y < bh; y++) for(int x = 0; x < bw; x++){
        float f = cov[y * bw + x] / 255.f, o = blk[y * bw + x] / 255.f;
        if(S && x >= S && y >= S){ float s = blk[(y - S) * bw + (x - S)] * (0.55f / 255.f); if(s > o) o = s; }
        float A = f + o * (1.f - f);
        uint32_t v = 0;
        if(A > 0.002f){ float c = f / A;
            uint32_t a8 = (uint32_t)lroundf(A * 255.f);
            v = (a8 << 24) | ((uint32_t)lroundf(fr * c) << 16) | ((uint32_t)lroundf(fg * c) << 8) | (uint32_t)lroundf(fb * c); }
        T->px[y * bw + x] = v;
    }
    free(cov); free(blk);
    T->x = (FBW - bw) / 2;
    T->y = top ? O.margin : FBH - O.margin - bh;
    if(T->y < 0) T->y = 0;
    if(T->y + bh > FBH) T->y = FBH - bh;
    /* sentinels: up to 6 fully opaque white-ish pixels (glyph cores), used to detect that someone cleared LOSD0 */
    for(int r = 1; r <= 3 && T->nsent < 6; r++){ int y = bh * r / 4;
        for(int x = 0; x < bw && T->nsent < 6; x++){ uint32_t v = T->px[y * bw + x];
            if((v >> 24) == 255 && (v & 0xFFFFFF) && (T->nsent == 0 || T->nsent % 2 == 1 || x > bw / 2)){
                T->sidx[T->nsent] = (T->y + y) * FBW + T->x + x; T->sval[T->nsent] = v; T->nsent++; x += bw / 3; } } }
    return 1;
}

/* ---------------------------------------------------------------------------------------------------- */
/* LOSD0 access                                                                                         */
static volatile uint32_t* g_fb;
static int g_exfd = -1;
static int fb_map(void){
#ifdef HOST
    const char* f = getenv("SUBOVL_FAKEFB");
    if(f){ int fd = open(f, O_RDWR | O_CREAT, 0644); if(fd < 0) return -1; if(ftruncate(fd, (off_t)LEN) < 0){ close(fd); return -1; }
        void* m = mmap(0, LEN, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0); close(fd); if(m == MAP_FAILED) return -1; g_fb = m; }
    else { g_fb = calloc(LEN, 1); if(!g_fb) return -1; }
    /* pre-fill with a "stale" pattern so the start-up clear is visible in tests */
    return 0;
#else
    g_exfd = open("/dev/exmem", O_RDWR | O_SYNC | O_CLOEXEC); if(g_exfd < 0){ lg("open /dev/exmem: %s", strerror(errno)); return -1; }
    void* m = mmap64(NULL, LEN, PROT_READ | PROT_WRITE, MAP_SHARED, g_exfd, (off64_t)LOSD0_PHYS);
    if(m == MAP_FAILED){ lg("mmap LOSD0: %s", strerror(errno)); close(g_exfd); g_exfd = -1; return -1; }
    g_fb = m; return 0;
#endif
}
static void fb_clear_all(void){ for(long i = 0; i < (long)FBW * FBH; i++) g_fb[i] = 0; }

typedef struct { tile cur; int shown; uint32_t key; } slot;     /* 0 = bottom, 1 = top */
static slot g_slot[2];
static uint32_t g_row[FBW];

/* replace the slot's old box with the new tile in one pass: rows of union(old, new); pixels outside new = 0 */
static void slot_show(slot* s, tile* nt){
    int ox0 = s->shown ? s->cur.x : 0, oy0 = s->shown ? s->cur.y : 0, ox1 = s->shown ? s->cur.x + s->cur.w : 0, oy1 = s->shown ? s->cur.y + s->cur.h : 0;
    int nx0 = nt ? nt->x : 0, ny0 = nt ? nt->y : 0, nx1 = nt ? nt->x + nt->w : 0, ny1 = nt ? nt->y + nt->h : 0;
    int ux0, uy0, ux1, uy1;
    if(s->shown && nt){ ux0 = ox0 < nx0 ? ox0 : nx0; uy0 = oy0 < ny0 ? oy0 : ny0; ux1 = ox1 > nx1 ? ox1 : nx1; uy1 = oy1 > ny1 ? oy1 : ny1; }
    else if(s->shown){ ux0 = ox0; uy0 = oy0; ux1 = ox1; uy1 = oy1; }
    else if(nt){ ux0 = nx0; uy0 = ny0; ux1 = nx1; uy1 = ny1; }
    else return;
    if(ux0 < 0) ux0 = 0; if(uy0 < 0) uy0 = 0; if(ux1 > FBW) ux1 = FBW; if(uy1 > FBH) uy1 = FBH;
    int uw = ux1 - ux0;
    for(int y = uy0; y < uy1; y++){
        int inrow = nt && y >= ny0 && y < ny1;
        int inold = s->shown && y >= oy0 && y < oy1;
        if(!inrow && !inold) continue;
        /* rows only touched by the new tile need only its own span */
        int a = inold ? ux0 : nx0, b = inold ? ux1 : nx1; if(a < 0) a = 0; if(b > FBW) b = FBW;
        if(!inold){ a = nx0 > ux0 ? nx0 : ux0; b = nx1 < ux1 ? nx1 : ux1; }
        for(int x = a; x < b; x++) g_row[x - ux0] = 0;
        if(inrow){ int xa = nx0 > a ? nx0 : a, xb = nx1 < b ? nx1 : b;
            if(xb > xa) memcpy(g_row + (xa - ux0), nt->px + (size_t)(y - ny0) * (size_t)nt->w + (size_t)(xa - nx0), sizeof(uint32_t) * (size_t)(xb - xa)); }
        memcpy((void*)(g_fb + (size_t)y * FBW + (size_t)a), g_row + (a - ux0), sizeof(uint32_t) * (size_t)(b - a));
    }
    (void)uw;
    free(s->cur.px); memset(&s->cur, 0, sizeof s->cur); s->shown = 0;
    if(nt){ s->cur = *nt; s->shown = 1; nt->px = 0; }
}
static int slot_intact(const slot* s){
    if(!s->shown) return 1;
    for(int i = 0; i < s->cur.nsent; i++) if(g_fb[s->cur.sidx[i]] != s->cur.sval[i]) return 0;
    return 1;
}
static void slot_redraw(slot* s){
    if(!s->shown) return;
    for(int y = 0; y < s->cur.h; y++)
        memcpy((void*)(g_fb + (size_t)(s->cur.y + y) * FBW + (size_t)s->cur.x), s->cur.px + (size_t)y * (size_t)s->cur.w, sizeof(uint32_t) * (size_t)s->cur.w);
}

/* ---------------------------------------------------------------------------------------------------- */
/* plane 7 (libavio): one short balanced Open..Close window per check                                   */
static int (*avOpen)(int); static int (*avClose)(void); static int (*avGet)(unsigned, unsigned*); static int (*avSet)(unsigned, unsigned*, int);
static long g_repoints, g_checks;
#ifdef HOST
static unsigned fake_p7[4] = { 1, GS_BUF_A, 0, 0 }; static int fake_opens, fake_flip_every;
static int fOpen(int x){ (void)x; fake_opens++; return 0; }
static int fClose(void){ fake_opens--; return 0; }
static int fGet(unsigned pl, unsigned* d){ (void)pl;
    static int n; if(fake_flip_every && ++n % fake_flip_every == 0){ fake_p7[1] = (n / fake_flip_every) & 1 ? GS_BUF_B : GS_BUF_A; lg("HOST: fake gserver flipped plane 7 to 0x%08x", fake_p7[1]); }
    memcpy(d, fake_p7, sizeof fake_p7); return 0; }
static int fSet(unsigned pl, unsigned* d, int c){ (void)pl; (void)c; memcpy(fake_p7, d, sizeof fake_p7); return 0; }
#endif
static int avio_load(void){
#ifdef HOST
    avOpen = fOpen; avClose = fClose; avGet = fGet; avSet = fSet;
    const char* e = getenv("SUBOVL_FAKE_FLIP"); fake_flip_every = e ? atoi(e) : 0; return 0;
#else
    void* a = dlopen("/usr/dtvrec/lib/libavio.so", RTLD_LAZY | RTLD_GLOBAL); if(!a){ lg("dlopen libavio failed"); return -1; }
    avOpen = (int(*)(int))dlsym(a, "AVIO_UP_Open"); avClose = (int(*)(void))dlsym(a, "AVIO_UP_Close");
    avGet = (int(*)(unsigned, unsigned*))dlsym(a, "_AVIO_UP_GFX_Get_Addr"); avSet = (int(*)(unsigned, unsigned*, int))dlsym(a, "AVIO_UP_GFX_Set_Addr");
    if(!avOpen || !avClose || !avGet || !avSet){ lg("dlsym libavio failed"); return -1; }
    return 0;
#endif
}
static int find_pid(const char* name){
    DIR* d = opendir("/proc"); if(!d) return -1; struct dirent* e; int found = -1; char path[64], buf[64];
    while((e = readdir(d))){ if(e->d_name[0] < '0' || e->d_name[0] > '9') continue;
        snprintf(path, sizeof path, "/proc/%s/comm", e->d_name);
        int f = open(path, O_RDONLY | O_CLOEXEC); if(f < 0) continue; int n = (int)read(f, buf, sizeof buf - 1); close(f); if(n <= 0) continue;
        buf[n] = 0; if(buf[n-1] == '\n') buf[n-1] = 0;
        if(!strcmp(buf, name)){ found = atoi(e->d_name); break; } }
    closedir(d); return found;
}
static int g_gpid = -1;
static int gserver_alive(void){
#ifdef HOST
    return 1;
#else
    if(g_gpid > 0 && kill(g_gpid, 0) == 0) return 1;
    g_gpid = find_pid("gserver"); return g_gpid > 0;
#endif
}
/* returns 1 = plane 7 is (now) LOSD0, 0 = left alone (unexpected state / Open failed), -1 = gserver gone */
static unsigned g_last_odd = 0xFFFFFFFFu;
static int g_lastsig[4] = { 99, 99, 99, 99 };
static int plane_check(void){
    if(!gserver_alive()) return -1;
    g_checks++;
    int r = avOpen(0);
    if(r != 0){ lg("AVIO_UP_Open=%d: skipping this plane check", r); g_lastsig[0] = r; return 0; }   /* no Get/Set on a failed Open */
    unsigned cur[4] = {0, 0, 0, 0}; int res = 0;
    int gr = avGet(7, cur);
    if(g_lastsig[0] != r || g_lastsig[1] != gr || g_lastsig[2] != (int)cur[0] || g_lastsig[3] != (int)cur[1]){   /* first check / change */
        lg("plane check #%ld: AVIO_UP_Open=%d Get_Addr(7)=%d sel=%u addr=0x%08x%s", g_checks, r, gr, cur[0], cur[1], cur[1] == LOSD0_BUS ? " (LOSD0)" : "");
        g_lastsig[0] = r; g_lastsig[1] = gr; g_lastsig[2] = (int)cur[0]; g_lastsig[3] = (int)cur[1]; }
    if(gr == 0 && cur[0] == 1 && cur[1] == LOSD0_BUS) res = 1;
    else if(gr == 0 && cur[0] == 1 && (cur[1] == GS_BUF_A || cur[1] == GS_BUF_B)){
        unsigned d[4] = { 1, LOSD0_BUS, 0, 0 };
        int sr = avSet(7, d, 1); g_repoints++;
        lg("plane 7 was on gserver buffer 0x%08x: re-pointed to LOSD0 (Set_Addr=%d, #%ld)", cur[1], sr, g_repoints);
        res = sr == 0;
    } else if(cur[1] != g_last_odd){
        lg("plane 7 in an unexpected state (Get=%d sel=%u addr=0x%08x): left alone", gr, cur[0], cur[1]);
        g_last_odd = cur[1];
    }
    if(res) g_last_odd = 0xFFFFFFFFu;
    r = avClose(); if(r != 0) lg("AVIO_UP_Close=%d", r);
    if(res && cur[1] != LOSD0_BUS){ g_lastsig[3] = (int)LOSD0_BUS; }      /* we just re-pointed it: don't re-log the expected state */
    return res;
}

/* ---------------------------------------------------------------------------------------------------- */
/* status file                                                                                          */
typedef struct { int valid; char state[32]; int has_pos; double pos; double mtime; int rc; } pstatus;
static double g_t_start_real;
static void read_status(pstatus* st){
    memset(st, 0, sizeof *st); st->rc = -999;
    int fd = open(O.status, O_RDONLY | O_CLOEXEC); if(fd < 0) return;     /* re-opened every poll: the player rename()s it */
    struct stat sb; char b[600]; int n = 0;
    if(fstat(fd, &sb) == 0){ n = (int)read(fd, b, sizeof b - 1); }
    close(fd); if(n <= 0) return;
    b[n] = 0;
    st->mtime = sb.st_mtim.tv_sec + sb.st_mtim.tv_nsec * 1e-9;
    if(st->mtime < g_t_start_real - 0.05) return;        /* written before we started: stale (previous run / session) */
    const char* p = strstr(b, "state="); if(!p) return;
    p += 6; int k = 0; while(*p && *p != ' ' && *p != '\n' && k < 31) st->state[k++] = *p++; st->state[k] = 0;
    const char* q = strstr(b, " pos="); if(q){ char* e; double v = strtod(q + 5, &e); if(e != q + 5){ st->pos = v; st->has_pos = 1; } }
    q = strstr(b, " rc="); if(q && q[4] >= '0' && q[4] <= '9') st->rc = atoi(q + 4);
    st->valid = 1;
}
static double read_base(void){
    char b[64]; int fd = open(O.base, O_RDONLY | O_CLOEXEC); if(fd < 0) return 0;
    int n = (int)read(fd, b, sizeof b - 1); close(fd); if(n <= 0) return 0; b[n] = 0; return strtod(b, 0);
}

/* ---------------------------------------------------------------------------------------------------- */
/* cue selection: join all cues active at t (per slot) in start order; key = FNV hash of the joined text */
static uint32_t fnv(const char* s, uint32_t h){ while(*s){ h ^= (unsigned char)*s++; h *= 16777619u; } return h; }
static char* active_text(double t, int top, uint32_t* key){
    size_t len = 0; int cnt = 0;
    for(int i = 0; i < g_ncues && g_cues[i].t0 <= t; i++) if(t < g_cues[i].t1 && g_cues[i].top == top){ len += strlen(g_cues[i].text) + 1; cnt++; }
    if(!cnt){ *key = 0; return 0; }
    char* s = malloc(len + 1); size_t k = 0;
    for(int i = 0; i < g_ncues && g_cues[i].t0 <= t; i++) if(t < g_cues[i].t1 && g_cues[i].top == top){
        if(k) s[k++] = '\n'; size_t l = strlen(g_cues[i].text); memcpy(s + k, g_cues[i].text, l); k += l; }
    s[k] = 0; *key = fnv(s, 2166136261u) | 1; return s;
}
/* make the slots show what is active at t (t < 0: nothing) */
static void update_slots(double t){
    for(int sl = 0; sl < 2; sl++){
        uint32_t key = 0; char* txt = t >= 0 ? active_text(t, sl, &key) : 0;
        slot* s = &g_slot[sl];
        if(key == s->key){ free(txt); continue; }
        if(txt){
            double c0 = mono(); tile nt; int ok = compose(txt, sl, &nt);
            if(g_verbose){ char one[160]; size_t i = 0; for(const char* p = txt; *p && i < sizeof one - 1; p++) one[i++] = *p == '\n' ? '|' : *p; one[i] = 0;
                lg("t=%.3f %s: \"%s\" (%dx%d at %d,%d, %.0f ms)", t, sl ? "top" : "bottom", one, nt.w, nt.h, nt.x, nt.y, (mono() - c0) * 1000); }
            slot_show(s, ok ? &nt : 0); free(nt.px);
        } else { if(s->shown) LV("t=%.3f %s: clear", t, sl ? "top" : "bottom"); slot_show(s, 0); }
        s->key = key; free(txt);
    }
}

/* ---------------------------------------------------------------------------------------------------- */
static void usage(void){
    lw("usage: subovl <file.srt|.vtt> [--size PX] [--margin PX] [--status PATH] [--base-file PATH] [--offset SEC]\n"
       "              [--font PATH] [--font-index N] [--width PCT] [--outline PX] [--shadow PX] [--color 0xRRGGBB]\n"
       "              [--no-kern] [--verbose]\n"
#ifdef HOST
       "  HOST: --render-at SEC --out FILE.raw [--text \"a\\nb\"]   (SUBOVL_FAKEFB=file, SUBOVL_FAKE_FLIP=N)\n"
#endif
       );
}
int main(int argc, char** argv){
    O.status = "/tmp/ub820play.status"; O.base = "/tmp/ub820run.base"; O.size = 54; O.margin = 64; O.width_pct = 80;
    O.outline = -1; O.shadow = -1; O.kern = 1; O.color = 0xFFFFFF; O.render_at = -1;
    for(int i = 1; i < argc; i++){
        const char* a = argv[i]; const char* v = i + 1 < argc ? argv[i+1] : 0;
#define ARG(name) (!strcmp(a, name) && v && (i++, 1))
        if(ARG("--size")) O.size = atoi(v);
        else if(ARG("--margin")) O.margin = atoi(v);
        else if(ARG("--status")) O.status = v;
        else if(ARG("--base-file")) O.base = v;
        else if(ARG("--offset")) O.offset = strtod(v, 0);
        else if(ARG("--font")) O.font = v;
        else if(ARG("--font-index")) O.font_index = atoi(v);
        else if(ARG("--width")) O.width_pct = atoi(v);
        else if(ARG("--outline")) O.outline = atoi(v);
        else if(ARG("--shadow")) O.shadow = atoi(v);
        else if(ARG("--color")) O.color = (uint32_t)strtoul(v, 0, 0) & 0xFFFFFF;
        else if(ARG("--render-at")) O.render_at = strtod(v, 0);
        else if(ARG("--out")) O.out = v;
        else if(ARG("--text")) O.text = v;
        else if(!strcmp(a, "--no-kern")) O.kern = 0;
        else if(!strcmp(a, "--verbose") || !strcmp(a, "-v")) g_verbose = 1;
        else if(!strcmp(a, "-h") || !strcmp(a, "--help")){ usage(); return 0; }
        else if(a[0] == '-' && a[1]){ lg("unknown or incomplete option %s", a); usage(); return 1; }
        else O.srt = a;
#undef ARG
    }
    if(!O.srt && !O.text){ usage(); return 1; }
    if(O.size < 12) O.size = 12; if(O.size > 160) O.size = 160;
    if(O.width_pct < 20) O.width_pct = 20; if(O.width_pct > 100) O.width_pct = 100;
    if(O.margin < 0) O.margin = 0; if(O.margin > FBH / 2) O.margin = FBH / 2;
    if(O.outline < 0){ O.outline = (int)lroundf(O.size * 0.06f); if(O.outline < 2) O.outline = 2; }
    if(O.shadow < 0){ O.shadow = (int)lroundf(O.size * 0.04f); if(O.shadow < 1) O.shadow = 1; }
    if(O.outline > 12) O.outline = 12; if(O.shadow > 12) O.shadow = 12;
    struct sigaction sa; memset(&sa, 0, sizeof sa); sa.sa_handler = onsig;
    sigaction(SIGTERM, &sa, 0); sigaction(SIGINT, &sa, 0); sigaction(SIGHUP, &sa, 0);
    signal(SIGPIPE, SIG_IGN);

    font_init();

#ifdef HOST
    if(O.render_at >= 0 || O.text){
        if(fb_map() < 0){ lg("fake fb failed"); return 2; }
        fb_clear_all();
        if(O.text){
            /* "\n" in the argument = newline */
            char* t = strdup(O.text); char* w = t; for(const char* p = O.text; *p; p++){ if(p[0] == '\\' && p[1] == 'n'){ *w++ = '\n'; p++; } else *w++ = *p; } *w = 0;
            int top; char* c = clean_text(t, &top); free(t);
            tile nt; double c0 = mono(); if(compose(c, top, &nt)){ slot_show(&g_slot[top], &nt); }
            lg("rendered --text (%dx%d at %d,%d) in %.1f ms", g_slot[top].cur.w, g_slot[top].cur.h, g_slot[top].cur.x, g_slot[top].cur.y, (mono() - c0) * 1000);
            free(c);
        } else {
            load_subs(1); double c0 = mono(); update_slots(O.render_at);
            lg("rendered cues active at %.3f in %.1f ms", O.render_at, (mono() - c0) * 1000);
        }
        if(O.out){ int fd = open(O.out, O_WRONLY | O_CREAT | O_TRUNC, 0644); if(fd < 0){ lg("open %s failed", O.out); return 3; }
            const char* p = (const char*)g_fb; size_t left = (size_t)FBW * FBH * 4;
            while(left){ ssize_t r = write(fd, p, left); if(r <= 0) break; p += r; left -= (size_t)r; } close(fd);
            lg("wrote %s (1920x1080 bgra)", O.out); }
        return 0;
    }
#endif

    g_t_start_real = realnow();
    if(fb_map() < 0) return 2;
    if(avio_load() < 0) return 3;
    if(!gserver_alive()){ lg("gserver not running (standby?): nothing to overlay, exiting"); return 4; }
    load_subs(1);
    fb_clear_all();
    int pc = plane_check();
    lg("started: %s, size %d, margin %d, outline %d, shadow %d, offset %+.3f s, plane 7 %s", O.srt, O.size, O.margin, O.outline, O.shadow, O.offset,
       pc == 1 ? "-> LOSD0" : "left alone");

    double next_plane = mono() + 1.0, next_reload = mono() + 1.0, done_t = 0, anchor_t = 0, anchor_pos = 0;
    int live_seen = 0; char last_state[32] = ""; double last_pos = -1, last_mtime = 0; const char* why = "signal";
    double tick = mono();
    while(!g_stop){
        double now = mono();
        if(now >= next_reload){ load_subs(0); next_reload = now + 1.0; if(g_slot[0].key || g_slot[1].key){ /* text may have changed */ } }
        if(now >= next_plane){
            int r = plane_check(); next_plane = now + 1.0;
            if(r < 0){ why = "gserver gone"; break; }
        }
        pstatus st; read_status(&st);
        int playing = 0, paused = 0, show = 0;
        if(st.valid){
            if(!strcmp(st.state, "done")){
                if(live_seen){ if(!done_t) done_t = now; if(now - done_t > 2.0){ why = "player done"; break; } }
            } else { live_seen = 1; done_t = 0; }
            playing = !strcmp(st.state, "playing") || !strcmp(st.state, "draining");
            paused = !strcmp(st.state, "playing-paused") || !strcmp(st.state, "draining-paused");
            show = (playing || paused) && st.has_pos;
            if(show && (st.mtime != last_mtime) && (st.pos != last_pos || strcmp(st.state, last_state))){
                double age = realnow() - st.mtime; if(age < 0) age = 0; if(age > 0.5) age = 0.5;
                anchor_pos = st.pos; anchor_t = now - (playing ? age : 0);
                if(strcmp(st.state, last_state)) LV("state %s pos=%.3f", st.state, st.pos);
            }
            if(strcmp(st.state, last_state)){ if(!show) LV("state %s", st.state); memcpy(last_state, st.state, sizeof last_state); }
            last_pos = st.pos; last_mtime = st.mtime;
        }
        double t = -1;
        if(show){
            double ex = (playing && anchor_pos > 0) ? now - anchor_t : 0; if(ex < 0) ex = 0; if(ex > 1.5) ex = 1.5;
            t = read_base() + anchor_pos + ex + O.offset;
        }
        uint32_t k0 = g_slot[0].key, k1 = g_slot[1].key;
        update_slots(t);
        if(k0 == g_slot[0].key && k1 == g_slot[1].key)
            for(int sl = 0; sl < 2; sl++) if(!slot_intact(&g_slot[sl])){ LV("LOSD0 was overwritten: redrawing the %s cue", sl ? "top" : "bottom"); slot_redraw(&g_slot[sl]); }
        tick += 0.1; double d = tick - mono(); if(d < 0){ tick = mono(); d = 0; }
        sleep_s(d);
    }
    for(int sl = 0; sl < 2; sl++) slot_show(&g_slot[sl], 0);
    fb_clear_all();                       /* plane 7 left alone: gserver re-flips it on its next draw */
    lg("exit (%s): plane checks %ld, re-points %ld%s", why, g_checks, g_repoints,
#ifdef HOST
       fake_opens ? " AVIO UNBALANCED!" : " (host: AVIO balanced)"
#else
       ""
#endif
       );
    return 0;
}
