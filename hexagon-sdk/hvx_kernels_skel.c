/*
 * libhvx_kernels_skel.so — Native Hexagon V79 (SM8750) 6x1024-bit HVX + 8 MiB VTCM Compute Kernels
 * Compiled with upstream LLVM Clang 21 (--target=hexagon-unknown-elf -mv79 -mhvx -mhvx-length=128B)
 * Dispatched via Qualcomm FastRPC Unsigned PD (/dev/fastrpc-cdsp).
 */
#include <stdint.h>
#include <stddef.h>

typedef union {
    struct {
        void    *pv;
        uint32_t nLen;
    } buf;
    struct {
        int32_t  fd;
        uint32_t offset;
    } dma;
} remote_arg;

typedef uint64_t remote_handle64;

/* QuRT + HAP APIs provided by fastrpc_shell_unsigned_3 on CDSP */
#define QURT_HVX_MODE_128B 1
extern int   qurt_hvx_lock(int mode) __attribute__((weak));
extern int   qurt_hvx_unlock(void) __attribute__((weak));
extern int   qurt_hvx_get_units(void) __attribute__((weak));
extern int   HAP_power_request(int clock, int bus, int latency) __attribute__((weak));
extern int   HAP_power_get_max_speed(int *clock_max, int *bus_max) __attribute__((weak));
extern void *HAP_request_VTCM(unsigned int size, unsigned int single_page_flag) __attribute__((weak));
extern int   HAP_release_VTCM(void *pVA) __attribute__((weak));

/* QuRT thread API (standard qurt_thread_attr_t is 64 bytes on Hexagon QuRT) */
typedef struct {
    char     name[16];
    uint8_t  tcb_partition;
    uint8_t  affinity;
    uint16_t priority;
    uint8_t  asid;
    uint8_t  bus_priority;
    uint16_t timetest_id;
    uint32_t stack_size;
    void    *stack_addr;
    uint8_t  detachstate;
    uint8_t  pad[27];
} qurt_thread_attr_t;

typedef unsigned int qurt_thread_t;
extern int qurt_thread_create(qurt_thread_t *thread_id, qurt_thread_attr_t *attr, void (*entrypoint)(void *), void *arg) __attribute__((weak));
extern int qurt_thread_join(unsigned int tid, int *status) __attribute__((weak));

/* 1024-bit (128-byte) HVX vector types */
typedef uint8_t  hvx_u8  __attribute__((vector_size(128), aligned(128)));
typedef uint32_t hvx_u32 __attribute__((vector_size(128), aligned(128)));

/* Dedicated worker stacks for 6-thread parallel HVX execution */
#define NUM_HVX_WORKERS 6
static uint8_t worker_stacks[NUM_HVX_WORKERS][16384] __attribute__((aligned(64)));

typedef struct {
    const uint8_t *src0;
    const uint8_t *src1;
    uint8_t       *dst;
    uint32_t       nvecs;
    uint32_t       rounds;
    uint32_t       digest;
    int            kind; /* 0=luma, 1=blur, 2=parity, 3=arx_compute */
} hvx_job_t;

