#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#if defined(__ANDROID__)
#include "platform/android_thread_policy.h"
#include "platform/android_sched.h"
#include "platform/android_hitch.h"
#include <pthread.h>
#include <sched.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include "recomp_settings.h"

static pthread_once_t s_cpu_once = PTHREAD_ONCE_INIT;
static cpu_set_t s_original, s_native, s_render, s_vertex, s_audio, s_host;
static unsigned long s_capacity[CPU_SETSIZE];
static int s_have_mask, s_request;
static _Atomic int s_render_allowed;
static _Thread_local int s_native_entered;
static _Thread_local int s_is_guest, s_callback_noted;
static _Thread_local int s_guest_result, s_guest_errno;
static _Thread_local cpu_set_t s_guest_target;

static unsigned long read_cpu_value(int cpu,const char *leaf) {
    char path[192];unsigned long value=0;
    snprintf(path,sizeof path,"/sys/devices/system/cpu/cpu%d/%s",cpu,leaf);
    FILE *file=fopen(path,"r");
    if(file){if(fscanf(file,"%lu",&value)!=1)value=0;fclose(file);}
    return value;
}
static int fastest(const cpu_set_t *mask) {
    int best=-1;
    for(int cpu=0;cpu<CPU_SETSIZE;cpu++)if(CPU_ISSET(cpu,mask)&&
            (best<0||s_capacity[cpu]>s_capacity[best]||
             (s_capacity[cpu]==s_capacity[best]&&cpu>best)))best=cpu;
    return best;
}

static void cpu_list(const cpu_set_t *set, char out[512])
{
    size_t used = 0;
    out[0] = 0;
    for (int cpu = 0; cpu < CPU_SETSIZE && used + 16 < 512; ++cpu)
        if (CPU_ISSET(cpu, set))
            used += (size_t)snprintf(out + used, 512 - used, "%s%d", used ? "," : "", cpu);
    if (!used) snprintf(out, 512, "unknown");
}

static void choose_audio_mask(const char *layout)
{
    s_audio=s_native;
    if(layout&&!strcmp(layout,"split")&&CPU_COUNT(&s_native)>=4){
        int prime=fastest(&s_original);cpu_set_t remain=s_native,chosen;CPU_ZERO(&chosen);
        if(prime>=0)CPU_CLR(prime,&remain);
        if(CPU_COUNT(&s_render)==1)for(int c=0;c<CPU_SETSIZE;c++)if(CPU_ISSET(c,&s_render))CPU_CLR(c,&remain);
        for(unsigned n=0;n<2;n++){
            int c=fastest(&remain);
            if(c<0||prime<0||!s_capacity[prime]||s_capacity[c]*100<s_capacity[prime]*90)break;
            CPU_SET(c,&chosen);CPU_CLR(c,&remain);
        }
        if(CPU_COUNT(&chosen)==2)s_audio=chosen;
    }
}

