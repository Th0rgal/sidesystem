/*
 * hexagon-probe.c — Direct Linux glibc/musl FastRPC + DMA-Heap probe for Qualcomm Hexagon V79 NPU.
 *
 * Queries /dev/fastrpc-cdsp (FASTRPC_IOCTL_GETINFO / FASTRPC_IOCTL_GETDSPINFO)
 * and tests zero-copy DMA-BUF allocation from /dev/dma_heap/qcom,system inside NixOS.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

/* Upstream Linux / Qualcomm FastRPC ioctl definitions (drivers/misc/fastrpc.c) */
#define FASTRPC_IOCTL_GETINFO        _IOWR('R', 8, uint32_t)
#define FASTRPC_IOCTL_GET_DSP_INFO   _IOWR('R', 13, struct fastrpc_ioctl_capability)

struct fastrpc_ioctl_capability {
    uint32_t domain;
    uint32_t attribute_id;
    uint32_t capability;
    uint32_t reserved[4];
};

/* CDSP domain = 3 */
#define CDSP_DOMAIN_ID 3

/* Linux DMA-Heap allocation ioctl */
struct dma_heap_allocation_data {
    uint64_t len;
    uint32_t fd;
    uint32_t fd_flags;
    uint64_t heap_flags;
};
#define DMA_HEAP_IOC_MAGIC 'H'
#define DMA_HEAP_IOCTL_ALLOC _IOWR(DMA_HEAP_IOC_MAGIC, 0x0, struct dma_heap_allocation_data)

static void query_cap(int fd, uint32_t attr, const char *label) {
    struct fastrpc_ioctl_capability cap;
    memset(&cap, 0, sizeof(cap));
    cap.domain = CDSP_DOMAIN_ID;
    cap.attribute_id = attr;
    if (ioctl(fd, FASTRPC_IOCTL_GET_DSP_INFO, &cap) == 0) {
        printf("  %-26s (attr %2u): 0x%x (%u)\n", label, attr, cap.capability, cap.capability);
    } else {
        printf("  %-26s (attr %2u): ioctl err=%d (%s)\n", label, attr, errno, strerror(errno));
    }
}

int main(void) {
    const char *devs[] = { "/dev/fastrpc-cdsp", "/dev/fastrpc-cdsp-secure" };
    int fd = -1;
    const char *opened = NULL;
    for (int i = 0; i < 2; i++) {
        fd = open(devs[i], O_RDWR);
        if (fd >= 0) { opened = devs[i]; break; }
        fprintf(stderr, "open(%s): %s\n", devs[i], strerror(errno));
    }
    if (fd < 0) return 1;

    printf("Opened Hexagon CDSP FastRPC: %s (fd=%d)\n", opened, fd);

    uint32_t info = 0;
    if (ioctl(fd, FASTRPC_IOCTL_GETINFO, &info) == 0) {
        printf("  FASTRPC_IOCTL_GETINFO    : 0x%x (smmu=%u)\n", info, info & 1);
    }

    /* Qualcomm CDSP capability attribute IDs */
    query_cap(fd, 0, "DOMAIN_SUPPORT");
    query_cap(fd, 1, "UNSIGNED_PD_SUPPORT");
    query_cap(fd, 2, "HVX_SUPPORT_64B");
    query_cap(fd, 3, "HVX_SUPPORT_128B");
    query_cap(fd, 4, "VTCM_PAGE");
    query_cap(fd, 5, "VTCM_COUNT");
    query_cap(fd, 6, "ARCH_VER (Hexagon)");
    query_cap(fd, 7, "HMX_SUPPORT_DEPTH");
    query_cap(fd, 8, "HMX_SUPPORT_SPATIAL");
    query_cap(fd, 9, "ASYNC_FASTRPC_SUPPORT");
    close(fd);

    /* Test zero-copy DMA-BUF allocation for NPU/GPU shared tensors */
    int hfd = open("/dev/dma_heap/qcom,system", O_RDWR);
    if (hfd < 0) hfd = open("/dev/dma_heap/system", O_RDWR);
    if (hfd >= 0) {
        struct dma_heap_allocation_data alloc = {
            .len = 4 * 1024 * 1024, /* 4 MiB shared tensor buffer */
            .fd_flags = O_RDWR | O_CLOEXEC,
        };
        if (ioctl(hfd, DMA_HEAP_IOCTL_ALLOC, &alloc) == 0) {
            void *ptr = mmap(NULL, alloc.len, PROT_READ | PROT_WRITE, MAP_SHARED, (int)alloc.fd, 0);
            if (ptr != MAP_FAILED) {
                memset(ptr, 0x42, 4096);
                printf("DMA-Heap zero-copy 4 MiB tensor buffer: OK (fd=%u, ptr=%p)\n", alloc.fd, ptr);
                munmap(ptr, alloc.len);
            }
            close((int)alloc.fd);
        } else {
            printf("DMA-Heap alloc failed: %s\n", strerror(errno));
        }
        close(hfd);
    }
    return 0;
}