static void worker_entry(void *arg) {
    hvx_job_t *job = (hvx_job_t *)arg;
    int locked = 0;
    if (qurt_hvx_lock && qurt_hvx_lock(QURT_HVX_MODE_128B) == 0) locked = 1;

    uint32_t nvecs = job->nvecs;
    if (job->kind == 0) {
        /* RGBA -> Luma + XOR digest (512B in -> 128B out per vec) */
        const uint8_t *src = job->src0;
        uint8_t *dst = job->dst;
        hvx_u8 acc = {0};
        for (uint32_t v = 0; v < nvecs; v++) {
            hvx_u8 v0 = *(const hvx_u8 *)(src + 0);
            hvx_u8 v1 = *(const hvx_u8 *)(src + 128);
            hvx_u8 v2 = *(const hvx_u8 *)(src + 256);
            hvx_u8 v3 = *(const hvx_u8 *)(src + 384);
            src += 512;
            hvx_u8 yv = ((v0 >> 2) + (v1 >> 2)) + ((v2 >> 2) + (v3 >> 2));
            *(hvx_u8 *)(dst + v * 128) = yv;
            acc ^= yv;
        }
        uint32_t tmp[32] __attribute__((aligned(128)));
        *(hvx_u8 *)tmp = acc;
        uint32_t d = 0;
        for (int k = 0; k < 32; k++) d ^= tmp[k];
        job->digest = d;
    } else if (job->kind == 1) {
        /* 3-tap blur */
        const uint8_t *in_img = job->src0;
        uint8_t *out_img = job->dst;
        for (uint32_t v = 1; v + 1 < nvecs; v++) {
            hvx_u8 prev = *(const hvx_u8 *)(in_img + (v - 1) * 128);
            hvx_u8 curr = *(const hvx_u8 *)(in_img + v * 128);
            hvx_u8 next = *(const hvx_u8 *)(in_img + (v + 1) * 128);
            *(hvx_u8 *)(out_img + v * 128) = (prev >> 2) + (curr >> 1) + (next >> 2);
        }
    } else if (job->kind == 2) {
        /* 4-way unrolled XOR parity + checksum */
        const uint8_t *a = job->src0;
        const uint8_t *b = job->src1;
        uint8_t *p = job->dst;
        hvx_u8 acc0 = {0}, acc1 = {0}, acc2 = {0}, acc3 = {0};
        uint32_t v = 0;
        for (; v + 3 < nvecs; v += 4) {
            hvx_u8 p0 = *(const hvx_u8 *)(a + (v + 0) * 128) ^ *(const hvx_u8 *)(b + (v + 0) * 128);
            hvx_u8 p1 = *(const hvx_u8 *)(a + (v + 1) * 128) ^ *(const hvx_u8 *)(b + (v + 1) * 128);
            hvx_u8 p2 = *(const hvx_u8 *)(a + (v + 2) * 128) ^ *(const hvx_u8 *)(b + (v + 2) * 128);
            hvx_u8 p3 = *(const hvx_u8 *)(a + (v + 3) * 128) ^ *(const hvx_u8 *)(b + (v + 3) * 128);
            *(hvx_u8 *)(p + (v + 0) * 128) = p0;
            *(hvx_u8 *)(p + (v + 1) * 128) = p1;
            *(hvx_u8 *)(p + (v + 2) * 128) = p2;
            *(hvx_u8 *)(p + (v + 3) * 128) = p3;
            acc0 ^= p0; acc1 ^= p1; acc2 ^= p2; acc3 ^= p3;
        }
        for (; v < nvecs; v++) {
            hvx_u8 pv = *(const hvx_u8 *)(a + v * 128) ^ *(const hvx_u8 *)(b + v * 128);
            *(hvx_u8 *)(p + v * 128) = pv;
            acc0 ^= pv;
        }
        hvx_u8 acc = (acc0 ^ acc1) ^ (acc2 ^ acc3);
        uint32_t tmp[32] __attribute__((aligned(128)));
        *(hvx_u8 *)tmp = acc;
        uint32_t d = 0;
        for (int k = 0; k < 32; k++) d ^= tmp[k];
        job->digest = d;
    } else if (job->kind == 3) {
        /* Compute-bound 32-bit ARX/ChaCha-style vector mix (32 uint32 lanes x 16 rounds/vec) */
        const uint8_t *src = job->src0;
        uint8_t *dst = job->dst;
        uint32_t rounds = job->rounds;
        hvx_u32 acc = {0};
        for (uint32_t v = 0; v + 1 < nvecs; v += 2) {
            hvx_u32 x = *(const hvx_u32 *)(src + v * 128);
            hvx_u32 y = *(const hvx_u32 *)(src + (v + 1) * 128);
            for (uint32_t r = 0; r < rounds; r++) {
                x = x + y;
                y = y ^ x;
                y = (y << 7) | (y >> 25);
                x = x + y;
                y = y ^ x;
                y = (y << 13) | (y >> 19);
            }
            *(hvx_u32 *)(dst + v * 128) = x;
            *(hvx_u32 *)(dst + (v + 1) * 128) = y;
            acc ^= x ^ y;
        }
        uint32_t tmp[32] __attribute__((aligned(128)));
        *(hvx_u32 *)tmp = acc;
        uint32_t d = 0;
        for (int k = 0; k < 32; k++) d ^= tmp[k];
        job->digest = d;
    }

    if (locked && qurt_hvx_unlock) qurt_hvx_unlock();
}

static uint32_t run_parallel_hvx(int kind, const uint8_t *src0, const uint8_t *src1, uint8_t *dst,
                                 uint32_t total_vecs, uint32_t src0_stride_per_vec, uint32_t rounds) {
    hvx_job_t jobs[NUM_HVX_WORKERS];
    qurt_thread_t tids[NUM_HVX_WORKERS];
    int created[NUM_HVX_WORKERS];

    uint32_t chunk = (total_vecs / NUM_HVX_WORKERS) & ~3u;
    if (chunk < 4 || !qurt_thread_create || !qurt_thread_join) {
        jobs[0] = (hvx_job_t){ src0, src1, dst, total_vecs, rounds, 0, kind };
        worker_entry(&jobs[0]);
        return jobs[0].digest;
    }

    for (int t = 0; t < NUM_HVX_WORKERS; t++) {
        uint32_t start = (uint32_t)t * chunk;
        uint32_t count = (t == NUM_HVX_WORKERS - 1) ? (total_vecs - start) : chunk;
        jobs[t].src0   = src0 + start * src0_stride_per_vec;
        jobs[t].src1   = src1 ? (src1 + start * 128u) : NULL;
        jobs[t].dst    = dst  + start * 128u;
        jobs[t].nvecs  = count;
        jobs[t].rounds = rounds;
        jobs[t].digest = 0;
        jobs[t].kind   = kind;
        created[t] = 0;

        if (t > 0) {
            qurt_thread_attr_t attr;
            uint8_t *ap = (uint8_t *)&attr;
            for (size_t i = 0; i < sizeof(attr); i++) ap[i] = 0;
            attr.name[0] = 'h'; attr.name[1] = 'v'; attr.name[2] = 'x'; attr.name[3] = (char)('0' + t);
            attr.priority   = 100;
            attr.stack_size = sizeof(worker_stacks[t]);
            attr.stack_addr = worker_stacks[t];
            if (qurt_thread_create(&tids[t], &attr, worker_entry, &jobs[t]) == 0) {
                created[t] = 1;
            }
        }
    }

    /* Run worker 0 on calling thread */
    worker_entry(&jobs[0]);

    uint32_t digest = jobs[0].digest;
    for (int t = 1; t < NUM_HVX_WORKERS; t++) {
        if (created[t]) {
            int st = 0;
            qurt_thread_join(tids[t], &st);
        } else {
            worker_entry(&jobs[t]);
        }
        digest ^= jobs[t].digest;
    }
    return digest;
}

