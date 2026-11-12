#ifndef DEFJAM_ANDROID_PERF_H
#define DEFJAM_ANDROID_PERF_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct DefjamKernelProbe {
    uint64_t wall,cpu;
    int slot,index;
} DefjamKernelProbe;
#if defined(__ANDROID__)
extern int defjam_kernel_profile_mode; /* set before guest threads start */
void defjam_perf_init(void);
DefjamKernelProbe defjam_perf_begin(unsigned ordinal);
void defjam_perf_end(DefjamKernelProbe probe);
void defjam_perf_frame(uint64_t present_ns,int presented);
#endif
#ifdef __cplusplus
}
#endif
#endif
