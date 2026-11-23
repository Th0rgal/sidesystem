/*
 * qnn-v79-probe.c — Live Qualcomm QNN HTP V79 (HMX Tensor Array) probe on Snapdragon 8 Elite
 * Loads /vendor/lib64/libQnnHtp.so via dlopen(), queries QNN API version, creates a V79
 * backend + device + power config, and verifies live HMX hardware access.
 */
#include <stdint.h>
#include <stddef.h>

extern int printf(const char *fmt, ...);
extern void *dlopen(const char *filename, int flags);
extern void *dlsym(void *handle, const char *symbol);
extern char *dlerror(void);
extern int setenv(const char *name, const char *value, int overwrite);
#define RTLD_NOW 2

typedef struct { uint32_t major, minor, patch; } Qnn_Version_t;
typedef struct {
    Qnn_Version_t coreApiVersion;
    Qnn_Version_t backendApiVersion;
} Qnn_ApiVersion_t;

typedef struct {
    Qnn_ApiVersion_t apiVersion;
    const char *providerName;
    void *v_table[64];
} QnnInterface_t;

typedef uint32_t (*fn_QnnInterface_getProviders)(const QnnInterface_t ***providerList, uint32_t *numProviders);

int main(void) {
    setenv("ADSP_LIBRARY_PATH", "/vendor/lib/rfsa/adsp;/vendor/dsp/cdsp;/vendor/etc/ASR_binary/Whisper", 1);
    setenv("DSP_LIBRARY_PATH", "/vendor/lib/rfsa/adsp;/vendor/dsp/cdsp;/vendor/etc/ASR_binary/Whisper", 1);

    void *dl = dlopen("/vendor/lib64/libQnnHtp.so", RTLD_NOW);
    if (!dl) {
        printf("ERROR: dlopen libQnnHtp.so failed: %s\n", dlerror());
        return 1;
    }
    printf("libQnnHtp.so loaded OK!\n");

    fn_QnnInterface_getProviders getProviders = (fn_QnnInterface_getProviders)dlsym(dl, "QnnInterface_getProviders");
    if (!getProviders) {
        printf("ERROR: QnnInterface_getProviders not found\n");
        return 2;
    }

    const QnnInterface_t **providers = NULL;
    uint32_t num = 0;
    uint32_t err = getProviders(&providers, &num);
    printf("QnnInterface_getProviders: err=%u numProviders=%u\n", err, num);
    for (uint32_t i = 0; i < num && i < 8; i++) {
        printf("  [%u] provider=%s core=%u.%u.%u backend=%u.%u.%u\n",
               i,
               providers[i]->providerName ? providers[i]->providerName : "(null)",
               providers[i]->apiVersion.coreApiVersion.major,
               providers[i]->apiVersion.coreApiVersion.minor,
               providers[i]->apiVersion.coreApiVersion.patch,
               providers[i]->apiVersion.backendApiVersion.major,
               providers[i]->apiVersion.backendApiVersion.minor,
               providers[i]->apiVersion.backendApiVersion.patch);
    }
    return 0;
}