/*
 * FastRPC skeleton entry point:
 *   method 0/1: open/close
 *   method 2: ping + Turbo power vote + max CDSP clock + VTCM probe
 *   method 3: 6-thread HVX RGBA->Luma + XOR digest
 *   method 4: 6-thread HVX 3-tap Blur
 *   method 5: 6-thread HVX XOR Parity + Checksum
 *   method 6: 6-thread HVX 16-round ARX/ChaCha-style 1024-bit Vector Compute
 */
int hvx_kernels_skel_handle_invoke(remote_handle64 _h, uint32_t _sc, remote_arg *_pra) {
    (void)_h;
    uint32_t method = (_sc >> 24) & 0xFF;

    if (method == 0 || method == 1) {
        if (method == 0 && HAP_power_request) {
            HAP_power_request(100, 100, 1); /* Vote max CDSP Turbo clock + DDR bus */
        }
        return 0;
    }

    switch (method) {
        case 2: {
            uint32_t *out = (uint32_t *)_pra[0].buf.pv;
            int clk_max = 0, bus_max = 0;
            if (HAP_power_request) HAP_power_request(100, 100, 1);
            if (HAP_power_get_max_speed) HAP_power_get_max_speed(&clk_max, &bus_max);
            int vtcm_ok = 0;
            if (HAP_request_VTCM && HAP_release_VTCM) {
                void *v = HAP_request_VTCM(4u * 1024u * 1024u, 1);
                if (v) { vtcm_ok = 1; HAP_release_VTCM(v); }
            }
            out[0] = 0x48565839u; /* "HVX9" */
            out[1] = qurt_hvx_get_units ? (uint32_t)qurt_hvx_get_units() : 0u;
            out[2] = (uint32_t)clk_max;
            out[3] = (uint32_t)bus_max;
            out[4] = (uint32_t)vtcm_ok;
            out[5] = NUM_HVX_WORKERS;
            return 0;
        }
        case 3: {
            const uint8_t *rgba = (const uint8_t *)_pra[0].buf.pv;
            uint32_t npix       = _pra[0].buf.nLen / 4u;
            uint8_t *y_out      = (uint8_t *)_pra[1].buf.pv;
            uint32_t *meta      = (uint32_t *)_pra[2].buf.pv;
            meta[0] = npix;
            meta[1] = run_parallel_hvx(0, rgba, NULL, y_out, npix / 128u, 512u, 0);
            return 0;
        }
        case 4: {
            const uint8_t *in_img = (const uint8_t *)_pra[0].buf.pv;
            uint8_t *out_img      = (uint8_t *)_pra[1].buf.pv;
            uint32_t nbytes       = _pra[0].buf.nLen;
            run_parallel_hvx(1, in_img, NULL, out_img, nbytes / 128u, 128u, 0);
            return 0;
        }
        case 5: {
            const uint8_t *a = (const uint8_t *)_pra[0].buf.pv;
            const uint8_t *b = (const uint8_t *)_pra[1].buf.pv;
            uint8_t *p       = (uint8_t *)_pra[2].buf.pv;
            uint32_t *meta   = (uint32_t *)_pra[3].buf.pv;
            uint32_t nbytes  = _pra[0].buf.nLen;
            meta[0] = run_parallel_hvx(2, a, b, p, nbytes / 128u, 128u, 0);
            return 0;
        }
        case 6: {
            const uint8_t *src = (const uint8_t *)_pra[0].buf.pv;
            uint8_t *dst       = (uint8_t *)_pra[1].buf.pv;
            uint32_t *meta     = (uint32_t *)_pra[2].buf.pv;
            uint32_t nbytes    = _pra[0].buf.nLen;
            meta[0] = run_parallel_hvx(3, src, NULL, dst, nbytes / 128u, 128u, 16u);
            return 0;
        }
        default:
            return -1;
    }
}

int hvx_kernels_skel_invoke(uint32_t _sc, remote_arg *_pra) {
    return hvx_kernels_skel_handle_invoke(0, _sc, _pra);
}
