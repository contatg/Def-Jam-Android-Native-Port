"""Real Linux affinity tests of the Android native/guest policy; no device claim."""
from pathlib import Path
import os
import subprocess

ROOT = Path(__file__).resolve().parents[2]


def test_native_mask_does_not_change_guest_mask(tmp_path):
    source = tmp_path / 'policy.c'
    source.write_text(r'''
#define _GNU_SOURCE
#include "platform/android_thread_policy.h"
#include <pthread.h>
#include <sched.h>
#include <errno.h>
#include <stdlib.h>
#include <stdint.h>
#include <assert.h>
static cpu_set_t original, expected;
static int reject_restore;
int __real_sched_setaffinity(pid_t pid, size_t size, const cpu_set_t *set);
int __wrap_sched_setaffinity(pid_t pid, size_t size, const cpu_set_t *set) {
    if(reject_restore){errno=EPERM;return -1;}
    return __real_sched_setaffinity(pid,size,set);
}
static void *native_worker(void *unused) {
    (void)unused;
    cpu_set_t got;
    assert(sched_getaffinity(0,sizeof got,&got)==0 && CPU_COUNT(&got)==1);
    defjam_android_native_thread_enter("render","fixture_native",(uintptr_t)native_worker);
    assert(sched_getaffinity(0,sizeof got,&got)==0);
    if(reject_restore)assert(CPU_COUNT(&got)==1);
    else assert(CPU_EQUAL(&expected,&got));
    assert(defjam_android_native_core_count()==CPU_COUNT(&got));
    return NULL;
}
int main(int argc,char **argv) {
    assert(argc==2);
    assert(defjam_android_native_core_count()==0); // getter must not capture configuration
    assert(sched_getaffinity(0,sizeof original,&original)==0);
    int request=atoi(argv[1]),n=0;
    CPU_ZERO(&expected);
    for(int cpu=CPU_SETSIZE-1;cpu>=0;--cpu)
        if(CPU_ISSET(cpu,&original) && (!request||n<request)){CPU_SET(cpu,&expected);++n;}
    setenv("RECOMP_NATIVE_CORES",argv[1],1);
    defjam_android_cpu_init(); // capture BEFORE the guest is narrowed
    cpu_set_t guest,got;CPU_ZERO(&guest);
    int highest=CPU_SETSIZE-1;while(!CPU_ISSET(highest,&original))--highest;
    CPU_SET(highest,&guest);assert(sched_setaffinity(0,sizeof guest,&guest)==0);
    defjam_android_guest_thread_note("guest-main","fixture_guest",0,0x123456,42);
    // A native callback invoked by guest code must never widen that guest.
    defjam_android_native_thread_enter("audio","callback_on_guest",0);
    assert(sched_getaffinity(0,sizeof got,&got)==0 && CPU_EQUAL(&guest,&got));
    pthread_t worker;assert(!pthread_create(&worker,NULL,native_worker,NULL));
    assert(!pthread_join(worker,NULL));
    assert(sched_getaffinity(0,sizeof got,&got)==0 && CPU_EQUAL(&guest,&got));
    // A scheduler rejection must publish the effective mask, not the request.
    reject_restore=1;
    assert(!pthread_create(&worker,NULL,native_worker,NULL));assert(!pthread_join(worker,NULL));
    assert(sched_getaffinity(0,sizeof got,&got)==0 && CPU_EQUAL(&guest,&got));
}
''')
    out = tmp_path / 'policy'
    subprocess.run(['cc', '-std=gnu11', '-O2', '-Wall', '-Wextra', '-Werror',
                    '-pthread', '-D__ANDROID__=1', '-I' + str(ROOT / 'tools/xboxrecomp/src'),
                    str(source), str(ROOT / 'src/android_thread_policy.c'),
                    '-Wl,--wrap=sched_setaffinity', '-o', str(out)],
                   check=True, capture_output=True, text=True)
    for count in ('0', '1', '2', '4', '8'):
        subprocess.run([str(out), count], check=True, capture_output=True, text=True,
                       env={**os.environ, 'RECOMP_NATIVE_CORES': count})
