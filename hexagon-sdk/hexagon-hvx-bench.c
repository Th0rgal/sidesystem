/*
 * hexagon-hvx-bench.c — Host runner & benchmark for native Hexagon V79 1024-bit HVX kernels
 * Loads /vendor/lib64/libcdsprpc.so via dlopen(), requests an Unsigned PD on CDSP,
 * dispatches libhvx_kernels_skel.so, and benchmarks HVX vs Oryon CPU on zero-copy rpcmem buffers.
 */
#include <stdint.h>
#include <stddef.h>

/* Minimal bionic ABI declarations (zero NDK headers required) */
extern int printf(const char *fmt, ...);
extern int fprintf(void *stream, const char *fmt, ...);
extern void *dlopen(const char *filename, int flags);
extern void *dlsym(void *handle, const char *symbol);
extern char *dlerror(void);
extern int setenv(const char *name, const char *value, int overwrite);
extern void *memset(void *s, int c, size_t n);
extern void *memcpy(void *dst, const void *src, size_t n);

struct timespec { long tv_sec; long tv_nsec; };
extern int clock_gettime(int clk_id, struct timespec *tp);
#define CLOCK_MONOTONIC 1
#define RTLD_NOW 2

static double now_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

typedef uint64_t remote_handle64;
typedef union {
    struct { void *pv; size_t nLen; } buf;
    struct { int32_t fd; uint32_t offset; } dma;
} remote_arg;

#define REMOTE_SCALARS_MAKEX(attr, method, in, out, oin, oout) \
    ((((uint32_t)(attr) & 0x7) << 29) | \
     (((uint32_t)(method) & 0x1f) << 24) | \
     (((uint32_t)(in) & 0xff) << 16) | \
     (((uint32_t)(out) & 0xff) << 8) | \
     (((uint32_t)(oin) & 0xf) << 4) | \
     (((uint32_t)(oout) & 0xf) << 0))
#define REMOTE_SCALARS_MAKE(method, in, out) REMOTE_SCALARS_MAKEX(0, method, in, out, 0, 0)

/* FastRPC session control for Unsigned PD (DSPRPC_CONTROL_UNSIGNED_MODULE = 2 in remote_session_control) */
#define DSPRPC_CONTROL_UNSIGNED_MODULE 2
struct remote_rpc_control_unsigned_module {
    int domain;
    int enable;
};
#define CDSP_DOMAIN_ID 3

/* RPCMEM constants */
#define RPCMEM_HEAP_ID_SYSTEM 25
#define RPCMEM_DEFAULT_FLAGS  1

typedef int (*fn_remote_session_control)(uint32_t req, void *data, uint32_t datalen);
typedef int (*fn_remote_handle64_open)(const char *name, remote_handle64 *ph);
typedef int (*fn_remote_handle64_invoke)(remote_handle64 h, uint32_t sc, remote_arg *pra);
typedef int (*fn_remote_handle64_close)(remote_handle64 h);
typedef void (*fn_rpcmem_init)(void);
typedef void (*fn_rpcmem_deinit)(void);
typedef void *(*fn_rpcmem_alloc)(int heapid, uint32_t flags, int size);
typedef void (*fn_rpcmem_free)(void *po);

/* CPU reference implementations (for correctness & speed comparison) */
static uint32_t cpu_rgba_to_luma(const uint8_t *rgba, uint8_t *y_out, uint32_t npix) {
    uint32_t nvecs = npix / 128;
    const uint8_t *src = rgba;
    uint32_t digest = 0;
    for (uint32_t v = 0; v < nvecs; v++) {
        for (int k = 0; k < 128; k++) {
            uint8_t y = (uint8_t)((src[k] >> 2) + (src[128 + k] >> 2) + (src[256 + k] >> 2) + (src[384 + k] >> 2));
            y_out[v * 128 + k] = y;
        }
        src += 512;
    }
    const uint32_t *w = (const uint32_t *)y_out;
    for (uint32_t i = 0; i < (nvecs * 128) / 4; i++) digest ^= w[i];
    return digest;
}

static uint32_t cpu_parity(const uint8_t *a, const uint8_t *b, uint8_t *p, uint32_t nbytes) {
    uint32_t n = nbytes / 4;
    const uint32_t *wa = (const uint32_t *)a;
    const uint32_t *wb = (const uint32_t *)b;
    uint32_t *wp = (uint32_t *)p;
    uint32_t csum = 0;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t v = wa[i] ^ wb[i];
        wp[i] = v;
        csum ^= v;
    }
    return csum;
}

