#ifndef DEFJAM_ANDROID_HITCH_H
#define DEFJAM_ANDROID_HITCH_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
enum { DJ_UNSCOPED,DJ_FILE,DJ_SYNC,DJ_KERNEL,DJ_GATHER,DJ_DRAW,DJ_TEXTURE,DJ_PIPELINE,DJ_SHADER,DJ_ALLOC,DJ_AUDIO,DJ_PACE,DJ_PARSE22,DJ_COMBINER22,DJ_LOOKUP22,DJ_UNIFORM22,DJ_UPLOAD22,DJ_RECORD22,DJ_SUBMIT22,DJ_PRESENT22,DJ_CATEGORIES };
#if defined(__ANDROID__)
/* Optional game instrumentation: standalone runtime/host fixtures still link. */
unsigned defjam_trace_push(unsigned category) __attribute__((weak));
void defjam_trace_pop(unsigned token) __attribute__((weak));
void defjam_trace_thread(unsigned lane) __attribute__((weak));
uint64_t defjam_kernel_trace_begin(unsigned ordinal,uint32_t argument,uint32_t caller) __attribute__((weak));
void defjam_kernel_trace_end(uint64_t start) __attribute__((weak));
void defjam_trace_enable(int enabled);
void defjam_trace_poll22(void);
unsigned defjam_trace_fine22(unsigned category);
void defjam_trace_frame(uint64_t now,uint64_t dt,int combat);
void defjam_trace_report(void);
void defjam_trace_runtime(int64_t gc_count,int64_t gc_ms,int64_t allocated);
static inline unsigned dj_trace(unsigned c){return defjam_trace_push?defjam_trace_push(c):0;}
static inline unsigned dj_fine22(unsigned c){return defjam_trace_fine22(c);}
static inline void dj_trace_end(unsigned t){if(t&&defjam_trace_pop)defjam_trace_pop(t);}
#else
static inline unsigned dj_trace(unsigned c){(void)c;return 0;}
static inline unsigned dj_fine22(unsigned c){(void)c;return 0;}
static inline void dj_trace_end(unsigned t){(void)t;}
#endif
#if defined(__ANDROID__) && (defined(__GNUC__) || defined(__clang__))
static inline void dj_cleanup22(unsigned *t){dj_trace_end(*t);}
#define DJ_SCOPE22(cat) unsigned dj_scope22 __attribute__((cleanup(dj_cleanup22))) = defjam_trace_fine22(cat)
#else
#define DJ_SCOPE22(cat) ((void)0)
#endif
#ifdef __cplusplus
}
#endif
#endif
