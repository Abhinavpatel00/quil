/* heapprof.c -- LD_PRELOAD malloc interposer that attributes LIVE heap bytes
 * to allocation call sites, snapshotted at the moment total live heap peaks.
 *
 * That is the number that matters: it explains the kernel's [heap] mapping and
 * the process's Private_Dirty pages, and it is what you can actually shrink.
 *
 * Note it also sees malloc traffic from shared libraries (Mesa/LLVM, fontconfig,
 * HarfBuzz). Those show up as offsets inside the .so, not inside ./quill, and
 * are labelled by map lookup, so they cannot be mistaken for app allocations.
 *
 * Build:  cc -shared -fPIC -O2 -o libheapprof.so heapprof.c
 * Run:    LD_PRELOAD=$PWD/libheapprof.so ./quill --headless ...
 * Read:   symbolise offsets with  addr2line -e quill -f -C -i <offset>
 *         (offsets are already relative to the PIE load base)
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <signal.h>

extern void *__libc_malloc(size_t);
extern void  __libc_free(void *);
extern void *__libc_calloc(size_t, size_t);
extern void *__libc_realloc(void *, size_t);
extern void *__libc_memalign(size_t, size_t);

#define PTR_BITS 21
#define PTR_SIZE (1u << PTR_BITS)
#define SITE_MAX 16384

typedef struct { void *p; size_t sz; void *ra; } Rec;
typedef struct { void *ra; size_t bytes, count, peak_bytes; } Site;

static Rec  tbl[PTR_SIZE];
static Site agg[SITE_MAX];
static Site snap[SITE_MAX];
static size_t nagg, nsnap, snap_live, snap_count;

static size_t live_bytes, live_count, peak_bytes;
static uintptr_t load_base;
static int in_hook, found_base;

static inline size_t hashp(uint64_t x) {
    x ^= x >> 33; x *= 0xff51afd7ed558ccdULL; x ^= x >> 33;
    x *= 0xc4ceb9fe1a85ec53ULL; x ^= x >> 33;
    return (size_t)(x & (PTR_SIZE - 1));
}
static inline size_t hashr(void *ra) {
    return hashp((uint64_t)(uintptr_t)ra) % SITE_MAX;
}

static void find_base(void) {
    FILE *f;
    char line[512];
    if (found_base) return;
    found_base = 1;
    f = fopen("/proc/self/maps", "r");
    if (!f) return;
    while (fgets(line, sizeof line, f)) {
        char *path = strchr(line, '/');
        if (path && strstr(path, "quill")) {
            load_base = (uintptr_t)strtoull(line, NULL, 16);
            break;
        }
    }
    fclose(f);
}

static void site_add(void *ra, size_t sz, int sign) {
    size_t i;
    if (!ra) return;
    for (i = hashr(ra); i < SITE_MAX; i++) {
        if (agg[i].ra == NULL) {
            if (sign < 0) return;
            agg[i].ra = ra;
            agg[i].bytes = agg[i].count = 0;
            nagg++;
            break;
        }
        if (agg[i].ra == ra) break;
    }
    if (i >= SITE_MAX) return;
    if (sign > 0) { agg[i].bytes += sz; agg[i].count++; }
    else if (agg[i].bytes >= sz) { agg[i].bytes -= sz; if (agg[i].count) agg[i].count--; }
    else { agg[i].bytes = 0; }
}

static void snapshot_peak(void) {
    size_t i;
    nsnap = 0;
    for (i = 0; i < SITE_MAX; i++) {
        if (agg[i].ra && agg[i].bytes) {
            snap[nsnap] = agg[i];
            nsnap++;
        }
    }
    snap_live = live_bytes;
    snap_count = live_count;
    peak_bytes = live_bytes;
}

static void record(void *p, size_t sz, void *ra) {
    size_t i;
    if (!p) return;
    in_hook = 1;
    if (!load_base) find_base();
    for (i = hashp((uint64_t)(uintptr_t)p); i < PTR_SIZE; i++) {
        if (tbl[i].p == NULL) { tbl[i].p = p; tbl[i].sz = sz; tbl[i].ra = ra; break; }
        if (tbl[i].p == p)    { tbl[i].sz = sz; tbl[i].ra = ra; break; }
    }
    live_bytes += sz;
    live_count++;
    site_add(ra, sz, +1);
    if (live_bytes > peak_bytes) snapshot_peak();
    in_hook = 0;
}

static void forget(void *p) {
    size_t i;
    if (!p) return;
    in_hook = 1;
    for (i = hashp((uint64_t)(uintptr_t)p); i < PTR_SIZE; i++) {
        if (tbl[i].p == NULL) break;
        if (tbl[i].p == p) {
            if (live_bytes >= tbl[i].sz) live_bytes -= tbl[i].sz;
            else live_bytes = 0;
            if (live_count) live_count--;
            site_add(tbl[i].ra, tbl[i].sz, -1);
            tbl[i].p = NULL;
            break;
        }
    }
    in_hook = 0;
}

void *malloc(size_t sz) {
    void *p;
    if (in_hook) return __libc_malloc(sz);
    p = __libc_malloc(sz);
    record(p, sz, __builtin_return_address(0));
    return p;
}
void free(void *p) {
    if (in_hook) { __libc_free(p); return; }
    forget(p);
    __libc_free(p);
}
void *calloc(size_t n, size_t sz) {
    void *p;
    if (in_hook) return __libc_calloc(n, sz);
    p = __libc_calloc(n, sz);
    record(p, n * sz, __builtin_return_address(0));
    return p;
}
void *realloc(void *old, size_t sz) {
    void *p;
    if (in_hook) return __libc_realloc(old, sz);
    forget(old);
    p = __libc_realloc(old, sz);
    record(p, sz, __builtin_return_address(0));
    return p;
}
void *memalign(size_t a, size_t sz) {
    void *p;
    if (in_hook) return __libc_memalign(a, sz);
    p = __libc_memalign(a, sz);
    record(p, sz, __builtin_return_address(0));
    return p;
}
void *aligned_alloc(size_t a, size_t sz) { return memalign(a, sz); }
int posix_memalign(void **out, size_t a, size_t sz) {
    void *p = memalign(a, sz);
    if (!p) return 12;
    *out = p;
    return 0;
}

/* Which shared object does this address belong to? */
static const char *map_for(void *ra, uintptr_t *off) {
    static char buf[256];
    FILE *f = fopen("/proc/self/maps", "r");
    char line[512];
    uintptr_t a = (uintptr_t)ra;
    if (!f) return "?";
    while (fgets(line, sizeof line, f)) {
        unsigned long lo, hi;
        char *path;
        if (sscanf(line, "%lx-%lx", &lo, &hi) != 2) continue;
        if (a >= lo && a < hi) {
            path = strchr(line, '/');
            if (path) {
                char *nl = strchr(path, '\n');
                if (nl) *nl = 0;
                snprintf(buf, sizeof buf, "%s", path);
            } else {
                snprintf(buf, sizeof buf, "%s", a >= load_base && load_base ? "./quill" : "[anon]");
            }
            if (off) *off = a - (path ? lo : lo);
            fclose(f);
            return buf;
        }
    }
    fclose(f);
    return "?";
}

