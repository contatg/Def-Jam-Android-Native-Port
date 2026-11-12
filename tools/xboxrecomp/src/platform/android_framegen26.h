#ifndef DEFJAM_FRAMEGEN26_H
#define DEFJAM_FRAMEGEN26_H
#ifdef __cplusplus
extern "C" {
#endif
#if defined(__ANDROID__)
/* active, real-source Hz, accepted-output Hz, interpolation-dispatch Hz */
void defjam_framegen26_stats(float out[4]);
int defjam_framegen26_ui(void);
void defjam_framegen26_pause(int paused);
#endif
#ifdef __cplusplus
}
#endif
#endif
