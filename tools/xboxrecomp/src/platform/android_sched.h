#ifndef DEFJAM_ANDROID_SCHED_H
#define DEFJAM_ANDROID_SCHED_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
#if defined(__ANDROID__)
void defjam_sched_settings_ready(void);
void defjam_sched_gpu(const char *name);
int defjam_sched_limit(int combat);
void defjam_sched_set_limit145(int cap);
int defjam_sched_apply_limit138(int cap,int combat);
int defjam_sched_low_end(void);
void defjam_sched_observe_steps(double hz);
void defjam_sched_observe_timing(unsigned steps,uint64_t wall,uint64_t network_wait);
unsigned defjam_sched_vertex_participants(unsigned requested);
unsigned defjam_sched_vertex_threshold(void);
int defjam_sched_vertex_cache141(void);
void defjam_monitor_init(int enabled);
void defjam_thermal138_init(int enabled);
int defjam_thermal138_limit(int cap,int combat,int cadence_blocked);
void defjam_thermal138_report(void);
void defjam_perf138_gather(uint64_t ns,unsigned helpers);
void defjam_perf138_report(void);
void defjam_perf138_frame(void);
void defjam_monitor_frame(uint64_t now, int combat, int cap, uint64_t present_ns);
void defjam_monitor_pause(int paused);
void defjam_monitor_hud(float out[6]);
void defjam_monitor_thermal(int status);
void defjam_monitor_fight_step(void);
void defjam_monitor_network_wait(uint64_t ns);
uint64_t defjam_monitor_network_wait_total(void);
void defjam_monitor_audio(unsigned queued, unsigned gaps, unsigned refused, unsigned missing);
void defjam_guest_sampler_init(int enabled);
void defjam_guest_sampler_enter(void);
void defjam_guest_sampler_report(void);
int defjam_android_combat_active(void);
#endif
#ifdef __cplusplus
}
#endif
#endif