static void capture_cpu_policy(void)
{
    const char *value = getenv("RECOMP_NATIVE_CORES");
    char original[512], selected[512];
    int count = 0;
    s_request = value && value[0] >= '0' && value[0] <= '8' && !value[1] ? value[0] - '0' : 0;
    s_have_mask = sched_getaffinity(0, sizeof s_original, &s_original) == 0;
    if (!s_have_mask) {
        fprintf(stderr, "[ANDROID-THREAD] original affinity unavailable: errno=%d; no native mask applied\n", errno);
        return;
    }
    int have_capacity=1;
    for(int cpu=0;cpu<CPU_SETSIZE;cpu++)if(CPU_ISSET(cpu,&s_original)){
        s_capacity[cpu]=read_cpu_value(cpu,"cpu_capacity");
        if(!s_capacity[cpu])have_capacity=0;
    }
    if(!have_capacity)for(int cpu=0;cpu<CPU_SETSIZE;cpu++)if(CPU_ISSET(cpu,&s_original))
        s_capacity[cpu]=read_cpu_value(cpu,"cpufreq/cpuinfo_max_freq");
    fprintf(stderr,"[ANDROID-THREAD] topology rank=%s (missing values fall back to CPU ID); guest target CPU=%d\n",
        have_capacity?"cpu_capacity":"max-frequency",fastest(&s_original));
    CPU_ZERO(&s_native);
    for (int cpu = CPU_SETSIZE - 1; cpu >= 0; --cpu)
        if (CPU_ISSET(cpu, &s_original) && (!s_request || count < s_request)) {
            CPU_SET(cpu, &s_native);
            ++count;
        }
    s_render = s_native;
    const char *layout = getenv("RECOMP_NATIVE_LAYOUT");
    if (layout && !strcmp(layout, "split") && CPU_COUNT(&s_original) > 1) {
        /* Reserve the highest-ranked allowed core for the guest. This is an
         * explicit split profile; equal/missing capacities fall back to IDs.
         * Audio, clocks and other native workers retain their full budget. */
        int guest=fastest(&s_original),render;
        cpu_set_t candidates=s_native;
        if(guest>=0)CPU_CLR(guest,&candidates);
        render=fastest(&candidates);
        if (render >= 0) {
            CPU_ZERO(&s_render);
            for(int c=0;c<CPU_SETSIZE;c++)if(CPU_ISSET(c,&candidates)&&
                    s_capacity[render]&&s_capacity[c]*100>=s_capacity[render]*90)
                CPU_SET(c,&s_render);
            if(!CPU_COUNT(&s_render))CPU_SET(render,&s_render);
        }
    }
    choose_audio_mask(layout);
    s_vertex=s_native; s_host=s_native;
    int guest=fastest(&s_original);
    if(guest>=0&&CPU_COUNT(&s_vertex)>1)CPU_CLR(guest,&s_vertex);
    if(layout&&!strcmp(layout,"split")){
        /* Keep two non-guest performance CPUs for three sleeping helpers.
         * Oversubscription is intentional/configurable, not three physical cores. */
        s_vertex=s_render;
        if(CPU_COUNT(&s_vertex)>2)CPU_CLR(fastest(&s_vertex),&s_vertex);
        CPU_ZERO(&s_host);
        for(int c=0;c<CPU_SETSIZE;c++)if(CPU_ISSET(c,&s_native)&&c!=guest&&
                !CPU_ISSET(c,&s_render))CPU_SET(c,&s_host);
        if(!CPU_COUNT(&s_host))s_host=s_native;
        /* Affinity only: no mixer, buffer, priority or audio algorithm edits. */
        s_audio=s_host;
    }
    cpu_list(&s_original, original); cpu_list(&s_native, selected);
    fprintf(stderr, "[ANDROID-THREAD] native budget=%s; original_allowed={%s}; native_target={%s}; guest affinity is separate\n",
            s_request ? value : "auto", original, selected);
    cpu_list(&s_audio,selected);
    fprintf(stderr,"[AUDIO122] native mixer target={%s}; split layout uses non-performance CPUs when available; placement only\n",selected);
    cpu_list(&s_render, selected);
    fprintf(stderr, "[ANDROID-THREAD] layout=%s render_target={%s}; host events/audio/clock use remaining CPUs; mixer target above; scheduler approval required\n",
            layout && !strcmp(layout,"split") ? "split" : "auto", selected);
}

void defjam_android_cpu_init(void) { pthread_once(&s_cpu_once, capture_cpu_policy); }

int defjam_android_guest_select(const char *value,const void *allowed,void *selected,size_t size) {
    if(size!=sizeof(cpu_set_t)||!allowed||!selected)return 0;
    defjam_android_cpu_init();
    cpu_set_t remaining=*(const cpu_set_t *)allowed,*out=selected;CPU_ZERO(out);
    int request=value&&!strcmp(value,"all")?CPU_SETSIZE:
        value&&value[0]>='1'&&value[0]<='8'&&!value[1]?value[0]-'0':1;
    int count=0,cpu;
    while(count<request&&(cpu=fastest(&remaining))>=0){
        CPU_SET(cpu,out);CPU_CLR(cpu,&remaining);count++;
    }
    return count;
}
unsigned defjam_android_vertex_budget(void) {
    defjam_android_cpu_init();
    return s_have_mask?(unsigned)CPU_COUNT(&s_vertex)+(CPU_COUNT(&s_vertex)>=2?2:1):1;
}

