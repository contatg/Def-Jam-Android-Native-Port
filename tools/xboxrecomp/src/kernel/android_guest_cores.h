#ifndef DEFJAM_ANDROID_GUEST_CORES_H
#define DEFJAM_ANDROID_GUEST_CORES_H
#include <sched.h>
#include <string.h>
/* Work only within the caller's allowed set; sparse CPU IDs are valid.
 * Android's existing highest-ID preference is retained, not claimed to be
 * topology-aware. More than one CPU changes guest ordering and is opt-in. */
static int defjam_select_guest_cores(const char *value, const cpu_set_t *allowed, cpu_set_t *selected)
{
    int request = 1, count = 0, cpu;
    if (value && !strcmp(value, "all")) request = CPU_SETSIZE;
    else if (value && value[0] >= '1' && value[0] <= '8' && !value[1]) request = value[0] - '0';
    CPU_ZERO(selected);
    for (cpu = CPU_SETSIZE - 1; cpu >= 0 && count < request; --cpu)
        if (CPU_ISSET(cpu, allowed)) { CPU_SET(cpu, selected); ++count; }
    return count;
}
#endif
