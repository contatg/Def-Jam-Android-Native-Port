#ifndef DEFJAM_POWER25_H
#define DEFJAM_POWER25_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
#if defined(__ANDROID__)
void defjam_power25_init(void);
void defjam_power25_tick(void);
int defjam_power25_monitor_on(void);
int defjam_power25_fg30_blocked(void);
void defjam_power25_register(const char *role);
void defjam_power25_work_begin(void);
void defjam_power25_work_end(int cap);
void defjam_power25_present(uint64_t now,int combat,int cap);
void defjam_power25_paused(int paused);
int defjam_power25_limit(int cap,int combat);
int defjam_power25_resolution(void);
void defjam_power25_resolution_applied(int scaled);
int defjam_power25_ui(void);
void defjam_power25_display_observed(int requested,int milli_hz,int mode,int sustained);
int defjam_power25_eco(void);
void defjam_power25_wait(int kind);
#else
static inline void defjam_power25_work_begin(void){}
static inline void defjam_power25_work_end(int cap){(void)cap;}
static inline int defjam_power25_resolution(void){return 0;}
static inline void defjam_power25_resolution_applied(int s){(void)s;}
static inline int defjam_power25_eco(void){return 0;}
static inline void defjam_power25_wait(int kind){(void)kind;}
#endif
#ifdef __cplusplus
}
#endif
#endif