int defjam_android_native_core_count(void)
{
    /* Never lazily initialize from a Java/UI getter, which could run before
     * SDL_main receives the configuration. The renderer publishes its actual
     * mask after applying it. Zero means it has not reported yet. */
    return atomic_load(&s_render_allowed);
}

static void thread_note(const char *role, const char *function, uintptr_t entry,
                        uint32_t guest_entry, uint32_t bridge_id, int result, int saved_errno,
                        const cpu_set_t *target)
{
    cpu_set_t applied;
    char list[512], requested[512], name[16];
    snprintf(name, sizeof name, "dj-%s", role);
    pthread_setname_np(pthread_self(), name);
    CPU_ZERO(&applied);
    int have = sched_getaffinity(0, sizeof applied, &applied) == 0;
    cpu_list(&applied, list);
    cpu_list(target, requested);
    fprintf(stderr, "[ANDROID-THREAD] role=%s function=%s linux_tid=%ld bridge_id=%u guest_entry=%08X host_entry=%p allowed={%s} mask_result=%d errno=%d requested={%s}\n",
            role, function, (long)syscall(SYS_gettid), bridge_id, guest_entry,
            (void *)entry, list, result, saved_errno, requested);
    if (!strcmp(role, "render")) atomic_store(&s_render_allowed, have ? CPU_COUNT(&applied) : 0);
}

static int apply_mask(const cpu_set_t *target)
{
    cpu_set_t current;
    if (sched_getaffinity(0,sizeof current,&current)==0 && CPU_EQUAL(target,&current))
        return 0; /* No redundant syscall for the usual native Auto mask. */
    return sched_setaffinity((pid_t)syscall(SYS_gettid),sizeof *target,target);
}

int defjam_android_guest_affinity_apply(const void *target, size_t target_size)
{
    if (!target || target_size != sizeof s_guest_target) {
        CPU_ZERO(&s_guest_target);
        s_guest_result = -1;
        s_guest_errno = EINVAL;
    } else {
        memcpy(&s_guest_target, target, sizeof s_guest_target);
        s_guest_result = apply_mask(&s_guest_target);
        s_guest_errno = s_guest_result ? errno : 0;
    }
    errno = s_guest_errno;
    return s_guest_result;
}

void defjam_android_native_thread_enter(const char *role, const char *function, uintptr_t entry)
{
    if (s_is_guest) {
        if (!s_callback_noted) {
            s_callback_noted = 1;
            fprintf(stderr, "[ANDROID-THREAD] native callback %s on guest Linux TID %ld; guest affinity retained\n",
                    function, (long)syscall(SYS_gettid));
        }
        return;
    }
    if (s_native_entered) return;
    s_native_entered = 1;
    defjam_android_cpu_init();
    int result = -1, saved_errno = ENOTSUP;
    const cpu_set_t *target = !strcmp(role,"audio-mix") ? &s_audio :
        !strcmp(role,"render") ? &s_render :
        !strncmp(role,"vertex",6)?&s_vertex:&s_host;
    cpu_set_t pinned22;
    if(recomp_settings_get("vksched","pin_render22",0)){
        int cpu22=!strcmp(role,"render")?6:!strncmp(role,"vertex",6)?5:-1;
        if(cpu22>=0 && CPU_ISSET(cpu22,target)){
            CPU_ZERO(&pinned22);CPU_SET(cpu22,&pinned22);target=&pinned22;
        }
    }
    if (s_have_mask && CPU_COUNT(target)) {
        result = apply_mask(target);
        saved_errno = result ? errno : 0;
    }
    if(defjam_trace_thread){if(!strcmp(role,"render"))defjam_trace_thread(1);else if(!strcmp(role,"audio-mix"))defjam_trace_thread(2);}
    thread_note(role, function, entry, 0, 0, result, saved_errno, target);
}

void defjam_android_guest_thread_note(const char *role, const char *function, uintptr_t entry,
                                       uint32_t guest_entry, uint32_t bridge_id)
{
    /* Logs only. Never broadens a guest/interrupt thread onto the native mask. */
    s_is_guest = 1;
    if(defjam_trace_thread&&!strcmp(role,"guest-main"))defjam_trace_thread(0);
    defjam_guest_sampler_enter();
    thread_note(role, function, entry, guest_entry, bridge_id, s_guest_result, s_guest_errno, &s_guest_target);
}
#endif
