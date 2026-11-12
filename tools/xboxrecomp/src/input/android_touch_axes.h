#ifndef XBOX_ANDROID_TOUCH_AXES_H
#define XBOX_ANDROID_TOUCH_AXES_H
/* Software axes only: no SDL virtual device, keyboard keys or physical-pad
 * registration. Lifetime lock stays valid while the host reader starts/stops. */
#include <pthread.h>
#include <stdint.h>
#include <string.h>
static pthread_mutex_t s_touch_axes_lock=PTHREAD_MUTEX_INITIALIZER;
static int16_t s_touch_axes[4];
static uint32_t s_touch_packet;
static int s_touch_seen;
static int16_t touch_axis_clamp(int v){return (int16_t)(v>32767?32767:v< -32767?-32767:v);}
static void android_touch_set(int lx,int ly,int rx,int ry){
    int16_t next[4]={touch_axis_clamp(lx),touch_axis_clamp(ly),touch_axis_clamp(rx),touch_axis_clamp(ry)};
    pthread_mutex_lock(&s_touch_axes_lock);
    if(next[0]||next[1]||next[2]||next[3])s_touch_seen=1;
    if(memcmp(next,s_touch_axes,sizeof next)){memcpy(s_touch_axes,next,sizeof next);++s_touch_packet;}
    pthread_mutex_unlock(&s_touch_axes_lock);
}
static void android_touch_reset(void){
    pthread_mutex_lock(&s_touch_axes_lock);memset(s_touch_axes,0,sizeof s_touch_axes);s_touch_seen=0;++s_touch_packet;pthread_mutex_unlock(&s_touch_axes_lock);
}
static int64_t touch_magnitude(int x,int y){return (int64_t)x*x+(int64_t)y*y;}
static int android_touch_merge(XBOX_INPUT_STATE *state){
    pthread_mutex_lock(&s_touch_axes_lock);
    if(touch_magnitude(s_touch_axes[0],s_touch_axes[1])>touch_magnitude(state->Gamepad.sThumbLX,state->Gamepad.sThumbLY)){
        state->Gamepad.sThumbLX=s_touch_axes[0];state->Gamepad.sThumbLY=s_touch_axes[1];
    }
    if(touch_magnitude(s_touch_axes[2],s_touch_axes[3])>touch_magnitude(state->Gamepad.sThumbRX,state->Gamepad.sThumbRY)){
        state->Gamepad.sThumbRX=s_touch_axes[2];state->Gamepad.sThumbRY=s_touch_axes[3];
    }
    state->dwPacketNumber+=s_touch_packet;
    int present=s_touch_seen;pthread_mutex_unlock(&s_touch_axes_lock);return present;
}
#endif