static void dump(void);
static void on_signal(int sig) { dump(); _exit(sig == SIGUSR1 ? 0 : 1); }

__attribute__((constructor))
static void install(void) {
    signal(SIGUSR1, on_signal);
    signal(SIGTERM, on_signal);
}

__attribute__((destructor))
static void dump(void) {
    char buf[512];
    size_t i, j;
    int n;

    for (i = 1; i < nsnap; i++) {
        Site k = snap[i];
        for (j = i; j > 0 && snap[j-1].bytes < k.bytes; j--) snap[j] = snap[j-1];
        snap[j] = k;
    }

    n = snprintf(buf, sizeof buf,
        "\n=== heapprof: PEAK live heap %.2f MiB in %zu allocs "
        "(load_base 0x%lx) ===\n%-34s %10s %9s\n",
        snap_live / 1048576.0, snap_count, (unsigned long)load_base,
        "site", "liveMiB", "count");
    { ssize_t r = write(2, buf, (size_t)n); (void)r; }

    in_hook = 1;
    for (i = 0; i < nsnap && i < 20; i++) {
        uintptr_t off = 0;
        const char *lib = map_for(snap[i].ra, &off);
        if (strstr(lib, "quill")) {
            n = snprintf(buf, sizeof buf, "quill+0x%-9lx %10.3f %9zu\n",
                         (unsigned long)((uintptr_t)snap[i].ra - load_base),
                         snap[i].bytes / 1048576.0, snap[i].count);
        } else {
            n = snprintf(buf, sizeof buf, "%-22s+0x%-8lx %10.3f %9zu\n",
                         strrchr(lib, '/') ? strrchr(lib, '/') + 1 : lib, (unsigned long)off,
                         snap[i].bytes / 1048576.0, snap[i].count);
        }
        { ssize_t r = write(2, buf, (size_t)n); (void)r; }
    }
    in_hook = 0;
}