int main(int argc, char **argv) {
    const char *lib_dir = (argc > 1) ? argv[1] : "/data/local/nixos/hexagon";
    setenv("ADSP_LIBRARY_PATH", lib_dir, 1);
    setenv("DSP_LIBRARY_PATH", lib_dir, 1);

    void *dl = dlopen("/vendor/lib64/libcdsprpc.so", RTLD_NOW);
    if (!dl) {
        printf("ERROR: dlopen libcdsprpc.so failed: %s\n", dlerror());
        return 1;
    }

    fn_remote_session_control p_sess_ctrl = (fn_remote_session_control)dlsym(dl, "remote_session_control");
    fn_remote_handle64_open   p_open      = (fn_remote_handle64_open)dlsym(dl, "remote_handle64_open");
    fn_remote_handle64_invoke p_invoke    = (fn_remote_handle64_invoke)dlsym(dl, "remote_handle64_invoke");
    fn_remote_handle64_close  p_close     = (fn_remote_handle64_close)dlsym(dl, "remote_handle64_close");
    fn_rpcmem_init            p_mem_init  = (fn_rpcmem_init)dlsym(dl, "rpcmem_init");
    fn_rpcmem_deinit          p_mem_deinit= (fn_rpcmem_deinit)dlsym(dl, "rpcmem_deinit");
    fn_rpcmem_alloc           p_mem_alloc = (fn_rpcmem_alloc)dlsym(dl, "rpcmem_alloc");
    fn_rpcmem_free            p_mem_free  = (fn_rpcmem_free)dlsym(dl, "rpcmem_free");

    if (!p_sess_ctrl || !p_open || !p_invoke || !p_close || !p_mem_alloc) {
        printf("ERROR: missing symbols in libcdsprpc.so\n");
        return 1;
    }

    /* Request Unsigned PD on CDSP */
    struct remote_rpc_control_unsigned_module uns = { .domain = CDSP_DOMAIN_ID, .enable = 1 };
    int rc = p_sess_ctrl(DSPRPC_CONTROL_UNSIGNED_MODULE, &uns, sizeof(uns));
    printf("Unsigned PD control(CDSP): rc=%d\n", rc);

    if (p_mem_init) p_mem_init();

    const char *uri = "file:///libhvx_kernels_skel.so?hvx_kernels_skel_handle_invoke&_modver=1.0&_dom=cdsp";
    remote_handle64 h = 0;
    rc = p_open(uri, &h);
    printf("remote_handle64_open(%s): rc=%d handle=0x%llx\n", uri, rc, (unsigned long long)h);
    if (rc != 0) {
        /* Try fallback URI without _dom */
        const char *uri2 = "file:///libhvx_kernels_skel.so?hvx_kernels_skel_handle_invoke&_modver=1.0";
        rc = p_open(uri2, &h);
        printf("fallback open(%s): rc=%d handle=0x%llx\n", uri2, rc, (unsigned long long)h);
        if (rc != 0) return 2;
    }

    /* Method 2: Ping Hexagon V79 HVX + Turbo clock + 4 MiB VTCM probe */
    uint32_t *ping_out = (uint32_t *)p_mem_alloc(RPCMEM_HEAP_ID_SYSTEM, RPCMEM_DEFAULT_FLAGS, 4096);
    memset(ping_out, 0, 64);
    remote_arg pra_ping[1];
    pra_ping[0].buf.pv = ping_out;
    pra_ping[0].buf.nLen = 24;
    rc = p_invoke(h, REMOTE_SCALARS_MAKE(2, 0, 1), pra_ping);
    printf("PING rc=%d: magic=0x%08x hvx_units=0x%x max_clk=%u MHz max_bus=%u MHz vtcm_4MiB=%u workers=%u\n",
           rc, ping_out[0], ping_out[1], ping_out[2], ping_out[3], ping_out[4], ping_out[5]);

    /* Benchmark 1: 3440x1440 RGBA -> Luma + XOR digest (19.8 MB in, 4.95 MB out) */
    uint32_t npix = 3440u * 1440u;
    npix = (npix / 128u) * 128u;
    uint32_t rgba_bytes = npix * 4u;
    uint32_t y_bytes = npix;
    uint8_t *rgba = (uint8_t *)p_mem_alloc(RPCMEM_HEAP_ID_SYSTEM, RPCMEM_DEFAULT_FLAGS, rgba_bytes);
    uint8_t *y_dsp = (uint8_t *)p_mem_alloc(RPCMEM_HEAP_ID_SYSTEM, RPCMEM_DEFAULT_FLAGS, y_bytes);
    uint8_t *y_cpu = (uint8_t *)p_mem_alloc(RPCMEM_HEAP_ID_SYSTEM, RPCMEM_DEFAULT_FLAGS, y_bytes);
    uint32_t *meta = (uint32_t *)p_mem_alloc(RPCMEM_HEAP_ID_SYSTEM, RPCMEM_DEFAULT_FLAGS, 4096);

    uint32_t rng = 0x12345678u;
    for (uint32_t i = 0; i < rgba_bytes / 4; i++) {
        rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
        ((uint32_t *)rgba)[i] = rng;
    }

    remote_arg pra_luma[3];
    pra_luma[0].buf.pv = rgba;  pra_luma[0].buf.nLen = rgba_bytes;
    pra_luma[1].buf.pv = y_dsp; pra_luma[1].buf.nLen = y_bytes;
    pra_luma[2].buf.pv = meta;  pra_luma[2].buf.nLen = 8;

    /* Warmup */
    rc = p_invoke(h, REMOTE_SCALARS_MAKE(3, 1, 2), pra_luma);
    int iters = 30;
    double t0 = now_sec();
    for (int it = 0; it < iters; it++) {
        rc = p_invoke(h, REMOTE_SCALARS_MAKE(3, 1, 2), pra_luma);
    }
    double dt_dsp = (now_sec() - t0) / iters;
    uint32_t dsp_digest = meta[1];

    double tc0 = now_sec();
    uint32_t cpu_digest = 0;
    for (int it = 0; it < 10; it++) {
        cpu_digest = cpu_rgba_to_luma(rgba, y_cpu, npix);
    }
    double dt_cpu = (now_sec() - tc0) / 10.0;

    printf("KERNEL1 [3440x1440 RGBA->Luma+Digest]: HVX=%.3f ms (%.2f GB/s) | CPU=%.3f ms (%.2f GB/s) | speedup=%.2fx | digest_match=%s (0x%08x vs 0x%08x)\n",
           dt_dsp * 1e3, ((double)(rgba_bytes + y_bytes) / 1e9) / dt_dsp,
           dt_cpu * 1e3, ((double)(rgba_bytes + y_bytes) / 1e9) / dt_cpu,
           dt_cpu / dt_dsp, (dsp_digest == cpu_digest) ? "EXACT" : "DIFF", dsp_digest, cpu_digest);

    /* Benchmark 2: 16 MiB 3-tap Blur (HVX) */
    uint32_t blur_bytes = 16u * 1024u * 1024u;
    remote_arg pra_blur[2];
    pra_blur[0].buf.pv = rgba; pra_blur[0].buf.nLen = blur_bytes;
    pra_blur[1].buf.pv = rgba; /* separate buffer: use y_cpu? no, alloc 16MB */
    uint8_t *blur_out = (uint8_t *)p_mem_alloc(RPCMEM_HEAP_ID_SYSTEM, RPCMEM_DEFAULT_FLAGS, blur_bytes);
    pra_blur[1].buf.pv = blur_out; pra_blur[1].buf.nLen = blur_bytes;
    p_invoke(h, REMOTE_SCALARS_MAKE(4, 1, 1), pra_blur);
    t0 = now_sec();
    for (int it = 0; it < iters; it++) p_invoke(h, REMOTE_SCALARS_MAKE(4, 1, 1), pra_blur);
    double dt_blur = (now_sec() - t0) / iters;
    printf("KERNEL2 [16 MiB 3-tap Blur]: HVX=%.3f ms (%.2f GB/s)\n",
           dt_blur * 1e3, ((double)(blur_bytes * 2.0) / 1e9) / dt_blur);

    /* Benchmark 3: 16 MiB RAID/Backup XOR Parity + Checksum */
    uint32_t par_bytes = 16u * 1024u * 1024u;
    uint8_t *buf_b = (uint8_t *)p_mem_alloc(RPCMEM_HEAP_ID_SYSTEM, RPCMEM_DEFAULT_FLAGS, par_bytes);
    for (uint32_t i = 0; i < par_bytes; i++) buf_b[i] = (uint8_t)((i * 73u + 5u) & 0xFF);
    remote_arg pra_par[4];
    pra_par[0].buf.pv = rgba;     pra_par[0].buf.nLen = par_bytes;
    pra_par[1].buf.pv = buf_b;    pra_par[1].buf.nLen = par_bytes;
    pra_par[2].buf.pv = blur_out; pra_par[2].buf.nLen = par_bytes;
    pra_par[3].buf.pv = meta;     pra_par[3].buf.nLen = 4;

    p_invoke(h, REMOTE_SCALARS_MAKE(5, 2, 2), pra_par);
    t0 = now_sec();
    for (int it = 0; it < iters; it++) p_invoke(h, REMOTE_SCALARS_MAKE(5, 2, 2), pra_par);
    double dt_par_dsp = (now_sec() - t0) / iters;
    uint32_t dsp_csum = meta[0];

    tc0 = now_sec();
    uint32_t cpu_csum = 0;
    for (int it = 0; it < 10; it++) cpu_csum = cpu_parity(rgba, buf_b, blur_out, par_bytes);
    double dt_par_cpu = (now_sec() - tc0) / 10.0;

    printf("KERNEL3 [16 MiB XOR Parity+Csum]: HVX=%.3f ms (%.2f GB/s) | CPU=%.3f ms (%.2f GB/s) | speedup=%.2fx | match=%s (0x%08x vs 0x%08x)\n",
           dt_par_dsp * 1e3, ((double)(par_bytes * 3.0) / 1e9) / dt_par_dsp,
           dt_par_cpu * 1e3, ((double)(par_bytes * 3.0) / 1e9) / dt_par_cpu,
           dt_par_cpu / dt_par_dsp, (dsp_csum == cpu_csum) ? "EXACT" : "DIFF", dsp_csum, cpu_csum);

    /* Benchmark 4: Compute-heavy 16-round 32-bit ARX/ChaCha-style cipher (8 MiB, 6x1024-bit HVX vs Oryon CPU) */
    uint32_t arx_bytes = 8u * 1024u * 1024u;
    remote_arg pra_arx[3];
    pra_arx[0].buf.pv = rgba;     pra_arx[0].buf.nLen = arx_bytes;
    pra_arx[1].buf.pv = blur_out; pra_arx[1].buf.nLen = arx_bytes;
    pra_arx[2].buf.pv = meta;     pra_arx[2].buf.nLen = 4;
    p_invoke(h, REMOTE_SCALARS_MAKE(6, 1, 2), pra_arx);
    t0 = now_sec();
    for (int it = 0; it < iters; it++) p_invoke(h, REMOTE_SCALARS_MAKE(6, 1, 2), pra_arx);
    double dt_arx_dsp = (now_sec() - t0) / iters;
    uint32_t dsp_arx_csum = meta[0];

    /* CPU reference for 16-round ARX */
    tc0 = now_sec();
    uint32_t cpu_arx_csum = 0;
    for (int it = 0; it < 10; it++) {
        const uint32_t *s32 = (const uint32_t *)rgba;
        uint32_t *d32 = (uint32_t *)blur_out;
        uint32_t acc = 0;
        uint32_t nvecs = arx_bytes / 128u;
        for (uint32_t v = 0; v + 1 < nvecs; v += 2) {
            for (int k = 0; k < 32; k++) {
                uint32_t x = s32[v * 32 + k];
                uint32_t y = s32[(v + 1) * 32 + k];
                for (int r = 0; r < 16; r++) {
                    x += y; y ^= x; y = (y << 7) | (y >> 25);
                    x += y; y ^= x; y = (y << 13) | (y >> 19);
                }
                d32[v * 32 + k] = x;
                d32[(v + 1) * 32 + k] = y;
                acc ^= x ^ y;
            }
        }
        cpu_arx_csum = acc;
    }
    double dt_arx_cpu = (now_sec() - tc0) / 10.0;
    /* 16 rounds * 6 ops * (arx_bytes / 8 pairs) */
    double arx_gops = ((double)(arx_bytes / 8u) * 16.0 * 6.0) / 1e9;
    printf("KERNEL4 [8 MiB 16-rd ARX Cipher]: 6xHVX=%.3f ms (%.2f GOPS, %.2f GB/s) | CPU-1T=%.3f ms (%.2f GOPS) | speedup=%.2fx | match=%s (0x%08x vs 0x%08x)\n",
           dt_arx_dsp * 1e3, arx_gops / dt_arx_dsp, ((double)(arx_bytes * 2.0) / 1e9) / dt_arx_dsp,
           dt_arx_cpu * 1e3, arx_gops / dt_arx_cpu,
           dt_arx_cpu / dt_arx_dsp, (dsp_arx_csum == cpu_arx_csum) ? "EXACT" : "DIFF", dsp_arx_csum, cpu_arx_csum);

    p_mem_free(buf_b);
    p_mem_free(blur_out);
    p_mem_free(meta);
    p_mem_free(y_cpu);
    p_mem_free(y_dsp);
    p_mem_free(rgba);
    p_mem_free(ping_out);
    p_close(h);
    if (p_mem_deinit) p_mem_deinit();
    return 0;
}
