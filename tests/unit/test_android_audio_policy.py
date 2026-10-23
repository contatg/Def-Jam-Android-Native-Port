"""The real SDL callback keeps PCM/silence behavior after native thread setup."""
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[2]


def test_audio_callback_policy_does_not_change_pcm(tmp_path):
    code = (ROOT / 'tools/xboxrecomp/src/apu/apu_xaudio2.c').read_text()
    start = code.index('static void sdl_callback(')
    body = code[start:code.index('#ifdef XBOXRECOMP_HAVE_SDL3', start)]
    c = tmp_path / 'audio.c'
    c.write_text(r'''
#define __ANDROID__ 1
#include <stdint.h>
#include <assert.h>
#include <string.h>
typedef unsigned char Uint8;
#define XA2_CHANNELS 2
#define RING_FRAMES 64
#define XA2_PREROLL 8
static int16_t g_ring[RING_FRAMES][2];
static unsigned g_ring_r,g_ring_w;
static int g_primed,g_dry,g_xa2_frames_written,g_xa2_starved,policy_calls;
void defjam_android_native_thread_enter(const char *role,const char *function,uintptr_t entry) {
    assert(!strcmp(role,"audio")&&!strcmp(function,"sdl_callback")&&entry);++policy_calls;
}
''' + body + r'''
int main(void) {
    int16_t out[16][2];memset(out,0x5a,sizeof out);
    sdl_callback(NULL,(Uint8 *)out,sizeof out);
    for(unsigned n=0;n<16;++n)assert(!out[n][0]&&!out[n][1]);
    assert(policy_calls==1);
    for(unsigned n=0;n<20;++n){g_ring[n][0]=n+1;g_ring[n][1]=-(int)(n+1);}
    g_ring_w=20;g_xa2_frames_written=20;
    sdl_callback(NULL,(Uint8 *)out,sizeof out);
    for(unsigned n=0;n<16;++n)assert(out[n][0]==(int)n+1&&out[n][1]==-(int)(n+1));
    sdl_callback(NULL,(Uint8 *)out,sizeof out);
    for(unsigned n=0;n<4;++n)assert(out[n][0]==(int)n+17&&out[n][1]==-(int)(n+17));
    for(unsigned n=4;n<16;++n)assert(!out[n][0]&&!out[n][1]);
    assert(policy_calls==1&&g_xa2_starved==1&&!g_primed);
}
''')
    binary = tmp_path / 'audio'
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
                    str(c), '-o', str(binary)], check=True, capture_output=True, text=True)
    subprocess.run([str(binary)], check=True, capture_output=True, text=True)
